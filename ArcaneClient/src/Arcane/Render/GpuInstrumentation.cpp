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

        // Set from ProjectBoot.hpp's config load every boot. There is no
        // draw-granular marker scope to read it, so this is currently
        // write-only -- see GpuInstrumentation.hpp's banner. Relaxed
        // ordering, kept as-is: a
        // toggle observed one frame late was never meaningful and this must
        // not fence the render path if a future reader arrives.
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

        // ONE CHANNEL: the backend's marker buffer. There is no annotation
        // channel here -- see the header's F-2c-bis paragraph for why that
        // does not strand the Dist DRED tier, and what restores it.
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

}
