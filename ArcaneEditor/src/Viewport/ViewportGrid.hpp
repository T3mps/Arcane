#pragma once

// The editor viewport's 2D reference grid (F4 plan 1 T9, spec s5.1): world
// XY-plane lines drawn as overlay lines through the view transform at the
// BOTTOM batcher layer, so sprites paint over it.
//
// Two halves, split the same way as PhysicsOverlay / EditorCamera:
//
//   PlanGrid2D  -- PURE. Which decade levels (0.1, 1, 10, 100 m ...) are
//                  visible at a given zoom and how strong each is. Nothing but
//                  arithmetic on pixels-per-metre; the [editor][grid] units
//                  drive it directly.
//   DrawGrid2D  -- the lines. Derives the visible world rect from the view
//                  (ScreenToRay at the viewport corners -- never a fixed line
//                  count), walks every multiple of each level's spacing inside
//                  it, and projects each line back through WorldToScreen into
//                  Batcher2D::Line's screen pixels. The axes go last, in
//                  colour. Orthographic views only: the 3D grid is Task 10's
//                  (an analytic, depth-tested GridNode), not a line list.
//
// THE LOD RULE (binding, brief T9). Candidate spacings are the decades 10^k
// metres. A level is IN when its screen spacing s = spacing * ppm reaches
// kGridFadeInPx, and it ramps to full strength by kGridFadeFullPx:
//
//     t     = clamp((s - 8) / (24 - 8), 0, 1)
//     alpha = t * 0.35                            (a minor level)
//
// Only the three finest qualifying levels are kept: the finest (the MINOR,
// the one that may still be fading in), the decade above it (the MAJOR --
// "a major line every ten minors", spec s5.1, ruling L-b -- at 0.55), and
// the decade above that (also 0.55: its lines are the major's every tenth,
// drawn once). UE's editor grid toggles levels on and off at a threshold; the
// ramp is the improvement spec s5.1 asks for, and the major's promotion
// rides the same ramp so nothing pops at the 8 px crossing either (ruling
// L-a): major alpha = 0.35 + t_finest * (0.55 - 0.35), exactly 0.55 once the
// finest level is fully in. Steady state is therefore 0.35 / 0.55 / 0.55.
//
// The grid unit is the METRE and nothing else: it is independent of the
// gizmo's snap (R7) and of the scene's content.
//
// Each line is drawn ONCE: a finer level skips the lines a coarser level in
// the plan also owns (every tenth), and x = 0 / y = 0 are never grey -- they
// are the axes, drawn after every level in their own colours (X red, Y green).
// The coarsest level's own every-tenth line -- the decade the plan just
// dropped when a finer one qualified -- is held at the major strength, so the
// crossing moves no line discontinuously from either side (ViewportGrid.cpp).

#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>

#include <array>

namespace Arcane
{
    class Batcher2D;
}

namespace Arcane::Editor
{
    // The crossfade window, in screen pixels of a level's spacing.
    inline constexpr float kGridFadeInPx   = 8.0f;
    inline constexpr float kGridFadeFullPx = 24.0f;

    // Peak strengths: a fully faded-in minor level, and the major level.
    inline constexpr float kGridMinorAlpha = 0.35f;
    inline constexpr float kGridMajorAlpha = 0.55f;

    // Level lines are neutral grey at the level's alpha; the axes are full
    // colour on top.
    inline constexpr glm::vec3 kGridLineRgb    { 0.5f, 0.5f, 0.5f };
    inline constexpr glm::vec4 kGridAxisXColor { 0.85f, 0.25f, 0.25f, 0.9f };
    inline constexpr glm::vec4 kGridAxisYColor { 0.3f,  0.8f,  0.3f,  0.9f };

    inline constexpr float kGridLineThicknessPx = 1.0f;

    struct GridLevel
    {
        float spacingMetres = 0.0f;
        float alpha         = 0.0f;   // in [0, 1]; 0 only at the exact fade-in edge
    };

    // Finest first; levels[i + 1] is ten times levels[i]. Only the first
    // `count` entries are meaningful.
    struct Grid2DPlan
    {
        std::array<GridLevel, 3> levels{};
        int count = 0;
    };

    // The pixels-per-metre of an orthographic XY view (|scale.x| of its
    // Affine2D); 0 for a perspective view, a tilted orthographic one, or a
    // view with no viewport, so PlanGrid2D of it is the empty plan.
    [[nodiscard]] float PixelsPerMetre(const Arcane::ViewTransform& view) noexcept;

    // PURE: the rule in the file comment. A non-positive or non-finite ppm
    // yields count 0.
    [[nodiscard]] Grid2DPlan PlanGrid2D(float pixelsPerMetre) noexcept;

    // Overlay lines at batcher layer (0, 0) -- SetLayer(0, 0) first, and left
    // at (0, 0) after (the batcher resets per Begin anyway; the caller is the
    // first submitter in the bracket). Returns without a draw when the view
    // is not orthographic, has no viewport, or the plan is empty.
    void DrawGrid2D(Arcane::Batcher2D& b, const Arcane::ViewTransform& view, const Grid2DPlan& plan);
}
