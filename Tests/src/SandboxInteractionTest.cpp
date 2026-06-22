// [sandbox] CPU-only: the Sandbox mouse-interaction layer (Task 7).
//
// Drives Arcane::Sandbox::Interaction directly against a real PhysicsWorld + a
// Sandbox::Camera + fabricated InputSnapshots -- NO graphics device, so this is a
// pure CPU test (tag [sandbox], NOT [gpu]). The Interaction::Tick signature takes
// every dependency as a parameter (registry, world, camera, input, dt) so the test
// owns the whole world and asserts exact outcomes through the camera transform.
//
// The Sandbox plugin sources Interaction.cpp + Scenes.cpp compile straight into
// ArcaneTests (added to the project's file list in premake5.lua): they only need
// Arcane/src + Core/src + glm + Astra, all on the test include path. The plugin
// ENTRY (Sandbox.cpp, with the extern "C" exports + g_app) is NOT compiled in --
// only the standalone helper TUs are. This is the same "compile the pure unit, not
// the plugin shell" split the SandboxCameraTest relies on (it includes Camera.hpp
// by relative path).
//
// Cases:
//   (a) Spawn      -- LMB-press over empty space spawns a dynamic body entity at the
//                     cursor world point (entity count up by 1; a PhysicsSystem step
//                     materializes it as a live PhysicsWorld body).
//   (b) Pick+drag  -- LMB-press over an existing body grabs it; moving the cursor +
//                     ticking drives the body toward the new cursor world point.
//   (c) Throw      -- after a drag, LMB-release clears the grab and the body keeps a
//                     nonzero velocity (momentum).
//   (d) Zoom clamp -- repeated zoom-out never drives camera.zoom to 0 (stays >= min),
//                     so Camera::ScreenToWorld (divides by zoom) never blows up.
//   (e) Pan        -- RMB-drag translates camera.offset by the screen-space delta.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../Sandbox/src/Interaction.hpp"
#include "../../Sandbox/src/Scenes.hpp"

#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cmath>
#include <memory>

using Catch::Approx;

namespace
{
    namespace Phys = Arcane::Physics;
    namespace Sbx  = Arcane::Sandbox;

    constexpr float kDt       = 1.0f / 60.0f;
    constexpr float kGravityY = 900.0f;

    // Mouse button bits (sdlButton - 1): LMB=bit0, RMB=bit1 (InputSnapshot.hpp).
    constexpr std::uint8_t kLMB = 0x1;
    constexpr std::uint8_t kRMB = 0x2;

    // ---------------------------------------------------------------------------
    // A minimal sandbox-like world: SceneRoot + a fresh PhysicsResource. Mirrors a
    // (much shrunk) scene builder. The test parents spawned/known bodies under root.
    // ---------------------------------------------------------------------------
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Astra::Entity   root{};

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents(reg);

            Phys::WorldDef wd;
            wd.gravityY = kGravityY;
            reg.SetResource(Arcane::PhysicsResource{
                std::make_unique<Phys::PhysicsWorld>(wd), {}
            });

            root = reg.CreateEntity();
            reg.AddComponent<Arcane::LocalTransform>(root, Arcane::LocalTransform{});
            reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        }

        Phys::PhysicsWorld& Physics()
        {
            return *reg.GetResource<Arcane::PhysicsResource>()->world;
        }

        // Run one PhysicsSystem step so newly-authored entities become live bodies
        // and bodies advance one tick (the CREATE pass mints bodies from
        // RigidBody2D + Collider2D + PhysicsBodyRef).
        void Step()
        {
            Arcane::PhysicsSystem physics(kDt);
            physics(reg);
        }

        // Number of entities carrying a RigidBody2D (a proxy for "bodies in the scene").
        std::size_t BodyEntityCount()
        {
            std::size_t n = 0;
            auto view = reg.CreateView<Arcane::RigidBody2D>();
            view.ForEach([&](Astra::Entity, Arcane::RigidBody2D&) { ++n; });
            return n;
        }
    };

    // Fabricate an InputSnapshot with a cursor at (sx,sy) window-px and `buttons` held.
    Arcane::InputSnapshot Snap(float sx, float sy, std::uint8_t buttons)
    {
        Arcane::InputSnapshot s{};
        s.mouseX        = sx;
        s.mouseY        = sy;
        s.mouseButtons  = buttons;
        return s;
    }

    // Same, but with the ImGui "wants the mouse" capture flag set -- the host sets
    // this when the cursor is over an ImGui widget. The interaction layer must treat
    // mouse-sourced edits (spawn/grab/pan/drag/wheel-zoom) as released while it is set,
    // so a click on the HUD does not reach the world behind it (the click-through bug).
    Arcane::InputSnapshot SnapCaptured(float sx, float sy, std::uint8_t buttons)
    {
        Arcane::InputSnapshot s = Snap(sx, sy, buttons);
        s.wantCaptureMouse = true;
        return s;
    }
}

