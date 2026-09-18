// Arcane 3D transform-gizmo core ([gizmo], CPU-only): pure value tests over
// GizmoTransform + ViewTransform -- no Registry, no graphics device.
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <rapidcheck/catch.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Scene/Components.hpp>     // Transform::ToMatrix -- pins ComposeTRS against it
#include <Arcane/Scene/ViewTransform.hpp>

using Catch::Matchers::WithinAbs;
using namespace Arcane;

namespace
{
    constexpr float kPi = 3.14159265358979f;
    // The 2D view: orthographic, centred on the origin, 800x600 at 100 px/m
    // (halfH = 3 m). world (x, y) -> pixel (400 + 100x, 300 - 100y).
    ViewTransform Ortho() { return ViewTransform::Orthographic({0.0f, 0.0f}, 3.0f, {800u, 600u}); }
    // A perspective view on the same origin from +Z, 60 deg fov.
    ViewTransform Persp() { return ViewTransform::Perspective({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f, {800u, 600u}, 0.1f, 100.0f); }
    // An oblique perspective: above and to the side, so no world axis is edge-on.
    ViewTransform Oblique() { return ViewTransform::Perspective({4.0f, 3.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f, {800u, 600u}, 0.1f, 100.0f); }
    glm::vec2 Px(const ViewTransform& v, glm::vec3 p) { return glm::vec2(v.WorldToScreen(p)); }
    bool NearQuat(glm::quat a, glm::quat b, float eps = 1e-4f) { return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < eps; }   // same rotation, either sign
}

TEST_CASE("Gizmo: WorldUnitsPerPixel is the ortho zoom in 2D and grows with distance in perspective", "[gizmo]")
{
    CHECK_THAT(WorldUnitsPerPixel(Ortho(), {0,0,0}), WithinAbs(0.01f, 1e-6f));      // 100 px per metre
    CHECK_THAT(WorldUnitsPerPixel(Ortho(), {5,5,-3}), WithinAbs(0.01f, 1e-6f));     // constant in ortho
    const float nearW = WorldUnitsPerPixel(Persp(), {0,0,3});    // 3 m from the eye   (`near`/`far` are windef.h macros)
    const float farW  = WorldUnitsPerPixel(Persp(), {0,0,-3});   // 9 m from the eye
    CHECK_THAT(farW / nearW, WithinAbs(3.0f, 1e-3f));            // proportional to w (UnrealWidget's rule)
    // A handle of kAxisLenPx (80) at size 1 projects to 80 px in EITHER view.
    for (const ViewTransform& v : { Ortho(), Persp() })
    {
        const float R = 80.0f * WorldUnitsPerPixel(v, {0,0,0});
        CHECK_THAT(glm::length(Px(v, {R,0,0}) - Px(v, {0,0,0})), WithinAbs(80.0f, 0.5f));
    }
}

TEST_CASE("Gizmo: ClosestLineParam and RayPlane", "[gizmo]")
{
    Ray r; r.origin = {0, 0, 5}; r.direction = {0, 0, -1};
    // The ray passes 2 m along +X of the line origin: t = 2.
    CHECK_THAT(ClosestLineParam({-2, 0, 0}, {1, 0, 0}, r), WithinAbs(2.0f, 1e-5f));
    // Parallel: the projection of the ray origin onto the line.
    CHECK_THAT(ClosestLineParam({0, 0, 0}, {0, 0, 1}, r), WithinAbs(5.0f, 1e-5f));
    const auto hit = RayPlane(r, {0, 0, 1}, {0, 0, 1});
    REQUIRE(hit); CHECK_THAT(hit->z, WithinAbs(1.0f, 1e-5f));
    CHECK_FALSE(RayPlane(r, {0, 0, 0}, {1, 0, 0}));            // grazing
    Ray away = r; away.direction = {0, 0, 1};
    CHECK_FALSE(RayPlane(away, {0, 0, 1}, {0, 0, 1}));         // the plane is behind the ray
}

TEST_CASE("Gizmo ApplyDrag: translate along X, in the XY plane and in the camera plane -- 2D view", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; start.position = {0, 0, 0.75f};    // an authored z: must come back UNTOUCHED
    const GizmoSnap noSnap;
    // Mouse (400,300)->(450,300) == world +0.5 along X.
    GizmoTransform rx = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::X, start, v, {400,300}, {450,300}, noSnap);
    CHECK_THAT(rx.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rx.position.y, WithinAbs(0.0f, 1e-4f)); CHECK(rx.position.z == 0.75f);
    // XY plane: (400,300)->(450,350) == (+0.5, -0.5): screen DOWN is world -Y.
    GizmoTransform rp = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::XY, start, v, {400,300}, {450,350}, noSnap);
    CHECK_THAT(rp.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rp.position.y, WithinAbs(-0.5f, 1e-4f)); CHECK(rp.position.z == 0.75f);
    // Center = the camera plane, which in the 2D view IS the XY plane.
    GizmoTransform rc = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::Center, start, v, {400,300}, {450,350}, noSnap);
    CHECK_THAT(rc.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rc.position.y, WithinAbs(-0.5f, 1e-4f)); CHECK(rc.position.z == 0.75f);
    // Snap 0.5: a 0.37 m X drag lands on 0.5.
    GizmoSnap snap; snap.enabled = true; snap.translate = 0.5f;
    GizmoTransform rs = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::X, start, v, {400,300}, {437,300}, snap);
    CHECK_THAT(rs.position.x, WithinAbs(0.5f, 1e-4f));
    // A SNAPPED Center drag snaps only the components the camera plane spans:
    // (+0.37, -0.37) lands on (0.5, -0.5) and z stays 0.75 to the bit (fix round 1).
    GizmoTransform rcs = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::Center, start, v, {400,300}, {437,337}, snap);
    CHECK_THAT(rcs.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rcs.position.y, WithinAbs(-0.5f, 1e-4f)); CHECK(rcs.position.z == 0.75f);
    // Rotation and scale untouched by a translate.
    CHECK(NearQuat(rx.rotation, start.rotation)); CHECK(rx.scale == start.scale);
}

