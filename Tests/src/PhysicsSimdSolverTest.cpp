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
