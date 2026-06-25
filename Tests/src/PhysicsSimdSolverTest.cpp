// Physics SIMD solver: solver-local body-state SoA (Task 1) -- ROUND-TRIP test.
//
// The SIMD constraint-solver initiative mirrors the world's per-component Vec2
// SoA into a packed `float` body-state SoA (BodyStateSoA), indexed by world
// slot, so the lane-wide solve can gather/scatter body velocities by body-index
// lanes. This TU validates the FIRST de-risking piece: the SyncIn/SyncOut
// world<->solver bridge.
//
// SCOPE (Task 1): only the SoA struct + its two sync helpers exist yet -- the
// solver does NOT consume BodyStateSoA (that is a later task). These tests pin
// the sync contract:
//   * SyncIn copies awake-dynamic world velocities into the packed arrays and
//     zeroes the TGS position deltas.
//   * mutating the packed velocities then SyncOut writes them BACK to the world.
//   * non-matching slots (a Static body) are LEFT UNTOUCHED by SyncOut.
//
// The awake-dynamic predicate MUST mirror SoftStep::FinalizePositions:
//   Alive(i) && TypeSlot(i) == BodyType::Dynamic && AwakeSlot(i).
//
// PRESENTATION-FREE + C++20-clean.

#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Solver/BodyStateSoA.hpp>
#include <Arcane/Physics/Solver/ContactColoring.hpp>
#include <Arcane/Physics/Solver/ContactConstraintSimd.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    // A dynamic circle (non-fixedRotation -> a meaningful angular velocity).
    BodyHandle AddDynamicCircle(PhysicsWorld& w, Vec2 pos, Real r)
    {
        BodyDef def;
        def.type     = BodyType::Dynamic;
        def.position = pos;
        def.shape    = MakeCircle(r);
        def.density  = Real(1);
        return w.AddBody(def);
    }

    // A static box (fails the Dynamic predicate -> never synced).
    BodyHandle AddStaticBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
    {
        BodyDef def;
        def.type     = BodyType::Static;
        def.position = pos;
        def.shape    = MakeAabb(hw, hh);
        return w.AddBody(def);
    }
} // namespace

TEST_CASE("PhysicsSimd: BodyStateSoA SyncIn/SyncOut round-trips world velocities",
          "[physics]")
{
    PhysicsWorld w;

    // Two awake dynamic bodies with known velocities + one static body.
    const BodyHandle a = AddDynamicCircle(w, Vec2(Real(0), Real(0)), Real(1));
    const BodyHandle b = AddDynamicCircle(w, Vec2(Real(10), Real(0)), Real(1));
    const BodyHandle s = AddStaticBox(w, Vec2(Real(0), Real(50)), Real(20), Real(1));

    // Seed known linear + angular velocities. SetVelocity wakes the dynamics
    // (clears the sleep timer), which the awake-dynamic predicate requires.
    const Vec2 vaIn(Real(3), Real(-4));
    const Vec2 vbIn(Real(-7), Real(2));
    const Real waIn = Real(1.5);
    const Real wbIn = Real(-0.25);
    w.SetVelocity(a, vaIn);
    w.SetVelocity(b, vbIn);
    // Angular velocity has no handle-based setter; the public solver-seam slot
    // accessor is the canonical write path (BodyHandle.index IS the SoA slot).
    w.SetAngVelSlot(a.index, waIn);
    w.SetAngVelSlot(b.index, wbIn);

    // Give the static body a velocity too (Static ignores SetVelocity, so this
    // stays 0); we will assert SyncOut never disturbs the static slot.
    REQUIRE(w.VelSlot(s.index) == Vec2(Real(0), Real(0)));

    // ---- SyncIn: world velocities -> packed SoA; dp/dq zeroed --------------
    BodyStateSoA soa;
    soa.Resize(w.Count());            // caller sizes to world slot count
    // Pre-poison the position deltas so we can prove SyncIn zeroes them.
    soa.dpx[a.index] = Real(99);
    soa.dpy[a.index] = Real(99);
    soa.dq[a.index]  = Real(99);
    soa.SyncIn(w);

    CHECK(soa.vx[a.index] == Approx(vaIn.x));
    CHECK(soa.vy[a.index] == Approx(vaIn.y));
    CHECK(soa.w[a.index]  == Approx(waIn));
    CHECK(soa.vx[b.index] == Approx(vbIn.x));
    CHECK(soa.vy[b.index] == Approx(vbIn.y));
    CHECK(soa.w[b.index]  == Approx(wbIn));

    // dp/dq for synced slots are zeroed by SyncIn (TGS delta accumulator).
    CHECK(soa.dpx[a.index] == Approx(0.0f));
    CHECK(soa.dpy[a.index] == Approx(0.0f));
    CHECK(soa.dq[a.index]  == Approx(0.0f));

    // ---- mutate the packed velocities, then SyncOut -----------------------
    const Vec2 vaOut(Real(100), Real(-200));
    const Vec2 vbOut(Real(-1), Real(0.5));
    const Real waOut = Real(-9);
    const Real wbOut = Real(42);
    soa.vx[a.index] = vaOut.x; soa.vy[a.index] = vaOut.y; soa.w[a.index] = waOut;
    soa.vx[b.index] = vbOut.x; soa.vy[b.index] = vbOut.y; soa.w[b.index] = wbOut;

    soa.SyncOut(w);

    // The world's velocities now reflect the mutated packed values.
    CHECK(w.VelSlot(a.index).x == Approx(vaOut.x));
    CHECK(w.VelSlot(a.index).y == Approx(vaOut.y));
    CHECK(w.AngVelSlot(a.index) == Approx(waOut));
    CHECK(w.VelSlot(b.index).x == Approx(vbOut.x));
    CHECK(w.VelSlot(b.index).y == Approx(vbOut.y));
    CHECK(w.AngVelSlot(b.index) == Approx(wbOut));

    // ---- invariant: the non-synced static slot is untouched ---------------
    CHECK(w.VelSlot(s.index) == Vec2(Real(0), Real(0)));
    CHECK(w.AngVelSlot(s.index) == Approx(0.0f));
}

