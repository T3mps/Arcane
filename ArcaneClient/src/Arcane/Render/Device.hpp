#pragma once

// Render module: the engine's GPU device. Creation is headless by design --
// a swapchain is created separately against a Window (next tasks), so tools
// and tests can run compute/offscreen work without any window.

#include <Arcane/Base/Api.hpp>
// NRI Phase 5a, Task 8a: two groups of declarations that used to live HERE
// now live in headers with no nvrhi dependency, because the graph path needs
// them and this file is the NVRHI device interface the phase deletes. Both
// are included so every existing consumer of Device.hpp keeps compiling
// unchanged:
//   * GraphicsBackend + ToString  -> Render/GraphicsBackend.hpp
//   * RenderErrorCount and the four *ForTest seams, plus the latch they read
//                                 -> Render/RenderErrorLatch.hpp
#include <Arcane/Render/GraphicsBackend.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Arcane
{
    class Window;
    class Swapchain;

    struct RenderDeviceDesc
    {
        GraphicsBackend backend = GraphicsBackend::D3D12;
#if defined(ARCANE_DEBUG)
        bool enableValidation = true;   // NVRHI validation layer + VK validation
#else
        bool enableValidation = false;
#endif
        // Opt-in: D3D12 CPU debug layer (EnableDebugLayer). Disabled by
        // default because D3D12SDKLayers.dll raises RaiseFailFastException
        // (code 0x87D) when third-party window hooks (e.g. Nahimic OSD) are
        // loaded. The NVRHI validation layer covers command-level errors;
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
        // DEFAULT FALSE, AND THE ENGINE'S OWN BOOT NEVER SETS IT. Its only
        // caller today is the `--nri-graph` frame-graph vehicle
        // (Render/Nri/NriGraphContext.cpp), which forces it on in Debug --
        // the class of defect sync validation catches (hazards in hand- or
        // graph-derived barrier placement) is exactly what core validation
        // does not. Sync validation is expensive and false-positive-prone on
        // a full engine frame, which is why it is opt-in per device rather
        // than folded into `enableValidation`: turning it on for the normal
        // boot is a separate decision nobody has made yet.
        //
        // Requires `enableValidation` (it configures the validation layer; with
        // no layer loaded there is nothing to configure) and the
        // VK_EXT_validation_features instance extension. Missing either is a
        // WARN and a degrade, never a create failure -- see
        // DeviceVulkan.cpp's CreateVulkanNativeDevice.
        bool enableSyncValidation = false;
    };

    class ARCANE_API RenderDevice
    {
    public:
        // Returns null on failure (no adapter, missing runtime, ...);
        // the failure reason is logged via ARC_ERROR.
        static std::unique_ptr<RenderDevice> Create(const RenderDeviceDesc& desc);

        virtual ~RenderDevice() = default;

        virtual GraphicsBackend Backend() const = 0;
        virtual nvrhi::IDevice* Nvrhi() const = 0;
        virtual std::string AdapterName() const = 0;

        // NOTE: there is deliberately NO GpuCrashBackend() accessor here. The
        // instrumentation that consumes the backend (Render/GpuInstrumentation.hpp)
        // must be reachable from OffscreenCanvas and PickBuffer too, and those
        // hold only an nvrhi::IDevice* -- so the device layer publishes the
        // backend to a process-wide slot instead, installed and cleared beside
        // the Diagnostics GPU-section provider it shares a lifetime with. ONE
        // access path on purpose: an accessor here would be a second one, and
        // when both existed the accessor had zero callers repo-wide.

        // The window must outlive the swapchain. For Vulkan, the window
        // must have been created with WindowDesc::vulkan = true.
        // The swapchain must be destroyed before the RenderDevice that
        // created it (its teardown uses the device's native handles).
        virtual std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                           bool vsync) = 0;
    };
}
