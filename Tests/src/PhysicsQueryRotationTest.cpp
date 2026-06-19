// Physics v2 Task 7: rotation + fixture-aware migration of the remaining
// "old narrowphase" consumers (events, queries, CCD).
//
// T5 made the SOLVER contact path (GenerateContacts) rotation + fixture aware.
// T7 finishes the job for the THREE other narrowphase consumers that still ran
// the OLD path on the legacy single body shape at angle 0:
//   Part 0 -- the GJK conservative-advancement core builder (BuildCore) now
//             honors xf.rotation, making ShapeDistance / ShapeCast /
//             ShapePolyDistance rotation-aware automatically.
//   Part A -- ContactManager overlap (events / re-arm) is rotation + fixture
//             aware via PhysicsWorld::SlotsOverlap.
//   Part B -- Queries (RayVsBody, ShapeCast query, OverlapShape) iterate body
//             fixtures with the composed real angle.
//   Part C -- BulletSweep (CCD) sweeps the bullet's fixtures with real angle.
//
// All six gate cases are ANALYTIC (hand-derived expected values, NOT captured
// from the old engine):
//   (a) Part 0 -- ShapeDistance to a rotated capsule (and angle-0 unchanged).
//   (b) Part A -- a kinematic body that overlaps a static ONLY when rotated
//                 fires a Begin contact event (and not at angle 0).
//   (c) Part B -- a ray vs a 90-deg-rotated capsule hits the rotated extent
//                 (and angle-0 unchanged).
//   (d) Part B -- OverlapShape on a body that overlaps the query ONLY rotated.
//   (e) Part B -- OverlapShape on a COMPOUND body via fixture[1] (offset).
//   (f) Part C -- a fast bullet whose ROTATED fixture extent is clamped just
//                 outside a thin wall it would otherwise tunnel.
//
// PRESENTATION-FREE + C++20-clean.

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/Fixture.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Narrowphase/Gjk.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep   = Real(1) / Real(60);
    constexpr Real kHalfPi = Real(1.5707963267948966);

    // Box polygon (half-extents hw x hh), CCW: BL, BR, TR, TL. A polygon (not
    // an AABB) so a Dynamic/Kinematic body can carry it at a non-zero angle.
    Shape MakeBoxPolygon(Real hw, Real hh)
    {
        return MakePolygon({
            Vec2(-hw, -hh),
            Vec2( hw, -hh),
            Vec2( hw,  hh),
            Vec2(-hw,  hh),
        });
    }
}

// ===========================================================================
// (a) Part 0 -- BuildCore honors rotation: ShapeDistance to a rotated capsule.
//
// MakeCapsule(6, 2): core segment (-6,0)-(+6,0), radius 2.
//   At angle 0 it is HORIZONTAL: x in [-8,8], y in [-2,2].
//   Rotated +pi/2 it is VERTICAL: x in [-2,2], y in [-8,8].
//
// A zero-radius point (MakeCircle(0)) at (0,12):
//   vs the ROTATED (vertical) capsule: nearest segment point is (0,6),
//     gap 12-6 = 6, minus radius 2 = 4.
//   vs the angle-0 (horizontal) capsule: nearest segment point is (0,0),
//     gap 12, minus radius 2 = 10.
// Rotation-blind code would report 10 for BOTH (it would never rotate the
// segment), so the rotated case is the discriminator.
// ===========================================================================
TEST_CASE("physics-v2 T7 (a): ShapeDistance honors rotation (rotated capsule)",
          "[physics][PhysicsQueryRotation]")
{
    const Shape capsule = MakeCapsule(Real(6), Real(2));
    const Shape point   = MakeCircle(Real(0));

    // Rotated +pi/2 (vertical): surface distance from (0,12) = 12 - 6 - 2 = 4.
    {
        const Transform xfCap{ Vec2(Real(0), Real(0)), kHalfPi };
        const Transform xfPt { Vec2(Real(0), Real(12)), Real(0) };
        const ShapeDistanceResult d = ShapeDistance(point, xfPt, capsule, xfCap);
        CHECK(static_cast<double>(d.distance) == Approx(4.0).margin(1e-3));
    }

    // Angle 0 (horizontal): surface distance from (0,12) = 12 - 0 - 2 = 10.
    // (Proves the angle-0 path is unchanged by the rotation fix.)
    {
        const Transform xfCap{ Vec2(Real(0), Real(0)), Real(0) };
        const Transform xfPt { Vec2(Real(0), Real(12)), Real(0) };
        const ShapeDistanceResult d = ShapeDistance(point, xfPt, capsule, xfCap);
        CHECK(static_cast<double>(d.distance) == Approx(10.0).margin(1e-3));
    }
}

