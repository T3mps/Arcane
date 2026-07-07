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
#include "../../Sandbox/src/SandboxApp.hpp"   // PolygonDraftResource + PolygonDraftRenderSystem

#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Render/Batcher2D.hpp>        // Arcane::Batcher2D virtual interface

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;

namespace
{
    namespace Phys = Arcane::Physics;
    namespace Sbx  = Arcane::Sandbox;

    constexpr float kDt       = 1.0f / 60.0f;
    constexpr float kGravityY = 10.0f;   // MKS: +10 m/s^2 (matches the WorldDef default)

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
            wd.gravityY = kGravityY;   // MKS content inherits the WorldDef defaults for
                                       // sleep/restitution/push/hash (P1 already MKS).
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

    // Feed a WORLD-space point (meters) through the camera to a screen-px cursor
    // snapshot. Interaction converts the cursor screen px back to world via
    // camera.ScreenToWorld, so authoring targets in meters and projecting keeps the
    // cursor in the same unit system as the body positions (identity camera:
    // screen = world * Camera::kPixelsPerMeter = world * 100).
    Arcane::InputSnapshot SnapWorld(const Sbx::Camera& cam, glm::vec2 world,
                                    std::uint8_t buttons)
    {
        const glm::vec2 s = cam.WorldToScreen(world);
        return Snap(s.x, s.y, buttons);
    }

    // As SnapWorld, but with the ImGui capture flag set (see SnapCaptured).
    Arcane::InputSnapshot SnapWorldCaptured(const Sbx::Camera& cam, glm::vec2 world,
                                            std::uint8_t buttons)
    {
        Arcane::InputSnapshot s = SnapWorld(cam, world, buttons);
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
    Sbx::Camera cam;                 // identity: screen = world * 100 (100 px/m)
    Sbx::Interaction it;

    // Establish a released-button baseline (no spurious press edge on frame 1).
    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0), kDt);
    const std::size_t before = w.BodyEntityCount();

    // Press LMB over empty space at world (4.0, 1.2) m -> screen (400, 120) px.
    const glm::vec2 cursorWorld{4.0f, 1.2f};
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, cursorWorld, kLMB), kDt);

    // A new body-entity exists (spawn creates the Astra entity synchronously).
    const std::size_t after = w.BodyEntityCount();
    CHECK(after == before + 1);

    // It materializes as a live PhysicsWorld body on the next PhysicsSystem step,
    // positioned at ~the cursor world point.
    const std::size_t bodiesBefore = w.Physics().Count();
    w.Step();
    CHECK(w.Physics().Count() > bodiesBefore);

    // Find the spawned entity's body and confirm it sits near the cursor world point
    // (one gravity step at g=10 nudges it ~1.4 mm from the spawn point). Tolerances
    // are the px-era 1/5 px windows divided by 100 px/m -> 0.01/0.05 m.
    const auto* res = w.reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    bool found = false;
    for (const auto& [entity, handle] : res->entityToBody)
    {
        const Phys::Vec2 p = res->world->Position(handle);
        if (std::abs(p.x - cursorWorld.x) < 0.01f && std::abs(p.y - cursorWorld.y) < 0.05f)
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
    Sbx::Camera cam;                 // identity: screen = world * 100
    Sbx::Interaction it;

    // Place a dynamic body at a known world point (meters) and materialize it.
    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();                        // CREATE pass mints the body

    // Released baseline.
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, 0), kDt);

    // LMB-press with the cursor ON the body -> grab.
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, kLMB), kDt);
    CHECK(it.IsGrabbing());

    // Drag toward a new cursor world point up + to the right. Each tick drives the
    // body's velocity toward the cursor; the PhysicsSystem step then integrates it.
    //
    // MKS drag: a 1.3 m drag (target (4.2, 2.5) m from (3.0, 3.0) m: sqrt(1.2^2 +
    // 0.5^2) = 1.3 m) over 30 ticks @ 1/60 s = 0.5 s needs an average speed of only
    // 1.3 / 0.5 = 2.6 m/s -- FAR under kDragMaxSpeed = 40 m/s (15x headroom) and
    // nowhere near the WorldDef maxLinearVelocity = 400 m/s clamp. The px-era test
    // fought that clamp (130 u over 0.5 s at 260 u/s against a 400 u/s cap = only
    // 1.5x headroom, hence a loose ~19%-of-distance threshold); at MKS the cap does
    // not bind, so the mouse-spring converges to within a few mm of the cursor.
    const glm::vec2 target{4.2f, 2.5f};
    float prevDist = glm::length(target - bodyPos);
    for (int i = 0; i < 30; ++i)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, target, kLMB), kDt);
        w.Step();
    }

    // The body moved meaningfully toward the target cursor world point.
    const auto* res = w.reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    REQUIRE(it.GrabbedHandleForTest() != Phys::kInvalidBody);
    const Phys::Vec2 p = res->world->Position(it.GrabbedHandleForTest());
    const float endDist = glm::length(target - glm::vec2(p.x, p.y));
    CHECK(endDist < prevDist);       // closed distance to the cursor
    // Re-derived empirically for MKS (protocol rule 6, measure-then-bound): the drag
    // converges to endDist ~ 0.00347 m of the cursor (0.27% of the 1.3 m drag -- vs
    // the px-era ~19%, since kDragMaxSpeed = 40 m/s has 15x headroom over the 2.6 m/s
    // this drag needs, so the velocity cap no longer binds). Bound 0.006 m ~ 1.7x the
    // measured convergence -- driven by kDragMaxSpeed/kDragMaxAccel, not the cap.
    CHECK(endDist < 0.006f);
}

