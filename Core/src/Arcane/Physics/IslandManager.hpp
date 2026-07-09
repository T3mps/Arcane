#pragma once

// IslandManager: owns island TOPOLOGY -- the persistent island registry (per-body
// island id + the id-indexed record pool + free list), merge/split, and the
// SplitIsland connected-component rebuild. Extracted from PhysicsWorld as a
// contained collaborator (PhysicsWorld decomposition step 1, 2026-07-09),
// mirroring Box2D v3's b2Island (connectivity topology + the sleep trigger) as
// distinct from:
//   * b2ConstraintGraph -- the coloring / solve partition, which stays with the
//     contact pool for now (a future Step-2 collaborator), and
//   * b2SolverSet -- the hot awake/kinematic sets, which STAY world-owned as flat
//     SoA: m_awakeIndex[slot] is read per-body every substep (and handed as a raw
//     pointer into the SIMD packer), and the awake-set element ORDER feeds
//     persistent contact coloring -- a determinism tripwire. "Behavior over
//     shared hot SoA -> keep flat" (the data-oriented design constraint).
//
// DELEGATION: like ContactManager, IslandManager owns its state outright and takes
// PhysicsWorld& per method to reach the flat body/fixture/contact SoA + the
// awake-set MECHANISM (AddToAwakeSet) it does not own. Unlike ContactManager's
// narrow public "port seam", SplitIsland/WakeIsland need intimate world state (the
// contact pool + joint edges + the body-contact adjacency + the awake flag/timer),
// so PhysicsWorld befriends IslandManager -- the SAME trust boundary this code had
// when it lived inside the world. This keeps the extraction a verbatim,
// byte-identity-preserving move rather than a new public accessor surface.
//
// Split-linkage adjacency (m_bodyContacts) is still owned by PhysicsWorld and read
// through the friend here; it relocates in decomp step 2 (Task 3). The awake/
// kinematic sets + m_awake flag + sleep-pass (Island::UpdateSleep) stay for now.
//
// PRESENTATION-FREE + C++20-clean: std + sibling Physics headers only.
// namespace Arcane::Physics, Core style.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Island.hpp> // Island registry struct + constants (Phase A)
#include <Arcane/Util/FunctionRef.hpp>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld; // IslandManager reaches the world SoA (befriended).

        class IslandManager
        {
        public:
            // ---- registry query (inline; all cold / seam-time, NONE in the substep
            //      solve -- see pwdecomp survey 2, Part B5) --------------------------
            // m_islandId[slot] (or kInvalidIsland). Inline -> zero call cost.
            [[nodiscard]] std::uint32_t IslandOf(std::uint32_t slot) const noexcept
            {
                return slot < m_islandId.size() ? m_islandId[slot] : Island::kInvalidIsland;
            }
            // Per-body island-id column size (== body-slot capacity). Bounds guard.
            [[nodiscard]] std::size_t IslandIdCount() const noexcept { return m_islandId.size(); }
            // Island ROOT of slot i (debug/inspection, Phase A): the persistent id IS
            // the root; a non-member returns a high-bit-tagged slot (never collides).
            [[nodiscard]] std::uint32_t IslandRootOf(std::uint32_t i) const noexcept;

            // Visit each LIVE island's member-slot list, ascending island-id
            // (deterministic). A live island has a non-empty member list; freed ids
            // (empty) are skipped. Const callback (the sleep pass mutates bodies
            // through the world's slot accessors, not the island record).
            void ForEachIsland(
                FunctionRef<void(const std::vector<std::uint32_t>&)> fn) const
            {
                for (const Island::Island& isl : m_islands)
                {
                    if (!isl.bodies.empty())
                    {
                        fn(isl.bodies);
                    }
                }
            }

            // ---- registry lifecycle (Phase A) -------------------------------
            // Mint or reuse an island id (empty members, not a split candidate).
            std::uint32_t AllocIsland();
            // Return an island id to the free list (clears members + flag).
            void          FreeIsland(std::uint32_t id) noexcept;
            // Weighted union: relabel the smaller island's members into the larger,
            // free the smaller, return the survivor id. Pass two DISTINCT live ids.
            std::uint32_t MergeIslands(std::uint32_t idA, std::uint32_t idB);
            // Flag an island for the deferred split pass (no-op for kInvalidIsland).
            void          MarkSplitCandidate(std::uint32_t islandId) noexcept;
            // Rebuild one candidate island into 1+ connected components (fresh local
            // UF over its members' current touching pool contacts + joint edges);
            // clears the flag. Reaches w.m_bodyContacts / w.m_contactPool / w.m_joints.
            void          SplitIsland(PhysicsWorld& w, std::uint32_t islandId);
            // Wake every member of the body's island (set awake flag + reset timer +
            // re-add to the world's awake-set).
            void          WakeIsland(PhysicsWorld& w, std::uint32_t slot) noexcept;

            // ---- body-lifecycle seams (PhysicsWorld drives these) -----------
            // Grow the per-body columns to `next` (EnsureCapacity seam).
            void Grow(std::uint32_t next);
            // A new dynamic body is its own 1-body island (AddBody seam).
            void CreateSingletonIsland(std::uint32_t slot);
            // Mark a slot as belonging to no island (static/kinematic; AddBody seam).
            void ClearIsland(std::uint32_t slot) noexcept;
            // Release a destroyed body from its island: swap-erase membership, then
            // free the island if empty or mark it a split candidate (RemoveBody seam).
            void RemoveBodyFromIsland(std::uint32_t slot) noexcept;
            // Collect (clear + fill) the split-candidate island ids, ascending id
            // (Step stage 5 seam; determinism).
            void CollectSplitCandidates(std::vector<std::uint32_t>& out) const;

        private:
            // SplitIsland scratch sentinel: member body slot -> local DSU index.
            static constexpr std::uint32_t kSplitLocalNone = 0xFFFFFFFFu;

            // m_islandId[slot] is the body's persistent island id (id == root);
            // Island::kInvalidIsland for Static/Kinematic or an un-assigned slot.
            // m_islands is the id-indexed record pool; freed ids recycle via
            // m_islandFree. Maintained incrementally at the lifecycle seams above.
            std::vector<std::uint32_t>  m_islandId;        // per-body island id
            std::vector<Island::Island> m_islands;         // id-indexed record pool
            std::vector<std::uint32_t>  m_islandFree;      // recycled island ids
            // SplitIsland scratch: member body slot -> local DSU index, O(1).
            // All-sentinel; SplitIsland writes only its members then resets them.
            std::vector<std::uint32_t>  m_splitLocalIndex;
        };
    } // namespace Physics
} // namespace Arcane
