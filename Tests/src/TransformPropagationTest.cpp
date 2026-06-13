// BFS transform propagation: a child's world translation is parent.world * child.local.
// Reversed insertion order must give the same result. Binary scene save/load
// round-trips transforms + relations.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <memory>

using Catch::Approx;

namespace
{
    Astra::Entity MakeNode(Astra::Registry& reg, glm::vec2 pos)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::LocalTransform lt;
        lt.position = pos;
        reg.AddComponent<Arcane::LocalTransform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        return e;
    }
}

TEST_CASE("transform propagation composes the parent chain in BFS order", "[scene]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root = MakeNode(reg, glm::vec2(100.0f, 0.0f));
    Astra::Entity child = MakeNode(reg, glm::vec2(10.0f, 5.0f));
    Astra::Entity grandchild = MakeNode(reg, glm::vec2(1.0f, 2.0f));
    reg.SetParent(grandchild, child);   // deliberately wire deepest first
    reg.SetParent(child, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    auto worldPos = [&](Astra::Entity e)
    {
        const glm::mat3& m = reg.GetComponent<Arcane::WorldTransform>(e)->matrix;
        return glm::vec2(m[2].x, m[2].y);
    };
    CHECK(worldPos(root).x == Catch::Approx(100.0f));
    CHECK(worldPos(child).x == Catch::Approx(110.0f));
    CHECK(worldPos(child).y == Catch::Approx(5.0f));
    CHECK(worldPos(grandchild).x == Catch::Approx(111.0f));
    CHECK(worldPos(grandchild).y == Catch::Approx(7.0f));
}

TEST_CASE("transform propagation applies parent rotation to child offset", "[scene]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    // Parent at origin rotated +90 degrees; child offset (10,0) in parent space
    // must land at world (0,10).
    Astra::Entity root = reg.CreateEntity();
    Arcane::LocalTransform rootT; rootT.rotation = 1.57079632679f;  // pi/2
    reg.AddComponent<Arcane::LocalTransform>(root, rootT);
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});

    Astra::Entity child = reg.CreateEntity();
    Arcane::LocalTransform childT; childT.position = glm::vec2(10.0f, 0.0f);
    reg.AddComponent<Arcane::LocalTransform>(child, childT);
    reg.AddComponent<Arcane::WorldTransform>(child, Arcane::WorldTransform{});
    reg.SetParent(child, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    const glm::mat3& cm = reg.GetComponent<Arcane::WorldTransform>(child)->matrix;
    CHECK(cm[2].x == Catch::Approx(0.0f).margin(1e-4));
    CHECK(cm[2].y == Catch::Approx(10.0f).margin(1e-4));
}

TEST_CASE("binary scene save/load round-trips transforms and relations", "[scene]")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "arcane_scene_roundtrip.bin";

    Astra::Entity savedChild{};
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Astra::Entity root = MakeNode(reg, glm::vec2(100.0f, 0.0f));
        Astra::Entity child = MakeNode(reg, glm::vec2(10.0f, 5.0f));
        reg.SetParent(child, root);
        savedChild = child;
        auto saved = Arcane::Scene::SaveBinary(reg, path);
        REQUIRE(saved.IsOk());
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);   // ComponentRegistry& overload
    auto loaded = Arcane::Scene::LoadBinary(path, components);
    REQUIRE(loaded.IsOk());
    std::unique_ptr<Astra::Registry> reg = std::move(*loaded.GetValue());

    int spatial = 0;
    glm::vec2 childLocal{0.0f};
    auto view = reg->CreateView<Arcane::LocalTransform>();
    view.ForEach([&](Astra::Entity, Arcane::LocalTransform& lt)
    {
        ++spatial;
        if (std::abs(lt.position.x - 10.0f) < 1e-4f) childLocal = lt.position;
    });
    CHECK(spatial == 2);
    CHECK(childLocal.y == Catch::Approx(5.0f));
    // Relation survived: the saved child still has a parent.
    CHECK(reg->HasParent(savedChild));

    std::filesystem::remove(path);
}
