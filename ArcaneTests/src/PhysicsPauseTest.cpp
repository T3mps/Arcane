// Pausing must SKIP the solve, not run Step(0): a no-step Arcane::Physics2D::System mints +
// writes back but generates zero contacts (the expensive narrowphase/solve is
// skipped). Guards the interactive "pause to inspect" path + the perf claim.
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Astra/Registry/Registry.hpp>

using namespace Arcane;

namespace
{
    // Spawn one dynamic box resting on a wider static box so a real step would
    // generate >=1 contact constraint.
    void BuildOverlap(Astra::Registry& reg)
    {
        RegisterSceneComponents(reg);
        Arcane::Physics2D::RegisterComponents(reg);

        Manifold2D::Physics::WorldDef wd;
        reg.SetResource(Arcane::Physics2D::Detail::Adopt(std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
        auto add = [&](glm::vec2 pos, glm::vec2 half, Arcane::Physics2D::BodyType t) {
            Astra::Entity e = reg.CreateEntity();
            Transform lt; lt.position = glm::vec3(pos, 0.0f);
            reg.AddComponent<Transform>(e, lt);
            reg.AddComponent<WorldTransform>(e, WorldTransform{});
            Arcane::Physics2D::RigidBody rb; rb.type = t; rb.fixedRotation = true;
            reg.AddComponent<Arcane::Physics2D::RigidBody>(e, rb);
            Arcane::Physics2D::Collider col; Arcane::Physics2D::Fixture fx;
            fx.kind = Arcane::Physics2D::ShapeKind::Aabb; fx.halfW = half.x; fx.halfH = half.y;
            col.fixtures.push_back(fx);
            reg.AddComponent<Arcane::Physics2D::Collider>(e, col);
            reg.AddComponent<Arcane::Physics2D::BodyRef>(e, Arcane::Physics2D::BodyRef{});
        };
        add({0.0f, 10.0f}, {20.0f, 2.0f}, Arcane::Physics2D::BodyType::Static);
        add({0.0f, 7.9f}, { 2.0f, 2.0f}, Arcane::Physics2D::BodyType::Dynamic); // resting/overlapping
    }
}

TEST_CASE("Arcane::Physics2D::System no-step skips contact generation", "[physics][pause]")
{
    Astra::Registry reg;
    BuildOverlap(reg);

    // No-step: mint + write-back, but DO NOT solve -> zero contacts generated.
    Arcane::Physics2D::System noStep(1.0f / 60.0f, /*stepWorld=*/false);
    noStep(reg);
    auto* res = reg.GetResource<Arcane::Physics2D::World>();
    REQUIRE(Arcane::Physics2D::Detail::Access::Solver(*res)->ActiveContactCount() == 0);
    // The CREATE/SYNC pass must still have minted both bodies even while paused.
    REQUIRE(Arcane::Physics2D::Detail::Access::Entities(*res).size() == 2);

    // A real step on the same overlap DOES generate at least one contact.
    Arcane::Physics2D::System real(1.0f / 60.0f, /*stepWorld=*/true);
    real(reg);
    CHECK(Arcane::Physics2D::Detail::Access::Solver(*res)->ActiveContactCount() >= 1);
}
