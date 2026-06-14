// Physics M6 P2.1: dynamic-body velocity integration (NO solver yet).
//
// PORT NOTE: P2.1 OPENS Phase 2 (dynamics). It extends PhysicsWorld with the
// dynamics SoA (mass/invMass/invInertia, angle/angVel, restitution/friction/
// damping/sleep) + AddBody dynamics params + the Step velocity/position
// integration stages + the Body dynamics accessors. There is NO solver and NO
// collision response in P2.1 (those are P2.2). So these tests are PURE
// integration: a dynamic body free-falls under constant gravity per the
// SEMI-IMPLICIT (symplectic) EULER scheme the Lua step() implements
// (PhysicsWorld.lua:302-310 velocity-integrate, then 393-401 position-integrate
// using the NEW velocity).
//
// ANALYTIC reference (constant gravity g, start at rest v0=0, pos0):
//   per step k: v_k = v_{k-1} + g*dt ; pos_k = pos_{k-1} + v_k*dt
//   closed form: v_k   = g*dt*k
//                pos_k = pos0 + g*dt^2 * k(k+1)/2
// (This is semi-implicit Euler: velocity first, then position uses it -- so
//  the sum runs i=1..k, NOT i=0..k-1 as explicit Euler would.)
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

// The solver scaffolding header must compile + the interface must exist (it is
// NOT called in P2.1 -- this is a presence + design-anchor check).
#include <Arcane/Physics/Solver/Solver.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // Build a world with gravity and one free-falling dynamic circle at origin.
    BodyHandle AddFallingBody(PhysicsWorld& w, Vec2 pos = Vec2(Real(0), Real(0)))
    {
        BodyDef def;
        def.type     = BodyType::Dynamic;
        def.position = pos;
        def.shape    = MakeCircle(Real(1));
        def.density  = Real(1);
        return w.AddBody(def);
    }
}

// ---------------------------------------------------------------------------
// Free-fall under constant gravity matches the semi-implicit Euler closed form
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: free-fall matches semi-implicit Euler", "[physics][dynamics]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    const Vec2 pos0(Real(10), Real(-5));
    BodyHandle h = AddFallingBody(w, pos0);

    const Real g  = Real(400);
    const Real dt = kStep;

    // Body starts at rest, awake, at pos0.
    REQUIRE(w.GetType(h) == BodyType::Dynamic);
    REQUIRE(w.IsAwake(h));
    REQUIRE(w.Velocity(h).x == Approx(Real(0)));
    REQUIRE(w.Velocity(h).y == Approx(Real(0)));

    const int kN = 30;
    for (int k = 1; k <= kN; ++k)
    {
        w.Step(dt);

        // v_k = g*dt*k ; pos_k = pos0 + g*dt^2 * k(k+1)/2
        const Real expV   = g * dt * static_cast<Real>(k);
        const Real expPos = pos0.y + g * dt * dt
                          * (static_cast<Real>(k) * static_cast<Real>(k + 1) * Real(0.5));

        const Vec2 v = w.Velocity(h);
        const Vec2 p = w.Position(h);

        // No horizontal force -> x stays put; gravity acts on y only.
        REQUIRE(v.x == Approx(Real(0)).margin(Real(1e-4)));
        REQUIRE(p.x == Approx(pos0.x).margin(Real(1e-3)));

        REQUIRE(v.y == Approx(expV).margin(Real(1e-3) + std::abs(expV) * Real(1e-4)));
        // generous margin: semi-implicit Euler position error accumulates ~O(k*dt^2)
        REQUIRE(p.y == Approx(expPos).margin(Real(1e-2) + std::abs(expPos) * Real(1e-4)));
    }
}

