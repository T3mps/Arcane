// GraphGridPhase (NRI Phase 3, Task 11) -- the shader-graph canvas backdrop's
// PURE half, split out of GraphGridPass so the shader pass and the graph arm's
// ImGui-primitive fallback provably move the SAME lattice.
//
// It had no coverage at all before the split, because it was a private member
// of a class that cannot be constructed without an nvrhi device. It is the one
// part of the grid where a bug is a WRONG PICTURE rather than a missing one --
// a phase that does not track the view makes the backdrop swim under the nodes
// on every pan, which is exactly the defect the state machine was written to
// fix (a phase computed straight from the view pins a zoom at the region's
// top-left corner).
//
// What is NOT here: the shader's octave crossfade, its vignette and its
// anti-aliasing, none of which are in this struct.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Widgets/GraphGridPass.hpp"
#include "Widgets/GraphGridPhase.hpp"

#include <cmath>
#include <cstdint>

using Arcane::Editor::GraphGridPhase;
using Arcane::Editor::GraphGridView;
using Catch::Approx;

namespace
{
    GraphGridView View(float originX, float originY, float scale)
    {
        GraphGridView v;
        v.width = 800;
        v.height = 600;
        v.originX = originX;
        v.originY = originY;
        v.scale = scale;
        return v;
    }
}

TEST_CASE("graph grid phase: a PURE PAN slides the lattice with the content, 1:1",
          "[editor][material]")
{
    // The whole point of the pan branch: the phase is a SCREEN-space point, so
    // it must slide by exactly the amount the content slid, which is
    // -(dOrigin * scale). Anything else and the grid swims under the nodes.
    GraphGridPhase phase;
    phase.Update(View(0.0f, 0.0f, 1.0f));
    const float x0 = phase.x, y0 = phase.y;

    phase.Update(View(10.0f, -4.0f, 1.0f));
    CHECK(phase.x == Approx(x0 - 10.0f).margin(1e-3));
    CHECK(phase.y == Approx(y0 + 4.0f).margin(1e-3));

    // ...and at a different zoom the slide is scaled by it, because a canvas
    // unit is `scale` screen pixels.
    GraphGridPhase zoomed;
    zoomed.Update(View(0.0f, 0.0f, 2.0f));
    const float zx = zoomed.x;
    zoomed.Update(View(3.0f, 0.0f, 2.0f));
    CHECK(zoomed.x == Approx(zx - 6.0f).margin(1e-3));
}

TEST_CASE("graph grid phase: a ZOOM grows the lattice out of the view's own fixed point",
          "[editor][material]")
{
    // A zoom step is a scale-about-a-point in screen space, and the point is
    // DERIVED from the two view states rather than read off the mouse -- which
    // is what makes it right for cursor zoom, keyboard zoom AND
    // NavigateToContent (which pans and zooms at once).
    //
    // Construct a step whose fixed canvas point is known: hold canvas point
    // c = 40 stationary while the scale goes 1 -> 2. Screen position of c is
    // (c - O) * Z; for it to be unchanged, (40 - 0)*1 == (40 - Onew)*2, i.e.
    // Onew = 20.
    GraphGridPhase phase;
    phase.Update(View(0.0f, 0.0f, 1.0f));
    const float before = phase.x;

    // The fixed point's SCREEN position under the old view -- what the phase
    // must scale about.
    const float fixedScreenX = (40.0f - 0.0f) * 1.0f;
    const float ratio = GraphGridPhase::GridScale(2.0f) / GraphGridPhase::GridScale(1.0f);
    const float expected = fixedScreenX + (before - fixedScreenX) * ratio;

    phase.Update(View(20.0f, 0.0f, 2.0f));
    CHECK(phase.x == Approx(expected).margin(1e-2));

    // The ratio is the GRID's own, not the view's -- sublinear, so a 2x view
    // zoom grows the pattern by 2^0.7 (~1.62), not by 2.
    CHECK(ratio == Approx(std::pow(2.0f, GraphGridPhase::kZoomExponent)).margin(1e-4));
    CHECK(ratio < 2.0f);
}

