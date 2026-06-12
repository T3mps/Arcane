#pragma once

// Render module: the engine's GPU device. Creation is headless by design --
// a swapchain is created separately against a Window (next tasks), so tools
// and tests can run compute/offscreen work without any window.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Arcane
{
    class Window;
    class Swapchain;

    enum class GraphicsBackend : uint8_t
    {
        D3D12,
        Vulkan,
    };

    ARCANE_API const char* ToString(GraphicsBackend backend);

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

        // The window must outlive the swapchain. For Vulkan, the window
        // must have been created with WindowDesc::vulkan = true.
        virtual std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                           bool vsync) = 0;
    };
}
