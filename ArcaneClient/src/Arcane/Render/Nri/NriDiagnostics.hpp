#pragma once

// NriDiagnostics -- THE CRASH CHAIN, ARMED OVER THE ONE DEVICE.
//
// Every link of the GPU crash-diagnostics chain -- the device-removed hook,
// the Diagnostics GPU-section provider, the process-wide active-crash-backend
// slot, and the Diagnostics::GpuHeartbeat publisher -- is installed from
// HERE. This file is the ONLY installer of that chain in the process.
//
// -------------------------------------------------------------------------
// WHAT IT DOES NOT DO, said plainly
// -------------------------------------------------------------------------
// It does NOT implement a GPU-API crash BACKEND, and the tree has none: there
// is no D3D12 marker buffer (WriteBufferImmediate over an
// OpenExistingHeapFromAddress placed resource) and no Vulkan one
// (VK_AMD_buffer_marker + VK_EXT_device_fault). GpuCrashD3D12.cpp survives as
// EnableD3D12Dred/DredTier only -- the process-global DRED tier, which never
// went through a backend object. The consequence, named in
// IGpuCrashBackend.hpp: NOTHING IN THE TREE READS DRED BREADCRUMBS OR VULKAN
// DEVICE-FAULT INFO BACK, and restoring that readback belongs to the same
// NRI-shaped marker layer this header keeps deferring to.
//
// What Arm() installs is the GRAPH-FLAVORED backend: the CPU-side breadcrumb
// ring the graph's NodeScope already writes into (RenderGraphExec.cpp), plus
// a device identity so the cross-device native-marker gate there can open --
// and no GPU-written marker layer of its own. That is not a shortcut, it is
// the honest inventory: RenderGraphExec.cpp already records that "the
// CPU-side breadcrumb ring ... is the half today's hang and crash reports are
// actually built from", and Diag::ReplayMarkerBuffer is explicitly written to
// accept a backend with no marker region (it pushes `breadcrumbs:off` and
// adds no section). A native NRI marker buffer is its own arc; when it lands
// it fills in exactly one method here.
//
// -------------------------------------------------------------------------
// ONE RULE, WHATEVER HOLDS THE SLOT
// -------------------------------------------------------------------------
// Arm() fills every link when the process-wide crash-backend slot is EMPTY,
// and NO-OPS COMPLETELY when something already holds it -- nothing
// reinstalled, nothing displaced. Which case it is in is INFERRED from the
// slot, never configured: a flag would be a third thing to keep in sync with
// a fact the slot already states. The already-held branch is unreachable in
// production (nothing else installs one) and still refuses any foreign
// backend -- NriDiagnosticsTest drives it with a stub.
//
// Idempotent either way: a second Arm() is a no-op and says so through its
// return value.

// Include order: NRI headers first, ALWAYS (see NriCommon.hpp) --
// Extensions/NRIDeviceCreation.h (via NriDevice.hpp) declares
// nri::Message::ERROR and <windows.h> (via spdlog) #defines ERROR.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

#include <cstdint>

namespace Arcane
{
    class IGpuCrashBackend;   // <Arcane/Render/IGpuCrashBackend.hpp>

    namespace NriDiagnostics
    {
        // Arms the crash chain over `device`: the per-backend
        // device-removed observer (reached by address through
        // DeviceRemovedObservers.hpp), the graph-flavored
        // Diagnostics::SetGpuSectionProvider, and SetActiveGpuCrashBackend for
        // this device's backend. Also clears the process-wide device-lost
        // latch -- a latch that outlived the dead device it described would
        // quit the host the moment a healthy replacement started presenting.
        //
        // Returns true iff THIS call armed. False means "already armed" -- by
        // a previous Arm() (idempotence), or by a foreign backend holding the
        // slot -- and in that case nothing at all was touched. Never
        // fails loudly: an un-armable device degrades one diagnostic channel,
        // it does not fail a host.
        //
        // `device` must outlive the arming, i.e. Disarm() must run before it
        // is destroyed. NriGraphContext owns both calls.
        ARCANE_API bool Arm(NriDevice& device);

        // Empties exactly what Arm() installed, and only if Arm() installed
        // it -- the active-backend slot clears CONDITIONALLY
        // (ClearActiveGpuCrashBackendIfCurrent), so a later owner that armed
        // after us keeps its registration. Fences reports before the backend
        // object is destroyed, the same ordering ~DeviceD3D12 owed
        // (Diagnostics::FenceReports). Idempotent; a no-op when not armed.
        ARCANE_API void Disarm() noexcept;

        // Whether Arm() currently holds the chain. False when something else
        // holds it even though the chain IS armed -- which is the honest
        // answer to "did THIS installer arm it". That was the two-device
        // topology's normal state; since Task 8b it is a test-only shape.
        [[nodiscard]] ARCANE_API bool IsArmed() noexcept;

        // The backend Arm() installed, or null when not armed. Exposed so a
        // headless case can state the slot-identity property
        // (ActiveGpuCrashBackend() == ArmedBackend()) rather than merely
        // "something is installed".
        [[nodiscard]] ARCANE_API IGpuCrashBackend* ArmedBackend() noexcept;

        // "The GPU is still retiring work", published from the graph path.
        //
        // Called after EVERY PRESENTED FRAME from NriGraphContext::RenderFrame
        // with NriSwapChain::CompletedFrameValue() -- the pacing timeline
        // fence's completed value. A monotone count of sync points the device has
        // actually passed is exactly what Diagnostics::GpuHeartbeat's rule
        // wants; publishing it from the presented-frames path (not the
        // skipped ones) is what keeps "the counter froze while the render
        // path was demonstrably alive" the observable it is supposed to be.
        //
        // A thin forwarder on purpose: the value SOURCE is the interesting
        // decision and it lives at the two ends (the swapchain's fence, the
        // watchdog's rule). Having the seam named here anyway is what lets the
        // graph path's publisher be found, tested and moved as one thing.
        ARCANE_API void PublishHeartbeat(std::uint64_t completedFenceValue) noexcept;

#if !defined(ARCANE_DIST)
        // THE FAULT INJECTOR TWIN (`--crash-gpu N` on the graph path).
        //
        // Dispatches data/shaders/gpu_fault.hlsl -- the TDR-loop shader --
        // as a one-off NRI compute dispatch on `queue`. THIS IS the injector,
        // and GpuFaultInjector.hpp is reduced to the breadcrumb name it
        // shares with this call. Everything downstream (the device-removed
        // observation, the report, the `.gpudump`, the Problems notify) is
        // what Arm() above put in place.
        //
        // Builds its own pipeline, buffers, descriptor pool, command allocator
        // and command buffer, submits ONCE, and destroys nothing that matters:
        // the device is expected to be lost moments later, so the objects are
        // freed behind a best-effort idle and any failure past the submit is
        // noise. Returns false (already logged) when a step could not be
        // built -- an unavailable injector is a missing diagnostic, never a
        // host failure.
        //
        // NOT a general compute facility: a closed, single-purpose object
        // with no knobs. When a real NRI compute
        // pass lands it should grow its own seam and this must not be its
        // template.
        //
        // THE ONE HAND-WRITTEN BARRIER IN THE NRI TREE lives inside this call.
        // See the .cpp for the exemption's terms.
        ARCANE_API bool FireFault(NriDevice& device, nri::Queue& queue);
#endif
    }
}
