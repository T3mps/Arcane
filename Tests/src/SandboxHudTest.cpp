// [sandbox] CPU-only: the Sandbox ImGui control-panel HUD (Task 8).
//
// Drives Arcane::Sandbox::Hud::Draw(SandboxApp&) headless: a throwaway ImGui
// context is created (with a built font atlas) so the HUD's ImGui:: calls have a
// valid context and NewFrame does not assert. The gate is two-fold:
//
//   1. DrawUI runs headless without tripping an ImGui assert / crashing.
//   2. The HUD-bound state on SandboxApp actually DRIVES SandboxApp behavior --
//      a paused step is skipped, a single-step runs exactly one tick then
//      re-pauses, the time-scale scales the dt, the debug-draw flags reach the
//      render options, and the spawn knobs reach the Interaction. These are
//      asserted by driving the SandboxApp state directly (the same fields the
//      HUD's widgets bind to) and observing the simulation outcome, NOT by
//      faking ImGui input (headless ImGui cannot synthesize clicks).
//
// Like SandboxInteractionTest, the pure Sandbox helper TUs (SandboxApp.cpp +
// Hud.cpp + Interaction.cpp + Scenes.cpp) compile straight into ArcaneTests
// (premake5.lua file list); the plugin ENTRY (Sandbox.cpp) is NOT compiled in.
// ImGui is available via the dllimport-from-Arcane.dll path (the same single
// GImGui the rest of the workspace shares).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../Sandbox/src/Hud.hpp"
#include "../../Sandbox/src/SandboxApp.hpp"
#include "../../Sandbox/src/Scenes.hpp"

#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp>  // NarrowphaseTrace (subject contacts)
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>   // ContactConstraint (subject enumeration)

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <imgui.h>

#include <cmath>
#include <memory>

using Catch::Approx;

namespace
{
    namespace Phys = Arcane::Physics;
    namespace Sbx  = Arcane::Sandbox;

    constexpr float kGravityY = 10.0f;   // MKS: +10 m/s^2 (matches the WorldDef default)

    // ---------------------------------------------------------------------------
    // A throwaway headless ImGui frame guard: create a context, build a minimal
    // font atlas so NewFrame does not assert, and BeginFrame. RAII tears it down.
    // ---------------------------------------------------------------------------
    struct ImGuiHeadlessFrame
    {
        ImGuiContext* ctx = nullptr;

        ImGuiHeadlessFrame()
        {
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
            io.DeltaTime   = 1.0f / 60.0f;
            // Build the font atlas (NewFrame asserts FontAtlas->IsBuilt()).
            unsigned char* px = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
            io.Fonts->TexID = (ImTextureID)(intptr_t)1;   // pretend a texture is bound
            ImGui::NewFrame();
        }

        ~ImGuiHeadlessFrame()
        {
            ImGui::Render();          // EndFrame + build draw data (no GPU touch)
            ImGui::DestroyContext(ctx);
        }
    };

    // ---------------------------------------------------------------------------
    // A minimal sandbox-like world: a real SandboxApp driving a real Registry with
    // the scene components + a fresh PhysicsResource. SandboxApp owns the physics
    // step (Task 8 sim-control path), so the test steps through SandboxApp.
    // ---------------------------------------------------------------------------
    struct Fixture
    {
        std::shared_ptr<Astra::ComponentRegistry> components =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Sbx::SandboxApp app;

        Fixture()
        {
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents(reg);
            app.Configure(kGravityY);
            app.BuildInitialScene(reg);   // scene 0 (Playground): floor + walls + dynamics
        }

        Phys::PhysicsWorld& Physics()
        {
            return *reg.GetResource<Arcane::PhysicsResource>()->world;
        }

        // One frame of the plugin-owned fixed step (no interaction input).
        void Step()
        {
            Arcane::InputSnapshot input{};
            app.FixedUpdate(reg, 1.0 / 60.0, input);
        }

        // Total Y displacement of the FIRST dynamic body across `n` steps -- a proxy
        // for "did the sim advance" (gravity pulls dynamics down each real step).
        float FirstDynamicYAfter(int n)
        {
            const float y0 = FirstDynamicY();
            for (int i = 0; i < n; ++i) Step();
            return FirstDynamicY() - y0;
        }

        float FirstDynamicY()
        {
            const auto* res = reg.GetResource<Arcane::PhysicsResource>();
            for (std::uint32_t i = 0; i < res->world->Count(); ++i)
            {
                if (!res->world->Alive(i)) continue;
                if (res->world->TypeSlot(i) != Phys::BodyType::Dynamic) continue;
                return res->world->PosSlot(i).y;
            }
            return 0.0f;
        }
    };
}

