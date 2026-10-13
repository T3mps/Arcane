#pragma once

// The Input Actions document's presentation (input-editor redesign spec s2):
// toolbar (+ Add, search, scheme filter, Preview), maps column, actions
// column drawn from InputActionsRows. View/tool state lives in
// InputActionsDocumentState (spec A s3.4 -- the document's own toolbar,
// never the Inspector); every mutation goes through the model (undoable).
// The asset model stays independent of ImGui.

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsRows.hpp"
#include "Documents/InputPendingAdd.hpp"
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Sim/SimSettings.hpp>   // the preview ticks one sim.fixedHz step
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
        // Scroll-into-view requests, ONE PER COLUMN (the maps child draws first,
        // so a shared flag would always be eaten by the selected map row):
        // scrollMapToSelection is set by the maps column's keys, its `+`/
        // Duplicate and a rename opened on a MAP, and consumed only by the
        // selected map row (or its rename box); scrollRowToSelection is set by
        // the actions column's keys, new actions/bindings/composites/parts and a
        // rename opened on an ACTION, and consumed only by the selected actions
        // row (or its rename box).
        bool scrollMapToSelection = false;
        bool scrollRowToSelection = false;
        Guid scrollRowToId;   // one-shot, set by the Inspector page's Rebind...: scroll the actions column to THIS binding/part row (the capture row), winning over scrollRowToSelection; cleared on every DrawActions exit, so an undrawn target never fires later
        enum class DragVerdict : std::uint8_t { None, Legal, Illegal };
        DragVerdict dragVerdict = DragVerdict::None;      // written by the hovered drop target this frame
        DragVerdict dragVerdictPrev = DragVerdict::None;  // read by the drag source's preview (one frame behind)
        bool schemePopupPending = false;              // opened at window scope (a popup cannot open from inside another)
        char newSchemeName[64] = "Gamepad";
        char newSchemeGroup[64] = "Gamepad";
        std::unordered_map<std::string, ImVec2>* probe = nullptr;   // TEST SEAM (InputActionsDocumentUiTest): rows record their centre under their id, Rebind buttons under "rebind:<id>". Production: nullptr.
        // The Rebind column (spec 2026-09-30 s8.3, amending 2026-09-28 s2.3's
        // hover-only rule): x relative to the actions column window's left edge
        // where every Binding/Part row's button sits. Each drawn row folds its
        // own trailing end into rebindColumnXNext; DrawActions publishes it after
        // the row loop, so the column shrinks one frame after the widest row goes.
        float rebindColumnX = 0.0f;
        float rebindColumnXNext = 0.0f;
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
        void Update(const InputSnapshot& raw) { if (evaluator) evaluator->Update(1.0 / Settings<SimSettings>().fixedHz, raw); }   // one sim step
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
            std::function<void(PendingAdd)> beginAdd;               // add-and-listen (spec 2026-09-30 s8.3)
            std::function<const PendingAdd*()> pending;             // the live pending add, or nullptr
        };
        void Draw(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services);
        // True when `row` is the row the actions column should scroll into view this frame: the one-shot scrollRowToId (a Binding/Part row only) wins, else the selected row while scrollRowToSelection is set. Pure; public for the row tests.
        [[nodiscard]] static bool ScrollsIntoView(const InputActionsEditorModel& model, const InputActionsDocumentState& state, const InputRow& row);

    private:
        using Edit = std::function<void()>;
        void DrawToolbar(InputActionsEditorModel& model, InputActionsDocumentState& state, const Services& services, Edit& edit);
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