// ===========================================================================
// Graph coloring (Task 2) -- the SIMD solve processes constraints 8-wide; two
// constraints in one lane-batch that share a DYNAMIC body would race on the
// read-modify-write of that body's velocity. ColorConstraints partitions edges
// into colors such that no two edges in a color share a dynamic body, so each
// color is safe to solve lane-wide. Static/kinematic endpoints (invMass==0) are
// read-only in the solve, so they may be shared freely within a color.
//
// SCOPE (Task 2): only ColorConstraints + these property tests exist; nothing
// consumes the coloring yet (Task 5 wires it into the solve). These cases pin:
//   (a) within-color dynamic-disjointness,
//   (b) a static body may fan out across one color,
//   (c) determinism (same input -> identical output),
//   (d) star overflow when one dynamic body needs > kColorCount colors,
//   plus the no-drop / no-duplicate ref invariant.
// ===========================================================================

namespace
{
    // Collect every ref emitted across all colors + overflow, so a test can
    // assert nothing was dropped or duplicated.
    std::vector<std::uint32_t> AllRefs(const Coloring& c)
    {
        std::vector<std::uint32_t> out;
        for (const auto& color : c.colors)
        {
            out.insert(out.end(), color.begin(), color.end());
        }
        out.insert(out.end(), c.overflow.begin(), c.overflow.end());
        std::sort(out.begin(), out.end());
        return out;
    }
} // namespace