TEST_CASE("Gizmo ApplyDrag: a LOCAL axis follows the rotation", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; start.rotation = glm::angleAxis(kPi * 0.5f, glm::vec3(0, 0, 1));   // local X points +Y
    const GizmoSnap noSnap;
    // A screen drag of (+50, +30) px = world (+0.5, -0.3); only the local-X (world +Y) part projects.
    GizmoTransform r = ApplyDrag(GizmoMode::Translate, GizmoSpace::Local, GizmoAxis::X, start, v, {400,300}, {450,330}, noSnap);
    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-4f)); CHECK_THAT(r.position.y, WithinAbs(-0.3f, 1e-4f));
}

TEST_CASE("Gizmo ApplyDrag: the Z ring turns about +Z, world sense, with snap", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; GizmoSnap noSnap;
    // From world (1,0) [angle 0] to (0,-1) [100 px DOWN]: a clockwise screen sweep is a NEGATIVE world turn.
    GizmoTransform r = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::Z, start, v, {500,300}, {400,400}, noSnap);
    CHECK(NearQuat(r.rotation, glm::angleAxis(-kPi * 0.5f, glm::vec3(0, 0, 1)), 1e-3f));
    // Snap 15 deg: a ~20 deg clockwise sweep snaps to -15.
    GizmoSnap snap; snap.enabled = true; snap.rotationDeg = 15.0f;
    GizmoTransform rs = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::Z, start, v, {500,300}, {400 + 93.97f, 300 + 34.20f}, snap);
    CHECK(NearQuat(rs.rotation, glm::angleAxis(-kPi / 12.0f, glm::vec3(0, 0, 1)), 1e-3f));
    // A step that does not divide 360 (7 deg): the raw atan2 difference of the first sweep is +270,
    // which would snap to 273; wrapped to -90 it snaps to -91 (fix round 1).
    GizmoSnap snap7; snap7.enabled = true; snap7.rotationDeg = 7.0f;
    GizmoTransform r7 = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::Z, start, v, {500,300}, {400,400}, snap7);
    CHECK(NearQuat(r7.rotation, glm::angleAxis(glm::radians(-91.0f), glm::vec3(0, 0, 1)), 1e-4f));   // 1e-4: 273 vs -91 is 4 deg, |dot| = cos 2 deg = 1 - 6e-4
    // The X ring in an oblique view: the result is a turn about WORLD X (its axis), whatever the amount.
    const ViewTransform o = Oblique();
    const glm::vec2 a = Px(o, {0, 1, 0}), b = Px(o, {0, 0, 1});   // two points on the X ring
    GizmoTransform rxr = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::X, start, o, a, b, noSnap);
    const glm::vec3 axis = glm::axis(rxr.rotation);
    CHECK_THAT(std::abs(axis.x), WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(glm::angle(rxr.rotation), WithinAbs(kPi * 0.5f, 2e-2f));   // (0,1,0) -> (0,0,1) is a quarter turn about X
}

