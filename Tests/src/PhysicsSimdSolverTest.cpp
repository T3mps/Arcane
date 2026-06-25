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
#include <cmath>
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

        // dynB float mask: 1.0f iff B is a dynamic body (bodyBIsBody && invMassB>0).
        const bool dyn = cc.bodyBIsBody && cc.invMassB > 0.0f;
        CHECK(b.dynB[L] == Approx(dyn ? 1.0f : 0.0f));

        // bodyIndexB mirrors cc.bodyB for ANY real body B (bodyBIsBody) -- dynamic
        // OR read-only (kinematic/static): a read-only B's velocity must still be
        // gatherable (a kinematic plate's authored velocity feeds the push). Only a
        // tile span (bodyBIsBody==false, cc.bodyB==kInvalidSlot) points at the
        // scatter-safe dummy (here the default 0). The scatter writes back the
        // unchanged gathered value for a read-only B, so its slot is preserved.
        CHECK(b.bodyIndexB[L]  == (cc.bodyBIsBody
                                       ? static_cast<std::int32_t>(cc.bodyB)
                                       : 0));

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

    // Lane 0: 1-point contact against a tile SPAN (bodyBIsBody=false, bodyB=9 but
    // NOT a real body). B is read-only (dynB 0) AND not a real body, so Build
    // points the packed index at the dummy (default 0). 2nd point absent.
    CheckLaneMatches(batches[0], 0, ccs[0]);
    CHECK(batches[0].dynB[0] == Approx(0.0f));           // span fixture
    CHECK(batches[0].bodyIndexB[0] == 0);                // span -> dummy (default 0)
    CHECK(batches[0].points[1].pointValid[0] == Approx(0.0f));  // 2nd point absent

    // Lane 1: 2-point contact, B a REAL kinematic body (bodyBIsBody=true,
    // invMassB==0) -> dynB 0 (not mutated) but the index is its REAL slot (4) so
    // the solver can gather the kinematic's velocity (the push driver).
    CheckLaneMatches(batches[0], 1, ccs[1]);
    CHECK(batches[0].dynB[1] == Approx(0.0f));
    CHECK(batches[0].bodyIndexB[1] == 4);                // real kinematic slot, gatherable
}

