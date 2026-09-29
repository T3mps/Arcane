#pragma once

#include "Documents/EditorDocument.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsDocumentWidgets.hpp"
#include <Arcane/Input/InputSnapshot.hpp>

#include <array>
#include <filesystem>
#include <memory>

namespace Arcane { class CommandStack; }

namespace Arcane::Editor
{
    class InputActionsDocument final : public EditorDocument
    {
    public:
        [[nodiscard]] static std::unique_ptr<InputActionsDocument> Open(
            const std::filesystem::path& path, Arcane::CommandStack* commands = nullptr);
        [[nodiscard]] static Guid PeekGuid(const std::filesystem::path& path);

        const std::string& Title() const override { return title_; }
        Guid AssetGuid() const override { return guid_; }
        bool Dirty() const override { return model_.Dirty(); }
        bool Save() override { return model_.Save(path_); }
        bool WindowFocused() const override { return focused_; }
        void Draw(bool& requestClose) override;
        InputActionsEditorModel& Model() noexcept { return model_; }
        const InputActionsEditorModel& Model() const noexcept { return model_; }
        void SetPreviewSnapshot(const InputSnapshot& snapshot) { previewSnapshot_ = snapshot; }

    private:
        InputActionsDocument(std::filesystem::path path, nlohmann::json draft,
                             Arcane::CommandStack* commands);
        void RefreshText();
        void SelectFirstMapAndAction();

        std::filesystem::path path_;
        std::string title_;
        std::string windowLabel_;
        Guid guid_;
        InputActionsEditorModel model_;
        InputActionsDocumentWidgets widgets_;
        InputSnapshot previewSnapshot_{};
        std::array<char, 131072> text_{};
        bool focused_ = false;
    };
}
