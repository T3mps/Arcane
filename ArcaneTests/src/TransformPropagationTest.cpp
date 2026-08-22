// BFS transform propagation: a child's world translation is parent.world * child.local.
// Reversed insertion order must give the same result. Binary scene save/load
// round-trips transforms + relations.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <memory>

#include "Helpers/TestTypeContext.hpp"

using Catch::Approx;

namespace
{
    Astra::Entity MakeNode(Astra::Registry& reg, glm::vec2 pos)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::Transform lt;
        lt.position = glm::vec3(pos, 0.0f);
        reg.AddComponent<Arcane::Transform>(e, lt);
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
        const glm::mat4& m = reg.GetComponent<Arcane::WorldTransform>(e)->matrix;
        return glm::vec2(m[3].x, m[3].y);   // Task 3 (F1): mat4 translation column
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
    Arcane::Transform rootT; rootT.rotation = Arcane::RotationAboutZ(1.57079632679f);  // pi/2 about +Z
    reg.AddComponent<Arcane::Transform>(root, rootT);
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});

    Astra::Entity child = reg.CreateEntity();
    Arcane::Transform childT; childT.position = glm::vec3(10.0f, 0.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(child, childT);
    reg.AddComponent<Arcane::WorldTransform>(child, Arcane::WorldTransform{});
    reg.SetParent(child, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    const glm::mat4& cm = reg.GetComponent<Arcane::WorldTransform>(child)->matrix;
    CHECK(cm[3].x == Catch::Approx(0.0f).margin(1e-4));
    CHECK(cm[3].y == Catch::Approx(10.0f).margin(1e-4));
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
    auto view = reg->CreateView<Arcane::Transform>();
    view.ForEach([&](Astra::Entity, Arcane::Transform& lt)
    {
        ++spatial;
        if (std::abs(lt.position.x - 10.0f) < 1e-4f) childLocal = glm::vec2(lt.position);
    });
    CHECK(spatial == 2);
    CHECK(childLocal.y == Catch::Approx(5.0f));
    // Relation survived: the saved child still has a parent.
    CHECK(reg->HasParent(savedChild));

    std::filesystem::remove(path);
}

// Reported bug: create a child in the Outliner, add a SpriteRenderer, pick a
// material -- nothing appears in the viewport. Root cause: Edit::CreateEntity
// (the real Outliner "New Child Entity" path) adds only Transform + Identity,
// never WorldTransform, and the propagation walk below used to only WRITE an
// already-present WorldTransform -- it never materialised the missing one. A
// child created this way therefore could never satisfy RenderSubmissionSystem's
// view no matter what components got added afterward.
TEST_CASE("editor-created child under SceneRoot gets a WorldTransform from propagation",
          "[scene][outliner]")
{
    // Cross-DLL: Edit::CreateEntity is compiled into Arcane.dll (see
    // EntityOpsTest.cpp's World helper) -- without pinning this module and
    // Arcane.dll would resolve component IDs through different TypeContext
    // slots, and Identity/SpriteRenderer added by CreateEntity would be
    // invisible to GetComponent calls made from this module.
    Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root = reg.CreateEntity();
    Arcane::Transform rootT;
    rootT.position = glm::vec3(5.0f, 0.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(root, rootT);
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    // The real Outliner path (EditorPanels.cpp's "New Child Entity" menu item).
    Astra::Entity child = Arcane::Edit::CreateEntity(reg, root);
    REQUIRE(reg.GetComponent<Arcane::WorldTransform>(child) == nullptr);   // the gap

    if (Arcane::Transform* childLocal = reg.GetComponent<Arcane::Transform>(child))
        childLocal->position = glm::vec3(2.0f, 3.0f, 0.0f);
    reg.AddComponent<Arcane::SpriteRenderer>(child, Arcane::SpriteRenderer{});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    Arcane::WorldTransform* childWorld = reg.GetComponent<Arcane::WorldTransform>(child);
    REQUIRE(childWorld != nullptr);
    CHECK(childWorld->matrix[3].x == Catch::Approx(7.0f));   // root(5,0) + local(2,3)
    CHECK(childWorld->matrix[3].y == Catch::Approx(3.0f));

    // "Has the component" and "renders" must not be allowed to drift apart:
    // confirm the child is matched by the exact view RenderSubmissionSystem uses.
    int matched = 0;
    auto view = reg.CreateView<Arcane::WorldTransform, Arcane::SpriteRenderer,
                               Astra::Not<Arcane::Hidden>>();
    view.ForEach([&](Astra::Entity e, Arcane::WorldTransform&, Arcane::SpriteRenderer&)
    {
        if (e == child)
            ++matched;
    });
    CHECK(matched == 1);
}
