// SeverityToggle (node-page phase s8.2): one IconToggle whose label carries the
// count; dim at zero, tinted above; a click flips `on`. Device-less ImGui.
#include <catch2/catch_test_macros.hpp>
#include <Widgets/EditorWidgets.hpp>
#include <Widgets/EditorTheme.hpp>
#include <Panels/SeverityStyle.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <Widgets/IconsLucide.h>

#include <string_view>

namespace
{
    struct ToggleHarness
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = nullptr;
        bool on = true;
        std::size_t count = 3;
        bool clicked = false;
        ImVec2 centre{};
        int stackDrift = 0;
        ToggleHarness()
        {
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~ToggleHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(400, 200));
            ImGui::Begin("T");
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            const int c0 = g.ColorStack.Size, v0 = g.StyleVarStack.Size;
            clicked = Arcane::Editor::SeverityToggle("warn", "W", Arcane::Editor::Theme::kWarning, count, on) || clicked;
            stackDrift += (g.ColorStack.Size - c0) + (g.StyleVarStack.Size - v0);
            centre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            ImGui::End();
            ImGui::Render();
        }
        bool DrewColour(const ImVec4& c) const
        {
            const ImU32 want = ImGui::ColorConvertFloat4ToU32(c);
            for (const ImDrawVert& v : ImGui::FindWindowByName("T")->DrawList->VtxBuffer) if (v.col == want) return true;
            return false;
        }
    };
}

TEST_CASE("SeverityToggle: the count is in the label, tinted above zero, dim at zero; a click flips it", "[editor]")
{
    ToggleHarness h;
    h.Frame();
    CHECK(h.DrewColour(Arcane::Editor::Theme::kWarning));
    h.count = 0;
    h.Frame();
    CHECK(h.DrewColour(Arcane::Editor::Theme::kTextDim));
    CHECK_FALSE(h.DrewColour(Arcane::Editor::Theme::kWarning));
    CHECK(h.stackDrift == 0);

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(h.centre.x, h.centre.y); h.Frame();
    io.AddMouseButtonEvent(0, true);  h.Frame();
    io.AddMouseButtonEvent(0, false); h.Frame();
    CHECK(h.clicked);
    CHECK_FALSE(h.on);
}

TEST_CASE("SeverityStyle maps each severity to its icon and token", "[editor]")
{
    using Arcane::Editor::StyleFor;
    CHECK(StyleFor(Arcane::DiagSeverity::Error).color.x == Arcane::Editor::Theme::kError.x);
    CHECK(StyleFor(Arcane::DiagSeverity::Warning).color.y == Arcane::Editor::Theme::kWarning.y);
    CHECK(StyleFor(Arcane::DiagSeverity::Info).color.x == Arcane::Editor::Theme::kText.x);
    CHECK(std::string_view(StyleFor(Arcane::DiagSeverity::Error).icon)   == ICON_LC_CIRCLE_X);
    CHECK(std::string_view(StyleFor(Arcane::DiagSeverity::Warning).icon) == ICON_LC_TRIANGLE_ALERT);
    CHECK(std::string_view(StyleFor(Arcane::DiagSeverity::Info).icon)    == ICON_LC_INFO);
}
