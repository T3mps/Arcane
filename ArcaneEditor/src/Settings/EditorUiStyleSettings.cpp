#include "Settings/EditorUiStyleSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorUiStyleSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.ui", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Style and Density")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, frameBorderSize)
            ARC_REFLECT_ATTR(DisplayName, "Field border") ARC_REFLECT_ATTR(Category, "Style") ARC_REFLECT_ATTR(Range, 0.0, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (px at scale 1) of the inset line around every input field and button. 0 draws fills only.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, dockNodeCloseButton)
            ARC_REFLECT_ATTR(DisplayName, "Dock close button") ARC_REFLECT_ATTR(Category, "Style")
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Show the extra X at the right end of every dock tab bar, which closes the node's visible tab. Each tab keeps its own X either way.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, tabOverlineSize)
            ARC_REFLECT_ATTR(DisplayName, "Tab overline") ARC_REFLECT_ATTR(Category, "Style") ARC_REFLECT_ATTR(Range, 0.0, 4.0)
            ARC_REFLECT_ATTR(Tooltip, "Thickness (px at scale 1) of the accent line over the selected tab.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, disabledAlpha)
            ARC_REFLECT_ATTR(DisplayName, "Disabled opacity") ARC_REFLECT_ATTR(Category, "Style") ARC_REFLECT_ATTR(Range, 0.2, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Opacity of disabled widgets. The default keeps disabled text a step under dim text.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, tabRounding)
            ARC_REFLECT_ATTR(DisplayName, "Tab rounding") ARC_REFLECT_ATTR(Category, "Style") ARC_REFLECT_ATTR(Range, 0.0, 8.0)
            ARC_REFLECT_ATTR(Tooltip, "Corner radius (px at scale 1) of tabs.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, tableRowHeight)
            ARC_REFLECT_ATTR(DisplayName, "Table row height") ARC_REFLECT_ATTR(Category, "Density") ARC_REFLECT_ATTR(Range, 16.0, 48.0)
            ARC_REFLECT_ATTR(Tooltip, "Height (px at scale 1 and font size 16) of a row in the asset tables and lists. Follows the UI scale and font size.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, assetRowThumbPx)
            ARC_REFLECT_ATTR(DisplayName, "Row thumbnail") ARC_REFLECT_ATTR(Category, "Density") ARC_REFLECT_ATTR(Range, 8.0, 48.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Size (px at scale 1 and font size 16) of the thumbnail cell at the start of an asset row. Keep it under the row height.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, assetRefThumbPx)
            ARC_REFLECT_ATTR(DisplayName, "Reference field thumbnail") ARC_REFLECT_ATTR(Category, "Density") ARC_REFLECT_ATTR(Range, 8.0, 48.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Size (px at scale 1 and font size 16) of the thumbnail in an asset reference field.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, propertyDragSpeed)
            ARC_REFLECT_ATTR(DisplayName, "Drag speed") ARC_REFLECT_ATTR(Category, "Inspector") ARC_REFLECT_ATTR(Range, 0.0001, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "How much a number field changes per pixel dragged, for fields without a range of their own.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, intStep)
            ARC_REFLECT_ATTR(DisplayName, "Integer step") ARC_REFLECT_ATTR(Category, "Inspector") ARC_REFLECT_ATTR(Range, 1.0, 1000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "What the - / + buttons of an unbounded integer field add.")
        ARC_REFLECT_FIELD(EditorUiStyleSettings, intStepFast)
            ARC_REFLECT_ATTR(DisplayName, "Integer step (Ctrl)") ARC_REFLECT_ATTR(Category, "Inspector") ARC_REFLECT_ATTR(Range, 1.0, 100000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "What the - / + buttons add with Ctrl held.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorUiStyleSettings);
}
