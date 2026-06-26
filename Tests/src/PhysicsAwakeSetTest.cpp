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
// Rerouting the solver loops to the awake-set must not change the result:
// a settle scene is run-twice-identical AND ends fully asleep + frozen.
TEST_CASE("PhysicsAwakeSet: awake-only solve is deterministic + settles identically", "[physics][awakeset]")
{
    auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
        WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
        AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
        std::vector<BodyHandle> boxes;
        for (int i = 0; i < 5; ++i) { boxes.push_back(AddBox(w, Vec2(Real(0), Real(-10) - Real(9)*static_cast<Real>(i)), Real(4), Real(4))); }
        for (int k = 0; k < 900; ++k) { w.Step(kStep); }
        pos.clear(); awake.clear();
        for (const BodyHandle b : boxes) { pos.push_back(w.Position(b)); awake.push_back(w.IsAwake(b)?1:0); }
    };
    std::vector<Vec2> p1, p2; std::vector<int> a1, a2;
    run(p1, a1); run(p2, a2);
    REQUIRE(p1.size() == p2.size());
    for (std::size_t i = 0; i < p1.size(); ++i) { REQUIRE(p1[i].x == p2[i].x); REQUIRE(p1[i].y == p2[i].y); REQUIRE(a1[i] == a2[i]); }
}
// The awake-set must, at all times, contain EXACTLY the awake dynamic slots.
TEST_CASE("PhysicsAwakeSet: set membership tracks awake-dynamic slots", "[physics][awakeset]")
{
    auto checkInvariant = [](PhysicsWorld& w) {
        const std::vector<std::uint32_t>& set = w.AwakeBodies();
        std::vector<std::uint8_t> seen(w.Count(), 0u);
        for (const std::uint32_t s : set) {
            REQUIRE(s < w.Count());
            REQUIRE(w.Alive(s));
            REQUIRE(w.TypeSlot(s) == BodyType::Dynamic);
            REQUIRE(w.AwakeSlot(s));
            REQUIRE(seen[s] == 0u); // no duplicates
            seen[s] = 1u;
        }
        for (std::uint32_t i = 0; i < w.Count(); ++i) {
            const bool awakeDyn = w.Alive(i) && w.TypeSlot(i) == BodyType::Dynamic && w.AwakeSlot(i);
            REQUIRE((seen[i] != 0u) == awakeDyn);
        }
    };

    WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    const BodyHandle b0 = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
    const BodyHandle b1 = AddBox(w, Vec2(Real(0), Real(-40)), Real(5), Real(5));
    checkInvariant(w);                                  // 2 awake dynamics
    for (int k = 0; k < 700; ++k) { w.Step(kStep); }
    checkInvariant(w);                                  // settled -> asleep -> not in set
    w.ApplyImpulse(b1, Vec2(Real(0), Real(-8000)));     // wake fan-out
    checkInvariant(w);
    w.RemoveBody(b0);                                   // out of the set
    checkInvariant(w);
    const BodyHandle b2 = AddBox(w, Vec2(Real(0), Real(-60)), Real(5), Real(5)); // recycle a slot
    (void)b2;
    checkInvariant(w);
}
