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
#include <string>
#include <string_view>

namespace Arcane::Editor
{
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
    inline constexpr std::array<ThemeToken, 32> kThemeTokens = { {
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

    [[nodiscard]] float SrgbToLinear(float c) noexcept;
    [[nodiscard]] float LinearToSrgb(float c) noexcept;
    [[nodiscard]] Arcane::CVarColor ToSettingColor(const ImVec4& display) noexcept;   // display sRGB -> stored linear
    [[nodiscard]] ImVec4 ToDisplayColor(const Arcane::CVarColor& linear) noexcept;     // stored linear -> display sRGB
    [[nodiscard]] Theme::Palette ToPalette(const EditorThemeSettings& s);
    [[nodiscard]] bool SameThemeSettings(const EditorThemeSettings& a, const EditorThemeSettings& b) noexcept;
    [[nodiscard]] std::string ThemeCvarName(std::string_view field);                   // "editor.theme." + field
}
