#pragma once

// Broadphase interface for the Arcane 2D physics engine (M6, Task P1.6).
//
// PORT + MODERNIZE: the three Lua broadphases
// (Client/src/physics/AABBTree.lua, SpatialHash.lua, SAP.lua) share one
// surface -- Insert/Update/Remove/queryAABB/pairs. This header is the C++
// port of that surface as the abstract IBroadphase. The DynamicTree port
// (AABBTree.lua) is the DEFAULT mover broadphase (MODERNIZE: the Lua defaulted
// to SpatialHash); SpatialHash and SweepAndPrune are alternates behind the
// same interface. The world in P1.8 holds an IBroadphase*.
//
// DETERMINISM + UNIFORM CONTRACT (MODERNIZE -- the key behavioral change over
// the Lua):
//   * Pairs(out) returns TRUE-AABB-OVERLAP pairs ONLY. Every strategy performs
//     the final tight-box overlap test before emitting. The Lua SpatialHash
//     returned broad bucket CANDIDATES (the harness narrowed them afterward);
//     here SpatialHash narrows + dedups internally so ALL THREE strategies
//     emit the IDENTICAL set. This is the broadphase equivalence invariant
//     captured in Arcane/Tests/data/physics_oracle/broadphase.json.
//   * Pairs are emitted with a < b and the output is SORTED lexicographically
//     by (a, b). Strategy-independent + deterministic -- the Phase-2 solver
//     needs a fixed contact order. We do NOT leak std::unordered_map iteration
//     order into the output.
//   * QueryAABB returns ids SORTED ascending, narrowed against the TIGHT box
//     (the Lua AABBTree narrows tight; all strategies match it).
//
// ZERO STEADY-STATE ALLOCATION (MODERNIZE): the Lua used GC tables for tree
// nodes / buckets / scratch. The C++ implementations pool their internal
// storage (index-based node pool, reused scratch vectors) so there is no
// per-insert / per-Pairs heap traffic after warmup. Pairs/QueryAABB fill the
// caller's std::vector with clear()+push_back, which preserves capacity.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui. Compiles both /MD (Arcane.dll) and
// static-CRT/C++20 (project ArcaneCore, server flavor). namespace
// Arcane::Physics, Core style.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // Aabb2: the broadphase box. The Lua used a (x0,y0,x1,y1) tuple; we
        // reuse the engine's Shapes::Aabb { Vec2 min, max } so the world can
        // feed Shape::ComputeAABB output straight in without a conversion.
        // ----------------------------------------------------------------
        using Aabb2 = Aabb;

        // True iff two tight boxes overlap on both axes (inclusive, matching
        // the Lua "A.x0 <= B.x1 and B.x0 <= A.x1 and ..." test exactly). This
        // is the single narrowing predicate every strategy uses before
        // emitting a pair, so the emitted sets are identical.
        [[nodiscard]] inline bool AabbOverlap(const Aabb2& a,
                                              const Aabb2& b) noexcept
        {
            return a.min.x <= b.max.x && b.min.x <= a.max.x &&
                   a.min.y <= b.max.y && b.min.y <= a.max.y;
        }

        // ----------------------------------------------------------------
        // BroadphasePair: an overlapping id pair, always a < b.
        // ----------------------------------------------------------------
        struct BroadphasePair
        {
            std::uint32_t a = 0;
            std::uint32_t b = 0;

            friend constexpr bool operator==(const BroadphasePair& p,
                                             const BroadphasePair& q) noexcept
            {
                return p.a == q.a && p.b == q.b;
            }
            friend constexpr bool operator!=(const BroadphasePair& p,
                                             const BroadphasePair& q) noexcept
            {
                return !(p == q);
            }
            // Lexicographic order on (a, b) -- the deterministic emit order.
            friend constexpr bool operator<(const BroadphasePair& p,
                                            const BroadphasePair& q) noexcept
            {
                return p.a != q.a ? p.a < q.a : p.b < q.b;
            }
        };

        // ----------------------------------------------------------------
        // IBroadphase: the shared surface of all three strategies.
        // ----------------------------------------------------------------
        //
        // Ids are body indices (the Lua used integer ids). Update is an UPSERT
        // (insert-or-update), matching the Lua update-as-insert; there is no
        // separate Insert -- the first Update for an id inserts it.
        class IBroadphase
        {
        public:
            virtual ~IBroadphase() = default;

            // Upsert id with the given TIGHT box (insert if new, else move).
            virtual void Update(std::uint32_t id, const Aabb2& box) = 0;

            // Remove id. No-op if id is not present (matches the Lua guard).
            virtual void Remove(std::uint32_t id) = 0;

            // Ids whose TIGHT box overlaps box. out is cleared then filled,
            // SORTED ascending (deterministic). Returns out.size().
            virtual int QueryAABB(const Aabb2& box,
                                  std::vector<std::uint32_t>& out) const = 0;

            // All true-AABB-overlap pairs (a < b), SORTED lexicographically by
            // (a, b). out is cleared then filled. Returns out.size(). All three
            // strategies return the IDENTICAL set for the same scene.
            virtual int Pairs(std::vector<BroadphasePair>& out) const = 0;
        };

    } // namespace Physics
} // namespace Arcane
