#pragma once

// Contact.hpp -- persistent fixture-pair contact + a deduped, generation-safe
// pool of them (Arcane 2D physics, collision rebuild Phase 3, Task 1).
//
// One Contact is the durable record for a single solver-relevant overlapping
// fixture-pair. It SURVIVES across steps (so warm-starting + per-pair manifold
// caching have a stable home); the broadphase creates/destroys the pool entries
// and the narrowphase recomputes the cached Manifold at most once per step.
//
// ContactPool is the storage: a free-list-backed vector of Contacts indexed by
// a stable contact id, deduped by the UNORDERED fixture-slot pair, and
// generation-safe -- a recycled fixture slot (same index, bumped generation)
// is a DISTINCT key, so a stale contact never aliases a new fixture that lands
// in the same slot.
//
// PHASE 3 NOTE: this is a pure data structure -- defined + unit-tested but NOT
// yet consumed by the Step path, so it introduces no behavior change. Events
// stay in ContactManager; the Contact carries only what the solver feed needs.
//
// PRESENTATION-FREE + C++20-clean: std + sibling Physics headers only. No
// SDL3/NVRHI/Batcher2D/ImGui. Compiles both /MD (Arcane.dll) and static-CRT
// (ArcaneCore, server flavor).

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <Arcane/Physics/Fixture.hpp>        // pulls in PhysicsTypes.hpp (kInvalidSlot)
#include <Arcane/Physics/Narrowphase/Manifold.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // Contact: one persistent contact per solver-relevant overlapping
        // fixture-pair. Survives across steps; the manifold is recomputed at
        // most once per step (a future UpdateContacts). Phase 3 carries only
        // what the solver feed needs (events stay in ContactManager).
        // ----------------------------------------------------------------
        struct Contact
        {
            FixtureHandle a{};                      // canonical: A's body is dynamic (oriented at create)
            FixtureHandle b{};
            std::uint32_t bodyA = kInvalidSlot;     // cached body SLOT for A (kInvalidSlot until the caller fills it)
            std::uint32_t bodyB = kInvalidSlot;     // cached body SLOT for B (kInvalidSlot for a tile span / until filled)
            bool          bIsBody = true; // false => B is a tile-span virtual fixture
            Manifold      manifold{};     // last computed (pointCount 0 => not touching)
            bool          touching = false;
            // SOLVER vs EVENT relevance (collision-rebuild Phase 4, Task 1). The
            // pool now holds the EVENT union -- sensors + kinematic-kinematic +
            // kinematic-vs-static-body in addition to the solver-relevant pairs.
            // This flag is the OLD create filter: true iff (da||db) && !sensorA &&
            // !sensorB, i.e. dynamic-dynamic / dynamic-kinematic / dynamic-static,
            // non-sensor. EmitContactConstraints emits a ContactConstraint ONLY for
            // solverRelevant contacts, so the solver feed stays byte-identical to
            // Phase 3 even though the pool is now a superset. Set at create time.
            bool          solverRelevant = false;
        };

        // ----------------------------------------------------------------
        // ContactPool: pooled, deduped by the unordered fixture-slot pair,
        // generation-safe.
        // ----------------------------------------------------------------
        class ContactPool
        {
        public:
            static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

            // Result of EnsurePair: the contact id plus an EXPLICIT created/existing
            // signal. `created == true` means a fresh slot was minted (its body
            // slots are kInvalidSlot -- the caller MUST fill them via Get(id));
            // `created == false` means the existing pair was returned untouched.
            // This is reliable under churn, where the old "Count grew" heuristic
            // (a destroy+create in the same step leaves Count unchanged) is not.
            struct EnsureResult
            {
                std::uint32_t id      = kNone;
                bool          created = false;
            };

            // Create-if-absent. On a MISS returns { newId, true } with the slot
            // freshly reset (body slots kInvalidSlot); the caller fills body
            // slots/orientation via Get(id). On a HIT returns { existingId, false }
            // WITHOUT touching the existing contact. Order-independent:
            // EnsurePair(a,b) and EnsurePair(b,a) resolve to the same id.
            EnsureResult EnsurePair(FixtureHandle a, FixtureHandle b);

            // kNone if absent.
            std::uint32_t Find(FixtureHandle a, FixtureHandle b) const;

            void Destroy(std::uint32_t id);

            Contact& Get(std::uint32_t id)
            {
                assert(id < m_pool.size() && m_alive[id] && "ContactPool::Get on a dead/invalid id");
                return m_pool[id];
            }
            const Contact& Get(std::uint32_t id) const
            {
                assert(id < m_pool.size() && m_alive[id] && "ContactPool::Get on a dead/invalid id");
                return m_pool[id];
            }
            std::size_t Count() const { return m_live; }

            // Visit live contacts in ascending id order (deterministic). A const
            // overload (Phase 3 Task 3 walks the pool read-only).
            void ForEach(const std::function<void(std::uint32_t id, Contact&)>& fn);
            void ForEach(const std::function<void(std::uint32_t id, const Contact&)>& fn) const;
            void Clear();

        private:
            // Mix each handle's index + generation into a 32-bit half, then
            // pack (min<<32)|max so a recycled slot (new gen) never aliases and
            // the pair is order-independent. The per-handle mix is a NON-cryptographic
            // index ^ (generation * prime) scramble, so a full 64-bit key collision
            // is theoretically possible but negligible for realistic slot/generation
            // ranges (revisit if either field approaches 2^32).
            static std::uint64_t Key(FixtureHandle a, FixtureHandle b);

            std::vector<Contact>       m_pool;   // indexed by id; holes reused via m_free
            std::vector<std::uint8_t>  m_alive;  // 1 if id is a live contact
            std::vector<std::uint32_t> m_free;   // recycled ids
            std::unordered_map<std::uint64_t, std::uint32_t> m_index; // Key -> id
            std::size_t m_live = 0;
        };

    } // namespace Physics
} // namespace Arcane