// ---------------------------------------------------------------------------
// (c) Throw / release: LMB-release clears the grab; the body keeps momentum.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB release clears the grab and the body retains velocity", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, 0), kDt);
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, kLMB), kDt);
    REQUIRE(it.IsGrabbing());

    // Drag right so the mouse-spring builds a rightward velocity (don't step physics
    // here -- we want the drive velocity intact at the moment of release). 0.4 m/step.
    glm::vec2 cursor = bodyPos;
    for (int i = 0; i < 5; ++i)
    {
        cursor.x += 0.4f;
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, cursor, kLMB), kDt);
    }
    const Phys::BodyHandle grabbed = it.GrabbedHandleForTest();
    REQUIRE(grabbed != Phys::kInvalidBody);
    const Phys::Vec2 velAtRelease = w.Physics().Velocity(grabbed);
    CHECK(velAtRelease.x > 0.0f);    // mouse-spring built rightward momentum

    // Release: grab clears, body is no longer driven.
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, cursor, 0), kDt);
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
// cursor jumped 20 m in a single tick a velocity override would snap to
// kDragMaxSpeed (40 m/s); the bounded-force drag produces a far smaller ramped
// velocity (~kDragMaxAccel*dt = 400 * 1/60 ~ 6.7 m/s).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: a grabbed body accelerates under a bounded force", "[sandbox]")
{
    World w;
    Sbx::Camera cam;                 // identity: screen = world * 100
    Sbx::Interaction it;

    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();                        // materialize the body

    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, 0), kDt);      // baseline
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, kLMB), kDt);   // grab
    REQUIRE(it.IsGrabbing());

    // Jump the cursor 20 m in ONE tick (no physics step in between).
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {bodyPos.x + 20.0f, bodyPos.y}, kLMB), kDt);

    // A single frame cannot fling the body to the drag's max speed: the impulse
    // is capped (kDragMaxAccel*dt), so the velocity ramps rather than snapping to
    // kDragMaxSpeed (40 m/s). A velocity override would produce exactly 40.
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
    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnCircle(w.reg, w.root, bodyPos, 0.24f,
                     Phys::BodyType::Dynamic, glm::vec4(1.0f), 1.0f);
    w.Step();                        // materialize the body

    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, 0), kDt);      // baseline
    // Grab OFF-CENTER: the body's right side (+0.18 m in x, inside the 0.24 m radius).
    const glm::vec2 grabPt{bodyPos.x + 0.18f, bodyPos.y};
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, grabPt, kLMB), kDt);
    REQUIRE(it.IsGrabbing());

    // Pull the grabbed (right-side) point straight DOWN: r=+x, F=+y -> a CCW
    // torque about the COM. The body must visibly rotate (angle != 0).
    const glm::vec2 target{grabPt.x, grabPt.y + 0.8f};
    for (int i = 0; i < 24; ++i)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, target, kLMB), kDt);
        w.Step();
    }

    const Phys::BodyHandle b = it.GrabbedHandleForTest();
    REQUIRE(b != Phys::kInvalidBody);
    INFO("final angle = " << w.Physics().GetAngle(b));
    CHECK(std::abs(w.Physics().GetAngle(b)) > 0.1f);   // off-center drag rotated it
}