TEST_CASE("graph grid phase: the snapped minor period always lands in its half-octave band",
          "[editor][material]")
{
    // Mirror of the LOD block in graph_grid.hlsl's ps_main: whatever the zoom,
    // the drawn period stays inside (kMinorTargetPx/2, kMinorTargetPx]. That
    // is what keeps the grid legible at every zoom instead of collapsing into
    // a fill or spreading into two lines on screen -- and it is the number the
    // ImGui fallback steps its AddLine loop by, so a period outside the band
    // is either a fill or an empty canvas there.
    for (float scale : { 0.05f, 0.2f, 0.5f, 1.0f, 1.7f, 4.0f, 12.0f })
    {
        const float pm = GraphGridPhase::MinorPeriod(GraphGridPhase::GridScale(scale));
        INFO("scale " << scale << " -> period " << pm);
        CHECK(pm > GraphGridPhase::kMinorTargetPx * 0.5f);
        CHECK(pm <= GraphGridPhase::kMinorTargetPx + 1e-3f);
    }
}

TEST_CASE("graph grid phase: a long pan stays bounded, and the wrap is invisible",
          "[editor][material]")
{
    // The phase is wrapped by the COARSEST lattice actually drawn so a long
    // session cannot walk it out to where float spacing swallows a line width.
    // The wrap is invisible by construction -- every drawn period divides it --
    // which is exactly what this checks: after wrapping, the phase is still
    // congruent to the unwrapped one modulo the minor period.
    GraphGridPhase phase;
    phase.Update(View(0.0f, 0.0f, 1.0f));

    float originX = 0.0f;
    for (int i = 0; i < 5000; ++i)
    {
        originX += 37.0f;
        phase.Update(View(originX, 0.0f, 1.0f));
    }

    const float pm = GraphGridPhase::MinorPeriod(GraphGridPhase::GridScale(1.0f));
    const float wrap = pm * GraphGridPhase::kMajorEvery * 2.0f;
    CHECK(std::fabs(phase.x) <= wrap);

    // Congruence: the total unwrapped slide is -(5000 * 37) from the seed at
    // origin 0 (seed phase is -0 * s = 0), so the wrapped phase must agree
    // with it modulo the coarsest drawn lattice.
    const float unwrapped = -(5000.0f * 37.0f);
    const float diff = std::fmod(std::fabs(phase.x - unwrapped), wrap);
    CHECK((diff < 1e-1f || std::fabs(diff - wrap) < 1e-1f));
}

// ===================================================================
// THE NULL-PASS SHAPES (NRI Phase 3, Task 11, fix round 1)
// ===================================================================
// Review found a null dereference I introduced: DrawCanvasBackdrop's
// `if (!grid || ...) return;` had been narrowed to an ARM test
// (`!grid && !m_services.device`), which lets a DEVICE-CARRYING run with a
// null pass fall through to `grid->Update(...)`. Restored to a plain
// `if (!grid)` return, with the fallback draw nested inside it.
//
// WHAT THIS CASE PINS, precisely: the PREMISE the crash depended on -- that
// `grid` can legitimately be null on a run that HAS a device, so the return
// cannot be folded into the arm test. It does not pin DrawCanvasBackdrop
// itself, which submits ImGui and node-editor calls from inside a live
// ed::Begin and cannot be driven headlessly; that guard is held by the
// comment block at the site and by review.
TEST_CASE("graph grid: Create refuses without EITHER of its two inputs",
          "[editor][material]")
{
    using Arcane::Editor::GraphGridPass;

    // Neither -- the graph arm, where DocServices carries no device and no
    // ShaderLibrary at all.
    CHECK(GraphGridPass::Create(nullptr, nullptr) == nullptr);

    // ...and the shape the Critical was about: a DEVICE but no ShaderLibrary.
    // Create's very first statement is `if (!device || !shaders) return
    // nullptr;`, so the pointer below is never dereferenced -- it exists only
    // to make "device present, pass absent" expressible without a real device.
    // That combination is exactly what a caller reaching `grid->Update()`
    // behind an arm-only gate would crash on.
    auto* fakeDevice = reinterpret_cast<nvrhi::IDevice*>(std::uintptr_t{ 0x1 });
    CHECK(GraphGridPass::Create(fakeDevice, nullptr) == nullptr);

    // The third shape a device-carrying run reaches -- Create() entered and
    // Init() failed on an nvrhi object -- needs a real (failing) device and is
    // desk/GPU territory. Named here so the inventory is complete: it lands on
    // the same `if (!grid) return;`.
}