// ---------------------------------------------------------------------------
// (a) Spawn: LMB-press over empty space spawns a dynamic body at the cursor.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB press on empty space spawns a dynamic body at the cursor", "[sandbox]")
{
    World w;
    Sbx::Camera cam;                 // identity: screen == world
    Sbx::Interaction it;

    // Establish a released-button baseline (no spurious press edge on frame 1).
    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0), kDt);
    const std::size_t before = w.BodyEntityCount();

    // Press LMB over empty space at screen (400, 120) -> world (400, 120) at identity.
    const glm::vec2 cursorPx{400.0f, 120.0f};
    it.Tick(w.reg, w.Physics(), cam, Snap(cursorPx.x, cursorPx.y, kLMB), kDt);

    // A new body-entity exists (spawn creates the Astra entity synchronously).
    const std::size_t after = w.BodyEntityCount();
    CHECK(after == before + 1);

    // It materializes as a live PhysicsWorld body on the next PhysicsSystem step,
    // positioned at ~the cursor world point.
    const std::size_t bodiesBefore = w.Physics().Count();
    w.Step();
    CHECK(w.Physics().Count() > bodiesBefore);

    // Find the spawned entity's body and confirm it sits near the cursor world point
    // (one gravity step has only nudged it a sub-pixel from the spawn point).
    const auto* res = w.reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    bool found = false;
    for (const auto& [entity, handle] : res->entityToBody)
    {
        const Phys::Vec2 p = res->world->Position(handle);
        if (std::abs(p.x - cursorPx.x) < 1.0f && std::abs(p.y - cursorPx.y) < 5.0f)
            found = true;
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// (b) Pick + drag: LMB-press over a body grabs it; moving the cursor drives it.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB press on a body grabs it and dragging moves it toward the cursor", "[sandbox]")
{
    World w;
    Sbx::Camera cam;                 // identity
    Sbx::Interaction it;

    // Place a dynamic body at a known world point and materialize it as a live body.
    const glm::vec2 bodyPos{300.0f, 300.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(20.0f, 20.0f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();                        // CREATE pass mints the body

    // Released baseline.
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, 0), kDt);

    // LMB-press with the cursor ON the body -> grab.
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, kLMB), kDt);
    CHECK(it.IsGrabbing());

    // Drag toward a new cursor point well to the right + up. Each tick drives the
    // body's velocity toward the cursor; the PhysicsSystem step then integrates it.
    const glm::vec2 target{600.0f, 200.0f};
    float prevDist = glm::length(target - bodyPos);
    for (int i = 0; i < 30; ++i)
    {
        it.Tick(w.reg, w.Physics(), cam, Snap(target.x, target.y, kLMB), kDt);
        w.Step();
    }

    // The body moved meaningfully toward the target cursor world point.
    const auto* res = w.reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    REQUIRE(it.GrabbedHandleForTest() != Phys::kInvalidBody);
    const Phys::Vec2 p = res->world->Position(it.GrabbedHandleForTest());
    const float endDist = glm::length(target - glm::vec2(p.x, p.y));
    CHECK(endDist < prevDist);       // closed distance to the cursor
    CHECK(endDist < 60.0f);          // ... and ended up near it (mouse-spring)
}

// ---------------------------------------------------------------------------
// (c) Throw / release: LMB-release clears the grab; the body keeps momentum.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB release clears the grab and the body retains velocity", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const glm::vec2 bodyPos{300.0f, 300.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(20.0f, 20.0f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, 0), kDt);
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, kLMB), kDt);
    REQUIRE(it.IsGrabbing());

    // Drag right so the mouse-spring builds a rightward velocity (don't step physics
    // here -- we want the drive velocity intact at the moment of release).
    glm::vec2 cursor = bodyPos;
    for (int i = 0; i < 5; ++i)
    {
        cursor.x += 40.0f;
        it.Tick(w.reg, w.Physics(), cam, Snap(cursor.x, cursor.y, kLMB), kDt);
    }
    const Phys::BodyHandle grabbed = it.GrabbedHandleForTest();
    REQUIRE(grabbed != Phys::kInvalidBody);
    const Phys::Vec2 velAtRelease = w.Physics().Velocity(grabbed);
    CHECK(velAtRelease.x > 0.0f);    // mouse-spring built rightward momentum

    // Release: grab clears, body is no longer driven.
    it.Tick(w.reg, w.Physics(), cam, Snap(cursor.x, cursor.y, 0), kDt);
    CHECK_FALSE(it.IsGrabbing());
    CHECK(it.GrabbedHandleForTest() == Phys::kInvalidBody);

    // The thrown body keeps a nonzero velocity (momentum carries past release).
    const Phys::Vec2 velAfter = w.Physics().Velocity(grabbed);
    CHECK(std::abs(velAfter.x) > 0.0f);
}

