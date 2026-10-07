// Settings sweep S6-28 (spec s16.11): the editor's style metrics and density
// values are editor.ui.* cvars with the pre-sweep values, ApplyEditorTheme
// takes them from the struct, a Live change re-applies them, and every hard
// pixel size is a base times the one UI scale (identical at scale 1).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/AppearanceApplier.hpp"
#include "Settings/EditorUiSettings.hpp"
#include "Settings/EditorUiStyleSettings.hpp"
#include "Widgets/EditorFonts.hpp"             // EditorFontRequest::altFace
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"           // TableRowHeight, AssetRowThumbSize
#include "Widgets/PropertyGrid.hpp"            // PropertyDragSpeed
#include "Widgets/UiScale.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // ImTrunc: what ScaleAllSizes rounds to
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>
using namespace Arcane;

namespace
{
    // Reverts the EditorUser rung the cases write, and republishes.
    struct EditorUserLayerReset
    {
        ~EditorUserLayerReset()
        {
            CVarRegistry& reg = CVarRegistry::Get();
            reg.RevertLayer(SetBy::EditorUser);
            reg.PublishImmediate();
        }
    };

    void SetUi(std::string_view name, const CVarValue& v)
    {
        CVarRegistry& reg = CVarRegistry::Get();
        REQUIRE(reg.Set(reg.Find(name), v, SetBy::EditorUser, "editor", CVarContext::Editor) == SetResult::Applied);
        reg.PublishImmediate();
    }

