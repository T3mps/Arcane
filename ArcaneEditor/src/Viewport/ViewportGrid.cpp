// The editor viewport's 2D reference grid (F4 plan 1 T9, spec s5.1). The
// rule and the split are in the header; this file is the arithmetic.

#include "Viewport/ViewportGrid.hpp"

#include <Arcane/Render/Batcher2D.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Arcane::Editor
{
    namespace
    {
        // The decade range PlanGrid2D searches. Nothing an editor viewport can
        // show falls outside 1e-9 .. 1e12 m; the bounds just keep the scan
        // finite for an absurd ppm (count < 3 only at those extremes).
        constexpr int kMinDecade = -9;
        constexpr int kMaxDecade = 12;

        // 10^k as the nearest float, from EXACT integer powers: a positive
        // decade is an exact integer, a negative one is a single correctly
        // rounded division of two exact values. std::pow(10, k) can land a
        // ulp under 0.1 and put "0.1 m at 80 ppm" a hair below fade-in.
        [[nodiscard]] float Decade(int k) noexcept
        {
            float p = 1.0f;
            for (int i = 0; i < std::abs(k); ++i) p *= 10.0f;
            return k < 0 ? 1.0f / p : p;
        }

        // The crossfade: 0 at kGridFadeInPx, 1 at kGridFadeFullPx, clamped.
        [[nodiscard]] float Ramp(float screenSpacingPx) noexcept
        {
            return std::clamp((screenSpacingPx - kGridFadeInPx) / (kGridFadeFullPx - kGridFadeInPx), 0.0f, 1.0f);
        }

        // Too many lines for one level is a plan the viewport could never have
        // produced (a level at >= 8 px spacing has at most width / 8 lines):
        // the cap only guards a hand-built plan against a runaway loop.
        constexpr std::int64_t kMaxLinesPerAxis = 1 << 14;

        // The inclusive index range [i0, i1] of the multiples of `spacing`
        // inside [lo, hi]. A multiple that sits ON the edge counts (the edge
        // rule Ruling G pins); the epsilon absorbs the inverse-matrix noise
        // that would otherwise put an exact edge at 1.9999998 and drop it.
        struct IndexRange
        {
            std::int64_t first = 0, last = -1;   // empty when last < first
        };

        [[nodiscard]] IndexRange MultiplesInside(float lo, float hi, float spacing) noexcept
        {
            const float eps = spacing * 1e-4f;
            IndexRange r;
            r.first = static_cast<std::int64_t>(std::ceil((lo - eps) / spacing));
            r.last  = static_cast<std::int64_t>(std::floor((hi + eps) / spacing));
            return r;
        }

        [[nodiscard]] bool Finite2(glm::vec3 p) noexcept
        {
            return std::isfinite(p.x) && std::isfinite(p.y);
        }
    }

    float PixelsPerMetre(const Arcane::ViewTransform& view) noexcept
    {
        const std::optional<Arcane::Affine2D> a = view.AsAffine2D();
        if (!a) return 0.0f;
        const float ppm = std::abs(a->scale.x);
        return std::isfinite(ppm) ? ppm : 0.0f;
    }

    Grid2DPlan PlanGrid2D(float pixelsPerMetre) noexcept
    {
        Grid2DPlan plan;
        if (!(pixelsPerMetre > 0.0f) || !std::isfinite(pixelsPerMetre))
            return plan;   // degenerate: nothing

        // The finest decade whose screen spacing reaches fade-in, then the two
        // above it. Searching upward from the smallest decade keeps this a
        // straight scan; log10 would do the same in one step but rounds at
        // exactly the threshold, and the threshold is where the test lives.
        int finest = kMaxDecade + 1;
        for (int k = kMinDecade; k <= kMaxDecade; ++k)
        {
            const float spacing = Decade(k);
            if (spacing * pixelsPerMetre >= kGridFadeInPx)
            {
                finest = k;
                break;
            }
        }
        if (finest > kMaxDecade)
            return plan;

        // The finest level's ramp is THE crossfade parameter: the finest level
        // fades in with it, and the major (two decades up) is promoted from a
        // minor's 0.35 to the major's 0.55 with the same t, so the 8 px crossing
        // changes no line's strength discontinuously.
        const float tFinest = Ramp(Decade(finest) * pixelsPerMetre);

        for (int i = 0; i < 3 && finest + i <= kMaxDecade; ++i)
        {
            const float spacing = Decade(finest + i);
            const float t       = Ramp(spacing * pixelsPerMetre);   // 1 for every level above the finest
            GridLevel& level    = plan.levels[static_cast<std::size_t>(i)];
            level.spacingMetres = spacing;
            level.alpha = (i == 2)
                ? kGridMinorAlpha + tFinest * (kGridMajorAlpha - kGridMinorAlpha)
                : t * kGridMinorAlpha;
            plan.count = i + 1;
        }
        return plan;
    }

    void DrawGrid2D(Arcane::Batcher2D& b, const Arcane::ViewTransform& view, const Grid2DPlan& plan)
    {
        if (!view.IsOrthographic()) return;   // the 3D grid is Task 10's GridNode
        if (view.viewport.x == 0u || view.viewport.y == 0u) return;
        if (plan.count <= 0) return;

        // The visible world rect: the four viewport corners unprojected onto
        // the near plane (parallel rays, so the near-plane point IS the
        // corner's world XY), boxed. Boxing all four rather than trusting two
        // keeps a rotated orthographic view honest -- its footprint's AABB is a
        // superset of the view, and lines past the edge are simply off-screen.
        const float W = static_cast<float>(view.viewport.x);
        const float H = static_cast<float>(view.viewport.y);
        const glm::vec2 corners[4] = { { 0.0f, 0.0f }, { W, 0.0f }, { 0.0f, H }, { W, H } };
        float minX = std::numeric_limits<float>::infinity(), maxX = -minX;
        float minY = minX, maxY = maxX;
        for (const glm::vec2 c : corners)
        {
            const glm::vec3 o = view.ScreenToRay(c).origin;
            if (!Finite2(o)) return;
            minX = std::min(minX, o.x); maxX = std::max(maxX, o.x);
            minY = std::min(minY, o.y); maxY = std::max(maxY, o.y);
        }
        if (!(maxX > minX) || !(maxY > minY)) return;

        // A world line to a screen line, through the view. The near-plane
        // depth ScreenToRay handed back is irrelevant to an orthographic
        // projection's XY, so z = 0 (the authoring plane) is what projects.
        const auto line = [&](glm::vec2 a, glm::vec2 wb, glm::vec4 colour)
        {
            const glm::vec3 sa = view.WorldToScreen(glm::vec3(a, 0.0f));
            const glm::vec3 sb = view.WorldToScreen(glm::vec3(wb, 0.0f));
            if (!Finite2(sa) || !Finite2(sb)) return;
            b.Line(glm::vec2(sa), glm::vec2(sb), kGridLineThicknessPx, colour);
        };

        b.SetLayer(0, 0);   // the BOTTOM: sprites paint over the grid

        const int count = std::min(plan.count, static_cast<int>(plan.levels.size()));
        for (int li = 0; li < count; ++li)
        {
            const GridLevel& level = plan.levels[static_cast<std::size_t>(li)];
            const float spacing    = level.spacingMetres;
            if (!(spacing > 0.0f) || !std::isfinite(spacing)) continue;
            if (!(level.alpha > 0.0f)) continue;   // exactly at fade-in: invisible, skip the work

            // A finer level leaves every tenth line to the coarser level above
            // it in the plan (its lines coincide), so each line draws once at
            // the strength of the coarsest level that owns it. The coarsest
            // level in the plan owns all of its lines -- and its OWN every
            // tenth line (the decade above the plan) is held at the major
            // strength: that decade WAS the plan's major one wheel tick ago,
            // before a finer level qualified and pushed it off the end, and
            // holding it keeps the 8 px crossing pop-free from both sides (the
            // new finest level fades in from 0, the promoted level ramps
            // 0.35 -> 0.55, and the dropped decade's lines never move). Once
            // the promotion completes the two strengths coincide.
            const bool hasCoarser    = li + 1 < count;
            const glm::vec4 colour   = glm::vec4(kGridLineRgb, level.alpha);
            const glm::vec4 heldMajor = glm::vec4(kGridLineRgb, std::max(level.alpha, kGridMajorAlpha));

            const IndexRange xs = MultiplesInside(minX, maxX, spacing);
            const IndexRange ys = MultiplesInside(minY, maxY, spacing);
            if (xs.last - xs.first > kMaxLinesPerAxis || ys.last - ys.first > kMaxLinesPerAxis) continue;

            for (std::int64_t i = xs.first; i <= xs.last; ++i)
            {
                if (i == 0) continue;                        // the Y axis, drawn last
                const bool tenth = i % 10 == 0;
                if (hasCoarser && tenth) continue;           // the coarser level's line
                const float x = static_cast<float>(i) * spacing;
                line({ x, minY }, { x, maxY }, tenth ? heldMajor : colour);
            }
            for (std::int64_t j = ys.first; j <= ys.last; ++j)
            {
                if (j == 0) continue;                        // the X axis, drawn last
                const bool tenth = j % 10 == 0;
                if (hasCoarser && tenth) continue;
                const float y = static_cast<float>(j) * spacing;
                line({ minX, y }, { maxX, y }, tenth ? heldMajor : colour);
            }
        }

        // The axes, last and in colour: the X axis is the line y = 0, the Y
        // axis the line x = 0 -- each only when the rect actually crosses it
        // (inclusive, same edge rule as the level lines).
        const float axisEps = 1e-6f * std::max(maxX - minX, maxY - minY);
        if (minY - axisEps <= 0.0f && 0.0f <= maxY + axisEps)
            line({ minX, 0.0f }, { maxX, 0.0f }, kGridAxisXColor);
        if (minX - axisEps <= 0.0f && 0.0f <= maxX + axisEps)
            line({ 0.0f, minY }, { 0.0f, maxY }, kGridAxisYColor);

        b.SetLayer(0, 0);   // left where Begin() put it, for the submitter after us
    }
}
