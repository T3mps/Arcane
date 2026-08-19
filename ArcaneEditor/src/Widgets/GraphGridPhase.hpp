#pragma once

// GraphGridPhase: the shader-graph canvas backdrop's PURE half -- the view it
// is told about, the palette it is drawn in, and the phase state machine that
// makes a pan track the nodes 1:1 and a zoom grow out of the point the editor
// zoomed about.
//
// SPLIT OUT OF GraphGridPass.hpp AT NRI PHASE 3, TASK 11, when there were TWO
// consumers of this arithmetic -- the nvrhi shader pass (one fullscreen
// triangle) and the graph arm's ImGui-primitive fallback (AddLine) -- and one
// copy of the state machine was what made "both backdrops move identically" a
// fact rather than a hope.
//
// THE OTHER CONSUMER IS GONE (NRI Phase 5a, Task 9.5a): GraphGridPass could
// only be built with an nvrhi::IDevice and a ShaderLibrary, DocServices::device
// has been unconditionally null since Task 2b, and the class is deleted. Its
// DrawGraphGridFallback moved here verbatim (bottom of this file), so this
// header is now the WHOLE canvas grid rather than its pure half. It still has
// no device dependency, which is what lets a headless test drive it and what
// keeps it out of every TU that merely opens a document.
//
// Nothing here was rewritten -- constants, branches and wrap are verbatim from
// GraphGridPass's private half, as is the relocated drawing function.