// ---------------------------------------------------------------------------
// (g2) PICK AFFORDANCE anti-shrink: kPickRadiusPx is a FIXED SCREEN-PX radius the
//      camera converts to world each query (worldR = kPickRadiusPx/(100*zoom)), so
//      the affordance does NOT shrink in world space when you zoom out (spec P6).
//      Same body + same cursor gap: it PICKS zoomed out (0.5) but not at zoom 1.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: pick radius is a screen-px affordance that grows when zoomed out", "[sandbox]")
{
    World w;

    // A small body; the cursor sits 0.06 m past its surface -- inside the zoom-0.5
    // world pick radius (4/(100*0.5) = 0.08 m) but OUTSIDE the zoom-1 radius
    // (4/(100*1) = 0.04 m). Grabbing at 0.5 but not at 1 proves the affordance is
    // screen-fixed (it grew in world space as we zoomed out), not world-fixed.
    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnCircle(w.reg, w.root, bodyPos, 0.2f,
                     Phys::BodyType::Dynamic, glm::vec4(1.0f), 1.0f);
    w.Step();
    const glm::vec2 cursorWorld{bodyPos.x + 0.2f + 0.06f, bodyPos.y};   // 0.06 m past surface

    // Zoomed OUT (0.5): world pick radius 0.08 m > 0.06 m gap -> grabs.
    Sbx::Camera camOut; camOut.zoom = 0.5f;
    Sbx::Interaction itOut;
    itOut.Tick(w.reg, w.Physics(), camOut, SnapWorld(camOut, cursorWorld, 0), kDt);
    itOut.Tick(w.reg, w.Physics(), camOut, SnapWorld(camOut, cursorWorld, kLMB), kDt);
    CHECK(itOut.IsGrabbing());

    // Zoom 1: world pick radius 0.04 m < 0.06 m gap -> the same click does NOT grab
    // (a world-fixed 4-unit radius would have; the screen-px affordance shrank).
    Sbx::Camera camIn;   // default zoom 1
    Sbx::Interaction itIn;
    itIn.Tick(w.reg, w.Physics(), camIn, SnapWorld(camIn, cursorWorld, 0), kDt);
    itIn.Tick(w.reg, w.Physics(), camIn, SnapWorld(camIn, cursorWorld, kLMB), kDt);
    CHECK_FALSE(itIn.IsGrabbing());
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
// (p) WHEEL ZOOM (ITEM 3): a positive wheel delta zooms the camera IN, a negative
//     delta zooms OUT. The wheel is consumed from input.wheelY each frame.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: mouse wheel zooms the camera in and out", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const float z0 = cam.zoom;

    // Scroll UP (+wheel) -> zoom IN (zoom increases).
    Arcane::InputSnapshot up{};
    up.mouseX = 640.0f; up.mouseY = 360.0f; up.wheelY = 2.0f;
    it.ApplyWheelZoom(cam, up);
    CHECK(cam.zoom > z0);

    // Scroll DOWN (-wheel) -> zoom OUT (zoom decreases back).
    const float z1 = cam.zoom;
    Arcane::InputSnapshot down{};
    down.mouseX = 640.0f; down.mouseY = 360.0f; down.wheelY = -2.0f;
    it.ApplyWheelZoom(cam, down);
    CHECK(cam.zoom < z1);
}

// ---------------------------------------------------------------------------
// (q) WHEEL ZOOM clamp: a huge accumulated wheel delta never drives zoom out of
//     [kMinZoom, kMaxZoom] (ScreenToWorld divides by zoom, so it must stay > 0).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: wheel zoom is clamped to the zoom range", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    Arcane::InputSnapshot blastIn{};
    blastIn.mouseX = 640.0f; blastIn.mouseY = 360.0f; blastIn.wheelY = 1000.0f;
    it.ApplyWheelZoom(cam, blastIn);
    CHECK(cam.zoom <= Sbx::Interaction::kMaxZoom);

    Arcane::InputSnapshot blastOut{};
    blastOut.mouseX = 640.0f; blastOut.mouseY = 360.0f; blastOut.wheelY = -1000.0f;
    it.ApplyWheelZoom(cam, blastOut);
    CHECK(cam.zoom >= Sbx::Interaction::kMinZoom);
    CHECK(cam.zoom > 0.0f);
}

// ---------------------------------------------------------------------------
// (r) WHEEL ZOOM toward cursor: the WORLD point under the cursor stays fixed
//     across a wheel-zoom step (zoom-to-cursor, like every map/editor). The
//     camera offset is adjusted so screen->world of the cursor is invariant.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: wheel zoom keeps the world point under the cursor fixed", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    cam.offset = glm::vec2(50.0f, -30.0f);   // a non-identity camera so the math bites
    cam.zoom   = 1.5f;
    Sbx::Interaction it;

    const glm::vec2 cursor{700.0f, 300.0f};
    const glm::vec2 worldBefore = cam.ScreenToWorld(cursor);

    Arcane::InputSnapshot up{};
    up.mouseX = cursor.x; up.mouseY = cursor.y; up.wheelY = 3.0f;
    it.ApplyWheelZoom(cam, up);

    const glm::vec2 worldAfter = cam.ScreenToWorld(cursor);
    CHECK(cam.zoom > 1.5f);                       // it actually zoomed
    CHECK(worldAfter.x == Approx(worldBefore.x).margin(0.01f));
    CHECK(worldAfter.y == Approx(worldBefore.y).margin(0.01f));
}

