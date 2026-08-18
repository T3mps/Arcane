// THE LANDING'S HEADLESS HALF (NRI Phase 3, Task 6): the host-side shapes the
// one-device flip introduced, exercised with NO graphics device of any kind.
//
// What is here:
//   * GpuContext::CreateForGraph builds the device-less half and ONLY that
//     half -- window, device-less Batcher2D, graph ImGuiLayer, input -- and
//     answers GraphFlavor() true. That predicate is what every host branch
//     gates on, so pinning it is pinning the boot split.
//   * ImGuiLayer::Create pins its own context in BeginFrame and in
//     RenderToDrawData/EndFrameDiscard even when another context was left
//     current. That pin is the Phase-2 carry this task closes: the graph
//     render arm used to call ImGui::Render()/EndFrame() BARE.
//   * (Task 8) The EDITOR's graph-mode frame tail -- BeginFrame then
//     EndFrameDiscard, repeatedly, through the device-less GpuContext -- is
//     legal and balanced. That is the mid-phase decision Task 8 made about
//     what the editor's main window does before Task 10 gives it a chrome
//     frame, and an unbalanced pair there is a HANG rather than a bad pixel.
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

TEST_CASE("nri landing: the editor's graph-mode frame tail -- Begin then discard, repeatable, "
          "through the device-less GpuContext", "[nri]")
{
    // NRI Phase 3, TASK 8, and it pins a DECISION rather than a library
    // property. The editor's graph mode composites no chrome until Task 10, so
    // EditorAppFrame::PresentFrame's graph arm does exactly one thing:
    // Imgui().EndFrameDiscard(), pairing the BeginFrame that DrawEditorUi made
    // earlier in the same frame. THE HAZARD IT CLOSES IS A HANG, not a wrong
    // pixel -- an unpaired BeginFrame asserts inside ImGui on the NEXT frame,
    // and returning "no backbuffer" from that arm instead would make MainLoop
    // `continue` past EndFrame every frame, freezing the --frames N budget and
    // the plugin hot-reload poll forever.
    //
    // Driven THROUGH GpuContext rather than a bare ImGuiLayer (which the case
    // below already covers) because that is what the editor holds, and the
    // whole claim is that this pair is legal with no renderer, no swapchain, no
    // command list and no GpuFrameProgress anywhere in the object.
    ImGuiContext* const previous = ImGui::GetCurrentContext();

    Arcane::HostConfig cfg;
    cfg.backend = Arcane::GraphicsBackend::D3D12;   // never used: no device is created

    auto gpu = Arcane::GpuContext::CreateForGraph(cfg);
    REQUIRE(gpu != nullptr);
    REQUIRE(gpu->GraphFlavor());

    // The atlas, eagerly: this flavor installs no renderer backend, so nothing
    // would otherwise service the 1.92 texture protocol. Same recipe as the
    // case below and as ImGuiTest's headless smoke.
    {
        unsigned char* pixels = nullptr;
        int aw = 0, ah = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &aw, &ah);
        REQUIRE(pixels != nullptr);
    }

    // Three frames of the editor's graph-mode tail, with real submission in
    // between -- the repeatability is the property: frame N+1's BeginFrame is
    // what would assert if frame N's discard had left the frame unbalanced.
    for (int frame = 0; frame < 3; ++frame)
    {
        gpu->Imgui().BeginFrame();
        ImGui::Begin("editor chrome stand-in");
        ImGui::Text("frame %d", frame);
        ImGui::End();
        gpu->Imgui().EndFrameDiscard();
    }

    // And the arm is interchangeable with the rendering one at any frame
    // boundary, which is what lets Task 10 replace the discard with a real
    // chrome frame without touching anything else in the loop.
    gpu->Imgui().BeginFrame();
    ImDrawData* const drawData = gpu->Imgui().RenderToDrawData();
    REQUIRE(drawData != nullptr);
    CHECK(drawData->Valid);
    gpu->Imgui().BeginFrame();
    gpu->Imgui().EndFrameDiscard();

    gpu.reset();
    ImGui::SetCurrentContext(previous);
}

TEST_CASE("nri landing: the graph ImGuiLayer installs its platform backend on ITS OWN context",
          "[nri]")
{
    // Fix round 1. ImGui::CreateContext RESTORES whatever context was current
    // if there was one, so a layer built while ANOTHER context is current can
    // hand its platform backend to that other context -- and then abort on the
    // first BeginFrame (or trip imgui_impl_sdl3's own "Already initialized a
    // platform backend!" assert). Neither host reaches this today, so the
    // property has to be stated somewhere a future second-context host (the
    // editor's own flip) cannot quietly break it.
    //
    // Device-free in full: the graph flavor builds no renderer at all, so the
    // whole scenario runs on NONE.
    Arcane::Window window;
    Arcane::WindowDesc wd;
    wd.title  = "ArcaneTests nri imgui decoy";
    wd.width  = 320;
    wd.height = 200;
    wd.hidden = true;
    REQUIRE(window.Create(wd));

    // The decoy, left current -- exactly the state CreateContext restores.
    ImGuiContext* const decoy = ImGui::CreateContext();
    REQUIRE(decoy != nullptr);
    ImGui::SetCurrentContext(decoy);

    auto layer = Arcane::ImGuiLayer::Create(window);
    REQUIRE(layer != nullptr);

    // The PRIMARY layer leaves its OWN context current (deliberately unlike
    // OffscreenImGuiLayer, which restores): both hosts read
    // ImGui::GetCurrentContext() right after GpuContext::Create to publish it
    // across the plugin ABI. So this doubles as the layer's context handle.
    ImGuiContext* const owned = ImGui::GetCurrentContext();
    REQUIRE(owned != nullptr);
    REQUIRE(owned != decoy);

    // The SDL3 platform backend landed here...
    CHECK(ImGui::GetIO().BackendPlatformUserData != nullptr);
    CHECK(ImGui::GetIO().BackendPlatformName != nullptr);

    // ...and NOT on the decoy, which is precisely what a missing pin would do.
    ImGui::SetCurrentContext(decoy);
    CHECK(ImGui::GetIO().BackendPlatformUserData == nullptr);
    CHECK(ImGui::GetIO().BackendPlatformName == nullptr);

    ImGui::SetCurrentContext(owned);
    layer.reset();                  // pins + shuts the backend down on `owned`
    ImGui::DestroyContext(decoy);
    window.Destroy();
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

    auto layer = Arcane::ImGuiLayer::Create(window);
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

TEST_CASE("the ImGui layer has exactly one flavor and it installs the event tap", "[nri]")
{
    // Phase 3's unclickable-editor defect (@f7001ad9) was a stance applied to
    // one of two flavors. One factory makes that class of bug inexpressible.
    // The tap is what carries mouse BUTTON events (imgui_impl_sdl3 turns
    // SDL_EVENT_MOUSE_BUTTON_* into AddMouseButtonEvent; nothing polls them),
    // so a layer without it is a layer nothing can be clicked in.
    Arcane::Window window;
    Arcane::WindowDesc wd; wd.title = "ArcaneTests imgui"; wd.width = 320; wd.height = 200; wd.hidden = true;
    REQUIRE(window.Create(wd));

    auto layer = Arcane::ImGuiLayer::Create(window);
    REQUIRE(layer != nullptr);
    CHECK(window.HasNativeEventTap());

    layer.reset();
    CHECK_FALSE(window.HasNativeEventTap());   // teardown uninstalls
    window.Destroy();
}