// ---------------------------------------------------------------------------
// (a) DrawUI runs headless: the HUD issues its ImGui calls under a valid context
//     without tripping an assert / crash. This is the smoke gate.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: Draw runs headless without crashing", "[sandbox]")
{
    Fixture f;
    ImGuiHeadlessFrame frame;
    // Should issue Begin("Sandbox")...End() + all section widgets with no assert.
    REQUIRE_NOTHROW(Sbx::Hud::Draw(f.app, f.reg));
    SUCCEED("DrawUI completed under a headless ImGui context");
}

// ---------------------------------------------------------------------------
// (b) Pause gates the step: a paused SandboxApp does NOT advance the physics.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: paused state gates the physics step", "[sandbox]")
{
    Fixture f;
    f.Step();   // materialize bodies (CREATE pass) + one settle step

    f.app.SetPaused(true);
    CHECK(f.app.IsPaused());

    const float movedWhilePaused = std::abs(f.FirstDynamicYAfter(20));
    CHECK(movedWhilePaused < 0.01f);   // frozen: no motion while paused

    f.app.SetPaused(false);
    const float movedWhileRunning = std::abs(f.FirstDynamicYAfter(20));
    // MKS re-baseline (was > 1.0f, an implicit px threshold): 20 free-fall steps at
    // g = 10 drop the first dynamic ~0.6 m (v0*T + 0.5 g T^2, T = 20/60 s); 0.1 m
    // cleanly separates "moved" from the frozen 0 m (< 0.01 m above) with wide margin.
    CHECK(movedWhileRunning > 0.1f);   // gravity pulls it down once unpaused
}

// ---------------------------------------------------------------------------
// (c) Single-step: while paused, RequestSingleStep advances EXACTLY one tick then
//     the app re-pauses (no free-running after the single step).
// ---------------------------------------------------------------------------
TEST_CASE("Hud: single-step advances exactly one tick while paused", "[sandbox]")
{
    Fixture f;
    f.Step();              // materialize + settle
    f.app.SetPaused(true);

    const float yBefore = f.FirstDynamicY();

    f.app.RequestSingleStep();
    f.Step();              // consumes the single-step request -> one physics tick
    const float yAfterOne = f.FirstDynamicY();
    CHECK(std::abs(yAfterOne - yBefore) > 0.0f);   // advanced one tick

    // No further single-step requested: subsequent steps are frozen again.
    const float yAfterOneSnapshot = yAfterOne;
    for (int i = 0; i < 10; ++i) f.Step();
    CHECK(f.FirstDynamicY() == Approx(yAfterOneSnapshot));
    CHECK(f.app.IsPaused());   // re-paused after the single step
}

// ---------------------------------------------------------------------------
// (d) Time-scale: a >1 time-scale advances the sim FARTHER per real step than 1x
//     (the dt fed to the physics step is scaled).
// ---------------------------------------------------------------------------
TEST_CASE("Hud: time-scale scales the simulated dt", "[sandbox]")
{
    // Two identical worlds; one at 1x, one at 3x. After the same number of real
    // steps the 3x world has fallen farther (a larger effective dt per step).
    Fixture a;
    Fixture b;
    a.Step();  b.Step();   // both materialize bodies first

    a.app.SetTimeScale(1.0f);
    b.app.SetTimeScale(3.0f);

    const float dropA = std::abs(a.FirstDynamicYAfter(20));
    const float dropB = std::abs(b.FirstDynamicYAfter(20));
    CHECK(dropB > dropA * 1.5f);   // 3x time-scale clearly outpaces 1x
}

// ---------------------------------------------------------------------------
// (e) Debug-draw flags reach the render options: the flags the HUD toggles on
//     SandboxApp are what PhysicsDebugRenderSystem hands to DrawPhysicsDebug.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: debug-draw flags drive the render options", "[sandbox]")
{
    Fixture f;

    // Defaults: contacts on (the Task-7 default), AABBs off.
    CHECK(f.app.DebugOptions().drawContacts);
    CHECK_FALSE(f.app.DebugOptions().drawAabbs);

    // Toggle them as the HUD checkbox would.
    f.app.DebugOptionsMut().drawContacts = false;
    f.app.DebugOptionsMut().drawAabbs    = true;
    CHECK_FALSE(f.app.DebugOptions().drawContacts);
    CHECK(f.app.DebugOptions().drawAabbs);
}

