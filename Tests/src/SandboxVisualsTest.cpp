// [sandbox] CPU-only: the Sandbox authors a body's visuals from its COLLIDER
// FIXTURES, so a compound (multi-fixture) body renders as one shape-matching
// sprite per fixture instead of a single mismatched rectangle. Generic: adding a
// fixture adds its sprite automatically (AttachFixtureVisuals in Scenes.cpp).
//
// Builds the "Compound bodies" scene (no graphics device needed -- path-A
// builders only author Astra entities) and asserts the lopsided bodies are drawn
// as per-fixture DISCS (each lopsided body = a core circle + a heavy circle).

#include <catch2/catch_test_macros.hpp>

#include "../../Sandbox/src/Scenes.hpp"

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace
{
    namespace Sbx = Arcane::Sandbox;

    // Find the scene index by name in the roster (so the test is robust to roster
    // reordering).
    int SceneIndexByName(const char* name)
    {
        const std::span<const Sbx::SceneDef> scenes = Sbx::SceneRegistry();
        for (std::size_t i = 0; i < scenes.size(); ++i)
            if (std::string_view(scenes[i].name) == name)
                return static_cast<int>(i);
        return -1;
    }
}

TEST_CASE("Sandbox: compound bodies render as per-fixture circle sprites", "[sandbox]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    const int idx = SceneIndexByName("Compound bodies");
    REQUIRE(idx >= 0);
    Sbx::SceneRegistry()[idx].build(reg);

    // The scene has TWO lopsided bodies, each a core circle + a heavy circle, so
    // there must be >= 4 circle-shaped sprites (the per-fixture discs). The old
    // single 124x44 rectangle would yield ZERO circle sprites.
    int circleSprites = 0;
    int rectSprites   = 0;
    auto view = reg.CreateView<Arcane::SpriteRenderer>();
    view.ForEach([&](Astra::Entity, Arcane::SpriteRenderer& sp)
    {
        if (sp.shape == Arcane::SpriteShape::Circle) ++circleSprites;
        else if (sp.shape == Arcane::SpriteShape::Rect) ++rectSprites;
    });

    CHECK(circleSprites >= 4);   // 2 bodies x (core + heavy) circle fixtures
    CHECK(rectSprites   >= 1);   // the floor is still a rect
}
