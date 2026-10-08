#pragma once

// EditorThemeSettings (settings arc S4, spec s7.1): the editor theme as cvars,
// editor.theme.<token>, Preferences > Appearance > Theme, machine-wide.
// Stored LINEAR (CVarColor's contract; files carry sRGB hex); ToPalette turns
// a published block into the display-referred Theme::Palette the editor draws.
// A value equal to its default maps to the Dark constant BIT FOR BIT, so the
// default theme cannot drift by a float round trip (goldens).

#include "Widgets/EditorTheme.hpp"

#include <Arcane/Config/CVarTypes.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <Arcane/Core/Constant.hpp>

namespace Arcane::Editor
{
    enum class AssetKind : int;   // Panels/AssetPanelModel.hpp

    [[nodiscard]] float SrgbToLinear(float c) noexcept;
    [[nodiscard]] float LinearToSrgb(float c) noexcept;
    [[nodiscard]] Arcane::CVarColor ToSettingColor(const ImVec4& display) noexcept;   // display sRGB -> stored linear
    [[nodiscard]] ImVec4 ToDisplayColor(const Arcane::CVarColor& linear) noexcept;     // stored linear -> display sRGB

    struct EditorThemeSettings
    {
        Arcane::CVarColor chromeDeep    { 0.0036714235f, 0.0036714235f, 0.0036714235f, 1.0f };
        Arcane::CVarColor chrome        { 0.0097152395f, 0.0097152395f, 0.0097152395f, 1.0f };
        Arcane::CVarColor panel         { 0.0130468225f, 0.0130468225f, 0.0130468225f, 1.0f };
        Arcane::CVarColor panelRaised   { 0.0232278258f, 0.0232278258f, 0.0232278258f, 1.0f };
        Arcane::CVarColor well          { 0.0060965400f, 0.0060965400f, 0.0060965400f, 1.0f };
        Arcane::CVarColor wellHovered   { 0.0091167726f, 0.0091167726f, 0.0091167726f, 1.0f };
        Arcane::CVarColor wellActive    { 0.0116454307f, 0.0116454307f, 0.0116454307f, 1.0f };
        Arcane::CVarColor button        { 0.0283366870f, 0.0283366870f, 0.0283366870f, 1.0f };
        Arcane::CVarColor buttonHovered { 0.0465830229f, 0.0465830229f, 0.0465830229f, 1.0f };
        Arcane::CVarColor buttonActive  { 0.0703032017f, 0.0703032017f, 0.0703032017f, 1.0f };
        Arcane::CVarColor selection     { 0.0272117835f, 0.0512773395f, 0.0862332359f, 1.0f };
        Arcane::CVarColor accent        { 0.1047001705f, 0.2121946365f, 0.3813514709f, 1.0f };
        Arcane::CVarColor accentHovered { 0.1246129200f, 0.2379146814f, 0.4172953963f, 1.0f };
        Arcane::CVarColor accentActive  { 0.0846083686f, 0.1813783795f, 0.3327331543f, 1.0f };
        Arcane::CVarColor text          { 0.7445777655f, 0.7445777655f, 0.7445777655f, 1.0f };
        Arcane::CVarColor textDim       { 0.2706434131f, 0.2706434131f, 0.2706434131f, 1.0f };
        Arcane::CVarColor border        { 0.0040265042f, 0.0040265042f, 0.0040265042f, 1.0f };
        Arcane::CVarColor separator     { 0.0331047662f, 0.0331047662f, 0.0331047662f, 1.0f };
        Arcane::CVarColor separatorHot  { 0.0683848485f, 0.0683848485f, 0.0683848485f, 1.0f };
        Arcane::CVarColor separatorHeld { 0.1556399614f, 0.1556399614f, 0.1556399614f, 1.0f };
        Arcane::CVarColor grab          { 0.3232355118f, 0.3232355118f, 0.3232355118f, 1.0f };
        Arcane::CVarColor grabActive    { 0.5770624280f, 0.5770624280f, 0.5770624280f, 1.0f };
        Arcane::CVarColor check         { 0.6577108502f, 0.6577108502f, 0.6577108502f, 1.0f };
        Arcane::CVarColor amber         { 1.0f, 0.3800562918f, 0.0100228256f, 1.0f };
        Arcane::CVarColor amberLight    { 1.0f, 0.5704815388f, 0.1004815027f, 1.0f };
        Arcane::CVarColor error         { 0.7874122262f, 0.1004815027f, 0.1004815027f, 1.0f };
        Arcane::CVarColor warning       { 0.8900054097f, 0.5542217493f, 0.0732389614f, 1.0f };
        Arcane::CVarColor axisX         { 0.5520114303f, 0.0512694642f, 0.0368894525f, 1.0f };
        Arcane::CVarColor axisY         { 0.1169706732f, 0.3813260496f, 0.0423114114f, 1.0f };
        Arcane::CVarColor axisZ         { 0.0423114114f, 0.1946178377f, 0.5520114303f, 1.0f };
        Arcane::CVarColor modalDim      { 0.0015479876f, 0.0015479876f, 0.0015479876f, 0.55f };
        Arcane::CVarColor rowStripe     { 1.0f, 1.0f, 1.0f, 0.03f };
        // S6-26: the domain palettes. Written from the Dark palette's display
        // bytes, so the default IS kDarkPalette's value (ToPalette keeps the
        // constant itself while a value equals its default).
        Arcane::CVarColor actingOnFrame     = ToSettingColor(Theme::kDarkPalette.actingOnFrame);
        Arcane::CVarColor headerBand        = ToSettingColor(Theme::kDarkPalette.headerBand);
        Arcane::CVarColor headerBandHovered = ToSettingColor(Theme::kDarkPalette.headerBandHovered);
        Arcane::CVarColor headerBandActive  = ToSettingColor(Theme::kDarkPalette.headerBandActive);
        Arcane::CVarColor channelR          = ToSettingColor(Theme::kDarkPalette.channelR);
        Arcane::CVarColor channelG          = ToSettingColor(Theme::kDarkPalette.channelG);
        Arcane::CVarColor channelB          = ToSettingColor(Theme::kDarkPalette.channelB);
        Arcane::CVarColor channelW          = ToSettingColor(Theme::kDarkPalette.channelW);
        // Not a colour, so not a ThemeToken: ApplyEditorThemeColors' argument.
        float unfocusedOverlineAlpha = Theme::kDarkUnfocusedOverlineAlpha;
    };

