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
        bool enableValidation = true;   // D3D12 debug layer / VK validation
#else                                   // layer + the NVRHI validation layer
        bool enableValidation = false;
#endif
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
    };
}
