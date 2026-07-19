// Grimoire play-in-editor: snapshot on Play, restore on Stop. CPU-only ([grimoire]).
//
// Drives a real Arcane::Runtime bound to the process-wide SharedTypeContext (see
// Helpers/TestTypeContext.hpp) -- the same pattern as RuntimeTest.cpp -- rather than
// a test-local Astra::TypeContext. Reason: Runtime's ctor installs the passed context
// as Arcane.dll's per-module TypeContext (Astra::SetTypeContext runs inside the DLL),
// while this test TU's calls (RegisterSceneComponents/AddComponent/GetComponent, all
// header-only Astra code compiled straight into ArcaneTests.exe) resolve TypeIDs
// against ArcaneTests.exe's own per-module context -- already pinned to
// SharedTypeContext() once, at process start, by test_main.cpp. A test-local
// Astra::TypeContext would only be installed in Arcane.dll's module, leaving the
// exe module on SharedTypeContext(): two DIFFERENT TypeContext instances, disagreeing
// on dense component IDs between the Save() (Arcane.dll) and AddComponent (exe) call
// sites. Passing SharedTypeContext() explicitly keeps both modules on the SAME
// instance, exactly like RuntimeTest.cpp's snapshot/restore cases.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include "Helpers/TestTypeContext.hpp"

// The pure Sandbox helper TUs (SandboxApp.cpp + Hud.cpp + Interaction.cpp + Scenes.cpp)
// compile straight into ArcaneTests (see SandboxHudTest.cpp); the plugin ENTRY
// (Sandbox.cpp, the extern "C" GamePlugin_* thunks) is NOT. The test below drives
// SandboxApp exactly the way Sandbox.cpp's GamePlugin_* functions do, so it can
// reproduce the Grimoire Stop crash without loading the real Sandbox.dll.
#include "../../Sandbox/src/SandboxApp.hpp"

#include <PlayMode.hpp>

TEST_CASE("Play snapshots and Stop restores the authored registry", "[grimoire]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);

    // Register Scene components on THIS Runtime's ComponentRegistry (a fresh instance
    // per Runtime) so SnapshotRegistry's Registry::Save() knows how to serialize
    // SpriteRenderer -- mirrors RenderInterpolationTest.cpp's fixture.
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity e = reg.CreateEntity();
    Arcane::SpriteRenderer sp; sp.sortingLayer = 3;
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

    Grimoire::PlaySession play;
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);

    REQUIRE(play.Play(runtime));                  // snapshot + unpause
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    // Mutate during play: bump the field on the (possibly re-fetched) entity.
    {
        Astra::Registry& live = runtime.Registry();
        for (Astra::Entity le : live.GetEntityManager())
            if (Arcane::SpriteRenderer* s = live.GetComponent<Arcane::SpriteRenderer>(le))
                s->sortingLayer = 99;
    }

    REQUIRE(play.Stop(runtime));                  // restore + pause
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // The play-time mutation is gone -- back to the authored value.
    Astra::Registry& restored = runtime.Registry();
    bool found = false;
    for (Astra::Entity le : restored.GetEntityManager())
        if (Arcane::SpriteRenderer* s = restored.GetComponent<Arcane::SpriteRenderer>(le))
        { CHECK(s->sortingLayer == 3); found = true; }
    CHECK(found);
}

TEST_CASE("PlaySession Play/Stop are idempotent across repeated calls", "[grimoire]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);
    reg.CreateEntity();

    Grimoire::PlaySession play;

    // Stop while already in Edit mode: no-op success, no restore attempted.
    CHECK(play.Stop(runtime));
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);

    REQUIRE(play.Play(runtime));
    CHECK(play.IsPlaying());

    // A second Play while already playing is a no-op success (does not re-snapshot).
    CHECK(play.Play(runtime));
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    REQUIRE(play.Stop(runtime));
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // A second Stop while already stopped is a no-op success.
    CHECK(play.Stop(runtime));
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());
}

namespace
{
    // Minimal fake plugin vtable: SaveState writes a marker, LoadState reads it back.
    // Proves PlaySession routes Play/Stop through the plugin's SaveState/LoadState when
    // a vtable is supplied -- the fix for the Stop crash, where the plugin must
    // re-establish its own native resources (physics world) that the raw registry
    // snapshot omits. Counters live in a struct so each test run resets them locally.
    int g_fakeSaveCalls = 0;
    int g_fakeLoadCalls = 0;

    void FakeSaveState(Astra::BinaryWriter& w)
    {
        ++g_fakeSaveCalls;
        w(static_cast<std::uint64_t>(0xABCD));
    }

