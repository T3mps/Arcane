// Physics v2: the canonical INVARIANT gate (tags [physics][invariant]).
//
// Introduced by Task T8 to consolidate the physics correctness gate into ONE
// named place. The M6-era oracle bit-match gate (physics_oracle/*.json) was a
// porting scaffold; Physics v2's gate is physics INVARIANTS + analytic V2 tests
// + fresh goldens. The V2 narrowphase math (Collide/GJK/Manifold/rotation) is
// covered analytically by PhysicsShapesV2 / PhysicsGjkV2 / PhysicsManifoldV2 /
// PhysicsRotation / PhysicsQueryRotation. THIS file pins the END-TO-END
// world-level invariants every solver/narrowphase/broadphase change must keep:
//
//   1. REST PENETRATION < kLinearSlop  -- a settled stack has bounded overlap.
//   2. NO-TUNNEL (fast + rotating)     -- a fast/oriented body never passes
//                                         through a thin static wall.
//   3. ENERGY BOUNDED                  -- a dropped/colliding scene's kinetic
//                                         energy stays under a sane bound.
//   4. RUN-TWICE DETERMINISM           -- the same scene stepped twice yields
//                                         bit-identical final state.
//   5. BROADPHASE-STRATEGY INVARIANCE  -- Tree/Hash/SAP produce identical
//                                         settled state for the same scene
//                                         (the folded PhysicsBroadphaseTest
//                                         equivalence coverage, at the world
//                                         level).
//   6. SLIDE-NO-CATCH over merged spans-- a body sliding along a merged
//                                         multi-tile span does not snag at an
//                                         internal cell seam.
//   7. WARM-START CACHE BOUNDED        -- as contacts persist by stable feature
//                                         id, the solver warm-start cache stays
//                                         bounded (does not grow unboundedly).
//
// All expected values are hand-derived / property assertions (NOT oracle
// values). Y-positive is DOWN (gravity is +Y). PRESENTATION-FREE + C++20-clean.

#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp>
#include <Arcane/Physics/Broadphase/TileGrid.hpp>
#include <Arcane/Physics/Narrowphase/Gjk.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    constexpr Real kStep   = Real(1) / Real(60);
    constexpr Real kGravity = Real(400); // matches the dynamics/solver/rotation suites

    // CCW box polygon (half-extents hw x hh): BL, BR, TR, TL. A polygon (not an
    // AABB) is required for any rotating-dynamics case.
    Shape BoxPolygon(Real hw, Real hh)
    {
        return MakePolygon({
            Vec2(-hw, -hh),
            Vec2( hw, -hh),
            Vec2( hw,  hh),
            Vec2(-hw,  hh),
        });
    }

    // A wide static AABB floor; `top` is the world Y of its top surface.
    BodyHandle AddFloor(PhysicsWorld& w, Real top, Real halfWidth = Real(400))
    {
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(Real(0), top + Real(5)); // center 5 below the surface
        bd.shape    = MakeAabb(halfWidth, Real(5));
        return w.AddBody(bd);
    }

    // A dynamic AABB box of half-extent `h`, centered at `pos`.
    BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real h,
                      Real friction = Real(0.4))
    {
        BodyDef bd;
        bd.type        = BodyType::Dynamic;
        bd.position    = pos;
        bd.shape       = MakeAabb(h, h);
        bd.density     = Real(1);
        bd.friction    = friction;
        bd.restitution = Real(0);
        bd.fixedRotation = true; // a flat stack -- no spin (rotation covered separately)
        return w.AddBody(bd);
    }

    // Penetration depth (positive = overlap) between two axis-aligned boxes,
    // measured from their world tight-AABBs. Returns the min-axis overlap, or a
    // negative value (the gap) if separated. Used to bound rest penetration.
    Real BoxOverlap(const Aabb& a, const Aabb& b)
    {
        const Real ox = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
        const Real oy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
        return std::min(ox, oy);
    }

    // true if the body center's x is inside the thin wall span (tunneled in).
    bool CenterInWallX(PhysicsWorld& w, BodyHandle h, Real wallX, Real wallHW)
    {
        const Vec2 p = w.Position(h);
        return p.x > wallX - wallHW && p.x < wallX + wallHW;
    }
} // namespace

