#pragma once

// editor.viewport.grid.* and editor.viewport.grid3D.* (settings arc S6-21;
// inventory reconciliation R1 "Viewport grid"): ONE grid family read by both
// grids -- the 2D ViewportGrid (orthographic batched lines) and the 3D GridNode
// (the depth-tested ground quad). Mode-only values live in grid3D. Per-machine
// editor preferences, Live.
//
// The axis colours are NOT here: they stay values held for the axis
// unification re-bless (S5-2 decision 1, option A) -- GridSceneDesc::kAxis*
// and ViewportGrid.hpp's kGridAxis*.

#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Render/Nri/nodes/GridNode.hpp>   // GridSceneDesc

#include <cstdint>

namespace Arcane::Editor
{
    struct EditorGridSettings
    {
        float majorEvery = 10.0f;
        CVarColor lineColor{ 0.5f, 0.5f, 0.5f, 1.0f };
        float minorAlpha = 0.35f;
        float majorAlpha = 0.55f;
        float lineThickness = 1.0f;
        std::uint32_t maxLinesPerAxis = 16384;
        // The 2D grid's level crossfade window, in screen pixels of a level's
        // line spacing: a level fades in from fadeInPx and is full at fadeFullPx
        // (S6-45; the S5-2 review restored them from DERIVED: they are the grid
        // LOD's band, not UI chrome, so editor.ui.scale does not scale them).
        float fadeInPx   = 8.0f;
        float fadeFullPx = 24.0f;
    };

    struct EditorGrid3DSettings
    {
        CVarColor majorColor{ 0.6f, 0.6f, 0.6f, 0.6f };
        float minorSpacing = 1.0f;
        float fadeDistance = 200.0f;
        float minHalfExtent = 2000.0f;
        float extentPerAltitude = 100.0f;
    };

    // PURE: the 3D grid's desc from the two blocks -- every tunable the
    // GridNode reads. The view and the plane are the caller's (SetPlane after,
    // which also picks the V axis colour).
    [[nodiscard]] GridSceneDesc MakeGridScene(const EditorGridSettings& grid, const EditorGrid3DSettings& grid3D) noexcept;
}
