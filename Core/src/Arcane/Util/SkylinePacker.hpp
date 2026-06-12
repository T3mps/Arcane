#pragma once

// Skyline rectangle packing -- C++ port of the client's tested Lua packer
// (Client/src/services/assets/skyline.lua, the porting oracle). Each insert
// picks the skyline position with the lowest fitting height, raises the
// skyline, collapses overlapped spans, and merges equal-height neighbors.
// Not optimal but well-bounded; sort inputs by descending height for best
// packing. Header-only, presentation-free (Core).
//
// DIVERGENCE FROM PLAN SNIPPET: the collapse loop in AddSkyline moves the
// `break` inside the `else` branch, matching the Lua oracle (skyline.lua
// lines 40-50). The plan snippet placed `break` after the entire if-block,
// which would stop after the first partial shrink rather than continuing to
// shrink subsequent overlapping nodes. The Lua only breaks when no overlap
// is detected; it continues the loop after a partial shrink.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    class SkylinePacker
    {
    public:
        struct Pos
        {
            uint32_t x = 0;
            uint32_t y = 0;
        };

        SkylinePacker(uint32_t width, uint32_t height)
            : m_width(width), m_height(height)
        {
            m_nodes.push_back({ 0, 0, width });
        }

        // Places a w x h rectangle; returns its top-left or nullopt.
        std::optional<Pos> Insert(uint32_t w, uint32_t h)
        {
            if (w == 0 || h == 0 || w > m_width || h > m_height)
                return std::nullopt;

            int bestIndex = -1;
            uint32_t bestY = 0;
            for (size_t i = 0; i < m_nodes.size(); ++i)
            {
                const auto y = FindBestY(i, w);
                if (!y.has_value())
                    continue;
                if (*y + h > m_height)
                    continue;
                // Lua oracle (line 68): strict y < bestY -- ties are broken
                // by iteration order (leftmost wins) since nodes are sorted
                // left-to-right. The x tie-break here is equivalent and
                // defensive.
                if (bestIndex < 0 || *y < bestY ||
                    (*y == bestY && m_nodes[i].x < m_nodes[(size_t)bestIndex].x))
                {
                    bestIndex = (int)i;
                    bestY = *y;
                }
            }
            if (bestIndex < 0)
                return std::nullopt;

            const uint32_t x = m_nodes[(size_t)bestIndex].x;
            AddSkyline((size_t)bestIndex, x, bestY + h, w);
            return Pos{ x, bestY };
        }

    private:
        struct Node
        {
            uint32_t x = 0;
            uint32_t y = 0;   // skyline height at this span
            uint32_t w = 0;
        };

        // Best (max) y across the spans a w-wide rect would cover starting
        // at node i; nullopt when it overruns the bin (oracle: findBestY).
        std::optional<uint32_t> FindBestY(size_t i, uint32_t w) const
        {
            if (m_nodes[i].x + w > m_width)
                return std::nullopt;
            uint32_t y = m_nodes[i].y;
            int64_t widthLeft = (int64_t)w;
            size_t j = i;
            while (widthLeft > 0)
            {
                if (j >= m_nodes.size())
                    return std::nullopt;
                y = std::max(y, m_nodes[j].y);
                widthLeft -= (int64_t)m_nodes[j].w;
                ++j;
            }
            return y;
        }

        // Insert the raised span, shrink/remove overlapped successors,
        // merge equal-height neighbors (oracle: addSkyline, lines 36-62).
        //
        // Collapse loop: iterate forward from index+1; when a successor
        // is fully covered (cur.w <= 0 after shrink), erase and continue
        // from the same position (equivalent to the Lua's recursive restart
        // since erase slides the next node into place). When partially
        // covered, shrink cur and continue to the next node (Lua falls
        // through without breaking). Only break when there is no overlap
        // (cur.x >= prev.x + prev.w) -- the `else break` in the Lua.
        void AddSkyline(size_t index, uint32_t x, uint32_t newY, uint32_t w)
        {
            m_nodes.insert(m_nodes.begin() + (ptrdiff_t)index,
                           Node{ x, newY, w });
            for (size_t i = index + 1; i < m_nodes.size();)
            {
                Node& prev = m_nodes[i - 1];
                Node& cur  = m_nodes[i];
                if (cur.x < prev.x + prev.w)
                {
                    const uint32_t shrink = prev.x + prev.w - cur.x;
                    if (cur.w <= shrink)
                    {
                        // Node fully consumed -- erase and re-check same
                        // position (equivalent to Lua's recursive restart).
                        m_nodes.erase(m_nodes.begin() + (ptrdiff_t)i);
                        continue;
                    }
                    cur.x += shrink;
                    cur.w -= shrink;
                    // Partially shrunk: do NOT break, continue to next node
                    // to handle subsequent overlaps (matches Lua fall-through
                    // before the else-break).
                    ++i;
                }
                else
                {
                    // No overlap -- stop (oracle: else break, line 48).
                    break;
                }
            }
            // Merge equal-height adjacent nodes (oracle: lines 53-61).
            for (size_t i = 0; i + 1 < m_nodes.size();)
            {
                if (m_nodes[i].y == m_nodes[i + 1].y)
                {
                    m_nodes[i].w += m_nodes[i + 1].w;
                    m_nodes.erase(m_nodes.begin() + (ptrdiff_t)(i + 1));
                }
                else
                {
                    ++i;
                }
            }
        }

        uint32_t           m_width;
        uint32_t           m_height;
        std::vector<Node>  m_nodes;
    };

} // namespace Arcane
