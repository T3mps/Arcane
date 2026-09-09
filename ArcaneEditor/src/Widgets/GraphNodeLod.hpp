#pragma once

// Graph-canvas LEVEL OF DETAIL -- Unreal's zoom table, third column.
//
// GraphZoomLevels.hpp holds the STOPS a node canvas can zoom to; this holds
// what each stop is legible enough FOR. Both are read off the same vendored
// source (FFixedZoomLevelsContainer, Arcane/.example/UnrealEngine-release/
// Engine/Source/Editor/GraphEditor/Private/SNodePanel.cpp:53-75), so they move
// together or not at all.
//
// Its own header rather than a section of GraphZoomLevels.hpp, following that
// file's own reasoning about CanvasPopupScope.hpp: one named rule per file, and
// "how far can the wheel zoom" is a separate concern from "what is worth
// drawing at this scale". No node-editor include is needed -- a tier is a
// comparison on a float -- which is also why this is the one canvas header a
// device-less test can read with no canvas at all.
//
// It used to live in ShaderEditorDocument.hpp (the enum) and .cpp (the
// boundaries + the lookup), while the Graph lens copied THE NUMBER 0.250 out of
// it into a bare float compare and said so in a comment. Now both read the
// table.

namespace Arcane::Editor
{
    // Graph-canvas rendering level of detail -- Unreal's EGraphRenderingLOD,
    // ported including its ORDERING, which every gate depends on: the enum runs
    // from "zoomed all the way out" to "zoomed in past 1:1", so GREATER MEANS
    // MORE DETAIL and a degradation is always written as `lod <= Tier` (or
    // `lod < Tier`), exactly as UE writes them. Vendored source:
    // Arcane/.example/UnrealEngine-release/Engine/Source/Editor/GraphEditor/
    // Public/SNodePanel.h:70-90; the per-tier comments below are UE's own.
    enum class NodeLOD
    {
        LowestDetail = 0,   // zoomed all the way out (all optimizations on)
        LowDetail,          // text is unreadable, so it starts being dropped
        MediumDetail,       // text is hard to read but is still drawn
        DefaultDetail,      // zoomed in at 1:1
        FullyZoomedIn,      // zoomed in past 1:1
    };

    // Each constant is the LAST kZoomLevels entry belonging to that tier, read
    // straight off FFixedZoomLevelsContainer (SNodePanel.cpp:56-75):
    //   0.100 .. 0.200          LowestDetail
    //   0.225 .. 0.250          LowDetail
    //   0.375 .. 0.675          MediumDetail
    //   0.750 .. 1.375          DefaultDetail
    //   1.500 .. 2.000          FullyZoomedIn
    // UE indexes its table and looks the tier up by INDEX (SNodePanel.cpp:1921);
    // we compare the scale instead, because the canvas can also sit BETWEEN
    // stops -- ed::NavigateToContent / NavigateToSelection fit a rectangle and
    // land on an arbitrary scale (imgui_node_editor.cpp:3516-3548), which an
    // index lookup has no answer for. Comparing covers both.
    inline constexpr float kLodLowestMax  = 0.200f;
    inline constexpr float kLodLowMax     = 0.250f;
    inline constexpr float kLodMediumMax  = 0.675f;
    inline constexpr float kLodDefaultMax = 1.375f;

    // The canvas's tier at a given view scale (GraphWire.hpp's GraphViewScale,
    // i.e. a zoom-stop-space number, NOT ed::GetCurrentZoom's reciprocal).
    //
    // A boundary value belongs to the LOWER tier (0.200 is LowestDetail, not
    // LowDetail), matching the table; the epsilon only protects that from float
    // round-trips through the editor's zoom state.
    //
    // THE EPSILON'S REACH, stated exactly, because a consumer converting from a
    // bare float compare inherits it. A scale in the half-open band
    // (kLodBoundary, kLodBoundary + kEps] answers the LOWER tier. No entry in
    // kZoomLevels sits inside any such band, so no reachable zoom STOP changes
    // tier because of it -- but a STOP is not the only scale a canvas can sit
    // at: ed::NavigateToContent / NavigateToSelection fit a rectangle and land
    // on an arbitrary scale (imgui_node_editor.cpp:3516-3548), and such a fit
    // CAN land inside a band. There, this answers one tier lower than a bare
    // `scale > kLodBoundary` would -- i.e. a consumer degrades a hair earlier,
    // over a window 1e-4 wide. Recorded rather than engineered around: the
    // consequence is imperceptible and the epsilon is doing its real job at the
    // stops.
    inline NodeLOD NodeLODForScale(float scale) noexcept
    {
        constexpr float kEps = 1e-4f;
        if (scale <= kLodLowestMax  + kEps) return NodeLOD::LowestDetail;
        if (scale <= kLodLowMax     + kEps) return NodeLOD::LowDetail;
        if (scale <= kLodMediumMax  + kEps) return NodeLOD::MediumDetail;
        if (scale <= kLodDefaultMax + kEps) return NodeLOD::DefaultDetail;
        return NodeLOD::FullyZoomedIn;
    }
}
