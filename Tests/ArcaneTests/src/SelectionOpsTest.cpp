// Edit-menu selection collectors, headless (spec II.A).

#include <catch2/catch_test_macros.hpp>

#include "Scene/SelectionContext.hpp"
#include "Scene/SelectionOps.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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
            // Same cross-DLL TypeContext pin as EntityOpsTest.cpp -- Edit::
            // ops live in Arcane.dll and must agree on component IDs.
            Arcane::Runtime pin(Arcane::Test::Process());
            RegisterSceneComponents(reg);
        }
    };
}

TEST_CASE("CollectSceneEntities: descendants only, root excluded, no-root empty",
          "[editor]")
{
    World w;
    CHECK(Editor::CollectSceneEntities(w.reg).empty());   // no SceneRoot yet

    const Astra::Entity root = Scene::CreateEmpty(w.reg); // root + Main Camera
    const Astra::Entity a = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntityInScene(w.reg, a);

    const std::vector<Astra::Entity> all = Editor::CollectSceneEntities(w.reg);
    CHECK(std::find(all.begin(), all.end(), root) == all.end());
    CHECK(std::find(all.begin(), all.end(), a) != all.end());
    CHECK(std::find(all.begin(), all.end(), b) != all.end());
    CHECK(all.size() == 3);   // camera + a + b
    // BFS pre-order: the parent precedes its child.
    CHECK(std::find(all.begin(), all.end(), a) < std::find(all.begin(), all.end(), b));
}

TEST_CASE("InvertSelectionSet drops selected entries, keeps order", "[editor]")
{
    World w;
    Scene::CreateEmpty(w.reg);
    const Astra::Entity a = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());

    const std::vector<Astra::Entity> all = Editor::CollectSceneEntities(w.reg);
    Editor::SelectionContext sel;
    sel.Select(a);

    const std::vector<Astra::Entity> inv = Editor::InvertSelectionSet(all, sel);
    CHECK(std::find(inv.begin(), inv.end(), a) == inv.end());
    CHECK(std::find(inv.begin(), inv.end(), b) != inv.end());
    CHECK(inv.size() == all.size() - 1);
}

TEST_CASE("SelectionContext: every selection action bumps Epoch, including a re-select; Prune does not", "[editor][selection]")
{
    using Astra::Entity;
    Arcane::Editor::SelectionContext sel;
    const auto e0 = sel.Epoch();
    const Entity a(static_cast<Entity::StorageType>(7)), b(static_cast<Entity::StorageType>(9));
    sel.Select(a);                       CHECK(sel.Epoch() == e0 + 1);
    sel.Select(a);                       CHECK(sel.Epoch() == e0 + 2);   // re-click on the selected entity IS an action
    sel.Toggle(b);                       CHECK(sel.Epoch() == e0 + 3);
    sel.Prune([&](Entity e) { return e != b; });                          // the primary (b) dies: primary falls back to a
    CHECK(sel.Primary() == a);
    CHECK(sel.Epoch() == e0 + 3);                                         // a sweep is not a gesture
    sel.Clear();                         CHECK(sel.Epoch() == e0 + 4);
}

TEST_CASE("SelectionWithoutSceneRoot drops the root and keeps order; a root-only selection yields empty", "[editor][outliner]")
{
    World w;
    const Astra::Entity root = Scene::CreateEmpty(w.reg);
    const Astra::Entity a = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());

    const std::vector<Astra::Entity> mixed{ b, root, a };
    CHECK(Editor::SelectionWithoutSceneRoot(w.reg, mixed) == std::vector<Astra::Entity>{ b, a });
    CHECK_FALSE(Editor::IsSceneRootOnly(w.reg, mixed));

    const std::vector<Astra::Entity> rootOnly{ root };
    CHECK(Editor::SelectionWithoutSceneRoot(w.reg, rootOnly).empty());
    CHECK(Editor::IsSceneRootOnly(w.reg, rootOnly));
    CHECK_FALSE(Editor::IsSceneRootOnly(w.reg, std::vector<Astra::Entity>{}));   // nothing selected is not "root only"

    // No SceneRoot resource: nothing is the root, nothing is dropped.
    World bare;
    const Astra::Entity loose = Edit::CreateEntity(bare.reg, Astra::Entity::Invalid());
    CHECK(Editor::SelectionWithoutSceneRoot(bare.reg, std::vector<Astra::Entity>{ loose })
          == std::vector<Astra::Entity>{ loose });
}

TEST_CASE("SceneRootRefusal names the refused verb", "[editor][outliner]")
{
    CHECK(std::string(Editor::SceneRootRefusal(Editor::SceneRootVerb::Delete))    == "The scene root can't be deleted");
    CHECK(std::string(Editor::SceneRootRefusal(Editor::SceneRootVerb::Cut))       == "The scene root can't be cut");
    CHECK(std::string(Editor::SceneRootRefusal(Editor::SceneRootVerb::Copy))      == "The scene root can't be copied");
    CHECK(std::string(Editor::SceneRootRefusal(Editor::SceneRootVerb::Duplicate)) == "The scene root can't be duplicated");
}

TEST_CASE("a mixed root+child Duplicate copies only the child: no second scene, no second Camera", "[editor][outliner]")
{
    // DuplicateSelection's shape (EditorPanels.cpp): the root filter, then
    // SerializeSubtrees -> InstantiateSubtrees over what is left.
    World w;
    const Astra::Entity root = Scene::CreateEmpty(w.reg);   // root + Main Camera
    const Astra::Entity a = Edit::CreateEntityInScene(w.reg, Astra::Entity::Invalid());
    const auto identities = [&]
    {
        std::size_t n = 0;
        w.reg.CreateView<const Identity>().ForEach([&](Astra::Entity, const Identity&) { ++n; });
        return n;
    };
    const std::size_t before = identities();
    const std::vector<Astra::Entity> filtered =
        Editor::SelectionWithoutSceneRoot(w.reg, std::vector<Astra::Entity>{ root, a });
    REQUIRE(filtered == std::vector<Astra::Entity>{ a });
    const nlohmann::json payload = Edit::SerializeSubtrees(w.reg, filtered);
    const std::vector<Astra::Entity> made = Edit::InstantiateSubtrees(w.reg, payload);
    REQUIRE(made.size() == 1);
    CHECK(w.reg.GetParent(made[0]) == root);   // a sibling of `a` under the ONE root
    CHECK(identities() == before + 1);         // neither the root nor its Camera was copied
}
