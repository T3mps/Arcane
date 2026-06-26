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
// -- no physics math changes -- so the settle MUST be byte-identical.
//
// What this case actually guards: RUN-TWICE DETERMINISM under the dense re-home.
// It runs the same scene twice and asserts bit-identical positions + awake flags.
// The scene includes a MOVING kinematic plate over a stack of dynamics so a moving
// kinematic B-endpoint is EXERCISED (the dense kinematic-index gather path runs),
// but a determinism check passes both pre- and post-change, so it cannot by itself
// catch a *consistent* mis-gate (one that drops the push identically on both runs).
// The kinematic-PUSH behavior itself (a kinematic B actually moving a dynamic) is
// guarded by PhysicsSolverTest.cpp's "PhysicsSolver: kinematic pushes dynamic".
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

// Task 4 maintains a PERSISTENT incremental graph-coloring of the solver-relevant
// body-body contacts: each such contact is assigned a color ONCE at create and
// releases it at destroy (no per-step greedy recolor). The coloring invariant is
// that no two same-color contacts share a DYNAMIC body. ValidatePersistentColoring
// is the oracle that walks the live coloring and proves that invariant after a
// churny settle (30 boxes piling onto a floor, contacts created + destroyed as the
// pile compacts). Task 5 makes the solver CONSUME this coloring (the per-step greedy
// recolor is gone), and ValidatePersistentColoring also cross-checks the per-body
// color mask against the lists. The settle stays byte-identical (a valid coloring
// yields the same solve regardless of greedy-vs-persistent provenance).
TEST_CASE("PhysicsCompacted: persistent contact coloring is valid", "[physics][phasec]")
{
    WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
    AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
    for (int i = 0; i < 30; ++i) AddBox(w, Vec2(Real(-40) + Real(3)*static_cast<Real>(i%20), Real(-20) - Real(9)*static_cast<Real>(i/20)), Real(4), Real(4));
    for (int k = 0; k < 60; ++k) w.Step(kStep);
    REQUIRE(w.ValidatePersistentColoring());     // no two same-color contacts share a dynamic body
    // The oracle must not trivially pass on an EMPTY coloring: a 30-box pile on a
    // floor settles into many dynamic-static + dynamic-dynamic contacts, so at least
    // one body-body contact got colored. ColoredContactCount sums m_colorContacts[k].
    REQUIRE(w.ColoredContactCount() > 0u);
}

TEST_CASE("PhysicsCompacted: incremental coloring is deterministic across two runs (create/destroy churn)", "[physics][phasec]")
{
    auto run = [](std::vector<Vec2>& pos) {
        WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
        AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
        std::vector<BodyHandle> b;
        for (int i = 0; i < 16; ++i) b.push_back(AddBox(w, Vec2(Real(-20) + Real(3)*static_cast<Real>(i), Real(-20)), Real(4), Real(4)));
        for (int k = 0; k < 120; ++k) w.Step(kStep);
        w.RemoveBody(b[4]); w.RemoveBody(b[9]);
        for (int k = 0; k < 60; ++k) w.Step(kStep);
        b.push_back(AddBox(w, Vec2(Real(0), Real(-30)), Real(4), Real(4)));
        for (int k = 0; k < 200; ++k) w.Step(kStep);
        pos.clear(); for (std::size_t i = 0; i < b.size(); ++i) { if (i==4||i==9) continue; pos.push_back(w.Position(b[i])); }
    };
    std::vector<Vec2> p1,p2; run(p1); run(p2);
    REQUIRE(p1.size()==p2.size());
    for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); }
}
