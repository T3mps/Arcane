#pragma once

// The Asset Browser Graph lens's tuning as cvars (settings sweep S6-36,
// inventory Part 3 "Graph/Node editor"): editor.assetGraph.* -- the depth and
// breadth caps a new graph view starts with (per-project preferences), the
// focus popup's hit cap, the layout pitch, the wire/overflow/ghost dims and the
// dashed in-flight wire's LOD floor (per-machine preferences). Live.
//
// A plain struct, no settings machinery: ARC_SETTINGS and the reflection live
// in AssetGraphSettings.cpp. Every default is the pre-sweep literal it
// replaced. The lens's pixel metrics are DERIVED px (they scale with
// editor.ui.scale), not settings.

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
    };
}
