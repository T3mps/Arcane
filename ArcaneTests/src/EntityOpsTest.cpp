// Outliner slice 1: EntityOps structural mutators, headless. Undo semantics
// are RegistryStateCommandTest's job -- these prove the raw mutations.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <array>
#include <memory>
#include <span>

#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;
using Catch::Matchers::WithinAbs;

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
            // test module. Pin that DLL slot to the shared test context so both
            // modules agree on component IDs -- otherwise Identity (added inside
            // Arcane.dll) would be invisible to GetComponent<Identity> called
            // from this module.
            //
            // Belt-and-braces: test_main pins Arcane.dll's TypeContext slot once
            // before any test runs, which is the real guarantee (per-type IDs are
            // cached in per-module magic statics and never re-resolve, so a late
            // pin cannot repair an already-cached id). Re-pinning here only keeps
            // the slot pointed at the shared context; never install an unshared
            // one anywhere in this suite.
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            RegisterSceneComponents(reg);
        }
    };
}

TEST_CASE("CreateEntity: Transform + Identity + auto-name + parenting", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);

    CHECK(w.reg.GetComponent<Transform>(a) != nullptr);
    Identity* ia = w.reg.GetComponent<Identity>(a);
    Identity* ib = w.reg.GetComponent<Identity>(b);
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

