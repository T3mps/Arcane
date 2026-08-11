// Edit-menu selection collectors, headless (spec II.A).

#include <catch2/catch_test_macros.hpp>

#include "Scene/SelectionOps.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
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
            // Same cross-DLL TypeContext pin as EntityOpsTest.cpp -- Edit::
            // ops live in Arcane.dll and must agree on component IDs.
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
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
