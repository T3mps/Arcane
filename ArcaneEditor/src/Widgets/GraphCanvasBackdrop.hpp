#pragma once

// The node canvas's backdrop, composed: read the view off the node editor,
// pack the palette, and draw the lattice.
//
// GraphGridPhase.hpp holds the pieces -- the view struct, the colour struct,
// the phase state machine and the lattice draw -- and holds them with NO device
// and NO node-editor dependency, which is what lets a device-less test drive
// the whole grid. That independence is the point of that header, so the
// composition that DOES need ed:: lives here instead, one step up, in the
// node-editor-coupled canvas family beside CanvasPopupScope.hpp.
//
// It was written out twice: once inside ShaderEditorDocument::DrawCanvasBackdrop
// and once inline in the Assets panel's Graph lens, differing only in which
// constants it named and which phase instance it advanced.

#include "Widgets/GraphGridPhase.hpp"
#include "Widgets/GraphWire.hpp"   // GraphViewScale -- the GetCurrentZoom reciprocal flip

#include <imgui.h>
#include <imgui_node_editor.h>

#include <cstdint>

namespace Arcane::Editor
{
    // CALL THIS BEFORE ed::Begin. Two separate reasons, both load-bearing:
    //
    //  (1) LAYERING. The node editor offers no public way to draw beneath its
    //      own background/grid layer -- everything it emits lands in channels
    //      the API does not expose, and the two it does expose (the per-node
    //      background draw list, the group-hint lists) sit ABOVE links. Blitting
    //      before ed::Begin puts this in the window draw list ahead of every
    //      channel the editor merges in afterwards.
    //  (2) COORDINATES. ed::ScreenToCanvas below means what it says only out
    //      here; inside ed::Begin/End the editor moves ImGui itself into canvas
    //      space (imgui_canvas.cpp:476-487).
    //
    // `canvasMin`/`canvasSize` are the canvas region in SCREEN coordinates, as
    // the caller measured them (both callers need them for other things too, so
    // they are passed rather than re-measured here). `phase` is the caller's
    // PER-CANVAS state -- a pan/zoom history -- and is ADVANCED by this call;
    // two canvases sharing one would hand each other the other's accumulated
    // phase every time the view switched, so each canvas owns its own.
    //
    // DISCLOSED CONSEQUENCE, unchanged by the hoist: the transform read here is
    // the one ed::Begin installed LAST frame. The editor computes the new view
    // in End() (imgui_node_editor.cpp:1357) and installs it in the next Begin()
    // (:1257), so a frame that is actively panning or zooming draws the backdrop
    // one frame behind the nodes. What that costs is CONTINUITY, not
    // correctness -- and continuity is the property that matters now that the
    // grid's phase is STATE. The pass is fed the same sequence of views, just
    // one frame late, so it accumulates the same phase; no error builds up over
    // a gesture, and the final view of a gesture arrives on the following frame,
    // so the grid settles onto its exact position without a jump. Only the
    // moving frames are offset. Reading it after ed::Begin would remove even
    // that, but there is no channel under the content to put the blit in; the
    // fix, if the lag ever reads badly, is to host the canvas in a child window
    // and blit into the PARENT's draw list after ed::End (parent draw lists
    // render first).
    inline void DrawGraphCanvasBackdrop(ImVec2 canvasMin, ImVec2 canvasSize,
                                        const ImVec4& canvasColor,
                                        const ImVec4& minorColor,
                                        const ImVec4& majorColor,
                                        GraphGridPhase& phase)
    {
        GraphGridView view;
        view.width  = static_cast<std::uint32_t>(canvasSize.x);
        view.height = static_cast<std::uint32_t>(canvasSize.y);
        // RAW view state only -- the grid derives its own phase from the HISTORY
        // of these, because a sublinearly-scaled lattice has no canvas-space
        // anchor to be read off any single frame (GraphGridPhase::Update).
        view.scale = GraphViewScale();   // owns the reciprocal flip
        const ImVec2 originCanvas = ed::ScreenToCanvas(canvasMin);
        view.originX = originCanvas.x;
        view.originY = originCanvas.y;

        const auto fill = [](float (&dst)[4], const ImVec4& c) noexcept
        {
            dst[0] = c.x;
            dst[1] = c.y;
            dst[2] = c.z;
            dst[3] = c.w;
        };
        GraphGridColors colors;
        fill(colors.canvas, canvasColor);
        fill(colors.minor,  minorColor);
        fill(colors.major,  majorColor);

        // ONE snapped period, two octaves, drawn with ImGui primitives. That is
        // a CHOICE rather than a stopgap -- the grid is chrome, and an offscreen
        // graph context per canvas would be a RenderGraph, a descriptor pool, a
        // graveyard lane and a chrome-side user-texture entry to invalidate, to
        // draw straight lines. DrawGraphGridFallback's own header states it.
        DrawGraphGridFallback(ImGui::GetWindowDrawList(), canvasMin, canvasSize,
                              view, colors, phase);
    }
}
