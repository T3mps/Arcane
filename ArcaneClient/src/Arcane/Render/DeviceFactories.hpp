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
}
