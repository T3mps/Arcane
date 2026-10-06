#pragma once

// The node canvases' tuning as cvars (settings sweep S6-34, inventory Part 3
// "Graph/Node editor"): editor.graph.* (the zoom table, the node / wire / pin
// chrome metrics, the wire vertex budget, the shader canvas's header gap, cull
// guard band and pin dot, the selection modifier, the frame-to-fit range and
// the pin legend), editor.graph.lod.* (the rendering LOD tier boundaries) and
// editor.graph.grid.* (the backdrop grid). Per-machine editor preferences.
//
// Plain structs, no settings machinery: the pure canvas headers
// (GraphNodeLod.hpp, GraphGridPhase.hpp) take a block by reference, so a
// device-less test drives them with no registry at all. ARC_SETTINGS and the
// reflection live in GraphCanvasSettings.cpp.
//
// Every default is the pre-sweep literal it replaced; the colours of the same
// canvases are editor.theme.graph.* (GraphThemeSettings.hpp).

#include <cstdint>
#include <string>

namespace Arcane::Editor
{
    // editor.graph.*
    struct GraphCanvasSettings
    {
        // Unreal's FFixedZoomLevelsContainer stops (GraphZoomLevels.hpp), space-
        // separated and strictly ascending. Each parses back (strtof, round to
        // nearest) to the exact float the old kZoomLevels literal held.
        std::string zoomLevels =
            "0.1 0.125 0.15 0.175 0.2 0.225 0.25 0.375 0.5 0.675 "
            "0.75 0.875 1 1.25 1.375 1.5 1.675 1.75 1.875 2";
        float nodeRounding            = 4.0f;
        float nodeBorderWidth         = 1.0f;
        float nodeBorderHoverWidth    = 1.5f;
        float nodeBorderSelectedWidth = 2.0f;   // spec s10: "selection = 2px"
        float wireThickness           = 2.0f;
        std::int32_t pinSegments      = 12;
        float wireHighlight           = 0.25f;  // a highlighted wire's lerp toward white
        std::int32_t wireSegmentsMin  = 12;
        std::int32_t wireSegmentsMax  = 64;
        float wireSegmentsPxPer       = 6.0f;   // screen px per straight segment
        float nodeHeaderGap           = 5.0f;   // canvas units
        float cullGuardBand           = 0.25f;  // UE's NodePanelDefs::GuardBandArea
        float pinDotRadius            = 4.0f;   // the shader canvas's pin dot, canvas units
        bool  shiftAddsToSelection    = true;
        float fitMaxZoom              = 1.0f;
        float fitMinZoom              = 0.5f;
        bool  showPinLegend           = true;
    };

    // editor.graph.lod.* -- the LAST zoom stop of each tier (GraphNodeLod.hpp);
    // a boundary value belongs to the lower tier.
    struct GraphLodSettings
    {
        float lowestMax  = 0.200f;
        float lowMax     = 0.250f;
        float mediumMax  = 0.675f;
        float defaultMax = 1.375f;
    };

    // editor.graph.grid.* -- the backdrop lattice (GraphGridPhase.hpp).
    struct GraphGridSettings
    {
        float zoomExponent  = 0.7f;    // screen period = baseSpacing * pow(zoom, k)
        float baseSpacing   = 20.0f;   // canvas units at zoom 1, before the octave snap
        float minorTargetPx = 22.0f;   // the on-screen period the minor octave is driven toward
        std::int32_t majorEvery = 8;   // minor lines per major; a power of two
    };
}
