// Scene components are plain reflected data. ToMatrix() builds a 2D TRS matrix;
// reflected components expose a non-null visitFields slot (Astra 3.2 seam).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <glm/gtc/epsilon.hpp>
#include <cmath>
#include <memory>

TEST_CASE("Transform::ToMatrix composes translation/scale", "[scene]")
{
    Arcane::Transform t;
    t.position = glm::vec3(10.0f, 20.0f, 0.0f);
    t.scale = glm::vec3(2.0f, 3.0f, 1.0f);
    t.rotation = Arcane::RotationAboutZ(0.0f);

    // Task 3 (F1): mat4 -- the translation column moved from 2 to 3.
    const glm::mat4 m = t.ToMatrix();
    CHECK(glm::epsilonEqual(m[3].x, 10.0f, 1e-5f));
    CHECK(glm::epsilonEqual(m[3].y, 20.0f, 1e-5f));
    CHECK(glm::epsilonEqual(glm::length(glm::vec2(m[0])), 2.0f, 1e-5f));
    CHECK(glm::epsilonEqual(glm::length(glm::vec2(m[1])), 3.0f, 1e-5f));
}

TEST_CASE("scene components are reflected (visitFields slot populated)", "[scene]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Arcane::Transform>();
    creg.RegisterComponent<Arcane::SpriteRenderer>();

    const auto* lt = creg.GetComponentDescriptor(Astra::TypeID<Arcane::Transform>::Value());
    const auto* sr = creg.GetComponentDescriptor(Astra::TypeID<Arcane::SpriteRenderer>::Value());
    REQUIRE(lt != nullptr);
    REQUIRE(sr != nullptr);
    CHECK(lt->visitFields != nullptr);
    CHECK(sr->visitFields != nullptr);
}

TEST_CASE("PhysicsSettings is a reflected, roster-registered scene component", "[scene][physics]")
{
    // Spec 2026-09-11-physics-2d-wiring s5: the per-scene gravity override
    // rides on the scene-root entity as an ordinary component, so the
    // Inspector, JSON, undo and the catalog all get it for free.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::PhysicsSettings>();
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 1);
    CHECK(meta->fields[0].name == "gravity");
    CHECK(meta->fields[0].IsSerializable());
    REQUIRE(components->GetComponentDescriptor(Astra::TypeID<Arcane::PhysicsSettings>::Value()) != nullptr);
    Arcane::PhysicsSettings def;
    CHECK(def.gravity.x == 0.0f);
    CHECK(def.gravity.y == Catch::Approx(9.81f));   // +Y is down
}
