// SceneSession: the editor's scene identity + dirty state + the unsaved-changes
// confirm machine. Entirely pure -- no ImGui, no filesystem -- so the whole
// state machine is driven headlessly here.

#include <catch2/catch_test_macros.hpp>

#include "Scene/SceneSession.hpp"
#include "Helpers/AssetFileOpsFakes.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

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

    // A real CommandStack over a real Runtime -- SceneStateId is what dirty rides on,
    // and a fake would not exercise the undo/redo id restoration that matters.
    struct Harness
    {
        Arcane::Runtime runtime{Arcane::Test::Process()};
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

    struct DocumentStep final : Arcane::ICommand   // a material/sprite/mesh-style step
    {
        void Undo() override {}
        void Redo() override {}
        const char* Label() const override { return "Edit Param"; }
        bool AffectsScene() const override { return false; }
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
    // The reason dirty rides SceneStateId rather than an edit counter.
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

TEST_CASE("a document step leaves the scene clean", "[editor][scene][undo]")
{
    Harness h;
    SceneSession s;
    h.stack.Push(std::make_unique<DocumentStep>());
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("save, document step, undo: the scene stays clean throughout", "[editor][scene][undo]")
{
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    s.MarkSaved(h.stack);
    h.stack.Push(std::make_unique<DocumentStep>());
    CHECK_FALSE(s.IsDirty(h.stack));
    h.stack.Undo();                                 // the document step
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("document steps that evict the saved scene step leave the scene clean", "[editor][scene][undo]")
{
    Harness h;
    SceneSession s;
    Arcane::UndoLimits limits;
    limits.maxSteps = 2;
    h.stack.SetLimits(limits);
    h.Edit(1.0f);
    s.MarkSaved(h.stack);
    for (int i = 0; i < 3; ++i)
        h.stack.Push(std::make_unique<DocumentStep>());   // the saved scene step falls off the cap
    CHECK(h.stack.TouchedSinceState(s.SavedStateId()).baselineFound);   // no all-entities asterisks
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("undoing a scene step past the save point reads dirty, even under a document step", "[editor][scene][undo]")
{
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    s.MarkSaved(h.stack);
    h.stack.Push(std::make_unique<DocumentStep>());
    h.stack.Undo();
    h.stack.Undo();                                 // past the saved scene step
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
    // Adopt must run against a NONZERO StateId (i.e. after an edit), or
    // m_savedStateId lands on 0 -- the same value Clear() leaves StateId() at
    // -- and the final IsDirty check would pass even if Reset forgot to call
    // MarkSaved. Edit again after Adopt so Reset is the thing that re-cleans.
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    s.Adopt("D:/a/b.arcscene", Arcane::Guid::Generate(), h.stack);
    h.Edit(2.0f);

    // New Scene clears the undo stack, which is what the host does around Reset.
    h.stack.Clear("New scene");
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

    // Clean is not the same as available: Request checks "already pending"
    // BEFORE "is dirty", so a competing request must still be refused here --
    // swapping that order would let this one slip through onto a clean stack.
    CHECK_FALSE(s.Request(SceneIntent::Exit, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::NewScene);

    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("TakePending consumes the parked intent, unwedging future requests", "[editor][scene]")
{
    // The bug this exists to prevent: Reset/Adopt do not touch m_pending (see
    // their doc comments), so a host that forgets to consume the parked intent
    // wedges every future Request forever -- the pending-check in Request runs
    // before the dirty-check, so it refuses unconditionally, forever. TakePending
    // makes forgetting impossible: it is the one call that reads the parked
    // request and clears it in the same step.
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    REQUIRE_FALSE(s.Request(SceneIntent::OpenScene, "D:/a/b.arcscene", h.stack));

    const SceneSession::PendingRequest req = s.TakePending();
    CHECK(req.intent == SceneIntent::OpenScene);
    CHECK(req.path == std::filesystem::path("D:/a/b.arcscene"));
    CHECK(s.Pending() == SceneIntent::None);

    // A wedged session would refuse this unconditionally no matter the stack
    // state -- prove it is not wedged by going clean and getting an immediate
    // accept, not another silent refusal.
    s.MarkSaved(h.stack);
    CHECK(s.Request(SceneIntent::Exit, {}, h.stack));
}

TEST_CASE("LaunchStandalone parks on a dirty scene", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    s.Adopt("D:/Games/G/Content/scenes/level_one.arcscene",
            Arcane::Guid::Generate(), h.stack);
    h.Edit(1.0f);

    CHECK_FALSE(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::LaunchStandalone);

    const SceneSession::PendingRequest req = s.TakePending();
    CHECK(req.intent == SceneIntent::LaunchStandalone);
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("LaunchStandalone parks on a never-saved scene even when clean", "[editor][scene]")
{
    // A nil scene guid means the spawned runtime would boot the manifest's
    // bootScene instead of what is on screen -- "never saved" is exactly as
    // unready as "dirty" for this one intent (LaunchStandalone's old guard,
    // now owned by the machine).
    Harness h;
    SceneSession s;                       // Untitled: nil id, clean
    CHECK_FALSE(s.IsDirty(h.stack));
    CHECK_FALSE(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::LaunchStandalone);
    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("LaunchStandalone acts immediately on a saved clean scene", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    s.Adopt("D:/Games/G/Content/scenes/level_one.arcscene",
            Arcane::Guid::Generate(), h.stack);
    CHECK(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("a second Request while LaunchStandalone is parked is ignored", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    CHECK_FALSE(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK_FALSE(s.Request(SceneIntent::OpenScene, "other.arcscene", h.stack));
    CHECK(s.Pending() == SceneIntent::LaunchStandalone);
}

TEST_CASE("a module-reload clear names its cause and keeps dirty/clean as it was", "[editor][scene][undo][reload]")
{
    Harness h;
    SceneSession s;
    SECTION("clean stays clean")
    {
        h.Edit(1.0f);
        s.MarkSaved(h.stack);
        ClearHistoryForModuleReload(h.stack, s);
        CHECK_FALSE(h.stack.CanUndo());
        CHECK(h.stack.ClearedReason() == "Game module reloaded");
        CHECK_FALSE(s.IsDirty(h.stack));
    }
    SECTION("dirty stays dirty, even saved at state 0")
    {
        s.MarkSaved(h.stack);                     // saved at 0
        h.Edit(1.0f);
        REQUIRE(s.IsDirty(h.stack));
        ClearHistoryForModuleReload(h.stack, s);  // SceneStateId is 0 again...
        CHECK(s.IsDirty(h.stack));                // ...but the edits are unsaved
        h.Edit(2.0f);
        CHECK(s.IsDirty(h.stack));
    }
}

TEST_CASE("asset file steps never dirty the scene; scene steps still do", "[editor][scene][assetops]")
{
    using namespace Arcane::Editor;
    Harness h;
    SceneSession s;
    s.MarkSaved(h.stack);
    Arcane::Test::AssetOpsWorld w("scene_clean");
    const Arcane::Guid tex = w.Write("uv_marker.png", "png");
    Arcane::Test::FakeAssetOpHost host(w);
    AssetFileOpExecutor exec(host, h.stack, w.content);

    REQUIRE(exec.Execute(w.Plan(AssetOpKind::Delete, { tex }), h.stack).ok);
    CHECK_FALSE(s.IsDirty(h.stack));
    h.stack.Undo();
    CHECK_FALSE(s.IsDirty(h.stack));
    h.Edit(1.0f);
    CHECK(s.IsDirty(h.stack));
}
