// SoftStep solver resting-penetration + stability BUDGET tests.
//
// SoftStep (Solver/SoftStep.cpp) is THE constraint solver -- the Box2D-v3 TGS
// Soft modernization centerpiece. This file runs the SAME behavioral STABILITY
// invariants as PhysicsSolverTest.cpp against it: a ball rests on a floor, a
// stack settles with bounded penetration, a high mass-ratio pair stays stable, a
// settling scene's energy stays bounded, a bouncy ball rebounds, friction stops
// a slide, a kinematic body pushes a dynamic one, and the solver is
// deterministic across two runs -- each with an explicit resting-PENETRATION
// BUDGET on the contact-heavy scenes. The point is that SoftStep is STABLE +
// bounded, so the thresholds are SoftStep-appropriate.
//
// HISTORY: this file was the P2.3 A/B cross-check -- it ran these same scenes
// against BOTH SoftStep and the Baumgarte PGS oracle (parameterized by
// WorldDef::solverKind). The oracle was retired 2026-07-03 (MKS P2): nothing
// selected it in production and SoftStep's own coverage exceeds it. The scenes +
// SoftStep budgets survive here as the single-solver budget gate.
//
// PENETRATION BUDGETS (SoftStep): Box2D-v3 soft contacts drive overlap toward
// kLinearSlop (0.005); measured resting penetration is crisp. MEASURED (f32,
// 60 Hz): ball pen ~0.02 (assert < 0.1), stack ~0.18 (< 0.21), massratio ~0.56
// (< 0.7). All FINITE + non-exploding. The stack budget is 0.21 -- tightened
// from 0.22 for the v2 Collide narrowphase (the T3 clip-manifold path reports
// marginally different depths than the old SAT dispatch; 0.21 gives ~5% headroom
// over measured 0.2003; behavioral invariants unchanged).
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

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // A WorldDef with +Y-down gravity (SoftStep is the only solver).
    WorldDef MakeWorldDef(Real gravityY)
    {
        WorldDef wd;
        wd.gravityY   = gravityY;
        wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
        wd.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
        wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
        wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
        wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
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

    // SoftStep resting-penetration budget (see the header block above).
    struct PenBudget
    {
        Real ball;
        Real stack;
        Real massRatio;
    };
    PenBudget Budget()
    {
        // SoftStep: crisp soft contacts (the PhysicsSolverTest budgets).
        return PenBudget{ Real(0.1), Real(0.21), Real(0.7) };
    }
}

// ---------------------------------------------------------------------------
// Ball rests on a floor (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: ball rests on floor", "[physics][solver][budget]")
{
    PhysicsWorld w(MakeWorldDef(Real(400)));
    AddFloor(w, Vec2(Real(0), Real(5)), Real(50), Real(5));

    const Real r = Real(2);
    BodyHandle ball = AddCircle(w, Vec2(Real(0), Real(-20)), r);

    for (int k = 0; k < 240; ++k)
    {
        w.Step(kStep);
    }

    const Vec2 p = w.Position(ball);
    const Real floorTop = Real(0);
    const PenBudget budget = Budget();

    // Rests ON TOP of the floor (within the penetration budget).
    REQUIRE(p.y == Approx(floorTop - r).margin(budget.ball + Real(0.1)));
    const Real penetration = (p.y + r) - floorTop;
    REQUIRE(penetration < budget.ball);
    REQUIRE(penetration > Real(-0.5)); // not floating well above the floor
    REQUIRE(Speed(w, ball) < Real(1.0));
    REQUIRE(p.x == Approx(Real(0)).margin(Real(0.5)));
}

// ---------------------------------------------------------------------------
// Stack of N boxes settles (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: box stack settles", "[physics][solver][budget]")
{
    PhysicsWorld w(MakeWorldDef(Real(400)));
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

    const PenBudget budget = Budget();
    REQUIRE(maxSpeed < Real(2.0));      // settled, no jitter/explosion
    REQUIRE(maxPen   < budget.stack);   // bounded overlap (SoftStep-appropriate)
    for (int i = 0; i < N; ++i)
    {
        REQUIRE(w.Position(boxes[i]).x == Approx(Real(0)).margin(Real(0.5)));
        REQUIRE(std::isfinite(w.Position(boxes[i]).y));
    }
}

// ---------------------------------------------------------------------------
// High mass-ratio pair is stable (SoftStep)
//
// The 100:1 mass ratio is a SoftStep showcase -- its soft contacts + the
// contactPushMaxVelocity clamp tame it at the default 4 substeps.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: high mass-ratio stable", "[physics][solver][budget]")
{
    PhysicsWorld w(MakeWorldDef(Real(400)));
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
    const PenBudget budget = Budget();
    REQUIRE(lightPen < budget.massRatio); // bounded under the 100:1 load
    REQUIRE(std::isfinite(w.Position(heavy).y));
    REQUIRE(std::isfinite(w.Position(light).y));
}

// ---------------------------------------------------------------------------
// Energy bounded (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: energy bounded", "[physics][solver][budget]")
{
    PhysicsWorld w(MakeWorldDef(Real(400)));
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

// ---------------------------------------------------------------------------
// Restitution rebounds (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: restitution rebounds", "[physics][solver][budget]")
{
    PhysicsWorld w(MakeWorldDef(Real(400)));
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

// ---------------------------------------------------------------------------
// Friction stops a sliding box (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: friction stops a sliding box", "[physics][solver][budget]")
{
    PhysicsWorld w(MakeWorldDef(Real(400)));
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

    const PenBudget budget = Budget();
    REQUIRE(std::abs(w.Velocity(box).x) < Real(2.0)); // friction stopped it
    REQUIRE(maxX > Real(2.0));                        // it DID slide
    REQUIRE((w.Position(box).y + hh) - Real(0) < budget.ball); // did not sink
}

// ---------------------------------------------------------------------------
// Kinematic pushes dynamic (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: kinematic pushes dynamic", "[physics][solver][budget]")
{
    WorldDef wd; // gravity 0 -- isolate the push
    wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.gravityY               = Real(0);   // PX-PIN: remove when this file converts to MKS
    wd.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
    wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
    wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
    wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
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

// ---------------------------------------------------------------------------
// Determinism: run twice -> identical state trace (SoftStep)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsSolverBudget: deterministic across two runs", "[physics][solver][budget]")
{
    auto run = [](std::vector<Real>& trace)
    {
        PhysicsWorld w(MakeWorldDef(Real(400)));
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
