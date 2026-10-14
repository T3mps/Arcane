#pragma once

// InputActionsRows (input-editor redesign spec s2.3): the actions column as
// PURE DATA -- the EntityList/BuildOutlinerRows pattern. The widgets draw
// what this returns; the tests pin what it returns. Readable control names
// come from the evaluator (InputActions::DisplayForPath), never from a
// second table here.

#include "Documents/InputActionsEditorModel.hpp"   // BindingConflict
#include <Arcane/Guid.hpp>
#include <Json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    enum class InputRowKind : std::uint8_t { Action, Binding, CompositeHeader, Part, AddBinding };

    struct InputRow
    {
        InputRowKind kind = InputRowKind::Action;
        Guid id;          // the row's own id (AddBinding: the action's)
        Guid actionId;    // owning action
        Guid bindingId;   // Part: the composite it belongs to; Binding/CompositeHeader: == id
        int  depth = 0;   // 0 action, 1 binding / composite header / add-binding ghost, 2 part
        std::string name;    // action name | readable control | composite type
        std::string badge;   // action: type; binding/part/composite: scheme groups joined ", " ("" = ungrouped)
        std::vector<std::string> groups;   // one entry per scheme group (badge is the joined form, for search/tests); the widgets draw one tinted pill PER entry
        std::string detail;  // action: interaction text; part: "<composite type> · <role>" ("1D Axis · negative")
        std::string device;  // "Keyboard" | "Mouse" | "Gamepad" | ""
        std::string path;    // raw control path (binding/part), for the tooltip and the picker
        bool conflict = false;
    };

    struct InputRowFilter
    {
        std::string search;        // case-insensitive; actions by name, bindings by readable name or path
        std::string schemeGroup;   // "" = all schemes; otherwise hides bindings not in this group (ungrouped always show)
    };

    [[nodiscard]] std::vector<InputRow> BuildInputRows(
        const nlohmann::json& draft, const Guid& map, const InputRowFilter& filter,
        const std::vector<InputActionsEditorModel::BindingConflict>& conflicts,
        const std::unordered_set<std::string>& collapsedActions);   // action ids as strings

    // "Also bound by Crouch (KeyboardMouse), Fire (every scheme)": each OTHER
    // action once with its scheme text; empty when `row` has no conflict.
    [[nodiscard]] std::string ConflictTooltip(const std::vector<InputActionsEditorModel::BindingConflict>& conflicts, const Guid& row);

    // Up/Down over the selectable rows (every kind but AddBinding). Unknown
    // `current` lands on the first row; the ends clamp; empty rows -> nullopt.
    [[nodiscard]] std::optional<Guid> StepSelection(const std::vector<InputRow>& rows,
                                                    const Guid& current, int direction);

    // "hold(duration=0.3)" -> "Hold 0.30 s"; "press" -> "Press"; several
    // joined with " · ". Anything but an array of strings -> "".
    [[nodiscard]] std::string InteractionText(const nlohmann::json& interactions);

    struct SiblingPosition
    {
        Guid parent;        // the id of the object whose array holds the row (map for actions, action for bindings, composite for parts)
        std::size_t index;
        std::size_t count;
    };
    [[nodiscard]] std::optional<SiblingPosition> SiblingIndex(const nlohmann::json& draft, const Guid& id);
}