TEST_CASE("RenameEntity requires Identity -- rename never mints identity", "[outliner]")
{
    World w;

    // A raw registry entity is the runtime-spawn shape: no Identity.
    // Runtime spawns have no durable identity; the op must refuse, not repair.
    Astra::Entity raw = w.reg.CreateEntity();
    CHECK_FALSE(Edit::RenameEntity(w.reg, raw, "Boss Arena"));
    CHECK(w.reg.GetComponent<Identity>(raw) == nullptr);   // nothing added

    // An authored entity renames normally, and its Guid is untouched --
    // identity is creation-time only (UE: ActorLabel/ActorGuid are intrinsic
    // AActor fields, Actor.h:1055/:1188; there is no "add identity" edit).
    Astra::Entity authored = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Identity* info = w.reg.GetComponent<Identity>(authored);
    REQUIRE(info != nullptr);
    const Guid before = info->id;
    CHECK(Edit::RenameEntity(w.reg, authored, "Boss Arena"));
    CHECK(w.reg.GetComponent<Identity>(authored)->name == "Boss Arena");
    CHECK(w.reg.GetComponent<Identity>(authored)->id == before);
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
    CHECK(w.reg.GetComponent<Identity>(e)->name == "Hero2");
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

TEST_CASE("WorldMatrix composes the parent chain", "[outliner]")
{
    // World(child) must equal World(parent) * Local(child) -- computed from the
    // live graph, not the WorldTransform component (which EditorApp's gizmo
    // deliberately avoids; see EntityOps.hpp).
    World w;
    Astra::Entity parent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity child  = Edit::CreateEntity(w.reg, parent);

    Transform* tp = w.reg.GetComponent<Transform>(parent);
    tp->position = glm::vec3(5.0f, -3.0f, 0.0f);
    tp->rotation = Arcane::RotationAboutZ(0.6f);

    Transform* tc = w.reg.GetComponent<Transform>(child);
    tc->position = glm::vec3(2.0f, 0.0f, 0.0f);

    const glm::mat4 expected = tp->ToMatrix() * tc->ToMatrix();
    const glm::mat4 actual = Edit::WorldMatrix(w.reg, child);

    const GizmoTransform expDecomp = DecomposeTRS(expected);
    const GizmoTransform actDecomp = DecomposeTRS(actual);
    CHECK_THAT(actDecomp.position.x, WithinAbs(expDecomp.position.x, 1e-4f));
    CHECK_THAT(actDecomp.position.y, WithinAbs(expDecomp.position.y, 1e-4f));

    // A root's world matrix is just its own local matrix.
    const GizmoTransform parentDecomp = DecomposeTRS(Edit::WorldMatrix(w.reg, parent));
    CHECK_THAT(parentDecomp.position.x, WithinAbs(tp->position.x, 1e-4f));
    CHECK_THAT(parentDecomp.position.y, WithinAbs(tp->position.y, 1e-4f));
}

TEST_CASE("ParentWorldMatrix: identity for a root, parent's world matrix for a child",
          "[outliner]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.GetComponent<Transform>(root)->position = glm::vec3(3.0f, 4.0f, 0.0f);
    w.reg.GetComponent<Transform>(root)->rotation = Arcane::RotationAboutZ(0.4f);
    Astra::Entity child = Edit::CreateEntity(w.reg, root);
    w.reg.GetComponent<Transform>(child)->position = glm::vec3(1.0f, 1.0f, 0.0f);

    const glm::mat4 identity(1.0f);
    const glm::mat4 rootParentWorld = Edit::ParentWorldMatrix(w.reg, root);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK_THAT(rootParentWorld[col][row], WithinAbs(identity[col][row], 1e-6f));

    const glm::mat4 childParentWorld = Edit::ParentWorldMatrix(w.reg, child);
    const glm::mat4 rootWorld = Edit::WorldMatrix(w.reg, root);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK_THAT(childParentWorld[col][row], WithinAbs(rootWorld[col][row], 1e-6f));
}

TEST_CASE("Gizmo group delta crosses differently-parented roots correctly in WORLD space",
          "[outliner]")
{
    // The regression this whole task exists for. Transform is parent-local, so a
    // group delta computed/replayed on LOCAL poses is wrong the instant two
    // selected roots sit under differently-oriented parents. This performs
    // EXACTLY the math EditorApp performs -- DecomposeTRS(WorldMatrix(e)) ->
    // MakeGroupDelta -> ApplyGroupDelta -> inverse(ParentWorldMatrix(e)) *
    // ComposeTRS -> DecomposeTRS -- and pins the outcome: a world (5,0)
    // translate must move BOTH children by world (5,0), no matter how their
    // parents are rotated.
    auto runGroupTranslate = [](Astra::Registry& reg,
                                std::span<const Astra::Entity> roots,
                                Astra::Entity primary, glm::vec2 worldDelta)
    {
        const GizmoTransform startPrimary = DecomposeTRS(Edit::WorldMatrix(reg, primary));
        GizmoTransform endPrimary = startPrimary;
        endPrimary.position += worldDelta;
        const GizmoGroupDelta gd = MakeGroupDelta(startPrimary, endPrimary);

        for (Astra::Entity e : roots)
        {
            const GizmoTransform startWorld = DecomposeTRS(Edit::WorldMatrix(reg, e));
            const GizmoTransform w = ApplyGroupDelta(startWorld, gd);
            const glm::mat4 localMat =
                glm::inverse(Edit::ParentWorldMatrix(reg, e)) * ComposeTRS(w);
            const GizmoTransform r = DecomposeTRS(localMat);
            Transform* t = reg.GetComponent<Transform>(e);
            // Mirrors EditorAppFrame.cpp's write-back exactly, INCLUDING its
            // Task 3 (F1) merge: the 2D gizmo carries position.z/scale.z
            // through untouched and can only express a turn about +Z.
            t->position = glm::vec3(r.position, t->position.z);
            t->rotation = Arcane::RotationAboutZ(r.rotation);
            t->scale    = glm::vec3(r.scale, t->scale.z);
        }
    };

    SECTION("both parents unrotated")
    {
        World w;
        Astra::Entity parentA = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity childA  = Edit::CreateEntity(w.reg, parentA);
        Astra::Entity parentB = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity childB  = Edit::CreateEntity(w.reg, parentB);

        w.reg.GetComponent<Transform>(parentA)->position = glm::vec3(10.0f, 0.0f, 0.0f);
        w.reg.GetComponent<Transform>(childA)->position  = glm::vec3(1.0f, 0.0f, 0.0f);
        w.reg.GetComponent<Transform>(parentB)->position = glm::vec3(0.0f, 10.0f, 0.0f);
        w.reg.GetComponent<Transform>(childB)->position  = glm::vec3(1.0f, 0.0f, 0.0f);

        // Both children are selection roots -- neither's ancestor is selected.
        const std::array<Astra::Entity, 2> sel{ childA, childB };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, sel);
        REQUIRE(roots.size() == 2);

        const glm::vec2 worldBeforeA(Edit::WorldMatrix(w.reg, childA)[3]);
        const glm::vec2 worldBeforeB(Edit::WorldMatrix(w.reg, childB)[3]);

        runGroupTranslate(w.reg, roots, childA, glm::vec2(5.0f, 0.0f));

        const glm::vec2 worldAfterA(Edit::WorldMatrix(w.reg, childA)[3]);
        const glm::vec2 worldAfterB(Edit::WorldMatrix(w.reg, childB)[3]);
        CHECK_THAT(worldAfterA.x, WithinAbs(worldBeforeA.x + 5.0f, 1e-4f));
        CHECK_THAT(worldAfterA.y, WithinAbs(worldBeforeA.y, 1e-4f));
        CHECK_THAT(worldAfterB.x, WithinAbs(worldBeforeB.x + 5.0f, 1e-4f));
        CHECK_THAT(worldAfterB.y, WithinAbs(worldBeforeB.y, 1e-4f));
    }

    SECTION("parent B rotated 90 degrees")
    {
        World w;
        Astra::Entity parentA = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity childA  = Edit::CreateEntity(w.reg, parentA);
        Astra::Entity parentB = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        Astra::Entity childB  = Edit::CreateEntity(w.reg, parentB);

        w.reg.GetComponent<Transform>(parentA)->position = glm::vec3(10.0f, 0.0f, 0.0f);
        w.reg.GetComponent<Transform>(childA)->position  = glm::vec3(1.0f, 0.0f, 0.0f);
        w.reg.GetComponent<Transform>(parentB)->position = glm::vec3(0.0f, 10.0f, 0.0f);
        w.reg.GetComponent<Transform>(parentB)->rotation = Arcane::RotationAboutZ(1.5707963267948966f);   // 90deg
        w.reg.GetComponent<Transform>(childB)->position  = glm::vec3(1.0f, 0.0f, 0.0f);

        const std::array<Astra::Entity, 2> sel{ childA, childB };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, sel);
        REQUIRE(roots.size() == 2);

        const glm::vec2 worldBeforeA(Edit::WorldMatrix(w.reg, childA)[3]);
        const glm::vec2 worldBeforeB(Edit::WorldMatrix(w.reg, childB)[3]);

        runGroupTranslate(w.reg, roots, childA, glm::vec2(5.0f, 0.0f));

        const glm::vec2 worldAfterA(Edit::WorldMatrix(w.reg, childA)[3]);
        const glm::vec2 worldAfterB(Edit::WorldMatrix(w.reg, childB)[3]);
        // Under the OLD local-space code, childB's local +5 X would have moved
        // it along world +Y instead (parent B is rotated 90deg) -- this makes
        // the old behaviour impossible to pass accidentally.
        CHECK_THAT(worldAfterA.x, WithinAbs(worldBeforeA.x + 5.0f, 1e-4f));
        CHECK_THAT(worldAfterA.y, WithinAbs(worldBeforeA.y, 1e-4f));
        CHECK_THAT(worldAfterB.x, WithinAbs(worldBeforeB.x + 5.0f, 1e-4f));
        CHECK_THAT(worldAfterB.y, WithinAbs(worldBeforeB.y, 1e-4f));
    }
}

