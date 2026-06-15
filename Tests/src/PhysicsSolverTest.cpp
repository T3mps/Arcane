// Physics M6 P2.2: the Soft Step solver -- BEHAVIORAL INVARIANT tests.
//
// MODERNIZATION CENTERPIECE. The SoftStep solver (Solver/SoftStep.cpp) is a
// from-scratch Box2D-v3 TGS Soft implementation, NOT a port of the Lua
// SequentialImpulse (that is a separate A/B oracle in P2.3). It is therefore
// validated by BEHAVIORAL INVARIANTS, not bit-matched to the Lua: a ball rests
// on a floor, a stack of boxes settles, a high mass-ratio pair stays stable,
// energy stays bounded, restitution rebounds, friction stops a slide, a
// kinematic body pushes a dynamic one, and a scripted dynamic scene is
// deterministic across two runs.
//
// CARTESIAN reformulation of the Lua harness dynamics block (physics_harness/
// main.lua:686-754) + the P2.2 plan invariants. +Y is DOWN (gravity is +Y) to
// match the engine's screen-space convention (the dynamics tests use gravityY).
//
// TUNING (documented per the task). The WorldDef defaults are the Box2D v3
// values: substepCount = 4, contactHertz = 30, contactDampingRatio = 10,
// restitutionThreshold = 20, contactPushMaxVelocity = 300. They hold every
// invariant below with NO per-test overrides.
//
// PENETRATION characteristic of THIS soft solver (measured, f32, 60 Hz):
//   single ball on floor   : pen ~ 0.02  (assert < 0.1)
//   5-box stack            : pen ~ 0.18  (assert < 0.2)
//   100:1 mass-ratio pair  : pen ~ 0.56  (assert < 0.7)
// Soft contacts allow a few percent of overlap that the soft push-out drives
// toward kLinearSlop (0.005); it shrinks monotonically with more substeps or a
// higher contactHertz (a sweep showed subs=8/hertz=60 -> stack pen ~0.02,
// massratio pen ~0.14). The defaults trade a hair of resting overlap for the
// cheaper 4-substep step; a caller wanting crisper contacts raises
// substepCount/contactHertz on the WorldDef. kLinearSlop itself is the
// asymptotic floor, NOT the per-frame transient bound. NOTE: at substepCount=4
// the contact-hertz clamp (0.25*substepCount/dt == 60) means hertz=30 is the
// soft regime; pushing hertz to the clamp (60) at only 4 substeps destabilizes
// the 100:1 mass ratio -- the safe high-stiffness config is subs=8/hertz=60.
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
#include <Arcane/Physics/Solver/Solver.hpp>
#include <Arcane/Physics/Solver/SoftStep.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // A box AABB body (fixedRotation -- the engine requires it for AABBs).
    BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh,
                      Real density = Real(1), Real friction = Real(0.4),
                      Real restitution = Real(0))
    {
        BodyDef def;
        def.type          = BodyType::Dynamic;
        def.position      = pos;
        def.shape         = MakeAabb(hw, hh);
        def.density       = density;
        def.friction      = friction;
        def.restitution   = restitution;
        def.fixedRotation = true;
        return w.AddBody(def);
    }

    BodyHandle AddCircle(PhysicsWorld& w, Vec2 pos, Real r,
                         Real density = Real(1), Real friction = Real(0.4),
                         Real restitution = Real(0))
    {
        BodyDef def;
        def.type        = BodyType::Dynamic;
        def.position    = pos;
        def.shape       = MakeCircle(r);
        def.density     = density;
        def.friction    = friction;
        def.restitution = restitution;
        return w.AddBody(def);
    }

    // A static floor (AABB) centered at `pos`. Its TOP surface is pos.y - hh.
    BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh,
                        Real friction = Real(0.6))
    {
        BodyDef def;
        def.type     = BodyType::Static;
        def.position = pos;
        def.shape    = MakeAabb(hw, hh);
        def.friction = friction;
        return w.AddBody(def);
    }

    Real Speed(const PhysicsWorld& w, BodyHandle h)
    {
        const Vec2 v = w.Velocity(h);
        return std::sqrt(v.x * v.x + v.y * v.y);
    }
}