// ---------------------------------------------------------------------------
// (e2) Rich-overlay debug flags (nice-to-have): the velocity / COM / orientation
//      overlays the HUD exposes round-trip through SandboxApp's debug options
//      (defaults ON, matching PhysicsDebugDrawOptions) and the HUD can toggle them.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: rich-overlay debug flags round-trip through the options", "[sandbox]")
{
    Fixture f;

    // Defaults: the rich overlays are ON out of the box (informative showcase).
    CHECK(f.app.DebugOptions().drawVelocities);
    CHECK(f.app.DebugOptions().drawComMarkers);
    CHECK(f.app.DebugOptions().drawOrientations);

    // The HUD toggles + scalars write through DebugOptionsMut().
    f.app.DebugOptionsMut().drawVelocities    = false;
    f.app.DebugOptionsMut().drawComMarkers    = false;
    f.app.DebugOptionsMut().drawOrientations  = false;
    // Arbitrary write-through/read-back probe values (does this field round-trip?),
    // NOT slider-range-legal magnitudes -- the MKS COM/tick ranges are ~0.02-0.48 m.
    f.app.DebugOptionsMut().velocityScale     = 0.30f;
    f.app.DebugOptionsMut().comMarkerSize     = 9.0f;
    f.app.DebugOptionsMut().orientationTickLen = 24.0f;

    CHECK_FALSE(f.app.DebugOptions().drawVelocities);
    CHECK_FALSE(f.app.DebugOptions().drawComMarkers);
    CHECK_FALSE(f.app.DebugOptions().drawOrientations);
    CHECK(f.app.DebugOptions().velocityScale == Approx(0.30f));
    CHECK(f.app.DebugOptions().comMarkerSize == Approx(9.0f));
    CHECK(f.app.DebugOptions().orientationTickLen == Approx(24.0f));

    // Run a render-driven publish (FixedUpdate mirrors the flags into the resource);
    // the gate is "no crash + the resource carries the HUD's choices".
    Arcane::InputSnapshot idle{};
    REQUIRE_NOTHROW(f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle));
    const auto* res = f.reg.GetResource<Sbx::SandboxDebugDraw>();
    REQUIRE(res != nullptr);
    CHECK_FALSE(res->drawVelocities);
    CHECK(res->velocityScale == Approx(0.30f));
}

// ---------------------------------------------------------------------------
// (f) Spawn knobs reach the Interaction: the HUD's shape/size/density selection
//     on SandboxApp flows into the body the next empty-space LMB-press spawns.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: spawn knobs select the spawned shape", "[sandbox]")
{
    Fixture f;
    f.Step();   // materialize the scene-0 bodies

    // Select "circle" + a known size (meters) via the spawn knobs the HUD writes.
    f.app.SpawnConfigMut().shape  = Sbx::SpawnShape::Circle;
    f.app.SpawnConfigMut().size   = 0.3f;   // MKS: 0.3 m radius (was 30 px)
    CHECK(f.app.SpawnConfig().shape == Sbx::SpawnShape::Circle);

    // Press LMB over empty world space far from the ~13 x 8 m scene -> spawn a circle.
    // The cursor is SCREEN px, so project the world target through the app camera
    // (ppm=100): world (30,30) m -> screen (3000,3000) px at the default identity cam.
    const glm::vec2 wt{30.0f, 30.0f};
    const glm::vec2 sc = f.app.Cam().WorldToScreen(wt);
    Arcane::InputSnapshot release{};
    release.mouseX = sc.x; release.mouseY = sc.y;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, release);   // released baseline (no edge)

    Arcane::InputSnapshot press{};
    press.mouseX = sc.x; press.mouseY = sc.y; press.mouseButtons = 0x1;  // LMB
    const std::uint32_t before = f.Physics().Count();
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, press);     // spawn + step materializes it

    // A new body exists and it is a Circle (the selected spawn shape), near the world
    // target (one gravity step at g=10 nudges it ~1.4 mm). Tolerances are meters.
    REQUIRE(f.Physics().Count() > before);
    bool foundCircle = false;
    for (std::uint32_t i = 0; i < f.Physics().Count(); ++i)
    {
        if (!f.Physics().Alive(i)) continue;
        const Phys::Vec2 p = f.Physics().PosSlot(i);
        if (std::abs(p.x - wt.x) < 0.02f && std::abs(p.y - wt.y) < 0.05f)
        {
            if (f.Physics().ShapeSlot(i).kind == Phys::ShapeKind::Circle)
                foundCircle = true;
        }
    }
    CHECK(foundCircle);
}

