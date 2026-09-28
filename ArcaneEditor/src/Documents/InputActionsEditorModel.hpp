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
        [[nodiscard]] bool DuplicateAction(const Guid& map, const Guid& action);

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
    };
}