TEST_CASE("PhysicsSimd: ContactConstraintSimd::Build clamps a tile-span "
          "(kInvalidSlot) bodyIndexB to 0", "[physics]")
{
    // A tile-span virtual fixture contact: B is NOT a body (bodyBIsBody=false)
    // and cc.bodyB == kInvalidSlot == 0xFFFFFFFF, which casts to -1. Build MUST
    // clamp the packed bodyIndexB to 0 (an in-range gather slot) -- NOT leave it
    // at -1, which would make T5's unconditional AVX2 gather read base[-1] (an
    // out-of-bounds heap under-read) every sub-step. dynB==0 discards whatever
    // is gathered, so 0 is harmless and in-range. This pins the contract the
    // finite-slot static-B case above does not exercise.
    ContactConstraint span = MakeCC(/*seed=*/5.0f, /*pts=*/1, /*bA=*/2u,
                                    /*bB=*/kInvalidSlot, /*bIsBody=*/false,
                                    /*invMB=*/0.0f);
    span.invInertiaB = 0.0f;   // a span has no rotational inertia either

    std::vector<ContactConstraint> ccs = { span };
    std::vector<std::uint32_t>     refs = { 0u };

    const std::vector<ContactConstraintSimd> batches =
        ContactConstraintSimd::Build(ccs.data(), refs.data(), 1);

    REQUIRE(batches.size() == 1u);
    CHECK(batches[0].count == 1);

    // The crux: B is read-only -> dynB 0 AND bodyIndexB clamped to 0 (NOT -1).
    CHECK(batches[0].dynB[0]       == Approx(0.0f));
    CHECK(batches[0].bodyIndexB[0] == 0);

    // Sanity: bodyIndexA is the real (dynamic) A slot, untouched by the clamp.
    CHECK(batches[0].bodyIndexA[0] == 2);

    // The rest of the lane still mirrors the source (CheckLaneMatches now expects
    // the clamped bodyIndexB for any read-only B).
    CheckLaneMatches(batches[0], 0, span);
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

// ===========================================================================
// Lane-wide contact solve (Task 5) -- the THREE solve-correctness gates:
//
//   * LANE-WIDTH INVARIANCE: within a color all contacts touch DISJOINT dynamic
//     bodies, so the solve is independent per lane -> the result must NOT depend
//     on the SIMD packing width. We solve the SAME independent contact set two
//     ways through the SAME (active-backend) SimdSolve passes -- packed wide (one
//     batch of W lanes) vs packed NARROW (W batches of 1 live lane + padding) --
//     and assert BIT-IDENTICAL body velocities. If a lane leaked into another, or
//     a padding lane were not a true no-op, the two packings would diverge.
//
//   * SCALAR-ORACLE MATCH: the lane-wide AVX2 result must match a width-1 SCALAR
//     reference (plain `float` math transcribed straight from SoftStep.cpp) within
//     1e-5. This is the SIMD-vs-scalar oracle: the scalar reference IS the ported
//     math, computed without the Simd wrapper, so a transcription error in the
//     lane-wide passes shows up here.
//
//   * SCATTER-CORRUPTION GUARD: a color-batch where body slot 0 is a real DYNAMIC
//     body sharing the batch with PADDING lanes (and a read-only-B lane) must NOT
//     let those lanes clobber body 0's solved velocity. With the dummy slot at
//     world.Count(), padding scatters there, not to body 0; and a read-only B
//     scatters back its unchanged value. We assert body 0's velocity equals the
//     scalar-oracle value (a corruption would zero/stale it).
//
// All three drive the header-only SimdSolve passes directly (no PhysicsWorld), so
// they pin the ported math in isolation. Tag [physics][simd].
// ===========================================================================

namespace
{
    // 2D cross helpers (mirror SoftStep.cpp's anonymous-namespace helpers).
    inline Vec2 XCrossWR(Real wv, const Vec2& r) { return Vec2(-wv * r.y, wv * r.x); }
    inline Real XCrossRP(const Vec2& r, const Vec2& p) { return r.x * p.y - r.y * p.x; }
    inline Real XDot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }

    // A self-contained WIDTH-1 SCALAR reference for ONE contact, reading/writing a
    // BodyStateSoA -- a verbatim transcription of SoftStep.cpp's WarmStart /
    // SolveContacts / ApplyRestitution math (plain float, no Simd wrapper). This is
    // the oracle the lane-wide passes are compared against. A is always dynamic;
    // dynB gates the B-side write. All contacts here use 1 point (pointCount==1).
    struct ScalarRef
    {
        BodyStateSoA& bs;
        Real h, invH, maxBiasVel, threshold;

        void WarmStart(ContactConstraint& cc)
        {
            const Vec2 n = cc.normal;
            const Vec2 tangent(-n.y, n.x);
            const bool dynB = cc.bodyBIsBody && cc.invMassB > Real(0);
            Vec2 vA(bs.vx[cc.bodyA], bs.vy[cc.bodyA]); Real wA = bs.w[cc.bodyA];
            Vec2 vB(bs.vx[cc.bodyB], bs.vy[cc.bodyB]); Real wB = bs.w[cc.bodyB];
            for (int p = 0; p < cc.pointCount; ++p)
            {
                const ContactConstraintPoint& cp = cc.points[p];
                const Vec2 P = n * cp.normalImpulse + tangent * cp.tangentImpulse;
                vA += P * cc.invMassA; wA += cc.invInertiaA * XCrossRP(cp.anchorA, P);
                if (dynB) { vB -= P * cc.invMassB; wB -= cc.invInertiaB * XCrossRP(cp.anchorB, P); }
            }
            bs.vx[cc.bodyA] = static_cast<float>(vA.x); bs.vy[cc.bodyA] = static_cast<float>(vA.y); bs.w[cc.bodyA] = static_cast<float>(wA);
            if (dynB) { bs.vx[cc.bodyB] = static_cast<float>(vB.x); bs.vy[cc.bodyB] = static_cast<float>(vB.y); bs.w[cc.bodyB] = static_cast<float>(wB); }
        }

        void Solve(ContactConstraint& cc, bool useBias)
        {
            const Vec2 n = cc.normal;
            const Vec2 tangent(-n.y, n.x);
            const bool dynB = cc.bodyBIsBody && cc.invMassB > Real(0);
            const Real iMa = cc.invMassA, iIa = cc.invInertiaA, iMb = cc.invMassB, iIb = cc.invInertiaB;
            Vec2 vA(bs.vx[cc.bodyA], bs.vy[cc.bodyA]); Real wA = bs.w[cc.bodyA];
            Vec2 vB(bs.vx[cc.bodyB], bs.vy[cc.bodyB]); Real wB = bs.w[cc.bodyB];
            const Vec2 dpA(bs.dpx[cc.bodyA], bs.dpy[cc.bodyA]); const Real drA = bs.dq[cc.bodyA];
            const Vec2 dpB(bs.dpx[cc.bodyB], bs.dpy[cc.bodyB]); const Real drB = bs.dq[cc.bodyB];
            for (int p = 0; p < cc.pointCount; ++p)
            {
                ContactConstraintPoint& cp = cc.points[p];
                const Vec2 rA = cp.anchorA, rB = cp.anchorB;
                const Vec2 prA = dpA + XCrossWR(drA, rA), prB = dpB + XCrossWR(drB, rB);
                const Real s = cp.baseSeparation + XDot(prA - prB, n);
                Real bias = Real(0), massScale = Real(1), impulseScale = Real(0);
                if (s > Real(0)) { bias = s * invH; }
                else if (useBias) { bias = std::max(cc.biasRate * s, -maxBiasVel); massScale = cc.massScale; impulseScale = cc.impulseScale; }
                const Vec2 dv = (vA + XCrossWR(wA, rA)) - (vB + XCrossWR(wB, rB));
                const Real vn = XDot(dv, n);
                Real impulse = -cp.normalMass * massScale * (vn + bias) - impulseScale * cp.normalImpulse;
                const Real newI = std::max(cp.normalImpulse + impulse, Real(0));
                impulse = newI - cp.normalImpulse; cp.normalImpulse = newI;
                const Vec2 P = n * impulse;
                vA += P * iMa; wA += iIa * XCrossRP(rA, P);
                if (dynB) { vB -= P * iMb; wB -= iIb * XCrossRP(rB, P); }
            }
            for (int p = 0; p < cc.pointCount; ++p)
            {
                ContactConstraintPoint& cp = cc.points[p];
                const Vec2 rA = cp.anchorA, rB = cp.anchorB;
                const Vec2 dv = (vA + XCrossWR(wA, rA)) - (vB + XCrossWR(wB, rB));
                const Real vt = XDot(dv, tangent);
                Real impulse = -cp.tangentMass * vt;
                const Real maxF = cc.friction * cp.normalImpulse;
                const Real newI = std::clamp(cp.tangentImpulse + impulse, -maxF, maxF);
                impulse = newI - cp.tangentImpulse; cp.tangentImpulse = newI;
                const Vec2 P = tangent * impulse;
                vA += P * iMa; wA += iIa * XCrossRP(rA, P);
                if (dynB) { vB -= P * iMb; wB -= iIb * XCrossRP(rB, P); }
            }
            bs.vx[cc.bodyA] = static_cast<float>(vA.x); bs.vy[cc.bodyA] = static_cast<float>(vA.y); bs.w[cc.bodyA] = static_cast<float>(wA);
            if (dynB) { bs.vx[cc.bodyB] = static_cast<float>(vB.x); bs.vy[cc.bodyB] = static_cast<float>(vB.y); bs.w[cc.bodyB] = static_cast<float>(wB); }
        }

        void Restitution(ContactConstraint& cc)
        {
            if (cc.restitution <= Real(0)) { return; }
            const Vec2 n = cc.normal;
            const bool dynB = cc.bodyBIsBody && cc.invMassB > Real(0);
            Vec2 vA(bs.vx[cc.bodyA], bs.vy[cc.bodyA]); Real wA = bs.w[cc.bodyA];
            Vec2 vB(bs.vx[cc.bodyB], bs.vy[cc.bodyB]); Real wB = bs.w[cc.bodyB];
            for (int p = 0; p < cc.pointCount; ++p)
            {
                ContactConstraintPoint& cp = cc.points[p];
                if (cp.relativeVelocity > -threshold || cp.normalImpulse <= Real(0)) { continue; }
                const Vec2 rA = cp.anchorA, rB = cp.anchorB;
                const Vec2 dv = (vA + XCrossWR(wA, rA)) - (vB + XCrossWR(wB, rB));
                const Real vn = XDot(dv, n);
                Real impulse = -cp.normalMass * (vn + cc.restitution * cp.relativeVelocity);
                const Real newI = std::max(cp.normalImpulse + impulse, Real(0));
                impulse = newI - cp.normalImpulse; cp.normalImpulse = newI;
                const Vec2 P = n * impulse;
                vA += P * cc.invMassA; wA += cc.invInertiaA * XCrossRP(rA, P);
                if (dynB) { vB -= P * cc.invMassB; wB -= cc.invInertiaB * XCrossRP(rB, P); }
            }
            bs.vx[cc.bodyA] = static_cast<float>(vA.x); bs.vy[cc.bodyA] = static_cast<float>(vA.y); bs.w[cc.bodyA] = static_cast<float>(wA);
            if (dynB) { bs.vx[cc.bodyB] = static_cast<float>(vB.x); bs.vy[cc.bodyB] = static_cast<float>(vB.y); bs.w[cc.bodyB] = static_cast<float>(wB); }
        }
    };

    // Build a PREPARED 1-point contact between dynamic A (slot bA) and dynamic
    // B (slot bB), with effective masses already filled (as PrepareContacts would).
    // Geometry is a simple head-on normal contact with a slight penetration.
    ContactConstraint MakePreparedContact(std::uint32_t bA, std::uint32_t bB, float seed)
    {
        ContactConstraint cc{};
        cc.bodyA = bA; cc.bodyB = bB; cc.bodyBIsBody = true;
        cc.invMassA = Real(0.5); cc.invInertiaA = Real(0.2);
        cc.invMassB = Real(0.5); cc.invInertiaB = Real(0.2);
        cc.normal = Vec2(Real(0), Real(-1));            // B->A push (A above B)
        cc.friction = Real(0.4); cc.restitution = Real(0.2);
        cc.biasRate = Real(-2.0); cc.massScale = Real(0.7); cc.impulseScale = Real(0.1);
        cc.pointCount = 1;
        ContactConstraintPoint& cp = cc.points[0];
        cp.anchorA = Vec2(Real(0.1) * seed, Real(-1));
        cp.anchorB = Vec2(Real(0.1) * seed, Real(1));
        cp.baseSeparation = Real(-0.05);                // small penetration
        cp.normalMass = Real(1.0); cp.tangentMass = Real(1.0);
        cp.normalImpulse = Real(0.3) * seed;            // a warm-start seed
        cp.tangentImpulse = Real(0.05) * seed;
        cp.relativeVelocity = Real(-30) - seed;         // approaching (restitution-eligible)
        cp.id = static_cast<std::uint32_t>(seed) + 1u;
        return cc;
    }

    // Run the full substep solve on `batches` over `bs` (lane-wide path).
    void RunLaneWide(std::vector<ContactConstraintSimd>& batches, BodyStateSoA& bs,
                     int substeps, float h, float maxBiasVel, float threshold,
                     const std::vector<std::uint32_t>& dynSlots)
    {
        // No integrate-velocities here -- we isolate the SOLVE math (gravity is
        // covered by the [physics] world-level invariants). dp/dq accumulate
        // between the bias + relax passes (the TGS separation re-eval).
        for (int s = 0; s < substeps; ++s)
        {
            SimdSolve::WarmStart(batches, bs);
            SimdSolve::SolveNormalAndFriction(batches, bs, h, /*useBias=*/true, maxBiasVel);
            for (std::uint32_t i : dynSlots) { bs.dpx[i] += bs.vx[i] * h; bs.dpy[i] += bs.vy[i] * h; bs.dq[i] += bs.w[i] * h; }
            SimdSolve::SolveNormalAndFriction(batches, bs, h, /*useBias=*/false, maxBiasVel);
        }
        SimdSolve::ApplyRestitution(batches, bs, threshold);
    }
} // namespace