TEST_CASE("PhysicsSimd: ColorConstraints keeps dynamic bodies disjoint per color",
          "[physics]")
{
    // A chain of dynamic edges 0-1, 1-2, 2-3, 3-4: adjacent edges share a
    // dynamic body, so each must land in a different color than its neighbour.
    std::vector<ColorEdge> edges = {
        { 0u, 1u, true, true, 0u },
        { 1u, 2u, true, true, 1u },
        { 2u, 3u, true, true, 2u },
        { 3u, 4u, true, true, 3u },
    };

    const Coloring c = ColorConstraints(edges, /*bodyCount=*/5u);

    // (a) Within each color, no two edges share a dynamic body. Walk each color
    // and assert its dynamic endpoints are all distinct.
    for (const auto& color : c.colors)
    {
        std::vector<std::uint32_t> dynBodies;
        for (std::uint32_t ref : color)
        {
            const ColorEdge& e = edges[ref];
            if (e.aDyn) dynBodies.push_back(e.a);
            if (e.bDyn) dynBodies.push_back(e.b);
        }
        std::sort(dynBodies.begin(), dynBodies.end());
        const bool hasDup =
            std::adjacent_find(dynBodies.begin(), dynBodies.end()) != dynBodies.end();
        CHECK_FALSE(hasDup);
    }

    // No overflow expected -- a 5-vertex chain needs only 2 colors.
    CHECK(c.overflow.empty());

    // No-drop / no-duplicate: every input ref appears exactly once.
    const std::vector<std::uint32_t> refs = AllRefs(c);
    REQUIRE(refs.size() == edges.size());
    CHECK(refs == std::vector<std::uint32_t>({ 0u, 1u, 2u, 3u }));
}

TEST_CASE("PhysicsSimd: ColorConstraints shares a static body across one color",
          "[physics]")
{
    // (b) A fan of edges that all share ONE static body (body 0, aDyn=false),
    // each with a distinct dynamic partner. The static endpoint does not
    // constrain coloring, so -- since each edge's dynamic body is unique --
    // every edge should land in color 0.
    std::vector<ColorEdge> edges = {
        { 0u, 1u, false, true, 0u },
        { 0u, 2u, false, true, 1u },
        { 0u, 3u, false, true, 2u },
        { 0u, 4u, false, true, 3u },
        { 0u, 5u, false, true, 4u },
    };

    const Coloring c = ColorConstraints(edges, /*bodyCount=*/6u);

    // All five edges land in color 0; later colors stay empty.
    CHECK(c.colors[0] == std::vector<std::uint32_t>({ 0u, 1u, 2u, 3u, 4u }));
    for (std::size_t k = 1; k < c.colors.size(); ++k)
    {
        CHECK(c.colors[k].empty());
    }
    CHECK(c.overflow.empty());

    const std::vector<std::uint32_t> refs = AllRefs(c);
    REQUIRE(refs.size() == edges.size());
}

TEST_CASE("PhysicsSimd: ColorConstraints is deterministic", "[physics]")
{
    // (c) Same input vector -> identical Coloring output. Use a non-trivial mix
    // of dynamic/static endpoints so a non-deterministic implementation would
    // be likely to diverge.
    std::vector<ColorEdge> edges = {
        { 0u, 1u, true,  true,  0u },
        { 1u, 2u, true,  true,  1u },
        { 0u, 2u, true,  true,  2u },   // closes a triangle -> needs color 2
        { 3u, 0u, false, true,  3u },   // static 3 fans onto dynamic 0
        { 4u, 1u, true,  true,  4u },
        { 2u, 4u, true,  true,  5u },
    };

    const Coloring first  = ColorConstraints(edges, /*bodyCount=*/5u);
    const Coloring second = ColorConstraints(edges, /*bodyCount=*/5u);

    REQUIRE(first.colors.size() == second.colors.size());
    for (std::size_t k = 0; k < first.colors.size(); ++k)
    {
        CHECK(first.colors[k] == second.colors[k]);
    }
    CHECK(first.overflow == second.overflow);
}

TEST_CASE("PhysicsSimd: ColorConstraints overflows a star past kColorCount",
          "[physics]")
{
    // (d) A star: one central dynamic body (0) shared by N edges with
    // N > kColorCount. Each edge touches body 0, so each needs its OWN color;
    // only kColorCount colors exist, so the excess edges spill to overflow.
    const std::uint32_t n = static_cast<std::uint32_t>(kColorCount) + 3u;
    std::vector<ColorEdge> edges;
    edges.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        // hub = body 0 (dynamic); each spoke = a distinct dynamic body i+1.
        edges.push_back(ColorEdge{ 0u, i + 1u, true, true, i });
    }

    const Coloring c = ColorConstraints(edges, /*bodyCount=*/n + 1u);

    // The first kColorCount edges each take a distinct color (one per color).
    for (int k = 0; k < kColorCount; ++k)
    {
        REQUIRE(c.colors[k].size() == 1u);
        CHECK(c.colors[k][0] == static_cast<std::uint32_t>(k));
    }

    // The remaining edges (refs kColorCount .. n-1) found no free color.
    REQUIRE(c.overflow.size() == n - static_cast<std::uint32_t>(kColorCount));
    for (std::uint32_t i = 0; i < c.overflow.size(); ++i)
    {
        CHECK(c.overflow[i] == static_cast<std::uint32_t>(kColorCount) + i);
    }

    // No-drop / no-duplicate across the full union.
    const std::vector<std::uint32_t> refs = AllRefs(c);
    REQUIRE(refs.size() == n);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        CHECK(refs[i] == i);
    }
}

