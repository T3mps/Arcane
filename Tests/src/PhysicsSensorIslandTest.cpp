// Physics correctness batch -- Task A: sensor contacts must not couple islands.
//
// A dyn-dyn contact whose ONLY fixture pair is a sensor must:
//   (a) NOT merge the two bodies into the same island (sensor is not an edge), and
//   (b) still produce a pooled contact (event-relevant, DebugHasContact == true).
//
// A non-sensor dyn-dyn contact DOES merge islands (regression guard confirming
// the gate on solverRelevant did not over-fire).
//
// PRESENTATION-FREE + C++23-clean. ASCII comments only.

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>

using namespace Arcane::Physics;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    BodyHandle AddSensorBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type          = BodyType::Dynamic;
        def.position      = pos;
        def.shape         = MakeAabb(hw, hh);
        def.isSensor      = true;
        def.density       = Real(1);
        def.fixedRotation = true;
        return w.AddBody(def);
    }

    BodyHandle AddSolidBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type          = BodyType::Dynamic;
        def.position      = pos;
        def.shape         = MakeAabb(hw, hh);
        def.isSensor      = false;
        def.density       = Real(1);
        def.friction      = Real(0.4);
        def.fixedRotation = true;
        return w.AddBody(def);
    }

    BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type     = BodyType::Static;
        def.position = pos;
        def.shape    = MakeAabb(hw, hh);
        def.friction = Real(0.6);
        return w.AddBody(def);
    }
}

// ---------------------------------------------------------------------------
// Task A Step 1+2: a dyn-dyn SENSOR pair must NOT merge islands.
// The sensor contact IS pooled (event-relevant) but the bodies remain in
// separate islands.  IslandRootOf must differ before AND after stepping.
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsSensorIsland: sensor dyn-dyn pair does not merge islands",
          "[physics][island]")
{
    // Zero gravity: both bodies start overlapping, no forces, no solver impulse
    // (sensor -> solverRelevant=false -> no collision response).
    WorldDef wd;
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    // Place bA (sensor) and bB (solid) overlapping so the broadphase detects them.
    // Half-width 5 each, centres 3 apart -> 7-unit overlap.
    const BodyHandle bA = AddSensorBox(w, Vec2(Real(0),  Real(0)), Real(5), Real(5));
    const BodyHandle bB = AddSolidBox (w, Vec2(Real(3),  Real(0)), Real(5), Real(5));

    // Before any Step: distinct islands (one-body each, assigned at AddBody).
    REQUIRE(w.IslandRootOf(bA.index) != w.IslandRootOf(bB.index));

    // Step enough for the broadphase to fire and the contact to be created.
    for (int k = 0; k < 5; ++k) { w.Step(kStep); }

    // The sensor pair produces a pooled contact (event path is live).
    CHECK(w.DebugHasContact(bA, bB));

    // Sensor contact must NOT have merged the islands.
    CHECK(w.IslandRootOf(bA.index) != w.IslandRootOf(bB.index));
}

// ---------------------------------------------------------------------------
// Regression: a NON-sensor dyn-dyn contact DOES merge islands (gate must not
// over-fire).  Mirrors the "touching chain merges to one island" test pattern
// from PhysicsPersistentIslandTest but as a minimal 2-body scene here.
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsSensorIsland: non-sensor dyn-dyn contact merges islands (regression)",
          "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));

    // Two solid boxes stacked: they will settle into contact and merge.
    const Real hw = Real(5), hh = Real(5);
    const Real gap = Real(0.5);
    const BodyHandle bA = AddSolidBox(w, Vec2(Real(0), -hh),                      hw, hh);
    const BodyHandle bB = AddSolidBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)), hw, hh);

    REQUIRE(w.IslandRootOf(bA.index) != w.IslandRootOf(bB.index));

    // Settle into contact while still awake.
    for (int k = 0; k < 30; ++k) { w.Step(kStep); }
    REQUIRE(w.IsAwake(bA));
    REQUIRE(w.IsAwake(bB));

    // Non-sensor dyn-dyn contact must have merged the two bodies into ONE island.
    CHECK(w.IslandRootOf(bA.index) == w.IslandRootOf(bB.index));
}
