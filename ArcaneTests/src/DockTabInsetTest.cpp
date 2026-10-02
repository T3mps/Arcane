// A dock node's first tab sits flush with the node's left edge when the node
// has no left window-menu button (Arcane's dockspace: NoWindowMenuButton). This
// pins the ARCANE LOCAL FIX in ThirdParty/imgui/imgui.cpp DockNodeCalcTabBarLayout
// (user desk, 2026-10-02: "the tab is inset a bit to the right"); upstream always
// insets the tab bar by FramePadding.x. A node WITH the left menu button keeps
// upstream's padding before its button.

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
    struct DockHarness
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;

        DockHarness()
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~DockHarness()
        {
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
        }

        // Two tabs in one dockspace node; returns that node after `frames` frames.
        ImGuiDockNode* Run(ImGuiDockNodeFlags spaceFlags, int frames = 3)
        {
            const ImGuiID dockId = ImHashStr("InsetDock");
            for (int i = 0; i < frames; ++i)
            {
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                if (i == 0)
                {
                    ImGui::DockBuilderRemoveNode(dockId);
                    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace | spaceFlags);
                    ImGui::DockBuilderSetNodeSize(dockId, ImVec2(600.0f, 300.0f));
                    ImGui::DockBuilderDockWindow("Inset A", dockId);
                    ImGui::DockBuilderDockWindow("Inset B", dockId);
                    ImGui::DockBuilderFinish(dockId);
                }
                ImGui::SetNextWindowPos(ImVec2(40.0f, 30.0f));
                ImGui::SetNextWindowSize(ImVec2(600.0f, 300.0f));
                ImGui::Begin("##insethost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), spaceFlags);
                ImGui::End();
                ImGui::Begin("Inset A"); ImGui::TextUnformatted("a"); ImGui::End();
                ImGui::Begin("Inset B"); ImGui::TextUnformatted("b"); ImGui::End();
                ImGui::Render();
            }
            return ImGui::DockBuilderGetNode(dockId);
        }
    };
}

TEST_CASE("Dock tab bar: no left window-menu button -> the first tab is flush with the node's left edge", "[editor][docking]")
{
    DockHarness h;
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_NoWindowMenuButton);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiStyle& style = ImGui::GetStyle();
    // Only the border separates the tab bar from the node edge -- no FramePadding.x.
    CHECK(node->TabBar->BarRect.Min.x == node->Pos.x + style.WindowBorderSize);
}

TEST_CASE("Dock tab bar: a left window-menu button keeps upstream's padding before it", "[editor][docking]")
{
    DockHarness h;
    ImGuiDockNode* node = h.Run(ImGuiDockNodeFlags_None);
    REQUIRE(node != nullptr);
    REQUIRE(node->TabBar != nullptr);
    const ImGuiStyle& style = ImGui::GetStyle();
    REQUIRE(node->HasWindowMenuButton);
    // Border + padding + the button + its spacing, exactly as upstream lays it out.
    CHECK(node->TabBar->BarRect.Min.x ==
          node->Pos.x + style.WindowBorderSize + style.FramePadding.x + ImGui::GetFontSize() + style.ItemInnerSpacing.x);
}
