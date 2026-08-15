#include <Arcane/Render/Device.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>

namespace Arcane
{
    uint64_t RenderErrorCount()
    {
        return NvrhiMessageCallback::Instance().ErrorCount();
    }

    void ResetRenderErrorCount()
    {
        NvrhiMessageCallback::Instance().ResetForTest();
    }

    void NoteRenderErrorForTest(const char* tag, const char* text) noexcept
    {
        NvrhiMessageCallback::Instance().NoteError(tag, text);
    }

    void SetRenderDeviceRemovedHookForTest(void (*hook)()) noexcept
    {
        NvrhiMessageCallback::Instance().SetDeviceRemovedHook(hook);
    }

    void (*RenderDeviceRemovedHookForTest() noexcept)()
    {
        return NvrhiMessageCallback::Instance().CurrentDeviceRemovedHook();
    }

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
