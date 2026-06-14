// Physics M6 P3.1: speculative-contact CCD + GJK-TOI bullet clamp.
//
// PORT NOTE: a BEHAVIORAL port of the physics_harness CCD block
// (Client/src/tests/physics_harness/main.lua ~661-683) reformulated to plain
// CARTESIAN. The harness asserts: an UNFLAGGED kinematic fast body TUNNELS a
// thin wall in one step (plain integration -- the expected baseline, since
// kinematics are script-driven), while a `bullet`-flagged kinematic body CLAMPS
// to time-of-impact against the wall (stops well before the destination, NOT
// buried in the wall span). P3.1 adds the MODERN speculative-contact CCD for
// fast DYNAMIC movers (the velocity-scaled speculative margin in
// GenerateContacts + the SoftStep solver's s > 0 bias), so we also assert a
// fast dynamic body does NOT tunnel a thin static wall.
//
// Two mechanisms under test:
//   A. Speculative contacts (fast DYNAMIC): the per-body speculative margin is
//      max(kSkin, |v| * dt); the solver's speculative bias caps the per-sub-step
//      advance to the gap so the body never crosses the wall. (Slow bodies use
//      kSkin, so resting/settling is unchanged -- covered by the dynamics/solver
//      suites, not regressed here.)
//   B. GJK-TOI bullet clamp (Step stage 6): an isBullet body's prev->curr sweep
//      vs statics; clamp to TOI. KINEMATIC bullets are the concrete harness
//      test (the solver never touches them); DYNAMIC bullets get it as a backup.
//
// INVARIANTS asserted:
//   * Fast dynamic body does not tunnel (final pos on the near side, not buried,
//     not beyond the wall).
//   * Unflagged kinematic fast body tunnels (expected baseline).
//   * Bullet kinematic body clamps to TOI (stops short, not buried).
//   * Bullet dynamic body does not tunnel.
//   * Deterministic: a CCD scene run twice yields identical state.
//
// PRESENTATION-FREE + C++20-clean.

#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // A thin static wall AABB at x = kWallX, spanning a tall band in y. "Thin"
    // (half-width 1 -> 2 units wide) is what makes tunneling possible without
    // CCD: a fast body's one-step displacement steps clean over it.
    constexpr Real kWallX  = Real(100);
    constexpr Real kWallHW = Real(1);   // half-width -> span x[99, 101]
    constexpr Real kWallHH = Real(50);  // half-height -> span y[-50, 50]

    // Add the thin static wall body.
    BodyHandle AddWall(PhysicsWorld& w)
    {
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(kWallX, Real(0));
        bd.shape    = MakeAabb(kWallHW, kWallHH);
        return w.AddBody(bd);
    }

    // The moving body's circle radius (matches the harness's tiny CCD probe).
    constexpr Real kProbeR = Real(2);

    // A fast mover: starts left of the wall, travels far PAST it in one step.
    // Start x = 0; one-step displacement = kSpeed * kStep. With kSpeed chosen so
    // the displacement (~200) clears the 2-wide wall by a wide margin.
    constexpr Real kSpeed = Real(200) / kStep; // displacement == 200 per step

    // Is the moving body buried in the wall span at (x,y)? Penetration deeper
    // than -0.5 (the harness's "buried" threshold) means it tunneled INTO the
    // wall rather than stopping at its surface.
    bool BuriedInWall(PhysicsWorld& w, BodyHandle h)
    {
        const Shape* s = w.GetShape(h);
        const Vec2 p = w.Position(h);
        const Aabb box = s->ComputeAABB(Transform{ p, Real(0) });
        // The wall span x[99,101], y[-50,50]. The probe overlaps it badly iff its
        // AABB center is inside the wall span by more than 0.5 on the x axis.
        const Real cx = (box.min.x + box.max.x) * Real(0.5);
        return cx > kWallX - kWallHW + Real(0.5) &&
               cx < kWallX + kWallHW - Real(0.5);
    }
}

// ---------------------------------------------------------------------------
// A. Speculative contacts: a fast DYNAMIC body does not tunnel a thin wall.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsCcd: a fast dynamic body does not tunnel a thin static wall "
          "(speculative)", "[physics][ccd]")
{
    WorldDef wd; // gravity 0 -- the body moves purely on its set velocity
    PhysicsWorld w(wd);
    AddWall(w);

    BodyDef bd;
    bd.type     = BodyType::Dynamic;
    bd.position = Vec2(Real(0), Real(0));
    bd.shape    = MakeCircle(kProbeR);
    bd.density  = Real(1);
    BodyHandle body = w.AddBody(bd);

    // Fire it straight at the wall fast enough to clear the 2-wide wall in one
    // step's worth of plain integration (displacement ~200, wall ~2 wide).
    w.SetVelocity(body, Vec2(kSpeed, Real(0)));
    w.Step(kStep);

    const Vec2 p = w.Position(body);
    // The speculative margin + solver bias must keep the body on the NEAR side
    // of the wall (it did not pass through to x ~ 200).
    REQUIRE(p.x < kWallX);
    // And it is not buried in the wall span (stopped at the surface, not inside).
    REQUIRE_FALSE(BuriedInWall(w, body));
}

