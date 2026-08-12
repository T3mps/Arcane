#include <Arcane/Render/GpuInstrumentation.hpp>

#include <Arcane/Base/Diagnostics.hpp>

#include <atomic>

namespace Arcane
{
    namespace
    {
        // One slot, in Arcane.dll, exactly like Diagnostics' provider slot.
        // Atomic rather than mutex-guarded: this is read on the recording path
        // once per pass, and the only writers are the device layer's install
        // and teardown -- so the cost has to be a load, and a torn read is
        // impossible for a pointer-width atomic.
        std::atomic<IGpuCrashBackend*> g_activeBackend{ nullptr };

        // Read once per DRAW when enabled; relaxed because a toggle observed one
        // frame late is meaningless and this must not fence the render path.
        std::atomic<bool> g_drawMarkers{ false };
    }

    void SetActiveGpuCrashBackend(IGpuCrashBackend* backend) noexcept
    {
        g_activeBackend.store(backend, std::memory_order_release);
    }

    IGpuCrashBackend* ActiveGpuCrashBackend() noexcept
    {
        return g_activeBackend.load(std::memory_order_acquire);
    }

    bool ClearActiveGpuCrashBackendIfCurrent(IGpuCrashBackend* backend) noexcept
    {
        IGpuCrashBackend* expected = backend;
        return g_activeBackend.compare_exchange_strong(expected, nullptr,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire);
    }

    void SetGpuDrawMarkersEnabled(bool enabled) noexcept
    {
        g_drawMarkers.store(enabled, std::memory_order_relaxed);
    }

    bool GpuDrawMarkersEnabled() noexcept
    {
        return g_drawMarkers.load(std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------------
    // GpuPassScope
    // ---------------------------------------------------------------------

    GpuPassScope::GpuPassScope(nvrhi::ICommandList* commandList, const char* name) noexcept
        : m_commandList(commandList)
    {
        if (!m_commandList || !name)
            return;

        // The nvrhi marker goes out even with no crash backend installed: it is
        // what a PIX/RenderDoc capture and D3D12 DRED's markers-only tier read,
        // and DRED enablement (EnableD3D12Dred) is process-global and
        // independent of whether a backend object exists.
        m_commandList->beginMarker(name);
        m_marked = true;

        // Latched, not re-read in the destructor: a teardown that cleared the
        // slot mid-scope must not leave a BeginScope without its EndScope, nor
        // hand the end marker to a different backend than the begin marker.
        m_backend = ActiveGpuCrashBackend();
        if (!m_backend)
            return;

        m_token  = m_backend->Breadcrumbs().BeginScope(name);
        m_scoped = true;

        // False here means "this layer is unavailable" (no marker buffer, a
        // failed QueryInterface). It is not fatal and not worth a per-pass log
        // -- the backend logs its one WARN and records the degrade in
        // activeLayers, which is where a reader looks.
        (void)m_backend->WriteMarker(m_commandList, m_token, true);
    }

    GpuPassScope::~GpuPassScope()
    {
        // Exact mirror of the constructor's order so the scopes nest: end
        // marker inside, nvrhi endMarker outside.
        if (m_scoped && m_backend)
        {
            (void)m_backend->WriteMarker(m_commandList, m_token, false);
            m_backend->Breadcrumbs().EndScope(m_token);
        }
        if (m_marked)
            m_commandList->endMarker();
    }

    // ---------------------------------------------------------------------
    // GpuDrawScope
    // ---------------------------------------------------------------------

#if !defined(ARCANE_DIST)
    GpuDrawScope::GpuDrawScope(nvrhi::ICommandList* commandList, const char* name) noexcept
    {
        if (!commandList || !name || !GpuDrawMarkersEnabled())
            return;
        m_commandList = commandList;
        m_commandList->beginMarker(name);
    }

    GpuDrawScope::~GpuDrawScope()
    {
        if (m_commandList)
            m_commandList->endMarker();
    }
#endif

    // ---------------------------------------------------------------------
    // GpuFrameProgress
    // ---------------------------------------------------------------------

    GpuFrameProgress::GpuFrameProgress(nvrhi::IDevice* device)
        : m_device(device)
    {
        if (!m_device)
            return;
        for (std::size_t i = 0; i < kSlots; ++i)
        {
            m_queries[i] = m_device->createEventQuery();
            if (!m_queries[i])
            {
                // All-or-nothing: a partial chain would report a progress count
                // derived from fewer slots than the retire logic assumes. Better
                // to publish nothing than to publish a number that lies.
                for (std::size_t j = 0; j <= i; ++j) m_queries[j] = nullptr;
                m_device = nullptr;
                return;
            }
        }
    }

    void GpuFrameProgress::EndFrame() noexcept
    {
        if (!m_device)
            return;

        // Retire oldest-first. Stopping at the first un-retired slot is correct
        // AND required: queries are stamped in submission order on one queue, so
        // a later one cannot have signalled before an earlier one, and scanning
        // past it would let a stale slot look retired twice.
        while (m_live > 0 && m_device->pollEventQuery(m_queries[m_oldest]))
        {
            m_device->resetEventQuery(m_queries[m_oldest]);
            m_oldest = (m_oldest + 1) % kSlots;
            --m_live;
            ++m_completed;
        }

        if (m_live < kSlots)
        {
            m_device->setEventQuery(m_queries[m_next], nvrhi::CommandQueue::Graphics);
            m_next = (m_next + 1) % kSlots;
            ++m_live;
        }

        Diagnostics::GpuHeartbeat(m_completed);
    }
}
