// Outliner slice 1: structural undo through the whole-registry memento.
// THE regression this design exists for: undoing a delete resurrects the
// EXACT entity id (binary restore round-trips the EntityManager), so later
// undo-stack entries referencing the entity stay valid.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <array>
#include <memory>

#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;

namespace
{
    // A registry in a swappable slot: restore REPLACES the object, mirroring
    // Runtime::RestoreRegistry. The resolver hands out the CURRENT one.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };

        World()
        {
            // Cross-DLL note (mirrors EntityOpsTest.cpp's World, PickBufferTest.cpp's
            // MakePickRegistry): EntityOps.cpp is compiled into Arcane.dll, so
            // Edit::CreateEntity's reg.AddComponent<T>(...) resolves component IDs
            // through Arcane.dll's own per-module TypeContext slot, not the one
            // main() installs in this test module. Pin that DLL slot to the shared
            // test context once (a throwaway Runtime installs it in Arcane.dll; the
            // slot persists after the Runtime is destroyed) so both modules agree on
            // component IDs -- otherwise EntityInfo (added inside Arcane.dll) would
            // be invisible to GetComponent<EntityInfo> called from this module.
            static const bool s_ctxPinned = []
            {
                Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
                return true;
            }();
            (void)s_ctxPinned;
            RegisterSceneComponents(*reg);
        }

        RegistryStateCommand::SnapshotFn Snapshot()
        {
            return [this]() -> std::vector<std::byte>
            {
                auto r = reg->Save();
                return r.IsOk() ? std::move(*r) : std::vector<std::byte>{};
            };
        }
        RegistryStateCommand::RestoreFn Restore()
        {
            return [this](std::span<const std::byte> bytes)
            {
                auto r = Astra::Registry::Load(bytes, creg);
                if (!r.IsOk())
                    return false;
                reg = std::move(*r);
                return true;
            };
        }
    };
}

TEST_CASE("delete-undo resurrects the exact entity id", "[outliner]")
{
    World w;
    Astra::Entity top  = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    Astra::Entity mid  = Edit::CreateEntity(*w.reg, top);
    Astra::Entity leaf = Edit::CreateEntity(*w.reg, mid);
    const Guid stableId = w.reg->GetComponent<EntityInfo>(mid)->id;

    const std::array<Astra::Entity, 1> doomed{ mid };
    REQUIRE(ApplyRegistryMutation(w.stack, "Delete Entity", w.Snapshot(), w.Restore(),
        [&] { return Edit::DeleteEntities(*w.reg, doomed) > 0; }));
    CHECK(w.reg->GetComponent<EntityInfo>(mid) == nullptr);   // gone
    CHECK(w.reg->GetParent(leaf) == top);                      // spliced

    w.stack.Undo();
    // SAME id/version works against the restored registry -- the whole point.
    EntityInfo* info = w.reg->GetComponent<EntityInfo>(mid);
    REQUIRE(info != nullptr);
    CHECK(info->id == stableId);
    CHECK(w.reg->GetParent(mid) == top);
    CHECK(w.reg->GetParent(leaf) == mid);

    w.stack.Redo();
    CHECK(w.reg->GetComponent<EntityInfo>(mid) == nullptr);
    CHECK(w.reg->GetParent(leaf) == top);

    w.stack.Undo();   // and back once more -- the memento swap is stable
    CHECK(w.reg->GetComponent<EntityInfo>(mid) != nullptr);
}

TEST_CASE("create-undo destroys; redo restores the SAME entity", "[outliner]")
{
    World w;
    Astra::Entity created = Astra::Entity::Invalid();
    REQUIRE(ApplyRegistryMutation(w.stack, "New Entity", w.Snapshot(), w.Restore(),
        [&]
        {
            created = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
            return created.IsValid();
        }));
    REQUIRE(created.IsValid());

    w.stack.Undo();
    CHECK(w.reg->GetComponent<EntityInfo>(created) == nullptr);
    w.stack.Redo();
    // The after-state restore brings back the entity under its ORIGINAL id --
    // a plain re-run of CreateEntity could not guarantee that.
    CHECK(w.reg->GetComponent<EntityInfo>(created) != nullptr);
}

TEST_CASE("no-op mutations produce no undo step", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> set{ a };
    // Already at root: unparent is a no-op -> refused push.
    CHECK_FALSE(ApplyRegistryMutation(w.stack, "Reparent", w.Snapshot(), w.Restore(),
        [&] { return Edit::Reparent(*w.reg, set, Astra::Entity::Invalid()) > 0; }));
    CHECK_FALSE(w.stack.CanUndo());
}

TEST_CASE("failed before-snapshot refuses the whole edit", "[outliner]")
{
    World w;
    bool mutated = false;
    CHECK_FALSE(ApplyRegistryMutation(
        w.stack, "Doomed",
        []() { return std::vector<std::byte>{}; },   // snapshot always fails
        w.Restore(),
        [&] { mutated = true; return true; }));
    CHECK_FALSE(mutated);          // the mutator never ran
    CHECK_FALSE(w.stack.CanUndo());
}

TEST_CASE("every structural op round-trips through the memento", "[outliner]")
{
    // Hide, rename, and add-component all ride the SAME memento path --
    // this proves each op's effect survives its own undo/redo cycle.
    World w;
    Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> set{ a };

    const Astra::ComponentDescriptor* spriteDesc = nullptr;
    w.reg->AddComponent<SpriteRenderer>(a, SpriteRenderer{});
    for (const Astra::Registry::ComponentInfo& ci : w.reg->InspectEntity(a))
        if (ci.meta && ci.meta->typeName == "Arcane::SpriteRenderer")
            spriteDesc = ci.descriptor;
    REQUIRE(spriteDesc != nullptr);
    w.reg->RemoveComponent<SpriteRenderer>(a);

    REQUIRE(ApplyRegistryMutation(w.stack, "Hide", w.Snapshot(), w.Restore(),
        [&] { return Edit::SetHiddenRecursive(*w.reg, a, true) > 0; }));
    REQUIRE(ApplyRegistryMutation(w.stack, "Rename", w.Snapshot(), w.Restore(),
        [&] { return Edit::RenameEntity(*w.reg, a, "Renamed"); }));
    REQUIRE(ApplyRegistryMutation(w.stack, "Add Sprite", w.Snapshot(), w.Restore(),
        [&] { return Edit::AddComponent(*w.reg, set, *spriteDesc) > 0; }));

    // Unwind all three, newest first.
    w.stack.Undo();
    CHECK(w.reg->GetComponent<SpriteRenderer>(a) == nullptr);
    w.stack.Undo();
    CHECK(w.reg->GetComponent<EntityInfo>(a)->name == "Entity");   // pre-rename
    w.stack.Undo();
    CHECK(w.reg->GetComponent<Hidden>(a) == nullptr);

    // And forward again.
    w.stack.Redo();
    w.stack.Redo();
    w.stack.Redo();
    CHECK(w.reg->GetComponent<Hidden>(a) != nullptr);
    CHECK(w.reg->GetComponent<EntityInfo>(a)->name == "Renamed");
    CHECK(w.reg->GetComponent<SpriteRenderer>(a) != nullptr);
}