    bool FakeLoadState(Astra::BinaryReader& r)
    {
        ++g_fakeLoadCalls;
        std::uint64_t marker = 0;
        r(marker);
        return !r.HasError() && marker == 0xABCD;
    }
}

TEST_CASE("PlaySession routes Play/Stop through the plugin vtable when present", "[grimoire]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);

    Arcane::PluginVTable vt{};
    vt.SaveState = &FakeSaveState;
    vt.LoadState = &FakeLoadState;

    g_fakeSaveCalls = 0;
    g_fakeLoadCalls = 0;

    Grimoire::PlaySession play;

    // Play routes through the plugin's SaveState (NOT Runtime::SnapshotRegistry), so the
    // plugin captures its own scene incl. native resources; then it unpauses the loop.
    REQUIRE(play.Play(runtime, &vt));
    CHECK(g_fakeSaveCalls == 1);
    CHECK(g_fakeLoadCalls == 0);
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    // Stop routes through the plugin's LoadState (which re-establishes native resources
    // after RestoreRegistry -- what Grimoire cannot do itself); then it re-pauses.
    REQUIRE(play.Stop(runtime, &vt));
    CHECK(g_fakeLoadCalls == 1);
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());
}

// ===========================================================================
// Real-Sandbox-shaped Play/Stop repro ([sandbox]).
//
// The two fixes already landed (route Play/Stop through the plugin vtable;
// RunLoop::Rebind keeps the RunLoop object stable across a restore) are proven
// by the fake-vtable test above and did NOT fix the desk-observed crash: Stop
// pauses fine, then the app dies a frame or few later. This test drives a REAL
// SandboxApp (the same TU compiled into this exe for SandboxHudTest.cpp)
// through a vtable whose SaveState/LoadState are byte-for-byte what
// Sandbox.cpp's GamePlugin_SaveState/GamePlugin_LoadState do (see Sandbox.cpp:
// they persist the root entity id explicitly, snapshot/restore the registry,
// then re-establish ONLY SceneRoot + PhysicsResource -- NOT a re-run of Init).
// It then runs real post-Stop Update + render frames (mirroring GrimoireApp's
// MainLoop, which renders every iteration regardless of play/pause) against a
// mock Batcher2D, exactly where the desk crash was observed to occur.
// ===========================================================================
namespace
{
    namespace Phys = Manifold2D::Physics;
    namespace Sbx  = Arcane::Sandbox;

    constexpr float kSandboxGravityY = 10.0f;   // matches Sandbox.cpp's kGravityY

    // Recording no-op Batcher2D (mirrors SpriteRotationTest.cpp's MockBatcher):
    // no graphics device, just counts calls so the render phase can be proven to
    // have actually run post-Stop.
    struct RecordingBatcher final : Arcane::Batcher2D
    {
        int calls = 0;

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float) override { ++calls; }
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override { ++calls; }
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override { ++calls; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override { ++calls; }
        void Circle(glm::vec2, float, glm::vec4) override { ++calls; }
        void End() override {}
        void RemoveTexture(nvrhi::ITexture*) override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };

    // ---- Sandbox.cpp's GamePlugin_Init/SaveState/LoadState, replicated exactly ----
    // File-local statics standing in for Sandbox.cpp's g_ctx/g_app/g_root/g_scheduler.
    // g_sbxRuntime is set once per TEST_CASE before Play/Stop; the vtable functions
    // below read it (mirrors g_ctx in Sandbox.cpp -- module-global, set at Init).
    Arcane::Runtime*  g_sbxRuntime = nullptr;
    Sbx::SandboxApp*  g_sbxApp     = nullptr;
    Astra::Entity     g_sbxRoot{};

    // EnsurePhysicsResource, mirrored from Sandbox.cpp's anonymous-namespace helper
    // (minus the executor wiring -- the CPU test never has an ITaskExecutor, exactly
    // like SandboxHudTest.cpp's Fixture).
    void EnsureSandboxPhysicsResource(Astra::Registry& reg)
    {
        // Astra snapshot format v2 round-trips resources -> restore reinstalls a world-less
        // PhysicsResource shell; rebuild the world whenever it is missing (mirror of Sandbox.cpp).
        if (auto* res = reg.GetResource<Arcane::PhysicsResource>(); res && res->world) return;
        Phys::WorldDef wd;
        wd.gravityY = kSandboxGravityY;
        auto world = std::make_unique<Phys::PhysicsWorld>(wd);
        reg.SetResource(Arcane::PhysicsResource{ std::move(world), {} });
    }

