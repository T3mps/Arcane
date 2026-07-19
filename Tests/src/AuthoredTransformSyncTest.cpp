// Authored-transform ownership + physics sync (SPEC #1). LocalTransform is the
// authored source of truth for a physics entity; this file exercises the engine
// PhysicsSystem sync. Task 1: birth-time scale + appliedScale baseline.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <vector>

using namespace Arcane;
using Catch::Approx;
namespace Phys = Manifold2D::Physics;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    // Build reg + zero-gravity world + one Aabb-fixture body. Gravity is zeroed so
    // a Dynamic body stays put across paused/played steps (position assertions stay
    // deterministic). Returns the body entity.
    Astra::Entity BuildAabbBody(Astra::Registry& reg, glm::vec2 pos, glm::vec2 half,
                                Phys::BodyType type, glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        RegisterSceneComponents(reg);
        RegisterPhysicsComponents(reg);
        Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
        reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });

        Astra::Entity e = reg.CreateEntity();
        LocalTransform lt; lt.position = pos; lt.scale = scale;
        reg.AddComponent<LocalTransform>(e, lt);
        reg.AddComponent<WorldTransform>(e, WorldTransform{});
        RigidBody2D rb; rb.type = type;
        // Manifold2D invariant: a dynamic AABB body must be fixedRotation (an
        // axis-aligned box has no meaningful orientation under the solver).
        // SetAngle still applies as an explicit teleport, so the reconcile rotate
        // path is exercised regardless.
        if (type == Phys::BodyType::Dynamic) rb.fixedRotation = true;
        reg.AddComponent<RigidBody2D>(e, rb);
        Collider2D col; Fixture fx;
        fx.kind = Phys::ShapeKind::Aabb; fx.halfW = half.x; fx.halfH = half.y;
        col.fixtures.push_back(fx);
        reg.AddComponent<Collider2D>(e, col);
        reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});
        return e;
    }

    // World-AABB half-extents of fixture[0] (single-fixture body at angle 0 -> the
    // world AABB equals the fixture box, so half-extents == the scaled half-dims).
    glm::vec2 Fixture0HalfExtents(Astra::Registry& reg)
    {
        auto* res = reg.GetResource<PhysicsResource>();
        std::vector<std::uint32_t> fx;
        std::vector<Phys::Aabb2>   boxes;
        res->world->LiveFixtureAabbs(fx, boxes);
        REQUIRE(!boxes.empty());
        const Phys::Aabb2& b = boxes[0];
        return glm::vec2((b.max.x - b.min.x) * 0.5f, (b.max.y - b.min.y) * 0.5f);
    }
}

TEST_CASE("create applies uniform scale to collider", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {2.0f,2.0f});

    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);  // CREATE pass mints the body with scaled fixtures

    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));   // 0.5 * 2
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
    CHECK(reg.GetComponent<PhysicsBodyRef>(e)->appliedScale == glm::vec2(2.0f, 2.0f));
}

TEST_CASE("create leaves scale-1 collider unchanged", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {1.0f,1.0f});
    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(0.5f).margin(1e-4f));
    CHECK(he.y == Approx(0.5f).margin(1e-4f));
}

TEST_CASE("create applies non-uniform scale per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {2.0f,1.0f});
    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));   // 0.5 * 2
    CHECK(he.y == Approx(0.5f).margin(1e-4f));   // 0.5 * 1
}

TEST_CASE("create scales multi-fixture local offsets per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    RegisterPhysicsComponents(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });

    Astra::Entity e = reg.CreateEntity();
    LocalTransform lt; lt.position = {0.0f, 0.0f}; lt.scale = {2.0f, 1.0f};
    reg.AddComponent<LocalTransform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    RigidBody2D rb; rb.type = Phys::BodyType::Kinematic;
    reg.AddComponent<RigidBody2D>(e, rb);
    Collider2D col;
    { Fixture f; f.kind = Phys::ShapeKind::Aabb; f.halfW = 0.3f; f.halfH = 0.3f; col.fixtures.push_back(f); }
    { Fixture f; f.kind = Phys::ShapeKind::Aabb; f.halfW = 0.3f; f.halfH = 0.3f; f.localPos = {2.0f, 0.0f}; col.fixtures.push_back(f); }
    reg.AddComponent<Collider2D>(e, col);
    reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});

    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    REQUIRE(res->world->FixtureCount(bh) == 2u);
    // Body at origin, angle 0: fixture[1] world pos = scaled localPos = (2*2, 0*1) = (4,0).
    const Phys::FixtureHandle fh1 = res->world->GetBodyFixture(bh, 1u);
    const Phys::Vec2 wp = res->world->GetFixtureWorldPos(fh1);
    CHECK(static_cast<float>(wp.x) == Approx(4.0f).margin(1e-4f));
    CHECK(static_cast<float>(wp.y) == Approx(0.0f).margin(1e-4f));
}