    struct ThemeToken
    {
        std::string_view field;         // "chromeDeep": the cvar is editor.theme.chromeDeep
        std::string_view displayName;   // the theme page's label
        std::string_view group;         // the theme page's sub-header
        Arcane::CVarColor EditorThemeSettings::* setting;
        ImVec4 Theme::Palette::* palette;
    };

    // Field order == Palette order == EditorThemeSettings order.
    inline constexpr std::array<ThemeToken, 40> kThemeTokens = { {
        { "chromeDeep",    "Chrome (deep)",       "Chrome",              &EditorThemeSettings::chromeDeep,    &Theme::Palette::chromeDeep },
        { "chrome",        "Chrome",              "Chrome",              &EditorThemeSettings::chrome,        &Theme::Palette::chrome },
        { "panel",         "Panel",               "Panels",              &EditorThemeSettings::panel,         &Theme::Palette::panel },
        { "panelRaised",   "Panel (raised)",      "Panels",              &EditorThemeSettings::panelRaised,   &Theme::Palette::panelRaised },
        { "well",          "Field",               "Fields",              &EditorThemeSettings::well,          &Theme::Palette::well },
        { "wellHovered",   "Field (hovered)",     "Fields",              &EditorThemeSettings::wellHovered,   &Theme::Palette::wellHovered },
        { "wellActive",    "Field (active)",      "Fields",              &EditorThemeSettings::wellActive,    &Theme::Palette::wellActive },
        { "button",        "Button",              "Buttons",             &EditorThemeSettings::button,        &Theme::Palette::button },
        { "buttonHovered", "Button (hovered)",    "Buttons",             &EditorThemeSettings::buttonHovered, &Theme::Palette::buttonHovered },
        { "buttonActive",  "Button (pressed)",    "Buttons",             &EditorThemeSettings::buttonActive,  &Theme::Palette::buttonActive },
        { "selection",     "Selection",           "Selection and accent",&EditorThemeSettings::selection,     &Theme::Palette::selection },
        { "accent",        "Accent",              "Selection and accent",&EditorThemeSettings::accent,        &Theme::Palette::accent },
        { "accentHovered", "Accent (hovered)",    "Selection and accent",&EditorThemeSettings::accentHovered, &Theme::Palette::accentHovered },
        { "accentActive",  "Accent (pressed)",    "Selection and accent",&EditorThemeSettings::accentActive,  &Theme::Palette::accentActive },
        { "text",          "Text",                "Text and lines",      &EditorThemeSettings::text,          &Theme::Palette::text },
        { "textDim",       "Text (dim)",          "Text and lines",      &EditorThemeSettings::textDim,       &Theme::Palette::textDim },
        { "border",        "Border",              "Text and lines",      &EditorThemeSettings::border,        &Theme::Palette::border },
        { "separator",     "Separator",           "Text and lines",      &EditorThemeSettings::separator,     &Theme::Palette::separator },
        { "separatorHot",  "Separator (hovered)", "Text and lines",      &EditorThemeSettings::separatorHot,  &Theme::Palette::separatorHot },
        { "separatorHeld", "Separator (dragged)", "Text and lines",      &EditorThemeSettings::separatorHeld, &Theme::Palette::separatorHeld },
        { "grab",          "Grab",                "Marks",               &EditorThemeSettings::grab,          &Theme::Palette::grab },
        { "grabActive",    "Grab (dragged)",      "Marks",               &EditorThemeSettings::grabActive,    &Theme::Palette::grabActive },
        { "check",         "Check mark",          "Marks",               &EditorThemeSettings::check,         &Theme::Palette::check },
        { "amber",         "Amber",               "Status",              &EditorThemeSettings::amber,         &Theme::Palette::amber },
        { "amberLight",    "Amber (light)",       "Status",              &EditorThemeSettings::amberLight,    &Theme::Palette::amberLight },
        { "error",         "Error",               "Status",              &EditorThemeSettings::error,         &Theme::Palette::error },
        { "warning",       "Warning",             "Status",              &EditorThemeSettings::warning,       &Theme::Palette::warning },
        { "axisX",         "X axis",              "Axes",                &EditorThemeSettings::axisX,         &Theme::Palette::axisX },
        { "axisY",         "Y axis",              "Axes",                &EditorThemeSettings::axisY,         &Theme::Palette::axisY },
        { "axisZ",         "Z axis",              "Axes",                &EditorThemeSettings::axisZ,         &Theme::Palette::axisZ },
        { "modalDim",      "Modal dim",           "Overlays",            &EditorThemeSettings::modalDim,      &Theme::Palette::modalDim },
        { "rowStripe",     "Row stripe",          "Overlays",            &EditorThemeSettings::rowStripe,     &Theme::Palette::rowStripe },
        { "actingOnFrame", "Acting-on frame",     "Overlays",            &EditorThemeSettings::actingOnFrame, &Theme::Palette::actingOnFrame },
        { "headerBand",        "Header band",           "Header bands", &EditorThemeSettings::headerBand,        &Theme::Palette::headerBand },
        { "headerBandHovered", "Header band (hovered)", "Header bands", &EditorThemeSettings::headerBandHovered, &Theme::Palette::headerBandHovered },
        { "headerBandActive",  "Header band (pressed)", "Header bands", &EditorThemeSettings::headerBandActive,  &Theme::Palette::headerBandActive },
        { "channelR",      "Red channel",         "Channel markers",     &EditorThemeSettings::channelR,      &Theme::Palette::channelR },
        { "channelG",      "Green channel",       "Channel markers",     &EditorThemeSettings::channelG,      &Theme::Palette::channelG },
        { "channelB",      "Blue channel",        "Channel markers",     &EditorThemeSettings::channelB,      &Theme::Palette::channelB },
        { "channelW",      "Alpha channel",       "Channel markers",     &EditorThemeSettings::channelW,      &Theme::Palette::channelW },
    } };