// ---------------------------------------------------------------------------
// (g) Polygon mode end-to-end: now driven via shape == Polygon (unified selector).
//     SetPolygonMode(true) is a shim that sets shape = Polygon.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: polygon mode collects clicks and spawns a polygon body", "[sandbox]")
{
    Fixture f;
    f.Step();   // materialize the scene-0 bodies

    // Drive via shape directly (canonical path).
    f.app.SpawnConfigMut().shape = Sbx::SpawnShape::Polygon;
    CHECK(f.app.IsPolygonMode());

    // Click three world points (released baseline + press each, away from the scene).
    const float pts[3][2] = {{120.0f, 200.0f}, {220.0f, 200.0f}, {170.0f, 120.0f}};
    for (const auto& p : pts)
    {
        Arcane::InputSnapshot rel{}; rel.mouseX = p[0]; rel.mouseY = p[1];
        f.app.FixedUpdate(f.reg, 1.0 / 60.0, rel);
        Arcane::InputSnapshot press{}; press.mouseX = p[0]; press.mouseY = p[1];
        press.mouseButtons = 0x1;   // LMB
        f.app.FixedUpdate(f.reg, 1.0 / 60.0, press);
    }
    CHECK(f.app.PolygonPointCount() == 3);

    // Request the commit (the HUD "Spawn polygon" button) -> next step adds the body.
    const std::uint32_t before = f.Physics().Count();
    f.app.RequestPolygonSpawn();
    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);

    CHECK(f.Physics().Count() == before + 1);   // one world-direct polygon body
    CHECK(f.app.PolygonPointCount() == 0);       // points cleared after the commit

    // The new body is a polygon.
    bool foundPolygon = false;
    for (std::uint32_t i = 0; i < f.Physics().Count(); ++i)
    {
        if (!f.Physics().Alive(i)) continue;
        if (f.Physics().ShapeSlot(i).kind == Phys::ShapeKind::Polygon)
            foundPolygon = true;
    }
    CHECK(foundPolygon);
}

// ---------------------------------------------------------------------------
// (h) Unified spawn: selecting Capsule shape via SpawnConfigMut sets shape,
//     and the capsule LMB-spawn works end-to-end through SandboxApp.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: selecting Capsule shape sets cfg.shape to Capsule", "[sandbox]")
{
    Fixture f;

    f.app.SpawnConfigMut().shape = Sbx::SpawnShape::Capsule;
    CHECK(f.app.SpawnConfig().shape == Sbx::SpawnShape::Capsule);
    // Polygon mode is NOT on when Capsule is selected.
    CHECK_FALSE(f.app.IsPolygonMode());
}

// ---------------------------------------------------------------------------
// (i) SetPolygonMode(true) shim sets shape to Polygon; false sets shape to Box.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: SetPolygonMode shim sets shape to Polygon / Box", "[sandbox]")
{
    Fixture f;

    f.app.SetPolygonMode(true);
    CHECK(f.app.SpawnConfig().shape == Sbx::SpawnShape::Polygon);
    CHECK(f.app.IsPolygonMode());

    f.app.SetPolygonMode(false);
    CHECK(f.app.SpawnConfig().shape == Sbx::SpawnShape::Box);
    CHECK_FALSE(f.app.IsPolygonMode());
}

// ---------------------------------------------------------------------------
// (j) Draft markers are empty when shape != Polygon (gating in FixedUpdate).
// ---------------------------------------------------------------------------
TEST_CASE("Hud: draft markers suppressed when shape is not Polygon", "[sandbox]")
{
    Fixture f;
    f.Step();

    // Collect some polygon points while in Polygon mode.
    f.app.SpawnConfigMut().shape = Sbx::SpawnShape::Polygon;
    const float pts[3][2] = {{120.0f, 200.0f}, {220.0f, 200.0f}, {170.0f, 120.0f}};
    for (const auto& p : pts)
    {
        Arcane::InputSnapshot rel{}; rel.mouseX = p[0]; rel.mouseY = p[1];
        f.app.FixedUpdate(f.reg, 1.0 / 60.0, rel);
        Arcane::InputSnapshot press{}; press.mouseX = p[0]; press.mouseY = p[1];
        press.mouseButtons = 0x1;
        f.app.FixedUpdate(f.reg, 1.0 / 60.0, press);
    }
    CHECK(f.app.PolygonPointCount() == 3);

    // Switch to Box: draft resource should publish empty list.
    f.app.SpawnConfigMut().shape = Sbx::SpawnShape::Box;
    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);

    const auto* draft = f.reg.GetResource<Arcane::Sandbox::PolygonDraftResource>();
    REQUIRE(draft != nullptr);
    CHECK(draft->worldPoints.empty());   // suppressed: shape != Polygon

    // Points are RETAINED internally; switching back to Polygon re-publishes them.
    f.app.SpawnConfigMut().shape = Sbx::SpawnShape::Polygon;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);
    draft = f.reg.GetResource<Arcane::Sandbox::PolygonDraftResource>();
    REQUIRE(draft != nullptr);
    CHECK(draft->worldPoints.size() == 3);   // retained + re-published
}