// ---------------------------------------------------------------------------
// Ball rests on a floor
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: ball rests on floor", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400); // +Y down
    PhysicsWorld w(wd);

    // Floor top at y = 0 (center y = 5, hh = 5).
    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

    const Real r = Real(2);
    // Drop a circle from a little above the floor.
    BodyHandle ball = AddCircle(w, Vec2(Real(0), Real(-20)), r);

    for (int k = 0; k < 240; ++k)
    {
        w.Step(kStep);
    }

    const Vec2 p = w.Position(ball);
    const Real floorTop = Real(0);
    // Center should rest ~ floorTop - r (ball sits ON TOP, which is the -Y side
    // since +Y is down: the ball's lowest point is p.y + r touching floorTop).
    REQUIRE(p.y == Approx(floorTop - r).margin(Real(0.1)));
    // Penetration: ball bottom (p.y + r) should not sink far past floorTop.
    const Real penetration = (p.y + r) - floorTop;
    REQUIRE(penetration < Real(0.1));
    // Nearly still.
    REQUIRE(Speed(w, ball) < Real(1.0));
    // Did not tunnel sideways.
    REQUIRE(p.x == Approx(Real(0)).margin(Real(0.5)));
}

// ---------------------------------------------------------------------------
// Stack of N boxes settles with small penetration + near-zero velocity
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: box stack settles", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    // Floor top at y = 0.
    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

    const Real hw = Real(2), hh = Real(2);
    const int N = 5;
    std::vector<BodyHandle> boxes;
    // Stack boxes with a small gap so they fall together a touch then settle.
    // Box i center starts at y = -(2*hh)*(i+1) (above the floor, +Y down).
    for (int i = 0; i < N; ++i)
    {
        const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
        boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
    }

    for (int k = 0; k < 600; ++k)
    {
        w.Step(kStep);
    }

    // Each box settled: near-zero velocity + small overlap with its neighbor.
    Real maxPen = Real(0);
    Real maxSpeed = Real(0);
    for (int i = 0; i < N; ++i)
    {
        maxSpeed = std::max(maxSpeed, Speed(w, boxes[i]));
    }
    // Penetration check: bottom box bottom vs floor top, and each box vs the one
    // below (centers should be ~2*hh apart).
    const Real floorTop = Real(0);
    const Real bottomPen = (w.Position(boxes[0]).y + hh) - floorTop;
    maxPen = std::max(maxPen, bottomPen);
    for (int i = 1; i < N; ++i)
    {
        const Real below = w.Position(boxes[i - 1]).y;
        const Real cur   = w.Position(boxes[i]).y;
        // cur is above below (more negative). gap of centers should be ~2*hh.
        const Real centerGap = below - cur; // positive
        const Real pen = (Real(2) * hh) - centerGap; // overlap if centers too close
        maxPen = std::max(maxPen, pen);
    }

    REQUIRE(maxSpeed < Real(2.0));     // settled, no jitter/explosion
    REQUIRE(maxPen   < Real(0.22));    // small soft-contact overlap (see header);
                                       // bumped from 0.2 to 0.22 for v2 Collide
                                       // narrowphase (T3 clip-manifold vs old SAT
                                       // reports marginally different depths)
    // The stack stayed roughly vertical (no sideways drift).
    for (int i = 0; i < N; ++i)
    {
        REQUIRE(w.Position(boxes[i]).x == Approx(Real(0)).margin(Real(0.5)));
    }
}

// ---------------------------------------------------------------------------
// High mass-ratio pair is stable (heavy box on a light box, on a floor)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: high mass-ratio stable", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

    const Real hw = Real(2), hh = Real(2);
    // Light box on the bottom, very heavy box on top (mass ratio ~100).
    BodyHandle light = AddBox(w, Vec2(Real(0), Real(-2.1)), hw, hh, /*density=*/Real(1));
    BodyHandle heavy = AddBox(w, Vec2(Real(0), Real(-6.3)), hw, hh, /*density=*/Real(100));

    for (int k = 0; k < 600; ++k)
    {
        w.Step(kStep);
    }

    // No explosion: bounded velocity + the light box not crushed through floor.
    REQUIRE(Speed(w, light) < Real(5.0));
    REQUIRE(Speed(w, heavy) < Real(5.0));
    const Real floorTop = Real(0);
    const Real lightPen = (w.Position(light).y + hh) - floorTop;
    REQUIRE(lightPen < Real(0.7)); // tolerant under the 100:1 load (see header)
    REQUIRE(std::isfinite(w.Position(heavy).y));
    REQUIRE(std::isfinite(w.Position(light).y));
}