// ---------------------------------------------------------------------------
// (f) BOUNDED-FORCE drag: a grabbed body accelerates under a BOUNDED force, so a
//     far cursor jump cannot fling it to the drag's max speed in a single frame.
//
// The old mouse-spring SET the body velocity to (cursor-bodyPos)/dt (clamped to
// kDragMaxSpeed) -- an instantaneous override that beat the contact solver, so a
// dragged body rammed/penetrated whatever it was dragged into and dumped huge
// momentum into bodies it slid across (the reported "accelerate / pushed in too
// far"). The fix drives the body with a CAPPED impulse the solver can resist, so
// one frame can only change the velocity by at most kDragMaxAccel*dt. With the
// cursor jumped 2000 units in a single tick the old code produced v.x == 4000
// (the clamp); the bounded-force drag produces a far smaller ramped velocity.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: a grabbed body accelerates under a bounded force", "[sandbox]")
{
    World w;
    Sbx::Camera cam;                 // identity
    Sbx::Interaction it;

    const glm::vec2 bodyPos{300.0f, 300.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(20.0f, 20.0f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();                        // materialize the body

    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, 0), kDt);      // baseline
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, kLMB), kDt);   // grab
    REQUIRE(it.IsGrabbing());

    // Jump the cursor 2000 units in ONE tick (no physics step in between).
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x + 2000.0f, bodyPos.y, kLMB), kDt);

    // A single frame cannot fling the body to the drag's max speed: the impulse
    // is capped (kDragMaxAccel*dt), so the velocity ramps rather than snapping to
    // kDragMaxSpeed (4000). The old SetVelocity override produced exactly 4000.
    const Phys::Vec2 v = w.Physics().Velocity(it.GrabbedHandleForTest());
    INFO("one-frame drag velocity x = " << v.x);
    CHECK(std::abs(v.x) < Sbx::Interaction::kDragMaxSpeed * 0.5f);
}

// ---------------------------------------------------------------------------
// (g) ANCHOR drag: a body grabbed OFF-CENTER rotates when dragged. The drive is
//     applied at the GRAB POINT (not the COM), so an off-center pull torques the
//     body -- grab a corner, the body turns to follow the cursor. A COM-only
//     drive would translate it with ZERO rotation.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: dragging a body grabbed off-center rotates it", "[sandbox]")
{
    World w;
    Sbx::Camera cam;                 // identity
    Sbx::Interaction it;

    // A CIRCLE -- free to rotate (Aabb dynamics are fixedRotation). No floor, so
    // the only torque source is the off-center drag (gravity acts at the COM).
    const glm::vec2 bodyPos{300.0f, 300.0f};
    Sbx::SpawnCircle(w.reg, w.root, bodyPos, 24.0f,
                     Phys::BodyType::Dynamic, glm::vec4(1.0f), 1.0f);
    w.Step();                        // materialize the body

    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, 0), kDt);      // baseline
    // Grab OFF-CENTER: the body's right side (+18 in x from the center).
    const glm::vec2 grabPt{bodyPos.x + 18.0f, bodyPos.y};
    it.Tick(w.reg, w.Physics(), cam, Snap(grabPt.x, grabPt.y, kLMB), kDt);
    REQUIRE(it.IsGrabbing());

    // Pull the grabbed (right-side) point straight DOWN: r=+x, F=+y -> a CCW
    // torque about the COM. The body must visibly rotate (angle != 0).
    const glm::vec2 target{grabPt.x, grabPt.y + 80.0f};
    for (int i = 0; i < 24; ++i)
    {
        it.Tick(w.reg, w.Physics(), cam, Snap(target.x, target.y, kLMB), kDt);
        w.Step();
    }

    const Phys::BodyHandle b = it.GrabbedHandleForTest();
    REQUIRE(b != Phys::kInvalidBody);
    INFO("final angle = " << w.Physics().GetAngle(b));
    CHECK(std::abs(w.Physics().GetAngle(b)) > 0.1f);   // off-center drag rotated it
}

