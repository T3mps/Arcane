// Grimoire entity enumeration + selection. CPU-only ([grimoire]).

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Scene/SceneModule.hpp>

#include <EntityList.hpp>
#include <SelectionContext.hpp>

namespace
{
    // Fresh registry with the shared Scene components registered, mirroring the
    // fixture in RenderInterpolationTest.cpp. Returned as a unique_ptr: Astra::Registry
    // declares a user-provided copy constructor (which suppresses the implicit move
    // constructor), so it is not a cheap-to-return-by-value type here.
    std::unique_ptr<Astra::Registry> MakeSceneRegistry()
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }
}

TEST_CASE("CollectEntities returns every live entity", "[grimoire]")
{
    auto reg = MakeSceneRegistry();          // fixture: fresh registry, Scene comps registered
    const Astra::Entity a = reg->CreateEntity();
    const Astra::Entity b = reg->CreateEntity();
    const Astra::Entity c = reg->CreateEntity();

    std::vector<Astra::Entity> all = Grimoire::CollectEntities(*reg);
    REQUIRE(all.size() == 3);
    // ids present regardless of order
    bool ha=false, hb=false, hc=false;
    for (Astra::Entity e : all) { ha |= (e.GetID()==a.GetID()); hb |= (e.GetID()==b.GetID()); hc |= (e.GetID()==c.GetID()); }
    CHECK((ha && hb && hc));
}

TEST_CASE("SelectionContext tracks and clears selection", "[grimoire]")
{
    Grimoire::SelectionContext sel;
    CHECK_FALSE(sel.HasSelection());
    Grimoire::SelectionContext::EntityT e(42u, 1u);   // see note in SelectionContext.hpp
    sel.Select(e);
    CHECK(sel.HasSelection());
    CHECK(sel.selected.GetID() == 42u);
    sel.Clear();
    CHECK_FALSE(sel.HasSelection());
}
