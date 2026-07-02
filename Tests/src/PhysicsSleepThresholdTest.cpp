// Sleep-threshold + maxExtent + combined-predicate tests (Box2D-faithful sleep).
// Companion to PhysicsAwakeSetTest.cpp / PhysicsIslandTest.cpp. The headline
// guard is "a slowly-rolling body sleeps": the never-settle bug was 1-2 circles
// rolling just over the old separate angular gate (0.05 rad/s) vetoing a whole
// island forever. The combined Box2D test ( |v| + |w|*maxExtent < sleepThreshold )
// counts a slow roll as small surface motion, so the pile sleeps.
// PRESENTATION-FREE + C++23-clean.
#include <cmath>
#include <cstdint>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
using namespace Arcane::Physics;
namespace { constexpr Real kStep = Real(1) / Real(60); }

TEST_CASE("PhysicsSleep: maxExtent equals circle radius for a centered circle", "[physics][sleep]")
{
    WorldDef wd;
    wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.gravityY               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
    wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
    wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
    wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
    PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1);
    const BodyHandle b = w.AddBody(d);
    CHECK_THAT(static_cast<double>(w.MaxExtentSlot(w.SlotOf(b))),
               Catch::Matchers::WithinAbs(10.0, 1e-4));
}

TEST_CASE("PhysicsSleep: maxExtent equals box half-diagonal for an AABB", "[physics][sleep]")
{
    WorldDef wd;
    wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.gravityY               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
    wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
    wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
    wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
    PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeAabb(Real(3), Real(4)); d.density = Real(1); d.fixedRotation = true;
    const BodyHandle b = w.AddBody(d);
    // corner distance from center = sqrt(3^2 + 4^2) = 5, radius 0.
    CHECK_THAT(static_cast<double>(w.MaxExtentSlot(w.SlotOf(b))),
               Catch::Matchers::WithinAbs(5.0, 1e-4));
}

TEST_CASE("PhysicsSleep: per-body sleepThreshold override beats the world default", "[physics][sleep]")
{
    WorldDef wd; wd.sleepThreshold = Real(5);
    wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.gravityY               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
    wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
    wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
    PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1);
    const BodyHandle inherit = w.AddBody(d);
    d.sleepThreshold = Real(12);                 // per-body override
    const BodyHandle overridden = w.AddBody(d);
    CHECK(w.SleepThresholdSlot(w.SlotOf(inherit))    == Real(5));
    CHECK(w.SleepThresholdSlot(w.SlotOf(overridden)) == Real(12));
}

// THE regression guard this bug lacked: a slowly-ROLLING body (the never-settle
// blocker class) must sleep. With the OLD separate gates (|w| < 0.05) a body at
// w=0.08 never idled -> never slept. The combined test ( |v| + |w|*maxExtent )
// counts 0.08*10 = 0.8 px/s < 5 -> idle -> sleeps.
TEST_CASE("PhysicsSleep: a slowly-rolling body sleeps (combined test)", "[physics][sleep]")
{
    WorldDef wd; wd.gravityY = Real(0); wd.sleepThreshold = Real(5);
    wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
    wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
    wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
    PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1); // NOT fixedRotation -> can roll
    const BodyHandle b = w.AddBody(d);
    w.SetAngularVelocity(b, Real(0.08));         // rolls at 0.08 rad/s; |v|=0
    for (int k = 0; k < 120; ++k) { w.Step(kStep); } // > kSleepTime (0.5s = 30 steps)
    REQUIRE_FALSE(w.IsAwake(b));                  // combined sleepVel 0.8 < 5 -> sleeps
}

// Counter-guard: a body genuinely spinning fast must NOT sleep.
TEST_CASE("PhysicsSleep: a fast-spinning body never sleeps", "[physics][sleep]")
{
    WorldDef wd; wd.gravityY = Real(0); wd.sleepThreshold = Real(5);
    wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
    wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
    wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
    PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1);
    const BodyHandle b = w.AddBody(d);
    w.SetAngularVelocity(b, Real(2.0));          // 2.0*10 = 20 px/s >> 5
    for (int k = 0; k < 120; ++k) { w.Step(kStep); REQUIRE(w.IsAwake(b)); }
}
