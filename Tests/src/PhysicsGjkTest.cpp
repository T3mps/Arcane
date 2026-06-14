// Physics M6 P1.4: GJK distance / closest points / surface distance + the
// conservative-advancement ShapeCast primitive.
//
// PORT NOTE: GjkDistance / ShapeDistance / ShapePolyDistance are faithful ports
// of Client/src/physics/GJK.lua; ShapeCast ports the conservative-advancement
// `advance` primitive from Client/src/physics/Cast.lua (TOL=0.05, MAX_ITER=32).
//
// LAYER A (GJK distance + shapeDistance) bit-matches the EXISTING oracle
// gjk.json (corner_corner, point_poly, poly_poly_overlap/separated,
// seg_seg_parallel, shape_circle_poly, shape_capsule_capsule), captured P1.0.
// Pixel-scale coords -> an absolute margin of 1e-4 is the f32-vs-f64 allowance
// (the GJK distance core runs in f64 internally to track the Lua oracle).
//
// LAYER B (ShapeCast) has NO standalone oracle -- the harness shapeCast goes
// through the world-coupled Cast.shapeCast (captured in P1.9). So ShapeCast is
// tested by INVARIANT: a circle swept toward a static convex shape stops with
// surface distance < TOL and a correct push-back normal; a clear miss returns
// no-hit; results are deterministic.

#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Narrowphase/Gjk.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include "Helpers/PhysicsOracle.hpp"

using Catch::Approx;
using namespace Arcane::Physics;

namespace
{
    // f32-vs-f64 cross-precision absolute margin for pixel-scale coords.
    constexpr double kTol = 1e-4;

