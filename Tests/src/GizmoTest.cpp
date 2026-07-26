// Arcane transform-gizmo core ([gizmo], CPU-only). Pure value tests over
// GizmoTransform -- no Registry, no graphics device.

#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/glm.hpp>

#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform::ToMatrix -- pins ComposeTRS against it

using Catch::Matchers::WithinAbs;

namespace
{
    // Simple view: origin (400,300), 100 px per world-unit, no Y-flip (matches the
    // engine canonical screen = world*scale + offset). worldToScreen(w) = (400 + w.x*100, 300 + w.y*100).
    Arcane::GizmoView MakeView()
    {
        Arcane::GizmoView v;
        v.cameraOffset        = glm::vec2(400.0f, 300.0f);
        v.worldToScreenScale  = 100.0f;
        return v;
    }
}

TEST_CASE("Gizmo ApplyDrag: translate world axis + center", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;                 // pos (0,0), rot 0, scale (1,1)
    const Arcane::GizmoSnap noSnap;               // enabled=false

    // Drag X: mouse (400,300)->(450,300) == world (0,0)->(0.5,0).
    Arcane::GizmoTransform rx = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::X,
        start, v, glm::vec2(400, 300), glm::vec2(450, 300), noSnap);
    CHECK_THAT(rx.position.x, WithinAbs(0.5f, 1e-4f));
    CHECK_THAT(rx.position.y, WithinAbs(0.0f, 1e-4f));

    // Center: mouse (400,300)->(450,350) == world (0,0)->(0.5,0.5).
    Arcane::GizmoTransform rc = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(400, 300), glm::vec2(450, 350), noSnap);
    CHECK_THAT(rc.position.x, WithinAbs(0.5f, 1e-4f));
    CHECK_THAT(rc.position.y, WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("Gizmo ApplyDrag: translate local axis rotates the direction", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;
    start.rotation = 3.14159265f * 0.5f;          // 90deg -> local X points +Y
    const Arcane::GizmoSnap noSnap;

    // Drag world delta (0.5, 0.3); local-X = (0,1) so only the Y component projects.
    Arcane::GizmoTransform r = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(400, 300), glm::vec2(450, 330), noSnap);
    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(r.position.y, WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("Gizmo ApplyDrag: rotate delta-angle + snap", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;                 // rotation 0, pivot (0,0)
    Arcane::GizmoSnap noSnap;

    // Mouse from world (1,0) [angle 0] to (0,1) [angle +90deg].
    Arcane::GizmoTransform r = Arcane::ApplyDrag(
        Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400, 400), noSnap);
    CHECK_THAT(r.rotation, WithinAbs(3.14159265f * 0.5f, 1e-3f));

    // Snap 15deg: rotate ~20deg -> 15deg. cos/sin(20deg)=(0.9397,0.3420).
    Arcane::GizmoSnap snap; snap.enabled = true; snap.rotationDeg = 15.0f;
    Arcane::GizmoTransform rs = Arcane::ApplyDrag(
        Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400 + 93.97f, 300 + 34.20f), snap);
    CHECK_THAT(rs.rotation, WithinAbs(3.14159265f / 12.0f, 1e-3f));   // 15deg
}

TEST_CASE("Gizmo ApplyDrag: scale ratio, uniform, clamp, snap", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;                 // scale (1,1), rot 0
    Arcane::GizmoSnap noSnap;

    // Axis X: world (1,0)->(2,0) => factor 2 on x only.
    Arcane::GizmoTransform rx = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(500, 300), glm::vec2(600, 300), noSnap);
    CHECK_THAT(rx.scale.x, WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(rx.scale.y, WithinAbs(1.0f, 1e-4f));

    // Center uniform: |world| 1 -> 2 => (2,2).
    Arcane::GizmoTransform rc = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400, 100), noSnap);
    CHECK_THAT(rc.scale.x, WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(rc.scale.y, WithinAbs(2.0f, 1e-4f));

    // Clamp: dragging the axis point onto the pivot => factor 0 => clamped > 0.
    Arcane::GizmoTransform rz = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(500, 300), glm::vec2(400, 300), noSnap);
    CHECK(rz.scale.x > 0.0f);

    // Snap 0.1: factor 1.37 -> 1.4.
    Arcane::GizmoSnap snap; snap.enabled = true; snap.scale = 0.1f;
    Arcane::GizmoTransform rsn = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(500, 300), glm::vec2(400 + 137.0f, 300), snap);
    CHECK_THAT(rsn.scale.x, WithinAbs(1.4f, 1e-4f));
}

