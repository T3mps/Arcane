#include "Settings/EditorThemeSettings.hpp"

#include "Panels/AssetPanelModel.hpp"

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
        ARC_REFLECT_FIELD(EditorThemeSettings, actingOnFrame) ARC_REFLECT_ATTR(DisplayName, "Acting-on frame") ARC_REFLECT_ATTR(Category, "Overlays") ARC_REFLECT_ATTR(Tooltip, "The muted-amber border of an attention pill and of the asset status card you are acting on.")
        ARC_REFLECT_FIELD(EditorThemeSettings, headerBand) ARC_REFLECT_ATTR(DisplayName, "Header band") ARC_REFLECT_ATTR(Category, "Header bands") ARC_REFLECT_ATTR(Tooltip, "The inspector's component header band.")
        ARC_REFLECT_FIELD(EditorThemeSettings, headerBandHovered) ARC_REFLECT_ATTR(DisplayName, "Header band (hovered)") ARC_REFLECT_ATTR(Category, "Header bands") ARC_REFLECT_ATTR(Tooltip, "A component header under the cursor.")
        ARC_REFLECT_FIELD(EditorThemeSettings, headerBandActive) ARC_REFLECT_ATTR(DisplayName, "Header band (pressed)") ARC_REFLECT_ATTR(Category, "Header bands") ARC_REFLECT_ATTR(Tooltip, "A component header being pressed.")
        ARC_REFLECT_FIELD(EditorThemeSettings, channelR) ARC_REFLECT_ATTR(DisplayName, "Red channel") ARC_REFLECT_ATTR(Category, "Channel markers") ARC_REFLECT_ATTR(Tooltip, "The colour picker's marker beside the red (X) channel field.")
        ARC_REFLECT_FIELD(EditorThemeSettings, channelG) ARC_REFLECT_ATTR(DisplayName, "Green channel") ARC_REFLECT_ATTR(Category, "Channel markers") ARC_REFLECT_ATTR(Tooltip, "The colour picker's marker beside the green (Y) channel field.")
        ARC_REFLECT_FIELD(EditorThemeSettings, channelB) ARC_REFLECT_ATTR(DisplayName, "Blue channel") ARC_REFLECT_ATTR(Category, "Channel markers") ARC_REFLECT_ATTR(Tooltip, "The colour picker's marker beside the blue (Z) channel field.")
        ARC_REFLECT_FIELD(EditorThemeSettings, channelW) ARC_REFLECT_ATTR(DisplayName, "Alpha channel") ARC_REFLECT_ATTR(Category, "Channel markers") ARC_REFLECT_ATTR(Tooltip, "The colour picker's marker beside the alpha (W) channel field.")
        ARC_REFLECT_FIELD(EditorThemeSettings, unfocusedOverlineAlpha) ARC_REFLECT_ATTR(DisplayName, "Unfocused tab overline opacity") ARC_REFLECT_ATTR(Category, "Overlays") ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Tooltip, "The opacity of the accent overline on the selected tab of a dock that does not have focus.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorThemeSettings);

    ARC_REFLECT_TYPE(EditorThemeInputPillSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme.inputPill", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Domain colours")
        ARC_REFLECT_FIELD(EditorThemeInputPillSettings, blueBorder)   ARC_REFLECT_ATTR(DisplayName, "Keyboard/mouse pill border") ARC_REFLECT_ATTR(Category, "Input pills") ARC_REFLECT_ATTR(Tooltip, "The border of a KeyboardMouse-scheme binding pill in the input editor.")
        ARC_REFLECT_FIELD(EditorThemeInputPillSettings, blueText)     ARC_REFLECT_ATTR(DisplayName, "Keyboard/mouse pill text")   ARC_REFLECT_ATTR(Category, "Input pills") ARC_REFLECT_ATTR(Tooltip, "The text of a KeyboardMouse-scheme binding pill in the input editor.")
        ARC_REFLECT_FIELD(EditorThemeInputPillSettings, violetBorder) ARC_REFLECT_ATTR(DisplayName, "Other scheme pill border")   ARC_REFLECT_ATTR(Category, "Input pills") ARC_REFLECT_ATTR(Tooltip, "The border of a binding pill of every other control scheme in the input editor.")
        ARC_REFLECT_FIELD(EditorThemeInputPillSettings, violetText)   ARC_REFLECT_ATTR(DisplayName, "Other scheme pill text")     ARC_REFLECT_ATTR(Category, "Input pills") ARC_REFLECT_ATTR(Tooltip, "The text of a binding pill of every other control scheme in the input editor.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorThemeInputPillSettings);

    ARC_REFLECT_TYPE(EditorThemeAssetKindSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme.assetKind", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Domain colours")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, texture)      ARC_REFLECT_ATTR(DisplayName, "Texture")       ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of a texture node in the asset graph.")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, material)     ARC_REFLECT_ATTR(DisplayName, "Material")      ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of a material node in the asset graph.")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, mesh)         ARC_REFLECT_ATTR(DisplayName, "Mesh")          ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of a mesh node in the asset graph.")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, sprite)       ARC_REFLECT_ATTR(DisplayName, "Sprite")        ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of a sprite node in the asset graph.")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, scene)        ARC_REFLECT_ATTR(DisplayName, "Scene")         ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of a scene node in the asset graph.")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, inputActions) ARC_REFLECT_ATTR(DisplayName, "Input actions") ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of an input-actions node in the asset graph.")
        ARC_REFLECT_FIELD(EditorThemeAssetKindSettings, model)        ARC_REFLECT_ATTR(DisplayName, "Model")         ARC_REFLECT_ATTR(Category, "Asset kinds") ARC_REFLECT_ATTR(Tooltip, "The accent of a model node in the asset graph.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorThemeAssetKindSettings);

    ARC_REFLECT_TYPE(EditorThemeViewportSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme.viewport", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Domain colours")
        ARC_REFLECT_FIELD(EditorThemeViewportSettings, cameraFrame) ARC_REFLECT_ATTR(DisplayName, "Camera frame") ARC_REFLECT_ATTR(Category, "Viewport") ARC_REFLECT_ATTR(Tooltip, "The thin line the 2D viewport draws around the scene camera's view bounds.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorThemeViewportSettings);

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
        static_assert(sizeof(EditorThemeSettings) == kThemeTokens.size() * sizeof(Arcane::CVarColor) + sizeof(float),
                      "EditorThemeSettings is exactly its colours + unfocusedOverlineAlpha: memcmp compares every field and no padding");
        return std::memcmp(&a, &b, sizeof(EditorThemeSettings)) == 0;
    }

    std::string ThemeCvarName(std::string_view field) { return "editor.theme." + std::string(field); }

    namespace
    {
        ImVec4 DisplayOfRgb(std::uint32_t rgb) noexcept
        {
            return ImVec4(static_cast<float>((rgb >> 16) & 0xffu) / 255.0f, static_cast<float>((rgb >> 8) & 0xffu) / 255.0f,
                          static_cast<float>(rgb & 0xffu) / 255.0f, 1.0f);
        }

        // kind -> its field, or null for a kind with no row.
        Arcane::CVarColor EditorThemeAssetKindSettings::* AssetKindField(AssetKind kind) noexcept
        {
            switch (kind)
            {
                case AssetKind::Texture:      return &EditorThemeAssetKindSettings::texture;
                case AssetKind::Material:     return &EditorThemeAssetKindSettings::material;
                case AssetKind::Mesh:         return &EditorThemeAssetKindSettings::mesh;
                case AssetKind::Sprite:       return &EditorThemeAssetKindSettings::sprite;
                case AssetKind::Scene:        return &EditorThemeAssetKindSettings::scene;
                case AssetKind::InputActions: return &EditorThemeAssetKindSettings::inputActions;
                case AssetKind::Model:        return &EditorThemeAssetKindSettings::model;
                default:                      return nullptr;
            }
        }
    }

    // The defaults are KindAccentRgb(kind)'s table, so that table stays the
    // one spelling of the asset-manager spec's hexes.
    EditorThemeAssetKindSettings::EditorThemeAssetKindSettings()
        : texture(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::Texture))))
        , material(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::Material))))
        , mesh(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::Mesh))))
        , sprite(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::Sprite))))
        , scene(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::Scene))))
        , inputActions(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::InputActions))))
        , model(ToSettingColor(DisplayOfRgb(KindAccentRgb(AssetKind::Model))))
    {
    }

    ImVec4 ResolveDomainColor(const Arcane::CVarColor& value, const Arcane::CVarColor& defaultValue, const ImVec4& legacy) noexcept
    {
        return std::memcmp(&value, &defaultValue, sizeof(Arcane::CVarColor)) == 0 ? legacy : ToDisplayColor(value);
    }

    std::uint32_t KindAccentRgb(AssetKind kind, const EditorThemeAssetKindSettings& s) noexcept
    {
        const auto field = AssetKindField(kind);
        if (field == nullptr)
            return 0;
        static const EditorThemeAssetKindSettings kDefaults{};
        const ImU32 u = ImGui::ColorConvertFloat4ToU32(ResolveDomainColor(s.*field, kDefaults.*field, DisplayOfRgb(KindAccentRgb(kind))));
        return (((u >> IM_COL32_R_SHIFT) & 0xffu) << 16) | (((u >> IM_COL32_G_SHIFT) & 0xffu) << 8) | ((u >> IM_COL32_B_SHIFT) & 0xffu);
    }
}