// ===========================================================================
// Narrowphase inspector (subject model, always on). CPU-only -- no GPU device,
// so the Minkowski-inset OffscreenCanvas path returns null gracefully; the
// grab-to-select subject + all-contacts enumeration + deepest-default partner
// selection + step-control + resource-publication wiring is what we gate here.
// ===========================================================================

namespace
{
    // The handle of the FIRST dynamic body that participates in any active contact
    // (so SetSubjectBody has a body whose contacts we can enumerate). Returns
    // kInvalidBody if no dynamic body is currently in contact.
    Phys::BodyHandle FirstDynamicContactBody(Phys::PhysicsWorld& world)
    {
        Phys::BodyHandle out = Phys::kInvalidBody;
        world.ForEachContactConstraint(
            [&](const Phys::ContactConstraint& cc)
            {
                if (out != Phys::kInvalidBody) return;
                if (cc.bodyA == Phys::kInvalidSlot) return;
                if (cc.pointCount <= 0) return;
                // bodyA is always the dynamic slot.
                out = world.HandleOf(cc.bodyA);
            });
        return out;
    }

    // Wake every dynamic body, then let the caller step once more so
    // EmitContactConstraints (PhysicsWorld.cpp) re-emits their contacts.
    //
    // MKS re-derive (MKS P6): scene 0 (BuildPlayground) is now authored in meters
    // (drops ~4.7-7.0 m under g = 10 m/s^2). Two facts set the settle count:
    //
    //  1. FALL TIME. t = sqrt(2h/g): the deepest drop (~7.0 m) lands at
    //     ~1.18 s ~ 71 steps; the shallowest (~4.7 m) at ~0.97 s ~ 58 steps.
    //     At MKS the WorldDef::maxLinearVelocity = 400 (now honest m/s) clamp is
    //     NEVER engaged -- peak fall speed is ~12 m/s, ~33x under the cap -- so,
    //     unlike the px era, nothing is throttled; first contact is ~58-71 steps.
    //     The 200-step settle below therefore lands + rests every dynamic with
    //     ~2.8x headroom over first contact (probe precedent: a comparable
    //     5-box/3-circle pile fully sleeps by ~338 steps, so 200 sits safely in
    //     the landed-and-resting window). Empirically re-verified: after 200 steps
    //     + wake + 1 step, FirstDynamicContactBody returns a valid body.
    //
    //  2. SLEEP AWAKE-GATE (scale-independent, KEEP the wake). EmitContactConstraints
    //     has an awake-gate (PhysicsWorld.cpp) that excludes a sleeping dynamic's
    //     contacts from ForEachContactConstraint. Once the scene lands it genuinely
    //     sleeps, so a settled scene publishes zero live contact constraints --
    //     FirstDynamicContactBody would return kInvalidBody. The explicit
    //     WakeAllDynamics + one extra Step() re-emits the resting contacts. This is
    //     unchanged by the unit switch (the gate is on sleep state, not scale).
    //
    // Production never hits the gap: the drag path calls world.Wake(m_grabbed)
    // every tick (Interaction.cpp:277). These tests bypass the grab and set the
    // subject directly on a settled body, so each test (a) settles for long enough
    // (200 steps) that the scene has actually landed, then (b) wakes it
    // explicitly -- mirroring the production wake-on-grab, just called by hand.
    void WakeAllDynamics(Phys::PhysicsWorld& world)
    {
        for (std::uint32_t i = 0; i < world.Count(); ++i)
        {
            if (!world.Alive(i)) continue;
            if (world.TypeSlot(i) != Phys::BodyType::Dynamic) continue;
            world.Wake(world.HandleOf(i));
        }
    }
}

