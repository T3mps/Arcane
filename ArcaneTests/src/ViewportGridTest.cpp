// Arcane Editor 2D grid (F4 plan 1 T9, spec s5.1): the PURE level plan
// (PlanGrid2D -- decade levels on the metre, chosen by screen spacing, with
// the 8..24 px crossfade) and the draw (DrawGrid2D -- overlay lines through
// the view transform, clipped to the visible world rect, the two axes on
// top). Driven headlessly with a recording Batcher2D double, same idiom as
// PhysicsDebugRichTest's RecMock ([editor][grid]).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Viewport/ViewportGrid.hpp>

using Catch::Approx;
using namespace Arcane::Editor;

namespace
{
    // Recording double: every Line() call is kept with its endpoints, colour
    // and the (layer, order) current at submission, so a test can count the
    // emitted lines, sort them into verticals / horizontals, and check the
    // axes' colours and their place in the submission order.
    struct RecLine
    {
        glm::vec2 a, b;
        glm::vec4 color;
        std::uint16_t layer, order;
    };

    struct RecMock final : Arcane::Batcher2D
    {
        std::vector<RecLine> lines;
        std::vector<std::pair<std::uint16_t, std::uint16_t>> layerCalls;
        std::uint16_t layer = 0, order = 0;

        void Begin(uint32_t, uint32_t) override {}
        void SetLayer(uint16_t l, uint16_t o) override
        {
            layer = l;
            order = o;
            layerCalls.emplace_back(l, o);
        }
        void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Line(glm::vec2 a, glm::vec2 b, float, glm::vec4 c) override
        {
            lines.push_back({ a, b, c, layer, order });
        }
        void Circle(glm::vec2, float, glm::vec4) override {}
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
        void QuadWorld(uint16_t, const Arcane::Guid&, const std::array<glm::vec3, 4>&,
                       glm::vec2, glm::vec2, glm::vec4) override {}
        void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
        void SetViewProjection(const glm::mat4&) override {}
    };

    bool IsVertical(const RecLine& l)   { return std::abs(l.a.x - l.b.x) < 1e-3f; }
    bool IsHorizontal(const RecLine& l) { return std::abs(l.a.y - l.b.y) < 1e-3f; }

    bool SameColor(glm::vec4 a, glm::vec4 b)
    {
        return std::abs(a.r - b.r) < 1e-5f && std::abs(a.g - b.g) < 1e-5f &&
               std::abs(a.b - b.b) < 1e-5f && std::abs(a.a - b.a) < 1e-5f;
    }
}

