// Authored-transform ownership + physics sync (SPEC #1). Transform is the
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
        Transform lt; lt.position = pos; lt.scale = scale;
        reg.AddComponent<Transform>(e, lt);
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
    Transform lt; lt.position = {0.0f, 0.0f}; lt.scale = {2.0f, 1.0f};
    reg.AddComponent<Transform>(e, lt);
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

// ===========================================================================
// Task 2: paused pos/rot stateless reconcile (PASS 3.5)
// ===========================================================================

TEST_CASE("paused author move teleports body, zeroes velocity, is not stomped", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mint at (0,0)

    // Author moves the entity while paused (as a gizmo/inspector edit would).
    reg.GetComponent<Transform>(e)->position = glm::vec2(5.0f, 5.0f);
    paused(reg);                                   // reconcile: body <- lt, then write-back

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    const Phys::Vec2 bp = res->world->Position(bh);
    const Phys::Vec2 bv = res->world->Velocity(bh);
    CHECK(static_cast<float>(bp.x) == Approx(5.0f).margin(1e-4f));
    CHECK(static_cast<float>(bp.y) == Approx(5.0f).margin(1e-4f));
    CHECK(static_cast<float>(bv.x) == Approx(0.0f).margin(1e-4f));
    CHECK(static_cast<float>(bv.y) == Approx(0.0f).margin(1e-4f));
    // Not stomped back to (0,0) by the same frame's write-back.
    const glm::vec2 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(5.0f).margin(1e-4f));
    CHECK(lp.y == Approx(5.0f).margin(1e-4f));
}

TEST_CASE("paused author rotate sets body angle, is not stomped", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);

    reg.GetComponent<Transform>(e)->rotation = 1.0f;   // radians
    paused(reg);

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    CHECK(static_cast<float>(res->world->GetAngle(bh)) == Approx(1.0f).margin(1e-4f));
    CHECK(reg.GetComponent<Transform>(e)->rotation == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("author-while-paused then play resumes from authored pose", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->position = glm::vec2(3.0f, 0.0f);
    paused(reg);                                   // reconcile pushes (3,0) into the body

    PhysicsSystem play(kDt, /*stepWorld=*/true);
    play(reg);                                     // stepping resumes from (3,0), no snap-back

    const glm::vec2 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(3.0f).margin(1e-3f));     // kinematic, zero velocity -> stays at 3
    CHECK(lp.y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("play mode ignores author lt edits (body owns pose)", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    PhysicsSystem play(kDt, /*stepWorld=*/true);
    play(reg);                                     // mint + step; body at ~origin

    reg.GetComponent<Transform>(e)->position = glm::vec2(9.0f, 9.0f); // bogus author edit
    play(reg);                                     // Play: PASS 4 overwrites lt from the body

    const glm::vec2 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(0.0f).margin(1e-3f));     // body owns; edit discarded
    CHECK(lp.y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("untouched paused body is not spuriously teleported", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    for (int i = 0; i < 5; ++i) paused(reg);       // no author edits between ticks

    const glm::vec2 lp = reg.GetComponent<Transform>(e)->position;
    CHECK(lp.x == Approx(2.0f).margin(1e-4f));
    CHECK(lp.y == Approx(3.0f).margin(1e-4f));
}

TEST_CASE("paused author move works on a static body", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Static);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->position = glm::vec2(4.0f, 0.0f);
    paused(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::Vec2 bp = res->world->Position(res->entityToBody.at(e));
    CHECK(static_cast<float>(bp.x) == Approx(4.0f).margin(1e-4f));
    // If this FAILS (SetPosition no-ops on Static), the reconcile pass must
    // remove+re-add the static body at the authored pose for Static bodies.
}

// ===========================================================================
// Task 3: paused scale -> collider rebuild
// ===========================================================================

TEST_CASE("paused scale-up rebuilds fixtures at effective size, pose preserved", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);

    reg.GetComponent<Transform>(e)->scale = glm::vec2(2.0f, 2.0f);
    paused(reg);                                   // reconcile: rebuild fixtures

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    CHECK(res->world->FixtureCount(bh) == 1u);     // count preserved across rebuild
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));      // 0.5 * 2
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
    const Phys::Vec2 bp = res->world->Position(bh); // body did not move
    CHECK(static_cast<float>(bp.x) == Approx(2.0f).margin(1e-4f));
    CHECK(static_cast<float>(bp.y) == Approx(3.0f).margin(1e-4f));
    CHECK(reg.GetComponent<PhysicsBodyRef>(e)->appliedScale == glm::vec2(2.0f, 2.0f));
}

