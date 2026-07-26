#pragma once

// Outliner core (pure, headless-tested; the ImGui shell lives in
// EditorPanels.cpp) -- flat depth-annotated rows over the relationship
// graph.
//
// Row semantics:
// - Roots = entities without a parent, in EntityManager order; children in
//   GetChildren order; depth-first emission.
// - `sort` reorders SIBLING groups (and roots) case-insensitively by label;
//   the tree structure is never broken.
// - `filter`: case-insensitive substring over labels. A match keeps itself
//   AND all its ancestors; kept non-matching ancestors get dimmed = true.
//   A non-empty filter ignores `collapsed` (search auto-expands).
// - `collapsed` (entity GetValue()s): the row is emitted, descendants not.

#include <Astra/Entity/Entity.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    struct OutlinerRow
    {
        Astra::Entity entity;
        int           depth = 0;
        std::string   label;
        bool          hidden = false;
        bool          dimmed = false;
        bool          hasChildren = false;
        std::size_t   childCount = 0;
    };

    struct OutlinerSort
    {
        enum class Column { None, Label };
        Column column = Column::None;
        bool   ascending = true;
    };

    std::vector<OutlinerRow> BuildOutlinerRows(Astra::Registry& reg,
                                               std::string_view filter,
                                               const OutlinerSort& sort,
                                               const std::unordered_set<std::uint64_t>& collapsed);

    // Inclusive visible-row span between a and b (either order); empty when
    // either has no row. Backs shift-range selection.
    std::vector<Astra::Entity> RowRange(std::span<const OutlinerRow> rows,
                                        Astra::Entity a, Astra::Entity b);
}
