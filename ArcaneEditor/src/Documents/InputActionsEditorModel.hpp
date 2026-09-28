#pragma once

#include <Arcane/Input/InputActionAsset.hpp>

#include <Json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
        void SelectAction(const Guid& action) noexcept { selectedAction_ = action; }
        [[nodiscard]] Guid SelectedAction() const noexcept { return selectedAction_; }
        void SelectMap(const Guid& map) noexcept { selectedMap_ = map; selectedAction_ = {}; selectedBinding_ = {}; selectedPart_ = {}; }
        void SelectBinding(const Guid& binding) noexcept { selectedBinding_ = binding; selectedPart_ = {}; }
        void SelectPart(const Guid& part) noexcept { selectedPart_ = part; }
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
        [[nodiscard]] bool SetField(const Guid& id, std::string key, nlohmann::json value);
        [[nodiscard]] bool SetDefaultMap(const Guid& map);
        [[nodiscard]] bool AddScheme(std::string name, std::string group);
        [[nodiscard]] bool EditScheme(const Guid& scheme, std::string name, std::string group);
        [[nodiscard]] bool RemoveScheme(const Guid& scheme);
        [[nodiscard]] std::vector<std::string> Warnings() const;

        // Called by an undo command after its weak document anchor is checked.
        void RestoreDraft(const nlohmann::json& draft);

    private:
        void Validate();
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
    };
}
