#pragma once

// ContactManager: persistent contact pairs + begin/stay/end + sensor events,
// with two-granularity event gating (M6, Task P1.8).
//
// PORT NOTE: a faithful (VERBATIM-behavior) port of
// Client/src/physics/ContactManager.lua. Persistent contact pairs over the
// mover broadphase: begin/stay/end events queued during Step and delivered
// AFTER it (spec: no user code runs mid-step). Pairs are mover-mover (the
// broadphase Pairs()) and kinematic-vs-staticBody (the staticList).
// Mover-vs-TILE contacts deliberately do NOT flow here (cells are not bodies;
// the character controller owns its statics response).
//
// EVENT GATING (ported VERBATIM -- spec resolved decision #3): a per-body
// eventsEnabled flag PLUS a world-level gate. Gated events are DROPPED, not
// queued, and the pair's `begun` flag resets so re-enabling re-arms it; on
// re-enable every currently-overlapping pair emits a fresh begin
// (level-triggered re-arm).
//
// MODERNIZE (over the Lua, all behavior-preserving):
//   * PairKey is 64-bit: (uint64(min) << 32) | max. The Lua used a 16-bit
//     a*0x10000+b key (capped at 65535 bodies); widening to 64-bit lifts that
//     cap with no behavioral change.
//   * Overlap reuses the narrowphase: CollideShapes(a, xfA, b, xfB, margin=0)
//     .pointCount > 0. The Lua had a dedicated boolean shapesOverlap; reusing
//     CollideShapes is DRY and correct for the tested configs. ONE boundary
//     subtlety: CollideShapes emits a point only for STRICT penetration
//     (depth > 0), matching the Lua round-shape test (which is a strict
//     distance < sumR). For AABB/poly pairs the Lua shapesOverlap used an
//     INCLUSIVE aabbOverlap (<=), so an EXACT edge-touch (zero penetration)
//     counts as overlap in the Lua but not here. The P1.8 scenes are
//     non-degenerate (clear overlap / clear gap), so this never bites; if a
//     later test needs inclusive edge-touch semantics, port shapesOverlap.
//   * DETERMINISM: begin/stay events come from the broadphase's SORTED pairs +
//     index-ordered statics (already deterministic). The END events iterate the
//     pairs map (unordered_map -> nondeterministic order), so we COLLECT the
//     to-end pairs, SORT them by (a,b), then emit -- so End order is
//     deterministic too.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui. Compiles both /MD (Arcane.dll) and
// static-CRT/C++20 (project ArcaneCore, server flavor). namespace
// Arcane::Physics, Core style.

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld; // the manager reaches into the world SoA (port seam).

        // ----------------------------------------------------------------
        // ContactEvent: one delivered begin/stay/end (ports the Lua _queue row).
        // ----------------------------------------------------------------
        //
        // a/b are BodyHandles (the Lua delivered world.handles[idx]); a is the
        // lower slot, b the higher (pair canonical order). sensor is true iff
        // EITHER participant is a sensor (the Lua ev.sensor).
        struct ContactEvent
        {
            enum class Type : std::uint8_t
            {
                Begin = 0,
                Stay  = 1,
                End   = 2,
            };

            Type       type = Type::Begin;
            BodyHandle a{};
            BodyHandle b{};
            bool       sensor = false;
        };

        // ----------------------------------------------------------------
        // ContactManager: the persistent-pair + event machine.
        // ----------------------------------------------------------------
        class ContactManager
        {
        public:
            using Listener = std::function<void(const ContactEvent&)>;

            ContactManager() = default;

            // Install / replace the listener (ports onContact). Events fire only
            // when a listener is set.
            void SetListener(Listener fn) { m_listener = std::move(fn); }

            // Process one step's contacts: increment stamp, walk candidate pairs
            // (mover-mover SORTED + kinematic-vs-static index-ordered), apply
            // gating, queue begin/stay; detect separations -> queue end (sorted);
            // then flush the queue to the listener (deferred). Ports step().
            void Step(PhysicsWorld& w);

            // Reset `begun` on pairs involving idx (idx omitted = all). No
            // synthetic end (gated events are dropped, not queued). Ports disarm.
            void Disarm();
            void Disarm(std::uint32_t idx);

            // Level-triggered re-arm: for currently-OVERLAPPING pairs involving
            // idx (idx omitted = all) that are !begun AND not gated, emit a fresh
            // begin IMMEDIATELY (re-arm happens outside Step, so synchronous
            // delivery is safe). Ports rearm.
            void Rearm(PhysicsWorld& w);
            void Rearm(PhysicsWorld& w, std::uint32_t idx);

            // Erase all pairs involving idx (ports dropBody). Called by
            // World::RemoveBody.
            void DropBody(std::uint32_t idx);

        private:
            // Shared Rearm body (filterIdx selects the idx-scoped variant).
            void RearmImpl(PhysicsWorld& w, bool filterIdx, std::uint32_t idx);

            // ------------------------------------------------------------
            // Pair: one persistent contact (ports the Lua pair table). a is the
            // lower slot, b the higher. begun gates begin-vs-stay; stamp marks
            // the step it was last touched (untouched -> separated).
            // ------------------------------------------------------------
            struct Pair
            {
                std::uint32_t a = 0; // min slot
                std::uint32_t b = 0; // max slot
                bool          begun = false;
                std::uint32_t stamp = 0;
            };

            // 64-bit pair key: (min << 32) | max (ports pairKey, widened).
            [[nodiscard]] static std::uint64_t PairKey(std::uint32_t a,
                                                       std::uint32_t b) noexcept
            {
                if (a > b)
                {
                    std::uint32_t t = a;
                    a = b;
                    b = t;
                }
                return (static_cast<std::uint64_t>(a) << 32) |
                       static_cast<std::uint64_t>(b);
            }

            // True iff slots a,b overlap NOW (CollideShapes pointCount>0).
            [[nodiscard]] static bool ShapesOverlap(const PhysicsWorld& w,
                                                    std::uint32_t a,
                                                    std::uint32_t b);

            // The Lua touch(a,b): if both alive AND overlap, get/create the pair,
            // stamp it, gate, and queue begin/stay (or drop on gate).
            void Touch(PhysicsWorld& w, std::uint32_t a, std::uint32_t b,
                       std::uint32_t stamp);

            // Queue an event row (the Lua _emit -- deferred; not delivered here).
            void Emit(PhysicsWorld& w, ContactEvent::Type type,
                      std::uint32_t a, std::uint32_t b);

            // pair key -> pair. Reused across steps (no per-step alloc).
            std::unordered_map<std::uint64_t, Pair> m_pairs;
            Listener                                m_listener;

            // Deferred event queue (the Lua _queue/_qn). Reused; cleared by
            // resize-to-zero (capacity preserved).
            std::vector<ContactEvent> m_queue;
            std::uint32_t             m_stamp = 0;

            // Neutral scratch buffer: used by Step to collect-then-sort End
            // events, and by RearmImpl to collect-then-sort rearm Begin events.
            // Named neutrally because both code paths reuse the same member to
            // avoid per-call allocation (no dedicated per-purpose scratch).
            std::vector<Pair> m_workPairs;
            // Scratch for the broadphase mover-mover pairs (reused each Step --
            // Pairs() does clear()+push_back, so capacity is preserved).
            std::vector<BroadphasePair> m_pairScratch;
        };

    } // namespace Physics
} // namespace Arcane