// ---------------------------------------------------------------------------
// prev/curr snapshot still drives DrawPosition for a falling body
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: DrawPosition lerps the fall", "[physics][dynamics]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    BodyHandle h = AddFallingBody(w);
    w.Step(kStep); // one step: prev = start (0), curr = first integrated pos

    const Vec2 prev = w.DrawPosition(h, Real(0)); // alpha 0 -> prev snapshot
    const Vec2 curr = w.DrawPosition(h, Real(1)); // alpha 1 -> current pos
    const Vec2 mid  = w.DrawPosition(h, Real(0.5));

    REQUIRE(prev.y == Approx(Real(0)).margin(Real(1e-5)));
    REQUIRE(curr.y == Approx(w.Position(h).y));
    REQUIRE(mid.y  == Approx((prev.y + curr.y) * Real(0.5)));
    REQUIRE(curr.y > prev.y); // fell downward (positive gravity)
}

// ---------------------------------------------------------------------------
// Linear damping reduces velocity vs the undamped fall (exact recurrence ref)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: linear damping decays velocity", "[physics][dynamics]")
{
    const Real g  = Real(400);
    const Real dt = kStep;
    const Real damp = Real(2);

    WorldDef wd;
    wd.gravityY = g;

    // Damped body.
    PhysicsWorld wDamp(wd);
    BodyDef ddef;
    ddef.type          = BodyType::Dynamic;
    ddef.shape         = MakeCircle(Real(1));
    ddef.linearDamping = damp;
    BodyHandle hd = wDamp.AddBody(ddef);

    // Undamped body (same gravity, no damping) -- the comparison baseline.
    PhysicsWorld wPlain(wd);
    BodyHandle hp = AddFallingBody(wPlain);

    // Analytic damped recurrence: each step v = (v + g*dt) * 1/(1 + damp*dt).
    Real refV = Real(0);
    const Real f = Real(1) / (Real(1) + damp * dt);

    // Track the recurrence for the first 60 steps (still far from terminal).
    const int kN = 60;
    for (int k = 1; k <= kN; ++k)
    {
        wDamp.Step(dt);
        wPlain.Step(dt);
        refV = (refV + g * dt) * f;

        const Real vDamp  = wDamp.Velocity(hd).y;
        const Real vPlain = wPlain.Velocity(hp).y;

        // Damped velocity tracks the closed-form recurrence.
        REQUIRE(vDamp == Approx(refV).margin(Real(1e-3) + std::abs(refV) * Real(1e-4)));
        // Damping always leaves the body slower than the undamped baseline.
        REQUIRE(vDamp < vPlain);
    }

    // The discrete fixed point of v = (v + g*dt)*f is EXACTLY g/damp (the
    // continuous terminal velocity): v(1-f) = g*dt*f, and 1-f = d*dt*f, so
    // v = g/d. Convergence is geometric in f (~0.968 here), so step long enough
    // for the transient to die out before asserting it.
    for (int k = 0; k < 400; ++k)
    {
        wDamp.Step(dt);
    }
    const Real terminal = g / damp;
    REQUIRE(wDamp.Velocity(hd).y == Approx(terminal).margin(terminal * Real(0.01)));
}

// ---------------------------------------------------------------------------
// ApplyImpulse changes velocity by impulse * invMass and wakes the body
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: ApplyImpulse adds impulse/mass and wakes", "[physics][dynamics]")
{
    PhysicsWorld w; // gravity 0

    BodyDef def;
    def.type    = BodyType::Dynamic;
    def.shape   = MakeCircle(Real(1));
    def.density = Real(1);
    BodyHandle h = w.AddBody(def);

    // mass = density * pi * r^2 = pi ; invMass = 1/pi.
    const Real mass    = Real(3.14159265358979323846) * Real(1) * Real(1);
    const Real invMass = Real(1) / mass;

    REQUIRE(w.Velocity(h).x == Approx(Real(0)));

    const Vec2 impulse(Real(10), Real(-4));
    w.ApplyImpulse(h, impulse);

    REQUIRE(w.Velocity(h).x == Approx(impulse.x * invMass).margin(Real(1e-4)));
    REQUIRE(w.Velocity(h).y == Approx(impulse.y * invMass).margin(Real(1e-4)));
    REQUIRE(w.IsAwake(h));

    // Off-center impulse imparts angular velocity: angVel += cross(r, i)*invI.
    const Real angBefore = w.GetAngle(h);
    // Body at origin; apply at world point (0, 1) with a +x impulse -> negative
    // z torque (cross((0,1),(j,0)) = 0*0 - 1*j = -j). With invInertia > 0 this
    // changes the body's angle after a Step.
    w.ApplyImpulse(h, Vec2(Real(5), Real(0)), Vec2(Real(0), Real(1)));
    w.Step(kStep);
    REQUIRE(w.GetAngle(h) != Approx(angBefore)); // rotated (invInertia > 0)
}

