// THE LANDING'S HEADLESS HALF (NRI Phase 3, Task 6): the host-side shapes the
// one-device flip introduced, exercised with NO graphics device of any kind.
//
// What is here:
//   * GpuContext::CreateForGraph builds the device-less half and ONLY that
//     half -- window, device-less Batcher2D, graph ImGuiLayer, input -- and
//     answers GraphFlavor() true. That predicate is what every host branch
//     gates on, so pinning it is pinning the boot split.
//   * ImGuiLayer::CreateForGraph pins its own context in BeginFrame and in
//     RenderToDrawData/EndFrameDiscard even when another context was left
//     current. That pin is the Phase-2 carry this task closes: the graph
//     render arm used to call ImGui::Render()/EndFrame() BARE.
//
// What is NOT here, and why:
//   * The gated NVRHI accessors (Device/Swap/Shaders/Cnv/Tone/Cmd/...) cannot
//     be exercised: they ARC_ASSERT, and a failing Mosaic assert calls
//     std::abort() (Mosaic/Assert.hpp, FailFatal). The tree has no death-test
//     idiom, so the pinned observable is GraphFlavor() -- the gate every call
//     site is required to consult -- rather than the abort behind it.
//   * NriGraphContext's borrowed-window mode needs a real device and a real
//     swapchain; it is desk checkpoint D3b's, per the phase's no-GPU-in-session
//     rule.
//
// Windows are created HIDDEN throughout (GpuContext::CreateForGraph does so
// itself), so this runs on an agent without flashing windows.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <imgui.h>

#include <glm/glm.hpp>

TEST_CASE("nri landing: GpuContext's graph flavor builds the device-less half and nothing else",
          "[nri]")
{
    Arcane::HostConfig cfg;
    cfg.backend = Arcane::GraphicsBackend::D3D12;   // never used: no device is created

    auto gpu = Arcane::GpuContext::CreateForGraph(cfg);
    REQUIRE(gpu != nullptr);

    // THE GATE every host call site branches on.
    CHECK(gpu->GraphFlavor());

    // The window is real (the NRI swapchain binds it at MainLoop time) and
    // hidden (the host reveals it once the graph vehicle exists).
    CHECK(gpu->Win().NativeHandle() != nullptr);
    std::uint32_t w = 0, h = 0;
    gpu->Win().GetPixelSize(w, h);
    CHECK(w > 0);
    CHECK(h > 0);

    // The data-supply side is fully live on a device-less Batcher2D -- this is
    // what the graph's Batch2DNode drains, and it is why the NVRHI device could
    // be dropped at all (Task 2's severance).
    gpu->Batch().Begin(nullptr, nullptr, 128, 64);
    gpu->Batch().Rect(glm::vec2(0.0f), glm::vec2(16.0f), glm::vec4(1.0f));
    const Arcane::Batch2DDrained drained = gpu->Batch().Drain();
    CHECK(drained.vertices.size() == 4);
    CHECK(drained.indices.size() == 6);
    CHECK(drained.viewport == glm::vec2(128.0f, 64.0f));

    // The input stack and the ImGui facade exist -- the host samples both every
    // frame regardless of which recorder it has.
    (void)gpu->InDevices();
    (void)gpu->Input();
    (void)gpu->Imgui().WantCaptureMouse();
    (void)gpu->Imgui().WantCaptureKeyboard();
}

TEST_CASE("nri landing: the graph ImGuiLayer pins its own context across Begin/Render",
          "[nri]")
{
    Arcane::Window window;
    Arcane::WindowDesc wd;
    wd.title  = "ArcaneTests nri imgui";
    wd.width  = 320;
    wd.height = 200;
    wd.hidden = true;
    REQUIRE(window.Create(wd));

    auto layer = Arcane::ImGuiLayer::CreateForGraph(window);
    REQUIRE(layer != nullptr);

    // The layer's own context: ImGui::CreateContext left it current, and it is
    // what every entry point below must re-pin to.
    ImGuiContext* const owned = ImGui::GetCurrentContext();
    REQUIRE(owned != nullptr);

    // Build the atlas eagerly: this flavor installs NO renderer backend (the
    // graph's ImGuiNriNode is the renderer, and it is not in play headlessly),
    // so nothing would otherwise service the 1.92 texture protocol. Same
    // recipe as ImGuiTest's headless smoke.
    {
        unsigned char* pixels = nullptr;
        int aw = 0, ah = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &aw, &ah);
        REQUIRE(pixels != nullptr);
    }

    // THE HAZARD, reproduced: a second context, left current. Before Task 6 the
    // graph render arm called ImGui::Render()/EndFrame() bare, so this state
    // would have ended the WRONG frame.
    ImGuiContext* const foreign = ImGui::CreateContext();
    REQUIRE(foreign != nullptr);

    ImGui::SetCurrentContext(foreign);
    layer->BeginFrame();                       // must re-pin
    CHECK(ImGui::GetCurrentContext() == owned);
    ImGui::Begin("landing");
    ImGui::Text("one device");
    ImGui::End();

    ImGui::SetCurrentContext(foreign);         // steal it again mid-frame
    ImDrawData* const drawData = layer->RenderToDrawData();
    CHECK(ImGui::GetCurrentContext() == owned);
    REQUIRE(drawData != nullptr);
    CHECK(drawData->Valid);                    // Render() ran on a context that HAD a frame

    // The discard half of the pair (both non-Full golden stages take it) also
    // pins, and leaves the frame balanced so the next BeginFrame is legal.
    ImGui::SetCurrentContext(foreign);
    layer->BeginFrame();
    CHECK(ImGui::GetCurrentContext() == owned);
    ImGui::SetCurrentContext(foreign);
    layer->EndFrameDiscard();
    CHECK(ImGui::GetCurrentContext() == owned);
    layer->BeginFrame();                       // would assert on an unbalanced frame
    layer->EndFrameDiscard();

    ImGui::DestroyContext(foreign);
    layer.reset();
    window.Destroy();
}
