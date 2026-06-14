// SpatialHash.cpp -- uniform-grid broadphase.
//
// See SpatialHash.hpp for the contract. Faithful port of
// Client/src/physics/SpatialHash.lua with the modernized uniform contract:
// pairs()/queryAABB narrow to TRUE tight-box overlap (the Lua returned broad
// candidates) and emit SORTED output, so all three strategies agree.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Broadphase/SpatialHash.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    namespace Physics
    {
        SpatialHash::SpatialHash(Real cellSize) : m_cellSize(cellSize) {}

        std::int32_t SpatialHash::CellOf(Real v) const noexcept
        {
            // floor(v / cs) -- matches the Lua math.floor(x0 / cs). std::floor
            // keeps negative coords correct (floor(-0.1) == -1), unlike a plain
            // int truncation.
            return static_cast<std::int32_t>(
                std::floor(static_cast<double>(v) / static_cast<double>(m_cellSize)));
        }

        void SpatialHash::RemoveFromBuckets(std::uint32_t id, const CellRange& r)
        {
            for (std::int32_t kx = r.kx0; kx <= r.kx1; ++kx)
            {
                for (std::int32_t ky = r.ky0; ky <= r.ky1; ++ky)
                {
                    auto it = m_buckets.find(BucketKey(kx, ky));
                    if (it == m_buckets.end())
                    {
                        continue;
                    }
                    auto& bucket = it->second;
                    // swap-remove (the Lua removeFromBucket).
                    for (std::size_t i = bucket.size(); i-- > 0;)
                    {
                        if (bucket[i] == id)
                        {
                            bucket[i] = bucket.back();
                            bucket.pop_back();
                            break;
                        }
                    }
                }
            }
        }

        void SpatialHash::Update(std::uint32_t id, const Aabb2& box)
        {
            const CellRange nr{ CellOf(box.min.x), CellOf(box.min.y),
                                CellOf(box.max.x), CellOf(box.max.y) };

            auto it = m_entries.find(id);
            if (it != m_entries.end())
            {
                Entry& e = it->second;
                e.box    = box; // always refresh the tight box (for narrowing)
                const CellRange& r = e.range;
                if (r.kx0 == nr.kx0 && r.ky0 == nr.ky0 && r.kx1 == nr.kx1 &&
                    r.ky1 == nr.ky1)
                {
                    return; // same cell range: skip re-bucketing (Lua fast path)
                }
                RemoveFromBuckets(id, r);
                e.range = nr;
            }
            else
            {
                Entry e;
                e.box   = box;
                e.range = nr;
                m_entries.emplace(id, e);
            }

            for (std::int32_t kx = nr.kx0; kx <= nr.kx1; ++kx)
            {
                for (std::int32_t ky = nr.ky0; ky <= nr.ky1; ++ky)
                {
                    m_buckets[BucketKey(kx, ky)].push_back(id);
                }
            }
        }

        void SpatialHash::Remove(std::uint32_t id)
        {
            auto it = m_entries.find(id);
            if (it == m_entries.end())
            {
                return; // Lua guard: not present
            }
            RemoveFromBuckets(id, it->second.range);
            m_entries.erase(it);
        }

        int SpatialHash::QueryAABB(const Aabb2& box,
                                   std::vector<std::uint32_t>& out) const
        {
            out.clear();
            ++m_gen;
            const std::int32_t kx0 = CellOf(box.min.x);
            const std::int32_t ky0 = CellOf(box.min.y);
            const std::int32_t kx1 = CellOf(box.max.x);
            const std::int32_t ky1 = CellOf(box.max.y);

            for (std::int32_t kx = kx0; kx <= kx1; ++kx)
            {
                for (std::int32_t ky = ky0; ky <= ky1; ++ky)
                {
                    auto it = m_buckets.find(BucketKey(kx, ky));
                    if (it == m_buckets.end())
                    {
                        continue;
                    }
                    for (const std::uint32_t cand : it->second)
                    {
                        // dedup across cells (Lua seen[id] ~= gen)
                        auto& stamp = m_seen[cand];
                        if (stamp == m_gen)
                        {
                            continue;
                        }
                        stamp = m_gen;
                        // MODERNIZE: narrow against the stored tight box so the
                        // result matches DynamicTree / SweepAndPrune (the Lua
                        // returned every bucket member).
                        const auto e = m_entries.find(cand);
                        if (e != m_entries.end() && AabbOverlap(e->second.box, box))
                        {
                            out.push_back(cand);
                        }
                    }
                }
            }

            std::sort(out.begin(), out.end());
            return static_cast<int>(out.size());
        }

        int SpatialHash::Pairs(std::vector<BroadphasePair>& out) const
        {
            out.clear();
            ++m_gen;

            // Iterate ids ASCENDING so candidate generation is deterministic;
            // for each id, sweep its own cells and test every co-bucket member.
            // (Iterating m_buckets directly would leak unordered_map order; we
            // drive from the entry list instead, but the dedup makes the
            // emitted SET order-independent anyway -- we still sort at the end.)
            m_idScratch.clear();
            m_idScratch.reserve(m_entries.size());
            for (const auto& kv : m_entries)
            {
                m_idScratch.push_back(kv.first);
            }
            std::sort(m_idScratch.begin(), m_idScratch.end());

            for (const std::uint32_t a : m_idScratch)
            {
                const Entry& ea = m_entries.at(a);
                for (std::int32_t kx = ea.range.kx0; kx <= ea.range.kx1; ++kx)
                {
                    for (std::int32_t ky = ea.range.ky0; ky <= ea.range.ky1; ++ky)
                    {
                        auto it = m_buckets.find(BucketKey(kx, ky));
                        if (it == m_buckets.end())
                        {
                            continue;
                        }
                        for (const std::uint32_t c : it->second)
                        {
                            if (c <= a)
                            {
                                continue; // emit a < b only; skip self + lower
                            }
                            // dedup this (a,c) candidate across shared cells via
                            // a per-a generation-stamped seen on the partner id.
                            auto& stamp = m_seen[c];
                            if (stamp == m_gen)
                            {
                                continue;
                            }
                            stamp = m_gen;
                            const Entry& ec = m_entries.at(c);
                            if (AabbOverlap(ea.box, ec.box)) // true overlap
                            {
                                out.push_back(BroadphasePair{ a, c });
                            }
                        }
                    }
                }
                // bump m_gen after each 'a' so the next 'a' gets a fresh dedup namespace
                ++m_gen;
            }

            std::sort(out.begin(), out.end());
            return static_cast<int>(out.size());
        }

    } // namespace Physics
} // namespace Arcane