// ---------------------------------------------------------------------------
// (s) WHEEL ZOOM suppressed under ImGui capture: scrolling over a HUD widget must
//     not zoom the world (the click-through guard covers the wheel too).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: ImGui mouse capture suppresses wheel zoom", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;
    const float z0 = cam.zoom;

    Arcane::InputSnapshot s{};
    s.mouseX = 640.0f; s.mouseY = 360.0f; s.wheelY = 4.0f;
    s.wantCaptureMouse = true;                    // ImGui owns the mouse
    it.ApplyWheelZoom(cam, s);
    CHECK(cam.zoom == Approx(z0));                // no world zoom under the HUD
}

// ---------------------------------------------------------------------------
// (s2) The wheel is consumed ONCE PER FRAME (ApplyWheelZoom), NOT in the fixed-step
//      Tick. RunLoop fires Tick 0..N times per host frame, so consuming the per-frame
//      wheel impulse there would drop notches (no fixed step that frame) or double
//      them (several) -- the "wheel zoom not smooth" bug. Guard the contract.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: the fixed-step Tick ignores the wheel; ApplyWheelZoom consumes it", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;
    const float z0 = cam.zoom;

    Arcane::InputSnapshot s{};
    s.mouseX = 640.0f; s.mouseY = 360.0f; s.wheelY = 3.0f;

    // Fixed-step Tick must NOT zoom on the wheel.
    it.Tick(w.reg, w.Physics(), cam, s, kDt);
    CHECK(cam.zoom == Approx(z0));

    // The once-per-frame path DOES apply it (exactly once per call).
    it.ApplyWheelZoom(cam, s);
    CHECK(cam.zoom > z0);
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

    // LMB press over empty space (world (4.0, 1.2) m), but ImGui WANTS the mouse ->
    // the press is the HUD's.
    it.Tick(w.reg, w.Physics(), cam, SnapWorldCaptured(cam, {4.0f, 1.2f}, kLMB), kDt);

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

    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, 0), kDt);   // baseline

    // LMB press ON the body, but ImGui WANTS the mouse -> no grab.
    it.Tick(w.reg, w.Physics(), cam, SnapWorldCaptured(cam, bodyPos, kLMB), kDt);
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
    Sbx::Camera cam;                 // identity: screen = world * 100
    Sbx::Interaction it;

    it.SetPolygonMode(true);
    CHECK(it.IsPolygonMode());
    CHECK(it.PolygonPoints().empty());

    const std::size_t bodiesBefore = w.BodyEntityCount();

    // Three separate clicks (release between each so each is a fresh press edge).
    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);  // released
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);  // press -> add vert
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
    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
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
    const glm::vec2 pts[2] = {{2.0f, 5.0f}, {4.0f, 5.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
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
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {1.0f, 1.0f}, kLMB), kDt);
    REQUIRE(it.PolygonPoints().size() == 1);

    it.ClearPolygonPoints();
    CHECK(it.PolygonPoints().empty());
}

// ---------------------------------------------------------------------------
// (o) Polygon mode, NO points yet: the first click PREFERS interacting with a
//     body under the cursor (grab) over dropping a vertex -- so you can still
//     rearrange the scene before you start authoring a polygon.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: in Polygon mode the first click on a body grabs it (no vertex)", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, 0), kDt);   // released
    REQUIRE(it.PolygonPoints().empty());

    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, kLMB), kDt);
    CHECK(it.IsGrabbing());                         // grabbed the body...
    CHECK(it.PolygonPoints().empty());              // ...and dropped NO vertex
}

// ---------------------------------------------------------------------------
// (o2) Polygon mode, NO points yet: a click on EMPTY space starts the polygon
//      (first vertex; no grab).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: in Polygon mode the first click on empty space places a vertex", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    // A body far from the click point, so the cursor is over empty space.
    Sbx::SpawnBox(w.reg, w.root, glm::vec2(3.0f, 3.0f), glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;
    const glm::vec2 empty{0.4f, 0.4f};
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, empty, 0),    kDt);
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, empty, kLMB), kDt);

    CHECK_FALSE(it.IsGrabbing());
    CHECK(it.PolygonPoints().size() == 1);
}

