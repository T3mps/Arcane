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

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Solver/BodyStateSoA.hpp>

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