    void CacheSandboxRoot(Astra::Registry& reg)
    {
        g_sbxRoot = {};
        if (auto* sr = reg.GetResource<Arcane::SceneRoot>())
            g_sbxRoot = sr->entity;
    }

    // Byte-for-byte mirror of GamePlugin_SaveState (Sandbox.cpp).
    void SandboxSaveState(Astra::BinaryWriter& w)
    {
        const uint64_t rootRaw = static_cast<uint64_t>(g_sbxRoot);
        w(rootRaw);

        auto snap = g_sbxRuntime->SnapshotRegistry();
        if (snap.IsErr())
        {
            w(static_cast<uint64_t>(0));
            return;
        }
        const std::vector<std::byte>& blob = *snap.GetValue();
        w(static_cast<uint64_t>(blob.size()));
        w.WriteBytes(blob.data(), blob.size());
    }

    // Byte-for-byte mirror of GamePlugin_LoadState (Sandbox.cpp): RestoreRegistry,
    // then re-establish ONLY SceneRoot + PhysicsResource. Deliberately does NOT
    // re-run Init (no re-registered systems, no re-pointed caches) -- that gap is
    // exactly what this test is probing.
    bool SandboxLoadState(Astra::BinaryReader& r)
    {
        uint64_t rootRaw = 0; r(rootRaw);
        const Astra::Entity savedRoot(static_cast<Astra::Entity::StorageType>(rootRaw));

        uint64_t n = 0; r(n);
        std::vector<std::byte> blob(static_cast<size_t>(n));
        r.ReadBytes(blob.data(), static_cast<size_t>(n));
        if (r.HasError()) return false;
        if (!g_sbxRuntime->RestoreRegistry(blob)) return false;

        Astra::Registry& reg = g_sbxRuntime->Registry();   // re-fetched: RestoreRegistry swaps the object
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ savedRoot });
        EnsureSandboxPhysicsResource(reg);
        CacheSandboxRoot(reg);
        return true;
    }

    // Find the first live Dynamic body in the world (for a grab click).
    Phys::BodyHandle FirstDynamicBody(Phys::PhysicsWorld& world)
    {
        for (std::uint32_t i = 0; i < world.Count(); ++i)
        {
            if (!world.Alive(i)) continue;
            if (world.TypeSlot(i) != Phys::BodyType::Dynamic) continue;
            return world.HandleOf(i);
        }
        return Phys::kInvalidBody;
    }
}