// ---------------------------------------------------------------------------
// (k) Grab-to-select: grabbing a body sets it as the subject; the subject
//     persists; grabbing empty space does not change it; Clear drops it.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: grabbing a body sets/persists/clears the subject", "[sandbox]")
{
    Fixture f;
    // 200 steps (~3.3 s): scene 0's 5 dynamics land by ~58-71 steps at MKS (drops
    // ~4.7-7.0 m under g = 10, no velocity clamp) and then rest; 200 gives ~2.8x
    // headroom over first contact -- see WakeAllDynamics for the full derivation.
    // By 200 steps every dynamic has landed and may have gone to sleep; wake them
    // back up below so the settled contacts are live again.
    for (int i = 0; i < 200; ++i) f.Step();

    WakeAllDynamics(f.Physics());
    f.Step();   // one more tick with the island awake: EmitContactConstraints runs

    CHECK_FALSE(f.app.HasSubject());

    // Pick a live dynamic body in contact and set it as the subject directly (the
    // same path the grab side effect drives through SetSubjectBody).
    const Phys::BodyHandle body = FirstDynamicContactBody(f.Physics());
    REQUIRE(body != Phys::kInvalidBody);
    f.app.SetSubjectBody(f.reg, body);
    CHECK(f.app.HasSubject());
    CHECK(f.Physics().IsValid(f.app.Subject()));
    CHECK(f.app.SubjectBody().index == body.index);

    // It persists across an idle FixedUpdate (no new grab).
    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);
    CHECK(f.app.HasSubject());

    // Clear subject drops it.
    f.app.ClearSubject();
    CHECK_FALSE(f.app.HasSubject());
}

// ---------------------------------------------------------------------------
// (l) Subject enumerates ALL its contacts; resource is published with subject.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: subject enumerates its contacts and publishes the resource", "[sandbox]")
{
    Fixture f;
    for (int i = 0; i < 200; ++i) f.Step();   // settle: dynamics land + rest on the floor

    // Wake the now-sleeping scene (see WakeAllDynamics) + step once so contacts
    // are re-emitted for the pick below.
    WakeAllDynamics(f.Physics());
    f.Step();

    const Phys::BodyHandle body = FirstDynamicContactBody(f.Physics());
    REQUIRE(body != Phys::kInvalidBody);
    f.app.SetSubjectBody(f.reg, body);

    // Publish the resource (FixedUpdate enumerates + mirrors the inspector state).
    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);

    // The subject is touching at least one partner (a settled body on the floor).
    REQUIRE_FALSE(f.app.Contacts().empty());

    const auto* insp = f.reg.GetResource<Sbx::SandboxInspectorResource>();
    REQUIRE(insp != nullptr);
    CHECK(insp->hasSubject);
    CHECK(f.Physics().IsValid(insp->subject));
    CHECK(insp->subjectBody == body.index);
    CHECK(insp->contacts.size() == f.app.Contacts().size());
    // Every published contact has a recorded (non-empty) manifold (0-point dropped).
    for (const auto& cv : insp->contacts)
        CHECK(cv.trace.manifold.pointCount > 0);
}

// ---------------------------------------------------------------------------
// (m) Deepest contact is the default focus; selection re-points the inset.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: selection defaults to the deepest contact", "[sandbox]")
{
    Fixture f;
    for (int i = 0; i < 200; ++i) f.Step();   // settle: dynamics land + rest (see WakeAllDynamics)

    // Wake the now-sleeping scene (see WakeAllDynamics) + step once so contacts
    // are re-emitted for the pick below.
    WakeAllDynamics(f.Physics());
    f.Step();

    const Phys::BodyHandle body = FirstDynamicContactBody(f.Physics());
    REQUIRE(body != Phys::kInvalidBody);
    f.app.SetSubjectBody(f.reg, body);

    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);

    REQUIRE_FALSE(f.app.Contacts().empty());
    const int sel = f.app.SelectedIndex();
    REQUIRE(sel >= 0);
    REQUIRE(sel < static_cast<int>(f.app.Contacts().size()));

    // The selected contact is the DEEPEST (max manifold separation over all contacts).
    auto MaxDepth = [](const Phys::NarrowphaseTrace& t)
    {
        float d = -1e30f;
        for (int pi = 0; pi < t.manifold.pointCount; ++pi)
            d = std::max(d, static_cast<float>(t.manifold.points[pi].separation));
        return d;
    };
    const float selDepth = MaxDepth(f.app.Contacts()[static_cast<std::size_t>(sel)].trace);
    for (const auto& cv : f.app.Contacts())
        CHECK(selDepth >= MaxDepth(cv.trace) - 1e-4f);

    // Selecting a different partner (when present) re-points the selection by id, and
    // the selection is STABLE by id across a re-enumerate. Pause the sim first so the
    // contact set is identical on the next enumerate (a running step could legitimately
    // make the chosen partner lose contact -- that is fallback-to-deepest, not the
    // stability property we are asserting here).
    if (f.app.Contacts().size() >= 2)
    {
        f.app.SetPaused(true);
        const int other = (sel == 0) ? 1 : 0;
        const std::uint32_t otherId = f.app.Contacts()[static_cast<std::size_t>(other)].partnerBodyId;
        f.app.SetSelectedIndex(other);
        CHECK(f.app.SelectedIndex() == other);
        // Stable by id: another enumerate (paused -> same contacts) keeps the partner.
        f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);
        REQUIRE(f.app.SelectedIndex() >= 0);
        CHECK(f.app.Contacts()[static_cast<std::size_t>(f.app.SelectedIndex())].partnerBodyId == otherId);
    }
}

