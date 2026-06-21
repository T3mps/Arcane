// Physics v2 compound-body behavior guards (Task: compound-COM / multi-fixture
// bug investigation).
//
// REPORTED (interactive sandbox "Compound bodies" / "Mixed shapes"): off-COM
// 2-fixture bodies (a light core circle + a heavy offset circle) looked buggy --
// accelerating while sliding, over-penetrating. Investigation found the Core
// SoftStep solver is ROBUST for compound bodies (it settles them in every
// configuration), and pinned two real defects in the compound/multi-fixture path
// that these tests + PhysicsCompoundComTest (e) guard:
//
//   1. WARM-START ID COLLISION across fixture pairs -- two fixtures of one body
//      that hit the same surface produced the SAME warm-start key and aliased in
//      the solver cache. PROBE below pins that each contact point now owns a
//      distinct key (cache size >= contact-point count).
//   2. ApplyImpulse-at-point torqued about the ORIGIN, not the COM (off-COM
//      bodies span wrong) -- guarded analytically by PhysicsCompoundComTest (e).
//
// The behavioral guards (A/B) pin that compound bodies -- the EXACT sandbox
// "lopsided" body, dropped and free to rotate, including dynamic-on-dynamic
// piles -- DISSIPATE to rest and never inject energy or tunnel the floor.
//
// CONVENTION: Cartesian, +Y DOWN (gravity +Y). PRESENTATION-FREE + C++20-clean.

#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/Fixture.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep    = Real(1) / Real(60);
    constexpr Real kGravity = Real(400);

    // Wide static AABB floor centered at x=0; `top` is the world Y of its top
    // surface. Half-width 560 -> spans x[-560,560], so bodies near x=0 cannot
    // roll off the edge.
    BodyHandle AddFloor(PhysicsWorld& w, Real top, Real halfWidth = Real(560))
    {
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(Real(0), top + Real(20));
        bd.shape    = MakeAabb(halfWidth, Real(20));
        bd.friction = Real(0.5);
        return w.AddBody(bd);
    }

    void AddWall(PhysicsWorld& w, Real cx, Real cy, Real hw, Real hh)
    {
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(cx, cy);
        bd.shape    = MakeAabb(hw, hh);
        bd.friction = Real(0.5);
        w.AddBody(bd);
    }

    Real KineticEnergy(const PhysicsWorld& w, BodyHandle b)
    {
        const Vec2 v  = w.Velocity(b);
        const Real wv = w.AngVelSlot(b.index);
        return Real(0.5) * w.GetBodyMass(b) * (v.x * v.x + v.y * v.y)
             + Real(0.5) * w.GetBodyInertia(b) * (wv * wv);
    }

    // EXACT sandbox "Compound bodies" body (Scenes.cpp makeLopsided): a light
    // core circle (r18, density 0.5) at the body origin + a heavy circle (r22,
    // density 4.0) at local (heavySign*40, 0). Dynamic, free to rotate, rest 0,
    // friction 0.5. The aggregate COM sits at local x ~ +36.9*heavySign.
    BodyHandle MakeLopsided(PhysicsWorld& w, Vec2 pos, Real heavySign)
    {
        BodyDef bd;
        bd.type          = BodyType::Dynamic;
        bd.position      = pos;
        bd.shape         = MakeCircle(Real(18));
        bd.density       = Real(0.5);
        bd.friction      = Real(0.5);
        bd.restitution   = Real(0);
        bd.fixedRotation = false;
        const BodyHandle bh = w.AddBody(bd);

        FixtureDef fd;
        fd.shape       = MakeCircle(Real(22));
        fd.localPos    = Vec2(heavySign * Real(40), Real(0));
        fd.density     = Real(4.0);
        fd.friction    = Real(0.5);
        fd.restitution = Real(0);
        w.AddFixture(bh, fd);
        return bh;
    }
} // namespace

// ============================================================================
// PROBE [FIX 1 GUARD]: two-fixture contacts own DISTINCT warm-start ids.
//
// A 2-fixture body whose two fixtures both rest on the floor produces two
// contact points. The solver warm-start cache is keyed by the contact point id;
// before the fix both points shared one id (the geometric feature id carried no
// fixture identity) and aliased -- the cache held FEWER entries than there were
// contact points. After the fix each fixture pair's contact owns a distinct key.
// ============================================================================
TEST_CASE("compound-slide PROBE: two-fixture contacts have distinct warm-start ids",
          "[physics]")
{
    WorldDef wd;
    wd.gravityY = kGravity;
    PhysicsWorld w(wd);

    AddFloor(w, Real(620));

    // A flat 2-circle body (both r=12) straddling so BOTH circles rest on the
    // floor: fixture0 at origin, fixture1 at +40. Centers at y=620-12=608.
    BodyDef bd;
    bd.type          = BodyType::Dynamic;
    bd.position      = Vec2(Real(-20), Real(608));
    bd.shape         = MakeCircle(Real(12));
    bd.density       = Real(1);
    bd.friction      = Real(0.5);
    bd.fixedRotation = true;   // keep it flat so both circles stay on the floor
    BodyHandle body  = w.AddBody(bd);
    {
        FixtureDef fd;
        fd.shape    = MakeCircle(Real(12));
        fd.localPos = Vec2(Real(40), Real(0));
        fd.density  = Real(1);
        fd.friction = Real(0.5);
        w.AddFixture(body, fd);
    }

    for (int i = 0; i < 30; ++i) w.Step(kStep);

    const std::size_t activeContacts = w.ActiveContactCount();
    const std::size_t cacheSize      = w.SolverWarmStartCacheSize();
    INFO("active contact constraints = " << activeContacts
         << ", warm-start cache size = " << cacheSize);

    CHECK(activeContacts >= 2u);   // both circles rest on the floor
    // Each distinct contact point owns a distinct warm-start id (no aliasing).
    CHECK(cacheSize >= 2u);
}

