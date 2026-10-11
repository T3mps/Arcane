#pragma once

// The Inspector's, PropertyGrid's and Outliner's tuning as cvars (settings
// sweep S6-37, inventory Part 3 "Inspector/PropertyGrid" + "Shortcut timing"):
// editor.inspector.* -- the row rhythm, the label column seed, the asset and
// material page previews, the node page's pin-row text run, the shared
// selection history and the instance pool, and the field drag speeds -- and
// editor.outliner.slowClickMaxSeconds. Per-machine preferences, Live except
// maxInstances (Restart: the host latches it at construction).
//
// The four one-off cvars editor.inspector.materialPreviewFraction,
// .nodePageMinTextRun, .assetThumbMinPx and .assetThumbHeightFraction fold in
// as fields with the same names, ranges and defaults (assetThumbMinPx's
// ceiling widens to 512 with assetThumbMaxPx; the reader clamps min <= max).
//
// Plain structs, no settings machinery: ARC_SETTINGS and the reflection live
// in InspectorSettings.cpp. Every default is the pre-sweep literal it replaced.

#include <cstdint>

namespace Arcane::Editor
{
    // editor.inspector.*
    struct InspectorSettings
    {
        float        framePaddingY            = 3.0f;    // px, the component list's row FramePadding.y
        float        itemSpacingY             = 4.0f;    // px, the component list's row ItemSpacing.y
        float        labelColumnFraction      = 0.4f;    // the label column's first-use share of the grid
        float        labelSeedMinEm           = 8.0f;    // font heights a region needs before it seeds the column
        float        assetThumbMaxPx          = 140.0f;  // the asset page thumbnail's largest size
        std::int32_t historyDepth             = 32;      // shared selection-history entries
        std::int32_t maxInstances             = 8;       // Inspector windows (ids 0..n-1); Restart
        float        dragSpeed                = 0.1f;    // field units per dragged pixel
        float        rotationDragSpeedDeg     = 0.5f;    // degrees per dragged pixel (radians scale with it)
        float        materialPreviewFraction  = 0.45f;   // the material page preview's height cap
        std::int32_t nodePageMinTextRun       = 16;      // characters a node page pin row keeps readable
        std::int32_t assetThumbMinPx          = 64;      // the asset page thumbnail's floor
        float        assetThumbHeightFraction = 0.30f;   // the asset page thumbnail's height share
    };

    // editor.outliner.*
    struct OutlinerSettings
    {
        double slowClickMaxSeconds = 1.2;   // a second click on the sole-selected row within this renames it
    };
}
