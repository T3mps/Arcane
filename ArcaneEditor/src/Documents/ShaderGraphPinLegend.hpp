#pragma once

// The shader graph canvas's PIN LEGEND (T3-D1, 2026-10-01 desk): a compact key
// pinned to the canvas's bottom-left corner -- the four type colours with
// their words, "ring = adapts to its input" and "filled = wired, hollow =
// unwired" -- that folds to a small "?" chip. Every swatch is painted through
// PinPaintFor / PinColorForWidth (ShaderGraphPinTypes.hpp) and
// DrawGraphPinDot, never from a literal, so the key cannot contradict the
// canvas (the AssetGraphPanel legend ruling). The box is the shared legend
// chrome (Widgets/GraphLegend.hpp).
//
// Whether it is open is the Archive cvar editor.graph.showPinLegend (Bool,
// default true).
//
// SCREEN SPACE, never the node editor's transformed space, so zoom never
// scales it. Two calls per canvas frame, because the click and the paint want
// opposite ends of the frame:
//   1. GraphPinLegendInteract -- inside ed::Begin/End, under a
//      CanvasPopupScope (ed::Suspend), BEFORE any node is submitted. ImGui
//      gives hover to the FIRST item submitted over a point, and the canvas
//      submits its own node / pin / background hit areas later, in ed::End --
//      so a click on the legend folds it instead of reaching the graph.
//   2. DrawGraphPinLegend -- after ed::End, so the paint lands above every
//      node (the AssetGraphPanel legend's placement).

#include <imgui.h>

namespace Arcane::Editor
{
    // editor.graph.showPinLegend: true = the full key, false = the "?" chip.
    [[nodiscard]] bool GraphPinLegendShown();
    // Writes the cvar (SetBy::User, the layer an Archive write keeps). It
    // publishes with the frame's cvar publish, so the legend flips on the
    // next frame.
    void SetGraphPinLegendShown(bool shown);

    // The legend box's size open (`expanded`) or folded to the chip, in screen
    // pixels at the legend's 13 px font.
    [[nodiscard]] ImVec2 GraphPinLegendBoxSize(bool expanded);

    // Step 1 above. `canvasMin` / `canvasSize` = the canvas's screen rect.
    // Returns whether the legend is hovered (step 2 brightens its border).
    bool GraphPinLegendInteract(const ImVec2& canvasMin, const ImVec2& canvasSize);
    // Step 2 above.
    void DrawGraphPinLegend(const ImVec2& canvasMin, const ImVec2& canvasSize, bool hovered);
}
