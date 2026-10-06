// Settings arc S4 (spec s7.1): a published theme change re-applies the editor
// style once, colours only; the boot theme applies nothing.
#include <catch2/catch_test_macros.hpp>
#include "Settings/AppearanceApplier.hpp"
#include "Settings/EditorThemeSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include <imgui.h>
#include <cmath>
#include <cstring>

using namespace Arcane::Editor;

TEST_CASE("AppearanceApplier: the boot theme applies nothing; a changed token re-applies the colours once", "[theme][editor]")
{
    const Theme::ScopedLivePalette restore(Theme::kDarkPalette);   // the applier swaps the live palette
    ImGuiStyle style;
    ApplyEditorTheme(style);
    const ImGuiStyle boot = style;
    AppearanceApplier applier;
    applier.Init(style);

    CHECK_FALSE(applier.UpdateTheme(EditorThemeSettings{}, style));
    CHECK(applier.ThemeApplies() == 0);
    CHECK(std::memcmp(style.Colors, boot.Colors, sizeof style.Colors) == 0);

    EditorThemeSettings s;
    s.accent = ToSettingColor(ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    CHECK(applier.UpdateTheme(s, style));
    CHECK(applier.ThemeApplies() == 1);
    CHECK(std::abs(style.Colors[ImGuiCol_TabSelectedOverline].x - 0.8f) < 1e-5f);
    CHECK(std::abs(Theme::kAccent.x - 0.8f) < 1e-5f);          // the named token follows
    CHECK(style.FrameBorderSize == boot.FrameBorderSize);       // colours only
    CHECK(style.TabRounding == boot.TabRounding);

    CHECK_FALSE(applier.UpdateTheme(s, style));                 // unchanged: nothing re-applied
    CHECK(applier.ThemeApplies() == 1);

    CHECK(applier.UpdateTheme(EditorThemeSettings{}, style));   // back to Dark
    CHECK(std::memcmp(style.Colors, boot.Colors, sizeof style.Colors) == 0);
}