// ---------------------------------------------------------------------------
// (d) Zoom clamp: repeated zoom-out never reaches 0 (ScreenToWorld stays safe).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: zoom-out is clamped to a positive minimum", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    // Drive zoom-out many times via the keyboard zoom-out key (option B input).
    Arcane::InputSnapshot s{};
    s.AddKeycode(Sbx::Interaction::kZoomOutKeycode);
    for (int i = 0; i < 500; ++i)
        it.Tick(w.reg, w.Physics(), cam, s, kDt);

    CHECK(cam.zoom >= Sbx::Interaction::kMinZoom);
    CHECK(cam.zoom > 0.0f);

    // ScreenToWorld must stay finite (the carry-forward this clamp protects).
    const glm::vec2 world = cam.ScreenToWorld({640.0f, 360.0f});
    CHECK(std::isfinite(world.x));
    CHECK(std::isfinite(world.y));
}

// ---------------------------------------------------------------------------
// (e) Pan: RMB-drag translates camera.offset by the screen-space mouse delta.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: RMB drag pans the camera by the screen delta", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;
    const glm::vec2 startOffset = cam.offset;

    // Press RMB to anchor the pan (no offset change on the press frame).
    it.Tick(w.reg, w.Physics(), cam, Snap(500.0f, 400.0f, kRMB), kDt);
    CHECK(cam.offset.x == Approx(startOffset.x));
    CHECK(cam.offset.y == Approx(startOffset.y));

    // Drag RMB: cursor moves by (+30, -15) -> offset shifts by the same screen delta.
    it.Tick(w.reg, w.Physics(), cam, Snap(530.0f, 385.0f, kRMB), kDt);
    CHECK(cam.offset.x == Approx(startOffset.x + 30.0f));
    CHECK(cam.offset.y == Approx(startOffset.y - 15.0f));
}

// ---------------------------------------------------------------------------
// (h) CLICK-THROUGH GUARD: when ImGui owns the mouse (wantCaptureMouse), an LMB
//     press over EMPTY SPACE must NOT spawn a body -- the click belongs to the HUD
//     widget under the cursor, not the world behind it. Root cause: Interaction read
//     input.mouseButtons without consulting input.wantCaptureMouse (the host already
//     fills the flag from ImGui::GetIO().WantCaptureMouse).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: ImGui mouse capture suppresses spawn (no click-through)", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0), kDt);   // released baseline
    const std::size_t before = w.BodyEntityCount();

    // LMB press over empty space, but ImGui WANTS the mouse -> the press is the HUD's.
    it.Tick(w.reg, w.Physics(), cam, SnapCaptured(400.0f, 120.0f, kLMB), kDt);

    CHECK(w.BodyEntityCount() == before);   // nothing spawned in the world behind the UI
}

// ---------------------------------------------------------------------------
// (i) CLICK-THROUGH GUARD: when ImGui owns the mouse, an LMB press over a BODY must
//     NOT grab it -- the click belongs to the HUD, not the world.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: ImGui mouse capture suppresses grab (no click-through)", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const glm::vec2 bodyPos{300.0f, 300.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(20.0f, 20.0f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, 0), kDt);   // baseline

    // LMB press ON the body, but ImGui WANTS the mouse -> no grab.
    it.Tick(w.reg, w.Physics(), cam, SnapCaptured(bodyPos.x, bodyPos.y, kLMB), kDt);
    CHECK_FALSE(it.IsGrabbing());
}

// ---------------------------------------------------------------------------
// (j) CLICK-THROUGH GUARD: ImGui mouse capture suppresses RMB PAN -- dragging over a
//     HUD widget must not pan the world camera.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: ImGui mouse capture suppresses pan (no click-through)", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;
    const glm::vec2 startOffset = cam.offset;

    // RMB held across two captured frames: with capture, the pan delta must be ignored.
    it.Tick(w.reg, w.Physics(), cam, SnapCaptured(500.0f, 400.0f, kRMB), kDt);
    it.Tick(w.reg, w.Physics(), cam, SnapCaptured(560.0f, 360.0f, kRMB), kDt);

    CHECK(cam.offset.x == Approx(startOffset.x));   // camera did not pan under the HUD
    CHECK(cam.offset.y == Approx(startOffset.y));
}

