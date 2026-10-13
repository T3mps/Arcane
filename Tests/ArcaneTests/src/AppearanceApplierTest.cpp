// Settings arc S4 (spec s7.1): a published theme change re-applies the editor
// style once, colours only; the boot theme applies nothing.
#include <catch2/catch_test_macros.hpp>
#include "Settings/AppearanceApplier.hpp"
#include "Settings/EditorThemeSettings.hpp"
#include "Settings/EditorUiSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/UiMetrics.hpp"
#include <imgui.h>
#include <imgui_internal.h>   // ImTrunc: ScaleAllSizes truncates the scaled paddings
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

// Settings arc S4-15 (spec s7.3): editor.ui.scale recomposes the metrics from
// the boot style; a font change is deferred to the next frame's take.
TEST_CASE("AppearanceApplier: default UI settings change nothing, whatever the monitor DPI", "[settings-ui][editor]")
{
    ImGuiStyle style; ApplyEditorTheme(style);
    const ImGuiStyle boot = style;
    AppearanceApplier applier; applier.Init(style);
    const Ui::ScopedMetrics restore(Ui::Metrics{});
    CHECK_FALSE(applier.UpdateUi(EditorUiSettings{}, /*displayScale*/ 1.5f, style));
    CHECK(applier.MetricsApplies() == 0);
    CHECK_FALSE(applier.TakeFontRebuild().has_value());
    CHECK(style.FramePadding.x == boot.FramePadding.x);
    CHECK(style.FontScaleMain == boot.FontScaleMain);
    CHECK(Ui::Px(8.0f) == 8.0f);
    CHECK(Ui::FontPx(13.0f) == 13.0f);
}

TEST_CASE("AppearanceApplier: scale recomposes the metrics from the boot style; back to 1.0 is the boot style", "[settings-ui][editor]")
{
    ImGuiStyle style; ApplyEditorTheme(style);
    const ImGuiStyle boot = style;
    AppearanceApplier applier; applier.Init(style);
    const Ui::ScopedMetrics restore(Ui::Metrics{});
    EditorUiSettings ui; ui.scale = 1.5f;
    CHECK(applier.UpdateUi(ui, 1.0f, style));
    CHECK(style.FontScaleMain == 1.5f);
    CHECK(style.FramePadding.x == ImTrunc(boot.FramePadding.x * 1.5f));
    CHECK(Ui::Px(8.0f) == 12.0f);
    CHECK_FALSE(applier.TakeFontRebuild().has_value());      // scale never rebuilds the atlas
    ui.scale = 1.0f;
    CHECK(applier.UpdateUi(ui, 1.0f, style));
    CHECK(style.FramePadding.x == boot.FramePadding.x);
    CHECK(style.FontScaleMain == 1.0f);
    ui.followDpi = true;
    CHECK(applier.UpdateUi(ui, 1.25f, style));
    CHECK(EffectiveUiScale(ui, 1.25f) == 1.25f);
    CHECK(style.FontScaleMain == 1.25f);
}

TEST_CASE("AppearanceApplier: a font change is handed over once, on the NEXT frame's take", "[settings-ui][editor]")
{
    ImGuiStyle style; ApplyEditorTheme(style);
    AppearanceApplier applier; applier.Init(style);
    const Ui::ScopedMetrics restore(Ui::Metrics{});
    CHECK_FALSE(applier.TakeFontRebuild().has_value());       // frame N: nothing queued yet
    EditorUiSettings ui; ui.fontSize = 18.0f; ui.fontFamily = "Roboto";
    CHECK(applier.UpdateUi(ui, 1.0f, style));                 // frame N: queued
    CHECK(Ui::FontPx(16.0f) == 18.0f);
    const auto first = applier.TakeFontRebuild();              // frame N+1
    REQUIRE(first.has_value());
    CHECK(first->fontFamily == "Roboto");
    CHECK_FALSE(applier.UpdateUi(ui, 1.0f, style));           // unchanged since
    CHECK_FALSE(applier.TakeFontRebuild().has_value());
    CHECK(applier.FontRebuilds() == 1);
}