TEST_CASE("PlanGrid2D picks decade levels by screen spacing and crossfades between 8 and 24 px", "[editor][grid]")
{
    // 1 m = 100 px: 0.1 m (10 px, faint -- 2 px into the 16 px ramp), 1 m
    // (100 px, a full minor at 0.35), 10 m (the major).
    const Grid2DPlan a = PlanGrid2D(100.0f);
    REQUIRE(a.count == 3);
    CHECK(a.levels[0].spacingMetres == Approx(0.1f));
    CHECK(a.levels[0].alpha > 0.0f);
    CHECK(a.levels[0].alpha < 0.35f);
    CHECK(a.levels[0].alpha == Approx((10.0f - kGridFadeInPx) / (kGridFadeFullPx - kGridFadeInPx) * kGridMinorAlpha));
    CHECK(a.levels[1].spacingMetres == Approx(1.0f));
    CHECK(a.levels[1].alpha == Approx(kGridMinorAlpha));
    CHECK(a.levels[2].spacingMetres == Approx(10.0f));
    // The major's promotion rides the finest level's ramp (a crossfade too,
    // not a pop): at t = 0.125 it is 0.35 + 0.125 * (0.55 - 0.35).
    CHECK(a.levels[2].alpha == Approx(kGridMinorAlpha + 0.125f * (kGridMajorAlpha - kGridMinorAlpha)));
    // Levels are finest first, each ten times the last.
    CHECK(a.levels[1].spacingMetres / a.levels[0].spacingMetres == Approx(10.0f));
    CHECK(a.levels[2].spacingMetres / a.levels[1].spacingMetres == Approx(10.0f));

    // 1 m = 5 px: 1 m is below fade-in; 10 m (50 px, past the ramp) is the
    // finest, so the plan is 10 / 100 / 1000 m at 0.35 / 0.35 / 0.55.
    const Grid2DPlan b = PlanGrid2D(5.0f);
    REQUIRE(b.count == 3);
    CHECK(b.levels[0].spacingMetres == Approx(10.0f));
    CHECK(b.levels[0].alpha == Approx(kGridMinorAlpha));
    CHECK(b.levels[1].spacingMetres == Approx(100.0f));
    CHECK(b.levels[1].alpha == Approx(kGridMinorAlpha));
    CHECK(b.levels[2].spacingMetres == Approx(1000.0f));
    CHECK(b.levels[2].alpha == Approx(kGridMajorAlpha));

    // Exactly at fade-in (0.1 m at 8 px = 80 ppm) the level qualifies at
    // alpha 0 -- continuous with "not there" one pixel earlier, which is the
    // whole point of the ramp.
    const Grid2DPlan e = PlanGrid2D(80.0f);
    REQUIRE(e.count == 3);
    CHECK(e.levels[0].spacingMetres == Approx(0.1f));
    CHECK(e.levels[0].alpha == Approx(0.0f).margin(1e-6f));
    CHECK(e.levels[2].alpha == Approx(kGridMinorAlpha));   // the major is still 0.35 at t = 0: no pop on promotion
    // One pixel below: 0.1 m does not qualify, 1 m is the finest.
    const Grid2DPlan f = PlanGrid2D(79.0f);
    REQUIRE(f.count == 3);
    CHECK(f.levels[0].spacingMetres == Approx(1.0f));

    // Degenerate: nothing.
    CHECK(PlanGrid2D(0.0f).count == 0);
    CHECK(PlanGrid2D(-3.0f).count == 0);
    CHECK(PlanGrid2D(std::numeric_limits<float>::quiet_NaN()).count == 0);
    CHECK(PlanGrid2D(std::numeric_limits<float>::infinity()).count == 0);
}

TEST_CASE("PixelsPerMetre reads the orthographic view's scale and is 0 for perspective / no viewport", "[editor][grid]")
{
    // halfHeight 1.5 m over 300 px = 100 px per metre.
    const auto ortho = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 1.5f, glm::uvec2(400u, 300u));
    CHECK(PixelsPerMetre(ortho) == Approx(100.0f));
    const auto persp = Arcane::ViewTransform::Perspective(glm::vec3(0, 0, 10), glm::vec3(0), glm::vec3(0, 1, 0),
                                                          60.0f, glm::uvec2(400u, 300u), 0.1f, 100.0f);
    CHECK(PixelsPerMetre(persp) == 0.0f);
    CHECK(PixelsPerMetre(Arcane::ViewTransform{}) == 0.0f);
}

