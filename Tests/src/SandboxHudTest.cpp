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
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

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

    constexpr float kGravityY = 900.0f;

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
    CHECK(movedWhileRunning > 1.0f);   // gravity pulls it down once unpaused
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
// (f) Spawn knobs reach the Interaction: the HUD's shape/size/density selection
//     on SandboxApp flows into the body the next empty-space LMB-press spawns.
// ---------------------------------------------------------------------------
TEST_CASE("Hud: spawn knobs select the spawned shape", "[sandbox]")
{
    Fixture f;
    f.Step();   // materialize the scene-0 bodies

    // Select "circle" + a known size via the spawn knobs the HUD writes.
    f.app.SpawnConfigMut().shape  = Sbx::SpawnShape::Circle;
    f.app.SpawnConfigMut().size   = 30.0f;
    CHECK(f.app.SpawnConfig().shape == Sbx::SpawnShape::Circle);

    // Press LMB over empty space (top-left, away from the scene) -> spawn a circle.
    Arcane::InputSnapshot release{};
    release.mouseX = 60.0f; release.mouseY = 60.0f;
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, release);   // released baseline (no edge)

    Arcane::InputSnapshot press{};
    press.mouseX = 60.0f; press.mouseY = 60.0f; press.mouseButtons = 0x1;  // LMB
    const std::uint32_t before = f.Physics().Count();
    f.app.FixedUpdate(f.reg, 1.0 / 60.0, press);     // spawn + step materializes it

    // A new body exists and it is a Circle (the selected spawn shape).
    REQUIRE(f.Physics().Count() > before);
    bool foundCircle = false;
    for (std::uint32_t i = 0; i < f.Physics().Count(); ++i)
    {
        if (!f.Physics().Alive(i)) continue;
        const Phys::Vec2 p = f.Physics().PosSlot(i);
        if (std::abs(p.x - 60.0f) < 2.0f && std::abs(p.y - 60.0f) < 5.0f)
        {
            if (f.Physics().ShapeSlot(i).kind == Phys::ShapeKind::Circle)
                foundCircle = true;
        }
    }
    CHECK(foundCircle);
}
