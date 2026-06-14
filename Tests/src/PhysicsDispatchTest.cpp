// Physics M6 P1.5: narrowphase dispatch routing smoke tests.
//
// Verifies that CollideShapes (Dispatch.hpp) routes each shape-pair combination
// to the correct underlying collider by asserting the dispatch result equals the
// direct collider call for overlapping configurations in all six category-pairs:
//
//   1. circle  vs circle    -> CollideRoundRound
//   2. circle  vs polygon   -> CollideRoundPolygon
//   3. polygon vs circle    -> CollidePolygonRound   (flipped-normal path)
//   4. polygon vs polygon   -> CollidePolygons
//   5. aabb    vs polygon   -> CollidePolygons       (aabb treated as poly)
//   6. capsule vs aabb      -> CollideRoundPolygon
//
// Each test uses an overlapping setup so the result is non-empty (verifying the
// wiring reached the real collider, not the old P1.3 stub). The comparison
// checks normal + pointCount + per-point (position, separation, id) via a shared
// CompareManifolds helper.

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Narrowphase/Dispatch.hpp>
#include <Arcane/Physics/Narrowphase/Manifold.hpp>
#include <Arcane/Physics/Narrowphase/Specialized.hpp>
#include <Arcane/Physics/Shapes.hpp>

using Catch::Approx;
using namespace Arcane::Physics;

namespace
{
    // Absolute tolerance for f32 geometry comparisons (pixel-scale coords).
    constexpr double kTol = 1e-5;

    // Assert that two Manifolds are equivalent: same pointCount, same
    // representative normal, and per-point position/separation/normal/id.
    void CompareManifolds(const Manifold& got, const Manifold& ref)
    {
        REQUIRE(got.pointCount == ref.pointCount);
        if (ref.pointCount == 0) return;

        // Representative normal.
        CHECK(static_cast<double>(got.normal.x) ==
              Approx(static_cast<double>(ref.normal.x)).margin(kTol));
        CHECK(static_cast<double>(got.normal.y) ==
              Approx(static_cast<double>(ref.normal.y)).margin(kTol));

        for (int i = 0; i < ref.pointCount; ++i)
        {
            const ManifoldPoint& gp = got.points[i];
            const ManifoldPoint& rp = ref.points[i];

            CHECK(static_cast<double>(gp.point.x) ==
                  Approx(static_cast<double>(rp.point.x)).margin(kTol));
            CHECK(static_cast<double>(gp.point.y) ==
                  Approx(static_cast<double>(rp.point.y)).margin(kTol));
            CHECK(static_cast<double>(gp.separation) ==
                  Approx(static_cast<double>(rp.separation)).margin(kTol));
            CHECK(static_cast<double>(gp.normal.x) ==
                  Approx(static_cast<double>(rp.normal.x)).margin(kTol));
            CHECK(static_cast<double>(gp.normal.y) ==
                  Approx(static_cast<double>(rp.normal.y)).margin(kTol));
            CHECK(gp.id == rp.id);
        }
    }
} // namespace

// ====================================================================
// 1. circle vs circle -> CollideRoundRound
//    Two unit circles overlapping along the x-axis by 0.5 units.
// ====================================================================
TEST_CASE("physics dispatch: circle vs circle routes to CollideRoundRound", "[physics]")
{
    const Shape ca = MakeCircle(Real(1));
    const Shape cb = MakeCircle(Real(1));
    const Transform xfA{ Vec2(Real(0), Real(0)), Real(0) };
    const Transform xfB{ Vec2(Real(1.5), Real(0)), Real(0) };

    const Manifold via_dispatch = CollideShapes(ca, xfA, cb, xfB);
    const Manifold direct       = CollideRoundRound(ca, xfA, cb, xfB);

    REQUIRE(via_dispatch.pointCount > 0); // overlapping -- must hit
    CompareManifolds(via_dispatch, direct);
}