TEST_CASE("DrawGrid2D emits only lines inside the visible world rect, plus the two axes on top", "[editor][grid]")
{
    // Ruling G derivation. A 400 x 300 px view centred on the origin at 100
    // px per metre spans x in [-2, 2], y in [-1.5, 1.5] (world metres, +Y up).
    // A single 1 m level (the plan is hand-built so only that level draws):
    //   verticals   at every integer x in [-2, 2]:  -2, -1, 0, 1, 2  -> 5
    //   horizontals at every integer y in [-1.5, 1.5]: -1, 0, 1      -> 3
    // The x = 0 and y = 0 lines are NOT drawn as grey level lines: they are
    // the axes, drawn ONCE, after every level, in their own colours. So:
    //   grey verticals   -2, -1, 1, 2  -> 4   (edge rule: a line exactly ON
    //   grey horizontals -1, 1         -> 2    the rect edge counts, INCLUSIVE)
    //   axes             x = 0 (green), y = 0 (red) -> 2
    //   total 8.
    const auto view = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 1.5f, glm::uvec2(400u, 300u));
    Grid2DPlan plan;
    plan.levels[0] = { 1.0f, kGridMinorAlpha };
    plan.count     = 1;

    RecMock b;
    b.SetLayer(3, 7);   // something else was current: the grid must go to (0,0)
    DrawGrid2D(b, view, plan);

    REQUIRE(b.lines.size() == 8u);
    for (const RecLine& l : b.lines)
    {
        CHECK(l.layer == 0);
        CHECK(l.order == 0);
        CHECK(IsVertical(l) != IsHorizontal(l));   // axis-aligned, never both / neither
    }

    // The six grey level lines come first, in screen pixels: x = -2 m is
    // pixel 0, -1 m is 100, +1 m is 300, +2 m is 400; y = +1 m is pixel 50
    // (y down), -1 m is 250.
    std::vector<float> greyX, greyY;
    for (std::size_t i = 0; i < 6; ++i)
    {
        const RecLine& l = b.lines[i];
        CHECK(SameColor(l.color, glm::vec4(kGridLineRgb, kGridMinorAlpha)));
        if (IsVertical(l))
        {
            greyX.push_back(l.a.x);
            CHECK(std::min(l.a.y, l.b.y) == Approx(0.0f).margin(1e-3f));
            CHECK(std::max(l.a.y, l.b.y) == Approx(300.0f).margin(1e-3f));
        }
        else
        {
            greyY.push_back(l.a.y);
            CHECK(std::min(l.a.x, l.b.x) == Approx(0.0f).margin(1e-3f));
            CHECK(std::max(l.a.x, l.b.x) == Approx(400.0f).margin(1e-3f));
        }
    }
    std::sort(greyX.begin(), greyX.end());
    std::sort(greyY.begin(), greyY.end());
    REQUIRE(greyX.size() == 4u);
    REQUIRE(greyY.size() == 2u);
    CHECK(greyX[0] == Approx(0.0f).margin(1e-3f));
    CHECK(greyX[1] == Approx(100.0f).margin(1e-3f));
    CHECK(greyX[2] == Approx(300.0f).margin(1e-3f));
    CHECK(greyX[3] == Approx(400.0f).margin(1e-3f));
    CHECK(greyY[0] == Approx(50.0f).margin(1e-3f));
    CHECK(greyY[1] == Approx(250.0f).margin(1e-3f));

    // The axes are the LAST two lines, at full colour: X axis (y = 0, pixel
    // row 150) red, Y axis (x = 0, pixel column 200) green.
    const RecLine& xAxis = b.lines[6];
    const RecLine& yAxis = b.lines[7];
    CHECK(IsHorizontal(xAxis));
    CHECK(xAxis.a.y == Approx(150.0f).margin(1e-3f));
    CHECK(SameColor(xAxis.color, kGridAxisXColor));
    CHECK(SameColor(xAxis.color, glm::vec4(0.85f, 0.25f, 0.25f, 0.9f)));
    CHECK(IsVertical(yAxis));
    CHECK(yAxis.a.x == Approx(200.0f).margin(1e-3f));
    CHECK(SameColor(yAxis.color, kGridAxisYColor));
    CHECK(SameColor(yAxis.color, glm::vec4(0.3f, 0.8f, 0.3f, 0.9f)));

    // Layer discipline: the first thing the draw does is SetLayer(0, 0), and it
    // leaves (0, 0) current afterwards.
    REQUIRE(b.layerCalls.size() >= 2u);
    CHECK(b.layerCalls[1] == std::make_pair(std::uint16_t(0), std::uint16_t(0)));
    CHECK(b.layerCalls.back() == std::make_pair(std::uint16_t(0), std::uint16_t(0)));
}