// ===========================================================================
// SoA contact-constraint batch (Task 4) -- the lane-wide solve (Task 5) wants
// the per-color ContactConstraints packed into width-lane Structure-of-Arrays
// batches (one CONTACT per lane, up to 2 points per contact). ContactConstraint-
// Simd::Build is the pure packer that fills those batches from a color's refs.
//
// SCOPE (Task 4): only the SoA struct + Build exist; nothing solves it yet
// (Task 5 loads these lane arrays into f32w/i32w/b32w and runs the TGS-Soft
// math). These cases pin the packer contract:
//   (a) every per-lane SoA field mirrors the source ContactConstraint/...Point;
//   (b) a 1-point contact has point-2's pointValid false + its masses zeroed;
//   (c) a partial final batch masks its padding lanes to a lane-wide no-op
//       (invMassA==invMassB==0, pointValid false, impulses 0, bodyIndex 0).
// ===========================================================================

namespace
{
    // Build a fully-populated ContactConstraint with distinct known values keyed
    // off `seed`, so every packed lane field can be checked against a unique
    // number (a transposition bug -> a mismatch). `pts` chooses 1 or 2 points.
    ContactConstraint MakeCC(float seed, int pts, std::uint32_t bA,
                             std::uint32_t bB, bool bIsBody, float invMB)
    {
        ContactConstraint cc{};
        cc.bodyA       = bA;
        cc.bodyB       = bB;
        cc.bodyBIsBody = bIsBody;
        cc.invMassA    = seed + 0.10f;
        cc.invInertiaA = seed + 0.20f;
        cc.invMassB    = invMB;                 // caller controls (dynB depends on it)
        cc.invInertiaB = seed + 0.40f;
        cc.normal      = Vec2(seed + 0.50f, seed + 0.60f);
        cc.friction    = seed + 0.70f;
        cc.restitution = seed + 0.80f;
        cc.biasRate    = seed + 0.90f;
        cc.massScale   = seed + 1.10f;
        cc.impulseScale= seed + 1.20f;
        cc.pointCount  = pts;
        for (int p = 0; p < pts; ++p)
        {
            const float ps = seed + 10.0f * static_cast<float>(p + 1);
            ContactConstraintPoint& cp = cc.points[p];
            cp.anchorA         = Vec2(ps + 0.01f, ps + 0.02f);
            cp.anchorB         = Vec2(ps + 0.03f, ps + 0.04f);
            cp.baseSeparation  = ps + 0.05f;
            cp.normalMass      = ps + 0.06f;
            cp.tangentMass     = ps + 0.07f;
            cp.normalImpulse   = ps + 0.08f;
            cp.tangentImpulse  = ps + 0.09f;
            cp.relativeVelocity= ps + 0.11f;
            cp.id              = static_cast<std::uint32_t>(seed) * 100u
                                 + static_cast<std::uint32_t>(p);
        }
        return cc;
    }

