#pragma once

// Internal to the Render module: per-backend factory functions implemented
// in DeviceD3D12.cpp / DeviceVulkan.cpp, dispatched by RenderDevice::Create.
// Not part of the engine's public API surface.

#include <Arcane/Render/Device.hpp>

#include <memory>

namespace Arcane
{
    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc);
    std::unique_ptr<RenderDevice> CreateDeviceVulkan(const RenderDeviceDesc& desc);

    // -------------------------------------------------------------------
    // THE DEVICE-REMOVED OBSERVERS, BY NAME (NRI Phase 3, Task 5)
    // -------------------------------------------------------------------
    // F-3's ONE observation point per backend stays exactly where it always
    // was -- a file-local `ObserveDeviceRemoved` in DeviceD3D12.cpp /
    // DeviceVulkan.cpp, next to the once-only latch and the removal-report
    // wording it owns. What changed is who INSTALLS it: through Phase 2 the
    // only installer was the NVRHI device's own Init, and after Phase 3's
    // one-device flip a host may hold an NRI device with no NVRHI device
    // anywhere in the process (Task 6). `Render/Nri/NriDiagnostics.cpp` is
    // then the installer, and it needs the observer's ADDRESS.
    //
    // These two forwarders are the narrowest seam that gives it one: a
    // one-line call into the unchanged file-local function, declared in the
    // Render module's own internal factory header (not the public API --
    // Device.hpp) because nothing outside this module may install a hook.
    // The alternatives were worse: moving the observers out of their TUs
    // would separate them from the per-backend latch and the DRED/fault
    // state they describe (and the brief forbids it), and a new
    // "device-removed registry" would be a second slot to keep in sync with
    // NvrhiMessageCallback's, which is already last-writer-wins.
    //
    // Per-backend rather than one function, because the latch is per-backend:
    // each TU's `g_deviceRemovedReported` is armed/cleared beside the crash
    // backend of that TU's device, and collapsing the two would make a
    // D3D12 removal silence a later Vulkan one.
    void ObserveDeviceRemovedD3D12();
    void ObserveDeviceRemovedVulkan();

    // THE ONCE-ONLY LATCH THOSE OBSERVERS GUARD, RE-ARMED (Task 5, fix 1).
    //
    // `ObserveDeviceRemoved` reports the FIRST removal it sees and then
    // latches: one device loss cascades into many callbacks and one report is
    // the truth. So the latch has to be cleared whenever a NEW device arms --
    // the loss it described is over, and the next removal is a different event
    // that owes its own `.arcdiag`. Both device TUs already do exactly that at
    // their own arming site, one line above ResetGpuDeviceLost()
    // (DeviceD3D12.cpp / DeviceVulkan.cpp: "Reset when a new backend arms
    // (project switch recreates the device)") -- the two calls are a PAIR, and
    // ResetGpuDeviceLost's own header comment names this latch as its twin.
    //
    // `Render/Nri/NriDiagnostics.cpp` is the second arming site, and after
    // Task 6 the ONLY one. It therefore owes the same pair, and the latch is
    // file-local with no other way to reach it. Without this, a host that
    // survives a loss and rebuilds its graph context re-arms into a still-
    // latched observer, and the SECOND removal writes nothing at all: no
    // report, no `.gpudump`, and no NoteGpuDeviceLost for the host to quit on.
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
