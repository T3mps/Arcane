// Arcane transform-gizmo core ([gizmo], CPU-only). Pure value tests over
// GizmoTransform -- no Registry, no graphics device.

#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/glm.hpp>

#include <Arcane/Edit/Gizmo.hpp>

using Catch::Matchers::WithinAbs;

namespace
{
    // Simple view: center (400,300), 100 px/world-unit, no camera offset, Y-flip.
    // worldToScreen(w) = (400 + w.x*100, 300 - w.y*100).
    Arcane::GizmoView MakeView()
    {
        Arcane::GizmoView v;
        v.cameraOffset = glm::vec2(0.0f, 0.0f);
        v.zoom = 1.0f;
        v.pixelsPerMeter = 100.0f;
        v.viewportOriginPx = glm::vec2(0.0f, 0.0f);
        v.viewportSizePx = glm::vec2(800.0f, 600.0f);
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

    // Center: mouse (400,300)->(450,250) == world (0,0)->(0.5,0.5).
    Arcane::GizmoTransform rc = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(400, 300), glm::vec2(450, 250), noSnap);
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
        start, v, glm::vec2(400, 300), glm::vec2(450, 270), noSnap);
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
        start, v, glm::vec2(500, 300), glm::vec2(400, 200), noSnap);
    CHECK_THAT(r.rotation, WithinAbs(3.14159265f * 0.5f, 1e-3f));

    // Snap 15deg: rotate ~20deg -> 15deg. cos/sin(20deg)=(0.9397,0.3420).
    Arcane::GizmoSnap snap; snap.enabled = true; snap.rotationDeg = 15.0f;
    Arcane::GizmoTransform rs = Arcane::ApplyDrag(
        Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400 + 93.97f, 300 - 34.20f), snap);
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
