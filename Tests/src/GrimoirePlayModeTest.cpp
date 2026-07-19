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

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include "Helpers/TestTypeContext.hpp"

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
