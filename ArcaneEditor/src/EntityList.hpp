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
        // Per-entity unsaved-changes marker (the Outliner's asterisk column).
        // A SEAM today: nothing populates it -- per-entity dirty tracking
        // does not exist yet, so this stays false, the asterisk column stays
        // quiet, and Column::Modified sorts are a stable no-op. The wiring
        // pass fills it (and the matching probe in BuildOutlinerRows's
        // sibling sort) from whatever dirty source lands.
        bool          modified = false;
    };

    struct OutlinerSort
    {
        // Visibility sorts on the hidden flag (ascending = visible first);
        // Modified sorts on OutlinerRow::modified (ascending = clean first --
        // a stable no-op until that seam is wired); Label is case-insensitive
        // on the display name. Ties always keep hierarchy order (stable sort).
        enum class Column { None, Visibility, Modified, Label };
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