// ===========================================================================
// POLYGON-CREATION MODE (ITEM 2)
// ===========================================================================
// In "polygon mode" a left-click in the WORLD adds a vertex instead of spawning
// the default shape; a HUD button then spawns the collected points as a single
// world-direct convex polygon body (Physics::MakePolygon + PhysicsWorld::AddBody).

// ---------------------------------------------------------------------------
// (k) Polygon mode collects clicked WORLD points (not default-shape spawns).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: polygon mode collects clicked world points", "[sandbox]")
{
    World w;
    Sbx::Camera cam;                 // identity: screen == world
    Sbx::Interaction it;

    it.SetPolygonMode(true);
    CHECK(it.IsPolygonMode());
    CHECK(it.PolygonPoints().empty());

    const std::size_t bodiesBefore = w.BodyEntityCount();

    // Three separate clicks (release between each so each is a fresh press edge).
    const glm::vec2 pts[3] = {{200.0f, 500.0f}, {400.0f, 500.0f}, {300.0f, 300.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, Snap(p.x, p.y, 0),    kDt);  // released
        it.Tick(w.reg, w.Physics(), cam, Snap(p.x, p.y, kLMB), kDt);  // press -> add vert
    }

    // Each click added a vertex at the cursor world point; NOTHING was spawned.
    REQUIRE(it.PolygonPoints().size() == 3);
    CHECK(it.PolygonPoints()[0].x == Approx(pts[0].x));
    CHECK(it.PolygonPoints()[0].y == Approx(pts[0].y));
    CHECK(it.PolygonPoints()[2].x == Approx(pts[2].x));
    CHECK(w.BodyEntityCount() == bodiesBefore);   // no default-shape spawn in poly mode
}

// ---------------------------------------------------------------------------
// (l) SpawnPolygon with >= 3 points creates a live world body and clears points.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: SpawnPolygon adds a world-direct polygon body", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SetPolygonMode(true);
    const glm::vec2 pts[3] = {{200.0f, 500.0f}, {400.0f, 500.0f}, {300.0f, 300.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, Snap(p.x, p.y, 0),    kDt);
        it.Tick(w.reg, w.Physics(), cam, Snap(p.x, p.y, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    const std::size_t bodiesBefore = w.Physics().Count();
    const bool spawned = it.SpawnPolygon(w.Physics());
    CHECK(spawned);
    CHECK(w.Physics().Count() == bodiesBefore + 1);   // a live world-direct body
    CHECK(it.PolygonPoints().empty());                // points cleared after spawn
}

// ---------------------------------------------------------------------------
// (m) SpawnPolygon with < 3 points is a no-op (the factory needs >= 3 verts).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: SpawnPolygon with fewer than 3 points does nothing", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SetPolygonMode(true);
    const glm::vec2 pts[2] = {{200.0f, 500.0f}, {400.0f, 500.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, Snap(p.x, p.y, 0),    kDt);
        it.Tick(w.reg, w.Physics(), cam, Snap(p.x, p.y, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 2);

    const std::size_t bodiesBefore = w.Physics().Count();
    const bool spawned = it.SpawnPolygon(w.Physics());
    CHECK_FALSE(spawned);                              // not enough points
    CHECK(w.Physics().Count() == bodiesBefore);        // no body created
    CHECK(it.PolygonPoints().size() == 2);             // points kept (not cleared on no-op)
}

// ---------------------------------------------------------------------------
// (n) ClearPolygonPoints discards the in-progress vertex list.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: ClearPolygonPoints empties the in-progress polygon", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SetPolygonMode(true);
    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0),     kDt);
    it.Tick(w.reg, w.Physics(), cam, Snap(100.0f, 100.0f, kLMB), kDt);
    REQUIRE(it.PolygonPoints().size() == 1);

    it.ClearPolygonPoints();
    CHECK(it.PolygonPoints().empty());
}

// ---------------------------------------------------------------------------
// (o) Polygon mode does NOT grab existing bodies -- a click is always a vertex.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: polygon mode click on a body adds a vertex, not a grab", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const glm::vec2 bodyPos{300.0f, 300.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(20.0f, 20.0f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.SetPolygonMode(true);
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, 0),    kDt);
    it.Tick(w.reg, w.Physics(), cam, Snap(bodyPos.x, bodyPos.y, kLMB), kDt);

    CHECK_FALSE(it.IsGrabbing());                 // no grab in polygon mode
    CHECK(it.PolygonPoints().size() == 1);        // the click became a vertex
}
