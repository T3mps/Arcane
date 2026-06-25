#pragma once

// Island: the constraint-graph + sleep module (M6, Task P2.4).
//
// PORT NOTE: there is NO Island.lua -- the island/sleep logic lives INLINE in
// Client/src/physics/PhysicsWorld.lua (the union-find ufFind at lines 28-31 and
// the step() stage-5 island-sleep bookkeeping at lines 403-452). P2.4 EXTRACTS
// that inline logic into this clean module so the Step pipeline names a single
// island pass at the [P2.4 islands] seam. The behavior is a faithful port:
//
//   * Build a union-find constraint graph over the body slots: bodies are
//     nodes; THIS step's contacts (the GenerateContacts output) are edges. Two
//     bodies are unioned only when BOTH are DYNAMIC (a dynamic-vs-static /
//     kinematic / tile-span contact does NOT union -- statics anchor, they are
//     not island members). Ports PhysicsWorld.lua:411.
//   * Joint-attached dynamic bodies reset their sleep timer (jointed bodies
//     never sleep so target joints keep authority). Ports lines 415-423. The
//     joint list is empty until P2.5; the empty case is a no-op.
//   * Per-body sleep-timer update for awake dynamics: a body accumulates idle
//     time while its linear speed^2 < kSleepLinVel2 AND |angVel| < kSleepAngVel;
//     any faster motion resets the timer to 0. Ports lines 424-433.
//   * Island sleep: an island sleeps only when EVERY awake-dynamic member has
//     sleepT > kSleepTime. When it does, those members are set asleep (awake=0)
//     and their linear + angular velocities are zeroed. Ports lines 435-451.
//
// DETERMINISM (port + modernize): the graph is built by iterating contacts then
// bodies in STABLE SLOT INDEX order; ufFind is order-stable path-halving; the
// island sleep decision scans members by index. No wall clock, no fast math.
// Zero steady-state allocation: the union-find parent array is the world's
// pooled scratch (PhysicsWorld::UnionFindScratch -- the Lua w._uf), grown once
// per Step and reused.
//
// WAKE paths live ELSEWHERE (this module only puts islands to SLEEP):
//   * wake-on-contact -- PhysicsWorld::WakeMoverPair (a sleeping dynamic
//     touched by an awake mover wakes; ports lines 369-382).
//   * wake-on-force   -- PhysicsWorld::ApplyImpulse / SetVelocity / Wake (P2.1).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui. namespace Arcane::Physics, Core style.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld;        // the SoA owner; Island reads/writes through it
        struct ContactConstraint;  // Solver/Solver.hpp -- the GenerateContacts edges
        struct JointConstraint;    // Solver/Solver.hpp -- joint edges (empty until P2.5)

        namespace Island
        {
            // ----------------------------------------------------------------
            // Persistent island registry (Phase A -- replaces the per-step UF).
            // ----------------------------------------------------------------
            //
            // An Island is a connected component of DYNAMIC bodies joined by
            // touching dynamic-dynamic contacts. It SURVIVES across steps and is
            // maintained INCREMENTALLY at the body/contact lifecycle seams (create
            // = a 1-body island; touch-begin = merge; touch-end/destroy = mark a
            // split candidate; a deferred quota-limited pass rebuilds candidates).
            // Static/Kinematic bodies are NOT members (they anchor, they are not
            // island nodes -- the same dynamic-dynamic-only rule the old UF used).
            //
            // The registry lives on PhysicsWorld (m_islands, id-indexed, free-list
            // recycled). Sleep bookkeeping is DERIVED from members (per-body idle
            // timers stay per-body in m_sleepTimer); an island holds no timer.
            struct Island
            {
                // Member body SLOTS (dynamic only). Order is append-order; the
                // sleep + wake passes iterate it directly (no global scan).
                std::vector<std::uint32_t> bodies;

                // Set when a member's contact separated or a member was destroyed:
                // the deferred split pass (quota-limited, once per Step) re-derives
                // this island's connected components and clears the flag.
                bool splitCandidate = false;
            };

            // A body with no island (Static/Kinematic, or a transiently un-assigned
            // dynamic slot). m_islandId[slot] == kInvalidIsland.
            inline constexpr std::uint32_t kInvalidIsland = 0xFFFFFFFFu;

            // At most this many split-candidate islands are rebuilt per Step (Box2D
            // v3's value). Splits are deferred + amortized; the registry converges
            // over a few steps. Processed in ascending island-id order (determinism).
            inline constexpr std::uint32_t kMaxSplitsPerStep = 1u;

            // ---- sleep thresholds (ports the Lua magic numbers verbatim) -----

            // Linear speed^2 below which a body counts as idle (Lua line 426:
            // velX^2 + velY^2 < 4, i.e. |v| < 2 world units/s).
            inline constexpr Real kSleepLinVel2 = Real(4);

            // |angVel| below which a body counts as idle (Lua line 427).
            inline constexpr Real kSleepAngVel = Real(0.05);

            // Idle time a body (and every island member) must accumulate before
            // the island sleeps (Lua lines 437/442: sleepT > 0.5 seconds).
            inline constexpr Real kSleepTime = Real(0.5);

            // ----------------------------------------------------------------
            // UpdateSleep: the per-Step island sleep pass (Step stage 5).
            // ----------------------------------------------------------------
            //
            // Phase A: iterates the PERSISTENT island registry (members already
            // known -- no per-step union-find rebuild, no O(n^2) global scan).
            // Resets the sleep timer of joint-attached dynamics (jointed bodies
            // never sleep so target joints keep authority), advances each awake
            // dynamic's idle timer by `dt`, then for each island sleeps it AS A
            // UNIT iff every awake-dynamic member is past kSleepTime (clearing
            // awake + zeroing linear & angular velocity).
            //
            // `joints`/`jointCount` is the joint constraint list (nullptr/0 if no
            // joints). `dt` is the full Step timestep. No-op when the world has no
            // dynamics. Thresholds + whole-island-unit + exact-freeze are UNCHANGED
            // from the global-UF version this replaces.
            void UpdateSleep(PhysicsWorld& world,
                             const JointConstraint* joints,
                             std::uint32_t jointCount,
                             Real dt);

        } // namespace Island
    } // namespace Physics
} // namespace Arcane
