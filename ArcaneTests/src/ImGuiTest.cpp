// ImGui arrival gate: core compiles into the DLL's workspace flavor and a
// context round-trips headlessly (the headless smoke).
//
// NRI Phase 5a, Task 5 deleted the GPU integration test that used to live
// below this comment: it drove the first-party imgui_impl_nvrhi backend
// (ImGuiNvrhiRenderer) through ImGuiLayer::Create(window, device, shaders) +
// ImGuiLayer::Render(cmd, fb), painting one demo frame into an offscreen
// display-referred target on both backends. Both of those entry points are
// gone -- ImGuiLayer collapsed to one flavor with no NVRHI renderer at all,
// RenderToDrawData() is the only renderer entry point, and the frame graph's
// ImGuiNriNode is the only thing that ever draws the result. This is a named
// COVERAGE GAP, not a silent drop, in the same shape as NRI Phase 5a, Task
// 4's render-pass deletions: nothing in the current suite proves ImGui draw
// data actually paints correct pixels through ImGuiNriNode with a real
// device and a pixel readback. See that task's report for the sibling gaps
// (selection outline, pick, sprite/post/material correctness) this joins on
// the Phase 5b carry list.

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

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