TEST_CASE("Gizmo ApplyDrag: scale by screen ratio along the projected axis, uniform, sign-preserving clamp, snap", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; GizmoSnap noSnap;
    // X box at (500,300) dragged to (600,300): distance from the pivot doubles => x2 on X only.
    GizmoTransform rx = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, v, {500,300}, {600,300}, noSnap);
    CHECK_THAT(rx.scale.x, WithinAbs(2.0f, 1e-4f)); CHECK_THAT(rx.scale.y, WithinAbs(1.0f, 1e-4f)); CHECK_THAT(rx.scale.z, WithinAbs(1.0f, 1e-4f));
    // Uniform: |screen offset| 100 -> 200 px => (2,2,2).
    GizmoTransform rc = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::Center, start, v, {500,300}, {400,100}, noSnap);
    CHECK_THAT(rc.scale.x, WithinAbs(2.0f, 1e-4f)); CHECK_THAT(rc.scale.z, WithinAbs(2.0f, 1e-4f));
    // Onto the pivot: clamped to the minimum, never zero.
    GizmoTransform rz = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, v, {500,300}, {400,300}, noSnap);
    CHECK(rz.scale.x > 0.0f);
    // A MIRRORED start (scale.x = -1) keeps its sign under a scale drag.
    GizmoTransform mirrored = start; mirrored.scale.x = -1.0f;
    // (The local +X axis still projects to the RIGHT -- the scale sign lives in the
    // matrix, not the rotation -- so this grab on the negative side measures s0 = -100,
    // s1 = -200 along the projected axis: the ratio is 2 whichever side is grabbed.)
    GizmoTransform rm = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, mirrored, v, {300,300}, {200,300}, noSnap);
    CHECK_THAT(rm.scale.x, WithinAbs(-2.0f, 1e-4f));
    // Snap 0.1: 1.37 -> 1.4.
    GizmoSnap snap; snap.enabled = true; snap.scale = 0.1f;
    GizmoTransform rsn = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, v, {500,300}, {400 + 137.0f, 300}, snap);
    CHECK_THAT(rsn.scale.x, WithinAbs(1.4f, 1e-4f));
    // A snap that lands on -0 must not erase the mirror: the clamp takes the START's sign (fix round 1).
    GizmoTransform rmz = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, mirrored, v, {300,300}, {403,300}, snap);
    CHECK_THAT(rmz.scale.x, WithinAbs(-0.01f, 1e-6f));
    // The same X drag in PERSPECTIVE gives the same factor: the ratio is taken in pixels.
    const ViewTransform p = Persp();
    const glm::vec2 pv = Px(p, {0,0,0}), tip = Px(p, {1,0,0});
    GizmoTransform rpx = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, p, tip, pv + (tip - pv) * 2.0f, noSnap);
    CHECK_THAT(rpx.scale.x, WithinAbs(2.0f, 1e-3f));
}

TEST_CASE("Gizmo HitTest: 2D view, planar mask -- axes, the XY square, the centre, the Z ring, and a miss", "[gizmo]")
{
    const ViewTransform v = Ortho();
    const GizmoTransform t;   // pivot at (400,300); R = 80 px at size 1
    const float size = 1.0f;
    const GizmoHandleMask tr = GizmoHandleMask::Planar(GizmoMode::Translate);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {460, 302}) == GizmoAxis::X);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {398, 240}) == GizmoAxis::Y);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {440, 260}) == GizmoAxis::XY);   // the square spans 0.35R..0.65R on both axes
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {403, 297}) == GizmoAxis::Center);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {600, 100}) == GizmoAxis::None);
    // The Z arrow is MASKED in 2D (it would project onto the pivot anyway).
    CHECK_FALSE(tr.Has(GizmoAxis::Z)); CHECK_FALSE(tr.Has(GizmoAxis::YZ)); CHECK_FALSE(tr.Has(GizmoAxis::XZ)); CHECK_FALSE(tr.Has(GizmoAxis::Screen));
    // Rotate: only the Z ring (radius 0.8R = 64 px) exists in 2D.
    const GizmoHandleMask ro = GizmoHandleMask::Planar(GizmoMode::Rotate);
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, ro, size, {464, 300}) == GizmoAxis::Z);
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, ro, size, {445, 300}) == GizmoAxis::None);   // 19 px inside the band
    // Scale: X box at the tip.
    const GizmoHandleMask sc = GizmoHandleMask::Planar(GizmoMode::Scale);
    CHECK(HitTest(GizmoMode::Scale, GizmoSpace::World, t, v, sc, size, {478, 301}) == GizmoAxis::X);
    // Gizmo size 2: the X tip is at 560 px; 460 is now mid-shaft and still X, 700 is a miss.
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, 2.0f, {556, 300}) == GizmoAxis::X);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, 2.0f, {700, 300}) == GizmoAxis::None);
}

