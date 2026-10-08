#pragma once

// The node canvases' tuning as cvars (settings sweep S6-34, inventory Part 3
// "Graph/Node editor"): editor.graph.* (the zoom table, the node / wire / pin
// chrome metrics, the wire vertex budget, the shader canvas's header gap, cull
// guard band and pin dot, the selection modifier, the frame-to-fit range and
// the pin legend, the shader canvas's node padding), editor.graph.lod.* (the
// rendering LOD tier boundaries), editor.graph.grid.* (the backdrop grid) and
// editor.graph.pinRing.* (a pin dot's ring weights; S6-44). Per-machine editor
// preferences.
//
// Plain structs, no settings machinery: the pure canvas headers
// (GraphNodeLod.hpp, GraphGridPhase.hpp) take a block by reference, so a
// device-less test drives them with no registry at all. ARC_SETTINGS and the
// reflection live in GraphCanvasSettings.cpp.
//
// Every default is the pre-sweep literal it replaced; the colours of the same
// canvases are editor.theme.graph.* (GraphThemeSettings.hpp).

#include <Arcane/Config/CVarTypes.hpp>

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
        // The shader canvas's node padding, canvas units at zoom 1 (x left and
        // right, y top and bottom): ImGui measures those nodes from their
        // content, so they need it; the Graph lens lays its rows out by hand
        // with none (S6-44; the S5-2 review restored it from DERIVED).
        CVarVec2 nodePadding{ 10.0f, 6.0f };

        // The shader canvas's in-node widgets (S6-45; the S5-2 review restored
        // them from DERIVED): they sit inside canvas nodes and scale with the
        // graph zoom, so they are canvas units, NOT Ui::Px chrome.
        float nodePreviewMinPx      = 96.0f;   // the Output node's preview thumbnail, smallest side
        float dragSpeed             = 0.01f;   // a constant / param default's change per px dragged
        float rangeDragSpeed        = 0.05f;   // a param range's change per px dragged
        float constPinNeutralWidth1 = 64.0f;   // an unwired input's literal field, by lane count
        float constPinNeutralWidth2 = 106.0f;
        float constPinNeutralWidth4 = 190.0f;  // 3 or 4 lanes
        float constFloatWidth       = 90.0f;   // a Const / param default field, by lane count
        float constFloat2Width      = 140.0f;
        float constFloat4Width      = 220.0f;  // Float4 / Color, 3 or 4 lanes
        float constParamRangeWidth  = 120.0f;  // a param's min/max field
        float paramNameFieldWidth   = 110.0f;
        float passNameFieldWidth    = 120.0f;  // a pass node's name field (the pass chain)
        float swizzleFieldWidth     = 70.0f;
    };

    // The in-node field width for a value of `lanes` components (1, 2, else
    // 3-4): the unwired-pin literal row and the Const / param default rows.
    [[nodiscard]] inline float GraphPinNeutralWidth(const GraphCanvasSettings& s, int lanes) noexcept
    {
        return lanes == 1 ? s.constPinNeutralWidth1 : lanes == 2 ? s.constPinNeutralWidth2 : s.constPinNeutralWidth4;
    }
    [[nodiscard]] inline float GraphConstValueWidth(const GraphCanvasSettings& s, int lanes) noexcept
    {
        return lanes == 1 ? s.constFloatWidth : lanes == 2 ? s.constFloat2Width : s.constFloat4Width;
    }

    // editor.graph.nodePadding is ApplyMode::Restart (spec s3.4: read once at
    // boot), so the shader canvas never reads the published snapshot: the
    // editor captures the value once at startup (EditorApp's boot, after the
    // early config rungs) and the canvas reads that capture until the next
    // restart. An edit in Preferences waits behind the "restart required"
    // badge. A host that never runs the editor's boot latches at the first
    // GraphNodePaddingAtBoot() read; a test simulating a restart calls the
    // capture again. Defined in GraphCanvasSettings.cpp.
    void CaptureGraphNodePaddingAtBoot();
    [[nodiscard]] CVarVec2 GraphNodePaddingAtBoot();

    // editor.graph.pinRing.* -- a pin dot's ring weight, and the OPTIONAL outer
    // ring DrawGraphPinDot adds around a dot (the shader canvas's "adapts to
    // its input" mark on a resolved dynamic pin): its centreline sits outerGap
    // outside the dot's radius, at outerWidth -- thin, so it reads as a halo
    // and not as a second, hollow pin. Canvas units at zoom 1 (S6-44).
    struct GraphPinRingSettings
    {
        float width      = 1.6f;
        float outerGap   = 2.2f;
        float outerWidth = 1.0f;
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