// ====================================================================
// 2. circle vs polygon -> CollideRoundPolygon
//    Circle centered above a unit square, overlapping its top edge.
// ====================================================================
TEST_CASE("physics dispatch: circle vs polygon routes to CollideRoundPolygon", "[physics]")
{
    // Unit square: verts listed CCW.
    const Shape sq = MakePolygon({
        Vec2(Real(-0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real( 0.5)),
        Vec2(Real(-0.5), Real( 0.5)),
    });
    const Shape circ = MakeCircle(Real(0.75));

    const Transform xfSq{ Vec2(Real(0), Real(0)), Real(0) };
    // Circle center at y=0.9: circle radius 0.75, top of square at y=0.5 -> overlap 0.35.
    const Transform xfC{ Vec2(Real(0), Real(0.9)), Real(0) };

    const Manifold via_dispatch = CollideShapes(circ, xfC, sq, xfSq);
    const Manifold direct       = CollideRoundPolygon(circ, xfC, sq, xfSq);

    REQUIRE(via_dispatch.pointCount > 0);
    CompareManifolds(via_dispatch, direct);
}

// ====================================================================
// 3. polygon vs circle -> CollidePolygonRound (flipped-normal path)
//    Same geometry as above but arguments swapped: poly A, circle B.
// ====================================================================
TEST_CASE("physics dispatch: polygon vs circle routes to CollidePolygonRound", "[physics]")
{
    const Shape sq = MakePolygon({
        Vec2(Real(-0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real( 0.5)),
        Vec2(Real(-0.5), Real( 0.5)),
    });
    const Shape circ = MakeCircle(Real(0.75));

    const Transform xfSq{ Vec2(Real(0), Real(0)), Real(0) };
    const Transform xfC{ Vec2(Real(0), Real(0.9)), Real(0) };

    const Manifold via_dispatch = CollideShapes(sq, xfSq, circ, xfC);
    const Manifold direct       = CollidePolygonRound(sq, xfSq, circ, xfC);

    REQUIRE(via_dispatch.pointCount > 0);
    CompareManifolds(via_dispatch, direct);
}

// ====================================================================
// 4. polygon vs polygon -> CollidePolygons
//    Two unit squares overlapping along the x-axis.
//    This is the path that was a stub (empty Manifold{}) in P1.3.
// ====================================================================
TEST_CASE("physics dispatch: polygon vs polygon routes to CollidePolygons", "[physics]")
{
    const Shape sq = MakePolygon({
        Vec2(Real(-0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real( 0.5)),
        Vec2(Real(-0.5), Real( 0.5)),
    });

    const Transform xfA{ Vec2(Real(0), Real(0)), Real(0) };
    const Transform xfB{ Vec2(Real(0.7), Real(0)), Real(0) }; // overlap 0.3 along x

    const Manifold via_dispatch = CollideShapes(sq, xfA, sq, xfB);
    const Manifold direct       = CollidePolygons(sq, xfA, sq, xfB);

    REQUIRE(via_dispatch.pointCount > 0); // key: no longer a stub!
    CompareManifolds(via_dispatch, direct);
}

// ====================================================================
// 5. aabb vs polygon -> CollidePolygons
//    AABB treated as poly; overlap along y-axis.
// ====================================================================
TEST_CASE("physics dispatch: aabb vs polygon routes to CollidePolygons", "[physics]")
{
    const Shape ab = MakeAabb(Real(0.5), Real(0.5));
    const Shape sq = MakePolygon({
        Vec2(Real(-0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real(-0.5)),
        Vec2(Real( 0.5), Real( 0.5)),
        Vec2(Real(-0.5), Real( 0.5)),
    });

    const Transform xfAb{ Vec2(Real(0), Real(0)), Real(0) };
    const Transform xfSq{ Vec2(Real(0), Real(0.7)), Real(0) }; // overlap 0.3 along y

    const Manifold via_dispatch = CollideShapes(ab, xfAb, sq, xfSq);
    const Manifold direct       = CollidePolygons(ab, xfAb, sq, xfSq);

    REQUIRE(via_dispatch.pointCount > 0);
    CompareManifolds(via_dispatch, direct);
}

// ====================================================================
// 6. capsule vs aabb -> CollideRoundPolygon
//    Horizontal capsule overlapping the left face of an AABB.
// ====================================================================
TEST_CASE("physics dispatch: capsule vs aabb routes to CollideRoundPolygon", "[physics]")
{
    // Capsule: half-length 0.4, radius 0.2, center at (-0.8, 0).
    const Shape cap  = MakeCapsule(Real(0.4), Real(0.2));
    const Shape ab   = MakeAabb(Real(0.5), Real(0.5)); // box at origin, left face x=-0.5

    const Transform xfCap{ Vec2(Real(-0.8), Real(0)), Real(0) };
    // Capsule right endpoint: -0.8 + 0.4 = -0.4, plus radius 0.2 -> reaches x=-0.2.
    // Box left edge x=-0.5 -> overlap of 0.3.
    const Transform xfAb{ Vec2(Real(0), Real(0)), Real(0) };

    const Manifold via_dispatch = CollideShapes(cap, xfCap, ab, xfAb);
    const Manifold direct       = CollideRoundPolygon(cap, xfCap, ab, xfAb);

    REQUIRE(via_dispatch.pointCount > 0);
    CompareManifolds(via_dispatch, direct);
}