// ===========================================================================
// (b) Part A -- ContactManager events are rotation + fixture aware.
//
// A static CIRCLE radius 2 at (0,6) and a KINEMATIC box-polygon (half-extents
// 10 x 2) at the origin.
//   At angle 0 the box spans y in [-2,2]; the circle bottom is at y=4 -> a
//     gap of 2 -> NO overlap -> no Begin event.
//   Rotated +pi/2 the box spans y in [-10,10]; it now overlaps the circle
//     (whose center y=6 is well inside the box) -> a Begin event fires.
// ===========================================================================
TEST_CASE("physics-v2 T7 (b): contact Begin fires only when the body is rotated",
          "[physics][PhysicsQueryRotation]")
{
    auto buildWorld = [](Real boxAngle) -> int
    {
        PhysicsWorld w;

        BodyDef circ;
        circ.type     = BodyType::Static;
        circ.position = Vec2(Real(0), Real(6));
        circ.shape    = MakeCircle(Real(2));
        w.AddBody(circ);

        BodyDef box;
        box.type     = BodyType::Kinematic; // kinematic-vs-static emits events
        box.position = Vec2(Real(0), Real(0));
        box.shape    = MakeBoxPolygon(Real(10), Real(2));
        BodyHandle hb = w.AddBody(box);
        w.SetAngle(hb, boxAngle);

        int beginCount = 0;
        w.OnContact([&](const ContactEvent& ev)
        {
            if (ev.type == ContactEvent::Type::Begin)
            {
                ++beginCount;
            }
        });

        w.Step(kStep);
        return beginCount;
    };

    // Angle 0: no overlap -> no Begin.
    CHECK(buildWorld(Real(0)) == 0);
    // Rotated pi/2: overlap -> Begin fires.
    CHECK(buildWorld(kHalfPi) == 1);
}

// ===========================================================================
// (c) Part B -- Raycast / RayVsBody vs a rotated capsule hits the rotated
//               extent.
//
// A kinematic body with MakeCapsule(6,2) at the origin.
//   Rotated +pi/2 (vertical): top surface at y=8.  A ray from (0,20) down to
//     (0,-20) (delta y = -40) hits the top at y=8 -> t = (20-8)/40 = 0.30.
//   Angle 0 (horizontal): top surface at y=2 -> t = (20-2)/40 = 0.45.
// ===========================================================================
TEST_CASE("physics-v2 T7 (c): raycast vs a rotated capsule hits rotated extent",
          "[physics][PhysicsQueryRotation]")
{
    auto castT = [](Real capAngle) -> Real
    {
        PhysicsWorld w;
        BodyDef bd;
        bd.type     = BodyType::Kinematic;
        bd.position = Vec2(Real(0), Real(0));
        bd.shape    = MakeCapsule(Real(6), Real(2));
        BodyHandle h = w.AddBody(bd);
        w.SetAngle(h, capAngle);

        const auto hit = w.Raycast(Vec2(Real(0), Real(20)), Vec2(Real(0), Real(-20)));
        REQUIRE(hit.has_value());
        REQUIRE_FALSE(hit->isCell);
        return hit->t;
    };

    // Rotated pi/2: top at y=8 -> t = 12/40 = 0.30.
    CHECK(static_cast<double>(castT(kHalfPi)) == Approx(0.30).margin(1e-2));
    // Angle 0: top at y=2 -> t = 18/40 = 0.45 (unchanged path).
    CHECK(static_cast<double>(castT(Real(0))) == Approx(0.45).margin(1e-2));
}

// ===========================================================================
// (d) Part B -- OverlapShape on a body that overlaps the query ONLY rotated.
//
// A static MakeAabb(10,2) body at the origin (a long thin box).
//   At angle 0 it spans y in [-2,2].
//   Rotated +pi/2 it spans y in [-10,10].
// A query MakeCircle(1) at (0,9):
//   unrotated: nearest body point y=2, gap 9-2=7 > radius 1 -> NO overlap.
//   rotated  : the query center y=9 is inside the body (y in [-10,10]),
//              x=0 in [-2,2] -> OVERLAP.
// ===========================================================================
TEST_CASE("physics-v2 T7 (d): OverlapShape sees a body only when rotated",
          "[physics][PhysicsQueryRotation]")
{
    auto overlaps = [](Real bodyAngle) -> int
    {
        PhysicsWorld w;
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(Real(0), Real(0));
        bd.shape    = MakeAabb(Real(10), Real(2));
        BodyHandle h = w.AddBody(bd);
        w.SetAngle(h, bodyAngle);

        const Shape query = MakeCircle(Real(1));
        const Transform xf{ Vec2(Real(0), Real(9)), Real(0) };
        std::vector<BodyHandle> hits;
        return w.OverlapShape(query, xf, hits);
    };

    CHECK(overlaps(Real(0)) == 0);     // unrotated: misses
    CHECK(overlaps(kHalfPi) == 1);     // rotated: overlaps
}

