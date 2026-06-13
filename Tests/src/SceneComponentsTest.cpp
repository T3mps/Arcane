// Scene components are plain reflected data. ToMatrix() builds a 2D TRS matrix;
// reflected components expose a non-null visitFields slot (Astra 3.2 seam).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>

#include <Astra/Component/ComponentRegistry.hpp>

#include <glm/gtc/epsilon.hpp>
#include <cmath>

TEST_CASE("LocalTransform::ToMatrix composes translation/scale", "[scene]")
{
    Arcane::LocalTransform t;
    t.position = glm::vec2(10.0f, 20.0f);
    t.scale = glm::vec2(2.0f, 3.0f);
    t.rotation = 0.0f;

    const glm::mat3 m = t.ToMatrix();
    CHECK(glm::epsilonEqual(m[2].x, 10.0f, 1e-5f));
    CHECK(glm::epsilonEqual(m[2].y, 20.0f, 1e-5f));
    CHECK(glm::epsilonEqual(glm::length(glm::vec2(m[0])), 2.0f, 1e-5f));
    CHECK(glm::epsilonEqual(glm::length(glm::vec2(m[1])), 3.0f, 1e-5f));
}

TEST_CASE("scene components are reflected (visitFields slot populated)", "[scene]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Arcane::LocalTransform>();
    creg.RegisterComponent<Arcane::SpriteRenderer>();

    const auto* lt = creg.GetComponentDescriptor(Astra::TypeID<Arcane::LocalTransform>::Value());
    const auto* sr = creg.GetComponentDescriptor(Astra::TypeID<Arcane::SpriteRenderer>::Value());
    REQUIRE(lt != nullptr);
    REQUIRE(sr != nullptr);
    CHECK(lt->visitFields != nullptr);
    CHECK(sr->visitFields != nullptr);
}
