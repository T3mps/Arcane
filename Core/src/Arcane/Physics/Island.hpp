#pragma once

// Island: the persistent island registry + sleep module (Phase A).
//
// This module owns the per-Step sleep pass (UpdateSleep) over the PERSISTENT
// island registry. The old per-step union-find graph build (M6, P2.4) has been
// replaced by a per-island O(island) walk over the registry (members already
// known -- no UF rebuild, no O(n^2) global scan).
//
// REGISTRY LIFECYCLE (maintained INCREMENTALLY at PhysicsWorld lifecycle seams):
//   * AddBody(Dynamic)  -> AllocIsland: a new dynamic body is its own 1-body island.
//   * touch-BEGIN       -> MergeIslands: two dynamic bodies in fresh contact merge
//     into the larger island (weighted union, O(smaller island)).
//   * touch-END/destroy -> MarkSplitCandidate: the island is flagged; a quota-
//     limited deferred pass (kMaxSplitsPerStep per Step) runs SplitIsland, which
//     re-derives the connected components via a local UF over the body's current
//     contacts. Split pass is O(island) and amortized over a few Steps.
//   * Static/Kinematic bodies are NOT island members (they anchor; they are not
//     island nodes -- identical to the old dynamic-dynamic-only union rule).
//
// UpdateSleep (Step stage 5): iterates the registry via PhysicsWorld::ForEachIsland,
// advances each awake dynamic body's idle timer, then sleeps an island AS A UNIT
// when EVERY awake-dynamic member has accumulated kSleepTime seconds of idle.
// The thresholds and whole-island-unit-sleep behavior are UNCHANGED from the M6
// global-UF version this replaces.
//
// WAKE paths live in PhysicsWorld (this module only puts islands to SLEEP):
//   * wake-on-contact -- PhysicsWorld::WakeMoverPair (a sleeping dynamic touched
//     by an awake mover wakes and pulls the whole island via WakeIsland).
//   * wake-on-force   -- PhysicsWorld::ApplyImpulse / SetVelocity / Wake.
//
// DETERMINISM: merges are applied in canonical (min,max)-slot order; splits
// iterate members by ascending slot index; ForEachIsland iterates ascending
// island id. No wall clock, no fast math.
//
// PRESENTATION-FREE + C++23-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui. namespace Arcane::Physics, Core style.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld;        // the SoA owner; Island reads/writes through it

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

            // ---- sleep threshold --------------------------------------------
            //
            // The per-body VELOCITY gate is no longer a pair of separate Lua-ported
            // constants. UpdateSleep now uses Box2D v3's combined, extent-weighted
            // test: a body is idle when |v| + |w|*maxExtent < its per-body
            // sleepThreshold (PhysicsWorld::SleepThresholdSlot, defaulted from
            // WorldDef::sleepThreshold). The old kSleepLinVel2 (=4) / kSleepAngVel
            // (=0.05) gates were too strict + body-size-unaware for the gravity-900
            // world and never let a slowly-rolling pile sleep (see the never-settle
            // findings doc). Only the TIME gate remains a constant here.

            // Idle time a body (and every island member) must accumulate before
            // the island sleeps (Lua lines 437/442: sleepT > 0.5 seconds).
            inline constexpr Real kSleepTime = Real(0.5);

            // ----------------------------------------------------------------
            // UpdateSleep: the per-Step island sleep pass (Step stage 5).
            // ----------------------------------------------------------------
            //
            // Phase A: iterates the PERSISTENT island registry (members already
            // known -- no per-step union-find rebuild, no O(n^2) global scan).
            // Advances each awake dynamic's idle timer by `dt`, then for each island
            // sleeps it AS A UNIT iff every awake-dynamic member is past kSleepTime
            // (clearing awake + zeroing linear & angular velocity).
            //
            // Jointed dynamics are NOT pinned awake here: a joint is an ISLAND EDGE
            // (AddJoint merges islands; SplitIsland unions joint edges), so a jointed
            // construct sleeps as a unit via island membership. `dt` is the full Step
            // timestep. No-op when the world has no dynamics. Thresholds +
            // whole-island-unit + exact-freeze are UNCHANGED from the global-UF
            // version this replaces.
            void UpdateSleep(PhysicsWorld& world, Real dt);

        } // namespace Island
    } // namespace Physics
} // namespace Arcane
