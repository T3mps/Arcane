// Outliner slice 1: EntityOps structural mutators, headless. Undo semantics
// are RegistryStateCommandTest's job -- these prove the raw mutations.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <array>
#include <memory>

#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        World()
        {
            // Cross-DLL note (mirrors PickBufferTest.cpp's MakePickRegistry):
            // EntityOps.cpp is compiled into Arcane.dll, so Edit::CreateEntity's
            // reg.AddComponent<T>(...) resolves component IDs through Arcane.dll's
            // own per-module TypeContext slot, not the one main() installs in this
            // test module. Pin that DLL slot to the shared test context once (a
            // throwaway Runtime installs it in Arcane.dll; the slot persists after
            // the Runtime is destroyed) so both modules agree on component IDs --
            // otherwise EntityInfo (added inside Arcane.dll) would be invisible to
            // GetComponent<EntityInfo> called from this module.
            static const bool s_ctxPinned = []
            {
                Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
                return true;
            }();
            (void)s_ctxPinned;
            RegisterSceneComponents(reg);
        }
    };
}

TEST_CASE("CreateEntity: Transform + EntityInfo + auto-name + parenting", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);

    CHECK(w.reg.GetComponent<Transform>(a) != nullptr);
    EntityInfo* ia = w.reg.GetComponent<EntityInfo>(a);
    EntityInfo* ib = w.reg.GetComponent<EntityInfo>(b);
    REQUIRE(ia != nullptr);
    REQUIRE(ib != nullptr);
    CHECK(ia->id.IsValid());
    CHECK(ia->name == "Entity");
    CHECK(ib->name == "Entity_2");          // collision-free auto-name
    CHECK(w.reg.GetParent(b) == a);
    CHECK(Edit::DisplayName(w.reg, a) == "Entity");
}

TEST_CASE("DisplayName falls back to the id for untagged entities", "[outliner]")
{
    World w;
    Astra::Entity raw = w.reg.CreateEntity();
    CHECK(Edit::DisplayName(w.reg, raw) ==
          "Entity " + std::to_string(raw.GetID()));
}

TEST_CASE("DeleteEntities splices children to the nearest survivor", "[outliner]")
{
    World w;
    Astra::Entity top = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity mid = Edit::CreateEntity(w.reg, top);
    Astra::Entity leaf = Edit::CreateEntity(w.reg, mid);

    // Delete the middle: the leaf must climb to top, not dangle or die.
    const std::array<Astra::Entity, 1> doomed{ mid };
    CHECK(Edit::DeleteEntities(w.reg, doomed) == 1);
    CHECK(w.reg.GetParent(leaf) == top);

    // Nested set (parent + child both doomed): survivor climbs past both.
    Astra::Entity mid2  = Edit::CreateEntity(w.reg, top);
    Astra::Entity mid3  = Edit::CreateEntity(w.reg, mid2);
    Astra::Entity leaf2 = Edit::CreateEntity(w.reg, mid3);
    const std::array<Astra::Entity, 2> doomed2{ mid2, mid3 };
    CHECK(Edit::DeleteEntities(w.reg, doomed2) == 2);
    CHECK(w.reg.GetParent(leaf2) == top);
}

TEST_CASE("Reparent refuses cycles wholesale, skips no-ops", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);
    Astra::Entity c = Edit::CreateEntity(w.reg, b);

    // a -> under its own grandchild = cycle: whole op refused.
    const std::array<Astra::Entity, 1> setA{ a };
    CHECK(Edit::Reparent(w.reg, setA, c) == 0);
    CHECK(!w.reg.GetParent(a).IsValid());

    // b already under a: no-op skip. c -> a: moves.
    const std::array<Astra::Entity, 2> setBC{ b, c };
    CHECK(Edit::Reparent(w.reg, setBC, a) == 1);
    CHECK(w.reg.GetParent(c) == a);

    // Unparent (invalid parent) pulls both to root.
    CHECK(Edit::Reparent(w.reg, setBC, Astra::Entity::Invalid()) == 2);
    CHECK(!w.reg.GetParent(b).IsValid());
}

