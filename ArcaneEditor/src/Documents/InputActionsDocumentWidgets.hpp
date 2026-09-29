#pragma once

// The Input Actions document's presentation (input-editor redesign spec s2):
// toolbar (+ Add, search, scheme filter, Preview), maps column, actions
// column drawn from InputActionsRows. View/tool state lives in
// InputActionsDocumentState (spec A s3.4 -- the document's own toolbar,
// never the Inspector); every mutation goes through the model (undoable).
// The asset model stays independent of ImGui.

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsRows.hpp"
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    struct InputActionsDocumentState
    {
        char search[128] = {};
        std::string schemeFilter;                     // bindingGroup; "" = All schemes
        bool previewArmed = false;
        std::unordered_set<std::string> collapsedActions;   // action ids; a non-empty search overrides it without editing it
        Guid renameTarget;                            // inline rename (F2 / context menu); swept every frame when its row is not drawable
        std::string renameBuf;
        bool renameFocusPending = false;
        bool scrollToSelection = false;               // consumed by the row that draws as selected (keyboard step, F2, a new row)
        enum class DragVerdict : std::uint8_t { None, Legal, Illegal };
        DragVerdict dragVerdict = DragVerdict::None;      // written by the hovered drop target this frame
        DragVerdict dragVerdictPrev = DragVerdict::None;  // read by the drag source's preview (one frame behind)
        bool schemePopupPending = false;              // opened at window scope (a popup cannot open from inside another)
        char newSchemeName[64] = "Gamepad";
        char newSchemeGroup[64] = "Gamepad";
    };

    // The document-level preview evaluator (owned by InputActionsDocument;
    // the widgets and the Inspector page borrow it): rebuilt whenever the
    // model's LastValidPreview changes, fed the raw snapshot while the
    // toolbar's Preview is armed.
    struct InputActionsPreview
    {
        std::unique_ptr<InputActions> evaluator;
        nlohmann::json source;
        void Sync(const InputActionsEditorModel& model)
        {
            if (!model.LastValidPreview()) return;
            const nlohmann::json next = model.LastValidPreview()->ToJson();
            // Keyed on the source alone: a draft whose LoadAsset fails is
            // retried only when LastValidPreview actually changes, never every
            // frame (the evaluator stays null meanwhile; the readers null-guard).
            if (source == next) return;
            evaluator = InputActions::Create();
            if (!evaluator->LoadAsset(*model.LastValidPreview())) evaluator.reset();
            source = next;
        }
        void Update(const InputSnapshot& raw) { if (evaluator) evaluator->Update(1.0 / 60.0, raw); }
        [[nodiscard]] InputActionValue Value(const Guid& action) const { return evaluator ? evaluator->Value(action) : InputActionValue{}; }
        [[nodiscard]] float BindingValue(const Guid& binding) const { return evaluator ? evaluator->BindingValue(binding) : 0.0f; }
        [[nodiscard]] InputDevice ActiveDevice() const { return evaluator ? evaluator->ActiveDevice() : InputDevice::Kbm; }
    };

    class InputActionsDocumentWidgets
    {
    public:
        struct Services
        {
            std::function<void(const Guid&)> beginRebind;
            std::function<bool(const Guid&)> isRebinding;
            std::function<float()> rebindRemaining;
            std::function<float(const Guid&)> glow;   // 0 = off
            std::function<bool()> inputSwallowed;     // true while a capture is live and on its completing frame: keys and clicks belong to the capture
        };
        void Draw(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services);

    private:
        using Edit = std::function<void()>;
        void DrawToolbar(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit);
        void DrawMaps(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
        void DrawActions(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
        void DrawRow(const InputRow& row, InputActionsEditorModel& model, InputActionsDocumentState& state,
                     const Services& services, Edit& edit, const std::vector<InputRow>& rows);
        void DrawSchemePopup(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit);
        // Keyboard navigation (spec B s2.3). Each runs inside its own column
        // child, so IsWindowFocused(ChildWindows) routes the keys to whichever
        // column has focus. Actions: Up/Down step (auto-repeat), Left/Right tree
        // convention, Enter rebinds, F2 renames an action, Delete removes the
        // row. Maps: Up/Down, F2, Delete. Commands never auto-repeat; every key
        // is inert while a capture/drag/text box/inline rename owns input.
        void HandleKeys(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services,
                        Edit& edit, const std::vector<InputRow>& rows);
        void HandleMapKeys(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
        static void SelectRow(InputActionsEditorModel& model, const InputRow& row);
        [[nodiscard]] static bool RowSelected(const InputActionsEditorModel& model, const InputRow& row);
    };
}
