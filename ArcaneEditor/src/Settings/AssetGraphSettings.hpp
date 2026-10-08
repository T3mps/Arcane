#pragma once

// The Asset Browser Graph lens's tuning as cvars (settings sweep S6-36,
// inventory Part 3 "Graph/Node editor"): editor.assetGraph.* -- the depth and
// breadth caps a new graph view starts with (per-project preferences), the
// focus popup's hit cap, the layout pitch, the wire/overflow/ghost dims and the
// dashed in-flight wire's LOD floor (per-machine preferences), and the
// canvas-space node geometry: editor.assetGraph.node.* plus the pin radius,
// the overflow wire and the label pill pad (S6-44; the S5-2 review restored
// them from DERIVED: they scale with the graph zoom, not with editor.ui.scale).
// Live.
//
// Plain structs, no settings machinery: ARC_SETTINGS and the reflection live
// in AssetGraphSettings.cpp. Every default is the pre-sweep literal it
// replaced. The lens's screen-space chrome (the focus combo, the selection
// strip, the dash pattern) is DERIVED px (it scales with editor.ui.scale), not
// settings.

#include <Arcane/Config/CVarTypes.hpp>

#include <cstdint>

namespace Arcane::Editor
{
    // editor.assetGraph.*
    struct AssetGraphSettings
    {
        // Per-project (PreferencesProject): the view a graph build starts from.
        std::int32_t defaultDepth = 2;    // hops from the focus, per direction
        std::int32_t breadthCap   = 20;   // neighbours per node per direction before "+N more"
        // Per-machine (PreferencesMachine).
        std::int32_t focusHitCap       = 12;     // file rows in the focus popup; keywords always fit
        float        layoutColumnPitch = 300.0f; // canvas units between layer columns
        float        layoutRowPitch    = 90.0f;  // canvas units between stacked rows
        float        wireDim           = 0.62f;  // a non-emphasized edge's pull toward the canvas
        float        overflowDim       = 0.78f;  // the anchor -> "+N more" connector's pull
        float        ghostWash         = 0.55f;  // canvas tone over a tombstone / overflow body
        std::int32_t dashMaxCells      = 256;    // the dashed drag wire's cell-count LOD floor
        // Canvas units at zoom 1 (S6-44).
        float        pinRadius             = 4.5f;   // spec s11.2's 9px-across port dot
        float        overflowWireThickness = 1.5f;   // the anchor -> "+N more" connector
        float        labelPad              = 3.0f;   // a mid-edge label plate's horizontal inset
    };

    // editor.assetGraph.node.* -- spec s11.2's "graph nodes" row (w 180-220,
    // header 24px, accent bar 3px) and the board's `.nhead` insets, in canvas
    // units at zoom 1 (S6-44).
    struct AssetGraphNodeSettings
    {
        float minWidth       = 180.0f;
        float maxWidth       = 220.0f;
        float headerHeight   = 24.0f;
        float accentBarWidth = 3.0f;
        // x = left inset after the accent bar, y = right inset, z = the body
        // row's vertical pad, w = the gap beside a header icon or pill.
        CVarVec4 padding{ 8.0f, 8.0f, 6.0f, 6.0f };
    };
}