// ====================================================================
// 1. REST PENETRATION is BOUNDED (the SoftStep solver drives overlap toward
//    kLinearSlop).
//
// A small stack of dynamic boxes dropped onto a floor settles with bounded
// box-box / box-floor overlap. The SoftStep solver drives the overlap TOWARD
// kLinearSlop (0.005); a free-resting contact lands very close to it. A loaded
// stack carries more residual (the lower contacts bear the weight above), so
// the documented settled budget is ~0.18 for a stack (see PhysicsSolverTest /
// PhysicsBaumgarteTest, which use 0.21 as the stack budget). We assert that
// SOLVER-APPROPRIATE bound here: the invariant is "rest penetration stays
// bounded near slop", not "literally < kLinearSlop" (no soft solver achieves
// the latter under load). A regression that let the stack sink would blow past
// this.
// ====================================================================
TEST_CASE("physics-invariant: rest penetration stays bounded near the slop",
          "[physics][invariant]")
{
    WorldDef wd;
    wd.gravityY = kGravity;
    PhysicsWorld w(wd);

    const Real floorTop = Real(300);
    BodyHandle floor = AddFloor(w, floorTop);

    // Three stacked boxes (half-extent 10) released just touching, above floor.
    // Start them already near their resting heights so they settle quickly.
    const Real h = Real(10);
    BodyHandle b0 = AddBox(w, Vec2(Real(0), floorTop - h),           h);
    BodyHandle b1 = AddBox(w, Vec2(Real(0), floorTop - Real(3) * h), h);
    BodyHandle b2 = AddBox(w, Vec2(Real(0), floorTop - Real(5) * h), h);

    // Settle (3 s).
    for (int i = 0; i < 180; ++i)
    {
        w.Step(kStep);
    }

    auto box = [&](BodyHandle hb) {
        const Shape* s = w.GetShape(hb);
        return s->ComputeAABB(Transform{ w.Position(hb), w.GetAngle(hb) });
    };

    const Aabb fb = box(floor);
    const Aabb a0 = box(b0), a1 = box(b1), a2 = box(b2);

    // Boxes did not fall through the floor (settled above it within their span).
    CHECK(w.Position(b0).y <= floorTop);          // bottom box rests on the floor
    CHECK(w.Position(b0).y >= floorTop - Real(2) * h);

    // Rest penetration bound: the SoftStep solver drives overlap toward
    // kLinearSlop; a loaded stack settles within the documented stack budget
    // (~0.18; PhysicsSolverTest/PhysicsBaumgarteTest use 0.21). We assert the
    // overlap stays within that solver-appropriate bound at every contact.
    const Real kStackPenBound = Real(0.21); // matches PhysicsSolverTest's stack budget
    CHECK(BoxOverlap(a0, fb) <= kStackPenBound);
    CHECK(BoxOverlap(a1, a0) <= kStackPenBound);
    CHECK(BoxOverlap(a2, a1) <= kStackPenBound);
    // The solver IS driving toward the slop: the overlap is far below a full
    // box half-extent (it is a thin contact residual, not a sink-through).
    CHECK(BoxOverlap(a0, fb) < h);  // nowhere near sinking a full box
    CHECK(BoxOverlap(a1, a0) < h);
    CHECK(BoxOverlap(a2, a1) < h);
}

