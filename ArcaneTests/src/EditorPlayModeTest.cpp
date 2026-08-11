// Arcane Editor play-in-editor: snapshot on Play, restore on Stop. CPU-only ([editor]).
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
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>


#include "Helpers/TestTypeContext.hpp"

#include <App/PlayMode.hpp>

TEST_CASE("Play snapshots and Stop restores the authored registry", "[editor]")
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

    Arcane::Editor::PlaySession play;
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);

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
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // The play-time mutation is gone -- back to the authored value.
    Astra::Registry& restored = runtime.Registry();
    bool found = false;
    for (Astra::Entity le : restored.GetEntityManager())
        if (Arcane::SpriteRenderer* s = restored.GetComponent<Arcane::SpriteRenderer>(le))
        { CHECK(s->sortingLayer == 3); found = true; }
    CHECK(found);
}

namespace
{
    // The component descriptor for `typeName` on `entity`, via the same
    // InspectEntity path the Inspector/CommandStackTest use. TypeMeta::typeName
    // is namespace-qualified (e.g. "Arcane::Transform").
    const Astra::ComponentDescriptor* DescriptorFor(Astra::Registry& reg,
                                                    Astra::Entity e, const char* typeName)
    {
        for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            if (ci.meta && ci.meta->typeName == typeName)
                return ci.descriptor;
        return nullptr;
    }
}

// Regression guard for dropping the undo.Clear() on Play (EditorPanels.cpp):
// an Edit-mode CommandStack entry committed BEFORE Play must still Undo/Redo
// correctly AFTER a Play->mutate->Stop round-trip, even though Stop swaps in a
// brand-new Astra::Registry object (Runtime::RestoreRegistry). This only works
// because Registry::Save()/Load() (which SnapshotRegistry/RestoreRegistry ride)
// round-trip the EntityManager and so preserve entity ids/versions -- the same
// property a game plugin's GamePlugin_LoadState relies on to re-resolve SceneRoot
// by saved id after a restore. If this test fails, that assumption is false and
// clearing the undo stack on Play was NOT safe to drop.
TEST_CASE("Edit-mode undo/redo survives a Play/Stop round-trip", "[editor]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt;
    lt.position = glm::vec2(1.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);

    const Astra::ComponentDescriptor* desc = DescriptorFor(reg, e, "Arcane::Transform");
    REQUIRE(desc != nullptr);

    // Resolver-based CommandStack (survives a registry-object swap; see
    // CommandStackTest.cpp's swap regression case), bound to THIS Runtime.
    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });

    // Edit-mode edit, committed BEFORE Play: before={1,0}, after={5,0}.
    const Arcane::TransactionId txn = stack.Begin("edit");
    stack.SnapshotComponent(e, desc);
    reg.GetComponent<Arcane::Transform>(e)->position = glm::vec2(5.0f, 0.0f);
    stack.Commit(txn);
    REQUIRE(stack.CanUndo());

    // Play: snapshot captures the authored {5,0} state.
    auto snap = runtime.SnapshotRegistry();
    REQUIRE(snap.IsOk());

    // Play-time mutation: never captured by the stack (the Inspector/gizmo
    // gate capture to Edit mode) -- Stop must discard this, not undo it.
    runtime.Registry().GetComponent<Arcane::Transform>(e)->position = glm::vec2(99.0f, 99.0f);

    // Stop: restore swaps in a NEW registry object built from the snapshot.
    REQUIRE(runtime.RestoreRegistry(*snap.GetValue()));

    // The restore preserved the entity id and its authored {5,0} value.
    Arcane::Transform* lt2 = runtime.Registry().GetComponent<Arcane::Transform>(e);
    REQUIRE(lt2 != nullptr);
    CHECK(lt2->position.x == 5.0f);
    CHECK(lt2->position.y == 0.0f);

    // The core claim: the pre-Play undo entry still resolves and reverts
    // correctly across the registry swap.
    REQUIRE(stack.CanUndo());
    stack.Undo();
    Arcane::Transform* afterUndo = runtime.Registry().GetComponent<Arcane::Transform>(e);
    REQUIRE(afterUndo != nullptr);
    CHECK(afterUndo->position.x == 1.0f);
    CHECK(afterUndo->position.y == 0.0f);

    // Redo survives too.
    REQUIRE(stack.CanRedo());
    stack.Redo();
    Arcane::Transform* afterRedo = runtime.Registry().GetComponent<Arcane::Transform>(e);
    REQUIRE(afterRedo != nullptr);
    CHECK(afterRedo->position.x == 5.0f);
    CHECK(afterRedo->position.y == 0.0f);
}

TEST_CASE("PlaySession Play/Stop are idempotent across repeated calls", "[editor]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);
    reg.CreateEntity();

    Arcane::Editor::PlaySession play;

    // Stop while already in Edit mode: no-op success, no restore attempted.
    CHECK(play.Stop(runtime));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);

    REQUIRE(play.Play(runtime));
    CHECK(play.IsPlaying());

    // A second Play while already playing is a no-op success (does not re-snapshot).
    CHECK(play.Play(runtime));
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    REQUIRE(play.Stop(runtime));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // A second Stop while already stopped is a no-op success.
    CHECK(play.Stop(runtime));
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
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

TEST_CASE("PlaySession routes Play/Stop through the plugin vtable when present", "[editor]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);

    Arcane::PluginVTable vt{};
    vt.SaveState = &FakeSaveState;
    vt.LoadState = &FakeLoadState;

    g_fakeSaveCalls = 0;
    g_fakeLoadCalls = 0;

    Arcane::Editor::PlaySession play;

    // Play routes through the plugin's SaveState (NOT Runtime::SnapshotRegistry), so the
    // plugin captures its own scene incl. native resources; then it unpauses the loop.
    REQUIRE(play.Play(runtime, &vt));
    CHECK(g_fakeSaveCalls == 1);
    CHECK(g_fakeLoadCalls == 0);
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    // Stop routes through the plugin's LoadState (which re-establishes native resources
    // after RestoreRegistry -- what Arcane Editor cannot do itself); then it re-pauses.
    REQUIRE(play.Stop(runtime, &vt));
    CHECK(g_fakeLoadCalls == 1);
    CHECK(play.Mode() == Arcane::Editor::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());
}
