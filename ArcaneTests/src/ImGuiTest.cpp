// ImGui arrival gate: core compiles into the DLL's workspace flavor and a
// context round-trips headlessly (the headless smoke).
//
// ImGuiLayer has ONE flavor: RenderToDrawData() is its only renderer entry
// point, and the frame graph's ImGuiNriNode is the only thing that ever draws
// the result.
//
// A NAMED COVERAGE GAP: nothing in the suite proves ImGui draw data actually
// paints correct pixels through ImGuiNriNode with a real device and a pixel
// readback. It joins the sibling pixel gaps named in SelectionOutlineTest.cpp
// and PickBufferTest.cpp.

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
