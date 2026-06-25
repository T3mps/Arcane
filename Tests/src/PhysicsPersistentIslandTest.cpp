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

// ---------------------------------------------------------------------------
// Split: separating a touching pair fractures the island (deferred); a body
// that loses all contacts becomes its own 1-body island.
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsPersistentIsland: separating a chain splits the island (deferred)",
          "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    const Real hw = Real(5), hh = Real(5);
    const Real gap = Real(0.5);
    const BodyHandle b0 = AddBox(w, Vec2(Real(0), -hh),                      hw, hh);
    const BodyHandle b1 = AddBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)), hw, hh);

    for (int k = 0; k < 30; ++k) { w.Step(kStep); }
    REQUIRE(w.IsAwake(b0));
    REQUIRE(w.IsAwake(b1));
    REQUIRE(w.IslandRootOf(b0.index) == w.IslandRootOf(b1.index)); // merged

    // Fling the top box far up + sideways so its contact with b0 separates.
    w.SetVelocity(b1, Vec2(Real(400), Real(-2000)));
    // A few steps for the contact to drop + the deferred split to resolve
    // (quota = 1 per step, but only 1 candidate here, so it resolves quickly).
    for (int k = 0; k < 20; ++k) { w.Step(kStep); }

    CHECK(w.IslandRootOf(b0.index) != w.IslandRootOf(b1.index)); // split
}

// ---------------------------------------------------------------------------
// Wake fan-out: disturbing ONE member of a sleeping island wakes the WHOLE
// island immediately (Box2D-style island wake), not just the disturbed body.
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsPersistentIsland: impulse on one member wakes the whole island",
          "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    const Real hw = Real(4), hh = Real(4);
    const int N = 3;
    std::vector<BodyHandle> boxes;
    for (int i = 0; i < N; ++i)
    {
        const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
        boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
    }
    for (int k = 0; k < 700; ++k) { w.Step(kStep); }
    for (int i = 0; i < N; ++i) { REQUIRE_FALSE(w.IsAwake(boxes[i])); }

    // Impulse the MIDDLE box -> the whole island wakes at once.
    w.ApplyImpulse(boxes[1], Vec2(Real(0), Real(-8000)));
    for (int i = 0; i < N; ++i)
    {
        CHECK(w.IsAwake(boxes[i])); // every member awake (island wake)
    }
}

// ---------------------------------------------------------------------------
// Determinism: a scene exercising merge + split + sleep is bit-identical
// across two runs (positions, awake state, island roots).
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsPersistentIsland: merge+split+sleep is deterministic across two runs",
          "[physics][island]")
{
    auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake,
                  std::vector<std::uint32_t>& roots)
    {
        WorldDef wd;
        wd.gravityY = Real(400);
        PhysicsWorld w(wd);

        AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
        const Real hw = Real(4), hh = Real(4);
        std::vector<BodyHandle> boxes;
        for (int i = 0; i < 4; ++i)
        {
            const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
            boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
        }
        for (int k = 0; k < 200; ++k) { w.Step(kStep); }
        // Disturb the top -> a split candidate forms, then re-settles.
        w.ApplyImpulse(boxes[3], Vec2(Real(150), Real(-3000)));
        for (int k = 0; k < 500; ++k) { w.Step(kStep); }

        pos.clear(); awake.clear(); roots.clear();
        for (const BodyHandle b : boxes)
        {
            pos.push_back(w.Position(b));
            awake.push_back(w.IsAwake(b) ? 1 : 0);
            roots.push_back(w.IslandRootOf(b.index));
        }
    };

    std::vector<Vec2> p1, p2; std::vector<int> a1, a2;
    std::vector<std::uint32_t> r1, r2;
    run(p1, a1, r1);
    run(p2, a2, r2);

    REQUIRE(p1.size() == p2.size());
    for (std::size_t i = 0; i < p1.size(); ++i)
    {
        REQUIRE(p1[i].x == p2[i].x);
        REQUIRE(p1[i].y == p2[i].y);
        REQUIRE(a1[i] == a2[i]);
        REQUIRE(r1[i] == r2[i]); // island ids reproduce run-to-run
    }
}
