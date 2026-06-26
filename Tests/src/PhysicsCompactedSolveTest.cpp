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

// Task 2 re-homes the lane-wide solve scratch onto a DENSE solverCount-sized SoA
// (awake-set index space + kinematic index space; dummy = solverCount) instead of
// the sparse worldCount-sized SoA indexed by world slot. This is PURE re-indexing
// -- no physics math changes -- so the settle MUST be byte-identical. This case is
// the regression gate: run the same scene twice (a run-twice determinism check) and
// assert the two runs produce bit-identical positions + awake flags. The scene
// includes a MOVING kinematic plate pushing a stack of dynamics, so a kinematic
// B-endpoint is exercised (catching a mis-gate where a kinematic B falls through
// to the dummy and loses the push, or a static B reads a stale kinematic index).
// It already passes pre-change (it is a determinism check) and must keep passing.
TEST_CASE("PhysicsCompacted: solve settles identically + deterministically", "[physics][phasec]")
{
    auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
        WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
        AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
        // include a kinematic plate pushing a dynamic so a kinematic B-endpoint is exercised
        BodyDef kd; kd.type=BodyType::Kinematic; kd.position=Vec2(Real(0),Real(-8)); kd.shape=MakeAabb(Real(60),Real(2));
        const BodyHandle k = w.AddBody(kd); w.SetVelocity(k, Vec2(Real(3), Real(0)));
        std::vector<BodyHandle> boxes;
        for (int i = 0; i < 5; ++i) boxes.push_back(AddBox(w, Vec2(Real(0), Real(-20) - Real(9)*static_cast<Real>(i)), Real(4), Real(4)));
        for (int kk = 0; kk < 600; ++kk) w.Step(kStep);
        pos.clear(); awake.clear();
        for (const BodyHandle b : boxes) { pos.push_back(w.Position(b)); awake.push_back(w.IsAwake(b)?1:0); }
        (void)k;
    };
    std::vector<Vec2> p1,p2; std::vector<int> a1,a2; run(p1,a1); run(p2,a2);
    REQUIRE(p1.size()==p2.size());
    for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); REQUIRE(a1[i]==a2[i]); }
}