    // ImGuiStyle field by field. NOT a memcmp over the struct: it has padding
    // (3 bytes after DockingNodeHasCloseButton, 1 after the AntiAliased*
    // bools) that its member-wise constructor never writes, so two equal
    // styles can differ there (Debug's /RTC 0xCC stack fill hid it). Every
    // member of the vendored ImGui's ImGuiStyle is listed; Colors is an ImVec4 array
    // (no padding) and compares bytewise. Returns the first differing field
    // name, or "" when the styles are equal.
    std::string_view StyleDiff(const ImGuiStyle& a, const ImGuiStyle& b)
    {
#define ARC_STYLE_F(f) if (!Test::SameBits(a.f, b.f)) return #f;
#define ARC_STYLE_V(f) if (!Test::SameBits(a.f.x, b.f.x) || !Test::SameBits(a.f.y, b.f.y)) return #f;
#define ARC_STYLE_E(f) if (a.f != b.f) return #f;
        ARC_STYLE_F(FontSizeBase) ARC_STYLE_F(FontScaleMain) ARC_STYLE_F(FontScaleDpi) ARC_STYLE_F(Alpha)
        ARC_STYLE_F(DisabledAlpha) ARC_STYLE_F(WindowRounding) ARC_STYLE_F(WindowBorderSize) ARC_STYLE_F(WindowBorderHoverPadding)
        ARC_STYLE_F(ChildRounding) ARC_STYLE_F(ChildBorderSize) ARC_STYLE_F(PopupRounding) ARC_STYLE_F(PopupBorderSize)
        ARC_STYLE_F(FrameRounding) ARC_STYLE_F(FrameBorderSize) ARC_STYLE_F(IndentSpacing) ARC_STYLE_F(ColumnsMinSpacing)
        ARC_STYLE_F(ScrollbarSize) ARC_STYLE_F(ScrollbarRounding) ARC_STYLE_F(ScrollbarPadding) ARC_STYLE_F(GrabMinSize)
        ARC_STYLE_F(GrabRounding) ARC_STYLE_F(LogSliderDeadzone) ARC_STYLE_F(ImageRounding) ARC_STYLE_F(ImageBorderSize)
        ARC_STYLE_F(TabRounding) ARC_STYLE_F(TabBorderSize) ARC_STYLE_F(TabMinWidthBase) ARC_STYLE_F(TabMinWidthShrink)
        ARC_STYLE_F(TabCloseButtonMinWidthSelected) ARC_STYLE_F(TabCloseButtonMinWidthUnselected) ARC_STYLE_F(TabBarBorderSize) ARC_STYLE_F(TabBarOverlineSize)
        ARC_STYLE_F(TableAngledHeadersAngle) ARC_STYLE_F(TreeLinesSize) ARC_STYLE_F(TreeLinesRounding) ARC_STYLE_F(DragDropTargetRounding)
        ARC_STYLE_F(DragDropTargetBorderSize) ARC_STYLE_F(DragDropTargetPadding) ARC_STYLE_F(ColorMarkerSize) ARC_STYLE_F(SeparatorSize)
        ARC_STYLE_F(SeparatorTextBorderSize) ARC_STYLE_F(DockingSeparatorSize) ARC_STYLE_F(MouseCursorScale) ARC_STYLE_F(CurveTessellationTol)
        ARC_STYLE_F(CircleTessellationMaxError) ARC_STYLE_F(HoverStationaryDelay) ARC_STYLE_F(HoverDelayShort) ARC_STYLE_F(HoverDelayNormal)
        ARC_STYLE_F(_MainScale) ARC_STYLE_F(_NextFrameFontSizeBase)
        ARC_STYLE_V(WindowPadding) ARC_STYLE_V(WindowMinSize) ARC_STYLE_V(WindowTitleAlign) ARC_STYLE_V(FramePadding)
        ARC_STYLE_V(ItemSpacing) ARC_STYLE_V(ItemInnerSpacing) ARC_STYLE_V(CellPadding) ARC_STYLE_V(TouchExtraPadding)
        ARC_STYLE_V(TableAngledHeadersTextAlign) ARC_STYLE_V(ButtonTextAlign) ARC_STYLE_V(SelectableTextAlign) ARC_STYLE_V(SeparatorTextAlign)
        ARC_STYLE_V(SeparatorTextPadding) ARC_STYLE_V(DisplayWindowPadding) ARC_STYLE_V(DisplaySafeAreaPadding)
        ARC_STYLE_E(WindowMenuButtonPosition) ARC_STYLE_E(TreeLinesFlags) ARC_STYLE_E(ColorButtonPosition) ARC_STYLE_E(DockingNodeHasCloseButton)
        ARC_STYLE_E(AntiAliasedLines) ARC_STYLE_E(AntiAliasedLinesUseTex) ARC_STYLE_E(AntiAliasedFill) ARC_STYLE_E(HoverFlagsForTooltipMouse)
        ARC_STYLE_E(HoverFlagsForTooltipNav)
#undef ARC_STYLE_F
#undef ARC_STYLE_V
#undef ARC_STYLE_E
        if (std::memcmp(a.Colors, b.Colors, sizeof a.Colors) != 0) return "Colors";
        return {};
    }
}

