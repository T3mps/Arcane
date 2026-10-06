#include "Settings/EditorThemeSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <algorithm>
#include <cstring>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorThemeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Theme")
        ARC_REFLECT_FIELD(EditorThemeSettings, chromeDeep)    ARC_REFLECT_ATTR(DisplayName, "Chrome (deep)")       ARC_REFLECT_ATTR(Category, "Chrome")               ARC_REFLECT_ATTR(Tooltip, "Title bars and the scrollbar track: the darkest chrome tone.")
        ARC_REFLECT_FIELD(EditorThemeSettings, chrome)        ARC_REFLECT_ATTR(DisplayName, "Chrome")              ARC_REFLECT_ATTR(Category, "Chrome")               ARC_REFLECT_ATTR(Tooltip, "The menu bar, popups and table headers.")
        ARC_REFLECT_FIELD(EditorThemeSettings, panel)         ARC_REFLECT_ATTR(DisplayName, "Panel")               ARC_REFLECT_ATTR(Category, "Panels")               ARC_REFLECT_ATTR(Tooltip, "The base surface of every panel and of the selected tab.")
        ARC_REFLECT_FIELD(EditorThemeSettings, panelRaised)   ARC_REFLECT_ATTR(DisplayName, "Panel (raised)")      ARC_REFLECT_ATTR(Category, "Panels")               ARC_REFLECT_ATTR(Tooltip, "Row and tab hover: one step up from the panel.")
        ARC_REFLECT_FIELD(EditorThemeSettings, well)          ARC_REFLECT_ATTR(DisplayName, "Field")               ARC_REFLECT_ATTR(Category, "Fields")               ARC_REFLECT_ATTR(Tooltip, "Input fields (drags, text boxes, combos): the inset well.")
        ARC_REFLECT_FIELD(EditorThemeSettings, wellHovered)   ARC_REFLECT_ATTR(DisplayName, "Field (hovered)")     ARC_REFLECT_ATTR(Category, "Fields")               ARC_REFLECT_ATTR(Tooltip, "An input field under the cursor.")
        ARC_REFLECT_FIELD(EditorThemeSettings, wellActive)    ARC_REFLECT_ATTR(DisplayName, "Field (active)")      ARC_REFLECT_ATTR(Category, "Fields")               ARC_REFLECT_ATTR(Tooltip, "An input field being edited, and a checked box.")
        ARC_REFLECT_FIELD(EditorThemeSettings, button)        ARC_REFLECT_ATTR(DisplayName, "Button")              ARC_REFLECT_ATTR(Category, "Buttons")              ARC_REFLECT_ATTR(Tooltip, "Buttons and scrollbar grabs.")
        ARC_REFLECT_FIELD(EditorThemeSettings, buttonHovered) ARC_REFLECT_ATTR(DisplayName, "Button (hovered)")    ARC_REFLECT_ATTR(Category, "Buttons")              ARC_REFLECT_ATTR(Tooltip, "A button under the cursor.")
        ARC_REFLECT_FIELD(EditorThemeSettings, buttonActive)  ARC_REFLECT_ATTR(DisplayName, "Button (pressed)")    ARC_REFLECT_ATTR(Category, "Buttons")              ARC_REFLECT_ATTR(Tooltip, "A button being pressed.")
        ARC_REFLECT_FIELD(EditorThemeSettings, selection)     ARC_REFLECT_ATTR(DisplayName, "Selection")           ARC_REFLECT_ATTR(Category, "Selection and accent") ARC_REFLECT_ATTR(Tooltip, "Selected rows, selected text and the docking preview.")
        ARC_REFLECT_FIELD(EditorThemeSettings, accent)        ARC_REFLECT_ATTR(DisplayName, "Accent")              ARC_REFLECT_ATTR(Category, "Selection and accent") ARC_REFLECT_ATTR(Tooltip, "On, active and playing: lit toggles, the selected tab's overline, Play presence.")
        ARC_REFLECT_FIELD(EditorThemeSettings, accentHovered) ARC_REFLECT_ATTR(DisplayName, "Accent (hovered)")    ARC_REFLECT_ATTR(Category, "Selection and accent") ARC_REFLECT_ATTR(Tooltip, "A lit toggle under the cursor.")
        ARC_REFLECT_FIELD(EditorThemeSettings, accentActive)  ARC_REFLECT_ATTR(DisplayName, "Accent (pressed)")    ARC_REFLECT_ATTR(Category, "Selection and accent") ARC_REFLECT_ATTR(Tooltip, "A lit toggle being pressed.")
        ARC_REFLECT_FIELD(EditorThemeSettings, text)          ARC_REFLECT_ATTR(DisplayName, "Text")                ARC_REFLECT_ATTR(Category, "Text and lines")       ARC_REFLECT_ATTR(Tooltip, "Body text.")
        ARC_REFLECT_FIELD(EditorThemeSettings, textDim)       ARC_REFLECT_ATTR(DisplayName, "Text (dim)")          ARC_REFLECT_ATTR(Category, "Text and lines")       ARC_REFLECT_ATTR(Tooltip, "Secondary and disabled text.")
        ARC_REFLECT_FIELD(EditorThemeSettings, border)        ARC_REFLECT_ATTR(DisplayName, "Border")              ARC_REFLECT_ATTR(Category, "Text and lines")       ARC_REFLECT_ATTR(Tooltip, "The 1 px edge around field wells and windows.")
        ARC_REFLECT_FIELD(EditorThemeSettings, separator)     ARC_REFLECT_ATTR(DisplayName, "Separator")           ARC_REFLECT_ATTR(Category, "Text and lines")       ARC_REFLECT_ATTR(Tooltip, "Separators, table lines and the dock splitter.")
        ARC_REFLECT_FIELD(EditorThemeSettings, separatorHot)  ARC_REFLECT_ATTR(DisplayName, "Separator (hovered)") ARC_REFLECT_ATTR(Category, "Text and lines")       ARC_REFLECT_ATTR(Tooltip, "A splitter under the cursor.")
        ARC_REFLECT_FIELD(EditorThemeSettings, separatorHeld) ARC_REFLECT_ATTR(DisplayName, "Separator (dragged)") ARC_REFLECT_ATTR(Category, "Text and lines")       ARC_REFLECT_ATTR(Tooltip, "A splitter being dragged.")
        ARC_REFLECT_FIELD(EditorThemeSettings, grab)          ARC_REFLECT_ATTR(DisplayName, "Grab")                ARC_REFLECT_ATTR(Category, "Marks")                ARC_REFLECT_ATTR(Tooltip, "Slider grabs, resize grips and plot lines.")
        ARC_REFLECT_FIELD(EditorThemeSettings, grabActive)    ARC_REFLECT_ATTR(DisplayName, "Grab (dragged)")      ARC_REFLECT_ATTR(Category, "Marks")                ARC_REFLECT_ATTR(Tooltip, "A slider grab being dragged.")
        ARC_REFLECT_FIELD(EditorThemeSettings, check)         ARC_REFLECT_ATTR(DisplayName, "Check mark")          ARC_REFLECT_ATTR(Category, "Marks")                ARC_REFLECT_ATTR(Tooltip, "Check marks, radio dots and link text.")
        ARC_REFLECT_FIELD(EditorThemeSettings, amber)         ARC_REFLECT_ATTR(DisplayName, "Amber")               ARC_REFLECT_ATTR(Category, "Status")               ARC_REFLECT_ATTR(Tooltip, "The thing you are acting on: drop targets, histogram bars and the viewport selection outline.")
        ARC_REFLECT_FIELD(EditorThemeSettings, amberLight)    ARC_REFLECT_ATTR(DisplayName, "Amber (light)")       ARC_REFLECT_ATTR(Category, "Status")               ARC_REFLECT_ATTR(Tooltip, "Hovered histogram bars.")
        ARC_REFLECT_FIELD(EditorThemeSettings, error)         ARC_REFLECT_ATTR(DisplayName, "Error")               ARC_REFLECT_ATTR(Category, "Status")               ARC_REFLECT_ATTR(Tooltip, "Errors and refused values.")
        ARC_REFLECT_FIELD(EditorThemeSettings, warning)       ARC_REFLECT_ATTR(DisplayName, "Warning")             ARC_REFLECT_ATTR(Category, "Status")               ARC_REFLECT_ATTR(Tooltip, "Warnings.")
        ARC_REFLECT_FIELD(EditorThemeSettings, axisX)         ARC_REFLECT_ATTR(DisplayName, "X axis")              ARC_REFLECT_ATTR(Category, "Axes")                 ARC_REFLECT_ATTR(Tooltip, "The X axis colour: the inspector bars, the viewport grids and the gizmo all derive from it.")
        ARC_REFLECT_FIELD(EditorThemeSettings, axisY)         ARC_REFLECT_ATTR(DisplayName, "Y axis")              ARC_REFLECT_ATTR(Category, "Axes")                 ARC_REFLECT_ATTR(Tooltip, "The Y axis colour: the inspector bars, the viewport grids and the gizmo all derive from it.")
        ARC_REFLECT_FIELD(EditorThemeSettings, axisZ)         ARC_REFLECT_ATTR(DisplayName, "Z axis")              ARC_REFLECT_ATTR(Category, "Axes")                 ARC_REFLECT_ATTR(Tooltip, "The Z axis colour: the inspector bars, the 3D grid and the gizmo all derive from it.")
        ARC_REFLECT_FIELD(EditorThemeSettings, modalDim)      ARC_REFLECT_ATTR(DisplayName, "Modal dim")           ARC_REFLECT_ATTR(Category, "Overlays")             ARC_REFLECT_ATTR(Tooltip, "The wash behind a modal dialog.")
        ARC_REFLECT_FIELD(EditorThemeSettings, rowStripe)     ARC_REFLECT_ATTR(DisplayName, "Row stripe")          ARC_REFLECT_ATTR(Category, "Overlays")             ARC_REFLECT_ATTR(Tooltip, "The alternate table row stripe, a wash over the row.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorThemeSettings);

    // SrgbToLinear / LinearToSrgb live in Widgets/EditorWidgets.cpp (LNK2005
    // if this TU defines them too; ArcaneTests source-compiles both).

    Arcane::CVarColor ToSettingColor(const ImVec4& d) noexcept
    {
        return Arcane::CVarColor{ SrgbToLinear(d.x), SrgbToLinear(d.y), SrgbToLinear(d.z), std::clamp(d.w, 0.0f, 1.0f) };
    }

    ImVec4 ToDisplayColor(const Arcane::CVarColor& c) noexcept
    {
        return ImVec4(LinearToSrgb(c.r), LinearToSrgb(c.g), LinearToSrgb(c.b), std::clamp(c.a, 0.0f, 1.0f));
    }

    Theme::Palette ToPalette(const EditorThemeSettings& s)
    {
        static const EditorThemeSettings kDefaults{};
        Theme::Palette p = Theme::kDarkPalette;
        for (const ThemeToken& t : kThemeTokens)
        {
            const Arcane::CVarColor& v = s.*(t.setting);
            const Arcane::CVarColor& d = kDefaults.*(t.setting);
            // Equal to the default -> the Dark constant itself, so the default
            // theme never passes through pow() (byte-identical goldens).
            if (std::memcmp(&v, &d, sizeof(Arcane::CVarColor)) != 0)
                p.*(t.palette) = ToDisplayColor(v);
        }
        return p;
    }

    bool SameThemeSettings(const EditorThemeSettings& a, const EditorThemeSettings& b) noexcept
    {
        static_assert(sizeof(EditorThemeSettings) == kThemeTokens.size() * sizeof(Arcane::CVarColor),
                      "EditorThemeSettings is exactly its colours: memcmp compares every field and no padding");
        return std::memcmp(&a, &b, sizeof(EditorThemeSettings)) == 0;
    }

    std::string ThemeCvarName(std::string_view field) { return "editor.theme." + std::string(field); }
}
