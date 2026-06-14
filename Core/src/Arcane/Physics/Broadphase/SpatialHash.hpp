#pragma once

// SpatialHash: uniform-grid broadphase (M6, Task P1.6).
//
// PORT + MODERNIZE of Client/src/physics/SpatialHash.lua. An alternate mover
// broadphase behind IBroadphase (the DEFAULT is DynamicTree).
//
// Faithful ports of the Lua algorithm:
//   * Uniform grid of cellSize (default 64). Buckets keyed kx*0x40000 + ky.
//   * Per-id range tracking {kx0,ky0,kx1,ky1}; update skips re-bucketing when
//     the cell range is unchanged (the Lua's update-skip-if-same-range).
//   * remove clears the id from its bucket range exactly.
//   * queryAABB dedups candidate ids via a generation-stamped "seen" scratch.
//
// MODERNIZE (uniform contract -- the key change over the Lua):
//   * The Lua's pairs() returned broad CANDIDATES (shared-bucket pairs); the
//     harness narrowed them afterward. Here pairs() narrows to TRUE TIGHT-BOX
//     OVERLAP and dedups, so it returns the IDENTICAL set as DynamicTree /
//     SweepAndPrune. queryAABB likewise narrows against the stored tight box
//     (the Lua returned all bucket members). Output is SORTED (ids ascending /
//     pairs by (a,b)) for determinism.
//
// MODERNIZE (zero steady-state alloc): buckets, the id->range map, and the
// dedup scratch are reused std containers (clear() preserves capacity). The
// only allocation churn after warmup is bucket growth on novel cells. We store
// each id's tight box so pairs()/queryAABB can narrow without a back-reference
// to the world.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        class SpatialHash final : public IBroadphase
        {
        public:
            // cellSize default 64 (SpatialHash.new(cellSize or 64)).
            explicit SpatialHash(Real cellSize = Real(64));

            void Update(std::uint32_t id, const Aabb2& box) override;
            void Remove(std::uint32_t id) override;
            int  QueryAABB(const Aabb2& box,
                           std::vector<std::uint32_t>& out) const override;
            int  Pairs(std::vector<BroadphasePair>& out) const override;

        private:
            // Integer cell range an id occupies (the Lua ranges[id] tuple).
            struct CellRange
            {
                std::int32_t kx0 = 0, ky0 = 0, kx1 = 0, ky1 = 0;
            };

            // Per-id record: its tight box (for narrowing) + cell range.
            struct Entry
            {
                Aabb2     box{};
                CellRange range{};
            };

            // Bucket key (the Lua bkey: kx*0x40000 + ky). Cells are bounded to
            // world scale, so this stays collision-free in practice.
            [[nodiscard]] static std::int64_t BucketKey(std::int32_t kx,
                                                        std::int32_t ky) noexcept
            {
                return static_cast<std::int64_t>(kx) * 0x40000 +
                       static_cast<std::int64_t>(ky);
            }

            // floor(v / cs) as an int cell coordinate.
            [[nodiscard]] std::int32_t CellOf(Real v) const noexcept;

            void RemoveFromBuckets(std::uint32_t id, const CellRange& r);

            Real m_cellSize = Real(64);

            // bucket key -> ids in that cell. unordered_map iteration order is
            // NOT used for output (pairs() iterates ids ascending via m_entries
            // and sorts), so determinism is preserved.
            std::unordered_map<std::int64_t, std::vector<std::uint32_t>> m_buckets;

            // id -> entry. Keyed by id; we iterate it sorted for deterministic
            // Pairs() (collect ids, sort, then walk).
            std::unordered_map<std::uint32_t, Entry> m_entries;

            // Generation-stamped dedup scratch (the Lua _seen / _gen). Reused
            // across queries; mutable because QueryAABB/Pairs are const.
            mutable std::unordered_map<std::int64_t, std::uint32_t> m_seen;
            mutable std::uint32_t                                   m_gen = 0;
            // Reused id list for sorted Pairs() iteration.
            mutable std::vector<std::uint32_t> m_idScratch;
        };

    } // namespace Physics
} // namespace Arcane