TEST_CASE("PhysicsSimd: lane-wide solve is packing-width invariant + matches scalar",
          "[physics][simd]")
{
    constexpr int W = ContactConstraintSimd::kWidth;
    // N independent contacts: each between a unique pair of dynamic bodies
    // (2*N dynamic slots, all disjoint) so coloring puts them ALL in one color.
    const int N = W; // exactly one full wide batch
    const std::uint32_t bodyCount = static_cast<std::uint32_t>(2 * N);
    const std::int32_t dummyIndex = static_cast<std::int32_t>(bodyCount);

    std::vector<ContactConstraint> ccs;
    std::vector<std::uint32_t> refs, dynSlots;
    for (int i = 0; i < N; ++i)
    {
        const std::uint32_t bA = static_cast<std::uint32_t>(2 * i);
        const std::uint32_t bB = static_cast<std::uint32_t>(2 * i + 1);
        ccs.push_back(MakePreparedContact(bA, bB, static_cast<float>(i + 1)));
        refs.push_back(static_cast<std::uint32_t>(i));
        dynSlots.push_back(bA); dynSlots.push_back(bB);
    }

    // Seed a BodyStateSoA (+1 dummy slot) with known velocities.
    auto seedSoA = [&](BodyStateSoA& bs) {
        bs.Resize(bodyCount + 1u);
        for (std::uint32_t b = 0; b < bodyCount; ++b)
        {
            bs.vx[b] = 0.5f * static_cast<float>(b) - 1.0f;
            bs.vy[b] = (b % 2 == 0) ? 40.0f : -10.0f;   // A's fall onto B's
            bs.w[b]  = 0.01f * static_cast<float>(b);
        }
    };

    const int substeps = 4;
    const float h = (1.0f / 60.0f) / static_cast<float>(substeps);
    const float maxBiasVel = 4.0f;
    const float threshold = 1.0f;

    // ---- Path A: lane-wide, ONE wide batch (W lanes) -----------------------
    BodyStateSoA bsWide; seedSoA(bsWide);
    {
        std::vector<ContactConstraint> wccs = ccs;   // local copy (impulses mutate)
        std::vector<ContactConstraintSimd> batches =
            ContactConstraintSimd::Build(wccs.data(), refs.data(), N, dummyIndex);
        REQUIRE(batches.size() == 1u);
        RunLaneWide(batches, bsWide, substeps, h, maxBiasVel, threshold, dynSlots);
    }

    // ---- Path B: lane-wide, NARROW -- N batches of 1 live lane + padding ----
    // Same SimdSolve passes, different packing width per batch. Lane-width
    // invariance => bit-identical to Path A.
    BodyStateSoA bsNarrow; seedSoA(bsNarrow);
    {
        std::vector<ContactConstraint> nccs = ccs;
        std::vector<ContactConstraintSimd> batches;
        for (int i = 0; i < N; ++i)
        {
            std::uint32_t one = static_cast<std::uint32_t>(i);
            std::vector<ContactConstraintSimd> b1 =
                ContactConstraintSimd::Build(nccs.data(), &one, 1, dummyIndex);
            REQUIRE(b1.size() == 1u);
            batches.push_back(b1[0]);
        }
        RunLaneWide(batches, bsNarrow, substeps, h, maxBiasVel, threshold, dynSlots);
    }

    // BIT-IDENTICAL: packing width does not change the result (the core invariant).
    for (std::uint32_t b = 0; b < bodyCount; ++b)
    {
        CHECK(bsWide.vx[b] == bsNarrow.vx[b]);
        CHECK(bsWide.vy[b] == bsNarrow.vy[b]);
        CHECK(bsWide.w[b]  == bsNarrow.w[b]);
    }

    // ---- Path C: width-1 SCALAR oracle (plain float, no Simd wrapper) -------
    BodyStateSoA bsScalar; seedSoA(bsScalar);
    {
        std::vector<ContactConstraint> sccs = ccs;
        ScalarRef ref{ bsScalar, Real(h), Real(1.0 / h), Real(maxBiasVel), Real(threshold) };
        for (int s = 0; s < substeps; ++s)
        {
            for (auto& cc : sccs) { ref.WarmStart(cc); }
            for (auto& cc : sccs) { ref.Solve(cc, /*useBias=*/true); }
            for (std::uint32_t i : dynSlots) { bsScalar.dpx[i] += bsScalar.vx[i] * h; bsScalar.dpy[i] += bsScalar.vy[i] * h; bsScalar.dq[i] += bsScalar.w[i] * h; }
            for (auto& cc : sccs) { ref.Solve(cc, /*useBias=*/false); }
        }
        for (auto& cc : sccs) { ref.Restitution(cc); }
    }

    // SCALAR-ORACLE MATCH within 1e-5: the lane-wide AVX2 math equals the plain-
    // float reference (a transcription error would show here). Per-op float
    // rounding differs only at the ~1e-6 ulp scale for these magnitudes.
    for (std::uint32_t b = 0; b < bodyCount; ++b)
    {
        CHECK(bsWide.vx[b] == Approx(bsScalar.vx[b]).margin(1e-5));
        CHECK(bsWide.vy[b] == Approx(bsScalar.vy[b]).margin(1e-5));
        CHECK(bsWide.w[b]  == Approx(bsScalar.w[b]).margin(1e-5));
    }
}