TEST_CASE("CreateEntityInScene parents the top-level create under SceneRoot, "
          "surviving a save/load round trip", "[outliner]")
{
    // The regression this test exists for: the Outliner's empty-space "New
    // Entity" used to call Edit::CreateEntity(reg, Astra::Entity::Invalid())
    // directly, which left the entity an unparented SIBLING of SceneRoot, not
    // a descendant. SceneSerializer::SaveJson and TransformPropagationSystem
    // both walk ONLY the SceneRoot subtree, so that entity never rendered and
    // was silently dropped by the next Save -- authored, saved, reloaded, gone.
    World w;
    Astra::Entity root = Arcane::Scene::CreateEmpty(w.reg);

    Astra::Entity created = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    REQUIRE(created.IsValid());
    CHECK(w.reg.GetParent(created) == root);

    // An explicit parent (the row context menu's "New Child Entity" path)
    // must behave exactly as CreateEntity always has -- unification must not
    // change that path's contract.
    Astra::Entity child = Edit::CreateEntityInScene(w.reg, created);
    REQUIRE(child.IsValid());
    CHECK(w.reg.GetParent(child) == created);

    Identity* createdInfo = w.reg.GetComponent<Identity>(created);
    REQUIRE(createdInfo != nullptr);
    const std::string createdName = createdInfo->name;

    const nlohmann::json doc = Arcane::Scene::SaveJson(w.reg);

    auto components2 = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg2(components2);
    RegisterSceneComponents(reg2);
    REQUIRE(Arcane::Scene::LoadJson(reg2, doc));

    bool found = false;
    reg2.CreateView<Identity>().ForEach(
        [&](Astra::Entity, Identity& info) { found = found || info.name == createdName; });
    CHECK(found);
}

TEST_CASE("CreateEntityInScene refuses when there is no SceneRoot at all",
          "[outliner]")
{
    // Mirrors DoSaveScene's existing "no SceneRoot" refusal (EditorApp.cpp):
    // an entity created outside the SceneRoot subtree can never be rendered
    // or saved, so silently creating one here would just relocate the same
    // data-loss bug rather than fix it. Refusing (no entity created) is
    // recoverable -- create a scene first -- where a create-then-lose is not.
    World w;
    Astra::Entity created = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    CHECK_FALSE(created.IsValid());
}
