#pragma once

// The Input Actions document's Inspector pages (input-editor redesign spec
// s2.4): asset / map / action / binding(part), every one drawn with
// PropertyGrid, edits routed through the model (undoable). The Live preview
// block reads the document's preview evaluator while its toolbar's Preview is
// armed. The page is KEYED (SetSelection) so a pinned Inspector can hold one
// binding while the document selects another.

#include "Documents/InputActionsDocumentWidgets.hpp"   // InputActionsDocumentState, InputActionsPreview
#include "Documents/InputActionsEditorModel.hpp"
#include "Panels/InspectorSource.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    struct InputSelection
    {
        Guid map, action, binding, part;
    };

    class InputActionsInspectorPage final : public InspectorPage
    {
    public:
        struct Services
        {
            std::function<void(const Guid&)> beginRebind;
            const InputActionsDocumentState* state = nullptr;
            const InputActionsPreview* preview = nullptr;
        };
        InputActionsInspectorPage(InputActionsEditorModel& model, std::string assetName,
                                  std::string assetPath, Services services);
        void SetSelection(const InputSelection& sel) { sel_ = sel; }

        // Every crumb carries a `key` in the model's 4-segment format (three
        // slashes always): asset root "", map "<map>///", action
        // "<map>/<action>//", binding "<map>/<action>/<binding>/", part all
        // four -- a PINNED instance re-targets itself through
        // InspectorHost::RepinKey; `select` re-selects in the model for an
        // unpinned one.
        std::vector<InspectorCrumb> Breadcrumb() const override;
        void Draw(PropertyGrid& grid) override;

    private:
        void DrawAsset(PropertyGrid& grid);
        void DrawMap(PropertyGrid& grid, const nlohmann::json& map);
        void DrawAction(PropertyGrid& grid, const nlohmann::json& action);
        void DrawBinding(PropertyGrid& grid, const nlohmann::json& row, bool isPart);
        void DrawLivePreview(PropertyGrid& grid, const Guid& action);
        void DrawPicker(const Guid& target);
        // Edits never mutate the draft under the row loop: inside Draw they are
        // queued (edit_) and run after the last Draft() reference; OUTSIDE Draw
        // -- a TextRow draft deactivated while this page was not drawn, flushed
        // by PropertyGrid::CommitOrphans before any Inspector window Begins --
        // the edit applies at once, so a following Ctrl+S saves it.
        void Defer(std::function<void()> fn) { if (drawing_) edit_ = std::move(fn); else fn(); }

        InputActionsEditorModel& model_;
        std::string assetName_, assetPath_;
        Services services_;
        InputSelection sel_;
        char pickerSearch_[64] = {};
        std::function<void()> edit_;
        bool drawing_ = false;
        // Liveness token for the commits TextRow stores in its draft: a draft
        // outlives the page (the document closed while a box was active), so a
        // stored commit checks the weak_ptr before touching model_.
        std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    };
}
