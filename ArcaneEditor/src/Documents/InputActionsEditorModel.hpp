#pragma once

#include "Documents/InputSelectionKey.hpp"

#include <Arcane/Input/InputActionAsset.hpp>

#include <Json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { class CommandStack; }

namespace Arcane::Editor
{
    class InputActionsEditorModel
    {
    public:
        explicit InputActionsEditorModel(nlohmann::json draft,
                                          Arcane::CommandStack* commands = nullptr);
        ~InputActionsEditorModel();
        InputActionsEditorModel(const InputActionsEditorModel&) = delete;
        InputActionsEditorModel& operator=(const InputActionsEditorModel&) = delete;

        [[nodiscard]] const nlohmann::json& Draft() const noexcept { return draft_; }
        [[nodiscard]] const std::optional<InputActionAsset>& LastValidPreview() const noexcept
        { return preview_; }
        [[nodiscard]] const std::vector<std::string>& Diagnostics() const noexcept
        { return diagnostics_; }
        [[nodiscard]] bool Dirty() const noexcept { return draft_ != saved_; }
        [[nodiscard]] bool ApplyEdit(std::string label, nlohmann::json before,
                                      nlohmann::json after);
        [[nodiscard]] bool Undo();
        [[nodiscard]] bool Redo();
        [[nodiscard]] bool Save(const std::filesystem::path& path);
        // Selection. Every Select* call with a VALID id bumps SelectionEpoch()
        // (a re-click on the selected row is a gesture: it re-asserts the
        // Inspector on this document); a clear bumps only when it changes
        // something. Undo/redo restore the selection SILENTLY (no bump).
        void SelectAction(const Guid& action) noexcept;
        void SelectMap(const Guid& map) noexcept;
        void SelectBinding(const Guid& binding) noexcept;
        void SelectPart(const Guid& part) noexcept;
        // Container-fallback deselects from INSIDE the document (Ruling P18):
        // empty space in the actions column drops to the map, in the maps
        // column to the asset page. SILENT (no epoch bump): not a selection
        // event, so it never moves the Inspector away from another source --
        // but Page() re-reads the live key, so an Inspector already showing
        // this document drops to the container page.
        void DeselectToMap(const Guid& map) { SetSelectionSilently({ map, Guid{}, Guid{}, Guid{} }); }
        void DeselectToAsset() { SetSelectionSilently({}); }
        [[nodiscard]] std::uint64_t SelectionEpoch() const noexcept { return selectionEpoch_; }
        [[nodiscard]] std::string SelectionKey() const;                    // "<map>/<action>/<binding>/<part>"; "" when no map
        [[nodiscard]] bool RestoreSelection(std::string_view key);         // every non-empty segment must exist
        [[nodiscard]] std::optional<std::array<Guid, 4>> ResolveKey(std::string_view key) const;   // ParseSelectionKey + every named id exists
        [[nodiscard]] bool Resolves(std::string_view key) const;           // PURE: would RestoreSelection succeed? no selection, no bump
        [[nodiscard]] const nlohmann::json* FindNode(const Guid& id) const; // nullptr when no node carries that id
        [[nodiscard]] bool SelectByPath(std::string_view namePath);        // "<map>[/<action>[/<binding index>[/<part index>]]]"
        [[nodiscard]] Guid SelectedAction() const noexcept { return selectedAction_; }
        [[nodiscard]] Guid SelectedMap() const noexcept { return selectedMap_; }
        [[nodiscard]] Guid SelectedBinding() const noexcept { return selectedBinding_; }
        [[nodiscard]] Guid SelectedPart() const noexcept { return selectedPart_; }
        [[nodiscard]] bool DuplicateAction(const Guid& map, const Guid& action);
        [[nodiscard]] bool AddMap(std::string name = "Action Map");
        [[nodiscard]] bool RemoveMap(const Guid& map);
        [[nodiscard]] bool AddAction(const Guid& map, std::string name = "Action");
        [[nodiscard]] bool RemoveAction(const Guid& map, const Guid& action);
        [[nodiscard]] bool AddBinding(const Guid& map, const Guid& action,
                                      std::string path = "<Keyboard>/space");
        [[nodiscard]] bool AddComposite(const Guid& map, const Guid& action,
                                        std::string composite);
        [[nodiscard]] bool RemoveBinding(const Guid& map, const Guid& action,
                                         const Guid& binding);
        [[nodiscard]] bool AddPart(const Guid& binding, std::string role,
                                   std::string path = "<Keyboard>/space");
        [[nodiscard]] bool RemovePart(const Guid& binding, const Guid& part);
        [[nodiscard]] bool DuplicateRow(const Guid& id);
        [[nodiscard]] bool MoveRow(const Guid& id, int direction);
        [[nodiscard]] bool MoveRowTo(const Guid& id, std::size_t index);   // reorder within the row's own parent array (undoable)
        [[nodiscard]] bool SetField(const Guid& id, std::string key, nlohmann::json value);
        [[nodiscard]] bool SetDefaultMap(const Guid& map);
        [[nodiscard]] bool AddScheme(std::string name, std::string group);
        [[nodiscard]] bool EditScheme(const Guid& scheme, std::string name, std::string group);
        [[nodiscard]] bool RemoveScheme(const Guid& scheme);
        struct BindingConflict { Guid binding; Guid otherBinding; Guid otherAction; std::string otherActionName; std::string path; std::string group; };
        // One entry PER DIRECTION (a and b each get one); compares the
        // compiled control (InputActions::CanonicalControlKey), never the spelling.
        [[nodiscard]] std::vector<BindingConflict> Conflicts() const;
        // Invalid names + unknown paths (the evaluator's own check) + one line per conflicting PAIR.
        [[nodiscard]] std::vector<std::string> Warnings() const;
        [[nodiscard]] std::uint64_t DraftRevision() const noexcept { return draftRevision_; }   // bumped by every draft write (Validate is the one funnel)
        // Name rules mirror the runtime's LoadAsset keys (InputActions.cpp:757-775):
        // map names unique across the document, action names unique within
        // their map; trimmed, case-sensitive. nullopt = acceptable (the
        // unchanged name always is).
        [[nodiscard]] static std::optional<std::string> ValidateName(const nlohmann::json& draft, const Guid& id, std::string_view proposed);
        [[nodiscard]] bool SiblingNameTaken(const Guid& id, std::string_view name) const;

        // Called by an undo command after its weak document anchor is checked.
        void RestoreDraft(const nlohmann::json& draft);
        void RestoreSelectionOrAncestor(std::string_view key);   // silent; trimmed to the deepest surviving ancestor

    private:
        void Validate();
        bool OwnerOfBinding(const Guid& binding, Guid& map, Guid& action) const;
        void SetSelectionSilently(const std::array<Guid, 4>& ids);
        std::uint64_t draftRevision_ = 0;
        nlohmann::json draft_;
        nlohmann::json saved_;
        std::optional<InputActionAsset> preview_;
        std::vector<std::string> diagnostics_;
        Arcane::CommandStack* commands_ = nullptr;
        std::shared_ptr<InputActionsEditorModel*> anchor_;
        Guid selectedAction_;
        Guid selectedMap_;
        Guid selectedBinding_;
        Guid selectedPart_;
        std::uint64_t selectionEpoch_ = 0;
    };
}
