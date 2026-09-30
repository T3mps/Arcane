#pragma once

// The editor's DEFAULT dock layout geometry (USER DECISION 2026-09-30,
// binding): the default IS the user's ReferenceProject layout (their saved
// %LOCALAPPDATA%\Arcane\editor\layouts\cfafaf09-...ini, at 1920x1080):
//   RIGHT  the main Inspector ("All but Assets"), FULL HEIGHT, ~380 px;
//   LEFT of it, split top/bottom:
//     TOP     the Outliner (~270 px) | the central node (Viewport + documents);
//     BOTTOM  a ~350 px band from the left window edge to the Inspector (it
//             runs UNDER the Outliner): ONE tab node -- Asset Browser
//             (selected), Asset Graph, Asset Status, Console, Problems --
//             with Inspector 2 ("Assets only", ~390 px) on its right.
// PIXEL targets, not ratios: ImGui docking keeps every NON-central node at
// its build-time pixel size when the dockspace grows (the central node
// absorbs the delta), and the first build usually happens in the hidden
// 1280x720 boot window -- a ratio taken there would shrink every panel at
// 1920x1080. BuildDefaultLayout (EditorPanels.cpp) converts each target
// against the node's CURRENT size; the targets are clamped here so the
// central node keeps at least kDefaultCentralMinFraction of each axis at the
// build size (a 1280x720 build must still be usable). Pure: unit-tested.

#include <algorithm>

namespace Arcane::Editor
{
    inline constexpr float kDefaultInspectorWidthPx   = 380.0f;   // the main Inspector's column
    inline constexpr float kDefaultOutlinerWidthPx    = 270.0f;
    inline constexpr float kDefaultBottomBandPx       = 350.0f;   // the asset/console band's height
    inline constexpr float kDefaultAssetsInspectorPx  = 390.0f;   // Inspector 2 ("Assets only") -- also the legacy upgrade's target
    inline constexpr float kDefaultCentralMinFraction = 0.40f;    // the central node keeps >= 40% of each axis
    inline constexpr float kAssetsInspectorMaxFraction = 0.45f;   // Inspector 2 never takes more than 45% of the node it splits

    struct DefaultLayoutPixels
    {
        float inspector = 0.0f;        // the main Inspector's column (right, full height)
        float outliner = 0.0f;         // the Outliner (top-left)
        float bottomBand = 0.0f;       // the asset/console band's height (under the Outliner, left of the Inspector)
        float assetsInspector = 0.0f;  // Inspector 2 ("Assets only"), right end of the band
    };

    // The geometry at a dockspace of `width` x `height`.
    [[nodiscard]] inline DefaultLayoutPixels ComputeDefaultLayoutPixels(float width, float height)
    {
        DefaultLayoutPixels px;
        // Width: Inspector + Outliner may take at most (1 - min) of it; over
        // that, both shrink by the same factor (their proportion kept).
        const float sideMax = width * (1.0f - kDefaultCentralMinFraction);
        const float sides = kDefaultInspectorWidthPx + kDefaultOutlinerWidthPx;
        const float sideScale = sides > sideMax ? sideMax / sides : 1.0f;
        px.inspector = kDefaultInspectorWidthPx * sideScale;
        px.outliner  = kDefaultOutlinerWidthPx * sideScale;
        // Height: the bottom band may take at most (1 - min) of it.
        px.bottomBand = std::min(kDefaultBottomBandPx, height * (1.0f - kDefaultCentralMinFraction));
        // Inspector 2 splits the band (width - Inspector): at most 45% of it.
        px.assetsInspector = std::min(kDefaultAssetsInspectorPx, (width - px.inspector) * kAssetsInspectorMaxFraction);
        return px;
    }

    // The legacy upgrade's Inspector 2 width inside a browser node `nodeWidth` wide.
    [[nodiscard]] inline float LegacyAssetsInspectorPixels(float nodeWidth)
    {
        return std::min(kDefaultAssetsInspectorPx, nodeWidth * kAssetsInspectorMaxFraction);
    }
}
