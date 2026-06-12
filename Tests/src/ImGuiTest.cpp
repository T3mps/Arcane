// ImGui arrival gate: core compiles into the DLL's workspace flavor and a
// context round-trips headlessly. The renderer/platform integration test
// joins this file with the ImGuiLayer task.

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
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);  // builds default font
    ImGui::NewFrame();
    ImGui::Begin("smoke");
    ImGui::Text("hello");
    ImGui::End();
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData() != nullptr);
    ImGui::DestroyContext(ctx);
}