// ---------------------------------------------------------------------------
// (o3) Once a polygon is IN PROGRESS (>= 1 point), a click on a body adds a
//      vertex -- interaction no longer takes priority (you committed to authoring).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: with a polygon in progress, a click on a body adds a vertex", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    const glm::vec2 bodyPos{3.0f, 3.0f};
    Sbx::SpawnBox(w.reg, w.root, bodyPos, glm::vec2(0.2f, 0.2f),
                  Phys::BodyType::Dynamic, glm::vec4(1.0f));
    w.Step();

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    // Start the polygon on empty space (first vertex).
    const glm::vec2 empty{0.4f, 0.4f};
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, empty, 0),    kDt);
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, empty, kLMB), kDt);
    REQUIRE(it.PolygonPoints().size() == 1);

    // Release, then click ON the body: a point already exists -> it is a vertex.
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, empty, 0),        kDt);
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, bodyPos, kLMB), kDt);
    CHECK_FALSE(it.IsGrabbing());
    CHECK(it.PolygonPoints().size() == 2);
}

// ===========================================================================
// CONVEX HULL ON POLYGON SPAWN (Task 8)
// ===========================================================================
// SpawnPolygon must hull the clicked points before passing them to MakePolygon,
// so any click order (even non-convex / self-crossing) yields a valid convex
// collider. A degenerate hull (collinear points -> < 3 hull verts) stays a
// no-op that keeps the in-progress points intact.

// ---------------------------------------------------------------------------
// (p) Non-convex click order -> ONE convex body.
// ---------------------------------------------------------------------------
TEST_CASE("SpawnPolygon hulls a non-convex click order into one convex body", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SetPolygonMode(true);

    // A square outline with an interior point added mid-list (non-convex click
    // order). The five raw points are NOT a convex polygon; before the hull step
    // MakePolygon would receive a self-intersecting / disordered vertex list.
    // The hull of these five points is the 4-vertex square (interior dropped).
    const glm::vec2 pts[5] = {
        {0.0f, 0.0f},   // corner
        {4.0f, 0.0f},   // corner
        {2.0f, 2.0f},   // interior point
        {4.0f, 4.0f},   // corner
        {0.0f, 4.0f},   // corner
    };
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);   // released
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);   // press -> vertex
    }
    REQUIRE(it.PolygonPoints().size() == 5);

    const std::size_t bodiesBefore = w.Physics().Count();
    const bool spawned = it.SpawnPolygon(w.Physics());
    CHECK(spawned);                                    // hull >= 3 verts -> spawned
    CHECK(w.Physics().Count() == bodiesBefore + 1);   // exactly one new body
    CHECK(it.PolygonPoints().empty());                 // committed -> cleared
}

// ---------------------------------------------------------------------------
// (q) Collinear click set -> no-op (< 3 hull verts), points kept.
// ---------------------------------------------------------------------------
TEST_CASE("SpawnPolygon rejects a collinear click set (keeps points)", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SetPolygonMode(true);

    // Three collinear points -> hull has only 2 (the two extreme endpoints).
    // SpawnPolygon must treat this as a no-op and leave the point list intact.
    const glm::vec2 pts[3] = {
        {0.0f, 0.0f},
        {2.0f, 2.0f},
        {4.0f, 4.0f},
    };
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    const std::size_t bodiesBefore = w.Physics().Count();
    const bool spawned = it.SpawnPolygon(w.Physics());
    CHECK_FALSE(spawned);                               // degenerate hull -> no-op
    CHECK(w.Physics().Count() == bodiesBefore);         // no body created
    CHECK(it.PolygonPoints().size() == 3);              // points kept, not cleared
}

// ===========================================================================
// POLYGON DRAFT RENDER SYSTEM (Task 9)
// ===========================================================================
// PolygonDraftRenderSystem reads PolygonDraftResource from the registry and
// draws a small fixed-pixel circle at each world point projected to screen as
// world*zoom + cameraOffset. CPU-only mock (no GPU device needed).

namespace
{
    // Recording mock Batcher2D: captures Circle() submissions (center + radius)
    // for CPU-only render-system tests. Mirrors the style of RecMock in
    // PhysicsDebugRichTest.cpp but lives here for the sandbox draft-render case.
    struct DraftBatcherMock final : Arcane::Batcher2D
    {
        struct CircleCall { glm::vec2 center; float radius; };
        std::vector<CircleCall> circles;

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*,
                   uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*,
                  glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*,
                   glm::vec2, glm::vec2, glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2 c, float r, glm::vec4) override
        {
            circles.push_back({c, r});
        }
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };
}

