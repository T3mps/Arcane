#pragma once

// AssetFileOpDialogs (T5 s7.5-s7.9): the host-owned modals of the asset file
// operations. Each one REPORTS a committed AssetOpRequest; EditorApp runs it
// through RunAssetOp, which re-plans from fresh facts (the dry-run shown here
// is advisory). Editor-only (the ArcaneEditor glob); the tests drive the
// Browser's inline paths instead.
//
// T5-B8: the Rename modal -- the asset page's pencil has no row to put an
// inline box on, and the asset may be filtered out of the Browser.

#include "Panels/AssetPanelCommon.hpp"   // AssetOpRequest, AssetPanelServices::fileOpRefusal

#include <Arcane/Guid.hpp>

#include <optional>

namespace Arcane::Editor
{
    // The Rename modal's session state, owned by EditorApp. `buf` holds the
    // STEM; `justOpened` opens the popup and focuses the field once.
    struct RenameModalState { bool open = false; Arcane::Guid guid; char buf[128] = {}; bool justOpened = false; };

    // Draws the modal while `st.open`; returns the Rename request on a
    // committed, unrefused Rename (button or Enter). The live refusal reason
    // shows under the field and disables the button.
    std::optional<AssetOpRequest> DrawRenameAssetModal(RenameModalState& st, const AssetPanelServices& sv);
}
