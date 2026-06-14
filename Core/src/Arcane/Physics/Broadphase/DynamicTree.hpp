#pragma once

// DynamicTree: dynamic AABB tree broadphase (M6, Task P1.6).
//
// PORT + MODERNIZE of Client/src/physics/AABBTree.lua. This is the DEFAULT
// mover broadphase (MODERNIZE: the Lua engine defaulted to SpatialHash).
//
// Faithful ports of the Lua algorithm:
//   * Fat-box leaves: each leaf stores a TIGHT box (the body's real AABB) and
//     a FAT box (tight grown by MARGIN). Insertion / queries descend on the
//     FAT box; narrowing (queryAABB / pairs) tests the TIGHT box so results
//     match SpatialHash / SweepAndPrune exactly.
//   * _insertLeaf descends by least combined-perimeter growth (the Lua's
//     "perim" -- a 2D SAH analogue), splitting the chosen leaf with a new
//     internal node.
//   * refit walks parents up, recomputing the union box.
//   * _removeLeaf promotes the sibling.
//   * update fast-path: if the new tight box still fits inside the stored FAT
//     box, only the tight box is updated (no reinsert) -- the Lua's coherent-
//     motion trick. Otherwise remove + reinsert with a fresh fat box.
//   * NO tree rotations (the Lua skips them; adequate at our scales).
//   * MARGIN = 8 (named constant, ported verbatim).
//
// MODERNIZE: nodes are POOLED in an index-based free-list (the Lua used GC
// tables). Node references are pool indices, not pointers, so there is zero
// per-insert heap traffic in steady state (the pool only `new`s to grow) and
// the structure is determinism-friendly (indices are stable + comparable).
// Query scratch (an explicit descent stack) and the pairs/query dedup buffers
// are pooled members reused across calls.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        class DynamicTree final : public IBroadphase
        {
        public:
            // Fat-box margin (AABBTree.lua MARGIN = 8). Grown around the tight
            // box so coherent motion rarely triggers a reinsert.
            static constexpr Real kMargin = Real(8);

            DynamicTree() = default;

            void Update(std::uint32_t id, const Aabb2& box) override;
            void Remove(std::uint32_t id) override;
            int  QueryAABB(const Aabb2& box,
                           std::vector<std::uint32_t>& out) const override;
            int  Pairs(std::vector<BroadphasePair>& out) const override;

        private:
            // Sentinel for "no node" (the Lua used nil parents/children).
            static constexpr std::uint32_t kNull = 0xFFFFFFFFu;

            // A pooled tree node. Internal nodes have two children; leaves have
            // kNull children, a body id, and a tight box. The `fat` box is the
            // node's bounds: for a leaf it is the tight box grown by MARGIN;
            // for an internal node it is the union of its children (the Lua's
            // x0..y1). Slots on the free-list chain via `parent` (== next).
            struct Node
            {
                Aabb2         fat{};                 // descent / query bounds
                Aabb2         tight{};               // leaf only: real body box
                std::uint32_t parent = kNull;        // also free-list "next"
                std::uint32_t left   = kNull;        // kNull => leaf
                std::uint32_t right  = kNull;
                std::uint32_t id     = 0;            // leaf only: body id

                [[nodiscard]] bool IsLeaf() const noexcept
                {
                    return left == kNull;
                }
            };

            // Allocate a node slot from the free-list (grows the pool only when
            // empty). Returns the slot index. Caller fills fields.
            std::uint32_t AllocNode();
            // Return a node slot to the free-list.
            void          FreeNode(std::uint32_t idx);

            // Lua _insertLeaf / _removeLeaf (operate on a leaf slot index).
            void InsertLeaf(std::uint32_t leaf);
            void RemoveLeaf(std::uint32_t leaf);
            // Lua refit: recompute union boxes from n up to the root.
            void Refit(std::uint32_t n);

            std::vector<Node>          m_nodes;     // node pool
            std::uint32_t              m_root      = kNull;
            std::uint32_t              m_freeList  = kNull;
            // id -> leaf slot index (the Lua's `leaves` table). Sized lazily;
            // entry kNull means "no such body".
            std::vector<std::uint32_t> m_leafOfId;

            // Reused scratch (no per-call heap). m_stack: descent stack for
            // queries. Mutable because QueryAABB / Pairs are const-by-contract
            // but reuse the scratch buffer.
            mutable std::vector<std::uint32_t> m_stack;

            // Map an id to its leaf slot (kNull if absent).
            [[nodiscard]] std::uint32_t LeafOf(std::uint32_t id) const noexcept;
            void SetLeafOf(std::uint32_t id, std::uint32_t leaf);
        };

    } // namespace Physics
} // namespace Arcane
