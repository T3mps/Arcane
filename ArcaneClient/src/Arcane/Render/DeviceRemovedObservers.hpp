#pragma once

// Internal to the Render module: the device-removed observation seam, by
// name. Not part of the engine's public API surface -- every declaration
// below is defined in, and consumed from, ArcaneClient.dll.
//
// NRI Phase 5a, Task 8b: this file is what is LEFT of DeviceFactories.hpp.
// That header carried two things: the per-backend `CreateDeviceD3D12` /
// `CreateDeviceVulkan` factories `RenderDevice::Create` dispatched to, and
// the observation seam below. The factories died with the NVRHI device layer;
// the seam did not, so it moved to a file whose name is still true. The
// observers themselves moved WITH it, in the same step, into the creation
// halves (Render/DeviceCreation{D3D12,Vulkan}.cpp) -- see those files for why
// the file-local observer and the forwarder here stay two distinct functions.
//
// -------------------------------------------------------------------
// THE DEVICE-REMOVED OBSERVERS, BY NAME (NRI Phase 3, Task 5)
// -------------------------------------------------------------------
// F-3's ONE observation point per backend is a file-local
// `ObserveDeviceRemoved`, next to the once-only latch and the removal-report
// wording it owns. What changed over the phases is who INSTALLS it: through
// Phase 2 the only installer was the NVRHI device's own Init; Phase 3's
// one-device flip let a host hold an NRI device with no NVRHI device anywhere
// in the process (Task 6); and Task 8b deleted the NVRHI device layer
// outright, so `Render/Nri/NriDiagnostics.cpp` is now the ONLY installer.
// It needs the observer's ADDRESS.
//
// These forwarders are the narrowest seam that gives it one: a one-line call
// into the unchanged file-local function, declared in a Render-internal
// header (never the public API) because nothing outside this module may
// install a hook. The alternatives were worse: collapsing the forwarder into
// the observer would change the address the hook slot holds -- which
// NriDiagnostics::Disarm compares against before clearing -- and a new
// "device-removed registry" would be a second slot to keep in sync with
// RenderErrorLatch's, which is already last-writer-wins.
//
// Per-backend rather than one function, because the latch is per-backend:
// each creation half's `g_deviceRemovedReported` is armed/cleared beside the
// crash backend of that backend's device, and collapsing the two would make a
// D3D12 removal silence a later Vulkan one.

namespace Arcane
{
    void ObserveDeviceRemovedD3D12();
    void ObserveDeviceRemovedVulkan();

    // THE ONCE-ONLY LATCH THOSE OBSERVERS GUARD, RE-ARMED (Task 5, fix 1).
    //
    // `ObserveDeviceRemoved` reports the FIRST removal it sees and then
    // latches: one device loss cascades into many callbacks and one report is
    // the truth. So the latch has to be cleared whenever a NEW device arms --
    // the loss it described is over, and the next removal is a different event
    // that owes its own `.arcdiag`. The NVRHI device TUs used to do exactly
    // that at their own arming site, one line above ResetGpuDeviceLost(); the
    // two calls are a PAIR, and ResetGpuDeviceLost's own header comment names
    // this latch as its twin.
    //
    // `Render/Nri/NriDiagnostics.cpp` inherited that obligation whole when
    // Task 8b deleted the other arming site, and the latch is file-local with
    // no other way to reach it. Without this, a host that survives a loss and
    // rebuilds its graph context re-arms into a still-latched observer, and
    // the SECOND removal writes nothing at all: no report, no `.gpudump`, and
    // no NoteGpuDeviceLost for the host to quit on.
    //
    // Per-backend for the same reason the observers above are: the latch IS
    // per-backend, so one shared reset would let a D3D12 arm silently re-arm
    // Vulkan's reporting and vice versa.
    void ResetDeviceRemovedLatchD3D12();
    void ResetDeviceRemovedLatchVulkan();

    // -------------------------------------------------------------------
    // "IS THIS D3D12 DEVICE GONE?" (NRI Phase 3, D3b 0x87D closeout)
    // -------------------------------------------------------------------
    // NRI's D3D12 `QueueWaitIdle` CANNOT report device loss, and that is
    // structural rather than a bug we can wait out: QueueD3D12::WaitIdle
    // (ThirdParty/NRI/Source/D3D12/QueueD3D12.hpp:84) returns the result of
    // CREATING its scratch fence, then calls FenceD3D12::Wait -- which is
    // `void` and can only shout "WaitForSingleObjectEx() failed!" down the
    // message callback (FenceD3D12.hpp:53). So a TDR'd D3D12 device answers
    // `Result::SUCCESS`. Its Vulkan twin (QueueVK::WaitIdle) forwards
    // vkQueueWaitIdle's VK_ERROR_DEVICE_LOST and therefore reaches
    // NriCheckImpl's typed DEVICE_LOST branch -- which is the entire reason
    // the vulkan `--crash-gpu` arm produced a device-removed verdict and the
    // dx12 one did not.
    //
    // This asks the device itself, which always knows. Narrow export for the
    // same reason as the observers above: `Render/Nri/` must not grow a
    // <d3d12.h> include or a per-backend #if -- NriDiagnostics.cpp says so in
    // its own words ("reaching for it would mean this file growing a D3D12 and
    // a [Vulkan] half"). No state and no second observation point: the CALLER
    // routes the answer through NoteDeviceLost, so the once-only latch and the
    // "gpu-crash: device removed" wording stay exactly where they live.
    //
    // `nativeDevice` is what nri::CoreInterface::GetDeviceNativeObject returns
    // for a D3D12 device (an ID3D12Device*). CALLERS MUST GATE ON THE BACKEND:
    // handing this a VkDevice would reinterpret it as a COM vtable. Null is
    // answered `false` -- "no device, no removal" -- never a guess.
    [[nodiscard]] bool D3D12NativeDeviceRemoved(void* nativeDevice) noexcept;
}