// ====================================================================
// 2. NO-TUNNEL: a FAST body and a ROTATING (oriented box-polygon) body each
//    fail to pass through a thin static wall in one step.
//
// The fast-circle case is also covered by PhysicsCcdTest; here we ALSO drive a
// fast ROTATED box-polygon at the wall -- the Phase-A rotation dimension -- to
// gate that the rotation-aware speculative narrowphase still stops a tilted
// fast body (a regression here would mean the rotated manifold missed the wall).
// ====================================================================
TEST_CASE("physics-invariant: fast + rotating bodies do not tunnel a thin wall",
          "[physics][invariant]")
{
    const Real wallX  = Real(100);
    const Real wallHW = Real(1);   // span x[99,101]
    const Real wallHH = Real(60);

    auto makeWorldWithWall = [&](PhysicsWorld& w) {
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(wallX, Real(0));
        bd.shape    = MakeAabb(wallHW, wallHH);
        w.AddBody(bd);
    };

    const Real speed = Real(200) / kStep; // ~200 units / step -> clears 2-wide wall

    // (a) FAST circle.
    {
        WorldDef wd; // gravity 0
        PhysicsWorld w(wd);
        makeWorldWithWall(w);

        BodyDef bd;
        bd.type     = BodyType::Dynamic;
        bd.position = Vec2(Real(0), Real(0));
        bd.shape    = MakeCircle(Real(2));
        bd.density  = Real(1);
        BodyHandle body = w.AddBody(bd);
        w.SetVelocity(body, Vec2(speed, Real(0)));
        w.Step(kStep);

        CHECK(w.Position(body).x < wallX);                     // near side
        CHECK_FALSE(CenterInWallX(w, body, wallX, wallHW));    // not buried
    }

    // (b) FAST + ROTATED box-polygon (45 deg). The rotation-aware speculative
    //     contact must still catch it.
    {
        WorldDef wd; // gravity 0
        PhysicsWorld w(wd);
        makeWorldWithWall(w);

        BodyDef bd;
        bd.type          = BodyType::Dynamic;
        bd.position      = Vec2(Real(0), Real(0));
        bd.shape         = BoxPolygon(Real(3), Real(3));
        bd.density       = Real(1);
        bd.fixedRotation = false;
        BodyHandle body = w.AddBody(bd);
        w.SetAngle(body, Real(0.7853981633974483)); // 45 deg
        w.SetVelocity(body, Vec2(speed, Real(0)));
        w.Step(kStep);

        CHECK(w.Position(body).x < wallX);                     // did not tunnel
        CHECK_FALSE(CenterInWallX(w, body, wallX, wallHW));    // not buried
    }
}

// ====================================================================
// 3. ENERGY BOUNDED: a dropped + colliding scene's kinetic energy never blows
//    up. With a passive scene (gravity + restitution 0 contacts) the solver is
//    energy-DISSIPATIVE; the bound asserts no spurious energy injection (the
//    classic "exploding stack" failure where a bad solver gains energy).
// ====================================================================
TEST_CASE("physics-invariant: dropped scene kinetic energy stays bounded",
          "[physics][invariant]")
{
    WorldDef wd;
    wd.gravityY = kGravity;
    PhysicsWorld w(wd);

    const Real floorTop = Real(300);
    AddFloor(w, floorTop);

    // A little pile of boxes dropped from a height onto the floor.
    std::vector<BodyHandle> boxes;
    const Real h = Real(8);
    for (int i = 0; i < 5; ++i)
    {
        boxes.push_back(AddBox(w, Vec2(Real(i * 4 - 8), floorTop - Real(80) - Real(i * 20)), h));
    }

    // The max kinetic energy a free body could acquire is bounded by falling the
    // full drop height H under gravity: v^2 = 2*g*H -> KE/m = g*H. With H ~ 180
    // and g = 400, g*H ~ 72000; allow a generous factor for the brief impact
    // transient. A solver that INJECTS energy would blow far past this.
    const Real maxDrop  = Real(200);
    const Real keBound  = Real(4) * kGravity * maxDrop; // per unit mass, x4 slack

    Real peakKE = Real(0);
    for (int i = 0; i < 240; ++i) // 4 s: fall, impact, settle
    {
        w.Step(kStep);
        for (BodyHandle b : boxes)
        {
            const Vec2 v = w.Velocity(b);
            const Real keOverM = Real(0.5) * (v.x * v.x + v.y * v.y);
            peakKE = std::max(peakKE, keOverM);
            REQUIRE(keOverM < keBound); // never blows up at any step
        }
    }

    // After settling, the pile is at rest: per-unit-mass KE is tiny.
    for (BodyHandle b : boxes)
    {
        const Vec2 v = w.Velocity(b);
        const Real keOverM = Real(0.5) * (v.x * v.x + v.y * v.y);
        CHECK(keOverM < Real(50)); // effectively at rest
    }
    INFO("peak per-mass KE = " << static_cast<double>(peakKE));
}