    // Assert lane L of batch `b` mirrors source ContactConstraint `cc` exactly.
    void CheckLaneMatches(const ContactConstraintSimd& b, int L,
                          const ContactConstraint& cc)
    {
        CHECK(b.normalX[L]     == Approx(cc.normal.x));
        CHECK(b.normalY[L]     == Approx(cc.normal.y));
        CHECK(b.friction[L]    == Approx(cc.friction));
        CHECK(b.restitution[L] == Approx(cc.restitution));
        CHECK(b.biasRate[L]    == Approx(cc.biasRate));
        CHECK(b.massScale[L]   == Approx(cc.massScale));
        CHECK(b.impulseScale[L]== Approx(cc.impulseScale));
        CHECK(b.invMassA[L]    == Approx(cc.invMassA));
        CHECK(b.invInertiaA[L] == Approx(cc.invInertiaA));
        CHECK(b.invMassB[L]    == Approx(cc.invMassB));
        CHECK(b.invInertiaB[L] == Approx(cc.invInertiaB));
        CHECK(b.bodyIndexA[L]  == static_cast<std::int32_t>(cc.bodyA));
        CHECK(b.bodyIndexB[L]  == static_cast<std::int32_t>(cc.bodyB));

        // dynB float mask: 1.0f iff B is a dynamic body (bodyBIsBody && invMassB>0).
        const bool dyn = cc.bodyBIsBody && cc.invMassB > 0.0f;
        CHECK(b.dynB[L] == Approx(dyn ? 1.0f : 0.0f));

        for (int p = 0; p < 2; ++p)
        {
            const ContactConstraintSimd::Point& pt = b.points[p];
            if (p < cc.pointCount)
            {
                const ContactConstraintPoint& cp = cc.points[p];
                CHECK(pt.anchorAx[L]      == Approx(cp.anchorA.x));
                CHECK(pt.anchorAy[L]      == Approx(cp.anchorA.y));
                CHECK(pt.anchorBx[L]      == Approx(cp.anchorB.x));
                CHECK(pt.anchorBy[L]      == Approx(cp.anchorB.y));
                CHECK(pt.baseSep[L]       == Approx(cp.baseSeparation));
                CHECK(pt.normalMass[L]    == Approx(cp.normalMass));
                CHECK(pt.tangentMass[L]   == Approx(cp.tangentMass));
                CHECK(pt.normalImpulse[L] == Approx(cp.normalImpulse));
                CHECK(pt.tangentImpulse[L]== Approx(cp.tangentImpulse));
                CHECK(pt.relVel[L]        == Approx(cp.relativeVelocity));
                CHECK(pt.pointValid[L]    == Approx(1.0f));
            }
            else
            {
                // (b) point >= pointCount: invalid lane, masses + impulses zeroed.
                CHECK(pt.pointValid[L]    == Approx(0.0f));
                CHECK(pt.normalMass[L]    == Approx(0.0f));
                CHECK(pt.tangentMass[L]   == Approx(0.0f));
                CHECK(pt.normalImpulse[L] == Approx(0.0f));
                CHECK(pt.tangentImpulse[L]== Approx(0.0f));
            }
        }
    }
} // namespace

TEST_CASE("PhysicsSimd: ContactConstraintSimd::Build packs lane fields verbatim",
          "[physics]")
{
    constexpr int W = ContactConstraintSimd::kWidth;

    // One full batch worth of distinct 2-point contacts, all dynamic-vs-dynamic
    // so dynB == 1 on every lane.
    std::vector<ContactConstraint> ccs;
    std::vector<std::uint32_t>     refs;
    for (int i = 0; i < W; ++i)
    {
        ccs.push_back(MakeCC(/*seed=*/static_cast<float>(i + 1), /*pts=*/2,
                             /*bA=*/static_cast<std::uint32_t>(i * 2 + 0),
                             /*bB=*/static_cast<std::uint32_t>(i * 2 + 1),
                             /*bIsBody=*/true, /*invMB=*/0.5f + static_cast<float>(i)));
        refs.push_back(static_cast<std::uint32_t>(i));
    }

    const std::vector<ContactConstraintSimd> batches =
        ContactConstraintSimd::Build(ccs.data(), refs.data(),
                                     static_cast<int>(refs.size()));

    REQUIRE(batches.size() == 1u);
    CHECK(batches[0].count == W);

    // (a) every lane mirrors its source ContactConstraint verbatim.
    for (int L = 0; L < W; ++L)
    {
        CheckLaneMatches(batches[0], L, ccs[refs[L]]);
    }
}

