#include "Settings/AssetGraphSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(AssetGraphSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.assetGraph", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(AssetGraphSettings, defaultDepth)
            ARC_REFLECT_ATTR(DisplayName, "Depth") ARC_REFLECT_ATTR(Category, "Scope")
            ARC_REFLECT_ATTR(Range, 1.0, 16.0) ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject)
            ARC_REFLECT_ATTR(Keywords, "hops levels references dependencies")
            ARC_REFLECT_ATTR(Tooltip, "How many reference hops the Graph lens follows from the focused asset, in each "
                                      "direction.")
        ARC_REFLECT_FIELD(AssetGraphSettings, breadthCap)
            ARC_REFLECT_ATTR(DisplayName, "Breadth cap") ARC_REFLECT_ATTR(Category, "Scope")
            ARC_REFLECT_ATTR(Range, 1.0, 1000.0) ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject)
            ARC_REFLECT_ATTR(Keywords, "more overflow neighbours limit")
            ARC_REFLECT_ATTR(Tooltip, "How many neighbours a node shows per direction before the rest fold into a "
                                      "\"+N more\" node.")
        ARC_REFLECT_FIELD(AssetGraphSettings, focusHitCap)
            ARC_REFLECT_ATTR(DisplayName, "Focus popup files") ARC_REFLECT_ATTR(Category, "Scope")
            ARC_REFLECT_ATTR(Range, 1.0, 200.0)
            ARC_REFLECT_ATTR(Keywords, "search results suggestions")
            ARC_REFLECT_ATTR(Tooltip, "How many matching files the Graph lens's focus popup lists (the @kind keywords "
                                      "always fit).")
        ARC_REFLECT_FIELD(AssetGraphSettings, layoutColumnPitch)
            ARC_REFLECT_ATTR(DisplayName, "Column pitch") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 230.0, 2000.0)
            ARC_REFLECT_ATTR(Keywords, "spacing horizontal gap")
            ARC_REFLECT_ATTR(Tooltip, "Distance, in canvas units, between the Graph lens's layer columns. Values near "
                                      "the widest node (220) leave little room for the wires.")
        ARC_REFLECT_FIELD(AssetGraphSettings, layoutRowPitch)
            ARC_REFLECT_ATTR(DisplayName, "Row pitch") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 60.0, 1000.0)
            ARC_REFLECT_ATTR(Keywords, "spacing vertical gap")
            ARC_REFLECT_ATTR(Tooltip, "Distance, in canvas units, between stacked nodes in a Graph lens column.")
        ARC_REFLECT_FIELD(AssetGraphSettings, wireDim)
            ARC_REFLECT_ATTR(DisplayName, "Wire dim") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Keywords, "edge fade")
            ARC_REFLECT_ATTR(Tooltip, "How far an edge that is not selected or hovered fades toward the canvas "
                                      "(0 = full asset-kind color, 1 = invisible).")
        ARC_REFLECT_FIELD(AssetGraphSettings, overflowDim)
            ARC_REFLECT_ATTR(DisplayName, "Overflow wire dim") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Keywords, "more edge fade")
            ARC_REFLECT_ATTR(Tooltip, "How far the connector to a \"+N more\" node fades toward the canvas.")
        ARC_REFLECT_FIELD(AssetGraphSettings, ghostWash)
            ARC_REFLECT_ATTR(DisplayName, "Ghost wash") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Keywords, "tombstone missing overflow fade")
            ARC_REFLECT_ATTR(Tooltip, "How strongly a missing asset's or a \"+N more\" node's body fades toward the "
                                      "canvas.")
        ARC_REFLECT_FIELD(AssetGraphSettings, dashMaxCells)
            ARC_REFLECT_ATTR(DisplayName, "Dashed wire cell cap") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 8.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "Most dash cells the in-flight (dragged) wire draws; past it the dashes "
                                      "stretch. A performance guard for extreme zoom.")
        ARC_REFLECT_FIELD(AssetGraphSettings, pinRadius)
            ARC_REFLECT_ATTR(DisplayName, "Pin radius") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 1.0, 16.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "port dot size")
            ARC_REFLECT_ATTR(Tooltip, "Radius, in canvas units, of a Graph lens node's port dot. It scales with the "
                                      "graph zoom.")
        ARC_REFLECT_FIELD(AssetGraphSettings, overflowWireThickness)
            ARC_REFLECT_ATTR(DisplayName, "Overflow wire thickness") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.25, 8.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "more connector width")
            ARC_REFLECT_ATTR(Tooltip, "Thickness, in canvas units, of the connector from a node to its \"+N more\" "
                                      "node.")
        ARC_REFLECT_FIELD(AssetGraphSettings, labelPad)
            ARC_REFLECT_ATTR(DisplayName, "Edge label padding") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.0, 16.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "plate inset")
            ARC_REFLECT_ATTR(Tooltip, "Horizontal inset, in canvas units, of the plate behind a mid-edge label.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(AssetGraphNodeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.assetGraph.node", SettingScope::PreferencesMachine, ApplyMode::Live,
                              Audience::Editor)
        ARC_REFLECT_FIELD(AssetGraphNodeSettings, minWidth)
            ARC_REFLECT_ATTR(DisplayName, "Minimum width") ARC_REFLECT_ATTR(Range, 40.0, 1000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Narrowest a Graph lens node gets, in canvas units, however short its labels.")
        ARC_REFLECT_FIELD(AssetGraphNodeSettings, maxWidth)
            ARC_REFLECT_ATTR(DisplayName, "Maximum width") ARC_REFLECT_ATTR(Range, 40.0, 1000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Widest a Graph lens node grows to fit its labels, in canvas units. Below "
                                      "the minimum width, the minimum wins.")
        ARC_REFLECT_FIELD(AssetGraphNodeSettings, headerHeight)
            ARC_REFLECT_ATTR(DisplayName, "Header height") ARC_REFLECT_ATTR(Range, 8.0, 96.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Height, in canvas units, of a Graph lens node's title band.")
        ARC_REFLECT_FIELD(AssetGraphNodeSettings, accentBarWidth)
            ARC_REFLECT_ATTR(DisplayName, "Accent bar width") ARC_REFLECT_ATTR(Range, 0.0, 16.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "kind colour stripe")
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of the asset-kind colour bar down a Graph lens node's "
                                      "left edge.")
        ARC_REFLECT_FIELD(AssetGraphNodeSettings, padding)
            ARC_REFLECT_ATTR(DisplayName, "Padding") ARC_REFLECT_ATTR(Range, 0.0, 32.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "inset gap margin")
            ARC_REFLECT_ATTR(Tooltip, "A Graph lens node's insets, in canvas units: x = left (after the accent bar), "
                                      "y = right, z = the body row's top and bottom, w = the gap beside a header icon "
                                      "or pill.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(AssetGraphSettings);
    ARC_SETTINGS(AssetGraphNodeSettings);
}