    // A text/background pair the theme page measures (Theme::ContrastRatio,
    // WCAG 2.x): 4.5:1 for text, 3:1 for icons, marks and large shapes (s7.1).
    struct ThemeContrastPair
    {
        std::string_view label;
        ImVec4 Theme::Palette::* fg;
        ImVec4 Theme::Palette::* bg;
        float minRatio;
    };

    ARC_CONSTANT("a change would be a bug: the WCAG 2.x contrast minima (4.5:1 text, 3:1 marks) the theme page checks against")
    inline constexpr std::array<ThemeContrastPair, 16> kThemeContrastPairs = { {
        { "Text on panel",          &Theme::Palette::text,    &Theme::Palette::panel,       4.5f },
        { "Text on chrome",         &Theme::Palette::text,    &Theme::Palette::chrome,      4.5f },
        { "Text on field",          &Theme::Palette::text,    &Theme::Palette::well,        4.5f },
        { "Text on button",         &Theme::Palette::text,    &Theme::Palette::button,      4.5f },
        { "Text on selection",      &Theme::Palette::text,    &Theme::Palette::selection,   4.5f },
        { "Text on hovered row",    &Theme::Palette::text,    &Theme::Palette::panelRaised, 4.5f },
        { "Dim text on panel",      &Theme::Palette::textDim, &Theme::Palette::panel,       4.5f },
        { "Dim text on chrome",     &Theme::Palette::textDim, &Theme::Palette::chrome,      4.5f },
        { "Dim text on field",      &Theme::Palette::textDim, &Theme::Palette::well,        4.5f },
        { "Error on panel",         &Theme::Palette::error,   &Theme::Palette::panel,       4.5f },
        { "Warning on panel",       &Theme::Palette::warning, &Theme::Palette::panel,       4.5f },
        { "Check mark on field",    &Theme::Palette::check,   &Theme::Palette::well,        3.0f },
        { "Accent on chrome",       &Theme::Palette::accent,  &Theme::Palette::chrome,      3.0f },
        { "Accent on button",       &Theme::Palette::accent,  &Theme::Palette::button,      3.0f },
        { "Icon on lit toggle",     &Theme::Palette::text,    &Theme::Palette::accent,      3.0f },
        { "Grab on field",          &Theme::Palette::grab,    &Theme::Palette::well,        3.0f },
    } };

