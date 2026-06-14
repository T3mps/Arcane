// Physics M6 P2.3: the Baumgarte PGS oracle -- A/B CROSS-CHECK tests.
//
// The Baumgarte solver (Solver/Baumgarte.cpp) is a faithful port of the Lua
// SequentialImpulse; it is the retained A/B cross-check behind the ISolver seam
// (the modernization centerpiece, SoftStep, is the other impl). This file runs
// the SAME behavioral STABILITY invariants as PhysicsSolverTest.cpp against BOTH
// solvers (parameterized by WorldDef::solverKind): a ball rests on a floor, a
// stack settles with bounded penetration, a high mass-ratio pair stays stable, a
// settling scene's energy stays bounded, a bouncy ball rebounds, friction stops
// a slide, a kinematic body pushes a dynamic one, and each solver is
// deterministic across two runs.
//
// WHY A/B: a bug in one solver's contact handling shows up as that solver
// failing a stability invariant the OTHER passes. Running both over identical
// scenes guards against solver-specific bugs (the task's stated goal). The two
// solvers are NOT expected to produce identical numbers -- only to be STABLE +
// bounded -- so penetration thresholds are SOLVER-APPROPRIATE.
//
// PENETRATION THRESHOLDS (per solver -- WHY they differ):
//   SoftStep  : Box2D-v3 soft contacts drive overlap toward kLinearSlop (0.005);
//               measured resting penetration is crisp (~0.02 ball, ~0.18 stack).
//   Baumgarte : the Lua positional bias only kicks in ABOVE SLOP = 0.5 (bias =
//               BETA/dt * max(depth - SLOP, 0)), so the solver intentionally
//               TOLERATES ~SLOP of resting penetration before correcting.
//               Resting overlap therefore settles NEAR SLOP, not near
//               kLinearSlop. The assertions below allow Baumgarte the larger
//               (but still bounded) penetration its SLOP design implies; the
//               POINT of the A/B check is that BOTH are stable + bounded, not
//               that they match.
// MEASURED (f32, 60 Hz): SoftStep ball pen ~0.02 (assert < 0.1), stack ~0.18
// (< 0.2), massratio ~0.56 (< 0.7). Baumgarte ball pen ~0.50 == SLOP (assert
// < 0.7), stack ~0.61 (< 1.2), massratio ~0.28 at velIters=32 (< 1.6 -- with
// headroom for the per-frame transient under load). Both are FINITE +
// non-exploding. (Baumgarte's high mass-ratio case raises velIters to 32 --
// see that test's header for the velIters sweep that motivates it.)
//
// CARTESIAN, +Y DOWN (gravity +Y), matching PhysicsSolverTest.cpp.
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

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // A WorldDef for the given solver, +Y-down gravity.
    WorldDef MakeWorldDef(SolverKind kind, Real gravityY)
    {
        WorldDef wd;
        wd.solverKind = kind;
        wd.gravityY   = gravityY;
        return wd;
    }

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

    // Per-solver resting-penetration budget (see the header block above).
    struct PenBudget
    {
        Real ball;
        Real stack;
        Real massRatio;
    };
    PenBudget BudgetFor(SolverKind kind)
    {
        if (kind == SolverKind::Baumgarte)
        {
            // Baumgarte tolerates ~SLOP (0.5) before correcting -> larger but
            // still bounded resting overlap.
            return PenBudget{ Real(0.7), Real(1.2), Real(1.6) };
        }
        // SoftStep: crisp soft contacts (the PhysicsSolverTest budgets).
        return PenBudget{ Real(0.1), Real(0.2), Real(0.7) };
    }
}

// ---------------------------------------------------------------------------
// Ball rests on a floor (both solvers)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: ball rests on floor (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        PhysicsWorld w(MakeWorldDef(kind, Real(400)));
        AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

        const Real r = Real(2);
        BodyHandle ball = AddCircle(w, Vec2(Real(0), Real(-20)), r);

        for (int k = 0; k < 240; ++k)
        {
            w.Step(kStep);
        }

        const Vec2 p = w.Position(ball);
        const Real floorTop = Real(0);
        const PenBudget budget = BudgetFor(kind);

        // Rests ON TOP of the floor (within the solver's penetration budget).
        REQUIRE(p.y == Approx(floorTop - r).margin(budget.ball + Real(0.1)));
        const Real penetration = (p.y + r) - floorTop;
        REQUIRE(penetration < budget.ball);
        REQUIRE(penetration > Real(-0.5)); // not floating well above the floor
        REQUIRE(Speed(w, ball) < Real(1.0));
        REQUIRE(p.x == Approx(Real(0)).margin(Real(0.5)));
    }
}

