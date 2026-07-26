// Outliner slice 4: the pure component-catalog core behind the Inspector's
// Add Component popup. The ImGui shell (EditorPanels.cpp) is NOT compiled into
// this exe, so these cover the filtering/counting rules only; the popup itself
// is desk-verified.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Component/Component.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ComponentCatalog.hpp"
#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;
using Arcane::Editor::BuildComponentCatalog;
using Arcane::Editor::ComponentCatalogEntry;
using Arcane::Editor::IsSystemManagedComponent;

namespace
{
    // A registry in a swappable slot (mirrors RegistryStateCommandTest's World):
    // a structural undo REPLACES the object, so the resolver hands out the
    // CURRENT one.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };

        World()
        {
            // EntityOps.cpp is compiled into Arcane.dll, so its AddComponent<T>
            // resolves ids through Arcane.dll's own per-module TypeContext slot.
            // Pin that slot to the shared test context; NEVER construct a bare
            // Arcane::Runtime here (it would install an unshared context and the
            // Edit:: ops would silently report 0 changes).
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            RegisterSceneComponents(*reg);
            // Registered so the PhysicsBodyRef hide-list assertion is a real
            // check rather than a vacuous one.
            RegisterPhysicsComponents(*reg);
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

    const ComponentCatalogEntry* Find(const std::vector<ComponentCatalogEntry>& v,
                                      std::string_view name)
    {
        for (const ComponentCatalogEntry& e : v)
            if (e.typeName == name)
                return &e;
        return nullptr;
    }
}

TEST_CASE("IsSystemManagedComponent covers the derived types plus EntityInfo", "[editor][outliner]")
{
    CHECK(IsSystemManagedComponent("Arcane::WorldTransform"));
    CHECK(IsSystemManagedComponent("Arcane::PreviousTransform"));
    CHECK(IsSystemManagedComponent("Arcane::PhysicsBodyRef"));
    // EntityInfo joined the list in the 2026-07-26 review fix: Edit::AddComponent
    // default-constructs, so a generic add stamped a NIL Guid on every selected
    // entity and a generic remove wiped the durable cross-save identity. The
    // Outliner owns this component via create + rename.
    CHECK(IsSystemManagedComponent("Arcane::EntityInfo"));

    // Transform is deliberately REMOVABLE (spec section 5).
    CHECK_FALSE(IsSystemManagedComponent("Arcane::Transform"));
    CHECK_FALSE(IsSystemManagedComponent("Arcane::SpriteRenderer"));
    CHECK_FALSE(IsSystemManagedComponent("Arcane::Hidden"));
    CHECK_FALSE(IsSystemManagedComponent(""));
}

TEST_CASE("BuildComponentCatalog hides system-managed types and sorts by name", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    const std::vector<ComponentCatalogEntry> all = BuildComponentCatalog(*w.reg, sel, "");
    REQUIRE_FALSE(all.empty());

    CHECK(Find(all, "Arcane::WorldTransform") == nullptr);
    CHECK(Find(all, "Arcane::PreviousTransform") == nullptr);
    CHECK(Find(all, "Arcane::PhysicsBodyRef") == nullptr);
    CHECK(Find(all, "Arcane::Transform") != nullptr);
    CHECK(Find(all, "Arcane::SpriteRenderer") != nullptr);

    CHECK(std::is_sorted(all.begin(), all.end(),
                         [](const ComponentCatalogEntry& x, const ComponentCatalogEntry& y)
                         { return x.typeName < y.typeName; }));

    for (const ComponentCatalogEntry& e : all)
        CHECK(e.desc != nullptr);
}

TEST_CASE("BuildComponentCatalog missingCount is the set Edit::AddComponent would touch",
          "[editor][outliner]")
{
    World w;
    // `a` carries Transform (CreateEntity adds it); `b` is a bare entity.
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = w.reg->CreateEntity();
    const std::array<Astra::Entity, 2> sel{ a, b };

    const std::vector<ComponentCatalogEntry> cat = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* transform = Find(cat, "Arcane::Transform");
    const ComponentCatalogEntry* sprite    = Find(cat, "Arcane::SpriteRenderer");
    REQUIRE(transform != nullptr);
    REQUIRE(sprite != nullptr);

    CHECK(transform->missingCount == 1);   // only b lacks it
    CHECK(sprite->missingCount == 2);      // neither has it

    // And the count is exactly what the mutator reports.
    CHECK(Edit::AddComponent(*w.reg, sel, *transform->desc) == 1);
    CHECK(Edit::AddComponent(*w.reg, sel, *sprite->desc) == 2);

    const std::vector<ComponentCatalogEntry> after = BuildComponentCatalog(*w.reg, sel, "");
    REQUIRE(Find(after, "Arcane::Transform") != nullptr);
    REQUIRE(Find(after, "Arcane::SpriteRenderer") != nullptr);
    CHECK(Find(after, "Arcane::Transform")->missingCount == 0);
    CHECK(Find(after, "Arcane::SpriteRenderer")->missingCount == 0);
}

TEST_CASE("BuildComponentCatalog counts TAG components via HasComponentByHash",
          "[editor][outliner]")
{
    // Regression guard: Hidden is an empty component, so GetComponentByHash
    // returns null even when the entity carries it. Counting with the getter
    // would report it missing forever.
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    const std::vector<ComponentCatalogEntry> beforeCat = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* before = Find(beforeCat, "Arcane::Hidden");
    REQUIRE(before != nullptr);
    CHECK(before->missingCount == 1);

    REQUIRE(Edit::SetHiddenRecursive(*w.reg, a, true) == 1);

    const std::vector<ComponentCatalogEntry> after = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* hidden = Find(after, "Arcane::Hidden");
    REQUIRE(hidden != nullptr);
    CHECK(hidden->missingCount == 0);
}

TEST_CASE("BuildComponentCatalog filter is a case-insensitive substring", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    CHECK(Find(BuildComponentCatalog(*w.reg, sel, "sprite"), "Arcane::SpriteRenderer") != nullptr);
    CHECK(Find(BuildComponentCatalog(*w.reg, sel, "SPRITE"), "Arcane::SpriteRenderer") != nullptr);
    CHECK(Find(BuildComponentCatalog(*w.reg, sel, "sprite"), "Arcane::Transform") == nullptr);
    CHECK(BuildComponentCatalog(*w.reg, sel, "zzzz-no-such-component").empty());

    // The filter runs over the FULL reflected name, so the namespace matches too.
    CHECK_FALSE(BuildComponentCatalog(*w.reg, sel, "arcane::").empty());
}

TEST_CASE("BuildComponentCatalog skips dead selection entries", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const Astra::Entity doomed = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> kill{ doomed };
    REQUIRE(Edit::DeleteEntities(*w.reg, kill) == 1);

    const std::array<Astra::Entity, 2> sel{ a, doomed };
    const std::vector<ComponentCatalogEntry> cat = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* sprite = Find(cat, "Arcane::SpriteRenderer");
    REQUIRE(sprite != nullptr);
    CHECK(sprite->missingCount == 1);   // the dead handle contributes nothing
}

TEST_CASE("catalog add/remove round-trip through ApplyRegistryMutation", "[editor][outliner]")
{
    // The exact call shape the Inspector uses: pick a descriptor out of the
    // catalog, mutate the whole selection, undo, redo.
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 2> sel{ a, b };

    const std::vector<ComponentCatalogEntry> cat = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* sprite = Find(cat, "Arcane::SpriteRenderer");
    REQUIRE(sprite != nullptr);
    const std::uint64_t spriteHash = sprite->desc->hash;

    REQUIRE(ApplyRegistryMutation(w.stack, "Add Component", w.Snapshot(), w.Restore(),
                                  [&] { return Edit::AddComponent(*w.reg, sel, *sprite->desc) > 0; }));
    CHECK(w.reg->HasComponentByHash(a, spriteHash));
    CHECK(w.reg->HasComponentByHash(b, spriteHash));

    w.stack.Undo();
    CHECK_FALSE(w.reg->HasComponentByHash(a, spriteHash));
    CHECK_FALSE(w.reg->HasComponentByHash(b, spriteHash));

    w.stack.Redo();
    CHECK(w.reg->HasComponentByHash(a, spriteHash));
    CHECK(w.reg->HasComponentByHash(b, spriteHash));

    // Removal is one step too. Re-fetch the descriptor: Restore REPLACED the
    // registry object, though the ComponentRegistry (and so the descriptor
    // addresses) is shared and survives.
    const std::vector<ComponentCatalogEntry> cat2 = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* sprite2 = Find(cat2, "Arcane::SpriteRenderer");
    REQUIRE(sprite2 != nullptr);
    REQUIRE(ApplyRegistryMutation(w.stack, "Remove Component", w.Snapshot(), w.Restore(),
                                  [&] { return Edit::RemoveComponent(*w.reg, sel, *sprite2->desc) > 0; }));
    CHECK_FALSE(w.reg->HasComponentByHash(a, spriteHash));

    w.stack.Undo();
    CHECK(w.reg->HasComponentByHash(a, spriteHash));
    CHECK(w.reg->HasComponentByHash(b, spriteHash));
}

TEST_CASE("a fresh Runtime registers the engine's own component roster", "[editor][outliner]")
{
    // THE regression this exists for (found at desk 2026-07-26, Aphelyon
    // project): the Add Component catalog can only offer what the runtime
    // ComponentRegistry knows, and NOTHING registered the engine's own scene
    // components in a live host -- the roster came entirely from whatever the
    // hosted game plugin chose to ReRegisterComponent<T>() in its Init. A
    // project whose plugin registered two types showed a two-row, fully
    // disabled catalog, so "+ Add Component" appeared to do nothing.
    //
    // The same gap silently dropped EntityInfo/Hidden when a runtime host
    // loaded a scene the editor saved: SceneSerializer skips a type that is
    // reflected but not REGISTERED as a component.
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity e = reg.CreateEntity();
    const std::array<Astra::Entity, 1> sel{ e };

    const std::vector<ComponentCatalogEntry> cat = BuildComponentCatalog(reg, sel, "");

    CHECK(Find(cat, "Arcane::Transform") != nullptr);
    CHECK(Find(cat, "Arcane::SpriteRenderer") != nullptr);
    CHECK(Find(cat, "Arcane::PostProcess") != nullptr);
    CHECK(Find(cat, "Arcane::Hidden") != nullptr);
    CHECK(Find(cat, "Arcane::RigidBody2D") != nullptr);
    CHECK(Find(cat, "Arcane::Collider2D") != nullptr);

    // The hide-list still applies to the engine roster.
    CHECK(Find(cat, "Arcane::WorldTransform") == nullptr);
    CHECK(Find(cat, "Arcane::PreviousTransform") == nullptr);
    CHECK(Find(cat, "Arcane::PhysicsBodyRef") == nullptr);
    CHECK(Find(cat, "Arcane::EntityInfo") == nullptr);

    // EntityInfo is hidden from the CATALOG but must still be REGISTERED -- the
    // other half of the roster bug (SceneSerializer silently drops a type that is
    // reflected but not registered as a component, so an editor-saved scene lost
    // its entity names and identities in a runtime host). Asserted against the
    // ComponentRegistry directly, since the catalog can no longer witness it.
    const Astra::ComponentRegistry* creg = reg.GetComponentRegistry();
    REQUIRE(creg != nullptr);
    bool sawEntityInfo = false;
    creg->ForEachComponent([&](Astra::ComponentID, const Astra::ComponentDescriptor& d)
    {
        if (d.meta && d.meta->typeName == "Arcane::EntityInfo")
            sawEntityInfo = true;
    });
    CHECK(sawEntityInfo);

    // A bare entity lacks all of them, so every row is offered as addable --
    // this is exactly the state that was broken at desk.
    for (const ComponentCatalogEntry& c : cat)
        CHECK(c.missingCount == 1);
}

TEST_CASE("no-op add pushes no undo step", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    const std::vector<ComponentCatalogEntry> cat = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* transform = Find(cat, "Arcane::Transform");
    REQUIRE(transform != nullptr);
    CHECK(transform->missingCount == 0);   // CreateEntity already added it

    CHECK_FALSE(ApplyRegistryMutation(w.stack, "Add Component", w.Snapshot(), w.Restore(),
                                      [&] { return Edit::AddComponent(*w.reg, sel, *transform->desc) > 0; }));
    CHECK_FALSE(w.stack.CanUndo());
}