// ---------------------------------------------------------------------------
// B (baseline). An UNFLAGGED kinematic fast body TUNNELS (expected). This
// documents that the bullet flag is REQUIRED for kinematic CCD -- kinematics
// are script-driven, so plain integration steps clean over a thin wall.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsCcd: an unflagged kinematic fast body tunnels a thin wall "
          "(expected baseline)", "[physics][ccd]")
{
    PhysicsWorld w; // no gravity needed
    AddWall(w);

    BodyDef bd;
    bd.type     = BodyType::Kinematic;
    bd.position = Vec2(Real(0), Real(0));
    bd.shape    = MakeCircle(kProbeR);
    bd.bullet   = false; // UNFLAGGED -> no CCD
    BodyHandle body = w.AddBody(bd);

    w.SetVelocity(body, Vec2(kSpeed, Real(0)));
    w.Step(kStep);

    const Vec2 p = w.Position(body);
    // Plain integration: the body lands at its full one-step displacement (~200),
    // having passed THROUGH the wall (this is the documented baseline behavior).
    REQUIRE(p.x == Approx(Real(200)).margin(Real(0.01)));
    REQUIRE(p.x > kWallX); // beyond the wall
}

// ---------------------------------------------------------------------------
// B. A BULLET kinematic body does NOT tunnel -- it clamps to TOI (the harness's
// concrete CCD assertion, lines 669-683).
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsCcd: a bullet kinematic body clamps to TOI against a thin wall",
          "[physics][ccd]")
{
    PhysicsWorld w;
    AddWall(w);

    BodyDef bd;
    bd.type     = BodyType::Kinematic;
    bd.position = Vec2(Real(0), Real(0));
    bd.shape    = MakeCircle(kProbeR);
    bd.bullet   = true; // FLAGGED -> CCD bullet clamp
    BodyHandle body = w.AddBody(bd);

    w.SetVelocity(body, Vec2(kSpeed, Real(0)));
    w.Step(kStep);

    const Vec2 p = w.Position(body);
    // Stops WELL before the destination (~200): clamped to TOI at the wall's near
    // face (x=99) minus the probe radius (~97).
    REQUIRE(p.x < kWallX);                 // before the wall
    REQUIRE(p.x == Approx(kWallX - kWallHW - kProbeR).margin(Real(0.5)));
    // And it is not buried in the wall span.
    REQUIRE_FALSE(BuriedInWall(w, body));
}

// ---------------------------------------------------------------------------
// B (backup). A BULLET dynamic body does NOT tunnel a thin wall.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsCcd: a bullet dynamic body does not tunnel a thin wall",
          "[physics][ccd]")
{
    WorldDef wd; // gravity 0
    PhysicsWorld w(wd);
    AddWall(w);

    BodyDef bd;
    bd.type     = BodyType::Dynamic;
    bd.position = Vec2(Real(0), Real(0));
    bd.shape    = MakeCircle(kProbeR);
    bd.density  = Real(1);
    bd.bullet   = true; // both speculative + the TOI backup apply
    BodyHandle body = w.AddBody(bd);

    w.SetVelocity(body, Vec2(kSpeed, Real(0)));
    w.Step(kStep);

    const Vec2 p = w.Position(body);
    REQUIRE(p.x < kWallX);                 // on the near side
    REQUIRE_FALSE(BuriedInWall(w, body));  // not buried in the wall span
}

// ---------------------------------------------------------------------------
// Determinism: a CCD scene run twice yields identical final state.
// ---------------------------------------------------------------------------

TEST_CASE("PhysicsCcd: a CCD scene is deterministic (run twice -> identical)",
          "[physics][ccd]")
{
    // Build + step an identical CCD scene twice, capturing the final positions
    // of a kinematic bullet, a dynamic mover, and a dynamic bullet; the two runs
    // must match BIT-for-bit (no wall-clock, fixed-iteration conservative
    // advancement, index-ordered Step).
    auto run = [](std::vector<Vec2>& out)
    {
        WorldDef wd;
        PhysicsWorld w(wd);
        AddWall(w);

        BodyDef kb;
        kb.type     = BodyType::Kinematic;
        kb.position = Vec2(Real(0), Real(-10));
        kb.shape    = MakeCircle(kProbeR);
        kb.bullet   = true;
        BodyHandle kbh = w.AddBody(kb);

        BodyDef dyn;
        dyn.type     = BodyType::Dynamic;
        dyn.position = Vec2(Real(0), Real(0));
        dyn.shape    = MakeCircle(kProbeR);
        dyn.density  = Real(1);
        BodyHandle dynh = w.AddBody(dyn);

        BodyDef db;
        db.type     = BodyType::Dynamic;
        db.position = Vec2(Real(0), Real(10));
        db.shape    = MakeCircle(kProbeR);
        db.density  = Real(1);
        db.bullet   = true;
        BodyHandle dbh = w.AddBody(db);

        w.SetVelocity(kbh, Vec2(kSpeed, Real(0)));
        w.SetVelocity(dynh, Vec2(kSpeed, Real(0)));
        w.SetVelocity(dbh, Vec2(kSpeed, Real(0)));

        for (int i = 0; i < 8; ++i)
        {
            w.Step(kStep);
        }

        out.clear();
        out.push_back(w.Position(kbh));
        out.push_back(w.Position(dynh));
        out.push_back(w.Position(dbh));
    };

    std::vector<Vec2> a, b;
    run(a);
    run(b);

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        REQUIRE(a[i].x == b[i].x); // bit-identical (determinism)
        REQUIRE(a[i].y == b[i].y);
    }
}