// ===========================================================================
// (e) Part B -- OverlapShape on a COMPOUND body via fixture[1].
//
// A body with TWO fixtures:
//   fixture0 = MakeAabb(2,2) at local (0,0) [the AddBody auto-fixture]
//   fixture1 = MakeCircle(2) at local (12,0)
// A query MakeCircle(1) at (12,0): the rotation-blind single-shape path would
// test only the primary box at the origin and MISS; the fixture-aware path
// finds the overlap via fixture[1] (the offset circle).
// ===========================================================================
TEST_CASE("physics-v2 T7 (e): OverlapShape sees a compound body via fixture[1]",
          "[physics][PhysicsQueryRotation]")
{
    PhysicsWorld w;

    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(0), Real(0));
    bd.shape    = MakeAabb(Real(2), Real(2)); // fixture0 at local (0,0)
    BodyHandle bh = w.AddBody(bd);

    FixtureDef fd;
    fd.shape    = MakeCircle(Real(2));
    fd.localPos = Vec2(Real(12), Real(0)); // fixture1 offset
    w.AddFixture(bh, fd);

    // Query at (12,0): overlaps fixture1 (a circle r=2 at (12,0)) but NOT the
    // primary box at the origin.
    const Shape query = MakeCircle(Real(1));
    const Transform xf{ Vec2(Real(12), Real(0)), Real(0) };
    std::vector<BodyHandle> hits;
    const int n = w.OverlapShape(query, xf, hits);
    REQUIRE(n == 1);
    CHECK(hits[0] == bh);

    // Control: the same query far from BOTH fixtures finds nothing.
    const Transform xfFar{ Vec2(Real(40), Real(0)), Real(0) };
    std::vector<BodyHandle> none;
    CHECK(w.OverlapShape(query, xfFar, none) == 0);
}

// ===========================================================================
// (f) Part C -- BulletSweep clamps a fast bullet whose ROTATED fixture extent
//               tunnels a thin wall.
//
// A thin horizontal static wall MakeAabb(50, 0.5) at (0,100): spans
//   y in [99.5, 100.5].
// A KINEMATIC bullet box MakeBoxPolygon(8, 1) rotated +pi/2 so its y
//   half-extent is 8 (instead of 1).  It starts at (0,0) and is given a huge
//   downward velocity so it would travel 200 units in one step (prev=(0,0),
//   curr=(0,200), delta=(0,200)).
//
// The rotated bullet's leading (bottom) edge is at center_y + 8.  It first
// touches the wall top (y=99.5) when center_y + 8 = 99.5 -> center_y = 91.5,
//   t = 91.5 / 200 = 0.4575.  Clamp = t - 0.001 -> clamped center_y ~= 91.3.
//
// Rotation-blind (y half-extent 1) would clamp at center_y ~= 98.5 instead, so
// the post-step y near 91.3 (NOT ~98.5) is the discriminator.  Either way the
// bullet must stop SHORT of the wall top (y=99.5).
// ===========================================================================
TEST_CASE("physics-v2 T7 (f): BulletSweep clamps a rotated bullet at its extent",
          "[physics][PhysicsQueryRotation]")
{
    PhysicsWorld w;

    BodyDef wall;
    wall.type     = BodyType::Static;
    wall.position = Vec2(Real(0), Real(100));
    wall.shape    = MakeAabb(Real(50), Real(0.5)); // thin: y in [99.5,100.5]
    w.AddBody(wall);

    BodyDef bullet;
    bullet.type     = BodyType::Kinematic;
    bullet.position = Vec2(Real(0), Real(0));
    bullet.shape    = MakeBoxPolygon(Real(8), Real(1)); // long in x, thin in y
    bullet.bullet   = true;
    BodyHandle hb = w.AddBody(bullet);
    w.SetAngle(hb, kHalfPi); // rotate so the y half-extent becomes 8

    // Velocity that moves the bullet 200 units down in one 1/60s step.
    w.SetVelocity(hb, Vec2(Real(0), Real(200) / kStep));

    w.Step(kStep);

    const Vec2 finalPos = w.Position(hb);
    // The bullet's bottom edge (center_y + 8) must stop just OUTSIDE the wall
    // top (y=99.5): center_y < 91.5.
    CHECK(static_cast<double>(finalPos.y) < 91.5);
    // And it must be clamped at the rotated extent (~91.3), NOT the angle-0
    // extent (~98.5) -- so it is well below 95.
    CHECK(static_cast<double>(finalPos.y) > 88.0);
    CHECK(static_cast<double>(finalPos.y) < 95.0);
}
