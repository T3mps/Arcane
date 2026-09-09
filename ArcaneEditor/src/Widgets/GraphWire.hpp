#pragma once

// Graph-canvas WIRE PAINT -- the curve math, the colour math and the one
// channel a hand-drawn wire is allowed to live in, with one definition each.
//
// Every function here is pure paint: no document, no model, no schema. That is
// the line the 2026-07-24 graph-framework directive draws -- the framework half
// (schema, node/pin/link model, serialization, gesture undo, badges, create
// menu) stays DEFERRED until a second real consumer drives its design, while
// "ALL visual polish" is exactly the material to keep cleanly separated now.
// These primitives were duplicated verbatim between ShaderEditorDocument.cpp
// and AssetsPanel.cpp's Graph lens; hoisting them here is that separation.
//
// Its own header, sibling to CanvasPopupScope.hpp / GraphZoomLevels.hpp, and
// NOT part of EditorWidgets.hpp/.cpp: that vocabulary is imgui.h plus
// Astra::Range only, and pulling imgui_node_editor.h into it would couple every
// editor widget to the node editor (CanvasPopupScope.hpp:16-19).

#include <imgui.h>
#include <imgui_node_editor.h>

namespace Arcane::Editor
{
    namespace ed = ax::NodeEditor;

    // ---- The links channel -----------------------------------------------
    // c_LinkChannel_Links, reproduced. THE WHOLE TWO-LAYER TECHNIQUE, once:
    //
    // ed::Link paints one flat colour, so a wire cannot say "this end is a
    // float, that end is a float2" the way its two dots do. Both canvases
    // therefore submit the link with a FULLY TRANSPARENT colour and draw the
    // curve themselves in the library's own link layer.
    //
    // Alpha 0 costs nothing and breaks nothing. The draw helper returns
    // immediately on `if ((color >> 24) == 0)` (imgui_node_editor.cpp:494-495),
    // so the flat wire is never tessellated. Registration ignores the colour
    // entirely -- DoLink stores it and calls UpdateEndpoints unconditionally
    // (:1648-1653) -- and every hit path (Link::TestHit :984-1032, FindLinkAt
    // :2240-2247) reads only the geometry and m_Thickness. So hover, selection,
    // rect-select and the delete flow are untouched, and the thickness passed
    // to ed::Link still has to be the REAL one or the wire would be hard to
    // grab (kGraphWireThickness, GraphCanvasStyle.hpp).
    //
    // Hover/selection feedback also survives on its own: those passes use
    // StyleColor_HovLinkBorder / StyleColor_SelLinkBorder, not the link's
    // colour (:899-929), and land in c_LinkChannel_Selection, one channel BELOW
    // the links -- so they stay a halo behind the hand-drawn gradient exactly
    // as they were behind the flat wire.
    //
    // The constant itself is a file-static in the vendored translation unit
    // (:130-131), so it cannot be named from here; it is reproduced from the
    // constants it is built out of (:113-121). Reproduced rather than guessed:
    // c_UserLayerChannelStart(0) + c_UserLayersCount(5) =
    // c_BackgroundChannelStart(5), + c_BackgroundChannelCount(1) =
    // c_LinkStartChannel(6), + 1 = 7.
    //
    // Retargeting the channel is not optional. Between ed::Begin and ed::End
    // but outside a node, the current channel is m_ExternalChannel (:1191-1194),
    // which is 0 -- the BOTTOM of the merge, under the grid's own opaque
    // background fill (:1512). Wires drawn there would simply be painted over.
    // The other reachable layer, GetNodeBackgroundDrawList, is a per-node
    // channel and sits ABOVE the links, so wires would cross in front of node
    // bodies. Channel 7 is the only one that puts them where the flat wires
    // were: above group nodes, below regular nodes (the End reshuffle keeps the
    // four link channels contiguous and in order, :1488-1492).
    //
    // The index is only meaningful DURING submission -- End swaps the channels
    // into their final z-order -- so a caller must be inside ed::Begin/End.
    inline constexpr int kGraphLinkChannel = 7;

    // ---- Curve + colour math ---------------------------------------------

    // Cubic bezier at t. Same evaluation the library tessellates (ImCubicBezier*
    // in imgui_bezier_math.inl); a hand-drawn wire needs per-segment points
    // because the colour changes along the curve.
    inline ImVec2 GraphCubicBezierAt(const ImVec2& p0, const ImVec2& p1,
                                     const ImVec2& p2, const ImVec2& p3, float t) noexcept
    {
        const float u = 1.0f - t;
        const float w0 = u * u * u;
        const float w1 = 3.0f * u * u * t;
        const float w2 = 3.0f * u * t * t;
        const float w3 = t * t * t;
        return ImVec2(p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                      p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3);
    }

    // Straight sRGB lerp. The pin palettes on both canvases are light,
    // low-saturation tones, so the midpoints stay clean without an OkLab
    // detour; the one pairing that could band (azure -> magenta) crosses
    // through a plausible lavender rather than through grey.
    inline ImVec4 GraphLerpColor(const ImVec4& a, const ImVec4& b, float t) noexcept
    {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }

    // A quarter of the way to white, alpha untouched. Hover/selection already
    // reads through the library's halo (see kGraphLinkChannel); this lifts the
    // wire itself the same way a highlighted dot lifts, so the emphasis lands
    // on the whole run.
    inline ImVec4 GraphBrightenColor(const ImVec4& c) noexcept
    {
        return GraphLerpColor(c, ImVec4(1.0f, 1.0f, 1.0f, c.w), 0.25f);
    }

    // The canvas's view scale, in the same units as a zoom stop
    // (GraphZoomLevels.hpp, kZoomLevels). THE TRAP: ed::GetCurrentZoom returns
    // InvScale -- canvas units per screen pixel
    // (imgui_node_editor_api.cpp:665-668) -- which is the RECIPROCAL of the
    // scale everything else means by "zoom". One helper, so the flip is written
    // once.
    //
    // Valid on either side of ed::Begin within a frame: Begin installs the view
    // the previous End computed (imgui_node_editor.cpp:1258) and the navigate
    // action only re-derives it during End, so both reads return the scale this
    // frame's nodes are actually drawn at.
    inline float GraphViewScale() noexcept
    {
        const float invScale = ed::GetCurrentZoom();
        return invScale > 0.0001f ? 1.0f / invScale : 1.0f;
    }
}