TEST_CASE("Reparent refuses wholesale on a dead parent or a set-contained parent",
          "[outliner]")
{
    World w;

    // (a) A stale-but-nonnull parent handle passes IsValid() but is no
    // longer registry-alive: the whole op refuses and the live entity's
    // parent is unchanged.
    {
        Astra::Entity deadParent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity live = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.DestroyEntity(deadParent);

        const std::array<Astra::Entity, 1> set{ live };
        CHECK(Edit::Reparent(w.reg, set, deadParent) == 0);
        CHECK(!w.reg.GetParent(live).IsValid());
    }

    // (b) `parent` itself is inside `set`: the cycle pre-check refuses the
    // whole operation, so nothing in the set moves -- not just `parent`.
    {
        Astra::Entity parent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity other = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());

        const std::array<Astra::Entity, 2> set{ parent, other };
        CHECK(Edit::Reparent(w.reg, set, parent) == 0);
        CHECK(!w.reg.GetParent(other).IsValid());
    }
}

TEST_CASE("SetHiddenRecursive covers the subtree, idempotently", "[outliner]")
{
    World w;
    Astra::Entity top = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity kid = Edit::CreateEntity(w.reg, top);
    Astra::Entity grandkid = Edit::CreateEntity(w.reg, kid);

    CHECK(Edit::SetHiddenRecursive(w.reg, top, true) == 3);
    CHECK(w.reg.GetComponent<Hidden>(grandkid) != nullptr);
    CHECK(Edit::SetHiddenRecursive(w.reg, top, true) == 0);    // idempotent
    CHECK(Edit::SetHiddenRecursive(w.reg, kid, false) == 2);   // partial unhide
    CHECK(w.reg.GetComponent<Hidden>(top) != nullptr);
    CHECK(w.reg.GetComponent<Hidden>(grandkid) == nullptr);
}

TEST_CASE("RenameEntity adds EntityInfo when missing", "[outliner]")
{
    World w;
    Astra::Entity raw = w.reg.CreateEntity();
    CHECK(Edit::RenameEntity(w.reg, raw, "Boss Arena"));
    EntityInfo* info = w.reg.GetComponent<EntityInfo>(raw);
    REQUIRE(info != nullptr);
    CHECK(info->id.IsValid());               // fresh stable id minted
    CHECK(info->name == "Boss Arena");
    CHECK(Edit::RenameEntity(w.reg, raw, "Boss Arena 2"));
    CHECK(w.reg.GetComponent<EntityInfo>(raw)->name == "Boss Arena 2");
}

TEST_CASE("Add/RemoveComponent by descriptor over a set", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.AddComponent<SpriteRenderer>(a, SpriteRenderer{});   // a already has it

    const Astra::ComponentDescriptor* desc = nullptr;
    for (const Astra::Registry::ComponentInfo& ci : w.reg.InspectEntity(a))
        if (ci.meta && ci.meta->typeName == "Arcane::SpriteRenderer")
            desc = ci.descriptor;
    REQUIRE(desc != nullptr);

    const std::array<Astra::Entity, 2> set{ a, b };
    CHECK(Edit::AddComponent(w.reg, set, *desc) == 1);     // only b lacked it
    CHECK(w.reg.GetComponent<SpriteRenderer>(b) != nullptr);
    CHECK(Edit::RemoveComponent(w.reg, set, *desc) == 2);
    CHECK(w.reg.GetComponent<SpriteRenderer>(a) == nullptr);
}

