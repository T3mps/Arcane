#pragma once

// GraphGridPhase: the shader-graph canvas backdrop's PURE half -- the view it
// is told about, the palette it is drawn in, and the phase state machine that
// makes a pan track the nodes 1:1 and a zoom grow out of the point the editor
// zoomed about.
//
// SPLIT OUT OF GraphGridPass.hpp AT NRI PHASE 3, TASK 11, and the split is the
// point: that header reaches nvrhi and ImGui, and TWO consumers now need only
// this arithmetic -- the shader pass (which renders it in one fullscreen
// triangle) and the graph arm's ImGui-primitive fallback (which draws the same
// lattice with AddLine). Keeping the state machine in one place is what makes
// "both backdrops move identically" a fact rather than a hope; keeping it in a
// header with no device dependency is what lets a headless test drive it and
// what keeps it out of every TU that merely opens a document.
//
// Nothing here was rewritten -- constants, branches and wrap are verbatim from
// GraphGridPass's private half.

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
        // MIRRORED CONSTANTS. These four must stay equal to graph_grid.hlsl's
        // kZoomExponent / kBaseSpacingPx / kMinorTargetPx / kMajorEvery. They
        // are duplicated here rather than passed in because the phase update
        // has to know the grid's own screen scale and its coarsest period, and
        // both are the shader's arithmetic; same mirrored-constant arrangement
        // msdf.hlsl and TextSystem.cpp use for kPxRange/kAtlasSize.
        static constexpr float kZoomExponent  = 0.7f;   // see the shader's rationale block
        static constexpr float kBaseSpacingPx = 20.0f;
        static constexpr float kMinorTargetPx = 22.0f;
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

        // The snapped minor period, in screen pixels. Mirror of the LOD block
        // in graph_grid.hlsl's ps_main; always lands in (kMinorTargetPx/2, kMinorTargetPx].
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
}
