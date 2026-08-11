// Outliner clipboard slice: SubtreeEntities / SerializeSubtrees /
// InstantiateSubtrees, headless. Round-trips through the same {"version",
// "entities"} shape SceneSerializer's SaveJson/LoadJson use, so most of
// these are shape assertions on the payload plus structural assertions on
// the registry after instantiation. Case 8 (undo) reuses
// RegistryStateCommandTest.cpp's swappable-registry World -- that is the
// one case that needs a real CommandStack.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;

namespace
{
    // Verbatim shape of EntityOpsTest.cpp's World: a live registry, no undo
    // machinery -- SubtreeEntities/SerializeSubtrees/InstantiateSubtrees are
    // pure structural mutators/readers, same as the rest of Edit.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        World()
        {
            // Cross-DLL note (mirrors EntityOpsTest.cpp/PickBufferTest.cpp):
            // EntityOps.cpp is compiled into Arcane.dll, so its
            // reg.AddComponent<T>(...) calls resolve component ids through
            // Arcane.dll's own per-module TypeContext slot, not the one
            // main() installs in this test module. Pin that slot to the
            // shared test context so both modules agree on component ids.
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            RegisterSceneComponents(reg);
        }
    };

    // Count of live entities carrying Identity -- used to prove a refused
    // InstantiateSubtrees created nothing lasting.
    std::size_t IdentityCount(Astra::Registry& reg)
    {
        std::size_t n = 0;
        reg.CreateView<Identity>().ForEach([&](Astra::Entity, Identity&) { ++n; });
        return n;
    }

    // Verbatim shape of RegistryStateCommandTest.cpp's World: a registry in
    // a swappable slot (restore REPLACES the object) plus a CommandStack
    // resolving to it, for case 8's real undo round-trip.
    struct UndoWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };

        UndoWorld()
        {
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
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

TEST_CASE("SerializeSubtrees + InstantiateSubtrees round-trip a named root",
          "[outliner][json]")
{
    World w;

    // root (SceneRoot) -> a("Foo") -> b ; root -> c (sibling of a)
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.SetResource<SceneRoot>(SceneRoot{ root });
    Astra::Entity a = Edit::CreateEntity(w.reg, root);
    Edit::RenameEntity(w.reg, a, "Foo");
    Astra::Entity b = Edit::CreateEntity(w.reg, a);
    Astra::Entity c = Edit::CreateEntity(w.reg, root);
    (void)c;

    const Guid aGuid    = w.reg.GetComponent<Identity>(a)->id;
    const Guid rootGuid = w.reg.GetComponent<Identity>(root)->id;

    const std::array<Astra::Entity, 1> selection{ a };
    nlohmann::json payload = Edit::SerializeSubtrees(w.reg, selection);

    REQUIRE(payload["entities"].size() == 2);
    CHECK(payload["entities"][0]["parent"] == -1);
    REQUIRE(payload["entities"][0].contains("rootParentGuid"));
    CHECK(payload["entities"][0]["rootParentGuid"] == rootGuid.ToString());
    CHECK(payload["entities"][1]["parent"] == 0);

    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(w.reg, payload);
    REQUIRE(roots.size() == 1);
    Astra::Entity newA = roots[0];

    CHECK(w.reg.GetParent(newA) == root);   // rootParentGuid matched root

    Identity* newInfo = w.reg.GetComponent<Identity>(newA);
    REQUIRE(newInfo != nullptr);
    CHECK(newInfo->id != aGuid);            // fresh guid
    CHECK(newInfo->name == "Foo_2");        // uniquified against the live registry

    const std::vector<Astra::Entity> newChildren = w.reg.GetChildren(newA);
    REQUIRE(newChildren.size() == 1);
    CHECK(w.reg.GetParent(newChildren[0]) == newA);
    (void)b;
}

TEST_CASE("SerializeSubtrees collapses a nested selection to one copy of the subtree",
          "[outliner][json]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.SetResource<SceneRoot>(SceneRoot{ root });
    Astra::Entity a = Edit::CreateEntity(w.reg, root);
    Astra::Entity b = Edit::CreateEntity(w.reg, a);

    const std::array<Astra::Entity, 2> selection{ a, b };   // b is a's child
    nlohmann::json payload = Edit::SerializeSubtrees(w.reg, selection);

    CHECK(payload["entities"].size() == 2);   // a's subtree emitted exactly once
}

TEST_CASE("InstantiateSubtrees falls back to SceneRoot when rootParentGuid has no live target",
          "[outliner][json]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.SetResource<SceneRoot>(SceneRoot{ root });
    Astra::Entity a = Edit::CreateEntity(w.reg, root);

    const std::array<Astra::Entity, 1> selection{ a };
    nlohmann::json payload = Edit::SerializeSubtrees(w.reg, selection);
    REQUIRE(payload["entities"][0].contains("rootParentGuid"));
    payload["entities"][0].erase("rootParentGuid");   // no target -> must fall back

    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(w.reg, payload);
    REQUIRE(roots.size() == 1);
    CHECK(w.reg.GetParent(roots[0]) == root);   // SceneRoot, not orphaned
}

TEST_CASE("InstantiateSubtrees pastes cross-registry under the destination's SceneRoot, uniquified there",
          "[outliner][json]")
{
    World src;
    Astra::Entity srcRoot = Edit::CreateEntity(src.reg, Astra::Entity::Invalid());
    src.reg.SetResource<SceneRoot>(SceneRoot{ srcRoot });
    Astra::Entity a = Edit::CreateEntity(src.reg, srcRoot);
    Edit::RenameEntity(src.reg, a, "Foo");

    const std::array<Astra::Entity, 1> selection{ a };
    nlohmann::json payload = Edit::SerializeSubtrees(src.reg, selection);

    World dst;
    Astra::Entity dstRoot = Scene::CreateEmpty(dst.reg);   // mints root "Scene" + "Main Camera"
    Astra::Entity existingFoo = Edit::CreateEntity(dst.reg, dstRoot);
    Edit::RenameEntity(dst.reg, existingFoo, "Foo");   // pre-taken in the DESTINATION only

    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(dst.reg, payload);
    REQUIRE(roots.size() == 1);
    CHECK(dst.reg.GetParent(roots[0]) == dstRoot);   // src's rootParentGuid can't match here

    Identity* info = dst.reg.GetComponent<Identity>(roots[0]);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Foo_2");   // uniquified against the DESTINATION's taken names
}

TEST_CASE("InstantiateSubtrees refuses when the registry has no SceneRoot",
          "[outliner][json]")
{
    World w;   // no Scene::CreateEmpty, no SetResource<SceneRoot>
    const std::size_t before = IdentityCount(w.reg);

    nlohmann::json payload;
    payload["version"] = Scene::kSceneJsonVersion;
    payload["entities"] = nlohmann::json::array();
    nlohmann::json entry;
    entry["components"] = nlohmann::json::object();
    entry["parent"] = -1;
    payload["entities"].push_back(entry);

    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(w.reg, payload);
    CHECK(roots.empty());
    CHECK(IdentityCount(w.reg) == before);   // nothing created
}

TEST_CASE("InstantiateSubtrees refuses on a schema version mismatch",
          "[outliner][json]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.SetResource<SceneRoot>(SceneRoot{ root });
    Astra::Entity a = Edit::CreateEntity(w.reg, root);

    const std::array<Astra::Entity, 1> selection{ a };
    nlohmann::json payload = Edit::SerializeSubtrees(w.reg, selection);
    payload["version"] = 999;   // doctor the version

    const std::size_t before = IdentityCount(w.reg);
    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(w.reg, payload);
    CHECK(roots.empty());
    CHECK(IdentityCount(w.reg) == before);
}

TEST_CASE("InstantiateSubtrees rolls back everything created so far on a malformed mid-walk entry",
          "[outliner][json]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.SetResource<SceneRoot>(SceneRoot{ root });
    Astra::Entity a = Edit::CreateEntity(w.reg, root);
    Astra::Entity b = Edit::CreateEntity(w.reg, a);
    (void)b;

    const std::array<Astra::Entity, 1> selection{ a };
    nlohmann::json payload = Edit::SerializeSubtrees(w.reg, selection);
    REQUIRE(payload["entities"].size() == 2);
    payload["entities"][1] = 42;   // corrupt the SECOND entry -- entry 0 already
                                    // created an entity by the time this is hit

    const std::size_t before = IdentityCount(w.reg);
    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(w.reg, payload);
    CHECK(roots.empty());
    CHECK(IdentityCount(w.reg) == before);   // entry 0's entity was rolled back too
}

TEST_CASE("SerializeSubtrees/InstantiateSubtrees keep an internal link, drop an external one",
          "[outliner][json]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.SetResource<SceneRoot>(SceneRoot{ root });
    Astra::Entity a = Edit::CreateEntity(w.reg, root);
    Astra::Entity b = Edit::CreateEntity(w.reg, root);
    Astra::Entity c = Edit::CreateEntity(w.reg, root);
    w.reg.AddLink(a, b);
    w.reg.AddLink(a, c);

    const std::array<Astra::Entity, 2> selection{ a, b };   // c stays outside the copy
    nlohmann::json payload = Edit::SerializeSubtrees(w.reg, selection);
    REQUIRE(payload["entities"].size() == 2);

    // Exactly one internal link entry (the a-b edge): the forward-edge rule
    // keeps it on whichever side has the smaller payload index, and the
    // external a-c edge is dropped because indexOf(c) == -1 never satisfies
    // "j > self".
    std::size_t linkEntries = 0, linkTotal = 0;
    for (const auto& entry : payload["entities"])
        if (entry.contains("links"))
        {
            ++linkEntries;
            linkTotal += entry["links"].size();
        }
    CHECK(linkEntries == 1);
    CHECK(linkTotal == 1);

    const std::vector<Astra::Entity> roots = Edit::InstantiateSubtrees(w.reg, payload);
    REQUIRE(roots.size() == 2);   // a and b are independent selection roots (siblings)

    CHECK(w.reg.GetRelationshipGraph().AreLinked(roots[0], roots[1]));   // new a'-b' link
    CHECK_FALSE(w.reg.GetRelationshipGraph().AreLinked(roots[0], c));    // nothing links to c
    CHECK_FALSE(w.reg.GetRelationshipGraph().AreLinked(roots[1], c));
}

TEST_CASE("SubtreeEntities: a root's whole subtree, duplicates collapse, dead roots skip",
          "[outliner]")
{
    World w;
    Astra::Entity root = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity a = Edit::CreateEntity(w.reg, root);
    Astra::Entity b = Edit::CreateEntity(w.reg, a);

    const std::array<Astra::Entity, 1> once{ a };
    std::vector<Astra::Entity> out = Edit::SubtreeEntities(w.reg, once);
    REQUIRE(out.size() == 2);
    CHECK(((out[0] == a && out[1] == b) || (out[0] == b && out[1] == a)));

    const std::array<Astra::Entity, 2> duplicated{ a, a };
    CHECK(Edit::SubtreeEntities(w.reg, duplicated).size() == 2);   // no double-count

    Astra::Entity dead = Edit::CreateEntity(w.reg, root);
    w.reg.DestroyEntity(dead);
    const std::array<Astra::Entity, 2> withDead{ a, dead };
    CHECK(Edit::SubtreeEntities(w.reg, withDead).size() == 2);   // dead root skipped
}

TEST_CASE("Cut hands DeleteEntities the whole subtree; undo restores it intact",
          "[outliner]")
{
    UndoWorld w;
    Astra::Entity root = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    Astra::Entity a = Edit::CreateEntity(*w.reg, root);
    Astra::Entity b = Edit::CreateEntity(*w.reg, a);
    const std::array<Astra::Entity, 1> roots{ a };

    REQUIRE(ApplyRegistryMutation(w.stack, "Cut", w.Snapshot(), w.Restore(),
        [&]
        {
            return Edit::DeleteEntities(*w.reg, Edit::SubtreeEntities(*w.reg, roots)) > 0;
        }));
    CHECK(w.reg->GetComponent<Identity>(a) == nullptr);
    CHECK(w.reg->GetComponent<Identity>(b) == nullptr);

    w.stack.Undo();
    // The splice hazard this pins: had Cut deleted only {a} (the bare root),
    // DeleteEntities would have spliced b up to root as a SURVIVOR instead of
    // deleting it, so b would never make it into the clipboard/undo payload
    // at all. Deleting SubtreeEntities(roots) instead takes b down WITH a, so
    // undo brings both back with b still a's child, not root's.
    Identity* ai = w.reg->GetComponent<Identity>(a);
    Identity* bi = w.reg->GetComponent<Identity>(b);
    REQUIRE(ai != nullptr);
    REQUIRE(bi != nullptr);
    CHECK(w.reg->GetParent(a) == root);
    CHECK(w.reg->GetParent(b) == a);
}
