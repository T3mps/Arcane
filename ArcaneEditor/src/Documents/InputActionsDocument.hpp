#pragma once

#include "Documents/EditorDocument.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsDocumentWidgets.hpp"
#include <Arcane/Input/InputRebindOperation.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

        // The snapshot a rebind capture observes: the document is the sole
        // claimant of the pointer while a capture is live (ImGui's
        // WantCaptureMouse is true over EVERY editor window, so it must not
        // gate the capture); the keyboard keeps ActiveId semantics (a text
        // field being typed into still claims keys). Pure; tested.
        [[nodiscard]] static InputSnapshot SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive);
        [[nodiscard]] const InputActionsDocumentState& State() const noexcept { return state_; }
        [[nodiscard]] const InputActionsPreview& Preview() const noexcept { return preview_; }
        // True while a capture is live and on the frame it completed/cancelled:
        // keys and clicks belong to the capture (UE consumes the heard key at
        // the selector; ImGui has no event consumption, so the frame stamp does).
        // Public for the app's raw-scancode shortcuts (Ctrl+Z/Y/N/O/S/X/C/V/D,
        // EditorApp::HandleUndoRedoAndSceneShortcuts), which run before the
        // document draws and must stand down while a capture is armed.
        [[nodiscard]] bool InputSwallowed() const noexcept { return captureTarget_.IsValid() || captureSwallowFrame_ == ImGui::GetFrameCount(); }

    private:
        InputActionsDocument(std::filesystem::path path, nlohmann::json draft,
                             Arcane::CommandStack* commands);
        void SelectFirstMapAndAction();
        void TickCapture(bool bodyDrawn);
        void BeginRebind(const Guid& target);

        std::filesystem::path path_;
        std::string title_;
        std::string windowLabel_;
        Guid guid_;
        InputActionsEditorModel model_;
        InputActionsDocumentState state_;
        InputActionsDocumentWidgets widgets_;
        InputActionsPreview preview_;
        InputSnapshot previewSnapshot_{};
        InputRebindOperation capture_;
        Guid captureTarget_;
        int captureSwallowFrame_ = -1;
        bool focused_ = false;
    };
}
