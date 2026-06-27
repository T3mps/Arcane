#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <algorithm>
#include <cmath>

namespace Arcane { namespace Physics {

SpatialGrid::SpatialGrid(Real tileSize, Vec2 origin)
    : m_tileSize(tileSize > Real(0) ? tileSize : Real(1)), m_origin(origin) {}

void SpatialGrid::CellCoord(Vec2 p, int& cx, int& cy) const
{
    cx = static_cast<int>(std::floor((p.x - m_origin.x) / m_tileSize));
    cy = static_cast<int>(std::floor((p.y - m_origin.y) / m_tileSize));
}

void SpatialGrid::CellRange(const Aabb2& box, int& x0, int& y0, int& x1, int& y1) const
{
    CellCoord(box.min, x0, y0);
    CellCoord(box.max, x1, y1);
}

void SpatialGrid::Insert(std::uint32_t id, const Aabb2& box)
{
    Remove(id); // self-heal: drop any prior registration so re-Insert is safe
                // (idempotent; Remove is a no-op for a fresh id).
    int x0, y0, x1, y1; CellRange(box, x0, y0, x1, y1);
    std::vector<std::uint64_t>& owned = m_idCells[id];
    owned.clear();
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            const std::uint64_t k = Key(cx, cy);
            m_cells[k].push_back(id);
            owned.push_back(k);
        }
}

void SpatialGrid::Remove(std::uint32_t id)
{
    auto it = m_idCells.find(id);
    if (it == m_idCells.end()) return;
    for (std::uint64_t k : it->second)
    {
        auto cit = m_cells.find(k);
        if (cit == m_cells.end()) continue;
        std::vector<std::uint32_t>& ids = cit->second;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) m_cells.erase(cit);
    }
    m_idCells.erase(it);
}

void SpatialGrid::Move(std::uint32_t id, const Aabb2& box)
{
    // Insert is self-healing (re-registers), so Move == Insert: drop the prior
    // cells + register the new ones in one pass.
    Insert(id, box);
}

void SpatialGrid::ForEachCell(
    FunctionRef<void(int, int, const std::vector<std::uint32_t>&)> fn) const
{
    // Unpack each occupied cell key with the EXACT inverse of Key():
    //   Key(cx,cy) = (uint32_t(cx) << 32) | uint32_t(cy)
    // so cx = int(uint32_t(k >> 32)), cy = int(uint32_t(k & 0xFFFFFFFF)).
    // Casting the uint32_t bit pattern back to int recovers the original signed
    // coord (two's-complement round-trip). Iteration order is unordered_map
    // order -- display only; no sim path depends on it.
    for (const auto& [k, ids] : m_cells)
    {
        const int cx = static_cast<int>(static_cast<std::uint32_t>(k >> 32));
        const int cy = static_cast<int>(static_cast<std::uint32_t>(k & 0xFFFFFFFFu));
        fn(cx, cy, ids);
    }
}

int SpatialGrid::QueryAABB(const Aabb2& box, std::vector<std::uint32_t>& out) const
{
    out.clear();
    int x0, y0, x1, y1; CellRange(box, x0, y0, x1, y1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            auto cit = m_cells.find(Key(cx, cy));
            if (cit == m_cells.end()) continue;
            out.insert(out.end(), cit->second.begin(), cit->second.end());
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return static_cast<int>(out.size());
}

}} // namespace Arcane::Physics
