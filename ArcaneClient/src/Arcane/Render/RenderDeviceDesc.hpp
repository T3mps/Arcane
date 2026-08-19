#pragma once

// Render module: WHAT KIND OF DEVICE to create -- backend plus the three
// validation switches. Creation itself is headless by design: a swapchain is
// created separately against a Window, so tools and tests can run
// compute/offscreen work without any window.
//
// NRI Phase 5a, Task 8b: this struct used to sit in Render/Device.hpp, the
// NVRHI device interface the phase deletes. It is the sixth non-NVRHI thing
// stranded there (Task 8a relocated the other five) and it is on the graph's
// critical path -- Nri/NriDevice's NativeDeviceOwner::Create takes one, and
// both creation halves (DeviceCreationD3D12.hpp / DeviceCreationVulkan.hpp)
// take one -- so it moved to a header with no nvrhi dependency rather than
// dying with its old home. Nothing about the struct changed; the comments
// below were repaired only where the deletion falsified them.

#include <Arcane/Render/GraphicsBackend.hpp>

namespace Arcane
{
    struct RenderDeviceDesc
    {
        GraphicsBackend backend = GraphicsBackend::D3D12;
#if defined(ARCANE_DEBUG)
        bool enableValidation = true;   // NRI validation layer + VK validation
#else
        bool enableValidation = false;
#endif
        // Opt-in: D3D12 CPU debug layer (EnableDebugLayer). Disabled by
        // default because D3D12SDKLayers.dll raises RaiseFailFastException
        // (code 0x87D) when third-party window hooks (e.g. Nahimic OSD) are
        // loaded. NRI's own validation layer covers command-level errors;
        // enable this only when debugging D3D12 API parameter errors on a
        // machine without injected window hooks.
        bool enableD3D12DebugLayer = false;

        // Opt-in: Vulkan SYNCHRONIZATION validation, on top of the ordinary
        // VK_LAYER_KHRONOS_validation core checks `enableValidation` turns on.
        // Vulkan-only -- D3D12's debug layer has no separate sync-validation
        // switch (its closest analogue, GPU-Based Validation, is a different
        // and far costlier thing), so `enableD3D12DebugLayer` above is the
        // whole D3D12 story.
        //
        // DEFAULT FALSE. Its only caller is the frame-graph vehicle
        // (Render/Nri/NriGraphContext.cpp), which forces it on in Debug -- the
        // class of defect sync validation catches (hazards in hand- or
        // graph-derived barrier placement) is exactly what core validation
        // does not. Sync validation is expensive and false-positive-prone on
        // a full engine frame, which is why it is opt-in per device rather
        // than folded into `enableValidation`: turning it on for Release/Dist
        // is a separate decision nobody has made yet.
        //
        // Requires `enableValidation` (it configures the validation layer; with
        // no layer loaded there is nothing to configure) and the
        // VK_EXT_validation_features instance extension. Missing either is a
        // WARN and a degrade, never a create failure -- see
        // DeviceCreationVulkan.cpp's CreateVulkanNativeDevice.
        bool enableSyncValidation = false;
    };
}
