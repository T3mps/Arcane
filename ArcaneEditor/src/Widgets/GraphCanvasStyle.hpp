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
// (CanvasPopupScope.hpp:16-19 states the rule). This header IS node-editor
// coupled -- it includes imgui_node_editor.h below, because ApplyGraphCanvasStyle
// writes ed::GetStyle() -- which is exactly why it lives in that family and not
// in EditorWidgets.

#include "Widgets/EditorTheme.hpp"

#include <imgui.h>
#include <imgui_node_editor.h>

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

    namespace ed = ax::NodeEditor;

    // ---- The style application, and what a canvas may differ on -----------
    //
    // THE POINT OF THIS STRUCT is that the two canvases' style blocks wrote the
    // same ed::Style fields in the same order and differed almost entirely in
    // VALUES, with nothing tying the structure together. This applier writes 15
    // of them: 10 colours (Grid, Bg, NodeBg, NodeBorder, HovNodeBorder,
    // SelNodeBorder, GroupBg, GroupBorder, PinRect, PinRectBorder) and 5 scalars
    // (NodeRounding, NodeBorderWidth, HoveredNodeBorderWidth,
    // SelectedNodeBorderWidth, NodePadding).
    //
    // "Almost" is exact, not hedging: the shader canvas's old block wrote all 15,
    // the Graph lens's wrote 13 -- it omitted the GroupBg/GroupBorder pair. The
    // shared applier writes them for both, which for the Graph lens means
    // Theme::kNone into two entries it never reads (it creates no group nodes),
    // and is therefore a state change with no drawing consequence. That one
    // asymmetry is the whole of the structural difference; everything else was
    // field-for-field the same.
    //
    // A desc preserves the value divergence BY CONSTRUCTION, which is strictly
    // better than the old arrangement where a structural change (a new style
    // field, a reordering) could land on one canvas and silently not the other.
    //
    // The two divergences are RULINGS, not accidents, and are named fields
    // rather than defaults so a caller has to state its side:
    //
    //   nodeBody / nodeBorder -- the surface tones. Controller ruling
    //     2026-09-08 (written out at AssetsPanel.cpp's canvas-palette block):
    //     the OptionD board is the redline for the Assets panel's Graph lens,
    //     and the ruling explicitly DECLINES to drag the shader canvas onto it
    //     -- that canvas has its own board, its own review history and no such
    //     ruling. Unifying these re-opens a settled decision.
    //   nodePadding -- the shader canvas measures its nodes from their content
    //     and pads them; the Graph lens lays every row out by hand so its 24px
    //     header band and total height are EXACT, which needs zero padding
    //     (with none, a node's content origin IS ed::GetNodePosition, which is
    //     what lets the pin geometry be computed with no frame of readback
    //     lag). This is the one style field whose divergence is STRUCTURAL
    //     rather than cosmetic.
    //
    // Everything else defaults to the editor-wide language above, so a canvas
    // that says nothing gets the shared answer.
    struct GraphCanvasStyleDesc
    {
        // No sane default: a canvas that leaves these unset draws invisible
        // nodes, which is the intended tell.
        ImVec4 nodeBody   = Theme::kNone;
        ImVec4 nodeBorder = Theme::kNone;

        // Group (comment box) nodes. Transparent by default because a canvas
        // that never creates one -- the Graph lens does not -- has nothing to
        // paint here.
        ImVec4 groupBg     = Theme::kNone;
        ImVec4 groupBorder = Theme::kNone;

        // left, top, right, bottom, in canvas units.
        ImVec4 nodePadding = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

        ImVec4 hovBorder = kGraphNodeHovBorderColor;
        ImVec4 selBorder = kGraphNodeSelBorderColor;
        float  rounding       = kGraphNodeRounding;
        float  borderWidth    = kGraphNodeBorderWidth;
        float  hovBorderWidth = kGraphNodeHovBorderWidth;
        float  selBorderWidth = kGraphNodeSelBorderWidth;
    };

    // One-time style for a node-editor context. Written to the PERSISTENT
    // style (ed::GetStyle returns a mutable reference,
    // imgui_node_editor.h:295) instead of pushed per frame, because every
    // value here is latched into the object at BeginNode/BeginPin time
    // (imgui_node_editor.cpp:5270-5278, 5367-5377) -- one assignment covers
    // every node for the context's life. Call it with the context CURRENT,
    // right after ed::CreateEditor.
    inline void ApplyGraphCanvasStyle(const GraphCanvasStyleDesc& desc)
    {
        ed::Style& s = ed::GetStyle();
        // The vendored grid AND background fill are switched off; our own
        // lattice is drawn underneath instead -- DrawGraphGridFallback
        // (Widgets/GraphGridPhase.hpp), through DrawGraphCanvasBackdrop.
        // Wholesale replacement is the only option available: the built-in
        // grid's 32 px spacing is a hardcoded local with no StyleVar and no LOD
        // fade (imgui_node_editor.cpp:1506-1517).
        s.Colors[ed::StyleColor_Grid] = Theme::kNone;
        s.Colors[ed::StyleColor_Bg]   = Theme::kNone;
        s.Colors[ed::StyleColor_NodeBg]        = desc.nodeBody;
        s.Colors[ed::StyleColor_NodeBorder]    = desc.nodeBorder;
        s.Colors[ed::StyleColor_HovNodeBorder] = desc.hovBorder;
        s.Colors[ed::StyleColor_SelNodeBorder] = desc.selBorder;
        s.Colors[ed::StyleColor_GroupBg]       = desc.groupBg;
        s.Colors[ed::StyleColor_GroupBorder]   = desc.groupBorder;
        // A pin draws nothing of its own except a hover rect
        // (imgui_node_editor.cpp:575-594) -- that rectangle would fight the
        // dot, so its alpha goes to zero and the dot IS the pin visual.
        s.Colors[ed::StyleColor_PinRect]       = Theme::kNone;
        s.Colors[ed::StyleColor_PinRectBorder] = Theme::kNone;
        s.NodeRounding            = desc.rounding;
        s.NodeBorderWidth         = desc.borderWidth;
        s.HoveredNodeBorderWidth  = desc.hovBorderWidth;
        s.SelectedNodeBorderWidth = desc.selBorderWidth;
        s.NodePadding             = desc.nodePadding;
    }
}
