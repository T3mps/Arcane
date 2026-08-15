#pragma once

// NriDiagnostics -- THE CRASH CHAIN, ARMED BY WHICHEVER DEVICE EXISTS
// (NRI Phase 3, Task 5).
//
// Through Phase 2 every link of the GPU crash-diagnostics chain hung off the
// NVRHI device's creation path (Render/DeviceD3D12.cpp's Init, Vulkan twin):
// the device-removed hook, the Diagnostics GPU-section provider, the
// process-wide active-crash-backend slot, and -- through GpuFrameProgress --
// the single Diagnostics::GpuHeartbeat publisher. That was fine while an
// NVRHI device was the only device a host could have.
//
// Task 6 makes it false: `--nri-graph` becomes ONE device, wrapped by NRI,
// with NO NVRHI device in the process at all. Every link above would then be
// unarmed -- no `.arcdiag` on a TDR, no `.gpudump`, no gpu-stall verdict, no
// host device-lost latch -- and a render-path port that silently gives up the
// crash diagnostics arc is not a port. CRASH-DIAGNOSTICS PARITY IS A PHASE
// GATE, so this file is the NRI-side installer for the same chain.
//
// -------------------------------------------------------------------------
// WHAT IT DOES NOT DO, said plainly
// -------------------------------------------------------------------------
// It does NOT reimplement the crash BACKENDS. The D3D12 marker buffer
// (WriteBufferImmediate over an OpenExistingHeapFromAddress placed resource)
// and the Vulkan one (VK_AMD_buffer_marker + VK_EXT_device_fault) are native,
// nvrhi-device-shaped objects that stay exactly where they are
// (GpuCrashD3D12.cpp / GpuCrashVulkan.cpp). What Arm() installs is the
// GRAPH-FLAVORED backend: the CPU-side breadcrumb ring the graph's NodeScope
// already writes into (RenderGraphExec.cpp), plus a device identity so the
// cross-device native-marker gate there can OPEN once the flip makes both
// halves one device -- and no GPU-written marker layer of its own. That is
// not a shortcut, it is the honest inventory: RenderGraphExec.cpp already
// records that "the CPU-side breadcrumb ring ... is the half today's hang and
// crash reports are actually built from", and Diag::ReplayMarkerBuffer is
// explicitly written to accept a backend with no marker region (it pushes
// `breadcrumbs:off` and adds no section). A native NRI marker buffer is its
// own arc; when it lands it fills in exactly one method here.
//
// -------------------------------------------------------------------------
// BOTH TOPOLOGIES, one rule
// -------------------------------------------------------------------------
// TODAY (two devices: the engine's NVRHI device + the vehicle's NRI device)
// Arm() finds the process-wide crash-backend slot already full -- the NVRHI
// device armed it during boot, before the vehicle existed -- and NO-OPS
// completely. Nothing is reinstalled, nothing is displaced, and the vehicle's
// breadcrumbs keep landing in the NVRHI backend's ring exactly as they did
// before this file existed.
//
// AFTER TASK 6 (one device, no NVRHI device anywhere) the slot is empty and
// Arm() fills every link. Same call, same call site; which topology it is in
// is inferred, never configured -- a flag would be a third thing to keep in
// sync with a fact the slot already states.
//
// Idempotent in both: a second Arm() is a no-op and says so through its
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
        // Arms the crash chain over `device`: the device-removed hook (the
        // SAME per-backend observer the NVRHI device installs, reached by
        // address through DeviceFactories.hpp), the graph-flavored
        // Diagnostics::SetGpuSectionProvider, and SetActiveGpuCrashBackend
        // for this device's backend. Also clears the process-wide device-lost
        // latch, for the same reason the NVRHI device's Init does: a latch
        // that outlived the dead device it described would quit the host the
        // moment a healthy replacement started presenting.
        //
        // Returns true iff THIS call armed. False means "already armed" --
        // either by a previous Arm() (idempotence) or by the NVRHI device
        // (the two-device transition runs) -- and in that case nothing at all
        // was touched. Never fails loudly: an un-armable device degrades one
        // diagnostic channel, it does not fail a host.
        //
        // `device` must outlive the arming, i.e. Disarm() must run before it
        // is destroyed. NriGraphContext owns both calls.
        ARCANE_API bool Arm(NriDevice& device);

        // Empties exactly what Arm() installed, and only if Arm() installed
        // it -- the active-backend slot clears CONDITIONALLY
        // (ClearActiveGpuCrashBackendIfCurrent), so a later owner that armed
        // after us keeps its registration. Fences reports before the backend
        // object is destroyed, the same ordering ~DeviceD3D12 owes
        // (Diagnostics::FenceReports). Idempotent; a no-op when not armed.
        ARCANE_API void Disarm() noexcept;

        // Whether Arm() currently holds the chain. False in the two-device
        // topology even while the chain IS armed -- by the NVRHI device, which
        // is the honest answer to "did THIS installer arm it".
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
        // fence's completed value, which is GpuFrameSlot/GpuFrameProgress's
        // 1:1 replacement (see that accessor's comment for why it is not an
        // approximation). A monotone count of sync points the device has
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
        // Dispatches data/shaders/gpu_fault.hlsl -- the SAME TDR-loop shader
        // Render/GpuFaultInjector.hpp fires through nvrhi -- as a one-off NRI
        // compute dispatch on `queue`. Same shader, same two fault mechanisms,
        // same desk-battery meaning; a port of the recording, not of the
        // fault. Everything downstream (the device-removed observation, the
        // report, the `.gpudump`, the Problems notify) is what Arm() above put
        // in place.
        //
        // Builds its own pipeline, buffers, descriptor pool, command allocator
        // and command buffer, submits ONCE, and destroys nothing that matters:
        // the device is expected to be lost moments later, so the objects are
        // freed behind a best-effort idle and any failure past the submit is
        // noise. Returns false (already logged) when a step could not be
        // built -- an unavailable injector is a missing diagnostic, never a
        // host failure.
        //
        // NOT a general compute facility, exactly like its nvrhi twin: a
        // closed, single-purpose object with no knobs. When a real NRI compute
        // pass lands it should grow its own seam and this must not be its
        // template.
        //
        // THE ONE HAND-WRITTEN BARRIER IN THE NRI TREE lives inside this call.
        // See the .cpp for the exemption's terms.
        ARCANE_API bool FireFault(NriDevice& device, nri::Queue& queue);
#endif
    }
}
