// SceneSession: the editor's scene identity + dirty state + the unsaved-changes
// confirm machine. Entirely pure -- no ImGui, no filesystem -- so the whole
// state machine is driven headlessly here.

#include <catch2/catch_test_macros.hpp>

#include "SceneSession.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

using namespace Arcane::Editor;

namespace
{
    // The component descriptor for `typeName` on `entity`, via the same
    // InspectEntity path the Inspector uses. Astra::TypeMeta::typeName is
    // namespace-qualified (e.g. "Arcane::Transform"). Mirrors the verified
    // DescriptorFor helper in CommandStackTest.cpp -- the brief's literal
    // `GetComponentRegistry()->GetDescriptor(Astra::TypeId<T>::Hash())` does
    // not compile (no such member; the real one is GetComponentDescriptorByHash,
    // and the type is Astra::TypeID, not TypeId).
    const Astra::ComponentDescriptor* DescriptorFor(Astra::Registry& reg,
                                                      Astra::Entity e, const char* typeName)
    {
        for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            if (ci.meta && ci.meta->typeName == typeName)
                return ci.descriptor;
        return nullptr;
    }

    // A real CommandStack over a real Runtime -- StateId is what dirty rides on,
    // and a fake would not exercise the undo/redo id restoration that matters.
    struct Harness
    {
        Arcane::Runtime runtime{&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false};
        Arcane::CommandStack stack{[this]() -> Astra::Registry& { return runtime.Registry(); }};
        Astra::Entity entity{};
        const Astra::ComponentDescriptor* desc = nullptr;

        Harness()
        {
            Astra::Registry& reg = runtime.Registry();
            Arcane::RegisterSceneComponents(reg);
            entity = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(entity, Arcane::Transform{});
            desc = DescriptorFor(reg, entity, "Arcane::Transform");
        }

        void Edit(float x)
        {
            const Arcane::TransactionId t = stack.Begin("Move");
            stack.SnapshotComponent(entity, desc);
            runtime.Registry().GetComponent<Arcane::Transform>(entity)->position.x = x;
            stack.Commit(t);
        }
    };
}

TEST_CASE("a fresh session is Untitled and clean", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    CHECK(s.Path().empty());
    CHECK_FALSE(s.Id().IsValid());
    CHECK(s.DisplayName() == "Untitled");
    CHECK_FALSE(s.IsDirty(h.stack));
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("editing dirties the session and saving cleans it", "[editor][scene]")
{
    Harness h;
    SceneSession s;

    h.Edit(1.0f);
    CHECK(s.IsDirty(h.stack));

    s.MarkSaved(h.stack);
    CHECK_FALSE(s.IsDirty(h.stack));

    h.Edit(2.0f);
    CHECK(s.IsDirty(h.stack));
}

TEST_CASE("undoing back to the save point goes clean again", "[editor][scene]")
{
    // The reason dirty rides StateId rather than an edit counter.
    Harness h;
    SceneSession s;

    h.Edit(1.0f);
    s.MarkSaved(h.stack);
    h.Edit(2.0f);
    CHECK(s.IsDirty(h.stack));

    h.stack.Undo();
    CHECK_FALSE(s.IsDirty(h.stack));

    h.stack.Redo();
    CHECK(s.IsDirty(h.stack));
}

TEST_CASE("Adopt retargets path and id and marks clean", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    CHECK(s.IsDirty(h.stack));

    const Arcane::Guid id = Arcane::Guid::Generate();
    s.Adopt("D:/Games/G/Content/scenes/level_one.arcscene", id, h.stack);

    CHECK(s.Id() == id);
    CHECK(s.DisplayName() == "level_one");
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("Reset returns the session to Untitled and clean", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    s.Adopt("D:/a/b.arcscene", Arcane::Guid::Generate(), h.stack);
    h.Edit(1.0f);

    // New Scene clears the undo stack, which is what the host does around Reset.
    h.stack.Clear();
    s.Reset(h.stack);

    CHECK(s.Path().empty());
    CHECK_FALSE(s.Id().IsValid());
    CHECK(s.DisplayName() == "Untitled");
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("a clean session proceeds immediately; a dirty one parks the intent", "[editor][scene]")
{
    Harness h;
    SceneSession s;

    CHECK(s.Request(SceneIntent::NewScene, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::None);

    h.Edit(1.0f);
    CHECK_FALSE(s.Request(SceneIntent::OpenScene, "D:/a/b.arcscene", h.stack));
    CHECK(s.Pending() == SceneIntent::OpenScene);
    CHECK(s.PendingPath() == std::filesystem::path("D:/a/b.arcscene"));

    // Cancel: the intent is dropped and the session is untouched.
    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
    CHECK(s.IsDirty(h.stack));
}

TEST_CASE("a second request while one is pending is ignored", "[editor][scene]")
{
    // Same rule as DocumentHost's close flow: the modal is app-modal, so a
    // second intent arriving behind it would silently replace what the user is
    // being asked about.
    Harness h;
    SceneSession s;
    h.Edit(1.0f);

    REQUIRE_FALSE(s.Request(SceneIntent::OpenScene, "D:/a/b.arcscene", h.stack));
    CHECK_FALSE(s.Request(SceneIntent::Exit, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::OpenScene);
    CHECK(s.PendingPath() == std::filesystem::path("D:/a/b.arcscene"));
}

TEST_CASE("saving while an intent is pending lets it proceed", "[editor][scene]")
{
    // The Save branch of the confirm modal: the host saves, which marks clean;
    // the parked intent is then performed and cleared.
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    REQUIRE_FALSE(s.Request(SceneIntent::NewScene, {}, h.stack));

    s.MarkSaved(h.stack);
    CHECK_FALSE(s.IsDirty(h.stack));
    CHECK(s.Pending() == SceneIntent::NewScene);   // still parked for the host to act on

    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
}
