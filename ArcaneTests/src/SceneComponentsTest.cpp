// Scene components are plain reflected data. ToMatrix() builds a 2D TRS matrix;
// reflected components expose a non-null visitFields slot (Astra 3.2 seam).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>

#include <Astra/Component/ComponentRegistry.hpp>

#include <glm/gtc/epsilon.hpp>
#include <cmath>

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