TEST_CASE("PhysicsSimd: padding + read-only-B lanes cannot corrupt body slot 0",
          "[physics][simd]")
{
    // The SCATTER-CORRUPTION guard. Build ONE batch where:
    //   lane 0 = a real DYNAMIC contact whose A is BODY SLOT 0 (the slot a naive
    //            "clamp read-only/padding index to 0" would clobber),
    //   lane 1 = a read-only-B contact (B kinematic) whose A is a different dynamic,
    //   lanes 2..W-1 = PADDING.
    // With the dummy slot at world.Count(), padding + the read-only B's WRITE-BACK
    // never touch slot 0. We solve the batch and assert body 0's velocity equals
    // the scalar-oracle value (a corruption would zero/stale it).
    constexpr int W = ContactConstraintSimd::kWidth;
    if (W < 2) { SUCCEED("scalar backend: single lane, no padding to corrupt"); return; }

    // Bodies: slot 0 (dyn A of lane0), slot 1 (dyn B of lane0), slot 2 (dyn A of
    // lane1), slot 3 (kinematic B of lane1, invMassB=0). dummy = slot 4.
    const std::uint32_t bodyCount = 4u;
    const std::int32_t dummyIndex = static_cast<std::int32_t>(bodyCount);

    std::vector<ContactConstraint> ccs;
    ccs.push_back(MakePreparedContact(0u, 1u, 1.0f));            // lane 0: dyn-dyn, A = slot 0
    ContactConstraint kin = MakePreparedContact(2u, 3u, 2.0f);  // lane 1: A=slot2, B=slot3
    kin.invMassB = Real(0); kin.invInertiaB = Real(0);          // make B kinematic (read-only)
    ccs.push_back(kin);
    std::vector<std::uint32_t> refs = { 0u, 1u };
    std::vector<std::uint32_t> dynSlots = { 0u, 1u, 2u };       // slot 3 is kinematic (read-only)

    auto seedSoA = [&](BodyStateSoA& bs) {
        bs.Resize(bodyCount + 1u);
        bs.vx[0] = 3.0f;  bs.vy[0] = 50.0f; bs.w[0] = 0.1f;     // body 0: a distinctive velocity
        bs.vx[1] = -1.0f; bs.vy[1] = -5.0f; bs.w[1] = 0.0f;
        bs.vx[2] = 2.0f;  bs.vy[2] = 30.0f; bs.w[2] = 0.05f;
        bs.vx[3] = 7.0f;  bs.vy[3] = 0.0f;  bs.w[3] = 0.0f;     // kinematic plate moving +x
    };

    const int substeps = 4;
    const float h = (1.0f / 60.0f) / static_cast<float>(substeps);
    const float maxBiasVel = 4.0f, threshold = 1.0f;

    // Lane-wide path (the batch has W lanes: 2 live + W-2 padding).
    BodyStateSoA bsWide; seedSoA(bsWide);
    {
        std::vector<ContactConstraint> wccs = ccs;
        std::vector<ContactConstraintSimd> batches =
            ContactConstraintSimd::Build(wccs.data(), refs.data(), 2, dummyIndex);
        REQUIRE(batches.size() == 1u);
        REQUIRE(batches[0].count == 2);     // 2 live lanes, the rest padding
        RunLaneWide(batches, bsWide, substeps, h, maxBiasVel, threshold, dynSlots);
    }

    // Scalar oracle (no lanes, no padding -> cannot corrupt by construction).
    BodyStateSoA bsScalar; seedSoA(bsScalar);
    {
        std::vector<ContactConstraint> sccs = ccs;
        ScalarRef ref{ bsScalar, Real(h), Real(1.0 / h), Real(maxBiasVel), Real(threshold) };
        for (int s = 0; s < substeps; ++s)
        {
            for (auto& cc : sccs) { ref.WarmStart(cc); }
            for (auto& cc : sccs) { ref.Solve(cc, true); }
            for (std::uint32_t i : dynSlots) { bsScalar.dpx[i] += bsScalar.vx[i] * h; bsScalar.dpy[i] += bsScalar.vy[i] * h; bsScalar.dq[i] += bsScalar.w[i] * h; }
            for (auto& cc : sccs) { ref.Solve(cc, false); }
        }
        for (auto& cc : sccs) { ref.Restitution(cc); }
    }

    // Body 0 (slot 0) is NOT corrupted: its solved velocity matches the oracle.
    // A padding/read-only lane clobbering slot 0 would make these diverge wildly.
    CHECK(bsWide.vx[0] == Approx(bsScalar.vx[0]).margin(1e-5));
    CHECK(bsWide.vy[0] == Approx(bsScalar.vy[0]).margin(1e-5));
    CHECK(bsWide.w[0]  == Approx(bsScalar.w[0]).margin(1e-5));
    // And it genuinely moved (the contact solve changed it -> a real, not trivial,
    // velocity that corruption could destroy).
    CHECK(bsWide.vy[0] != Approx(50.0f));

    // The kinematic plate (slot 3, read-only B) was NOT mutated by the solve
    // (its velocity is preserved through the read-only-B write-back).
    CHECK(bsWide.vx[3] == Approx(7.0f));
    CHECK(bsWide.vy[3] == Approx(0.0f));
    CHECK(bsWide.w[3]  == Approx(0.0f));
}

