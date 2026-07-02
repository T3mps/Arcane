// Box2D v3 b2IntegrateVelocitiesTask parity: per-body linear speed clamped to
// WorldDef.maxLinearVelocity; angular speed clamped to kMaxRotation * invDt.
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

using namespace Arcane::Physics;

TEST_CASE("Linear velocity is clamped to maxLinearVelocity", "[physics][clamp]")
{
    WorldDef def; // default maxLinearVelocity == 400
    PhysicsWorld w(def);
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
    d.fixedRotation = true; d.position = Vec2(0, 0);
    BodyHandle h = w.AddBody(d);
    w.SetVelocity(h, Vec2(Real(2000), Real(0)));      // 5x over the cap
    w.Step(Real(1) / Real(60));
    const Vec2 v = w.Velocity(h);
    const Real speed = std::sqrt(v.x * v.x + v.y * v.y);
    REQUIRE(speed <= Real(400) + Real(0.5));           // clamped to the cap
    REQUIRE(speed >= Real(400) - Real(0.5));           // exactly the cap (was 2000)
}

TEST_CASE("Sub-cap linear velocity is untouched", "[physics][clamp]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
    d.fixedRotation = true; d.position = Vec2(0, 0);
    BodyHandle h = w.AddBody(d);
    w.SetVelocity(h, Vec2(Real(100), Real(0)));        // well under 400
    w.Step(Real(1) / Real(60));
    const Vec2 v = w.Velocity(h);
    REQUIRE(std::abs(v.x - Real(100)) < Real(0.001));  // unchanged (no gravity/damping)
    REQUIRE(std::abs(v.y) < Real(0.001));
}

TEST_CASE("Angular velocity is clamped to kMaxRotation * invDt", "[physics][clamp]")
{
    PhysicsWorld w;
    // Rotatable dynamic body: a circle carries rotational inertia and is NOT
    // fixedRotation by default, so its angular velocity integrates (and is
    // clamped). A dynamic AABB is forbidden here -- the engine asserts dynamic
    // axis-aligned boxes must be fixedRotation (PhysicsWorld.cpp:979), which
    // would zero invInertia and make the spin untestable.
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
    d.position = Vec2(0, 0); // rotation allowed (not fixedRotation)
    BodyHandle h = w.AddBody(d);
    w.SetAngularVelocity(h, Real(1000));               // huge spin
    w.Step(Real(1) / Real(60));
    const Real maxAng = kMaxRotation * Real(60);       // invDt = 1/(1/60) = 60
    REQUIRE(std::abs(w.AngularVelocity(h)) <= maxAng + Real(0.01));
    REQUIRE(std::abs(w.AngularVelocity(h)) >= maxAng - Real(0.01));
}
