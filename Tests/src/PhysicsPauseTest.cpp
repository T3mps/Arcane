// Pausing must SKIP the solve, not run Step(0): a no-step PhysicsSystem mints +
// writes back but generates zero contacts (the expensive narrowphase/solve is
// skipped). Guards the interactive "pause to inspect" path + the perf claim.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsWorld.hpp>
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
        RegisterPhysicsComponents(reg);

        Physics::WorldDef wd;
        reg.SetResource(PhysicsResource{
            std::make_unique<Physics::PhysicsWorld>(wd), {} });
        auto add = [&](glm::vec2 pos, glm::vec2 half, Physics::BodyType t) {
            Astra::Entity e = reg.CreateEntity();
            LocalTransform lt; lt.position = pos;
            reg.AddComponent<LocalTransform>(e, lt);
            reg.AddComponent<WorldTransform>(e, WorldTransform{});
            RigidBody2D rb; rb.type = t; rb.fixedRotation = true;
            reg.AddComponent<RigidBody2D>(e, rb);
            Collider2D col; Fixture fx;
            fx.kind = Physics::ShapeKind::Aabb; fx.halfW = half.x; fx.halfH = half.y;
            col.fixtures.push_back(fx);
            reg.AddComponent<Collider2D>(e, col);
            reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});
        };
        add({0.0f, 10.0f}, {20.0f, 2.0f}, Physics::BodyType::Static);
        add({0.0f, 7.9f}, { 2.0f, 2.0f}, Physics::BodyType::Dynamic); // resting/overlapping
    }
}

TEST_CASE("PhysicsSystem no-step skips contact generation", "[physics][pause]")
{
    Astra::Registry reg;
    BuildOverlap(reg);

    // No-step: mint + write-back, but DO NOT solve -> zero contacts generated.
    PhysicsSystem noStep(1.0f / 60.0f, /*stepWorld=*/false);
    noStep(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    REQUIRE(res->world->ActiveContactCount() == 0);
    // The CREATE/SYNC pass must still have minted both bodies even while paused.
    REQUIRE(res->entityToBody.size() == 2);

    // A real step on the same overlap DOES generate at least one contact.
    PhysicsSystem real(1.0f / 60.0f, /*stepWorld=*/true);
    real(reg);
    CHECK(res->world->ActiveContactCount() >= 1);
}
