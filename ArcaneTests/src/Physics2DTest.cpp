// Arcane::Physics2D (input-seam spec 2026-10-02 s5.3): the game-facing physics
// commands as members of the published PhysicsResource, taking the body handle
// from entityToBody. Behaviour must equal the deleted free functions'.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    // An axis-aligned box collider. Collider2D{} has NO fixtures, and
    // PhysicsSystem skips an empty fixture list (no body is minted).
    Arcane::Collider2D BoxCollider(float halfW, float halfH)
    {
        Arcane::Fixture fx;
        fx.kind = Arcane::Phys::ShapeKind::Aabb;
        fx.halfW = halfW;
        fx.halfH = halfH;
        Arcane::Collider2D col;
        col.fixtures.push_back(fx);
        return col;
    }

    // A dynamic unit box resting on a static ground box.
    struct World
    {
        Arcane::Runtime rt{Arcane::Test::Process()};
        Astra::Entity ground, box;
        World()
        {
            auto& reg = rt.Registry();
            Arcane::RegisterSceneComponents(reg);
            ground = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(ground, Arcane::Transform{ .position = {0.0f, -0.5f, 0.0f} });
            Arcane::RigidBody2D sb; sb.type = Arcane::Phys::BodyType::Static;
            reg.AddComponent<Arcane::RigidBody2D>(ground, sb);
            reg.AddComponent<Arcane::Collider2D>(ground, BoxCollider(10.0f, 0.5f));
            box = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(box, Arcane::Transform{ .position = {0.0f, 0.5f, 0.0f} });
            Arcane::RigidBody2D db; db.type = Arcane::Phys::BodyType::Dynamic;
            db.fixedRotation = true;                        // a dynamic Aabb must be (Manifold2D asserts)
            reg.AddComponent<Arcane::RigidBody2D>(box, db);
            reg.AddComponent<Arcane::Collider2D>(box, BoxCollider(0.5f, 0.5f));
        }
        void Step(int n)
        {
            for (int i = 0; i < n; ++i) { rt.EnsurePhysics(); rt.Loop().Advance(1.0 / 60.0); }
        }
        Arcane::Physics2D* Physics() { return rt.Registry().GetResource<Arcane::Physics2D>(); }
        Arcane::RigidBody2D& Body(Astra::Entity e) { return *rt.Registry().GetComponent<Arcane::RigidBody2D>(e); }
    };
}

TEST_CASE("Physics2D is the PhysicsResource", "[physics][physics2d]")
{
    STATIC_REQUIRE(std::is_same_v<Arcane::Physics2D, Arcane::PhysicsResource>);
}

TEST_CASE("Physics2D::Motion before the body exists reads RigidBody2D, bodyReady false", "[physics][physics2d]")
{
    World w;
    w.rt.EnsurePhysics();                                   // world minted, no step yet: no bodies
    REQUIRE(w.Physics() != nullptr);
    Arcane::RigidBody2D& rb = w.Body(w.box);
    rb.velocity = {1.5f, -2.0f};
    const Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, rb);
    CHECK_FALSE(m.bodyReady);
    CHECK(m.velocityX == 1.5f);
    CHECK(m.velocityY == -2.0f);

    w.Physics()->SetVelocity(w.box, rb, 3.0f, 0.0f);       // unminted: authored mint velocity
    CHECK(rb.velocity.x == 3.0f);
}

TEST_CASE("Physics2D::SetVelocity drives the live body; a resting body reads as supported", "[physics][physics2d]")
{
    World w;
    // Landed and still AWAKE: support comes from the last step's contacts. (A
    // body that has gone to sleep has no active contacts, and HasFloorSupport's
    // shape-cast fallback starts inside the solver's slop overlap, so it reads
    // unsupported -- pre-existing behaviour, moved verbatim; see the IN-9 report.)
    w.Step(10);
    REQUIRE(w.Physics() != nullptr);
    const auto it = w.Physics()->entityToBody.find(w.box);
    REQUIRE(it != w.Physics()->entityToBody.end());
    REQUIRE(w.Physics()->world->IsAwake(it->second));
    Arcane::RigidBody2D& rb = w.Body(w.box);
    Arcane::BodyMotion2D m = w.Physics()->Motion(w.box, rb);
    CHECK(m.bodyReady);
    CHECK(m.supported);

    w.Physics()->SetVelocity(w.box, rb, 2.0f, 0.0f);
    m = w.Physics()->Motion(w.box, rb);
    CHECK(m.velocityX == 2.0f);
    CHECK(rb.velocity.x == 2.0f);
}

TEST_CASE("Physics2D ignores non-finite input and non-dynamic bodies", "[physics][physics2d]")
{
    World w;
    w.Step(2);
    REQUIRE(w.Physics() != nullptr);
    Arcane::RigidBody2D& rb = w.Body(w.box);
    const glm::vec2 before = rb.velocity;
    w.Physics()->SetVelocity(w.box, rb, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    w.Physics()->SetVelocity(w.box, rb, 0.0f, std::numeric_limits<float>::infinity());
    CHECK(rb.velocity == before);

    Arcane::RigidBody2D& sb = w.Body(w.ground);
    w.Physics()->SetVelocity(w.ground, sb, 5.0f, 5.0f);
    CHECK(sb.velocity == glm::vec2(0.0f, 0.0f));
    const Arcane::BodyMotion2D gm = w.Physics()->Motion(w.ground, sb);
    CHECK_FALSE(gm.bodyReady);
    CHECK_FALSE(gm.supported);
}
