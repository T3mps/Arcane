#pragma once

// Graph-canvas CHROME -- the metrics and the neutral palette every node canvas
// in the editor draws itself in, with one definition each.
//
// WHAT BELONGS HERE AND WHAT DOES NOT. These are the canvas's own LANGUAGE:
// how round a node's corner is, how thick its border grows when hovered or
// selected, how thick a wire is, how many segments a pin dot is drawn with,
// what a grid line looks like, and which accent means "selected" or "hovered".
// They are editor-wide facts, not per-canvas taste, and they were previously
// spelled as identical literals in two files (ShaderEditorDocument.cpp and
// AssetsPanel.cpp's Graph lens) with nothing holding them together.
//
// PER-CANVAS TASTE STAYS AT THE CANVAS. The node body/title/border tones and
// the canvas surface itself are NOT here and must not move here: the Graph
// lens answers to the OptionD board (a recorded controller ruling, 2026-09-08,
// written out at AssetsPanel.cpp's canvas-palette block) and the shader
// editor's canvas answers to its own board and its own review history. Those
// two value sets are deliberately different; the shared piece is the STRUCTURE
// that consumes them (GraphCanvasStyleDesc below), never the values.
//
// Its own header, sibling to CanvasPopupScope.hpp / GraphZoomLevels.hpp /
// GraphGridPhase.hpp rather than folded into EditorWidgets.hpp: that
// vocabulary is imgui.h plus Astra::Range only, and the canvas family is kept
// out of it so no editor widget acquires a node-editor dependency
// (CanvasPopupScope.hpp:16-19 states the rule; this header holds nothing that
// needs imgui_node_editor.h today, but it is the canvas family's home and the
// style applier that will join it does).

#include "Widgets/EditorTheme.hpp"

#include <imgui.h>

namespace Arcane::Editor
{
    // ---- Node chrome metrics (canvas units at zoom 1) --------------------
    // Both canvases wrote these as the same seven literals. A canvas that ever
    // wants to differ overrides through its own style application, not by
    // re-spelling the number.
    inline constexpr float kGraphNodeRounding       = 4.0f;
    inline constexpr float kGraphNodeBorderWidth    = 1.0f;
    inline constexpr float kGraphNodeHovBorderWidth = 1.5f;
    inline constexpr float kGraphNodeSelBorderWidth = 2.0f;   // spec §10: "selection = 2px"

    // ---- Wire + pin metrics ----------------------------------------------
    // The thickness handed to ed::Link is the REAL one even when the link is
    // submitted fully transparent, or the wire would be hard to grab -- see
    // GraphWire.hpp's channel note for why the visible curve is hand-drawn.
    inline constexpr float kGraphWireThickness = 2.0f;
    // A pin dot's tessellation and ring weight. The RADIUS is deliberately NOT
    // here: it is the one pin value the two canvases genuinely disagree about
    // (4.0 on the shader canvas, 4.5 for spec §11.2's 9px on the Graph lens),
    // so it stays a parameter at the call.
    inline constexpr int   kGraphPinSegments  = 12;
    inline constexpr float kGraphPinRingWidth = 1.6f;

    // ---- Grid palette -----------------------------------------------------
    // Display-referred RGBA (ImGui draws post-tonemap, imgui.hlsl:1-5). The
    // alphas are each octave's peak strength, not image opacity -- the backdrop
    // itself is always written opaque (GraphGridPhase.hpp, GraphGridColors).
    //
    // These two were byte-identical in both files, but shared BY ACCIDENT: the
    // Graph lens's own comment recorded them as "NOT covered by either ruling,
    // so NOT changed", i.e. inherited rather than chosen, with nothing policing
    // the drift. One definition is the whole fix.
    inline constexpr ImVec4 kGraphGridMinorColor = ImVec4(0.180f, 0.180f, 0.196f, 0.55f);
    inline constexpr ImVec4 kGraphGridMajorColor = ImVec4(0.235f, 0.235f, 0.255f, 0.90f);

    // ---- Selection / hover accents ---------------------------------------
    // The editor-wide outline language, so one accent means "selected"
    // everywhere: the viewport outline composite's kSelectColor/kHoverColor
    // (ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:101).
    //
    // Selection IS Theme::kAmber -- exactly, to the last digit -- and
    // EditorTheme.hpp:106-110 already names "the shader graph's selected-node
    // border" among that token's own citations. Both files re-spelled the token
    // as a literal; this spends it where it was authored to be spent. Hover
    // cyan has no theme token (it is canvas-only language), so it lives here.
    inline constexpr ImVec4 kGraphNodeSelBorderColor = Theme::kAmber;
    inline constexpr ImVec4 kGraphNodeHovBorderColor = ImVec4(0.25f, 0.70f, 1.0f, 1.0f);
}