    // Flatten an oracle {x0,y0,x1,y1,...} array into world verts.
    std::vector<Vec2> LoadFlat(const nlohmann::json& flat)
    {
        std::vector<Vec2> v;
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2)
        {
            v.emplace_back(flat[i].get<Real>(), flat[i + 1].get<Real>());
        }
        return v;
    }

    // Build a Shape from a gjk.json descriptor (circle/capsule/polygon/aabb).
    Shape ShapeFromDesc(const nlohmann::json& d)
    {
        const std::string kind = d["kind"].get<std::string>();
        if (kind == "circle")  return MakeCircle(d["r"].get<Real>());
        if (kind == "capsule") return MakeCapsule(d["halfLen"].get<Real>(),
                                                  d["r"].get<Real>());
        if (kind == "aabb")    return MakeAabb(d["hw"].get<Real>(),
                                               d["hh"].get<Real>());
        return MakePolygon(LoadFlat(d["verts"]));
    }

    // Run one raw-core GJK distance case from gjk.json: `a`/`b` are flat world-
    // vertex spans, `expect` carries dist (always) and ax,ay,bx,by (when the
    // cores are separated -- overlap cases omit witnesses).
    void CheckDistanceCase(const nlohmann::json& cs)
    {
        const std::vector<Vec2> a = LoadFlat(cs["a"]);
        const std::vector<Vec2> b = LoadFlat(cs["b"]);
        const GjkResult g = GjkDistance(a.data(), static_cast<int>(a.size()),
                                        b.data(), static_cast<int>(b.size()));
        const auto& e = cs["expect"];
        CHECK(static_cast<double>(g.distance) ==
              Approx(e["dist"].get<double>()).margin(kTol));
        if (e.contains("ax"))
        {
            CHECK(static_cast<double>(g.witnessA.x) ==
                  Approx(e["ax"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(g.witnessA.y) ==
                  Approx(e["ay"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(g.witnessB.x) ==
                  Approx(e["bx"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(g.witnessB.y) ==
                  Approx(e["by"].get<double>()).margin(kTol));
        }
    }

    // Run one shapeDistance case from gjk.json: shapeA/shapeB + positions,
    // `expect` carries surface dist (always) and ax,ay,bx,by,nx,ny when present.
    void CheckShapeDistanceCase(const nlohmann::json& cs)
    {
        const Shape sa = ShapeFromDesc(cs["shapeA"]);
        const Shape sb = ShapeFromDesc(cs["shapeB"]);
        const Transform xfA{ Vec2(cs["xa"].get<Real>(), cs["ya"].get<Real>()),
                             Real(0) };
        const Transform xfB{ Vec2(cs["xb"].get<Real>(), cs["yb"].get<Real>()),
                             Real(0) };
        const ShapeDistanceResult r = ShapeDistance(sa, xfA, sb, xfB);
        const auto& e = cs["expect"];
        CHECK(static_cast<double>(r.distance) ==
              Approx(e["dist"].get<double>()).margin(kTol));
        if (e.contains("ax"))
        {
            CHECK(static_cast<double>(r.witnessA.x) ==
                  Approx(e["ax"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(r.witnessA.y) ==
                  Approx(e["ay"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(r.witnessB.x) ==
                  Approx(e["bx"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(r.witnessB.y) ==
                  Approx(e["by"].get<double>()).margin(kTol));
        }
        if (e.contains("nx"))
        {
            CHECK(static_cast<double>(r.normal.x) ==
                  Approx(e["nx"].get<double>()).margin(kTol));
            CHECK(static_cast<double>(r.normal.y) ==
                  Approx(e["ny"].get<double>()).margin(kTol));
        }
    }
} // namespace

// ====================================================================
// LAYER A -- GJK distance + closest points bit-match gjk.json.
//   corner_corner (3-4-5 distance + witnesses), point_poly (point vs poly
//   distance + witnesses), poly_poly_overlap (dist 0), poly_poly_separated
//   (separated distance + witnesses), seg_seg_parallel (parallel-segment
//   distance).
// ====================================================================
TEST_CASE("physics: GJK distance + closest points match the gjk oracle",
          "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("gjk");
    REQUIRE_FALSE(j.empty());

    CheckDistanceCase(j["corner_corner"]);
    CheckDistanceCase(j["point_poly"]);
    CheckDistanceCase(j["poly_poly_overlap"]);
    CheckDistanceCase(j["poly_poly_separated"]);
    CheckDistanceCase(j["seg_seg_parallel"]);
}

// ====================================================================
// LAYER A -- shapeDistance (surface distance + witnesses + normal) bit-match.
//   shape_circle_poly (circle r=3 above a diamond: surface dist 7, witnesses,
//   normal (0,-1)), shape_capsule_capsule (two parallel capsules: surface
//   dist 6).
// ====================================================================
TEST_CASE("physics: GJK shapeDistance matches the gjk oracle", "[physics]")
{
    const nlohmann::json j = Arcane::Test::LoadOracle("gjk");
    REQUIRE_FALSE(j.empty());

    CheckShapeDistanceCase(j["shape_circle_poly"]);
    CheckShapeDistanceCase(j["shape_capsule_capsule"]);
}

// ====================================================================
// LAYER B -- ShapeCast invariants (no oracle; the world-level cast is P1.9).
// A circle swept toward a static box stops within TOL of the surface with a
// push-back normal pointing back along the travel; a clear miss returns no
// hit; the result is deterministic across repeated calls.
// ====================================================================
TEST_CASE("physics: ShapeCast stops at the surface with a push-back normal",
          "[physics]")
{
    // Static 1x1 box (half-extents 0.5) at the origin. A small circle starts
    // to its left and is swept rightward straight at it.
    const Shape box    = MakeAabb(Real(0.5), Real(0.5));
    const Shape circle = MakeCircle(Real(0.5));
    const Transform boxXf{ Vec2(Real(0), Real(0)), Real(0) };

    // Circle center starts at x=-5 (well clear: box left edge -0.5, circle
    // right edge -4.5). Swept +x by 10 -> would end at +5, deep inside.
    const Transform start{ Vec2(Real(-5), Real(0)), Real(0) };
    const Vec2 translation(Real(10), Real(0));

    const ShapeCastResult hit = ShapeCast(circle, start, translation, box, boxXf);

    REQUIRE(hit.hit);
    // Stopped within TOL of the surface (and not past it -- conservative
    // advancement on a convex obstacle never overshoots).
    CHECK(static_cast<double>(hit.distance) < static_cast<double>(kShapeCastTol));
    // The circle should stop just left of contact: surfaces touch when the
    // circle center is at x = -1 (box left edge -0.5 minus circle radius 0.5),
    // i.e. t ~= 0.4 along the 10-unit sweep.
    CHECK(static_cast<double>(hit.t) > 0.0);
    CHECK(static_cast<double>(hit.t) < 1.0);
    const double centerX = -5.0 + 10.0 * static_cast<double>(hit.t);
    CHECK(centerX == Approx(-1.0).margin(0.05));
    // Push-back normal points from the box (B) toward the circle (A): -x.
    CHECK(static_cast<double>(hit.normal.x) == Approx(-1.0).margin(1e-2));
    CHECK(static_cast<double>(hit.normal.y) == Approx(0.0).margin(1e-2));
}

TEST_CASE("physics: ShapeCast against a raw poly stops at the surface",
          "[physics]")
{
    // A diamond poly (the gjk oracle's poly_a, centered ~ (32,16)) as a raw
    // world span; a circle swept straight up into its lower vertex (32,0).
    const std::vector<Vec2> poly = { Vec2(32, 0), Vec2(64, 16),
                                     Vec2(32, 32), Vec2(0, 16) };
    const Shape circle = MakeCircle(Real(3));

    // Start well below the diamond, sweep upward toward the (32,0) vertex.
    const Transform start{ Vec2(Real(32), Real(-40)), Real(0) };
    const Vec2 translation(Real(0), Real(60)); // would end at y=+20, inside.

    const ShapeCastResult hit = ShapeCastPoly(
        circle, start, translation, poly.data(),
        static_cast<int>(poly.size()));

    REQUIRE(hit.hit);
    CHECK(static_cast<double>(hit.distance) < static_cast<double>(kShapeCastTol));
    // Contact when the circle (r=3) center reaches y = -3 (vertex at y=0).
    const double centerY = -40.0 + 60.0 * static_cast<double>(hit.t);
    CHECK(centerY == Approx(-3.0).margin(0.05));
    // Normal points from the poly toward the circle: straight down (-y).
    CHECK(static_cast<double>(hit.normal.x) == Approx(0.0).margin(1e-2));
    CHECK(static_cast<double>(hit.normal.y) == Approx(-1.0).margin(1e-2));
}

TEST_CASE("physics: ShapeCast clear miss returns no hit + is deterministic",
          "[physics]")
{
    const Shape box    = MakeAabb(Real(0.5), Real(0.5));
    const Shape circle = MakeCircle(Real(0.5));
    const Transform boxXf{ Vec2(Real(0), Real(0)), Real(0) };

    // Sweep parallel to the box, far above it -- never comes within TOL.
    const Transform start{ Vec2(Real(-5), Real(50)), Real(0) };
    const Vec2 translation(Real(10), Real(0));

    const ShapeCastResult miss =
        ShapeCast(circle, start, translation, box, boxXf);
    CHECK_FALSE(miss.hit);

    // Determinism: an identical cast yields a bit-identical result.
    const ShapeCastResult miss2 =
        ShapeCast(circle, start, translation, box, boxXf);
    CHECK(miss2.hit == miss.hit);

    // A zero-length translation is a no-hit (Cast.lua's len < 1e-9 guard).
    const ShapeCastResult zero = ShapeCast(
        circle, start, Vec2(Real(0), Real(0)), box, boxXf);
    CHECK_FALSE(zero.hit);

    // Determinism on a HIT path too: repeat the straight-on cast and compare.
    const Transform startHit{ Vec2(Real(-5), Real(0)), Real(0) };
    const ShapeCastResult h1 =
        ShapeCast(circle, startHit, translation, box, boxXf);
    const ShapeCastResult h2 =
        ShapeCast(circle, startHit, translation, box, boxXf);
    REQUIRE(h1.hit);
    CHECK(h1.t == h2.t);
    CHECK(h1.distance == h2.distance);
    CHECK(h1.normal.x == h2.normal.x);
    CHECK(h1.normal.y == h2.normal.y);
}

// ====================================================================
// LAYER B -- ShapeCast t=0 overlap invariant.
// A moving circle that STARTS already touching or inside the static obstacle
// (surface distance < kShapeCastTol at t=0) must return hit==true, t==0, and
// distance < kShapeCastTol without crashing or looping. This guards the
// "body woken into a wall" case before P1.8/the solver wires ShapeCast in.
// ====================================================================
TEST_CASE("physics: ShapeCast returns hit at t=0 when starting overlapping",
          "[physics]")
{
    // Static 1x1 box (half-extents 0.5) at the origin.
    const Shape box    = MakeAabb(Real(0.5), Real(0.5));
    const Shape circle = MakeCircle(Real(0.5));
    const Transform boxXf{ Vec2(Real(0), Real(0)), Real(0) };

    // Circle center at x = -0.5: its right surface touches the box left surface
    // exactly (circle right edge -0.5+0.5 = 0.0 == box left edge -0.5+0.0?).
    // Actually place it at x = -0.5 so the surface distance = 0 (touching):
    //   box left edge = -0.5, circle right edge = -0.5 + 0.5 = 0.0. Hmm, that
    // gives surface distance = -0.5 - 0.0 = -0.5 (overlap). Good.
    // Use x = -0.5 so the circle is centered one radius away from box center:
    // surface dist = |(-0.5) - (-0.5)| - 0.5 - 0 = 0 - 0.5 (negative) -> overlap.
    //
    // Simpler: place the circle center INSIDE the box (center at origin).
    // Surface distance is deeply negative; Advance checks at t=0 first.
    const Transform startOverlap{ Vec2(Real(0), Real(0)), Real(0) };
    const Vec2 translation(Real(5), Real(0)); // sweep rightward

    const ShapeCastResult res =
        ShapeCast(circle, startOverlap, translation, box, boxXf);

    REQUIRE(res.hit);
    CHECK(static_cast<double>(res.t) == Approx(0.0).margin(1e-4));
    CHECK(static_cast<double>(res.distance) <
          static_cast<double>(kShapeCastTol));
}
