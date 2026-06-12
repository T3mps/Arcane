// Frame pacing: with 2 frames in flight, 20 consecutive frames must run
// without any validation noise (semaphore reuse, premature resource reuse,
// and upload-buffer races all surface as [nvrhi]/VK validation errors,
// which RenderErrorCount() latches). This is the regression gate for
// removing waitForIdle from the Present path.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Swapchain.hpp>

namespace
{
    void RunPacedFrames(Arcane::GraphicsBackend backend)
    {
        Arcane::Window window;
        Arcane::WindowDesc windowDesc;
        windowDesc.title  = std::string("PacingTest ") + Arcane::ToString(backend);
        windowDesc.width  = 320;
        windowDesc.height = 180;
        windowDesc.hidden = true;
        windowDesc.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
        REQUIRE(window.Create(windowDesc));

        Arcane::RenderDeviceDesc deviceDesc;
        deviceDesc.backend = backend;
        auto device = Arcane::RenderDevice::Create(deviceDesc);
        REQUIRE(device != nullptr);
        auto swapchain = device->CreateSwapchain(window, /*vsync=*/false);
        REQUIRE(swapchain != nullptr);

        nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();
        for (int frame = 0; frame < 20; ++frame)
        {
            nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
            REQUIRE(backbuffer != nullptr);
            commandList->open();
            commandList->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                           nvrhi::Color(0.05f * (float)frame, 0.1f, 0.2f, 1.0f));
            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            swapchain->Present();
        }
        device->Nvrhi()->waitForIdle();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: 20 frames with 2 in flight, validation silent", "[gpu][d3d12]")
{
    RunPacedFrames(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: 20 frames with 2 in flight, validation silent", "[gpu][vulkan]")
{
    RunPacedFrames(Arcane::GraphicsBackend::Vulkan);
}
