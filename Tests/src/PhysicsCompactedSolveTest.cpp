// Physics Phase C: awake-compacted solver state + incremental coloring -- BEHAVIORAL tests.
// Companion to PhysicsAwakeSetTest.cpp / PhysicsSimdSolverTest.cpp. PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
using namespace Arcane::Physics;
namespace {
    constexpr Real kStep = Real(1) / Real(60);
    BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
        BodyDef d; d.type=BodyType::Static; d.position=pos; d.shape=MakeAabb(hw,hh); d.friction=Real(0.6); return w.AddBody(d);
    }
    BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
        BodyDef d; d.type=BodyType::Dynamic; d.position=pos; d.shape=MakeAabb(hw,hh); d.density=Real(1); d.friction=Real(0.4); d.fixedRotation=true; return w.AddBody(d);
    }
    BodyHandle AddKinematic(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
        BodyDef d; d.type=BodyType::Kinematic; d.position=pos; d.shape=MakeAabb(hw,hh); return w.AddBody(d);
    }
}
// The kinematic-set must, at all times, contain EXACTLY the live kinematic slots.
TEST_CASE("PhysicsCompacted: kinematic-set tracks live kinematic slots", "[physics][phasec]")
{
    auto checkInvariant = [](PhysicsWorld& w) {
        const std::vector<std::uint32_t>& set = w.KinematicBodies();
        std::vector<std::uint8_t> seen(w.Count(), 0u);
        for (const std::uint32_t s : set) {
            REQUIRE(s < w.Count());
            REQUIRE(w.Alive(s));
            REQUIRE(w.TypeSlot(s) == BodyType::Kinematic);
            REQUIRE(seen[s] == 0u);
            seen[s] = 1u;
        }
        for (std::uint32_t i = 0; i < w.Count(); ++i) {
            const bool kin = w.Alive(i) && w.TypeSlot(i) == BodyType::Kinematic;
            REQUIRE((seen[i] != 0u) == kin);
        }
    };
    WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    const BodyHandle k0 = AddKinematic(w, Vec2(Real(-30), Real(-10)), Real(5), Real(5));
    const BodyHandle d0 = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
    const BodyHandle k1 = AddKinematic(w, Vec2(Real(30), Real(-10)), Real(5), Real(5));
    checkInvariant(w);
    for (int kk = 0; kk < 60; ++kk) w.Step(kStep);
    checkInvariant(w);
    w.RemoveBody(k0); checkInvariant(w);          // swap-remove from the kinematic-set
    const BodyHandle k2 = AddKinematic(w, Vec2(Real(0), Real(-50)), Real(5), Real(5));
    checkInvariant(w);                             // recycle a slot as a fresh kinematic
    (void)d0; (void)k1; (void)k2;
}
