// Physics M6 P2.4: the island constraint-graph + sleep module -- BEHAVIORAL tests.
//
// PORT NOTE: P2.4 extracted the inline Lua island/sleep logic
// (Client/src/physics/PhysicsWorld.lua:28-31 + 403-452) into Physics/Island.cpp
// and wired it at the Step stage-4 seam (after the solve, using this step's
// contacts). There is no Island.lua; the oracle is the harness "islands +
// sleeping (M3)" block (Client/src/tests/physics_harness/main.lua:756-777),
// reformulated Cartesian here (+Y is DOWN, gravity is +Y) plus the plan's
// island-as-a-unit invariants.
//
// INVARIANTS (behavioral, not bit-matched):
//   * a resting dynamic body sleeps after enough idle steps (sleepT > 0.5s);
//   * once asleep its position is FROZEN (no integrate, no solve, no drift);
//   * ApplyImpulse on a sleeper wakes it + it moves on the next step;
//   * an awake mover moving into a sleeper wakes it (wake-on-contact);
//   * an island settles + sleeps AS A UNIT (no half-sleep); disturbing one
//     member wakes it (and, via the contact graph, its neighbors on contact);
//   * determinism: run-twice identical.
//
// PRESENTATION-FREE + C++20-clean.

#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Island.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // A static floor (AABB) centered at `pos`; its TOP surface is pos.y - hh.
    BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type     = BodyType::Static;
        def.position = pos;
        def.shape    = MakeAabb(hw, hh);
        def.friction = Real(0.6);
        return w.AddBody(def);
    }

    BodyHandle AddCircle(PhysicsWorld& w, Vec2 pos, Real r = Real(10),
                         Real friction = Real(0.4))
    {
        BodyDef def;
        def.type     = BodyType::Dynamic;
        def.position = pos;
        def.shape    = MakeCircle(r);
        def.density  = Real(1);
        def.friction = friction;
        return w.AddBody(def);
    }

    BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh, Real friction = Real(0.4))
    {
        BodyDef def;
        def.type          = BodyType::Dynamic;
        def.position      = pos;
        def.shape         = MakeAabb(hw, hh);
        def.density       = Real(1);
        def.friction      = friction;
        // fixedRotation: boxes stay axis-aligned (stack stability), isolates the
        // island-sleep behavior under test.
        def.fixedRotation = true;
        return w.AddBody(def);
    }
}

// ---------------------------------------------------------------------------
// A resting body sleeps; once asleep it does not integrate (position frozen).
// (Direct Cartesian port of harness main.lua:756-777.)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsIsland: resting body sleeps then stays frozen", "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400); // +Y down
    PhysicsWorld w(wd);

    // Floor top at y = 0.
    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    BodyHandle ball = AddCircle(w, Vec2(Real(0), Real(-50)));

    // The ball drops, settles, and the island pass sleeps it once it has been
    // idle for > 0.5s. 400 steps is plenty (matches the harness loop count).
    REQUIRE(w.IsAwake(ball));
    for (int k = 0; k < 400; ++k)
    {
        w.Step(kStep);
    }
    REQUIRE_FALSE(w.IsAwake(ball)); // resting ball sleeps

    // FROZEN: position does not change across further steps (no integrate/solve).
    const Vec2 p0 = w.Position(ball);
    for (int k = 0; k < 120; ++k)
    {
        w.Step(kStep);
    }
    const Vec2 p1 = w.Position(ball);
    REQUIRE(p1.x == p0.x); // exact -- a sleeping body is not integrated at all
    REQUIRE(p1.y == p0.y);
    REQUIRE_FALSE(w.IsAwake(ball));
}

// ---------------------------------------------------------------------------
// ApplyImpulse wakes a sleeping body; it moves again on the next step.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsIsland: impulse wakes a sleeping body", "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
    BodyHandle ball = AddCircle(w, Vec2(Real(0), Real(-50)));

    for (int k = 0; k < 400; ++k)
    {
        w.Step(kStep);
    }
    REQUIRE_FALSE(w.IsAwake(ball));

    const Real y0 = w.Position(ball).y;

    // Kick the ball upward (-Y). ApplyImpulse must wake it (P2.1 wake-on-force).
    w.ApplyImpulse(ball, Vec2(Real(0), Real(-6000)));
    REQUIRE(w.IsAwake(ball)); // impulse wakes the body

    w.Step(kStep);
    REQUIRE(w.Position(ball).y < y0); // woken body moves again (upward = smaller y)
}