// ---------------------------------------------------------------------------
// Energy bounded: a settling scene's kinetic energy does not blow up
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: energy bounded", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

    const Real hw = Real(2), hh = Real(2);
    std::vector<BodyHandle> boxes;
    for (int i = 0; i < 4; ++i)
    {
        boxes.push_back(AddBox(w, Vec2(Real(0), -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1)),
                               hw, hh));
    }

    // Let them fall + settle, then track that KE stays bounded for the rest.
    for (int k = 0; k < 300; ++k)
    {
        w.Step(kStep);
    }

    Real peakKE = Real(0);
    for (int k = 0; k < 600; ++k)
    {
        w.Step(kStep);
        Real ke = Real(0);
        for (auto h : boxes)
        {
            const Vec2 v = w.Velocity(h);
            ke += Real(0.5) * (v.x * v.x + v.y * v.y); // unit-ish mass proxy
        }
        peakKE = std::max(peakKE, ke);
        REQUIRE(std::isfinite(ke));
    }
    // After settling the residual KE must be tiny (no energy injection blow-up).
    REQUIRE(peakKE < Real(50.0));
}

// ---------------------------------------------------------------------------
// Restitution rebounds: a bouncy ball rebounds a good fraction of drop height
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: restitution rebounds", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

    const Real r = Real(2);
    const Real startY = Real(-40); // floor top is y=0; drop height ~ 38
    BodyHandle ball = AddCircle(w, Vec2(Real(0), startY), r,
                                /*density=*/Real(1), /*friction=*/Real(0.1),
                                /*restitution=*/Real(0.8));

    // The ball's resting center would be y = -r = -2. Drop height (of the
    // contact point) ~ |startY - (-r)| = 38.
    const Real restY = -r;
    const Real dropHeight = std::abs(startY - restY);

    // Track the highest point reached AFTER the first bounce (most negative y).
    Real apexY = startY; // initialize to start
    bool hitFloor = false;
    Real prevY = startY;
    for (int k = 0; k < 240; ++k)
    {
        w.Step(kStep);
        const Real y = w.Position(ball).y;
        // Detect that it has been near the floor at least once.
        if ((y + r) > Real(-0.5))
        {
            hitFloor = true;
        }
        // After hitting the floor, track the rebound apex (min y == highest up).
        if (hitFloor && y < apexY)
        {
            apexY = y;
        }
        prevY = y;
    }
    (void)prevY;

    REQUIRE(hitFloor);
    // Rebound height of the contact point above the floor at apex.
    const Real reboundHeight = std::abs(apexY - restY);
    // With e = 0.8, energy ~ e^2 = 0.64 of drop -> rebound ~ 0.64 * dropHeight
    // ideally. Allow for solver damping: require at least 30% of drop height.
    REQUIRE(reboundHeight > Real(0.3) * dropHeight);
    // And not MORE than the drop (no energy gain).
    REQUIRE(reboundHeight < dropHeight + Real(1.0));
}

// ---------------------------------------------------------------------------
// Friction stops a sliding box
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: friction stops a sliding box", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5), /*friction=*/Real(0.8));

    const Real hw = Real(2), hh = Real(2);
    BodyHandle box = AddBox(w, Vec2(Real(0), Real(-2)), hw, hh,
                            /*density=*/Real(1), /*friction=*/Real(0.8));

    // Let it settle onto the floor first.
    for (int k = 0; k < 60; ++k)
    {
        w.Step(kStep);
    }
    // Give it a horizontal kick.
    w.SetVelocity(box, Vec2(Real(120), Real(0)));

    Real maxX = w.Position(box).x;
    for (int k = 0; k < 600; ++k)
    {
        w.Step(kStep);
        maxX = std::max(maxX, w.Position(box).x);
    }

    // Friction decelerated it to rest.
    REQUIRE(std::abs(w.Velocity(box).x) < Real(2.0));
    // It DID move some distance (friction is not infinite glue).
    REQUIRE(maxX > Real(2.0));
    // It did not sink through the floor.
    REQUIRE((w.Position(box).y + hh) - Real(0) < Real(0.1));
}

// ---------------------------------------------------------------------------
// Kinematic pushes dynamic (kinematic undeflected; dynamic moves)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: kinematic pushes dynamic", "[physics][solver]")
{
    PhysicsWorld w; // gravity 0 -- isolate the push

    const Real hw = Real(2), hh = Real(2);
    // Kinematic plate moving +x toward the dynamic box.
    BodyDef kdef;
    kdef.type     = BodyType::Kinematic;
    kdef.position = Vec2(Real(-10), Real(0));
    kdef.shape    = MakeAabb(hw, hh);
    BodyHandle plate = w.AddBody(kdef);
    w.SetVelocity(plate, Vec2(Real(60), Real(0)));

    // Dynamic box in its path.
    BodyHandle box = AddBox(w, Vec2(Real(0), Real(0)), hw, hh);

    const Real boxX0 = w.Position(box).x;
    for (int k = 0; k < 120; ++k)
    {
        w.Step(kStep);
    }

    // Kinematic kept its own trajectory (undeflected): x = -10 + 60*t.
    const Real t = kStep * Real(120);
    REQUIRE(w.Position(plate).x == Approx(Real(-10) + Real(60) * t).margin(Real(0.1)));
    REQUIRE(w.Position(plate).y == Approx(Real(0)).margin(Real(1e-3)));
    // Dynamic box was pushed forward (+x) and out of the way.
    REQUIRE(w.Position(box).x > boxX0 + Real(2.0));
    // The plate did not pass clean through (box stays ahead of plate's right edge
    // minus a small overlap).
    REQUIRE(w.Position(box).x > w.Position(plate).x);
}

