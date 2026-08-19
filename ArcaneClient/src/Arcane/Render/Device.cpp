#include <Arcane/Render/Device.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>

namespace Arcane
{
    // NRI Phase 5a, Task 8a: RenderErrorCount and the four *ForTest seams
    // moved to Render/RenderErrorLatch.cpp, and ToString(GraphicsBackend) to
    // Render/GraphicsBackend.cpp. None of them is about NVRHI device creation,
    // and all of them outlive this file. What is left below is.

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
