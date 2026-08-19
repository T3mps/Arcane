#include <Arcane/Render/GpuInstrumentation.hpp>

#include <Arcane/Base/Diagnostics.hpp>

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <chrono>

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

        // The device-lost latch (see the header). Written by the device layer
        // after the gpu-crash report lands; read once per host frame.
        std::atomic<bool> g_deviceLost{ false };

        // One millisecond, expressed where SDL wants it. Not smaller: below the
        // OS scheduling quantum a "sleep" degrades into a spin, and burning a
        // core to shave sub-millisecond latency off a frame that is already
        // waiting on the GPU is a bad trade.
        constexpr Uint64 kSlotPollSleepNs = 1'000'000;

        // How long to keep polling before parking in the blocking wait. Chosen
        // comfortably above Config::gpuStallSeconds' default of 8 so the GPU rule
        // has fired long before the fallback, and short enough that a genuinely
        // dead device is not woken every millisecond for the rest of the session.
        // A host configuring a gpuStallSeconds ABOVE this would lose the polling
        // window's benefit -- that is the one coupling here, and it is why this
        // constant lives next to that comment rather than in a header.
        constexpr std::chrono::seconds kSlotPollWindow{ 15 };
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

    void NoteGpuDeviceLost() noexcept
    {
        g_deviceLost.store(true, std::memory_order_release);

        // Push the same fact down to Base, whose top-level exception filter
        // needs it to tell a D3D12 debug-layer fail-fast raised BY this loss
        // apart from an unrelated crash (Base/Diagnostics.hpp,
        // Diagnostics::NoteGpuDeviceLost). This is that flag's only writer, so
        // the two cannot disagree.
        //
        // NOT undone by ResetGpuDeviceLost below: that one re-arms THIS
        // module's observation for a rebuilt device, while the Base flag is a
        // crash-classification fact about a process that has seen a loss --
        // and a fail-fast raised while tearing the OLD device down still wants
        // the device-loss verdict.
        Diagnostics::NoteGpuDeviceLost();
    }

    bool GpuDeviceLostObserved() noexcept
    {
        return g_deviceLost.load(std::memory_order_acquire);
    }

    void ResetGpuDeviceLost() noexcept
    {
        g_deviceLost.store(false, std::memory_order_release);
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

    GpuPassScope::GpuPassScope(void* nativeCommandList, const char* name) noexcept
        : m_commandList(nativeCommandList)
    {
        if (!m_commandList || !name)
            return;

        // THE NVRHI CHANNEL WAS HERE (NRI Phase 5a, Task 8b). The constructor
        // used to open with m_commandList->beginMarker(name) before touching
        // the backend at all -- deliberately, because that marker is what a
        // PIX/RenderDoc capture and D3D12 DRED's markers-only tier read, and
        // DRED enablement is process-global and independent of whether a
        // backend object exists. There is no nvrhi command list left to call
        // it on; see the header's F-2c-bis paragraph for why that does not
        // strand the Dist tier, and what restores both channels.
        //
        // Latched, not re-read in the destructor: a teardown that cleared the
        // slot mid-scope must not leave a BeginScope without its EndScope, nor
        // hand the end marker to a different backend than the begin marker.
        m_backend = ActiveGpuCrashBackend();
        if (!m_backend)
            return;

        m_token  = m_backend->Breadcrumbs().BeginScope(name);
        m_scoped = true;

        // False here means "this layer is unavailable" (no marker buffer, a
        // failed QueryInterface, or -- today -- a graph backend whose native
        // marker layer is still a stub). It is not fatal and not worth a
        // per-pass log: the backend logs its one WARN and records the degrade
        // in activeLayers, which is where a reader looks.
        (void)m_backend->WriteMarkerNative(m_commandList, m_token, true);
    }

    GpuPassScope::~GpuPassScope()
    {
        // Exact mirror of the constructor's order so the scopes nest.
        if (m_scoped && m_backend)
        {
            (void)m_backend->WriteMarkerNative(m_commandList, m_token, false);
            m_backend->Breadcrumbs().EndScope(m_token);
        }
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

    // ---------------------------------------------------------------------
    // GpuFrameSlot
    // ---------------------------------------------------------------------

    namespace
    {
    // The polling pacing wait. File-local on purpose: it is only correct for a
    // query that is currently STAMPED, and GpuFrameSlot is the thing that knows
    // whether it is. Exposing it would hand callers a way to poll an unstamped
    // query, which nvrhi reports as incomplete forever -- see the header.
    void PollingWaitForStampedQuery(nvrhi::IDevice* device, nvrhi::IEventQuery* query)
    {
        // Fast path, before any sleep: every frame that is not GPU-bound takes
        // this and pays exactly one poll. Skipping it would tax the common case
        // a full sleep quantum for a wait that was never going to happen.
        if (device->pollEventQuery(query))
            return;

        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < kSlotPollWindow)
        {
            // The two beats are the entire point of polling instead of
            // blocking. Heartbeat: this thread is alive and deliberately
            // waiting, so the hang rule must not call it wedged.
            // GpuHeartbeatRefresh: the counter has not moved AND somebody is
            // still watching it -- the exact state the GPU rule fires on, and
            // the exact state a blocking wait made invisible.
            Diagnostics::Heartbeat();
            Diagnostics::GpuHeartbeatRefresh();

            SDL_DelayNS(kSlotPollSleepNs);

            if (device->pollEventQuery(query))
                return;
        }

        // Past the window. Park properly rather than waking forever; the beats
        // stop here too, which is what lets a permanently wedged GPU still
        // produce the hang rule's parked-stack report after the gpu-stall one.
        device->waitEventQuery(query);
    }
    }   // namespace

    bool GpuFrameSlot::Init(nvrhi::IDevice* device)
    {
        m_stamped = false;
        m_query   = device ? device->createEventQuery() : nvrhi::EventQueryHandle{};
        return m_query != nullptr;
    }

    void GpuFrameSlot::Stamp(nvrhi::IDevice* device, nvrhi::CommandQueue queue)
    {
        if (!device || !m_query)
            return;
        device->setEventQuery(m_query, queue);
        // Set only AFTER the stamp actually went out. A slot that claims a
        // fence it does not have is exactly the state that sends the wait into
        // a poll loop nvrhi will never satisfy.
        m_stamped = true;
    }

    void GpuFrameSlot::WaitAndReset(nvrhi::IDevice* device)
    {
        if (!device || !m_query)
            return;

        if (m_stamped)
        {
            PollingWaitForStampedQuery(device, m_query);
        }
        else
        {
            // Nothing was ever submitted against this slot -- a frame that
            // bailed between reset and Present (an out-of-date surface, a lost
            // device). nvrhi's wait returns immediately for this state; its
            // poll reports incomplete forever. Take the wait, exactly as the
            // pre-polling code did, and do not touch the diagnostics beats:
            // there is no GPU progress being waited on to have an opinion
            // about.
            device->waitEventQuery(m_query);
        }

        device->resetEventQuery(m_query);
        m_stamped = false;
    }
}
