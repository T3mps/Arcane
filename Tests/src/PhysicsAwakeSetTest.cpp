// Physics Phase B: awake-set + sleep-by-migration -- BEHAVIORAL tests.
// Companion to PhysicsIslandTest.cpp / PhysicsPersistentIslandTest.cpp.
// PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/StepProf.hpp>
using namespace Arcane::Physics;
namespace {
    constexpr Real kStep = Real(1) / Real(60);
    BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
        BodyDef d; d.type=BodyType::Static; d.position=pos; d.shape=MakeAabb(hw,hh); d.friction=Real(0.6); return w.AddBody(d);
    }
    BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
        BodyDef d; d.type=BodyType::Dynamic; d.position=pos; d.shape=MakeAabb(hw,hh); d.density=Real(1); d.friction=Real(0.4); d.fixedRotation=true; return w.AddBody(d);
    }
}
// StepProf is a no-op by default: a Step still runs and the scoped timers compile away.
TEST_CASE("PhysicsAwakeSet: StepProf is a no-op when ARCANE_STEPPROF is off", "[physics][awakeset]")
{
    WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
    for (int k = 0; k < 10; ++k) { w.Step(kStep); }
    REQUIRE(StepProf::Enabled() == false);
}