// ---------------------------------------------------------------------------
// Stack of N boxes settles (both solvers)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: box stack settles (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        PhysicsWorld w(MakeWorldDef(kind, Real(400)));
        AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

        const Real hw = Real(2), hh = Real(2);
        const int N = 5;
        std::vector<BodyHandle> boxes;
        for (int i = 0; i < N; ++i)
        {
            const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
            boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
        }

        for (int k = 0; k < 600; ++k)
        {
            w.Step(kStep);
        }

        Real maxPen = Real(0);
        Real maxSpeed = Real(0);
        for (int i = 0; i < N; ++i)
        {
            maxSpeed = std::max(maxSpeed, Speed(w, boxes[i]));
        }
        const Real floorTop = Real(0);
        const Real bottomPen = (w.Position(boxes[0]).y + hh) - floorTop;
        maxPen = std::max(maxPen, bottomPen);
        for (int i = 1; i < N; ++i)
        {
            const Real below = w.Position(boxes[i - 1]).y;
            const Real cur   = w.Position(boxes[i]).y;
            const Real centerGap = below - cur;
            const Real pen = (Real(2) * hh) - centerGap;
            maxPen = std::max(maxPen, pen);
        }

        const PenBudget budget = BudgetFor(kind);
        REQUIRE(maxSpeed < Real(2.0));      // settled, no jitter/explosion
        REQUIRE(maxPen   < budget.stack);   // bounded overlap (solver-appropriate)
        for (int i = 0; i < N; ++i)
        {
            REQUIRE(w.Position(boxes[i]).x == Approx(Real(0)).margin(Real(0.5)));
            REQUIRE(std::isfinite(w.Position(boxes[i]).y));
        }
        // Warm-start cache is stamp-evicted and stays bounded: after 600 steps
        // only the currently-active contacts remain (at most 2 points per pair
        // for a 5-box stack -> well under 100). Locks the cache-bounded contract.
        REQUIRE(w.SolverWarmStartCacheSize() < 100u);
    }
}

// ---------------------------------------------------------------------------
// High mass-ratio pair is stable (both solvers)
//
// SOLVER-APPROPRIATE ITERATION COUNT (documented, within the Lua value family):
// the 100:1 mass ratio is a SoftStep showcase -- its soft contacts + the
// contactPushMaxVelocity clamp tame it at the default 4 substeps. Baumgarte
// (the faithful Lua SequentialImpulse PGS port) needs MORE velocity iterations
// to resolve a 100:1 ratio: a measured sweep on this exact scene showed
// velIters=8 explodes (peak light speed ~98, tunnels the floor), velIters=16 is
// marginal (~17), and velIters=32 is rock-stable (peak 0, settles at the floor
// with ~0.28 penetration). velIters is the Lua's own tunable (w.velIters,
// default 8); bumping it to 32 for the heavy-ratio scene is squarely within the
// Lua value family (the SoftStep test analogously documents subs=8/hertz=60 for
// crisp contacts). Both solvers then hold the SAME scene -- the A/B point.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: high mass-ratio stable (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        WorldDef wd = MakeWorldDef(kind, Real(400));
        // Baumgarte: 32 velocity iterations for the 100:1 ratio (see header).
        // SoftStep ignores velIters (it iterates by substepCount); the default
        // 4 substeps already hold the ratio.
        if (kind == SolverKind::Baumgarte)
        {
            wd.velIters = 32u;
        }
        PhysicsWorld w(wd);
        AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

        const Real hw = Real(2), hh = Real(2);
        BodyHandle light = AddBox(w, Vec2(Real(0), Real(-2.1)), hw, hh, /*density=*/Real(1));
        BodyHandle heavy = AddBox(w, Vec2(Real(0), Real(-6.3)), hw, hh, /*density=*/Real(100));

        for (int k = 0; k < 600; ++k)
        {
            w.Step(kStep);
        }

        REQUIRE(Speed(w, light) < Real(5.0));
        REQUIRE(Speed(w, heavy) < Real(5.0));
        const Real floorTop = Real(0);
        const Real lightPen = (w.Position(light).y + hh) - floorTop;
        const PenBudget budget = BudgetFor(kind);
        REQUIRE(lightPen < budget.massRatio); // bounded under the 100:1 load
        REQUIRE(std::isfinite(w.Position(heavy).y));
        REQUIRE(std::isfinite(w.Position(light).y));
    }
}

// ---------------------------------------------------------------------------
// Energy bounded (both solvers)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: energy bounded (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        PhysicsWorld w(MakeWorldDef(kind, Real(400)));
        AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

        const Real hw = Real(2), hh = Real(2);
        std::vector<BodyHandle> boxes;
        for (int i = 0; i < 4; ++i)
        {
            boxes.push_back(AddBox(w, Vec2(Real(0), -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1)),
                                   hw, hh));
        }

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
                ke += Real(0.5) * (v.x * v.x + v.y * v.y);
            }
            peakKE = std::max(peakKE, ke);
            REQUIRE(std::isfinite(ke));
        }
        REQUIRE(peakKE < Real(50.0)); // no energy-injection blow-up
    }
}