TEST_CASE("Gizmo HitTest: oblique perspective -- every handle is where it projects", "[gizmo]")
{
    const ViewTransform v = Oblique();
    const GizmoTransform t;
    const GizmoHandleMask all = GizmoHandleMask::All();
    const float R = 80.0f * WorldUnitsPerPixel(v, {0,0,0});
    // Each arrow's projected mid-shaft hits its axis.
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {R * 0.6f, 0, 0})) == GizmoAxis::X);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, R * 0.6f, 0})) == GizmoAxis::Y);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, 0, R * 0.6f})) == GizmoAxis::Z);
    // Each plane square's projected centre hits its plane.
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {R * 0.5f, R * 0.5f, 0})) == GizmoAxis::XY);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, R * 0.5f, R * 0.5f})) == GizmoAxis::YZ);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {R * 0.5f, 0, R * 0.5f})) == GizmoAxis::XZ);
    // A point on each ring hits that ring (ring radius 0.8R).
    const GizmoAxis onXZ = HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, 0.8f * R, 0}));
    CHECK((onXZ == GizmoAxis::X || onXZ == GizmoAxis::Z));   // (0,0.8R,0) lies on BOTH the X and Z rings; the more camera-facing wins (Catch2 cannot decompose an || of ==s)
    // (-0.57R, 0, +0.57R): only the Y ring passes here -- the (+,0,+) point is
    // 7 px from the PROJECTED X ring in this view, inside the 8 px band, and
    // the more camera-facing X ring is tried first.
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {-0.8f * R * 0.7071f, 0, 0.8f * R * 0.7071f})) == GizmoAxis::Y);
    // The screen ring: a pixel circle of kScreenRingRadiusPx (76) around the pivot.
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0,0,0}) + glm::vec2(76.0f, 0.0f)) == GizmoAxis::Screen);
    // LOCAL space with a turned entity: the X arrow follows the local X.
    GizmoTransform turned; turned.rotation = glm::angleAxis(kPi * 0.5f, glm::vec3(0, 0, 1));   // local X = world +Y
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::Local, turned, v, all, 1.0f, Px(v, {0, R * 0.6f, 0})) == GizmoAxis::X);
}

TEST_CASE("Gizmo HitTest: the plane squares sit in the quadrant FACING the camera and hide when edge-on", "[gizmo]")
{
    // The desk finding (2026-09-18): world-anchored +,+ squares wandered behind
    // the pivot and under the arrows as the camera orbited. Unreal's answer:
    // flip each spanning axis toward the eye, hide a square whose plane is
    // nearly edge-on.
    const GizmoTransform t;
    const GizmoHandleMask all = GizmoHandleMask::All();
    // Eye at (-4, 3, 6): the XY square must sit at (-x, +y), not (+x, +y).
    const ViewTransform left = ViewTransform::Perspective({-4.0f, 3.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f, {800u, 600u}, 0.1f, 100.0f);
    const float R = 80.0f * WorldUnitsPerPixel(left, {0,0,0});
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, left, all, 1.0f, Px(left, {-R * 0.5f, R * 0.5f, 0})) == GizmoAxis::XY);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, left, all, 1.0f, Px(left, { R * 0.5f, R * 0.5f, 0})) != GizmoAxis::XY);
    // ...and the XZ square at (-x, +z), the YZ square at (+y, +z).
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, left, all, 1.0f, Px(left, {-R * 0.5f, 0, R * 0.5f})) == GizmoAxis::XZ);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, left, all, 1.0f, Px(left, {0, R * 0.5f, R * 0.5f})) == GizmoAxis::YZ);
    // Looking straight down -Z the XZ and YZ planes are edge-on: never a target.
    const ViewTransform front = Persp();
    const float Rf = 80.0f * WorldUnitsPerPixel(front, {0,0,0});
    for (float sx : { -1.0f, 1.0f })
    {
        CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, front, all, 1.0f, Px(front, {sx * Rf * 0.5f, 0, Rf * 0.5f})) != GizmoAxis::XZ);
        CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, front, all, 1.0f, Px(front, {0, sx * Rf * 0.5f, Rf * 0.5f})) != GizmoAxis::YZ);
    }
    // The 2D view is unchanged: the XY square stays at (+x, +y) (the eye is on +Z, nothing flips).
    const ViewTransform o = Ortho();
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, o, GizmoHandleMask::Planar(GizmoMode::Translate), 1.0f, {440, 260}) == GizmoAxis::XY);
}