TEST_CASE("sweep: style metrics are EditorUiStyleSettings fields with the pre-sweep values", "[sweep][ui-style]")
{
    const Editor::EditorUiStyleSettings u{};
    CHECK(u.frameBorderSize == 1.0f); CHECK_FALSE(u.dockNodeCloseButton); CHECK(u.tabOverlineSize == 2.0f);
    CHECK(Test::SameBits(u.disabledAlpha, 0.45f)); CHECK(u.tabRounding == 2.0f); CHECK(u.tableRowHeight == 24.0f);
    CHECK(u.assetRowThumbPx == 18.0f); CHECK(u.assetRefThumbPx == 20.0f); CHECK(Test::SameBits(u.propertyDragSpeed, 0.01f));
    CHECK(u.intStep == 1); CHECK(u.intStepFast == 100);
    CHECK(Editor::EditorUiSettings{}.altFontFamily == "Roboto");
    ImGuiStyle s;
    Editor::ApplyEditorTheme(s, u);
    CHECK(s.FrameBorderSize == 1.0f); CHECK(s.TabBarOverlineSize == 2.0f); CHECK(s.TabRounding == 2.0f);
    CHECK(Test::SameBits(s.DisabledAlpha, 0.45f)); CHECK_FALSE(s.DockingNodeHasCloseButton);
    CHECK(s.WindowPadding.x == 4.0f); CHECK(s.WindowPadding.y == 4.0f);   // the user's inset stays a constant
    // The one-argument form (tests; the crash reporter spells EditorUiStyleSettings{}) is the defaults.
    ImGuiStyle d;
    Editor::ApplyEditorTheme(d);
    CHECK(StyleDiff(d, s) == "");
    Test::RequireDefault("editor.ui.tabRounding", CVarValue::Float32(2.0f));
    Test::RequireDefault("editor.ui.frameBorderSize", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.ui.dockNodeCloseButton", CVarValue::Bool(false));
    Test::RequireDefault("editor.ui.tabOverlineSize", CVarValue::Float32(2.0f));
    Test::RequireDefault("editor.ui.disabledAlpha", CVarValue::Float32(0.45f));
    Test::RequireDefault("editor.ui.tableRowHeight", CVarValue::Float32(24.0f));
    Test::RequireDefault("editor.ui.assetRowThumbPx", CVarValue::Float32(18.0f));
    Test::RequireDefault("editor.ui.assetRefThumbPx", CVarValue::Float32(20.0f));
    Test::RequireDefault("editor.ui.propertyDragSpeed", CVarValue::Float32(0.01f));
    Test::RequireDefault("editor.ui.intStep", CVarValue::Int32(1));
    Test::RequireDefault("editor.ui.intStepFast", CVarValue::Int32(100));
    Test::RequireDefault("editor.ui.altFontFamily", CVarValue::String("Roboto"));
}

TEST_CASE("sweep: the style metrics come from the struct, not literals", "[sweep][ui-style]")
{
    Editor::EditorUiStyleSettings u;
    u.frameBorderSize = 0.0f; u.dockNodeCloseButton = true; u.tabOverlineSize = 3.0f; u.disabledAlpha = 0.6f; u.tabRounding = 5.0f;
    ImGuiStyle s;
    Editor::ApplyEditorTheme(s, u);
    CHECK(s.FrameBorderSize == 0.0f); CHECK(s.DockingNodeHasCloseButton); CHECK(s.TabBarOverlineSize == 3.0f);
    CHECK(s.DisabledAlpha == 0.6f); CHECK(s.TabRounding == 5.0f);
}

TEST_CASE("sweep: a Live style change re-applies the metrics once, at the current UI scale, keeping the colours", "[sweep][ui-style][settings-ui]")
{
    const Editor::Ui::ScopedMetrics restore(Editor::Ui::Metrics{});
    ImGuiStyle style; Editor::ApplyEditorTheme(style);
    const ImGuiStyle boot = style;
    Editor::AppearanceApplier applier; applier.Init(style);
    CHECK_FALSE(applier.UpdateStyle(Editor::EditorUiStyleSettings{}, style));   // the boot style IS the defaults
    CHECK(StyleDiff(style, boot) == "");

    Editor::EditorUiStyleSettings u; u.tabRounding = 6.0f; u.disabledAlpha = 0.7f; u.dockNodeCloseButton = true;
    CHECK(applier.UpdateStyle(u, style));
    CHECK(style.TabRounding == 6.0f); CHECK(style.DisabledAlpha == 0.7f); CHECK(style.DockingNodeHasCloseButton);
    CHECK(std::memcmp(style.Colors, boot.Colors, sizeof style.Colors) == 0);
    CHECK_FALSE(applier.UpdateStyle(u, style));   // unchanged: nothing re-applied

    Editor::EditorUiSettings ui; ui.scale = 1.5f;
    CHECK(applier.UpdateUi(ui, 1.0f, style));
    CHECK(style.TabRounding == ImTrunc(6.0f * 1.5f));   // the new base, scaled -- not the boot 2
    CHECK(style.DisabledAlpha == 0.7f);

    CHECK(applier.UpdateStyle(Editor::EditorUiStyleSettings{}, style));
    CHECK(style.TabRounding == ImTrunc(2.0f * 1.5f));
    ui.scale = 1.0f;
    CHECK(applier.UpdateUi(ui, 1.0f, style));
    CHECK(StyleDiff(style, boot) == "");   // back to the defaults at 1.0: the boot style, bit for bit
}

TEST_CASE("sweep: UiPx is the identity at scale 1 and scales linearly", "[sweep][ui-style]")
{
    const Editor::Ui::ScopedMetrics restore(Editor::Ui::Metrics{});
    EditorUserLayerReset reset;
    CHECK(Test::SameBits(Editor::UiPx(8.0f), 8.0f));
    // editor.ui.scale publishes Live; the frame's AppearanceApplier::UpdateUi
    // turns it into the one effective scale every pixel size reads.
    SetUi("editor.ui.scale", CVarValue::Float32(1.5f));
    ImGuiStyle style; Editor::ApplyEditorTheme(style);
    Editor::AppearanceApplier applier; applier.Init(style);
    applier.UpdateUi(Arcane::Settings<Editor::EditorUiSettings>(), 1.0f, style);
    CHECK(Editor::UiPx(8.0f) == 12.0f);
}

TEST_CASE("sweep: the density sizes are the setting at the UI scale and font size; identical at the defaults", "[sweep][ui-style]")
{
    EditorUserLayerReset reset;
    {
        const Editor::Ui::ScopedMetrics at1(Editor::Ui::Metrics{});
        CHECK(Test::SameBits(Editor::TableRowHeight(), 24.0f));
        CHECK(Test::SameBits(Editor::AssetRowThumbSize(), 18.0f));
        CHECK(Test::SameBits(Editor::PropertyDragSpeed(), 0.01f));
    }
    {
        const Editor::Ui::ScopedMetrics scaled(Editor::Ui::Metrics{ 1.5f, 16.0f });
        CHECK(Editor::TableRowHeight() == 36.0f);
        CHECK(Editor::AssetRowThumbSize() == 27.0f);
    }
    // editor.ui.assetRowThumbPx is a Dev row: compiled out of Dist, where the default stands.
    const bool thumbRow = Test::InThisBuild("editor.ui.assetRowThumbPx");
    SetUi("editor.ui.tableRowHeight", CVarValue::Float32(30.0f));
    if (thumbRow) SetUi("editor.ui.assetRowThumbPx", CVarValue::Float32(22.0f));
    SetUi("editor.ui.propertyDragSpeed", CVarValue::Float32(0.5f));
    const Editor::Ui::ScopedMetrics at1(Editor::Ui::Metrics{});
    CHECK(Editor::TableRowHeight() == 30.0f);
    CHECK(Editor::AssetRowThumbSize() == (thumbRow ? 22.0f : 18.0f));
    CHECK(Editor::PropertyDragSpeed() == 0.5f);
}

TEST_CASE("sweep: the alternate face resolves from editor.ui.altFontFamily", "[sweep][ui-style]")
{
    const std::filesystem::path exe("exe");
    const std::vector<Editor::EditorFontFamily> families = Editor::ListEditorFontFamilies(exe, {});
    const Editor::EditorFontRequest req = Editor::DefaultEditorFontRequest(exe);
    CHECK(req.altFace == Editor::ResolveEditorFontFamily(families, Editor::EditorUiSettings{}.altFontFamily, "Roboto"));
    CHECK(req.altFace.filename() == "Roboto-Regular.ttf");
}

// S6-45: the toolbar's brand cluster ratios (inventory "EditorPanels.cpp:768" / ":794").
TEST_CASE("sweep: editor.ui.toolbar.logoScale and brandScale are the pre-sweep ratios", "[sweep][ui-style]")
{
    const Editor::EditorUiToolbarSettings t{};
    CHECK(t.logoScale == 1.35f);
    CHECK(t.brandScale == 0.80f);
    Test::RequireDefault("editor.ui.toolbar.logoScale", CVarValue::Float32(1.35f));
    Test::RequireDefault("editor.ui.toolbar.brandScale", CVarValue::Float32(0.80f));
}
