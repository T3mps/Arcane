#pragma once

// AssetFileOpDialogs (T5 s7.5-s7.9): the host-owned modals of the asset file
// operations. Each one REPORTS a committed AssetOpRequest; EditorApp runs it
// through RunAssetOp, which re-plans from fresh facts (the dry-run shown here
// is advisory). Editor-only (the ArcaneEditor glob); the tests drive the
// Browser's inline paths instead.
//
// T5-B8: the Rename modal -- the asset page's pencil has no row to put an
// inline box on, and the asset may be filtered out of the Browser.
// T5-B14: the ONE Delete confirm modal (s7.5). Unlike Rename it OWNS its plan:
// EditorApp plans once on open (facts gathered once, live scene included) and
// re-plans against those facts when the cascade box flips; Confirm executes
// that plan after the gate re-check.

#include "Panels/AssetPanelCommon.hpp"   // AssetOpRequest, AssetPanelServices::fileOpRefusal

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    // The Rename modal's session state, owned by EditorApp. `buf` holds the
    // STEM; `justOpened` opens the popup and focuses the field once.
    struct RenameModalState { bool open = false; Arcane::Guid guid; char buf[128] = {}; bool justOpened = false; };

    // Draws the modal while `st.open`; returns the Rename request on a
    // committed, unrefused Rename (button or Enter). The live refusal reason
    // shows under the field and disables the button.
    std::optional<AssetOpRequest> DrawRenameAssetModal(RenameModalState& st, const AssetPanelServices& sv);

    // The Delete modal's session state, owned by EditorApp: the request (its
    // cascadeDerived is the checkbox), the plan the modal shows and Confirm
    // runs, and the dirty documents' titles that plan would discard.
    struct DeleteConfirmState
    {
        bool open = false, justOpened = false;
        AssetOpRequest request;
        AssetOpPlan plan;
        std::vector<std::string> dirtyTitles;
    };
    // Replan = the cascade box flipped (re-plan, refill dirtyTitles); Confirm
    // and Cancel close the modal. Refusals replace the confirm (disabled).
    enum class DeleteModalResult : std::uint8_t { None, Cancel, Confirm, Replan };
    DeleteModalResult DrawDeleteConfirmModal(DeleteConfirmState& st, const AssetPanelServices& sv);
}