TEST_CASE("Gizmo: a pivot BEHIND the eye is neither hit nor scaled -- no phantom gizmo through the viewport centre", "[gizmo]")
{
    // Persp() has its eye at z = 6 looking down -Z; a pivot at z = 7 is behind it.
    // WorldToScreen still returns a FINITE pixel for it (mirrored through the
    // centre) -- that pixel must not be a gizmo (fix round 1).
    const ViewTransform v = Persp();
    GizmoTransform t; t.position = {0.5f, 0.25f, 7.0f};
    const glm::vec2 phantom = Px(v, t.position);
    REQUIRE(std::isfinite(phantom.x)); REQUIRE(std::isfinite(phantom.y));
    const GizmoHandleMask all = GizmoHandleMask::All();
    for (GizmoMode mode : { GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale })
        for (const glm::vec2 probe : { phantom, phantom + glm::vec2(40.0f, 0.0f), phantom + glm::vec2(0.0f, -40.0f), phantom + glm::vec2(64.0f, 0.0f), phantom + glm::vec2(76.0f, 0.0f) })
            CHECK(HitTest(mode, GizmoSpace::World, t, v, all, 1.0f, probe) == GizmoAxis::None);
    const GizmoTransform rs = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, t, v, phantom + glm::vec2(50.0f, 0.0f), phantom + glm::vec2(100.0f, 0.0f), GizmoSnap{});
    CHECK(rs.scale == t.scale);
    // The same pivot in FRONT of the eye is an ordinary gizmo.
    t.position.z = 0.0f;
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, t.position)) == GizmoAxis::Center);
}

TEST_CASE("Gizmo group delta: translate shared, rotate ORBITS about the pivot, scale moves along the pivot ray, replay reproduces", "[gizmo]")
{
    GizmoTransform start; start.position = {1, 2, 3};
    GizmoTransform end = start; end.position = {4, 2, 3};
    end.rotation = glm::angleAxis(kPi * 0.5f, glm::vec3(0, 0, 1)); end.scale = {2, 2, 2};
    const GizmoGroupDelta d = MakeGroupDelta(start, end);
    CHECK(d.translate == glm::vec3(3, 0, 0)); CHECK(d.pivot == start.position); CHECK(d.scale == glm::vec3(2, 2, 2));
    // A member 1 m along +X from the pivot: scaled to 2 m, turned to +Y, then shifted.
    GizmoTransform other; other.position = {2, 2, 3};
    const GizmoTransform o = ApplyGroupDelta(other, d);
    CHECK_THAT(o.position.x, WithinAbs(1.0f + 3.0f, 1e-4f)); CHECK_THAT(o.position.y, WithinAbs(2.0f + 2.0f, 1e-4f)); CHECK_THAT(o.position.z, WithinAbs(3.0f, 1e-4f));
    CHECK(NearQuat(o.rotation, end.rotation)); CHECK(o.scale == glm::vec3(2, 2, 2));
    // Replaying onto the primary's own start reproduces `end` exactly.
    const GizmoTransform p = ApplyGroupDelta(start, d);
    CHECK_THAT(glm::length(p.position - end.position), WithinAbs(0.0f, 1e-5f)); CHECK(NearQuat(p.rotation, end.rotation)); CHECK(p.scale == end.scale);
    // A degenerate start scale yields ratio 1, not infinity.
    GizmoTransform z = start; z.scale = {0, 1, 1}; GizmoTransform ze = z; ze.scale = {5, 1, 1};
    CHECK(MakeGroupDelta(z, ze).scale.x == 1.0f);
}

