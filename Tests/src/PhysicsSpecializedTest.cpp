// Physics M6 P1.3: specialized circle/capsule narrowphase + round manifolds.
//
// PORT NOTE: GeometryKernel (circleCircle / circlePoly / capsulePoly and the
// closestSegSeg / pointInPoly / closestOnPolyBoundary / polyCentroid helpers)
// and Specialized (Manifold.lua's round paths: roundView + roundVsPoly +
// round-round + poly-vs-round) are faithful ports of the Lua physics engine.
//
// LAYER A (kernels) bit-matches the EXISTING oracle geometry.json (circlePoly,
// capsulePoly, circleCircle, closestSegSeg, pointInPoly entries captured P1.0).
// LAYER B (round manifolds) bit-matches the NEW oracle round_manifold.json
// (circle/poly, capsule/floor two-point, circle/circle, capsule/capsule, and
// poly/circle with the flipped normal). Pixel-scale coords, so an absolute
// margin of 1e-4 is the f32-vs-f64 allowance.

#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp>
#include <Arcane/Physics/Narrowphase/Specialized.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include "Helpers/PhysicsOracle.hpp"

using Catch::Approx;
using namespace Arcane::Physics;

namespace
{
    // f32-vs-f64 cross-precision absolute margin for pixel-scale coords.
    constexpr double kTol = 1e-4;