// ---------------------------------------------------------------------------
// A new contact wakes a sleeper: an awake mover driven into it wakes it.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsIsland: new contact wakes a sleeping body", "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));

    // A dynamic ball that will settle + sleep on the floor.
    const Real r = Real(10);
    BodyHandle sleeper = AddCircle(w, Vec2(Real(0), Real(-50)));

    // A kinematic mover (statics/kinematics never sleep) parked off to the side.
    BodyDef mover;
    mover.type     = BodyType::Kinematic;
    mover.position = Vec2(Real(-80), Real(-10));
    mover.shape    = MakeCircle(r);
    BodyHandle pusher = w.AddBody(mover);

    for (int k = 0; k < 400; ++k)
    {
        w.Step(kStep);
    }
    REQUIRE_FALSE(w.IsAwake(sleeper)); // settled + asleep

    // Drive the pusher rightward into the sleeping ball. The wake-on-contact
    // path in GenerateContacts must wake the sleeper once they AABB-overlap.
    w.SetVelocity(pusher, Vec2(Real(120), Real(0)));
    bool woke = false;
    for (int k = 0; k < 120; ++k)
    {
        w.Step(kStep);
        if (w.IsAwake(sleeper))
        {
            woke = true;
            break;
        }
    }
    REQUIRE(woke); // an awake mover moving into the sleeper woke it
}

// ---------------------------------------------------------------------------
// An island settles + sleeps AS A UNIT; disturbing one member re-wakes it.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsIsland: stack sleeps as a unit, disturbance wakes a member", "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));

    // A small stack of boxes -> one island (each box contacts the one below).
    const Real hw = Real(4), hh = Real(4);
    const int N = 3;
    std::vector<BodyHandle> boxes;
    for (int i = 0; i < N; ++i)
    {
        const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
        boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
    }

    for (int k = 0; k < 700; ++k)
    {
        w.Step(kStep);
    }

    // The island slept AS A UNIT -- every member is asleep (no half-sleep).
    for (int i = 0; i < N; ++i)
    {
        REQUIRE_FALSE(w.IsAwake(boxes[i]));
    }

    // Positions frozen while the whole island sleeps.
    std::vector<Vec2> frozen;
    for (int i = 0; i < N; ++i)
    {
        frozen.push_back(w.Position(boxes[i]));
    }
    for (int k = 0; k < 60; ++k)
    {
        w.Step(kStep);
    }
    for (int i = 0; i < N; ++i)
    {
        REQUIRE(w.Position(boxes[i]).x == frozen[i].x);
        REQUIRE(w.Position(boxes[i]).y == frozen[i].y);
    }

    // Disturb the TOP box -> it wakes (wake-on-force).
    w.ApplyImpulse(boxes[N - 1], Vec2(Real(0), Real(-8000)));
    REQUIRE(w.IsAwake(boxes[N - 1]));

    // After a few steps the disturbed body has moved + its island neighbors,
    // touched by the now-awake mover, wake on contact (the island re-forms).
    for (int k = 0; k < 30; ++k)
    {
        w.Step(kStep);
    }
    bool anyNeighborAwake = false;
    for (int i = 0; i < N - 1; ++i)
    {
        if (w.IsAwake(boxes[i]))
        {
            anyNeighborAwake = true;
        }
    }
    REQUIRE(anyNeighborAwake); // contact graph propagated the wake to a neighbor
}

// ---------------------------------------------------------------------------
// Determinism: a settling-then-sleeping island is identical across two runs.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsIsland: settle + sleep is deterministic across two runs", "[physics][island]")
{
    auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
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
        for (int k = 0; k < 700; ++k)
        {
            w.Step(kStep);
        }
        pos.clear();
        awake.clear();
        for (int i = 0; i < N; ++i)
        {
            pos.push_back(w.Position(boxes[i]));
            awake.push_back(w.IsAwake(boxes[i]) ? 1 : 0);
        }
    };

    std::vector<Vec2> p1, p2;
    std::vector<int>  a1, a2;
    run(p1, a1);
    run(p2, a2);

    REQUIRE(p1.size() == p2.size());
    for (std::size_t i = 0; i < p1.size(); ++i)
    {
        REQUIRE(p1[i].x == p2[i].x); // bit-identical (same scalar ops, same order)
        REQUIRE(p1[i].y == p2[i].y);
        REQUIRE(a1[i] == a2[i]);     // same sleep state
    }
}
