// Windowed GPU path: hidden window + swapchain, several cleared and
// presented frames, then a resize and more frames. Hidden windows present
// fine on both DXGI and Vulkan; vsync off so the test never waits on the
// display.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Swapchain.hpp>

namespace
{
    void RunWindowedClearFrames(Arcane::GraphicsBackend backend)
    {
        Arcane::Window window;
        Arcane::WindowDesc windowDesc;
        windowDesc.title  = std::string("SwapchainTest ") + Arcane::ToString(backend);
        windowDesc.width  = 640;
        windowDesc.height = 360;
        windowDesc.hidden = true;
        windowDesc.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
        REQUIRE(window.Create(windowDesc));

        Arcane::RenderDeviceDesc deviceDesc;
        deviceDesc.backend = backend;
        auto device = Arcane::RenderDevice::Create(deviceDesc);
        REQUIRE(device != nullptr);

        auto swapchain = device->CreateSwapchain(window, /*vsync=*/false);
        REQUIRE(swapchain != nullptr);
        REQUIRE(swapchain->Width() == 640);
        REQUIRE(swapchain->Height() == 360);

        nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();

        auto renderFrame = [&](float red) {
            nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
            REQUIRE(backbuffer != nullptr);
            commandList->open();
            commandList->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                           nvrhi::Color(red, 0.2f, 0.4f, 1.0f));
            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            swapchain->Present();
        };

        for (int i = 0; i < 5; ++i)
            renderFrame(0.1f * (float)i);

        swapchain->Resize(800, 450);
        REQUIRE(swapchain->Width() == 800);
        REQUIRE(swapchain->Height() == 450);

        for (int i = 0; i < 2; ++i)
            renderFrame(0.8f);
    }
}

TEST_CASE("d3d12: windowed swapchain clears, presents, resizes", "[gpu][d3d12]")
{
    RunWindowedClearFrames(Arcane::GraphicsBackend::D3D12);
}
