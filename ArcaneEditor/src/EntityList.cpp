#include "EntityList.hpp"

#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cctype>

namespace
{
    bool ContainsCI(std::string_view hay, std::string_view needle)
    {
        if (needle.size() > hay.size())
            return false;
        auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            std::size_t j = 0;
            while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
                ++j;
            if (j == needle.size())
                return true;
        }
        return false;
    }

    bool LessCI(std::string_view a, std::string_view b)
    {
        auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            const char ca = lower(a[i]), cb = lower(b[i]);
            if (ca != cb)
                return ca < cb;
        }
        return a.size() < b.size();
    }

    // Priority table, first hit wins. Extend here when a new component type
    // deserves its own type-column string.
    std::string TypeLabel(Astra::Registry& reg, Astra::Entity e)
    {
        if (reg.HasComponent<Arcane::PostProcess>(e))    return "Post Process";
        if (reg.HasComponent<Arcane::RigidBody2D>(e))    return "Rigid Body";
        if (reg.HasComponent<Arcane::SpriteRenderer>(e)) return "Sprite";
        return "Entity";
    }
}

namespace Arcane::Editor
{
    std::vector<OutlinerRow> BuildOutlinerRows(Astra::Registry& reg,
                                               std::string_view filter,
                                               const OutlinerSort& sort,
                                               const std::unordered_set<std::uint64_t>& collapsed)
    {
        std::vector<OutlinerRow> rows;

        auto makeRow = [&](Astra::Entity e, int depth)
        {
            OutlinerRow r;
            r.entity = e;
            r.depth = depth;
            r.label = Edit::DisplayName(reg, e);
            r.type = TypeLabel(reg, e);
            r.hidden = reg.HasComponent<Hidden>(e);
            r.childCount = reg.GetChildCount(e);
            r.hasChildren = r.childCount > 0;
            return r;
        };

        auto sortKey = [&](Astra::Entity e) -> std::string
        {
            return sort.column == OutlinerSort::Column::Type ? TypeLabel(reg, e)
                                                             : Edit::DisplayName(reg, e);
        };
        auto sortSiblings = [&](std::vector<Astra::Entity>& kids)
        {
            if (sort.column == OutlinerSort::Column::None)
                return;
            std::stable_sort(kids.begin(), kids.end(),
                [&](Astra::Entity a, Astra::Entity b)
                {
                    const std::string ka = sortKey(a), kb = sortKey(b);
                    return sort.ascending ? LessCI(ka, kb) : LessCI(kb, ka);
                });
        };

        auto emit = [&](this auto&& self, Astra::Entity e, int depth) -> void
        {
            rows.push_back(makeRow(e, depth));
            // Search auto-expands: a non-empty filter ignores collapse.
            if (filter.empty() && collapsed.contains(static_cast<std::uint64_t>(e.GetValue())))
                return;
            std::vector<Astra::Entity> kids = reg.GetChildren(e);
            sortSiblings(kids);
            for (Astra::Entity c : kids)
                self(c, depth + 1);
        };

        std::vector<Astra::Entity> roots;
        for (Astra::Entity e : reg.GetEntityManager())
            if (!reg.HasParent(e))
                roots.push_back(e);
        sortSiblings(roots);
        for (Astra::Entity r : roots)
            emit(r, 0);

        if (!filter.empty())
        {
            // Keep matches and their ancestor chains; dim kept non-matches.
            std::vector<char> match(rows.size(), 0), keep(rows.size(), 0);
            for (std::size_t i = 0; i < rows.size(); ++i)
                match[i] = ContainsCI(rows[i].label, filter) ? 1 : 0;
            std::vector<std::size_t> chain;   // indices of the current ancestor path
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                while (!chain.empty() && rows[chain.back()].depth >= rows[i].depth)
                    chain.pop_back();
                if (match[i])
                {
                    keep[i] = 1;
                    for (std::size_t a : chain)
                        keep[a] = 1;
                }
                chain.push_back(i);
            }
            std::vector<OutlinerRow> out;
            out.reserve(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                if (!keep[i])
                    continue;
                rows[i].dimmed = !match[i];
                out.push_back(std::move(rows[i]));
            }
            rows = std::move(out);
        }
        return rows;
    }

    std::vector<Astra::Entity> RowRange(std::span<const OutlinerRow> rows,
                                        Astra::Entity a, Astra::Entity b)
    {
        std::size_t ia = rows.size(), ib = rows.size();
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].entity == a) ia = i;
            if (rows[i].entity == b) ib = i;
        }
        if (ia == rows.size() || ib == rows.size())
            return {};
        const auto [lo, hi] = std::minmax(ia, ib);
        std::vector<Astra::Entity> out;
        out.reserve(hi - lo + 1);
        for (std::size_t i = lo; i <= hi; ++i)
            out.push_back(rows[i].entity);
        return out;
    }
}