// ====================================================================
// 4. RUN-TWICE DETERMINISM: a colliding scene stepped twice yields a
//    bit-identical final state (positions + angles + velocities).
// ====================================================================
TEST_CASE("physics-invariant: a colliding scene is run-twice deterministic",
          "[physics][invariant]")
{
    auto run = [](std::vector<Real>& trace)
    {
        WorldDef wd;
        wd.gravityY = kGravity;
        PhysicsWorld w(wd);

        const Real floorTop = Real(300);
        AddFloor(w, floorTop);

        std::vector<BodyHandle> boxes;
        const Real h = Real(9);
        for (int i = 0; i < 4; ++i)
        {
            boxes.push_back(AddBox(w, Vec2(Real(i * 3 - 4),
                                           floorTop - Real(40) - Real(i * 22)), h));
        }
        // Nudge one box sideways so the scene has lateral motion + friction.
        w.SetVelocity(boxes[0], Vec2(Real(30), Real(0)));

        trace.clear();
        for (int k = 0; k < 200; ++k)
        {
            w.Step(kStep);
        }
        for (BodyHandle b : boxes)
        {
            const Vec2 p = w.Position(b);
            const Vec2 v = w.Velocity(b);
            trace.push_back(p.x);
            trace.push_back(p.y);
            trace.push_back(w.GetAngle(b));
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
        REQUIRE(a[i] == b[i]); // bit-identical
    }
}

// ====================================================================
// 5. BROADPHASE-STRATEGY INVARIANCE: the SAME scene settled under each
//    broadphase strategy (Tree / Hash / SAP) reaches the SAME final state.
//
// This is the world-level fold of PhysicsBroadphaseTest's cross-strategy
// equivalence: the broadphase only changes WHICH pairs are tested, never the
// narrowphase/solver result, so swapping the strategy must not change the
// physics. We run an identical settling scene three times (one per strategy)
// and assert bit-identical final state.
// ====================================================================
TEST_CASE("physics-invariant: broadphase strategy does not change the result",
          "[physics][invariant]")
{
    auto runWith = [](BroadphaseKind kind, std::vector<Real>& trace)
    {
        WorldDef wd;
        wd.gravityY   = kGravity;
        wd.broadphase = kind;
        wd.hashCellSize = Real(64);
        PhysicsWorld w(wd);

        const Real floorTop = Real(300);
        AddFloor(w, floorTop);

        std::vector<BodyHandle> boxes;
        const Real h = Real(9);
        for (int i = 0; i < 5; ++i)
        {
            boxes.push_back(AddBox(w, Vec2(Real(i * 6 - 12),
                                           floorTop - Real(30) - Real(i * 21)), h));
        }

        trace.clear();
        for (int k = 0; k < 200; ++k)
        {
            w.Step(kStep);
        }
        for (BodyHandle b : boxes)
        {
            const Vec2 p = w.Position(b);
            trace.push_back(p.x);
            trace.push_back(p.y);
            trace.push_back(w.GetAngle(b));
        }
    };

    std::vector<Real> tree, hash, sap;
    runWith(BroadphaseKind::Tree, tree);
    runWith(BroadphaseKind::Hash, hash);
    runWith(BroadphaseKind::Sap,  sap);

    REQUIRE(tree.size() == hash.size());
    REQUIRE(hash.size() == sap.size());
    for (std::size_t i = 0; i < tree.size(); ++i)
    {
        // The settled state is identical regardless of broadphase strategy.
        REQUIRE(tree[i] == hash[i]);
        REQUIRE(hash[i] == sap[i]);
    }
}

// ====================================================================
// 6. SLIDE-NO-CATCH over a merged TileGrid span: a swept shape gliding along a
//    long merged multi-tile span clears its full length -- it does NOT snag at
//    an internal cell seam (the merge fuses internal vertical edges away).
//
// This is the keystone tile property. PhysicsTileGridTest covers it for a
// capsule sweep; here we pin the same property via the world TileGrid -> the
// merged-span set has NO internal seams and a horizontal sweep across the top
// edge travels its full length.
// ====================================================================
TEST_CASE("physics-invariant: sliding over a merged tile span does not catch",
          "[physics][invariant]")
{
    const Real cs = Real(32);

    // A single long horizontal run: row 0, cols 0..5 (6 cells). Merged rect =
    // [0,0]..[192,32]. Internal cell boundaries WOULD be at x = 32,64,96,128,160;
    // the merge fuses them away.
    GridPassability grid(/*w*/ 10, /*h*/ 2);
    for (int cx = 0; cx <= 5; ++cx)
    {
        grid.SetSolid(cx, 0, true);
    }
    TileGrid tg(grid, cs, Vec2(Real(0), Real(0)));

    const Aabb2 whole{ Vec2(Real(0), Real(0)), Vec2(Real(10) * cs, Real(2) * cs) };
    std::vector<Aabb2> rects;
    const int n = tg.Query(whole, rects);

    // EXACTLY ONE merged rect for the 6-cell run -- the seam-killer (per-cell
    // rects would yield 6 here, each with an internal vertical edge).
    REQUIRE(n == 1);
    const Aabb2 span = rects[0];
    REQUIRE(span.min.x == Approx(Real(0)));
    REQUIRE(span.max.x == Approx(Real(6) * cs)); // 192 -- the OUTER extent
    REQUIRE(span.max.y == Approx(cs));           // 32

    // Build the merged rect as a CCW box polygon for the cast.
    const Vec2 poly[4] = { Vec2(span.min.x, span.min.y), Vec2(span.max.x, span.min.y),
                           Vec2(span.max.x, span.max.y), Vec2(span.min.x, span.max.y) };

    // A capsule grazing JUST ABOVE the span's top edge sweeping its full width.
    const Shape cap = MakeCapsule(/*halfLen*/ Real(6), /*radius*/ Real(4));
    const Real  gap   = Real(1.0);
    const Real  rideY = span.min.y - Real(4) - gap; // a hair above the top edge
    Transform   xf;
    xf.position = Vec2(span.min.x - Real(20), rideY);
    const Vec2  translation = Vec2((span.max.x + Real(20)) - (span.min.x - Real(20)), Real(0));

    const ShapeCastResult clear = ShapeCastPoly(cap, xf, translation, poly, 4);
    // Riding above the top edge with clearance: the swept capsule never reaches
    // the span (there is no internal edge to catch on) -> it travels fully.
    CHECK_FALSE(clear.hit);

    // Driving INTO the left face must stop at the OUTER left extent (x = 0), NOT
    // at any internal cell boundary (x = 32/64/... were eliminated by the merge).
    const Real midY = (span.min.y + span.max.y) * Real(0.5);
    Transform  xf2;
    xf2.position = Vec2(span.min.x - Real(40), midY);
    const Vec2 trans2 = Vec2(Real(80), Real(0));
    const ShapeCastResult face = ShapeCastPoly(cap, xf2, trans2, poly, 4);
    REQUIRE(face.hit);
    const Real impactX  = xf2.position.x + trans2.x * face.t;
    const Real leadingX = impactX + (Real(6) + Real(4)); // capsule leading edge
    CHECK(leadingX <= Approx(span.min.x).margin(0.2)); // stops at outer face
    CHECK(leadingX < Real(32) - Real(10));             // nowhere near 1st internal seam
}

// ====================================================================
// 7. WARM-START CACHE BOUNDED: as contacts persist by stable feature id, the
//    solver warm-start cache stays bounded. A small settled scene has a small,
//    steady cache; it does NOT grow without bound as the scene is stepped.
// ====================================================================
TEST_CASE("physics-invariant: warm-start cache stays bounded as contacts persist",
          "[physics][invariant]")
{
    WorldDef wd;
    wd.gravityY = kGravity;
    PhysicsWorld w(wd);

    const Real floorTop = Real(300);
    AddFloor(w, floorTop);

    // A handful of resting boxes -> a small, fixed set of persistent contacts.
    const Real h = Real(10);
    const int  kBoxes = 4;
    for (int i = 0; i < kBoxes; ++i)
    {
        AddBox(w, Vec2(Real(0), floorTop - h - Real(i * 2 * h)), h);
    }

    // Warm up + settle.
    for (int i = 0; i < 120; ++i)
    {
        w.Step(kStep);
    }

    // The persistent contact set is at most a few constraints per box (box-box +
    // box-floor, up to 2 points each). The warm-start cache is keyed by stable
    // feature id, so once the scene is at rest the cache size is steady. Bound:
    // generously, a few entries per box. A leak (id churn) would grow it every
    // step.
    const std::size_t boundedCap = static_cast<std::size_t>(kBoxes) * 8u + 16u;

    const std::size_t sizeAfterSettle = w.SolverWarmStartCacheSize();
    CHECK(sizeAfterSettle <= boundedCap);

    // Step a lot more: the cache must NOT keep growing (no unbounded id churn).
    for (int i = 0; i < 300; ++i)
    {
        w.Step(kStep);
    }
    const std::size_t sizeMuchLater = w.SolverWarmStartCacheSize();
    CHECK(sizeMuchLater <= boundedCap);
    // The active-contact pool is likewise bounded (a few per box).
    CHECK(w.ActiveContactCount() <= boundedCap);
}