TEST_CASE("DrawGrid2D: a view away from the origin draws no axes, and off-rect lines are never emitted", "[editor][grid]")
{
    // Centre (10.5, 20.5) m, same 4 x 3 m extent: x in [8.5, 12.5], y in
    // [19, 22]. 1 m level: verticals 9, 10, 11, 12 (4); horizontals 19, 20,
    // 21, 22 (4, both edges inclusive). No axis crosses the rect -> 8 grey
    // lines, no coloured ones. The coarsest level's every TENTH line (x = 10,
    // y = 20 -- the decade above the plan) is held at the major strength;
    // the other six are at the level's own alpha.
    const auto view = Arcane::ViewTransform::Orthographic(glm::vec2(10.5f, 20.5f), 1.5f, glm::uvec2(400u, 300u));
    Grid2DPlan plan;
    plan.levels[0] = { 1.0f, kGridMinorAlpha };
    plan.count     = 1;
    RecMock b;
    DrawGrid2D(b, view, plan);
    REQUIRE(b.lines.size() == 8u);
    int verticals = 0, minor = 0, held = 0;
    for (const RecLine& l : b.lines)
    {
        if (SameColor(l.color, glm::vec4(kGridLineRgb, kGridMinorAlpha))) ++minor;
        if (SameColor(l.color, glm::vec4(kGridLineRgb, kGridMajorAlpha)))
        {
            ++held;
            // x = 10 m is pixel 400 - (12.5 - 10) * 100 = 150; y = 20 m is
            // pixel (22 - 20) * 100 = 200 (y down).
            if (IsVertical(l)) CHECK(l.a.x == Approx(150.0f).margin(1e-3f));
            else               CHECK(l.a.y == Approx(200.0f).margin(1e-3f));
        }
        if (IsVertical(l)) ++verticals;
        // Every endpoint lies on the viewport's edge (inclusive).
        for (const glm::vec2 p : { l.a, l.b })
        {
            CHECK(p.x >= -1e-3f);
            CHECK(p.x <= 400.0f + 1e-3f);
            CHECK(p.y >= -1e-3f);
            CHECK(p.y <= 300.0f + 1e-3f);
        }
    }
    CHECK(verticals == 4);
    CHECK(minor == 6);
    CHECK(held == 2);
}

TEST_CASE("DrawGrid2D: the decade above the plan keeps its major strength across the 8 px crossing (no pop)", "[editor][grid]")
{
    // One wheel tick apart: at 79 ppm the plan is 1 / 10 / 100 m (100 m the
    // major, 0.55); at 80 ppm 0.1 m qualifies and the plan is 0.1 / 1 / 10 m,
    // with 10 m promoted from 0.35 and 100 m no longer in it. The line at
    // x = 100 m must read 0.55 on BOTH sides: it is the 100 m level's line
    // before, and the 10 m level's held every-tenth line after.
    //
    // View centred on (100, 0) m, 400 x 300 px: at 80 ppm x in [97.5, 102.5];
    // at 79 ppm x in [97.47, 102.53]. Either way x = 100 is the only
    // multiple of 10 (and of 100) inside, at pixel 200.
    for (const float ppm : { 79.0f, 80.0f })
    {
        const float halfHeight = 150.0f / ppm;
        const auto view = Arcane::ViewTransform::Orthographic(glm::vec2(100.0f, 0.0f), halfHeight, glm::uvec2(400u, 300u));
        REQUIRE(PixelsPerMetre(view) == Approx(ppm));
        RecMock b;
        DrawGrid2D(b, view, PlanGrid2D(ppm));
        int majorVerticals = 0;
        for (const RecLine& l : b.lines)
            if (IsVertical(l) && SameColor(l.color, glm::vec4(kGridLineRgb, kGridMajorAlpha)))
            {
                ++majorVerticals;
                CHECK(l.a.x == Approx(200.0f).margin(1e-2f));
            }
        INFO("ppm " << ppm);
        CHECK(majorVerticals == 1);
    }
}

