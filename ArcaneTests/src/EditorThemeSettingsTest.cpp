// Settings arc S4 (spec s7.1): the 31 theme tokens are cvars of EditorThemeSettings,
// the tokens read the LIVE palette, and the Dark defaults are today's palette to the bit.
#include <catch2/catch_test_macros.hpp>
#include "Settings/EditorThemeSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <imgui.h>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <tuple>

using namespace Arcane::Editor;

namespace
{
    bool SameBits(const ImVec4& a, const ImVec4& b) { return std::memcmp(&a, &b, sizeof(ImVec4)) == 0; }
}

TEST_CASE("EditorThemeSettings: the defaults ARE today's Dark palette, bit for bit", "[theme][editor]")
{
    const Theme::Palette p = ToPalette(EditorThemeSettings{});
    for (const ThemeToken& t : kThemeTokens)
    {
        INFO(t.field);
        CHECK(SameBits(p.*(t.palette), Theme::kDarkPalette.*(t.palette)));
    }
}

TEST_CASE("EditorThemeSettings: each linear default decodes to its Dark token within 1e-4", "[theme][editor]")
{
    const EditorThemeSettings defaults{};
    for (const ThemeToken& t : kThemeTokens)
    {
        INFO(t.field);
        const ImVec4 d = ToDisplayColor(defaults.*(t.setting));
        const ImVec4& want = Theme::kDarkPalette.*(t.palette);
        CHECK(std::abs(d.x - want.x) < 1e-4f);
        CHECK(std::abs(d.y - want.y) < 1e-4f);
        CHECK(std::abs(d.z - want.z) < 1e-4f);
        CHECK(d.w == want.w);
    }
}

TEST_CASE("Theme tokens: the live palette starts as Dark and the themed style is today's", "[theme][editor]")
{
    CHECK(SameBits(Theme::kText, Theme::kDarkPalette.text));
    ImGuiStyle s;
    ApplyEditorTheme(s);
    CHECK(SameBits(s.Colors[ImGuiCol_Text], ImVec4(0.878f, 0.878f, 0.878f, 1.00f)));
    CHECK(SameBits(s.Colors[ImGuiCol_TableRowBgAlt], ImVec4(1.00f, 1.00f, 1.00f, 0.03f)));
    CHECK(SameBits(s.Colors[ImGuiCol_ModalWindowDimBg], ImVec4(0.02f, 0.02f, 0.02f, 0.55f)));
    CHECK(SameBits(s.Colors[ImGuiCol_NavWindowingDimBg], ImVec4(0.02f, 0.02f, 0.02f, 0.55f)));
    CHECK(SameBits(s.Colors[ImGuiCol_PlotHistogram], ImVec4(1.000f, 0.650f, 0.100f, 1.00f)));
    CHECK(SameBits(s.Colors[ImGuiCol_TabDimmedSelectedOverline], ImVec4(0.357f, 0.498f, 0.651f, 0.45f)));
    CHECK(s.FrameBorderSize == 1.0f);
    CHECK(s.TabRounding == 2.0f);
}

TEST_CASE("Theme tokens are live: a swapped palette re-tones the tokens and the applied colours", "[theme][editor]")
{
    Theme::Palette red = Theme::kDarkPalette;
    red.text = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    {
        const Theme::ScopedLivePalette scope(red);
        CHECK(Theme::kText.x == 1.0f);
        CHECK(Theme::kText.y == 0.0f);
        ImGuiStyle s;
        ApplyEditorThemeColors(s);
        CHECK(SameBits(s.Colors[ImGuiCol_Text], red.text));
        CHECK(SameBits(s.Colors[ImGuiCol_InputTextCursor], red.text));
        CHECK(s.FrameBorderSize == ImGuiStyle().FrameBorderSize);   // colours only: no metric touched
    }
    CHECK(SameBits(Theme::kText, Theme::kDarkPalette.text));        // the scope restored Dark
}

TEST_CASE("EditorThemeSettings: a changed colour converts linear -> display sRGB; the rest stay Dark", "[theme][editor]")
{
    EditorThemeSettings s;
    s.panel = ToSettingColor(ImVec4(0.5f, 0.25f, 1.0f, 1.0f));
    const Theme::Palette p = ToPalette(s);
    CHECK(std::abs(p.panel.x - 0.5f) < 1e-5f);
    CHECK(std::abs(p.panel.y - 0.25f) < 1e-5f);
    CHECK(std::abs(p.panel.z - 1.0f) < 1e-5f);
    CHECK(p.panel.w == 1.0f);
    CHECK(SameBits(p.text, Theme::kDarkPalette.text));
    CHECK_FALSE(SameThemeSettings(s, EditorThemeSettings{}));
    CHECK(SameThemeSettings(EditorThemeSettings{}, EditorThemeSettings{}));
}

TEST_CASE("kThemeTokens: one entry per Palette member; editor.theme.<field> is registered with its default", "[theme][editor]")
{
    STATIC_REQUIRE(sizeof(Theme::Palette) == std::tuple_size_v<decltype(kThemeTokens)> * sizeof(ImVec4));
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    reg.PublishImmediate();
    std::set<std::string> fields;
    const EditorThemeSettings defaults{};
    for (const ThemeToken& t : kThemeTokens)
    {
        INFO(t.field);
        CHECK(fields.insert(std::string(t.field)).second);
        CHECK_FALSE(t.displayName.empty());
        CHECK_FALSE(t.group.empty());
        const Arcane::CVarHandle h = reg.Find(ThemeCvarName(t.field));
        REQUIRE_FALSE(h.IsStale());
        const std::optional<Arcane::CVarValue> v = reg.Get(h);
        REQUIRE(v.has_value());
        REQUIRE(v->type == Arcane::CVarType::Color);
        const Arcane::CVarColor got = v->AsColor();
        const Arcane::CVarColor& want = defaults.*(t.setting);
        CHECK(std::memcmp(&got, &want, sizeof got) == 0);
    }
}
