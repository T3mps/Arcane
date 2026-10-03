#pragma once

// Add-and-listen (spec 2026-09-30 s8.3, decision 9.24): an add is a PENDING
// transaction. Nothing enters the draft until capture ends; CommitPending makes
// exactly ONE model call (one undo step); Esc on the first part adds nothing;
// a later Esc commits the parts heard so far. Pure: no ImGui.

#include <Arcane/Guid.hpp>
#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    class InputActionsEditorModel;

    struct PendingAdd
    {
        enum class Kind : std::uint8_t { Binding, Composite, Part, RebindComposite };
        Kind kind = Kind::Binding;
        Guid map, action, binding;              // binding: Part / RebindComposite target
        std::string composite;                  // "1DAxis" | "2DVector"
        std::vector<std::string> roles;         // capture order
        std::vector<Guid> parts;                // RebindComposite: existing part ids, in order
        std::vector<std::string> captured;      // paths so far, parallel to roles
        std::vector<std::string> groups;        // [] = ungrouped
        [[nodiscard]] bool Done() const noexcept { return captured.size() == roles.size(); }
    };

    [[nodiscard]] std::vector<std::string> CompositeRoles(std::string_view composite);   // {} for an unknown composite
    // The scheme filter as the add's groups when some scheme's bindingGroup
    // equals it; otherwise (and under "All schemes") ungrouped.
    [[nodiscard]] std::vector<std::string> PrefillGroups(const nlohmann::json& draft, std::string_view schemeFilter);
    [[nodiscard]] PendingAdd MakeAddBinding(Guid map, Guid action, std::vector<std::string> groups);
    [[nodiscard]] PendingAdd MakeAddComposite(Guid map, Guid action, std::string composite, std::vector<std::string> groups);
    [[nodiscard]] PendingAdd MakeAddPart(Guid binding, std::string role);
    // nullopt unless `binding` is a composite with at least one valid part id.
    // map/action are filled from the draft, so the ghost rows find their action.
    [[nodiscard]] std::optional<PendingAdd> MakeRebindComposite(const nlohmann::json& draft, Guid binding);
    bool CommitPending(InputActionsEditorModel& model, const PendingAdd& add);   // captured.empty() -> false, no edit
}
