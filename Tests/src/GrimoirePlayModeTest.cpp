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

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
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
