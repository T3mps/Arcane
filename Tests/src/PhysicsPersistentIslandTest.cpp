// Physics Phase A: persistent incremental islands -- BEHAVIORAL tests.
// Companion to PhysicsIslandTest.cpp (the original 6-case sleep suite,
// which stays UNCHANGED). These cover the persistent-registry facts:
// create-time 1-body islands, merge on touch, deferred split, and the
// wake-the-whole-island fan-out. Determinism is the contract.
//
// PRESENTATION-FREE + C++23-clean.

#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>

using namespace Arcane::Physics;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type     = BodyType::Static;
        def.position = pos;
        def.shape    = MakeAabb(hw, hh);
        def.friction = Real(0.6);
        return w.AddBody(def);
    }
    BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type          = BodyType::Dynamic;
        def.position      = pos;
        def.shape         = MakeAabb(hw, hh);
        def.density       = Real(1);
        def.friction      = Real(0.4);
        def.fixedRotation = true;
        return w.AddBody(def);
    }
}

// ---------------------------------------------------------------------------
// Create-time: a fresh dynamic body is its own 1-body island; two separated
// dynamics have DISTINCT island roots; a static body never shares a dynamic's
// root (statics are not island members).
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsPersistentIsland: fresh dynamic body is its own island; static has none",
          "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(0); // no gravity -> bodies stay put, never touch
    PhysicsWorld w(wd);

    const BodyHandle floor = AddFloor(w, Vec2(Real(0), Real(500)), Real(50), Real(5));
    const BodyHandle b0    = AddBox(w, Vec2(Real(-200), Real(0)), Real(5), Real(5));
    const BodyHandle b1    = AddBox(w, Vec2(Real(200),  Real(0)), Real(5), Real(5));

    // Before any Step: each dynamic is its own island (assigned at AddBody).
    const std::uint32_t r0 = w.IslandRootOf(b0.index);
    const std::uint32_t r1 = w.IslandRootOf(b1.index);
    CHECK(r0 != r1); // two far-apart dynamics -> different islands

    // The static body is not a member. IslandRootOf returns a high-bit-tagged
    // slot for a non-member, which can NEVER equal a real (dense, small)
    // island id -> distinct from r0/r1.
    const std::uint32_t rf = w.IslandRootOf(floor.index);
    CHECK(rf != r0);
    CHECK(rf != r1);

    // Stepping with no contacts keeps them distinct (no merge happens).
    for (int k = 0; k < 5; ++k) { w.Step(kStep); }
    CHECK(w.IslandRootOf(b0.index) != w.IslandRootOf(b1.index));
}

// ---------------------------------------------------------------------------
// Merge: dynamic bodies in a touching chain coalesce into ONE island.
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsPersistentIsland: touching chain merges to one island",
          "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    const Real hw = Real(5), hh = Real(5);
    const Real gap = Real(0.5);
    const BodyHandle b0 = AddBox(w, Vec2(Real(0), -hh),                              hw, hh);
    const BodyHandle b1 = AddBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)),         hw, hh);
    const BodyHandle b2 = AddBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)*Real(2)), hw, hh);
    const BodyHandle far = AddBox(w, Vec2(Real(500), -hh), hw, hh);

    // Settle into contact while still awake (mirrors PhysicsIslandTest case 5).
    for (int k = 0; k < 30; ++k) { w.Step(kStep); }
    REQUIRE(w.IsAwake(b0));
    REQUIRE(w.IsAwake(b1));
    REQUIRE(w.IsAwake(b2));

    const std::uint32_t r0 = w.IslandRootOf(b0.index);
    CHECK(r0 == w.IslandRootOf(b1.index));
    CHECK(r0 == w.IslandRootOf(b2.index));
    CHECK(r0 != w.IslandRootOf(far.index)); // isolated body -> different island
}