// ---------------------------------------------------------------------------
// (n) Step control clamps to the recorded iteration count of the SELECTED trace.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: step index clamps to the selected contact's count", "[sandbox]")
{
    Fixture f;
    for (int i = 0; i < 200; ++i) f.Step();   // settle: dynamics land + rest (see WakeAllDynamics)

    // Wake the now-sleeping scene (see WakeAllDynamics) + step once so contacts
    // are re-emitted for the pick below.
    WakeAllDynamics(f.Physics());
    f.Step();

    const Phys::BodyHandle body = FirstDynamicContactBody(f.Physics());
    REQUIRE(body != Phys::kInvalidBody);
    f.app.SetSubjectBody(f.reg, body);

    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);
    REQUIRE_FALSE(f.app.Contacts().empty());

    const int n = f.app.RecordedStepCount();
    CHECK(n >= 0);   // analytic kinds report 0; stepped kinds report > 0

    // An out-of-range index is clamped (never out of [0, n-1], or 0 when n == 0).
    f.app.SetStepIndex(99999);
    if (n > 0) CHECK(f.app.StepIndex() == n - 1);
    else       CHECK(f.app.StepIndex() == 0);

    f.app.SetStepIndex(-5);
    CHECK(f.app.StepIndex() == 0);
}

// ---------------------------------------------------------------------------
// (o) Grab through FixedUpdate sets the subject; empty grab does not change it.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: a grab through FixedUpdate sets the subject; empty grab keeps it", "[sandbox]")
{
    Fixture f;
    for (int i = 0; i < 200; ++i) f.Step();   // settle: dynamics land + rest (see WakeAllDynamics)

    // Wake the now-sleeping scene (see WakeAllDynamics) + step once so contacts
    // are re-emitted for the pick below.
    WakeAllDynamics(f.Physics());
    f.Step();

    // Find a live dynamic body and its world position so we can click ON it.
    const Phys::BodyHandle body = FirstDynamicContactBody(f.Physics());
    REQUIRE(body != Phys::kInvalidBody);
    const Phys::Vec2 bp = f.Physics().Position(body);
    // The cursor is SCREEN px; project the body's WORLD position through the app
    // camera (ppm=100) so the click lands ON the body.
    const glm::vec2 bpScreen =
        f.app.Cam().WorldToScreen({static_cast<float>(bp.x), static_cast<float>(bp.y)});

    // Released baseline, then an LMB press ON the body.
    Arcane::InputSnapshot rel{}; rel.mouseX = bpScreen.x; rel.mouseY = bpScreen.y;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, rel);

    Arcane::InputSnapshot press{}; press.mouseX = bpScreen.x;
    press.mouseY = bpScreen.y; press.mouseButtons = 0x1;   // LMB
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, press);

    REQUIRE(f.app.HasSubject());                 // the grab set the subject
    const std::uint32_t subjBody = f.app.SubjectBody().index;

    // Release, then an LMB press on FAR EMPTY space: no body grabbed -> subject unchanged.
    Arcane::InputSnapshot rel2{}; rel2.mouseX = bpScreen.x; rel2.mouseY = bpScreen.y;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, rel2);
    Arcane::InputSnapshot emptyPress{}; emptyPress.mouseX = -9000.0f; emptyPress.mouseY = -9000.0f;
    emptyPress.mouseButtons = 0x1;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, emptyPress);

    CHECK(f.app.HasSubject());                   // subject persisted across the empty grab
    CHECK(f.app.SubjectBody().index == subjBody);
}