TEST_CASE("paused non-uniform scale rebuilds per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->scale = glm::vec2(3.0f, 1.0f);
    paused(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.5f).margin(1e-4f));      // 0.5 * 3
    CHECK(he.y == Approx(0.5f).margin(1e-4f));      // 0.5 * 1
}

TEST_CASE("unchanged scale does not rebuild fixtures (no compounding)", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->scale = glm::vec2(2.0f, 2.0f);
    paused(reg);                                   // rebuild once

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    const Phys::FixtureHandle before = res->world->GetBodyFixture(bh, 0u);
    for (int i = 0; i < 3; ++i) paused(reg);       // scale unchanged -> no rebuild
    const Phys::FixtureHandle after = res->world->GetBodyFixture(bh, 0u);

    // appliedScale suppresses re-rebuild: the fixture identity is unchanged.
    // (FixtureHandle is a {index, generation} slot handle, like BodyHandle.)
    CHECK(before.index == after.index);
    CHECK(before.generation == after.generation);
    // Dims did not compound.
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("mint at scale then reconcile does not double-apply", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {2.0f,2.0f});
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // CREATE bakes scale 2 (he == 1.0)
    paused(reg);                                   // reconcile: scale unchanged -> no rebuild
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));      // NOT 2.0 (no double-scale)
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("body still steps after a scale rebuild", "[transform-sync]")
{
    Astra::Registry reg;
    // Gravity on for this one: prove the rebuilt body is a live dynamic body.
    RegisterSceneComponents(reg);
    RegisterPhysicsComponents(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 10.0f;
    reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });
    Astra::Entity e = reg.CreateEntity();
    Transform lt; lt.position = {0,0};
    reg.AddComponent<Transform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    RigidBody2D rb; rb.type = Phys::BodyType::Dynamic; rb.fixedRotation = true;  // dynamic AABB invariant
    reg.AddComponent<RigidBody2D>(e, rb);
    Collider2D col; Fixture fx; fx.kind = Phys::ShapeKind::Aabb; fx.halfW = 0.5f; fx.halfH = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Collider2D>(e, col);
    reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});

    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<Transform>(e)->scale = glm::vec2(2.0f, 2.0f);
    paused(reg);                                   // rebuild fixtures

    PhysicsSystem play(kDt, /*stepWorld=*/true);
    for (int i = 0; i < 10; ++i) play(reg);        // must fall under gravity
    CHECK(reg.GetComponent<Transform>(e)->position.y > 0.1f);
}

// ---- review-coverage follow-ups -------------------------------------------

TEST_CASE("paused author move zeroes a moving body's velocity", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    // Author a non-zero velocity so the reconcile's zeroing is actually observable
    // (without this, a default (0,0) velocity would pass even if SetVelocity(0) were gone).
    reg.GetComponent<RigidBody2D>(e)->velocity = glm::vec2(3.0f, -2.0f);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mint applies the authored velocity

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    REQUIRE(static_cast<float>(res->world->Velocity(bh).x) == Approx(3.0f).margin(1e-4f));  // present pre-move

    reg.GetComponent<Transform>(e)->position = glm::vec2(5.0f, 0.0f);  // author move
    paused(reg);                                   // reconcile teleports + zeroes velocity

    const Phys::Vec2 bv = res->world->Velocity(bh);
    CHECK(static_cast<float>(bv.x) == Approx(0.0f).margin(1e-4f));   // "don't fling on resume"
    CHECK(static_cast<float>(bv.y) == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("create scales a circle by the representative (max) axis", "[transform-sync]")
{
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    RegisterPhysicsComponents(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });

    Astra::Entity e = reg.CreateEntity();
    Transform lt; lt.position = {0.0f, 0.0f}; lt.scale = {3.0f, 1.0f};  // non-uniform
    reg.AddComponent<Transform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    RigidBody2D rb; rb.type = Phys::BodyType::Kinematic;   // circle: no dynamic-AABB constraint
    reg.AddComponent<RigidBody2D>(e, rb);
    Collider2D col; Fixture fx; fx.kind = Phys::ShapeKind::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Collider2D>(e, col);
    reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});

    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);

    // Circle uses radius * max(|sx|,|sy|) = 0.5 * 3 -> a circle's world AABB is square,
    // so both half-extents are 1.5 (NOT 1.5 x 0.5 -- a circle has no distinguished axis).
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.5f).margin(1e-4f));
    CHECK(he.y == Approx(1.5f).margin(1e-4f));
}
