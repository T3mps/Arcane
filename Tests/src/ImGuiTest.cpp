// ImGui arrival gate: core compiles into the DLL's workspace flavor and a
// context round-trips headlessly (the headless smoke). The GPU integration
// test below drives the first-party imgui_impl_nvrhi backend through
// ImGuiLayer: one demo frame must paint pixels into an offscreen
// display-referred target on both backends, validation silent.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include <imgui.h>

#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

TEST_CASE("imgui: context creates headlessly", "[imgui]")
{
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640, 360);
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    // GetTexDataAsRGBA32 deliberately precedes NewFrame here: this headless
    // smoke builds the atlas eagerly (legacy-but-present in 1.92 -- under
    // RendererHasTextures the atlas would otherwise build lazily on render).
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);  // builds default font
    ImGui::NewFrame();
    ImGui::Begin("smoke");
    ImGui::Text("hello");
    ImGui::End();
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData() != nullptr);
    ImGui::DestroyContext(ctx);
}

namespace
{
    // One demo frame through ImGuiLayer + imgui_impl_nvrhi must paint pixels
    // into a display-referred offscreen target (256x144: large enough that
    // the demo window draws something visible). The window is hidden so CI
    // runs without flashing; it supplies DisplaySize to the sdl3 backend.
    void CheckImGuiFrameRenders(Arcane::GraphicsBackend backend)
    {
        constexpr uint32_t kW = 256, kH = 144;

        Arcane::Window window;
        Arcane::WindowDesc wdesc;
        wdesc.title  = "ArcaneTests imgui";
        wdesc.width  = kW;
        wdesc.height = kH;
        wdesc.hidden = true;
        wdesc.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
        REQUIRE(window.Create(wdesc));

        Arcane::RenderDeviceDesc ddesc;
        ddesc.backend = backend;
        auto device = Arcane::RenderDevice::Create(ddesc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "shaders");
        REQUIRE(shaders != nullptr);

        auto imgui = Arcane::ImGuiLayer::Create(window, *device, *shaders);
        REQUIRE(imgui != nullptr);

        // Display-referred offscreen target standing in for the backbuffer.
        const float clearR = 0.10f, clearG = 0.12f, clearB = 0.15f;
        auto outDesc = nvrhi::TextureDesc()
            .setWidth(kW).setHeight(kH)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true)
            .setDebugName("ImGuiOut");
        nvrhi::TextureHandle output = nv->createTexture(outDesc);
        REQUIRE(output != nullptr);
        nvrhi::FramebufferHandle outputFb = nv->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(output));
        REQUIRE(outputFb != nullptr);

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(kW).setHeight(kH)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("ImGuiReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);

        // Two frames: ImGui settles a freshly-created window's auto-fit size
        // over its first frame (frame 1 emits no geometry until the size is
        // known), so the visible draw lands on frame 2. Each BeginFrame must
        // be paired with a Render to close the ImGui frame.
        auto drawDemoFrame = [&](nvrhi::IFramebuffer* fb) {
            window.PumpEvents();
            imgui->BeginFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2((float)kW, (float)kH));
            bool open = true;
            ImGui::ShowDemoWindow(&open);

            nvrhi::CommandListHandle cl = nv->createCommandList();
            cl->open();
            cl->clearTextureFloat(output, nvrhi::AllSubresources,
                                  nvrhi::Color(clearR, clearG, clearB, 1.0f));
            imgui->Render(cl, fb);
            cl->close();
            nv->executeCommandList(cl);
            nv->waitForIdle();
        };

        drawDemoFrame(outputFb);  // frame 1: window settles its auto-fit size
        drawDemoFrame(outputFb);  // frame 2: the demo paints

        nvrhi::CommandListHandle commandList = nv->createCommandList();
        commandList->open();
        commandList->copyTexture(staging, nvrhi::TextureSlice(),
                                 output, nvrhi::TextureSlice());
        commandList->close();
        nv->executeCommandList(commandList);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);

        // The clear bytes in BGRA UNORM (the value ImGui must overdraw).
        const uint8_t clearBy[3] = {
            (uint8_t)(clearB * 255.0f + 0.5f),
            (uint8_t)(clearG * 255.0f + 0.5f),
            (uint8_t)(clearR * 255.0f + 0.5f) };
        int differing = 0;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
            {
                const uint8_t* p = pixels + y * rowPitch + x * 4;
                if (std::abs((int)p[0] - clearBy[0]) > 4 ||
                    std::abs((int)p[1] - clearBy[1]) > 4 ||
                    std::abs((int)p[2] - clearBy[2]) > 4)
                    ++differing;
            }
        CHECK(differing > 100);  // the demo window paints a meaningful area
        nv->unmapStagingTexture(staging);
        nv->runGarbageCollection();

        CHECK(Arcane::RenderErrorCount() == 0);

        // Tear the layer down before the window (the layer taps it).
        imgui.reset();
        window.Destroy();
    }
}

TEST_CASE("d3d12: ImGui demo frame renders through imgui_impl_nvrhi",
          "[gpu][d3d12][imgui]")
{
    CheckImGuiFrameRenders(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: ImGui demo frame renders through imgui_impl_nvrhi",
          "[gpu][vulkan][imgui]")
{
    CheckImGuiFrameRenders(Arcane::GraphicsBackend::Vulkan);
}
