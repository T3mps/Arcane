// Arcane Editor entity enumeration + selection. CPU-only ([editor]).

#include <array>
#include <memory>
#include <span>
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

TEST_CASE("SelectionContext: Select replaces, Toggle adds/removes, primary follows", "[editor]")
{
    Arcane::Editor::SelectionContext sel;
    auto reg = MakeSceneRegistry();
    Astra::Entity a = reg->CreateEntity();
    Astra::Entity b = reg->CreateEntity();
    Astra::Entity c = reg->CreateEntity();

    CHECK_FALSE(sel.HasSelection());
    CHECK_FALSE(sel.Primary().IsValid());

    sel.Select(a);
    CHECK(sel.Count() == 1);
    CHECK(sel.Primary() == a);
    CHECK(sel.Contains(a));

    sel.Toggle(b);                       // ctrl-click add
    CHECK(sel.Count() == 2);
    CHECK(sel.Primary() == b);
    CHECK(sel.Contains(a));

    sel.Toggle(b);                       // ctrl-click remove -> primary falls back
    CHECK(sel.Count() == 1);
    CHECK(sel.Primary() == a);

    sel.Select(c);                       // plain click replaces everything
    CHECK(sel.Count() == 1);
    CHECK(sel.Primary() == c);
    CHECK_FALSE(sel.Contains(a));

    sel.Clear();
    CHECK_FALSE(sel.HasSelection());
}

TEST_CASE("SelectionContext: AddRange appends without duplicates; Prune sweeps dead", "[editor]")
{
    Arcane::Editor::SelectionContext sel;
    auto reg = MakeSceneRegistry();
    Astra::Entity a = reg->CreateEntity();
    Astra::Entity b = reg->CreateEntity();
    Astra::Entity c = reg->CreateEntity();

    sel.Select(a);
    const std::array<Astra::Entity, 3> range{ a, b, c };   // shift-range includes the anchor
    sel.AddRange(range, c);
    CHECK(sel.Count() == 3);                               // a not duplicated
    CHECK(sel.Primary() == c);

    reg->DestroyEntity(c);
    sel.Prune([&](Astra::Entity e) { return reg->IsValid(e); });
    CHECK(sel.Count() == 2);
    CHECK(sel.Primary() == b);                             // fell back to most recent live
    CHECK_FALSE(sel.Contains(c));
}