TEST_CASE("Gizmo DecomposeTRS/ComposeTRS: 3D round trip, matches Transform::ToMatrix, determinant-aware for a mirror", "[gizmo]")
{
    GizmoTransform t; t.position = {1.5f, -2.0f, 0.25f}; t.rotation = glm::normalize(glm::quat(0.9f, 0.1f, 0.3f, -0.2f)); t.scale = {2.0f, 0.5f, 3.0f};
    const glm::mat4 m = ComposeTRS(t);
    Transform tf; tf.position = t.position; tf.rotation = t.rotation; tf.scale = t.scale;
    const glm::mat4 ref = tf.ToMatrix();
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK_THAT(m[c][r], WithinAbs(ref[c][r], 1e-5f));
    const GizmoTransform back = DecomposeTRS(m);
    CHECK_THAT(glm::length(back.position - t.position), WithinAbs(0.0f, 1e-5f));
    CHECK(NearQuat(back.rotation, t.rotation)); CHECK_THAT(glm::length(back.scale - t.scale), WithinAbs(0.0f, 1e-4f));
    // A Y-mirrored sprite (scale (1,-1,1)) decomposes to a NEGATIVE X (UE's convention) plus a half turn --
    // the same matrix -- and WithMirrorOn(1) re-homes the mirror onto Y with the authored rotation back.
    GizmoTransform my; my.scale = {1, -1, 1}; my.rotation = glm::angleAxis(0.4f, glm::vec3(0, 0, 1));
    const glm::mat4 mm = ComposeTRS(my);
    const GizmoTransform dm = DecomposeTRS(mm);
    CHECK(dm.scale.x < 0.0f); CHECK(dm.scale.y > 0.0f);
    const glm::mat4 again = ComposeTRS(dm);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK_THAT(again[c][r], WithinAbs(mm[c][r], 1e-5f));
    const GizmoTransform rehomed = WithMirrorOn(dm, 1);
    CHECK_THAT(rehomed.scale.x, WithinAbs(1.0f, 1e-5f)); CHECK_THAT(rehomed.scale.y, WithinAbs(-1.0f, 1e-5f));
    CHECK(NearQuat(rehomed.rotation, my.rotation, 1e-4f));
    const glm::mat4 same = ComposeTRS(rehomed);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK_THAT(same[c][r], WithinAbs(mm[c][r], 1e-5f));
    // WithMirrorOn is the identity when there is nothing to move (positive X) or the axis is 0.
    CHECK(WithMirrorOn(t, 1).scale == t.scale); CHECK(WithMirrorOn(dm, 0).scale == dm.scale);
}

// ---- rapidcheck: the spec s9 properties --------------------------------------
namespace
{
    glm::quat ArbQuat() { const auto ax = glm::normalize(glm::vec3(float(*rc::gen::inRange(-100, 100)) + 0.5f, float(*rc::gen::inRange(-100, 100)) + 0.25f, float(*rc::gen::inRange(-100, 100)) + 0.125f)); return glm::angleAxis(float(*rc::gen::inRange(-314, 314)) / 100.0f, ax); }
    glm::vec3 ArbVec(int lo, int hi, float div) { return { float(*rc::gen::inRange(lo, hi)) / div, float(*rc::gen::inRange(lo, hi)) / div, float(*rc::gen::inRange(lo, hi)) / div }; }
    glm::vec2 ArbPixel() { return { float(*rc::gen::inRange(50, 750)), float(*rc::gen::inRange(50, 550)) }; }
}