// ---------------------------------------------------------------------------
// (r) PolygonDraftRenderSystem draws one marker per draft point at the correct
//     projected screen position (world*zoom + cameraOffset).
// ---------------------------------------------------------------------------
TEST_CASE("PolygonDraftRenderSystem draws one marker per draft point", "[sandbox]")
{
    DraftBatcherMock batcher;

    // Minimal registry with just the two resources the system reads.
    Astra::Registry reg;

    Arcane::RenderContext2D ctx;
    ctx.batcher      = &batcher;
    ctx.zoom         = 2.0f;
    ctx.cameraOffset = glm::vec2(10.0f, 20.0f);
    reg.SetResource(std::move(ctx));

    Arcane::Sandbox::PolygonDraftResource draft;
    draft.worldPoints = { {0.0f, 0.0f}, {5.0f, 0.0f}, {5.0f, 5.0f} };
    reg.SetResource(std::move(draft));

    Arcane::Sandbox::PolygonDraftRenderSystem{}(reg);

    // One Circle call per world point.
    REQUIRE(batcher.circles.size() == 3);

    // First point (0,0) -> screen = (0,0)*2 + (10,20) = (10,20).
    CHECK(batcher.circles[0].center.x == Approx(10.0f));
    CHECK(batcher.circles[0].center.y == Approx(20.0f));

    // Second point (5,0) -> screen = (5,0)*2 + (10,20) = (20,20).
    CHECK(batcher.circles[1].center.x == Approx(20.0f));
    CHECK(batcher.circles[1].center.y == Approx(20.0f));

    // Third point (5,5) -> screen = (5,5)*2 + (10,20) = (20,30).
    CHECK(batcher.circles[2].center.x == Approx(20.0f));
    CHECK(batcher.circles[2].center.y == Approx(30.0f));
}

// ---------------------------------------------------------------------------
// (s) PolygonDraftRenderSystem draws nothing when the resource is absent.
// ---------------------------------------------------------------------------
TEST_CASE("PolygonDraftRenderSystem draws nothing without a resource", "[sandbox]")
{
    DraftBatcherMock batcher;

    Astra::Registry reg;
    Arcane::RenderContext2D ctx;
    ctx.batcher      = &batcher;
    ctx.zoom         = 1.0f;
    ctx.cameraOffset = glm::vec2(0.0f, 0.0f);
    reg.SetResource(std::move(ctx));
    // NO PolygonDraftResource set -> the system must draw nothing.

    Arcane::Sandbox::PolygonDraftRenderSystem{}(reg);

    CHECK(batcher.circles.empty());
}

// ---------------------------------------------------------------------------
// (t) PolygonDraftRenderSystem draws nothing when the world-point list is empty.
// ---------------------------------------------------------------------------
TEST_CASE("PolygonDraftRenderSystem draws nothing when draft is empty", "[sandbox]")
{
    DraftBatcherMock batcher;

    Astra::Registry reg;
    Arcane::RenderContext2D ctx;
    ctx.batcher      = &batcher;
    ctx.zoom         = 1.0f;
    ctx.cameraOffset = glm::vec2(0.0f, 0.0f);
    reg.SetResource(std::move(ctx));

    Arcane::Sandbox::PolygonDraftResource draft;   // empty worldPoints
    reg.SetResource(std::move(draft));

    Arcane::Sandbox::PolygonDraftRenderSystem{}(reg);

    CHECK(batcher.circles.empty());
}

// ===========================================================================
// UNIFIED SPAWN SELECTOR (2026-06-22 spec)
// ===========================================================================
// SpawnShape now has four values: Box, Circle, Capsule, Polygon.
// m_polygonMode is removed; polygon mode is derived from shape == Polygon.
// New Capsule instant-spawn, keyboard shortcuts for polygon, draft gating.

// ---------------------------------------------------------------------------
// (u) Capsule instant-spawn: LMB on empty space with shape == Capsule spawns
//     exactly one body entity (just like Box and Circle).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB press with Capsule shape spawns a capsule body", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Capsule;
    it.SpawnCfg().size  = 0.2f;

    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0), kDt);   // baseline
    const std::size_t before = w.BodyEntityCount();

    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {4.0f, 2.0f}, kLMB), kDt);
    CHECK(w.BodyEntityCount() == before + 1);

    // It materializes on the next step.
    w.Step();
    CHECK(w.Physics().Count() > 0);
}