#include <imgui.h>   // DrawGraphGridFallback draws the lattice with ImDrawList

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Arcane::Editor
{
    // The canvas transform, as the node editor reports it. This is RAW VIEW
    // STATE -- the grid's own phase is derived from the HISTORY of these, not
    // from any single one (see UpdatePhase).
    //
    // SCALE IS THE VISUAL SCALE (1 = 100%, 2 = zoomed in 2x). Note that
    // ed::GetCurrentZoom() returns the RECIPROCAL of this -- it hands back
    // CanvasView::InvScale, i.e. canvas units per screen pixel
    // (imgui_node_editor_api.cpp:665-668, imgui_canvas.h:70). Callers must
    // invert it before filling this in.
    //
    // originX/originY are the CANVAS-space coordinate under the region's
    // top-left corner (ed::ScreenToCanvas of that corner), unscaled.
    struct GraphGridView
    {
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        float originX = 0.0f;
        float originY = 0.0f;
        float scale   = 1.0f;
    };

    // Display-referred RGBA (ImGui draws post-tonemap, imgui.hlsl:1-5). The
    // minor/major alphas are the peak strength of each octave, not opacity of
    // the image -- the backdrop is always written opaque.
    struct GraphGridColors
    {
        float canvas[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float minor[4]  = { 0.0f, 0.0f, 0.0f, 1.0f };
        float major[4]  = { 0.0f, 0.0f, 0.0f, 1.0f };

        bool operator==(const GraphGridColors& o) const noexcept
        {
            return std::memcmp(this, &o, sizeof(GraphGridColors)) == 0;
        }
    };

    // ===================================================================
    // THE GRID'S WHOLE MEMORY, extracted (NRI Phase 3, Task 11)
    // ===================================================================
    // Lifted OUT of GraphGridPass verbatim -- same constants, same three
    // branches, same wrap -- because the ImGui fallback needs exactly this and
    // a second copy of a state machine is how two backdrops start disagreeing
    // about where the lattice is. It is PURE: no device, no ImGui, no NRI,
    // which also makes it the one piece of the grid a headless test can drive.
    //
    // The rationale for every line is at UpdatePhase's old site, kept below in
    // full; read it there.
    struct GraphGridPhase
    {
        // THESE FOUR ARE NOW FREE-STANDING -- NO MIRROR OBLIGATION.
        // They were MIRRORED CONSTANTS, pinned equal to graph_grid.hlsl's
        // kZoomExponent / kBaseSpacingPx / kMinorTargetPx / kMajorEvery, for as
        // long as a shader-rendered backdrop was the other consumer. That
        // consumer went at NRI Phase 5a Task 9.5a with GraphGridPass, and the
        // shader itself was deleted at the final-review fix wave once nothing
        // loaded it. There is no second copy left to drift against: the
        // ImGui-primitive fallback below is the ONLY reader, and these are
        // simply its tuning values. Change them freely -- but change them
        // TOGETHER with the band assertions in GraphGridPhaseTest.cpp, which
        // are what actually holds the design invariant now.
        //
        // WHY 0.7 (kept because the shader's rationale block was its only home):
        // screen period = kBaseSpacingPx * pow(zoom, k). At k = 1 the grid is
        // rigidly welded to canvas content; at k = 0 it ignores zoom entirely.
        // k = 0.7 makes the grid track zoom sublinearly, and over the editor's
        // 0.1-2.0 zoom table (ShaderEditorDocument.cpp, kZoomLevels) yields
        // 0.7*log2(20) = 3.0 LOD crossings. MORE CROSSINGS IS NOT A RISK: lines
        // at 2*pm are exactly every other line of pm for any k, so a crossing
        // can only ever fade out lines that are already redundant -- never a
        // pop. Nor does k change density, because the octave snap normalizes the
        // period at every zoom (see MinorPeriod), which is why kBaseSpacingPx
        // needed no compensating adjustment. Worked endpoints at k = 0.7:
        // pm = 16.0 px at zoom 0.100, 20.0 px at 1.000, 16.2 px at 2.000 -- the
        // whole domain sits inside the design band.
        static constexpr float kZoomExponent  = 0.7f;
        // Grid spacing in canvas units at zoom = 1, before the octave LOD snap.
        static constexpr float kBaseSpacingPx = 20.0f;
        // The on-screen spacing the minor octave is driven TOWARD, so the
        // backdrop can neither collapse into mush nor dissolve into emptiness.
        static constexpr float kMinorTargetPx = 22.0f;
        // Major lines every N minor lines. Powers of two only: the LOD steps by
        // doubling, and a power-of-two ratio keeps majors landing exactly on
        // minor lines across a step (so majors never "slide" through the field).
        static constexpr float kMajorEvery    = 8.0f;

        // A view scale change below this (relative) is treated as no change at
        // all, which routes the update through the pure-pan branch. The zoom
        // branch divides by (scaleOld - scaleNew), so this is also what keeps
        // that division away from zero.
        static constexpr float kScaleEpsilon = 1e-4f;

        // The grid's own screen scale -- the sublinear answer to view zoom.
        static float GridScale(float viewScale) noexcept
        {
            return std::pow(viewScale > 1e-4f ? viewScale : 1e-4f, kZoomExponent);
        }

        // The snapped minor period, in screen pixels. This IS the LOD block now
        // -- it was a mirror of graph_grid.hlsl's ps_main until that shader was
        // deleted; always lands in (kMinorTargetPx/2, kMinorTargetPx].
        static float MinorPeriod(float gridScale) noexcept
        {
            const float basePeriod = kBaseSpacingPx * gridScale;
            const float level = std::log2(kMinorTargetPx /
                                          (basePeriod > 1e-4f ? basePeriod : 1e-4f));
            return basePeriod * std::exp2(std::floor(level));
        }

        // THE FIX for corner-anchored zoom, and the reason the phase is state.
        //
        // Because the screen period is pow(scale, k) with k < 1, the lattice
        // matches no fixed canvas-space grid -- so nothing about the view tells
        // us where it "should" sit, and computing the phase straight from the
        // view (as the first version did) silently pins the scale change at the
        // RT's top-left corner. Instead the phase is carried forward:
        //
        //   (a) PURE PAN. The content slides by -(dOrigin * scale); the phase is
        //       a screen-space point, so it slides by the identical amount and
        //       the grid tracks the nodes 1:1.
        //
        //   (b) ZOOM. Any step that changes the scale is, in screen space, a
        //       scale-about-a-point, and that point is derived from the two view
        //       states rather than read off the mouse -- which is what makes it
        //       equally correct for cursor zoom, keyboard zoom, and
        //       NavigateToContent/Selection (which pan AND zoom, and whose
        //       fixed point is nowhere near the cursor). The canvas point c held
        //       fixed by the step satisfies
        //           (c - Oold)*Zold == (c - Onew)*Znew
        //       =>  c = (Oold*Zold - Onew*Znew) / (Zold - Znew)
        //       and its screen position is F = (c - Oold)*Zold. The phase then
        //       scales about F by the GRID's own ratio Snew/Sold -- not the
        //       view's -- so the pattern grows out of the same point the editor
        //       zoomed about while still answering zoom sublinearly.
        void Update(const GraphGridView& v) noexcept
        {
            const float sNew = GridScale(v.scale);
            // Hoisted so the zoom branch's divisor is PROVABLY non-zero. It
            // always was -- the relative-epsilon test below catches an exactly
            // equal pair for any prevScale >= 0, including 0 -- but the
            // compiler cannot see through fabs and a multiply (MSVC C4723),
            // and a reader should not have to re-derive it either. An exactly
            // equal pair routes through the PAN branch, which is what it means.
            const float denom = prevScale - v.scale;
            if (!havePrevView)
            {
                // Seed. Any phase is legal (the lattice is virtual), so pick the
                // one with a statement attached: at 100% zoom, and only there,
                // the lines fall on canvas-space multiples of kBaseSpacingPx.
                x = -v.originX * sNew;
                y = -v.originY * sNew;
                havePrevView = true;
            }
            else if (denom == 0.0f ||
                     std::fabs(v.scale - prevScale) <= kScaleEpsilon * prevScale)
            {
                x -= (v.originX - prevOriginX) * v.scale;
                y -= (v.originY - prevOriginY) * v.scale;
            }
            else
            {
                const float zOld = prevScale, zNew = v.scale;
                const float cx = (prevOriginX * zOld - v.originX * zNew) / denom;
                const float cy = (prevOriginY * zOld - v.originY * zNew) / denom;
                const float fx = (cx - prevOriginX) * zOld;
                const float fy = (cy - prevOriginY) * zOld;
                const float r  = sNew / GridScale(prevScale);
                x = fx + (x - fx) * r;
                y = fy + (y - fy) * r;
            }
            prevOriginX = v.originX;
            prevOriginY = v.originY;
            prevScale   = v.scale;

            // Keep the phase bounded so a long session of panning cannot walk it
            // out to where float spacing swallows a line width. Wrapping by the
            // COARSEST lattice actually drawn (16 * pm -- see ps_main's four
            // LineCoverage calls) is invisible by construction: every drawn
            // period divides it, so all four lattices land exactly where they
            // were. Later LOD steps stay continuous regardless, because the
            // octave crossfade's subset argument does not depend on phase.
            const float wrap = MinorPeriod(sNew) * kMajorEvery * 2.0f;
            if (wrap > 0.0f)
            {
                x = std::fmod(x, wrap);
                y = std::fmod(y, wrap);
            }
        }

        // Grid phase (screen px, region-relative) plus the view it was last
        // carried forward from. This is the grid's whole memory.
        float x = 0.0f, y = 0.0f;
        float prevOriginX = 0.0f, prevOriginY = 0.0f;
        float prevScale = 1.0f;
        bool  havePrevView = false;
    };

    // =====================================================================
    // DrawGraphGridFallback -- THE canvas grid (relocated NRI Phase 5a, 9.5a)
    // =====================================================================
    // RELOCATED VERBATIM from GraphGridPass.hpp, which is deleted. It was
    // written at NRI Phase 3, Task 11 as the graph arm's stand-in for a
    // shader-rendered lattice; the shader pass needed an nvrhi::IDevice and a
    // ShaderLibrary, DocServices::device has been unconditionally null since
    // Phase 5a Task 2b, and Task 9.5a deleted the pass outright. So this is no
    // longer a fallback in anything but name -- it is the canvas grid.
    //
    // THE CHOICE MADE, AND WHY (carried over from the deleted header, because
    // it is still the reason this is primitives and not a render pass). Two
    // routes were available for the device-less arm: render the grid through a
    // tiny offscreen graph frame, or draw it with ImGui primitives. The grid is
    // CHROME -- a backdrop for a node canvas, never captured, never compared
    // against a golden, and its only job is to tell the eye that the canvas
    // panned. An offscreen graph context per canvas (and there are two per
    // document: the graph canvas and the pass canvas) is a whole RenderGraph,
    // command-buffer set, descriptor pool and graveyard lane, plus a
    // user-texture entry on the chrome backend and the invalidate obligations
    // that come with it -- all to draw straight lines. ImGui primitives draw
    // the same lines with no GPU object at all. So: PRIMITIVES.
    //
    // WHAT IT KEEPS, and it is the part that matters -- the MOTION. It runs
    // the phase state machine above, so the grid tracks the nodes 1:1 on a pan
    // and grows out of the editor's own zoom fixed point, answering zoom
    // sublinearly through the same pow(scale, 0.7) curve. Same two lattices,
    // same snapped period.
    //
    // WHAT IT LOSES, relative to the deleted shader and stated rather than
    // discovered: the four-octave crossfade (one octave is drawn, so a zoom
    // crosses LOD steps as a pop rather than a fade), the vignette, and the
    // shader's analytic anti-aliasing (ImGui's 1px lines are its own AA). And
    // it costs a few hundred AddLine calls per canvas per frame. That was the
    // price of the mode when there were two; it is now simply the price.
    //
    // The vendored node editor's OWN grid stays switched off through its public
    // style seam (StyleColor_Grid alpha 0) -- it is a fixed 32 px line pair
    // with no fade and no styling beyond one color
    // (imgui_node_editor.cpp:1498-1519), which is what this replaces.
    //
    // `min`/`size` are the canvas region in SCREEN coordinates; `phase` is the
    // caller's per-canvas state (one instance per canvas) and is ADVANCED by
    // this call.
    inline void DrawGraphGridFallback(ImDrawList* dl, ImVec2 min, ImVec2 size,
                                      const GraphGridView& view,
                                      const GraphGridColors& colors,
                                      GraphGridPhase& phase)
    {
        if (!dl || size.x <= 0.0f || size.y <= 0.0f)
            return;

        // Phase first, and unconditionally: it is a function of the view
        // HISTORY, so a frame that skipped it would leave a gap the next frame
        // reads as a jump.
        phase.Update(view);

        const auto pack = [](const float (&c)[4])
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
        };
        const ImVec2 maxPt(min.x + size.x, min.y + size.y);
        // Opaque backdrop, exactly as the shader writes it ("the backdrop is
        // always written opaque", GraphGridColors above).
        dl->AddRectFilled(min, maxPt,
                          ImGui::ColorConvertFloat4ToU32(
                              ImVec4(colors.canvas[0], colors.canvas[1], colors.canvas[2], 1.0f)));

        const float pm = GraphGridPhase::MinorPeriod(GraphGridPhase::GridScale(view.scale));
        if (!(pm > 0.5f))
            return;   // degenerate; a line every half pixel is a fill, not a grid
        const float pM = pm * GraphGridPhase::kMajorEvery;

        const ImU32 minorCol = pack(colors.minor);
        const ImU32 majorCol = pack(colors.major);

        // ONE octave, at the snapped period -- see WHAT IT LOSES above. Majors
        // are drawn over minors rather than instead of them, which is what the
        // shader's additive octaves do at a major line.
        const auto lattice = [&](float period, ImU32 col)
        {
            // Start at the first lattice point at or after the region's left
            // edge. fmod can return either sign, so it is normalized into
            // [0, period) before use.
            float ox = std::fmod(phase.x, period);
            if (ox > 0.0f) ox -= period;
            for (float sx = ox; sx <= size.x; sx += period)
                if (sx >= 0.0f)
                    dl->AddLine(ImVec2(min.x + sx, min.y), ImVec2(min.x + sx, maxPt.y), col);
            float oy = std::fmod(phase.y, period);
            if (oy > 0.0f) oy -= period;
            for (float sy = oy; sy <= size.y; sy += period)
                if (sy >= 0.0f)
                    dl->AddLine(ImVec2(min.x, min.y + sy), ImVec2(maxPt.x, min.y + sy), col);
        };
        lattice(pm, minorCol);
        lattice(pM, majorCol);
    }
}