TEST_CASE("Gizmo property: an axis drag moves only along that axis, in both projections", "[gizmo]")
{
    rc::prop("axis-only motion", [] {
        GizmoTransform start; start.position = ArbVec(-200, 200, 100.0f); start.rotation = ArbQuat();
        const int which = *rc::gen::inRange(0, 3);
        const GizmoAxis axis = which == 0 ? GizmoAxis::X : which == 1 ? GizmoAxis::Y : GizmoAxis::Z;
        const GizmoSpace space = *rc::gen::arbitrary<bool>() ? GizmoSpace::World : GizmoSpace::Local;
        const glm::vec3 unit = axis == GizmoAxis::X ? glm::vec3(1,0,0) : axis == GizmoAxis::Y ? glm::vec3(0,1,0) : glm::vec3(0,0,1);
        const glm::vec3 dir = space == GizmoSpace::Local ? start.rotation * unit : unit;
        for (const ViewTransform& v : { Ortho(), Oblique() })
        {
            const GizmoTransform r = ApplyDrag(GizmoMode::Translate, space, axis, start, v, ArbPixel(), ArbPixel(), GizmoSnap{});
            const glm::vec3 delta = r.position - start.position;
            RC_ASSERT(glm::length(glm::cross(delta, dir)) <= 1e-3f * (1.0f + glm::length(delta)));   // parallel to the axis
            RC_ASSERT(NearQuat(r.rotation, start.rotation)); RC_ASSERT(r.scale == start.scale);
        }
    });
}

TEST_CASE("Gizmo property: a plane drag keeps the grabbed point under the cursor -- ortho and perspective agree", "[gizmo]")
{
    rc::prop("grabbed point follows the cursor", [] {
        GizmoTransform start; start.position = ArbVec(-100, 100, 100.0f);
        const glm::vec2 m0 = ArbPixel(), m1 = ArbPixel();
        for (const ViewTransform& v : { Ortho(), Oblique() })
        {
            const auto g0 = RayPlane(v.ScreenToRay(m0), start.position, glm::vec3(0, 0, 1));
            RC_PRE(g0.has_value());
            const auto g1 = RayPlane(v.ScreenToRay(m1), start.position, glm::vec3(0, 0, 1));
            RC_PRE(g1.has_value());
            const GizmoTransform r = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::XY, start, v, m0, m1, GizmoSnap{});
            const glm::vec2 px = Px(v, *g0 + (r.position - start.position));
            RC_ASSERT(glm::length(px - m1) < 0.5f);   // half a pixel: inverse(VP) and back in float
            RC_ASSERT(std::abs(r.position.z - start.position.z) < 1e-6f);   // Z untouched, exactly
        }
    });
}

TEST_CASE("Gizmo property: Compose(Decompose(M)) == M for any TRS, mirrored or not; components round-trip for positive scale", "[gizmo]")
{
    rc::prop("decompose/compose", [] {
        GizmoTransform t; t.position = ArbVec(-1000, 1000, 10.0f); t.rotation = ArbQuat();
        t.scale = { float(*rc::gen::inRange(1, 500)) / 100.0f, float(*rc::gen::inRange(1, 500)) / 100.0f, float(*rc::gen::inRange(1, 500)) / 100.0f };
        const int mirror = *rc::gen::inRange(-1, 3);   // -1 none, else that axis negated
        if (mirror >= 0) t.scale[mirror] = -t.scale[mirror];
        const glm::mat4 m = ComposeTRS(t);
        const GizmoTransform d = DecomposeTRS(m);
        const glm::mat4 back = ComposeTRS(d);
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) RC_ASSERT(std::abs(back[c][r] - m[c][r]) < 1e-3f * (1.0f + std::abs(m[c][r])));
        if (mirror < 0) { RC_ASSERT(NearQuat(d.rotation, t.rotation, 1e-3f)); RC_ASSERT(glm::length(d.scale - t.scale) < 1e-3f * glm::length(t.scale)); }
        else { const GizmoTransform h = WithMirrorOn(d, mirror); RC_ASSERT(glm::length(h.scale - t.scale) < 1e-3f * glm::length(t.scale)); RC_ASSERT(NearQuat(h.rotation, t.rotation, 1e-3f)); }
    });
}

TEST_CASE("Gizmo property: replaying a group delta onto the primary reproduces the drag result", "[gizmo]")
{
    rc::prop("replay", [] {
        GizmoTransform s; s.position = ArbVec(-100, 100, 10.0f); s.rotation = ArbQuat(); s.scale = ArbVec(10, 300, 100.0f);
        GizmoTransform e; e.position = ArbVec(-100, 100, 10.0f); e.rotation = ArbQuat(); e.scale = ArbVec(10, 300, 100.0f);
        const GizmoTransform p = ApplyGroupDelta(s, MakeGroupDelta(s, e));
        RC_ASSERT(glm::length(p.position - e.position) < 1e-3f); RC_ASSERT(NearQuat(p.rotation, e.rotation, 1e-3f)); RC_ASSERT(glm::length(p.scale - e.scale) < 1e-3f);
    });
}