// ---------------------------------------------------------------------------
// (p) Headless null-device: EnsureInspectorCanvas returns null and the HUD
//     (incl. the subject inspector window) draws safely; 0-contact path is safe.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: headless inset is null and the HUD draws safely", "[sandbox]")
{
    Fixture f;
    for (int i = 0; i < 200; ++i) f.Step();   // settle: dynamics land + rest (see WakeAllDynamics)

    // Wake the now-sleeping scene (see WakeAllDynamics) + step once so contacts
    // are re-emitted for the pick below.
    WakeAllDynamics(f.Physics());
    f.Step();

    const Phys::BodyHandle body = FirstDynamicContactBody(f.Physics());
    REQUIRE(body != Phys::kInvalidBody);
    f.app.SetSubjectBody(f.reg, body);
    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);

    // No Runtime injected (the CPU harness never calls SetRuntime) -> no device ->
    // no OffscreenCanvas is created (the inset is a render-only nicety).
    CHECK(f.app.EnsureInspectorCanvas(256, 256) == nullptr);
    CHECK(f.app.Inspector() == nullptr);

    // The HUD (main panel + the subject inspector window with the partner selector)
    // issues all its ImGui calls under a headless context without asserting.
    {
        ImGuiHeadlessFrame frame;
        REQUIRE_NOTHROW(Sbx::Hud::Draw(f.app, f.reg));
    }

    // A subject with ZERO contacts is crash-safe (the "not touching anything" + empty
    // idle inset path). Spawn an isolated body far from everything, make it the subject.
    f.app.ClearSubject();
    {
        ImGuiHeadlessFrame frame;
        REQUIRE_NOTHROW(Sbx::Hud::Draw(f.app, f.reg));   // no subject -> no inspector window
    }
}

// ---------------------------------------------------------------------------
// (q) Zero-contact subject: an isolated body (touching nothing) is a valid
//     subject with an empty contacts list; the HUD draws the "not touching"
//     path safely and SelectedTrace() returns a safe idle trace.
// ---------------------------------------------------------------------------
TEST_CASE("Inspector: a subject touching nothing is crash-safe", "[sandbox]")
{
    Fixture f;
    f.Step();   // materialize the scene-0 bodies

    // Spawn an isolated dynamic box far from everything via the spawn path, pause so it
    // does not fall into anything, and make it the subject.
    f.app.SetPaused(true);
    f.app.SpawnConfigMut().shape = Sbx::SpawnShape::Box;
    f.app.SpawnConfigMut().size  = 0.5f;   // MKS: 0.5 m half-extent (was 10 px)

    // MKS: (50, -50) m -- far from the ~13 x 8 m scene 0 layout ("far away" is the
    // only requirement). The cursor is SCREEN px, so project the world target through
    // the app camera (ppm=100): world (50,-50) m -> screen (5000,-5000) px.
    const glm::vec2 spawnWorld{50.0f, -50.0f};
    const glm::vec2 spawnScreen = f.app.Cam().WorldToScreen(spawnWorld);
    Arcane::InputSnapshot rel{};  rel.mouseX = spawnScreen.x; rel.mouseY = spawnScreen.y;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, rel);
    Arcane::InputSnapshot press{}; press.mouseX = spawnScreen.x; press.mouseY = spawnScreen.y; press.mouseButtons = 0x1;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, press);   // spawn + one mint step (paused: no fall)

    // Find the isolated body near the spawn point and make it the subject.
    Phys::BodyHandle isolated = Phys::kInvalidBody;
    for (std::uint32_t i = 0; i < f.Physics().Count(); ++i)
    {
        if (!f.Physics().Alive(i)) continue;
        const Phys::Vec2 p = f.Physics().PosSlot(i);
        if (std::abs(static_cast<float>(p.x) - 50.0f) < 0.5f &&
            std::abs(static_cast<float>(p.y) + 50.0f) < 0.5f)
        { isolated = f.Physics().HandleOf(i); break; }
    }
    REQUIRE(isolated != Phys::kInvalidBody);

    f.app.SetSubjectBody(f.reg, isolated);
    Arcane::InputSnapshot idle{};
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, idle);

    CHECK(f.app.HasSubject());            // valid subject...
    CHECK(f.app.Contacts().empty());      // ...touching nothing
    CHECK(f.app.SelectedIndex() == -1);   // no focused contact
    CHECK(f.app.RecordedStepCount() == 0);// idle trace -> analytic/no iterations

    // The HUD draws the "subject not touching anything" + empty idle inset path safely.
    ImGuiHeadlessFrame frame;
    REQUIRE_NOTHROW(Sbx::Hud::Draw(f.app, f.reg));
}
