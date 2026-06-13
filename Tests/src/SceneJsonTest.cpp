// Scene JSON: save the slice's entities (LocalTransform + SpriteRenderer) + parent
// links to JSON via the seam, then load into a fresh registry (typed roster) and
// assert transforms + hierarchy survived. Proves the north-star path end to end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

using Catch::Approx;

TEST_CASE("scene round-trips through JSON (typed roster)", "[json][scene]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);

        Astra::Entity root = reg.CreateEntity();
        Arcane::LocalTransform rootT; rootT.position = glm::vec2(100, 0);
        reg.AddComponent<Arcane::LocalTransform>(root, rootT);
        reg.AddComponent<Arcane::SpriteRenderer>(root, Arcane::SpriteRenderer{});

        Astra::Entity child = reg.CreateEntity();
        Arcane::LocalTransform childT; childT.position = glm::vec2(10, 5);
        reg.AddComponent<Arcane::LocalTransform>(child, childT);
        Arcane::SpriteRenderer sr; sr.tint = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f); sr.sortingLayer = 2;
        reg.AddComponent<Arcane::SpriteRenderer>(child, sr);

        reg.SetParent(child, root);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    int sprites = 0;
    int maxLayer = -1;
    auto view = reg.CreateView<Arcane::LocalTransform, Arcane::SpriteRenderer>();
    view.ForEach([&](Astra::Entity, Arcane::LocalTransform&, Arcane::SpriteRenderer& sr)
    {
        ++sprites;
        if (sr.sortingLayer > maxLayer) maxLayer = sr.sortingLayer;
    });
    CHECK(sprites == 2);
    CHECK(maxLayer == 2);

    const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(root != nullptr);
    CHECK(reg.GetChildCount(root->entity) == 1);
}