// ============================================================================
// (A) DROPPED lopsided bodies SETTLE (no energy injection, no tunnelling). Two
//     of the EXACT sandbox compound bodies (one tipping each way) are dropped on
//     a floor, free to rotate. A passive (restitution-0) scene is energy-
//     DISSIPATIVE: after the impact transient each body comes to rest. A solver
//     that injected energy (the reported "accelerate") would keep them moving.
// ============================================================================
TEST_CASE("compound-slide (A): dropped lopsided bodies settle to rest", "[physics]")
{
    WorldDef wd;
    wd.gravityY = kGravity;
    PhysicsWorld w(wd);

    AddFloor(w, Real(620));
    // Spaced so the heavy lobes (centers -50 / +50, r22) do not overlap, and well
    // inside the floor span so a rolling body cannot fall off the edge.
    BodyHandle b0 = MakeLopsided(w, Vec2(-Real(90), Real(360)),  Real(1)); // tips right
    BodyHandle b1 = MakeLopsided(w, Vec2( Real(90), Real(360)), -Real(1)); // tips left

    Real impactPeak = Real(0);
    for (int i = 0; i < 90; ++i)   // fall + impact + initial tip
    {
        w.Step(kStep);
        impactPeak = std::max(impactPeak,
                              KineticEnergy(w, b0) + KineticEnergy(w, b1));
    }
    REQUIRE(impactPeak > Real(0)); // the scene actually moved (guards a no-op test)

    Real lateMax = Real(0);
    for (int i = 90; i < 540; ++i) // 7.5 s to dissipate
    {
        w.Step(kStep);
        lateMax = std::max(lateMax, KineticEnergy(w, b0) + KineticEnergy(w, b1));
    }
    const Real finalKE = KineticEnergy(w, b0) + KineticEnergy(w, b1);
    INFO("impact-peak KE = " << static_cast<double>(impactPeak)
         << ", late-max KE = " << static_cast<double>(lateMax)
         << ", final KE = " << static_cast<double>(finalKE));

    // Energy never climbs back above the impact transient (no injection)...
    CHECK(lateMax <= impactPeak * Real(1.05));
    // ...and the pair dissipates essentially to rest.
    CHECK(finalKE < impactPeak * Real(0.02));
    // Neither body tunnelled the floor or flew away (stayed in the play area).
    CHECK(w.Position(b0).y < Real(700));
    CHECK(w.Position(b1).y < Real(700));
}

// ============================================================================
// (B) CONFINED HEAP of free-rotating compound bodies dissipates. Six lopsided
//     bodies packed between two walls cannot separate -- they pile and slide on
//     each other in sustained multi-fixture contact (the pure-Core analog of
//     "compound bodies sliding on top of each other"). A passive heap MUST reach
//     rest; energy that kept growing here is the reported instability.
// ============================================================================
TEST_CASE("compound-slide (B): confined heap of compound bodies dissipates",
          "[physics]")
{
    WorldDef wd;
    wd.gravityY = kGravity;
    PhysicsWorld w(wd);

    AddFloor(w, Real(620));
    AddWall(w, -Real(90), Real(500), Real(20), Real(140)); // left wall
    AddWall(w,  Real(90), Real(500), Real(20), Real(140)); // right wall

    std::vector<BodyHandle> bodies;
    for (int i = 0; i < 6; ++i)
    {
        const Real sign = (i % 2 == 0) ? Real(1) : -Real(1);
        const Real x    = Real(-20 + (i % 3) * 20);
        const Real y    = Real(420 - 55 * i);
        bodies.push_back(MakeLopsided(w, Vec2(x, y), sign));
    }
    auto totalKE = [&] {
        Real s = Real(0);
        for (BodyHandle b : bodies) s += KineticEnergy(w, b);
        return s;
    };

    // Settle (5 s), then assert it stays at rest over the next 5 s -- a solver
    // that injects energy in confined multi-fixture contact would re-energize.
    for (int i = 0; i < 300; ++i) w.Step(kStep);
    Real lateMax = Real(0);
    for (int i = 0; i < 300; ++i)
    {
        w.Step(kStep);
        lateMax = std::max(lateMax, totalKE());
    }
    INFO("confined-heap late-max KE = " << static_cast<double>(lateMax));

    // The whole heap is at rest (tiny per-body residual at most).
    CHECK(lateMax < Real(50));
    // No body escaped the bowl / tunnelled the floor.
    for (BodyHandle b : bodies)
        CHECK(w.Position(b).y < Real(700));
}