// ===========================================================================
// Overflow (un-colorable) contacts settle + stay bounded.
//
// Coloring spills a constraint when one DYNAMIC endpoint already occupies all
// kColorCount colors (a hub dynamic body sharing a dynamic endpoint with >
// kColorCount contacts). Those overflow refs are solved SEQUENTIALLY, scalar,
// over the same BodyStateSoA -- they must NOT be dropped (or the hub would sink
// /explode). We build a central dynamic disk surrounded by a dense ring of many
// dynamic disks all overlapping it (so the hub is a dynamic endpoint in > 12
// dynamic-dynamic contacts -> guaranteed overflow), drop it onto a floor under
// gravity, and assert the whole cluster settles to a bounded rest (energy bled
// off, nothing flung). Tag [physics][simd].
// ===========================================================================
TEST_CASE("PhysicsSimd: overflow (un-colorable hub) contacts settle + bounded",
          "[physics][simd]")
{
    WorldDef wd;
    wd.gravityY   = Real(400);
    wd.solverKind = SolverKind::SoftStep;
    PhysicsWorld w(wd);

    // Static floor.
    const Real floorTop = Real(300);
    {
        BodyDef bd;
        bd.type     = BodyType::Static;
        bd.position = Vec2(Real(0), floorTop + Real(5));
        bd.shape    = MakeAabb(Real(200), Real(5));
        w.AddBody(bd);
    }

    // Central HUB dynamic disk resting just above the floor.
    BodyHandle hub;
    {
        BodyDef bd;
        bd.type        = BodyType::Dynamic;
        bd.position    = Vec2(Real(0), floorTop - Real(12));
        bd.shape       = MakeCircle(Real(12));
        bd.density     = Real(1);
        bd.friction    = Real(0.4f);
        bd.restitution = Real(0);
        hub = w.AddBody(bd);
    }

    // A dense ring of small dynamic disks all OVERLAPPING the hub (so each forms
    // a dynamic-dynamic contact with it). 20 > kColorCount (12) -> the hub is an
    // endpoint in > 12 dynamic-dynamic contacts -> coloring overflows for the
    // excess, exercising the scalar overflow path.
    const int kRing = 20;
    std::vector<BodyHandle> ring;
    for (int i = 0; i < kRing; ++i)
    {
        const Real ang = (Real(2) * Real(3.14159265358979323846)) * Real(i) / Real(kRing);
        // Place each small disk so it overlaps the hub's rim (centre ~ hub R).
        const Real rr = Real(12);
        BodyDef bd;
        bd.type        = BodyType::Dynamic;
        bd.position    = Vec2(std::cos(ang) * rr, floorTop - Real(12) + std::sin(ang) * rr);
        bd.shape       = MakeCircle(Real(3));
        bd.density     = Real(1);
        bd.friction    = Real(0.4f);
        bd.restitution = Real(0);
        ring.push_back(w.AddBody(bd));
    }

    // Sanity: the scene actually overflowed at least once during settling (else
    // this test would silently not exercise the overflow path). We watch the
    // active-contact count proxy: a hub touching 20 dynamics yields well over 12
    // dynamic-dynamic contacts. (No direct overflow accessor; the bound + the
    // dense dynamic-dynamic fan guarantee it by construction.)
    REQUIRE(w.Count() >= static_cast<std::uint32_t>(kRing + 2));

    // Settle (4 s). Track peak per-mass KE of every dynamic -> a dropped/over-
    // packed overflow would explode (huge KE) or sink (hub through the floor).
    const Real kStep = Real(1) / Real(60);
    Real peakKE = Real(0);
    const Real keBound = Real(4) * Real(400) * Real(60); // generous (g * drop, x4)
    for (int s = 0; s < 240; ++s)
    {
        w.Step(kStep);
        const Vec2 vh = w.Velocity(hub);
        peakKE = std::max(peakKE, Real(0.5) * (vh.x * vh.x + vh.y * vh.y));
        for (BodyHandle b : ring)
        {
            const Vec2 v = w.Velocity(b);
            const Real ke = Real(0.5) * (v.x * v.x + v.y * v.y);
            peakKE = std::max(peakKE, ke);
            REQUIRE(ke < keBound);               // never blows up (overflow not dropped)
        }
    }

    // The hub did not sink through the floor (overflow contacts held it up).
    CHECK(w.Position(hub).y + Real(12) <= floorTop + Real(1));
    // The hub came to rest (the overflow solve dissipated its energy).
    const Vec2 vhf = w.Velocity(hub);
    CHECK(Real(0.5) * (vhf.x * vhf.x + vhf.y * vhf.y) < Real(200));
    INFO("overflow-hub peak per-mass KE = " << static_cast<double>(peakKE));
}