TEST_CASE("PhysicsSimd: ContactConstraintSimd::Build handles 1-point + static-B "
          "contacts", "[physics]")
{
    // Lane 0: a 1-point contact against a STATIC fixture (bodyBIsBody=false ->
    // dynB must be 0 even though we never set invMassB). Lane 1: a 2-point
    // contact against a body with invMassB==0 (kinematic -> dynB 0 too).
    std::vector<ContactConstraint> ccs;
    ccs.push_back(MakeCC(/*seed=*/3.0f, /*pts=*/1, /*bA=*/5u, /*bB=*/9u,
                         /*bIsBody=*/false, /*invMB=*/0.0f));
    ccs.push_back(MakeCC(/*seed=*/7.0f, /*pts=*/2, /*bA=*/2u, /*bB=*/4u,
                         /*bIsBody=*/true,  /*invMB=*/0.0f));   // kinematic B
    std::vector<std::uint32_t> refs = { 0u, 1u };

    const std::vector<ContactConstraintSimd> batches =
        ContactConstraintSimd::Build(ccs.data(), refs.data(), 2);

    REQUIRE(batches.size() == 1u);
    CHECK(batches[0].count == 2);

    // Lane 0: 1-point static-B contact.
    CheckLaneMatches(batches[0], 0, ccs[0]);
    CHECK(batches[0].dynB[0] == Approx(0.0f));           // static fixture
    CHECK(batches[0].points[1].pointValid[0] == Approx(0.0f));  // 2nd point absent

    // Lane 1: 2-point contact, but B is a kinematic body (invMassB==0) -> dynB 0.
    CheckLaneMatches(batches[0], 1, ccs[1]);
    CHECK(batches[0].dynB[1] == Approx(0.0f));
}

TEST_CASE("PhysicsSimd: ContactConstraintSimd::Build masks partial-batch padding "
          "lanes", "[physics]")
{
    constexpr int W = ContactConstraintSimd::kWidth;

    // count = W + 1 -> two batches; the SECOND batch has exactly one live lane
    // and (W - 1) padding lanes (>=0; if W==1 there is no partial tail, so the
    // test still passes trivially with the count==1 live lane).
    const int count = W + 1;
    std::vector<ContactConstraint> ccs;
    std::vector<std::uint32_t>     refs;
    for (int i = 0; i < count; ++i)
    {
        ccs.push_back(MakeCC(static_cast<float>(i + 1), /*pts=*/2,
                             static_cast<std::uint32_t>(i * 2 + 0),
                             static_cast<std::uint32_t>(i * 2 + 1),
                             /*bIsBody=*/true, /*invMB=*/1.0f));
        refs.push_back(static_cast<std::uint32_t>(i));
    }

    const std::vector<ContactConstraintSimd> batches =
        ContactConstraintSimd::Build(ccs.data(), refs.data(), count);

    const std::size_t expectedBatches =
        static_cast<std::size_t>((count + W - 1) / W);
    REQUIRE(batches.size() == expectedBatches);

    const ContactConstraintSimd& tail = batches.back();
    const int live = count - static_cast<int>((batches.size() - 1) * W);
    CHECK(tail.count == live);

    // Live lanes of the tail batch mirror their source.
    for (int L = 0; L < live; ++L)
    {
        const std::uint32_t ref = refs[(batches.size() - 1) * W + L];
        CheckLaneMatches(tail, L, ccs[ref]);
    }

    // (c) padding lanes [live, W) are a lane-wide no-op: zero inv-mass on BOTH
    // bodies, both points invalid + impulses zero, body index a safe 0.
    for (int L = live; L < W; ++L)
    {
        CHECK(tail.invMassA[L]    == Approx(0.0f));
        CHECK(tail.invInertiaA[L] == Approx(0.0f));
        CHECK(tail.invMassB[L]    == Approx(0.0f));
        CHECK(tail.invInertiaB[L] == Approx(0.0f));
        CHECK(tail.bodyIndexA[L]  == 0);
        CHECK(tail.bodyIndexB[L]  == 0);
        CHECK(tail.dynB[L]        == Approx(0.0f));
        for (int p = 0; p < 2; ++p)
        {
            CHECK(tail.points[p].pointValid[L]     == Approx(0.0f));
            CHECK(tail.points[p].normalImpulse[L]  == Approx(0.0f));
            CHECK(tail.points[p].tangentImpulse[L] == Approx(0.0f));
            CHECK(tail.points[p].normalMass[L]     == Approx(0.0f));
            CHECK(tail.points[p].tangentMass[L]    == Approx(0.0f));
        }
    }
}
