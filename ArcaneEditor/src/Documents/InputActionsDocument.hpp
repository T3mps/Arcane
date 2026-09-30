#pragma once

#include "Documents/EditorDocument.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsDocumentWidgets.hpp"
#include "Documents/InputActionsInspectorPage.hpp"
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Input/InputRebindOperation.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
        // Pushes the republish request from INSIDE Save(): DocumentHost::
        // ConfirmSaveAndClose runs Save() then Close() within DrawAll
        // (DocumentHost.cpp:208 -> :117-121), so a poll after DrawAll would
        // never see a save-and-close. After a successful model Save,
        // LastValidPreview() IS the asset just written -- no disk re-read.
        bool Save() override
        {
            if (!model_.Save(path_)) return false;
            if (onSaved_ && model_.LastValidPreview()) onSaved_(guid_, *model_.LastValidPreview());
            return true;
        }
        ~InputActionsDocument() override { Arcane::Diagnostics::Clear(diagKey_); }
        bool WindowFocused() const override { return focused_; }
        void Draw(bool& requestClose) override;
        void Tick(double) override { PublishWarnings(); }   // every frame, visible or not: a hidden tab's page edit or undo still reaches Problems
        [[nodiscard]] const std::string& DiagnosticKey() const noexcept { return diagKey_; }
        InputActionsEditorModel& Model() noexcept { return model_; }
        const InputActionsEditorModel& Model() const noexcept { return model_; }
        void SetPreviewSnapshot(const InputSnapshot& snapshot) { previewSnapshot_ = snapshot; }

        // ---- Inspector source (input-editor spec s2.4) ------------------
        // The document is the first NON-SCENE source: its pages are keyed in
        // the model's 4-segment selection key. Page() = PageFor(the current
        // key); PageFor validates every named id (nullptr when one is gone)
        // and "" IS the asset page, never null.
        std::string SourceName() const override { return path_.filename().string(); }
        std::string_view Kind() const override { return "input-actions"; }
        InspectorPage* Page() override { return PageFor(model_.SelectionKey()); }
        InspectorPage* PageFor(std::string_view key) override;
        std::string SelectionKey() const override { return model_.SelectionKey(); }
        bool RestoreSelection(std::string_view key) override { return model_.RestoreSelection(key); }
        bool Resolves(std::string_view key) const override { return model_.Resolves(key); }   // PURE: the host's PruneStale runs it once per frame per history entry
        std::uint64_t SelectionEpoch() const override { return model_.SelectionEpoch(); }
        bool SelectByPath(std::string_view path) override { return model_.SelectByPath(path); }

        // The snapshot a rebind capture observes: the document is the sole
        // claimant of the pointer while a capture is live (ImGui's
        // WantCaptureMouse is true over EVERY editor window, so it must not
        // gate the capture); the keyboard keeps ActiveId semantics (a text
        // field being typed into still claims keys). Pure; tested.
        // A press on a FLOATING window's own chrome (title bar, close/collapse,
        // resize border or grip) belongs to the window: pointerOnChrome sets
        // wantCaptureMouse so the press neither binds nor is heard later.
        [[nodiscard]] static InputSnapshot SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive, bool pointerOnChrome);
        // True when pressPos lies outside the content rect shrunk by borderPad.
        [[nodiscard]] static bool PressOnChrome(ImVec2 pressPos, ImVec2 contentMin, ImVec2 contentMax, float borderPad) noexcept;
        [[nodiscard]] const InputActionsDocumentState& State() const noexcept { return state_; }
        [[nodiscard]] const InputActionsPreview& Preview() const noexcept { return preview_; }
        void SetOnSaved(std::function<void(const Guid&, const InputActionAsset&)> fn) { onSaved_ = std::move(fn); }   // Task 11
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
        // The Inspector page's Rebind...: the page draws AFTER the document, so
        // the Inspector holds focus next frame and TickCapture would cancel at
        // once. The document takes focus for its next Begin (one-shot) and
        // scrolls the CAPTURE row into view (expanding its collapsed action;
        // a pinned page's binding need not be the selection), then arms the capture.
        void BeginRebindFromPage(const Guid& target);

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
        bool focusRequest_ = false;   // one-shot SetNextWindowFocus before the next Begin (DocumentHost's m_focusRequest pattern)
        // Declared (and initialised) AFTER model_, state_ and preview_: it holds
        // their addresses.
        InputActionsInspectorPage page_;
        void PublishWarnings();   // Task 11
        std::function<void(const Guid&, const InputActionAsset&)> onSaved_;         // Task 11
        std::string diagKey_;                                                     // Task 11: "input:" + guid
        std::uint64_t publishedRevision_ = 0;
        std::vector<std::string> publishedWarnings_;                              // Task 11
    };
}