// ---------------------------------------------------------------------------
// (v) Box instant-spawn still works after the enum extension (regression).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB press with Box shape still spawns a box body", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Box;

    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0), kDt);
    const std::size_t before = w.BodyEntityCount();
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {4.0f, 2.0f}, kLMB), kDt);
    CHECK(w.BodyEntityCount() == before + 1);
}

// ---------------------------------------------------------------------------
// (w) Circle instant-spawn still works after the enum extension (regression).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: LMB press with Circle shape still spawns a circle body", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Circle;

    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0), kDt);
    const std::size_t before = w.BodyEntityCount();
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {4.0f, 2.0f}, kLMB), kDt);
    CHECK(w.BodyEntityCount() == before + 1);
}

// ---------------------------------------------------------------------------
// (x) Shape == Polygon still collects vertices (deriving polygon mode from shape).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: shape Polygon collects clicked vertices", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;
    CHECK(it.IsPolygonMode());

    const std::size_t before = w.BodyEntityCount();
    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }

    CHECK(it.PolygonPoints().size() == 3);
    CHECK(w.BodyEntityCount() == before);   // no entity spawned
}

// ---------------------------------------------------------------------------
// (y) Enter key (>= 3 points) -> polygon spawns + points cleared (edge, once).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: Enter key with >= 3 polygon points spawns and clears", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    // Collect 3 points.
    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    const std::size_t bodiesBefore = w.Physics().Count();

    // Press Enter (edge: prev not down, now down).
    Arcane::InputSnapshot enterSnap{};
    enterSnap.AddKeycode(Sbx::Interaction::kEnterKeycode);
    it.Tick(w.reg, w.Physics(), cam, enterSnap, kDt);

    CHECK(w.Physics().Count() == bodiesBefore + 1);   // polygon was spawned
    CHECK(it.PolygonPoints().empty());                 // points cleared
}

// ---------------------------------------------------------------------------
// (z) Enter key with < 3 points is a no-op (not enough verts).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: Enter key with < 3 polygon points is a no-op", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    // Collect only 2 points.
    const glm::vec2 pts[2] = {{2.0f, 5.0f}, {4.0f, 5.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 2);

    const std::size_t bodiesBefore = w.Physics().Count();

    Arcane::InputSnapshot enterSnap{};
    enterSnap.AddKeycode(Sbx::Interaction::kEnterKeycode);
    it.Tick(w.reg, w.Physics(), cam, enterSnap, kDt);

    CHECK(w.Physics().Count() == bodiesBefore);         // no body spawned
    CHECK(it.PolygonPoints().size() == 2);              // points kept
}

// ---------------------------------------------------------------------------
// (aa) Backspace key pops the last collected point.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: Backspace key pops the last polygon point", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    // Collect 3 points.
    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    Arcane::InputSnapshot bsSnap{};
    bsSnap.AddKeycode(Sbx::Interaction::kBackspaceKeycode);
    it.Tick(w.reg, w.Physics(), cam, bsSnap, kDt);

    CHECK(it.PolygonPoints().size() == 2);   // one popped
}

// ---------------------------------------------------------------------------
// (ab) Backspace on empty list is a no-op (no crash).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: Backspace on empty polygon points is a no-op", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;
    REQUIRE(it.PolygonPoints().empty());

    Arcane::InputSnapshot bsSnap{};
    bsSnap.AddKeycode(Sbx::Interaction::kBackspaceKeycode);
    REQUIRE_NOTHROW(it.Tick(w.reg, w.Physics(), cam, bsSnap, kDt));
    CHECK(it.PolygonPoints().empty());
}

// ---------------------------------------------------------------------------
// (ac) Esc key clears all polygon points.
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: Esc key clears all polygon points", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    Arcane::InputSnapshot escSnap{};
    escSnap.AddKeycode(Sbx::Interaction::kEscKeycode);
    it.Tick(w.reg, w.Physics(), cam, escSnap, kDt);

    CHECK(it.PolygonPoints().empty());
}