// ---------------------------------------------------------------------------
// Determinism: a scripted dynamic scene run twice -> identical state hash
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: deterministic dynamic scene", "[physics][solver]")
{
    auto run = [](std::vector<Real>& trace)
    {
        WorldDef wd;
        wd.gravityY = Real(400);
        PhysicsWorld w(wd);

        AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));
        std::vector<BodyHandle> boxes;
        const Real hw = Real(2), hh = Real(2);
        for (int i = 0; i < 4; ++i)
        {
            boxes.push_back(AddBox(w, Vec2(Real(0.0), -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1)),
                                   hw, hh));
        }
        // Give one a sideways nudge for a richer (asymmetric) trajectory.
        w.ApplyImpulse(boxes[2], Vec2(Real(40), Real(0)));

        trace.clear();
        for (int k = 0; k < 200; ++k)
        {
            w.Step(kStep);
            for (auto h : boxes)
            {
                const Vec2 p = w.Position(h);
                const Vec2 v = w.Velocity(h);
                trace.push_back(p.x);
                trace.push_back(p.y);
                trace.push_back(v.x);
                trace.push_back(v.y);
            }
        }
    };

    std::vector<Real> a, b;
    run(a);
    run(b);

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        REQUIRE(a[i] == b[i]); // bit-identical: deterministic, index-ordered
    }
}

// ---------------------------------------------------------------------------
// Warm-start cache stays bounded across many steps
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: warm-start cache bounded", "[physics][solver]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));
    std::vector<BodyHandle> boxes;
    const Real hw = Real(2), hh = Real(2);
    for (int i = 0; i < 4; ++i)
    {
        boxes.push_back(AddBox(w, Vec2(Real(0), -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1)),
                               hw, hh));
    }

    // The cache holds one entry per live contact point. A 4-box stack on a
    // floor has a bounded number of contact points (each box: vs the box below
    // or floor, up to 2 points). The cache must NOT grow without bound over a
    // long run -- assert a generous ceiling. (Each AABB-AABB manifold emits <= 2
    // points; 4 boxes -> <= ~10 points -> well under 64.)
    std::size_t maxCache = 0;
    for (int k = 0; k < 1000; ++k)
    {
        w.Step(kStep);
        for (auto h : boxes)
        {
            REQUIRE(std::isfinite(w.Position(h).y));
        }
        maxCache = std::max(maxCache, w.SolverWarmStartCacheSize());
    }
    REQUIRE(maxCache < std::size_t(64)); // bounded -- eviction works
}

// ---------------------------------------------------------------------------
// Warm-start cache eviction (direct SoftStep unit test)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolver: warm-start cache evicts stale entries", "[physics][solver]")
{
    // Drive a SoftStep directly with a tiny synthetic scene through the world so
    // the cache fills, then run steps with NO contacts and confirm it drains.
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));
    BodyHandle ball = AddCircle(w, Vec2(Real(0), Real(-10)), Real(2));

    // Settle the ball so it is generating contacts (cache populated). Capture the
    // peak cache size WHILE the ball is still in contact -- as of P2.4 the island
    // pass eventually sleeps a resting ball, which stops contact generation and
    // legitimately drains the cache, so we must observe the populated state during
    // the active-contact window rather than at a fixed late step.
    std::size_t peakCache = 0;
    for (int k = 0; k < 120; ++k)
    {
        w.Step(kStep);
        peakCache = std::max(peakCache, w.SolverWarmStartCacheSize());
    }
    REQUIRE(peakCache > std::size_t(0)); // populated while in contact

    // Remove the ball -> no more contacts -> the cache must DRAIN to 0 within a
    // few steps (kCacheLife = 2 means an entry survives <= 2 unused stamps).
    w.RemoveBody(ball);
    for (int k = 0; k < 5; ++k)
    {
        w.Step(kStep);
    }
    REQUIRE(w.SolverWarmStartCacheSize() == std::size_t(0)); // fully evicted
}