TEST_CASE("Grimoire Stop: real SandboxApp runs post-Stop Update+render frames without crashing",
          "[sandbox]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    Astra::Registry& bootReg = runtime.Registry();
    Arcane::RegisterSceneComponents(bootReg);
    Arcane::RegisterPhysicsComponents(bootReg);

    // Register the SAME systems GamePlugin_Init registers, into the SAME
    // Runtime::Schedulers() a real plugin would use, so RunLoop::Advance/SubmitRender
    // drive them exactly like production (fixedUpdate stays empty -- the sandbox owns
    // its physics step via SandboxApp::FixedUpdate, called as the plugin-fixed hook).
    Arcane::SystemSchedulers& sch = runtime.Schedulers();
    sch.update.AddSystem<Arcane::TransformPropagationSystem>();
    sch.render.AddSystem<Arcane::RenderSubmissionSystem>();
    sch.render.AddSystem<Sbx::PhysicsDebugRenderSystem>();
    sch.render.AddSystem<Sbx::PolygonDraftRenderSystem>();

    Sbx::SandboxApp app;
    app.Configure(kSandboxGravityY);
    app.SetLoop(&runtime.Loop());
    app.BuildInitialScene(bootReg);
    CacheSandboxRoot(bootReg);

    g_sbxRuntime = &runtime;
    g_sbxApp     = &app;

    RecordingBatcher batcher;

    // One frame through the RunLoop, mirroring GrimoireApp::MainLoop exactly: the
    // plugin-fixed/plugin-update hooks via Advance, the camera push GamePlugin_Update
    // does right after SandboxApp::Update, then a render submission every iteration
    // (Grimoire's viewport renders regardless of play/pause).
    auto Frame = [&](const Arcane::InputSnapshot& input)
    {
        runtime.Loop().Advance(1.0 / 60.0,
            [&](double dt) { app.FixedUpdate(runtime.Registry(), dt, input); },
            [&](double dt, double a)
            {
                app.Update(runtime.Registry(), dt, a, input);
                runtime.SetCamera(app.Cam().offset, app.Cam().WorldToScreenScale());
            });
        runtime.SetRenderContext(&batcher);
        runtime.Loop().SubmitRender();
    };

    // Grimoire boots in Edit mode (paused). A couple of idle frames materialize the
    // scene-0 bodies via the paused mint-only pass (SandboxApp::Update), exactly as
    // they would sit on screen before the user presses Play.
    runtime.Loop().SetPaused(true);
    Arcane::InputSnapshot idle{};
    Frame(idle);
    Frame(idle);

    Grimoire::PlaySession play;
    Arcane::PluginVTable  vt{};
    vt.SaveState = &SandboxSaveState;
    vt.LoadState = &SandboxLoadState;

    // ---- PLAY: snapshot via the plugin vtable (fix #1), unpause -------------------
    REQUIRE(play.Play(runtime, &vt));
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    for (int i = 0; i < 15; ++i)
        Frame(idle);   // let the scene fall/settle a little

    // Grab a live dynamic body and hold it mid-drag -- the interaction layer's
    // m_grabbed + the inspector's subject fixture/body now point into the LIVE
    // (pre-Stop) PhysicsWorld. A user hitting Stop mid-drag is a realistic editor
    // interaction, not an edge case.
    {
        Astra::Registry& reg = runtime.Registry();
        auto* phys = reg.GetResource<Arcane::PhysicsResource>();
        REQUIRE(phys != nullptr);
        REQUIRE(phys->world != nullptr);
        const Phys::BodyHandle target = FirstDynamicBody(*phys->world);
        REQUIRE(target != Phys::kInvalidBody);
        const Phys::Vec2 p = phys->world->Position(target);
        const glm::vec2 screen =
            app.Cam().WorldToScreen({ static_cast<float>(p.x), static_cast<float>(p.y) });

        Arcane::InputSnapshot press{};
        press.mouseX = screen.x; press.mouseY = screen.y; press.mouseButtons = 0x1;   // LMB
        Frame(press);                 // press edge: grabs the body, sets the inspector subject
        for (int i = 0; i < 4; ++i)
            Frame(press);              // keep dragging (LMB still held) for a few frames
    }

    // ---- STOP mid-drag: LMB still logically "held" from the caller's POV ----------
    // (the next frame's input below releases it, matching a real mouse-up as focus
    // leaves the viewport -- but the grab/subject state was captured while it was
    // live). Route through the plugin vtable (fix #1); LoadState re-establishes ONLY
    // SceneRoot + PhysicsResource, mirroring the real Sandbox.cpp gap under test.
    REQUIRE(play.Stop(runtime, &vt));
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // ---- post-Stop: run several more Update+render frames -------------------------
    // This is exactly where the desk-observed crash lands ("pauses for a moment, then
    // the application closes a frame or few later"): SandboxApp::Update's SceneControl
    // pump, the interaction layer re-validating the stale grab against the NEW
    // (post-restore) PhysicsWorld, the inspector re-validating the stale subject, the
    // paused mint-only PhysicsSystem pass, and the render phase (RenderSubmissionSystem
    // + the physics-debug overlay, which reads the inspector + PhysicsInterpBuffer).
    // This is ALSO where the real root cause fires: destroying the pre-Stop registry
    // (inside RestoreRegistry) destructs every resource still resident in it, including
    // ones (SandboxInspectorResource, PolygonDraftResource, RenderContext2D, ...) whose
    // FIRST registration -- on frame 1, well AFTER PhysicsResource's own registration --
    // grew Astra::ComponentRegistry's descriptor map past its initial capacity. See
    // ThirdParty/Astra/include/Astra/Component/ComponentRegistry.hpp's ctor comment.
    Arcane::InputSnapshot release{};
    release.mouseX = release.mouseY = 0.0f;   // LMB released post-Stop
    for (int i = 0; i < 60; ++i)
        Frame(release);

    // If we get here without crashing, the scene is still coherent: a live
    // PhysicsResource with a sane body count, and the render phase actually ran
    // (the recording batcher saw calls from RenderSubmissionSystem / the debug
    // overlay every frame, paused or not -- Update/render always run).
    Astra::Registry& finalReg = runtime.Registry();
    auto* finalPhys = finalReg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(finalPhys != nullptr);
    REQUIRE(finalPhys->world != nullptr);
    CHECK(finalPhys->world->Count() > 0);
    CHECK(batcher.calls > 0);

    g_sbxRuntime = nullptr;
    g_sbxApp     = nullptr;
}