    // Flatten an oracle {..,"poly":[x0,y0,x1,y1,...]} array into world verts.
    std::vector<Vec2> LoadFlat(const nlohmann::json& flat)
    {
        std::vector<Vec2> v;
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2)
        {
            v.emplace_back(flat[i].get<Real>(), flat[i + 1].get<Real>());
        }
        return v;
    }

    // Build a Shape from a round_manifold descriptor (circle/capsule/polygon/aabb).
    Shape ShapeFromDesc(const nlohmann::json& d)
    {
        const std::string kind = d["kind"].get<std::string>();
        if (kind == "circle")  return MakeCircle(d["r"].get<Real>());
        if (kind == "capsule") return MakeCapsule(d["halfLen"].get<Real>(),
                                                  d["r"].get<Real>());
        if (kind == "aabb")    return MakeAabb(d["hw"].get<Real>(),
                                               d["hh"].get<Real>());
        // polygon
        return MakePolygon(LoadFlat(d["verts"]));
    }

    void CheckHit(const Hit& got, const nlohmann::json& expect)
    {
        CHECK(got.hit == expect["hit"].get<bool>());
        if (!expect["hit"].get<bool>()) return;
        CHECK(static_cast<double>(got.normal.x) ==
              Approx(expect["nx"].get<double>()).margin(kTol));
        CHECK(static_cast<double>(got.normal.y) ==
              Approx(expect["ny"].get<double>()).margin(kTol));
        CHECK(static_cast<double>(got.depth) ==
              Approx(expect["depth"].get<double>()).margin(kTol));
    }

    // Run one round_manifold oracle case (margin=0, faithful) through the
    // CollideShapes router and assert the manifold bit-matches index-aligned
    // (we reproduce the Lua emit order, so a same-index compare is strict).
    void CheckRoundCase(const nlohmann::json& cs)
    {
        const Shape sa = ShapeFromDesc(cs["shapeA"]);
        const Shape sb = ShapeFromDesc(cs["shapeB"]);
        const Transform xfA{ Vec2(cs["ax"].get<Real>(), cs["ay"].get<Real>()),
                             Real(0) };
        const Transform xfB{ Vec2(cs["bx"].get<Real>(), cs["by"].get<Real>()),
                             Real(0) };
        const auto keyBase = cs["keyBase"].get<std::uint32_t>();

        const Manifold m =
            CollideShapes(sa, xfA, sb, xfB, /*margin=*/Real(0), keyBase);

        const int expectCount = cs["count"].get<int>();
        REQUIRE(m.pointCount == expectCount);

        const auto& pts = cs["points"];
        for (int i = 0; i < expectCount; ++i)
        {
            const auto& row = pts[i];
            CHECK(static_cast<double>(m.normal.x) ==
                  Approx(row["nx"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(m.normal.y) ==
                  Approx(row["ny"].get<double>()).margin(kTol));

            const ManifoldPoint& p = m.points[i];
            CHECK(static_cast<double>(p.point.x) ==
                  Approx(row["x"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(p.point.y) ==
                  Approx(row["y"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(p.separation) ==
                  Approx(row["depth"].get<double>()).margin(kTol));
            CHECK(p.id == row["key"].get<std::uint32_t>());
        }
    }
} // namespace

// ====================================================================
// LAYER A -- pure geometry kernels bit-match geometry.json (P1.0 oracle).
//   circleCircle, circlePoly (clear/overlap/contained), capsulePoly
//   (overlap/clear), closestSegSeg (parallel/crossing), pointInPoly.
// ====================================================================
TEST_CASE("physics: round geometry kernels match the geometry oracle", "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("geometry");
    REQUIRE_FALSE(j.empty());

    // --- circleCircle -------------------------------------------------
    {
        const auto& c = j["circleCircle"];
        const Hit got = CircleCircle(
            Vec2(c["ax"].get<Real>(), c["ay"].get<Real>()), c["ar"].get<Real>(),
            Vec2(c["bx"].get<Real>(), c["by"].get<Real>()), c["br"].get<Real>());
        CheckHit(got, c["expect"]);
    }

    // --- circlePoly: clear / overlap / contained ----------------------
    {
        const auto& cp = j["circlePoly"];
        const std::vector<Vec2> poly = LoadFlat(cp["poly"]);
        const int n = static_cast<int>(poly.size());
        for (const char* key : { "clear_above", "overlap_top", "contained" })
        {
            const auto& cs = cp[key];
            const Hit got = CirclePoly(
                Vec2(cs["cx"].get<Real>(), cs["cy"].get<Real>()),
                cs["r"].get<Real>(), poly.data(), n);
            CheckHit(got, cs["expect"]);
        }
    }

    // --- capsulePoly: overlap_left / clear ----------------------------
    {
        const auto& cpp = j["capsulePoly"];
        const std::vector<Vec2> poly = LoadFlat(cpp["poly"]);
        const int n = static_cast<int>(poly.size());
        for (const char* key : { "overlap_left", "clear" })
        {
            const auto& cs = cpp[key];
            const Hit got = CapsulePoly(
                Vec2(cs["ax"].get<Real>(), cs["ay"].get<Real>()),
                Vec2(cs["bx"].get<Real>(), cs["by"].get<Real>()),
                cs["r"].get<Real>(), poly.data(), n);
            CheckHit(got, cs["expect"]);
        }
    }

    // --- closestSegSeg: parallel / crossing ---------------------------
    {
        const auto& ss = j["closestSegSeg"];
        for (const char* key : { "parallel", "crossing" })
        {
            const auto& cs = ss[key];
            const Vec2 p1(cs["p1"][0].get<Real>(), cs["p1"][1].get<Real>());
            const Vec2 p2(cs["p2"][0].get<Real>(), cs["p2"][1].get<Real>());
            const Vec2 q1(cs["q1"][0].get<Real>(), cs["q1"][1].get<Real>());
            const Vec2 q2(cs["q2"][0].get<Real>(), cs["q2"][1].get<Real>());
            const SegSeg got = ClosestSegSeg(p1, p2, q1, q2);
            const auto& ea = cs["expect"]["a"];
            const auto& eb = cs["expect"]["b"];
            CHECK(static_cast<double>(got.p.x) == Approx(ea[0].get<double>()).margin(kTol));
            CHECK(static_cast<double>(got.p.y) == Approx(ea[1].get<double>()).margin(kTol));
            CHECK(static_cast<double>(got.q.x) == Approx(eb[0].get<double>()).margin(kTol));
            CHECK(static_cast<double>(got.q.y) == Approx(eb[1].get<double>()).margin(kTol));
        }
    }

    // --- pointInPoly: center_inside / corner_outside ------------------
    {
        const auto& pip = j["pointInPoly"];
        const std::vector<Vec2> poly = LoadFlat(pip["poly"]);
        const int n = static_cast<int>(poly.size());
        {
            const auto& ci = pip["center_inside"];
            const Vec2 p(ci["p"][0].get<Real>(), ci["p"][1].get<Real>());
            CHECK(PointInPoly(p, poly.data(), n) == ci["expect"].get<bool>());
        }
        {
            const auto& co = pip["corner_outside"];
            const Vec2 p(co["p"][0].get<Real>(), co["p"][1].get<Real>());
            CHECK(PointInPoly(p, poly.data(), n) == co["expect"].get<bool>());
        }
    }
}

// ====================================================================
// LAYER B -- round manifolds (margin = 0) bit-match round_manifold.json.
//   circle/poly (1 pt), capsule/floor (TWO pts, the key 2-point case),
//   circle/circle (1 pt), capsule/capsule (deepest single), poly/circle
//   (1 pt, FLIPPED normal). Every normal/point/depth/key must match.
// ====================================================================
TEST_CASE("physics: CollideShapes matches the round manifold oracle", "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("round_manifold");
    REQUIRE_FALSE(j.empty());

    CheckRoundCase(j["circle_poly_overlap"]);
    CheckRoundCase(j["capsule_floor_two_points"]);
    CheckRoundCase(j["circle_circle"]);
    CheckRoundCase(j["capsule_capsule_parallel"]);
    CheckRoundCase(j["poly_circle_flipped"]);
}

// ====================================================================
// Speculative skin margin (MODERNIZATION). A circle clear of a poly by a small
// gap (< kSkin) emits a single speculative contact with negative separation
// once a margin is supplied; margin == 0 stays empty (faithful parity). NOT
// compared against the Lua (the Lua has no skin).
// ====================================================================
TEST_CASE("physics: round speculative skin margin", "[physics]")
{
    // 1x1 box (half-extents 0.5) at origin; circle r=0.5 to its right with a
    // small gap along +x.
    const Real gap = kSkin * Real(0.5); // 0.005, inside kSkin
    const Shape box    = MakeAabb(Real(0.5), Real(0.5));
    const Shape circle = MakeCircle(Real(0.5));
    const Transform xfBox{ Vec2(Real(0), Real(0)), Real(0) };
    // box right edge x=0.5; circle left edge at 0.5 + gap -> center at 1.0 + gap.
    const Transform xfCircle{ Vec2(Real(1) + gap, Real(0)), Real(0) };

    // margin == 0: circle is clear of the box, empty manifold (faithful path).
    {
        const Manifold m = CollideShapes(circle, xfCircle, box, xfBox,
                                         /*margin=*/Real(0));
        CHECK(m.pointCount == 0);
    }

    // margin == kSkin: a single speculative contact with separation in
    // (-kSkin, 0] (negative == a gap, pre-penetration).
    {
        const Manifold m = CollideShapes(circle, xfCircle, box, xfBox,
                                         /*margin=*/kSkin);
        REQUIRE(m.pointCount == 1);
        const double sep = static_cast<double>(m.points[0].separation);
        CHECK(sep < 0.0);
        CHECK(sep > -static_cast<double>(kSkin));
        // normal pushes the circle (A) out of the box (B) == +x here.
        CHECK(static_cast<double>(m.normal.x) == Approx(1.0).margin(kTol));
        CHECK(static_cast<double>(m.normal.y) == Approx(0.0).margin(kTol));
    }

    // gap > kSkin: empty even with margin.
    {
        const Transform xfFar{ Vec2(Real(1) + kSkin * Real(3), Real(0)), Real(0) };
        const Manifold m = CollideShapes(circle, xfFar, box, xfBox,
                                         /*margin=*/kSkin);
        CHECK(m.pointCount == 0);
    }
}