    [[nodiscard]] Theme::Palette ToPalette(const EditorThemeSettings& s);
    [[nodiscard]] bool SameThemeSettings(const EditorThemeSettings& a, const EditorThemeSettings& b) noexcept;
    [[nodiscard]] std::string ThemeCvarName(std::string_view field);                   // "editor.theme." + field

    // ---- The domain palettes outside the Palette (settings S6-26) ----------
    // Category editor.theme.<domain>: a cvar each, Preferences > Appearance >
    // Domain colours. Not theme-preset tokens: like the axes they are data
    // hues, so a preset leaves them as they are. Each kX constant is today's
    // DISPLAY value and the struct default is written from it.
    ARC_CONSTANT("a change would be a bug: the one spelling of this cvar's default (editor.theme.inputPill.*)")
    inline constexpr ImVec4 kInputPillBlueBorder   = ImVec4(0x3a / 255.0f, 0x4a / 255.0f, 0x5c / 255.0f, 1.0f);
    ARC_CONSTANT("a change would be a bug: the one spelling of this cvar's default (editor.theme.inputPill.*)")
    inline constexpr ImVec4 kInputPillBlueText     = ImVec4(0x9f / 255.0f, 0xb3 / 255.0f, 0xc8 / 255.0f, 1.0f);
    ARC_CONSTANT("a change would be a bug: the one spelling of this cvar's default (editor.theme.inputPill.*)")
    inline constexpr ImVec4 kInputPillVioletBorder = ImVec4(0x4a / 255.0f, 0x3a / 255.0f, 0x5c / 255.0f, 1.0f);
    ARC_CONSTANT("a change would be a bug: the one spelling of this cvar's default (editor.theme.inputPill.*)")
    inline constexpr ImVec4 kInputPillVioletText   = ImVec4(0xb8 / 255.0f, 0xa3 / 255.0f, 0xc8 / 255.0f, 1.0f);
    // The viewport's camera-bounds frame: a thin desaturated line legible over
    // bright and dark scene content (drawn by the scene batcher).
    ARC_CONSTANT("a change would be a bug: the one spelling of this cvar's default (editor.theme.viewport.cameraFrame)")
    inline constexpr ImVec4 kCameraFrameColor      = ImVec4(0.45f, 0.62f, 0.78f, 0.75f);

    // The input editor's per-scheme binding pills (input editor spec s2.3):
    // blue-grey = the KeyboardMouse scheme, violet-grey = every other scheme.
    struct EditorThemeInputPillSettings
    {
        Arcane::CVarColor blueBorder   = ToSettingColor(kInputPillBlueBorder);
        Arcane::CVarColor blueText     = ToSettingColor(kInputPillBlueText);
        Arcane::CVarColor violetBorder = ToSettingColor(kInputPillVioletBorder);
        Arcane::CVarColor violetText   = ToSettingColor(kInputPillVioletText);
    };

    // The asset graph's per-kind node accents (asset-manager spec s11.3); the
    // defaults are KindAccentRgb(kind)'s table.
    struct EditorThemeAssetKindSettings
    {
        Arcane::CVarColor texture;
        Arcane::CVarColor material;
        Arcane::CVarColor mesh;
        Arcane::CVarColor sprite;
        Arcane::CVarColor scene;
        Arcane::CVarColor inputActions;
        Arcane::CVarColor model;
        EditorThemeAssetKindSettings();
    };

    // editor.theme.viewport.* (the frozen name editor.theme.viewport.cameraFrame).
    struct EditorThemeViewportSettings
    {
        Arcane::CVarColor cameraFrame = ToSettingColor(kCameraFrameColor);
    };

    // A domain colour to draw: `legacy` itself while the setting equals its
    // default (bit for bit, so the default never passes through pow()), else
    // the setting's display value.
    [[nodiscard]] ImVec4 ResolveDomainColor(const Arcane::CVarColor& value, const Arcane::CVarColor& defaultValue,
                                            const ImVec4& legacy) noexcept;

    // The asset kind's accent as 0xRRGGBB under `s`; 0 = the kind has no row
    // (the caller falls back to the theme's grab gray). A user colour that
    // encodes to pure black also reads as 0 and therefore falls back.
    [[nodiscard]] std::uint32_t KindAccentRgb(AssetKind kind, const EditorThemeAssetKindSettings& s) noexcept;
}