TEST_CASE("Gizmo HitTest: translate axes, center, and miss", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform t;                      // pivot screen (400,300)

    // On the +X arrow (pivot .. pivot+~80px right).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(450, 300)) == Arcane::GizmoAxis::X);
    // On the +Y arrow (screen-down = +Y screen, no flip).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(400, 340)) == Arcane::GizmoAxis::Y);
    // On the center handle (priority over axes).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(401, 301)) == Arcane::GizmoAxis::Center);
    // Off-gizmo.
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(700, 300)) == Arcane::GizmoAxis::None);
}

TEST_CASE("Gizmo HitTest: rotate ring band, scale axes", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform t;                      // pivot screen (400,300)

    // Rotate: ring radius ~64px -> a point ~64px from pivot hits (reported Center).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(464, 300)) == Arcane::GizmoAxis::Center);
    // Rotate: near the pivot (inside the ring) misses.
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(410, 300)) == Arcane::GizmoAxis::None);
    // Scale: on the +X handle.
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, t, v,
                          glm::vec2(450, 300)) == Arcane::GizmoAxis::X);
}

TEST_CASE("Gizmo group delta: translate is shared, pivot-independent", "[gizmo]")
{
    // Translate mode: ApplyDrag moves only position, so the delta carries a
    // pure translation and every member shifts by the same world vector.
    const Arcane::GizmoTransform start{ glm::vec2(1.0f, 1.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.position = glm::vec2(4.0f, -1.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.translate.x, WithinAbs(3.0f, 1e-5f));
    CHECK_THAT(d.translate.y, WithinAbs(-2.0f, 1e-5f));

    const Arcane::GizmoTransform other{ glm::vec2(-5.0f, 10.0f), 0.5f, glm::vec2(2.0f, 3.0f) };
    const Arcane::GizmoTransform moved = Arcane::ApplyGroupDelta(other, d);
    CHECK_THAT(moved.position.x, WithinAbs(-2.0f, 1e-5f));
    CHECK_THAT(moved.position.y, WithinAbs(8.0f, 1e-5f));
    CHECK_THAT(moved.rotation, WithinAbs(0.5f, 1e-5f));   // untouched
    CHECK_THAT(moved.scale.x, WithinAbs(2.0f, 1e-5f));    // untouched
}

TEST_CASE("Gizmo group delta: rotate ORBITS others about the primary's pivot", "[gizmo]")
{
    // The whole point of a group rotate: a member one unit to the +X of the
    // pivot swings to +Y under a quarter turn, AND spins by the same angle.
    const float quarter = 1.5707963268f;
    const Arcane::GizmoTransform start{ glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.rotation = quarter;

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.rotate, WithinAbs(quarter, 1e-5f));
    CHECK_THAT(d.translate.x, WithinAbs(0.0f, 1e-5f));   // rotate does not translate

    const Arcane::GizmoTransform other{ glm::vec2(1.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(other, d);
    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(r.position.y, WithinAbs(1.0f, 1e-5f));    // orbited
    CHECK_THAT(r.rotation, WithinAbs(quarter, 1e-5f));   // and spun
}

TEST_CASE("Gizmo group delta: scale multiplies and moves others along the pivot ray", "[gizmo]")
{
    const Arcane::GizmoTransform start{ glm::vec2(2.0f, 2.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.scale = glm::vec2(2.0f, 2.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.scale.x, WithinAbs(2.0f, 1e-5f));

    const Arcane::GizmoTransform other{ glm::vec2(3.0f, 2.0f), 0.0f, glm::vec2(4.0f, 0.5f) };
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(other, d);
    CHECK_THAT(r.position.x, WithinAbs(4.0f, 1e-5f));    // pivot + 1*2
    CHECK_THAT(r.position.y, WithinAbs(2.0f, 1e-5f));    // on the pivot line: unmoved
    CHECK_THAT(r.scale.x, WithinAbs(8.0f, 1e-5f));       // 4 * 2
    CHECK_THAT(r.scale.y, WithinAbs(1.0f, 1e-5f));       // 0.5 * 2
}

TEST_CASE("Gizmo group delta: replaying onto the primary reproduces ApplyDrag", "[gizmo]")
{
    // EditorApp applies the delta UNIFORMLY across the selection, primary
    // included -- that is only sound if the primary round-trips exactly.
    const Arcane::GizmoTransform start{ glm::vec2(1.0f, -2.0f), 0.3f, glm::vec2(2.0f, 0.5f) };
    Arcane::GizmoTransform end{ glm::vec2(4.0f, 1.0f), 1.1f, glm::vec2(3.0f, 1.5f) };

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(start, d);
    CHECK_THAT(r.position.x, WithinAbs(end.position.x, 1e-5f));
    CHECK_THAT(r.position.y, WithinAbs(end.position.y, 1e-5f));
    CHECK_THAT(r.rotation, WithinAbs(end.rotation, 1e-5f));
    CHECK_THAT(r.scale.x, WithinAbs(end.scale.x, 1e-5f));
    CHECK_THAT(r.scale.y, WithinAbs(end.scale.y, 1e-5f));
}

TEST_CASE("Gizmo group delta: a degenerate start scale yields ratio 1, not infinity", "[gizmo]")
{
    const Arcane::GizmoTransform start{ glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(0.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.scale = glm::vec2(5.0f, 2.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.scale.x, WithinAbs(1.0f, 1e-5f));   // guarded
    CHECK_THAT(d.scale.y, WithinAbs(2.0f, 1e-5f));
}

TEST_CASE("Gizmo DecomposeTRS(ComposeTRS(t)) round-trips a non-trivial pose", "[gizmo]")
{
    const Arcane::GizmoTransform t{ glm::vec2(3.5f, -2.25f), 0.7f, glm::vec2(2.0f, 0.5f) };
    const glm::mat3 m = Arcane::ComposeTRS(t);
    const Arcane::GizmoTransform r = Arcane::DecomposeTRS(m);

    CHECK_THAT(r.position.x, WithinAbs(t.position.x, 1e-5f));
    CHECK_THAT(r.position.y, WithinAbs(t.position.y, 1e-5f));
    CHECK_THAT(r.rotation,   WithinAbs(t.rotation, 1e-5f));
    CHECK_THAT(r.scale.x,    WithinAbs(t.scale.x, 1e-5f));
    CHECK_THAT(r.scale.y,    WithinAbs(t.scale.y, 1e-5f));
}

TEST_CASE("Gizmo ComposeTRS matches Transform::ToMatrix for the same pose", "[gizmo]")
{
    // Pins the two composers against each other so they cannot drift --
    // ComposeTRS is a hand-written duplicate of Transform::ToMatrix (Gizmo
    // has no Scene dependency), so this is the only thing keeping them in sync.
    const Arcane::GizmoTransform t{ glm::vec2(-1.5f, 4.0f), 0.7f, glm::vec2(2.0f, 0.5f) };
    const glm::mat3 gizmoM = Arcane::ComposeTRS(t);

    Arcane::Transform xform;
    xform.position = t.position;
    xform.rotation = t.rotation;
    xform.scale    = t.scale;
    const glm::mat3 xformM = xform.ToMatrix();

    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            CHECK_THAT(gizmoM[col][row], WithinAbs(xformM[col][row], 1e-6f));
}

TEST_CASE("Gizmo group delta: non-uniform scale is applied BEFORE the rotation", "[gizmo]")
{
    // Order matters the moment scale is non-uniform and rotation is non-zero:
    // scale-then-rotate and rotate-then-scale diverge, and every other test
    // here is order-invariant (rotate=0, or replayed onto the pivot itself).
    // Member 2 units along +X of the pivot, scaled 2x on X only, then turned a
    // quarter: scale-then-rotate lands it at (0,4); rotate-then-scale would
    // land it at (0,2).
    const float quarter = 1.5707963268f;
    const Arcane::GizmoTransform start{ glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.rotation = quarter;
    end.scale    = glm::vec2(2.0f, 1.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    const Arcane::GizmoTransform other{ glm::vec2(2.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(other, d);

    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(r.position.y, WithinAbs(4.0f, 1e-5f));
    CHECK_THAT(r.rotation, WithinAbs(quarter, 1e-5f));
    CHECK_THAT(r.scale.x, WithinAbs(2.0f, 1e-5f));
    CHECK_THAT(r.scale.y, WithinAbs(1.0f, 1e-5f));
}
