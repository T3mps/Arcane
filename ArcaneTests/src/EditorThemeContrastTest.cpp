// Node page phase s6.1/s6.6: the editor theme's contrast bars, computed from
// the float tokens with Theme::ContrastRatio (WCAG 2.x, EditorTheme.hpp). No
// ImGui context: ApplyEditorTheme fills a caller-owned ImGuiStyle, and
// StyleColorsDark takes a destination pointer (imgui_draw.cpp), so nothing
// here touches GImGui.
// Resting state only: an icon on the HOVERED accent is 2.87:1, under the 3:1
// bar, and hover is transient (drafting pick, 9.28).
#include <catch2/catch_test_macros.hpp>
#include <Widgets/EditorTheme.hpp>
#include <imgui.h>

using namespace Arcane::Editor;

namespace
{
    bool SameColor(const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
    ImGuiStyle ThemedStyle() { ImGuiStyle s; ApplyEditorTheme(s); return s; }
}

TEST_CASE("EditorTheme: kAccent reads against a button, the tab strip, and under a lit icon", "[editor][theme]")
{
    CHECK(Theme::ContrastRatio(Theme::kAccent, Theme::kButton) >= 3.0f);   // lit vs unlit toggle (3.21:1)
    CHECK(Theme::ContrastRatio(Theme::kAccent, Theme::kChrome) >= 3.0f);   // the overline on the strip (4.21:1)
    CHECK(Theme::ContrastRatio(Theme::kText, Theme::kAccent) >= 3.0f);     // a lit toggle's icon (3.16:1)
}

TEST_CASE("EditorTheme: the selected-tab overline is the accent, 2 px, and 45% on unfocused nodes", "[editor][theme]")
{
    const ImGuiStyle s = ThemedStyle();
    CHECK(SameColor(s.Colors[ImGuiCol_TabSelectedOverline], Theme::kAccent));
    CHECK(SameColor(s.Colors[ImGuiCol_TabDimmedSelectedOverline], Theme::WithAlpha(Theme::kAccent, 0.45f)));
    CHECK(s.Colors[ImGuiCol_TabDimmedSelectedOverline].w == 0.45f);
    CHECK(s.TabBarOverlineSize == 2.0f);
    CHECK(s.TabRounding == 2.0f);   // user, 2026-10-02: less rounded tabs (ImGui default 5)
    // kSelection keeps the SELECTED things (s6.1): rows, text selection, docking preview.
    CHECK(SameColor(s.Colors[ImGuiCol_Header], Theme::kSelection));
}

TEST_CASE("EditorTheme: focus re-tones neither the tab well nor a tab -- only the overline marks it", "[editor][theme]")
{
    // User, 2026-10-02: "I just want the accent to show the selection". A
    // focused dock node's tab-bar well and tabs keep the unfocused tones; the
    // full-vs-45% accent overline is the one focus cue.
    const ImGuiStyle s = ThemedStyle();
    CHECK(SameColor(s.Colors[ImGuiCol_TitleBgActive], s.Colors[ImGuiCol_TitleBg]));
    CHECK(SameColor(s.Colors[ImGuiCol_TabDimmed], s.Colors[ImGuiCol_Tab]));
    CHECK(SameColor(s.Colors[ImGuiCol_TabDimmedSelected], s.Colors[ImGuiCol_TabSelected]));
}

TEST_CASE("EditorTheme: Visual Studio tabs -- unselected tabs draw no fill, the selected one is a real tab, hover lifts", "[editor][theme]")
{
    // User, 2026-10-02: "the selected tab should look like a tab, and the
    // others should sit where the tab would be but only show the text".
    const ImGuiStyle s = ThemedStyle();
    CHECK(s.Colors[ImGuiCol_Tab].w == 0.0f);
    CHECK(s.Colors[ImGuiCol_TabDimmed].w == 0.0f);
    CHECK(s.Colors[ImGuiCol_TabSelected].w == 1.0f);
    CHECK(s.Colors[ImGuiCol_TabHovered].w > 0.0f);   // kept: hover shows a faint tab (user's pick)
}

TEST_CASE("EditorTheme: kToggleOn* resolve to the accent trio", "[editor][theme]")
{
    CHECK(SameColor(Theme::kToggleOn,        Theme::kAccent));
    CHECK(SameColor(Theme::kToggleOnHovered, Theme::kAccentHovered));
    CHECK(SameColor(Theme::kToggleOnActive,  Theme::kAccentActive));
}

namespace
{
    ImVec4 Composite(const ImVec4& fg, const ImVec4& bg, float alpha)
    {
        return ImVec4(fg.x * alpha + bg.x * (1.0f - alpha), fg.y * alpha + bg.y * (1.0f - alpha),
                      fg.z * alpha + bg.z * (1.0f - alpha), 1.0f);
    }
}

TEST_CASE("EditorTheme: dim text reads at 5:1 on the panel", "[editor][theme]")
{
    CHECK(Theme::ContrastRatio(Theme::kTextDim, Theme::kPanel) >= 5.0f);    // 5.09:1 (was 3.52:1)
    CHECK(Theme::ContrastRatio(Theme::kTextDim, Theme::kChrome) >= 5.0f);   // 5.37:1
    CHECK(Theme::ContrastRatio(Theme::kTextDim, Theme::kWell) >= 5.0f);     // 5.72:1
}

TEST_CASE("EditorTheme: disabled text is dimmer than dim text", "[editor][theme]")
{
    const ImGuiStyle s = ThemedStyle();
    CHECK(s.DisabledAlpha == 0.45f);
    // Disabled kText composites to #757575 over the panel; dim text is #8e8e8e.
    // Both are lighter than kPanel, so against that same darker background the
    // contrast ratio orders exactly as luminance does: this is s6.6's luminance
    // assertion, measured with the one shared helper.
    CHECK(Theme::ContrastRatio(Composite(Theme::kText, Theme::kPanel, s.DisabledAlpha), Theme::kPanel)
          < Theme::ContrastRatio(Theme::kTextDim, Theme::kPanel));
}
