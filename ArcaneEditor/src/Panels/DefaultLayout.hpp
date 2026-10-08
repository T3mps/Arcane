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
//             with Inspector 2 ("Assets only") on its right, 1144 : 392 at
//             1920x1080.
//
// TWO KINDS OF SPLIT, because ImGui resizes them differently
// (imgui.cpp DockNodeTreeUpdatePosSize):
//  * CENTRAL-ADJACENT splits (Inspector | rest, band / top, Outliner |
//    Viewport) take PIXEL targets: one side holds the central node, so ImGui
//    keeps the OTHER side at its SizeRef pixels when the dockspace grows and
//    the central node absorbs the delta (its rule 3). The first build usually
//    happens in the hidden 1280x720 boot window -- a ratio taken there would
//    shrink every panel at 1920x1080. BuildDefaultLayout (EditorPanels.cpp)
//    converts each target against the node's CURRENT size; the targets are
//    clamped here so the central node keeps at least
//    LayoutFactorySettings::centralMinFraction of each axis at the build size
//    (a 1280x720 build must still be usable).
//  * The BAND's split (Asset Browser | Inspector 2) has NO central node on
//    either side, so ImGui re-divides it by the children's SizeRef RATIO on
//    every resize (its rule 4) -- a pixel target there only holds at the
//    build size (390 px built at 1280x720 grew to ~668 px maximized). It is
//    built from the user's 1920-scale PROPORTION instead -- exactly how their
//    own saved layout behaves (its ini stores SizeRef 1144 / 392) -- so it
//    is right at every size. The legacy upgrade's split of the browser's
//    node shares the rule and the proportion.
// The numbers are the editor.layout.factory.* cvars (settings S6-32,
// LayoutFactorySettings): BuildDefaultLayout reads the published snapshot
// when it builds (Reset Layout, a first run), so a changed value shows at the
// next Reset Layout. The maths below stays pure: unit-tested.

#include "Settings/EditorPlaySettings.hpp"   // LayoutFactorySettings

#include <algorithm>

namespace Arcane::Editor
{
    // Inspector 2's share of the node it splits (the default's band, or the
    // legacy upgrade's browser node) -- DockBuilderSplitNode's ratio, as is.
    [[nodiscard]] inline float DefaultAssetsInspectorBandFraction(const LayoutFactorySettings& f)
    {
        return f.assetsInspectorRefPx / (f.browserRefPx + f.assetsInspectorRefPx);
    }

    struct DefaultLayoutPixels
    {
        float inspector = 0.0f;        // the main Inspector's column (right, full height)
        float outliner = 0.0f;         // the Outliner (top-left)
        float bottomBand = 0.0f;       // the asset/console band's height (under the Outliner, left of the Inspector)
    };

    // The central-adjacent geometry at a dockspace of `width` x `height`
    // (Inspector 2 is not a pixel target: DefaultAssetsInspectorBandFraction).
    [[nodiscard]] inline DefaultLayoutPixels ComputeDefaultLayoutPixels(float width, float height,
                                                                       const LayoutFactorySettings& f)
    {
        DefaultLayoutPixels px;
        // Width: Inspector + Outliner may take at most (1 - min) of it; over
        // that, both shrink by the same factor (their proportion kept).
        const float sideMax = width * (1.0f - f.centralMinFraction);
        const float sides = f.inspectorWidth + f.outlinerWidth;
        const float sideScale = sides > sideMax ? sideMax / sides : 1.0f;
        px.inspector = f.inspectorWidth * sideScale;
        px.outliner  = f.outlinerWidth * sideScale;
        // Height: the bottom band may take at most (1 - min) of it.
        px.bottomBand = std::min(f.bottomBand, height * (1.0f - f.centralMinFraction));
        return px;
    }
}