// ---------------------------------------------------------------------------
// (ad) HELD key fires the shortcut ONCE (edge detection), not every frame.
//
// A key held down across many consecutive frames must only fire on the FIRST
// frame (rising edge), not on every subsequent held frame (NOT level-triggered).
// After the press fires and clears all points, the subsequent held-key frames
// must not keep firing even though the list is now empty (it simply stays empty).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: held Esc key fires once (edge detection, not level)", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    // Add 3 points via separate click events (Esc key not held yet).
    const glm::vec2 pts[3] = {{1.0f, 3.0f}, {3.0f, 3.0f}, {2.0f, 1.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    Arcane::InputSnapshot escSnap{};
    escSnap.AddKeycode(Sbx::Interaction::kEscKeycode);

    // Frame 1: Esc key DOWN (prev=false -> rising edge -> fires -> clears points).
    it.Tick(w.reg, w.Physics(), cam, escSnap, kDt);
    CHECK(it.PolygonPoints().empty());   // action fired on the rising edge

    // Frames 2+: Esc key STILL DOWN (prev=true -> NO rising edge -> must NOT fire again).
    // We cannot add new points here because LMB also belongs to the polygon branch;
    // but we can verify the empty list stays empty (not a double-clear crash, and the
    // "fired" count == 1 for the whole hold sequence).
    it.Tick(w.reg, w.Physics(), cam, escSnap, kDt);   // HELD frame 2
    it.Tick(w.reg, w.Physics(), cam, escSnap, kDt);   // HELD frame 3
    it.Tick(w.reg, w.Physics(), cam, escSnap, kDt);   // HELD frame 4

    // Still empty: only the first press triggered the clear.
    CHECK(it.PolygonPoints().empty());

    // Verify the Backspace held-key case too: add a point, hold Backspace for 3 frames,
    // only the first frame should pop the point.
    it.Tick(w.reg, w.Physics(), cam, Snap(0.0f, 0.0f, 0),    kDt);   // Esc released (baseline)
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {2.0f, 4.0f}, kLMB), kDt);  // add 1 point
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {3.0f, 4.0f}, 0),    kDt);
    it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, {3.0f, 4.0f}, kLMB), kDt);  // add 2nd point
    REQUIRE(it.PolygonPoints().size() == 2);

    Arcane::InputSnapshot bsSnap{};
    bsSnap.AddKeycode(Sbx::Interaction::kBackspaceKeycode);

    // Frame 1 of Backspace hold: fires once (pops 1 point).
    it.Tick(w.reg, w.Physics(), cam, bsSnap, kDt);
    CHECK(it.PolygonPoints().size() == 1);

    // Frames 2-3 held: must NOT pop again.
    it.Tick(w.reg, w.Physics(), cam, bsSnap, kDt);
    it.Tick(w.reg, w.Physics(), cam, bsSnap, kDt);
    CHECK(it.PolygonPoints().size() == 1);   // still 1, not 0 or below
}

// ---------------------------------------------------------------------------
// (ae) Draft markers empty when shape != Polygon (even if points are retained).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: draft markers empty when shape is not Polygon", "[sandbox]")
{
    // We simulate the gating logic: SandboxApp publishes PolygonPoints()
    // into PolygonDraftResource ONLY when shape == Polygon. Test here verifies
    // that when shape != Polygon, the Interaction still RETAINS the points
    // (they don't get wiped by switching shape) so switching back resumes them.
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    // Collect some points in Polygon mode.
    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;
    const glm::vec2 pts[3] = {{1.0f, 2.0f}, {3.0f, 2.0f}, {2.0f, 1.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    // Switch to Box: points are RETAINED in the Interaction.
    it.SpawnCfg().shape = Sbx::SpawnShape::Box;
    CHECK_FALSE(it.IsPolygonMode());
    CHECK(it.PolygonPoints().size() == 3);   // retained (not cleared by switch)

    // Switch back to Polygon: same points visible again.
    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;
    CHECK(it.IsPolygonMode());
    CHECK(it.PolygonPoints().size() == 3);   // points survived the round-trip
}

// ---------------------------------------------------------------------------
// (af) KP_Enter also triggers polygon spawn (alternate Enter key).
// ---------------------------------------------------------------------------
TEST_CASE("Interaction: KP_Enter also spawns polygon when >= 3 points", "[sandbox]")
{
    World w;
    Sbx::Camera cam;
    Sbx::Interaction it;

    it.SpawnCfg().shape = Sbx::SpawnShape::Polygon;

    const glm::vec2 pts[3] = {{2.0f, 5.0f}, {4.0f, 5.0f}, {3.0f, 3.0f}};
    for (const glm::vec2 p : pts)
    {
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, 0), kDt);
        it.Tick(w.reg, w.Physics(), cam, SnapWorld(cam, p, kLMB), kDt);
    }
    REQUIRE(it.PolygonPoints().size() == 3);

    const std::size_t bodiesBefore = w.Physics().Count();

    Arcane::InputSnapshot kpEnterSnap{};
    kpEnterSnap.AddKeycode(Sbx::Interaction::kKpEnterKeycode);
    it.Tick(w.reg, w.Physics(), cam, kpEnterSnap, kDt);

    CHECK(w.Physics().Count() == bodiesBefore + 1);
    CHECK(it.PolygonPoints().empty());
}
