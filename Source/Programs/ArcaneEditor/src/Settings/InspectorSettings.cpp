#include "Settings/InspectorSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(InspectorSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.inspector", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(InspectorSettings, framePaddingY)
            ARC_REFLECT_ATTR(DisplayName, "Row padding") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 0.0, 12.0)
            ARC_REFLECT_ATTR(Keywords, "frame padding density rhythm compact")
            ARC_REFLECT_ATTR(Tooltip, "Vertical padding, in pixels, inside each field of the Inspector's component "
                                      "list.")
        ARC_REFLECT_FIELD(InspectorSettings, itemSpacingY)
            ARC_REFLECT_ATTR(DisplayName, "Row spacing") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 0.0, 12.0)
            ARC_REFLECT_ATTR(Keywords, "item spacing gap density rhythm compact")
            ARC_REFLECT_ATTR(Tooltip, "Vertical gap, in pixels, between the rows of the Inspector's component list.")
        ARC_REFLECT_FIELD(InspectorSettings, labelColumnFraction)
            ARC_REFLECT_ATTR(DisplayName, "Label column width") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 0.2, 0.7)
            ARC_REFLECT_ATTR(Keywords, "splitter name column property grid details")
            ARC_REFLECT_ATTR(Tooltip, "Share of a property grid's width the label column takes before you drag its "
                                      "splitter. Once dragged, the dragged width is kept for the session.")
        ARC_REFLECT_FIELD(InspectorSettings, labelSeedMinEm)
            ARC_REFLECT_ATTR(DisplayName, "Label column seed minimum") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 1.0, 64.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "splitter first frame degenerate")
            ARC_REFLECT_ATTR(Tooltip, "Narrowest region, in font heights, a property grid takes its label column's "
                                      "first width from. A narrower first frame (a dock still settling) is skipped "
                                      "and the seed retried on the next frame.")
        ARC_REFLECT_FIELD(InspectorSettings, assetThumbMaxPx)
            ARC_REFLECT_ATTR(DisplayName, "Asset thumbnail maximum") ARC_REFLECT_ATTR(Category, "Asset page")
            ARC_REFLECT_ATTR(Range, 64.0, 512.0)
            ARC_REFLECT_ATTR(Keywords, "preview image size largest")
            ARC_REFLECT_ATTR(Tooltip, "Largest size, in pixels, of the asset page's thumbnail.")
        ARC_REFLECT_FIELD(InspectorSettings, historyDepth)
            ARC_REFLECT_ATTR(DisplayName, "History depth") ARC_REFLECT_ATTR(Category, "History and windows")
            ARC_REFLECT_ATTR(Range, 1.0, 256.0)
            ARC_REFLECT_ATTR(Keywords, "back forward navigation selection")
            ARC_REFLECT_ATTR(Tooltip, "How many selections the Inspectors' Back/Forward history remembers. A lower "
                                      "value trims the oldest entries at the next selection.")
        ARC_REFLECT_FIELD(InspectorSettings, maxInstances)
            ARC_REFLECT_ATTR(DisplayName, "Inspector windows") ARC_REFLECT_ATTR(Category, "History and windows")
            ARC_REFLECT_ATTR(Range, 2.0, 32.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "instances details panels limit")
            ARC_REFLECT_ATTR(Tooltip, "Most Inspector windows open at once, the main one included. Lowering it "
                                      "drops the saved windows above the new limit at the next start.")
        ARC_REFLECT_FIELD(InspectorSettings, dragSpeed)
            ARC_REFLECT_ATTR(DisplayName, "Drag speed") ARC_REFLECT_ATTR(Category, "Editing")
            ARC_REFLECT_ATTR(Range, 0.001, 10.0)
            ARC_REFLECT_ATTR(Keywords, "slider sensitivity scrub float vector")
            ARC_REFLECT_ATTR(Tooltip, "How much a number or vector field changes per pixel dragged, when the field "
                                      "declares no step of its own.")
        ARC_REFLECT_FIELD(InspectorSettings, rotationDragSpeedDeg)
            ARC_REFLECT_ATTR(DisplayName, "Rotation drag speed") ARC_REFLECT_ATTR(Category, "Editing")
            ARC_REFLECT_ATTR(Range, 0.01, 10.0)
            ARC_REFLECT_ATTR(Keywords, "slider sensitivity scrub degrees euler quaternion")
            ARC_REFLECT_ATTR(Tooltip, "Degrees a rotation field turns per pixel dragged. A field shown in radians "
                                      "scales with it.")
        ARC_REFLECT_FIELD(InspectorSettings, materialPreviewFraction)
            ARC_REFLECT_ATTR(DisplayName, "Material preview height") ARC_REFLECT_ATTR(Category, "Material page")
            ARC_REFLECT_ATTR(Range, 0.2, 0.8)
            ARC_REFLECT_ATTR(Keywords, "preview square sphere share")
            ARC_REFLECT_ATTR(Tooltip, "Largest share of the Inspector's height the material page's preview square "
                                      "may take.")
        ARC_REFLECT_FIELD(InspectorSettings, nodePageMinTextRun)
            ARC_REFLECT_ATTR(DisplayName, "Pin row text run") ARC_REFLECT_ATTR(Category, "Node page")
            ARC_REFLECT_ATTR(Range, 0.0, 256.0)
            ARC_REFLECT_ATTR(Keywords, "pin type word dot characters readable")
            ARC_REFLECT_ATTR(Tooltip, "Characters of a node page pin row's wiring or default text that must stay "
                                      "readable after the widest pin type word (e.g. 'dynamic (unresolved)'); a "
                                      "value cell narrower than dot + that word + this run shows only the pin's dot "
                                      "on every row of the page and moves the type word into the row's hover "
                                      "tooltip.")
        ARC_REFLECT_FIELD(InspectorSettings, assetThumbMinPx)
            ARC_REFLECT_ATTR(DisplayName, "Asset thumbnail minimum") ARC_REFLECT_ATTR(Category, "Asset page")
            ARC_REFLECT_ATTR(Range, 32.0, 512.0)
            ARC_REFLECT_ATTR(Keywords, "preview image size smallest floor")
            ARC_REFLECT_ATTR(Tooltip, "Smallest the asset page's thumbnail shrinks to in a short Inspector, in "
                                      "pixels. Never above the thumbnail maximum.")
        ARC_REFLECT_FIELD(InspectorSettings, assetThumbHeightFraction)
            ARC_REFLECT_ATTR(DisplayName, "Asset thumbnail height") ARC_REFLECT_ATTR(Category, "Asset page")
            ARC_REFLECT_ATTR(Range, 0.1, 0.6)
            ARC_REFLECT_ATTR(Keywords, "preview image share")
            ARC_REFLECT_ATTR(Tooltip, "Share of the Inspector's height the asset page's thumbnail may take.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(InspectorSettings);

    ARC_REFLECT_TYPE(OutlinerSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.outliner", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(OutlinerSettings, slowClickMaxSeconds)
            ARC_REFLECT_ATTR(DisplayName, "Slow-click rename window")
            ARC_REFLECT_ATTR(Range, 0.4, 3.0)
            ARC_REFLECT_ATTR(Keywords, "rename click delay seconds second click")
            ARC_REFLECT_ATTR(Tooltip, "A second click on the only selected Outliner row, slower than a double-click "
                                      "but within this many seconds of the first, starts renaming it.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(OutlinerSettings);
}