TEST_CASE("DrawGrid2D: a finer level skips the lines a coarser level in the plan draws (each line once)", "[editor][grid]")
{
    // Two levels, 0.5 m and 5 m, over x in [-2, 2], y in [-1.5, 1.5]. The
    // 5 m level has no line inside the rect except the axes (x = 0 / y = 0,
    // which are the axes anyway). The 0.5 m level: verticals at -2, -1.5,
    // -1, -0.5, 0.5, 1, 1.5, 2 (8; x = 0 is the axis), horizontals at -1.5,
    // -1, -0.5, 0.5, 1, 1.5 (6; y = 0 is the axis). No 0.5 m line coincides
    // with a 5 m line inside the rect other than the axes, so the count is
    // 8 + 6 + 2 = 16.
    {
        const auto view = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 1.5f, glm::uvec2(400u, 300u));
        Grid2DPlan plan;
        plan.levels[0] = { 0.5f, 0.2f };
        plan.levels[1] = { 5.0f, kGridMajorAlpha };
        plan.count     = 2;
        RecMock b;
        DrawGrid2D(b, view, plan);
        CHECK(b.lines.size() == 16u);
    }
    // Shift the view so a 5 m line sits inside: centre (5, 0) -> x in [3, 7].
    // 0.5 m verticals: 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7 = 9 candidates, but
    // x = 5 is a 5 m line, so the fine level skips it and the coarse level
    // draws it once at the major alpha: 8 fine + 1 coarse. Horizontals as
    // before (6 fine, y = 0 axis). No Y axis (x = 0 is outside). Total
    // 8 + 1 + 6 + 1 = 16, with exactly one line at the major alpha.
    {
        const auto view = Arcane::ViewTransform::Orthographic(glm::vec2(5.0f, 0.0f), 1.5f, glm::uvec2(400u, 300u));
        Grid2DPlan plan;
        plan.levels[0] = { 0.5f, 0.2f };
        plan.levels[1] = { 5.0f, kGridMajorAlpha };
        plan.count     = 2;
        RecMock b;
        DrawGrid2D(b, view, plan);
        REQUIRE(b.lines.size() == 16u);
        int major = 0, red = 0, green = 0;
        for (const RecLine& l : b.lines)
        {
            if (SameColor(l.color, glm::vec4(kGridLineRgb, kGridMajorAlpha))) ++major;
            if (SameColor(l.color, kGridAxisXColor)) ++red;
            if (SameColor(l.color, kGridAxisYColor)) ++green;
        }
        CHECK(major == 1);
        CHECK(red == 1);
        CHECK(green == 0);
        // The single major line is the vertical at x = 5 m = pixel 200.
        for (const RecLine& l : b.lines)
            if (SameColor(l.color, glm::vec4(kGridLineRgb, kGridMajorAlpha)))
            {
                CHECK(IsVertical(l));
                CHECK(l.a.x == Approx(200.0f).margin(1e-3f));
            }
    }
}

TEST_CASE("DrawGrid2D draws nothing for a perspective view, an empty plan, or a zero viewport", "[editor][grid]")
{
    Grid2DPlan plan;
    plan.levels[0] = { 1.0f, kGridMinorAlpha };
    plan.count     = 1;

    const auto persp = Arcane::ViewTransform::Perspective(glm::vec3(0, 0, 10), glm::vec3(0), glm::vec3(0, 1, 0),
                                                          60.0f, glm::uvec2(400u, 300u), 0.1f, 100.0f);
    REQUIRE_FALSE(persp.IsOrthographic());
    {
        RecMock b;
        DrawGrid2D(b, persp, plan);
        CHECK(b.lines.empty());
    }
    const auto ortho = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 1.5f, glm::uvec2(400u, 300u));
    {
        RecMock b;
        DrawGrid2D(b, ortho, Grid2DPlan{});   // count 0
        CHECK(b.lines.empty());
    }
    {
        RecMock b;
        DrawGrid2D(b, Arcane::ViewTransform{}, plan);   // no viewport pushed
        CHECK(b.lines.empty());
    }
}
