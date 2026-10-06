#include "Settings/EditorUiSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <algorithm>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorUiSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.ui", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Fonts and Scale")   // FontsPage.hpp kFontsPageCategory
        ARC_REFLECT_FIELD(EditorUiSettings, fontFamily)
            ARC_REFLECT_ATTR(DisplayName, "UI font") ARC_REFLECT_ATTR(Widget, "font")
            ARC_REFLECT_ATTR(Tooltip, "The editor's text face: a bundled family or any .ttf/.otf in your Fonts folder. Rebuilds the font atlas at the next frame.")
        ARC_REFLECT_FIELD(EditorUiSettings, monoFontFamily)
            ARC_REFLECT_ATTR(DisplayName, "Monospace font") ARC_REFLECT_ATTR(Widget, "font")
            ARC_REFLECT_ATTR(Tooltip, "The face for code, paths and log rows.")
        ARC_REFLECT_FIELD(EditorUiSettings, altFontFamily)
            ARC_REFLECT_ATTR(DisplayName, "Alternate font") ARC_REFLECT_ATTR(Widget, "font")
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "The secondary face loaded beside the UI font, for the few places that push a contrasting face.")
        ARC_REFLECT_FIELD(EditorUiSettings, fontSize)
            ARC_REFLECT_ATTR(DisplayName, "Font size") ARC_REFLECT_ATTR(Range, 10.0f, 32.0f)
            ARC_REFLECT_ATTR(Tooltip, "UI text size in pixels at scale 1.0. Text-relative sizes follow it.")
        ARC_REFLECT_FIELD(EditorUiSettings, scale)
            ARC_REFLECT_ATTR(DisplayName, "UI scale") ARC_REFLECT_ATTR(Range, 0.75f, 2.0f) ARC_REFLECT_ATTR(Widget, "slider")
            ARC_REFLECT_ATTR(Tooltip, "Multiplies every editor size: spacing, widgets, fonts.")
        ARC_REFLECT_FIELD(EditorUiSettings, followDpi)
            ARC_REFLECT_ATTR(DisplayName, "Follow monitor DPI")
            ARC_REFLECT_ATTR(Tooltip, "Also multiply the UI scale by the monitor's display scale.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorUiSettings);

    float EffectiveUiScale(const EditorUiSettings& ui, float displayScale) noexcept
    {
        const float dpi = (ui.followDpi && displayScale > 0.0f) ? displayScale : 1.0f;
        return std::clamp(ui.scale * dpi, 0.5f, 4.0f);
    }
}
