#include <Arcane/Render/Device.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>

namespace Arcane
{
    const char* ToString(GraphicsBackend backend)
    {
        switch (backend)
        {
        case GraphicsBackend::D3D12:  return "D3D12";
        case GraphicsBackend::Vulkan: return "Vulkan";
        }
        return "Unknown";
    }

    std::unique_ptr<RenderDevice> RenderDevice::Create(const RenderDeviceDesc& desc)
    {
        switch (desc.backend)
        {
        case GraphicsBackend::D3D12:
            return CreateDeviceD3D12(desc);
        case GraphicsBackend::Vulkan:
            return CreateDeviceVulkan(desc);
        }
        ARC_ERROR("RenderDevice::Create: unknown backend");
        return nullptr;
    }
}