// ---------------------------------------------------------------------------
// Restitution rebounds (both solvers)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: restitution rebounds (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        PhysicsWorld w(MakeWorldDef(kind, Real(400)));
        AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

        const Real r = Real(2);
        const Real startY = Real(-40);
        BodyHandle ball = AddCircle(w, Vec2(Real(0), startY), r,
                                    /*density=*/Real(1), /*friction=*/Real(0.1),
                                    /*restitution=*/Real(0.8));

        const Real restY = -r;
        const Real dropHeight = std::abs(startY - restY);

        Real apexY = startY;
        bool hitFloor = false;
        for (int k = 0; k < 240; ++k)
        {
            w.Step(kStep);
            const Real y = w.Position(ball).y;
            if ((y + r) > Real(-0.5))
            {
                hitFloor = true;
            }
            if (hitFloor && y < apexY)
            {
                apexY = y;
            }
        }

        REQUIRE(hitFloor);
        const Real reboundHeight = std::abs(apexY - restY);
        REQUIRE(reboundHeight > Real(0.3) * dropHeight); // rebounds a good fraction
        REQUIRE(reboundHeight < dropHeight + Real(1.0)); // no energy gain
    }
}

// ---------------------------------------------------------------------------
// Friction stops a sliding box (both solvers)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: friction stops a sliding box (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        PhysicsWorld w(MakeWorldDef(kind, Real(400)));
        AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5), /*friction=*/Real(0.8));

        const Real hw = Real(2), hh = Real(2);
        BodyHandle box = AddBox(w, Vec2(Real(0), Real(-2)), hw, hh,
                                /*density=*/Real(1), /*friction=*/Real(0.8));

        for (int k = 0; k < 60; ++k)
        {
            w.Step(kStep);
        }
        w.SetVelocity(box, Vec2(Real(120), Real(0)));

        Real maxX = w.Position(box).x;
        for (int k = 0; k < 600; ++k)
        {
            w.Step(kStep);
            maxX = std::max(maxX, w.Position(box).x);
        }

        const PenBudget budget = BudgetFor(kind);
        REQUIRE(std::abs(w.Velocity(box).x) < Real(2.0)); // friction stopped it
        REQUIRE(maxX > Real(2.0));                        // it DID slide
        REQUIRE((w.Position(box).y + hh) - Real(0) < budget.ball); // did not sink
    }
}

// ---------------------------------------------------------------------------
// Kinematic pushes dynamic (both solvers)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: kinematic pushes dynamic (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        WorldDef wd;
        wd.solverKind = kind; // gravity 0 -- isolate the push
        PhysicsWorld w(wd);

        const Real hw = Real(2), hh = Real(2);
        BodyDef kdef;
        kdef.type     = BodyType::Kinematic;
        kdef.position = Vec2(Real(-10), Real(0));
        kdef.shape    = MakeAabb(hw, hh);
        BodyHandle plate = w.AddBody(kdef);
        w.SetVelocity(plate, Vec2(Real(60), Real(0)));

        BodyHandle box = AddBox(w, Vec2(Real(0), Real(0)), hw, hh);

        const Real boxX0 = w.Position(box).x;
        for (int k = 0; k < 120; ++k)
        {
            w.Step(kStep);
        }

        const Real t = kStep * Real(120);
        // Kinematic kept its own trajectory (undeflected).
        REQUIRE(w.Position(plate).x == Approx(Real(-10) + Real(60) * t).margin(Real(0.1)));
        REQUIRE(w.Position(plate).y == Approx(Real(0)).margin(Real(1e-3)));
        // Dynamic box was pushed forward (+x) and out of the way.
        REQUIRE(w.Position(box).x > boxX0 + Real(2.0));
        REQUIRE(w.Position(box).x > w.Position(plate).x);
    }
}

// ---------------------------------------------------------------------------
// Determinism per solver: each run twice -> identical state hash
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsBaumgarte: deterministic per solver (A/B)", "[physics][solver][baumgarte]")
{
    for (SolverKind kind : { SolverKind::SoftStep, SolverKind::Baumgarte })
    {
        auto run = [kind](std::vector<Real>& trace)
        {
            PhysicsWorld w(MakeWorldDef(kind, Real(400)));
            AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));
            std::vector<BodyHandle> boxes;
            const Real hw = Real(2), hh = Real(2);
            for (int i = 0; i < 4; ++i)
            {
                boxes.push_back(AddBox(w, Vec2(Real(0.0), -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1)),
                                       hw, hh));
            }
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
}