TEST_CASE("Dead entities in a set no-op and don't inflate the returned count",
          "[outliner]")
{
    // A stale selection (the Outliner's future use case) can carry an
    // entity that died between selection and the op running. Every op
    // must treat it as absent, not count it as touched.
    World w;

    // (a) DeleteEntities: an already-destroyed entity in the set is not
    // recounted as destroyed.
    {
        Astra::Entity live = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity dead = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.DestroyEntity(dead);
        const std::array<Astra::Entity, 2> set{ live, dead };
        CHECK(Edit::DeleteEntities(w.reg, set) == 1);
        CHECK(!w.reg.IsValid(live));
    }

    // (b) DeleteEntities: a duplicate LIVE entity counts once -- the
    // header's documented "duplicates tolerated" contract.
    {
        Astra::Entity live = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        const std::array<Astra::Entity, 2> set{ live, live };
        CHECK(Edit::DeleteEntities(w.reg, set) == 1);
    }

    // (c) Reparent and AddComponent: a dead entity in the set is excluded
    // from both the moved/touched count and the effect.
    {
        Astra::Entity parent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity live   = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity dead   = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.DestroyEntity(dead);
        const std::array<Astra::Entity, 2> set{ live, dead };

        CHECK(Edit::Reparent(w.reg, set, parent) == 1);
        CHECK(w.reg.GetParent(live) == parent);

        Astra::Entity spriteHost = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.AddComponent<SpriteRenderer>(spriteHost, SpriteRenderer{});
        const Astra::ComponentDescriptor* desc = nullptr;
        for (const Astra::Registry::ComponentInfo& ci : w.reg.InspectEntity(spriteHost))
            if (ci.meta && ci.meta->typeName == "Arcane::SpriteRenderer")
                desc = ci.descriptor;
        REQUIRE(desc != nullptr);

        CHECK(Edit::AddComponent(w.reg, set, *desc) == 1);   // only `live` touched
        CHECK(w.reg.GetComponent<SpriteRenderer>(live) != nullptr);
    }

    // (d) SetHiddenRecursive on a destroyed root returns 0.
    {
        Astra::Entity dead = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.DestroyEntity(dead);
        CHECK(Edit::SetHiddenRecursive(w.reg, dead, true) == 0);
    }
}

TEST_CASE("Same-name rename is a no-op and reports false", "[outliner]")
{
    // Inline rename commits on defocus even when the user changed nothing;
    // reporting false lets ApplyRegistryMutation skip the memento push so
    // the undo history never records a step that reverts nothing.
    World w;
    Astra::Entity e = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    REQUIRE(Edit::RenameEntity(w.reg, e, "Hero"));
    CHECK_FALSE(Edit::RenameEntity(w.reg, e, "Hero"));   // unchanged -> false
    CHECK(Edit::RenameEntity(w.reg, e, "Hero2"));        // real change -> true
    CHECK(w.reg.GetComponent<EntityInfo>(e)->name == "Hero2");
}

TEST_CASE("CreateEntity under a dead parent creates at root", "[outliner]")
{
    // Documented fallback (slice-1 final review, Minor #2): a stale parent
    // handle silently no-ops in Registry::SetParent, so the entity lands
    // unparented. Pin the behavior the header now documents.
    World w;
    Astra::Entity parent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.DestroyEntity(parent);
    Astra::Entity e = Edit::CreateEntity(w.reg, parent);
    CHECK(w.reg.IsValid(e));
    CHECK_FALSE(w.reg.GetParent(e).IsValid());
}

TEST_CASE("SelectionRoots drops entities covered by a selected ancestor", "[outliner]")
{
    // Transform edits must apply to roots ONLY: a selected child already
    // rides its selected parent through WorldTransform propagation, so
    // transforming both would double-move the child.
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);          // child of a
    Astra::Entity c = Edit::CreateEntity(w.reg, b);          // grandchild of a
    Astra::Entity lone = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());

    SECTION("ancestor in the set covers descendants at any depth")
    {
        const std::array<Astra::Entity, 4> set{ a, b, c, lone };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, set);
        REQUIRE(roots.size() == 2);
        CHECK(roots[0] == a);        // input order preserved
        CHECK(roots[1] == lone);
    }

    SECTION("a child selected WITHOUT its parent is its own root")
    {
        const std::array<Astra::Entity, 2> set{ c, lone };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, set);
        REQUIRE(roots.size() == 2);
        CHECK(roots[0] == c);
    }

    SECTION("dead entities are skipped and duplicates collapse")
    {
        Astra::Entity dead = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.DestroyEntity(dead);
        const std::array<Astra::Entity, 4> set{ lone, dead, lone, a };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, set);
        REQUIRE(roots.size() == 2);      // lone once, a once
        CHECK(roots[0] == lone);
        CHECK(roots[1] == a);
    }
}