// ---------------------------------------------------------------------------
// Static / kinematic bodies are UNAFFECTED by gravity (no dynamics regression)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: static + kinematic ignore gravity", "[physics][dynamics]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    BodyDef sdef;
    sdef.type     = BodyType::Static;
    sdef.position = Vec2(Real(0), Real(0));
    sdef.shape    = MakeAabb(Real(2), Real(2));
    BodyHandle hs = w.AddBody(sdef);

    BodyDef kdef;
    kdef.type     = BodyType::Kinematic;
    kdef.position = Vec2(Real(5), Real(0));
    kdef.shape    = MakeCircle(Real(1));
    BodyHandle hk = w.AddBody(kdef);
    w.SetVelocity(hk, Vec2(Real(3), Real(0))); // pure horizontal kinematic motion

    for (int k = 0; k < 20; ++k)
    {
        w.Step(kStep);
    }

    // Static never moves; gravity does not touch it.
    REQUIRE(w.Position(hs).x == Approx(Real(0)));
    REQUIRE(w.Position(hs).y == Approx(Real(0)));

    // Kinematic integrates ONLY its own velocity -- no gravity on y.
    REQUIRE(w.Position(hk).y == Approx(Real(0)).margin(Real(1e-4)));
    REQUIRE(w.Position(hk).x == Approx(Real(5) + Real(3) * kStep * Real(20)).margin(Real(1e-3)));
    REQUIRE(w.Velocity(hk).y == Approx(Real(0)));
}

// ---------------------------------------------------------------------------
// mass override scales mass + inertia (ports addBody lines 241-243)
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: mass override sets invMass", "[physics][dynamics]")
{
    PhysicsWorld w; // gravity 0

    BodyDef def;
    def.type    = BodyType::Dynamic;
    def.shape   = MakeCircle(Real(1));
    def.density = Real(1);
    def.mass    = Real(2); // override: invMass should be 1/2 regardless of density
    BodyHandle h = w.AddBody(def);

    // impulse of magnitude (mass) along x -> velocity exactly 1.
    w.ApplyImpulse(h, Vec2(Real(2), Real(0)));
    REQUIRE(w.Velocity(h).x == Approx(Real(1)).margin(Real(1e-5)));
}

// ---------------------------------------------------------------------------
// Run-twice determinism: identical inputs -> bit-identical trajectory
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsDynamics: run-twice determinism", "[physics][dynamics]")
{
    auto run = [](std::vector<Real>& trace)
    {
        WorldDef wd;
        wd.gravityX = Real(50);
        wd.gravityY = Real(400);
        PhysicsWorld w(wd);

        BodyDef def;
        def.type          = BodyType::Dynamic;
        def.position      = Vec2(Real(1), Real(2));
        def.shape         = MakeCircle(Real(1));
        def.density       = Real(1.5);
        def.linearDamping = Real(0.3);
        BodyHandle h = w.AddBody(def);
        w.ApplyImpulse(h, Vec2(Real(7), Real(-3)));

        trace.clear();
        for (int k = 0; k < 40; ++k)
        {
            w.Step(kStep);
            const Vec2 p = w.Position(h);
            const Vec2 v = w.Velocity(h);
            trace.push_back(p.x);
            trace.push_back(p.y);
            trace.push_back(v.x);
            trace.push_back(v.y);
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
