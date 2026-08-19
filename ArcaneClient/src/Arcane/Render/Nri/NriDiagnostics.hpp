#pragma once

// NriDiagnostics -- THE CRASH CHAIN, ARMED BY WHICHEVER DEVICE EXISTS
// (NRI Phase 3, Task 5).
//
// Through Phase 2 every link of the GPU crash-diagnostics chain hung off the
// NVRHI device's creation path (the deleted Render/DeviceD3D12.cpp's Init and
// its Vulkan twin):
// the device-removed hook, the Diagnostics GPU-section provider, the
// process-wide active-crash-backend slot, and -- through GpuFrameProgress --
// the single Diagnostics::GpuHeartbeat publisher. That was fine while an
// NVRHI device was the only device a host could have.
//
// Task 6 made it false: `--nri-graph` became ONE device, wrapped by NRI, with
// NO NVRHI device in the process at all. Every link above would then have been
// unarmed -- no `.arcdiag` on a TDR, no `.gpudump`, no gpu-stall verdict, no
// host device-lost latch -- and a render-path port that silently gives up the
// crash diagnostics arc is not a port. CRASH-DIAGNOSTICS PARITY IS A PHASE
// GATE, so this file is the NRI-side installer for the same chain. Phase 5a
// Task 8b finished the job by deleting the NVRHI device layer outright: this
// file is now the ONLY installer of that chain in the process.
//
// -------------------------------------------------------------------------
// WHAT IT DOES NOT DO, said plainly
// -------------------------------------------------------------------------
// It does NOT reimplement the crash BACKENDS -- and as of NRI Phase 5a,
// Task 9.5a there are none left to reimplement. The D3D12 marker buffer
// (WriteBufferImmediate over an OpenExistingHeapFromAddress placed resource)
// and the Vulkan one (VK_AMD_buffer_marker + VK_EXT_device_fault) were
// native, nvrhi-device-shaped objects; because the deleted device layer was
// the only thing that ever called MakeD3D12CrashBackend /
// MakeVulkanCrashBackend, both had been unreachable since Task 8b and both
// are now DELETED. GpuCrashVulkan.cpp is gone entirely; GpuCrashD3D12.cpp
// survives as EnableD3D12Dred/DredTier only -- the process-global DRED tier,
// which never went through a backend object. The consequence, named in
// IGpuCrashBackend.hpp: nothing in the tree reads DRED breadcrumbs or Vulkan
// device-fault info back any more, and restoring that readback belongs to the
// same NRI-shaped marker layer this header keeps deferring to. What Arm()
// installs is the
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
// THROUGH PHASE 3 (two devices: the engine's NVRHI device + the vehicle's NRI
// device) Arm() found the process-wide crash-backend slot already full -- the
// NVRHI device armed it during boot, before the vehicle existed -- and NO-OPPED
// completely. Nothing was reinstalled, nothing displaced, and the vehicle's
// breadcrumbs kept landing in the NVRHI backend's ring exactly as they did
// before this file existed.
//
// SINCE TASK 6 (one device, no NVRHI device anywhere) the slot is empty and
// Arm() fills every link. Same call, same call site; which topology it is in
// is inferred, never configured -- a flag would be a third thing to keep in
// sync with a fact the slot already states. That inference is what let Task 8b
// delete the other writer without touching a line of this file: the full-slot
// branch simply became unreachable in production, and it still refuses for any
// foreign backend (NriDiagnosticsTest drives it with a stub).
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
        // SAME per-backend observer the NVRHI device used to install, reached
        // by address through DeviceRemovedObservers.hpp), the graph-flavored
        // Diagnostics::SetGpuSectionProvider, and SetActiveGpuCrashBackend for
        // this device's backend. Also clears the process-wide device-lost
        // latch, for the same reason the NVRHI device's Init did: a latch that
        // outlived the dead device it described would quit the host the moment
        // a healthy replacement started presenting.
        //
        // Returns true iff THIS call armed. False means "already armed" --
        // by a previous Arm() (idempotence), or by a foreign backend holding
        // the slot (the NVRHI device, in the two-device transition runs Task
        // 8b ended) -- and in that case nothing at all was touched. Never
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
        // fence's completed value. It replaced GpuFrameSlot/GpuFrameProgress
        // 1:1 -- and outlived them, since NRI Phase 5a Task 9.5a deleted both
        // (see that accessor's comment for why it was never an
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
        // Render/GpuFaultInjector.hpp used to fire through nvrhi -- as a
        // one-off NRI compute dispatch on `queue`. Same shader, same two fault
        // mechanisms, same desk-battery meaning; a port of the recording, not
        // of the fault. Task 8b deleted the nvrhi arm, so THIS IS the injector
        // now, and GpuFaultInjector.hpp is reduced to the breadcrumb name both
        // arms always shared. Everything downstream (the device-removed observation, the
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
        // NOT a general compute facility, exactly as its nvrhi twin was not: a
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
