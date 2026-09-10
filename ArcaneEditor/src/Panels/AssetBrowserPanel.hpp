#pragma once

// AssetBrowserPanel (panel-split arc): the "Asset Browser" window -- the
// toolbar (`+ Create` + search), the kind rail, the grouped/folded asset
// table (scroll-to-selection + arrow-key nav), the table<->preview drag
// splitter, the resizable preview pane, and the bottom bar (context + health
// digest). Task 6 moved the body here as pure motion out of AssetsPanel.cpp's
// DrawBrowseLens; Task 7 wrapped it in its own panel shell and gave it the
// half of AssetsPanelState it actually reads. See AssetBrowserPanel.cpp's own
// header comment for the full section-by-section accounting.

#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/AssetPanelServices

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;

    // The preview pane's default width (2026-09-07 follow-up). Lives here,
    // not as a second literal duplicated in AssetBrowserPanel.cpp, so
    // AssetBrowserPanelState's own field default below and the splitter's
    // double-click-reset target (AssetBrowserPanel.cpp) can never drift
    // apart -- a review minor on the first cut of this feature, where both
    // spellings independently hardcoded 165.0f. (Panel-split Task 7 moved
    // this constant here from AssetsPanel.hpp with the state field that
    // references it; the preview pane is Browser-only, spec s9.4.)
    inline constexpr float kAssetsPreviewPaneDefaultWidth = 165.0f;

    // The Asset Browser window's session-only UI state (spec s6: panel state
    // is session-only in v1). Panel-split Task 7: one of the two structs
    // AssetsPanelState dissolved into, carrying exactly the fields the
    // Browser body and its toolbar read -- the partition its own comments
    // already drew. `search` feeds AssetPanelModel::SetSearch every frame.
    // `railKind` feeds SetKindFilter. `seenSelectionStamp` lets the table
    // scroll-to-selection exactly once by comparing against
    // model.selectionStamp.
    //
    // Browse's filter never leaks into the other two panels by construction:
    // Rows()/Rail()/ShownAssetCount() are the model's ONLY filtered views and
    // only this panel reads them (spec s6).
    struct AssetBrowserPanelState
    {
        char search[128] = {};
        int  railKind = -1;                 // -1 = All
        std::uint32_t seenSelectionStamp = 0; // scroll-to-selection once

        // 2026-09-07 follow-up (spec s5/s11.2 addendum, post-Task-11): the
        // preview pane's DESIRED width, user-resizable via a drag splitter
        // between the table and the pane. Session-only, same convention as
        // every other field here -- NOT persisted to imgui.ini (contrast the
        // Material panel's ShaderEditorDocument PaneSplitter ratio, which IS
        // persisted; this one deliberately is not).
        //
        // "Desired", precisely: this field is written ONLY by the splitter's
        // drag and its double-click reset (both in AssetBrowserPanel.cpp) --
        // never by the per-frame layout clamp, which computes a separate,
        // purely local DRAWN width instead (ClampPreviewForLayout). A review
        // fix (2026-09-07): the first cut clamped this field itself every
        // frame, which meant a transient panel-narrowing (a window resize,
        // nothing the user asked of the pane) silently and PERMANENTLY
        // reduced whatever the user had actually dragged to, with no way back
        // once the panel widened again. Splitting "what the user wants" from
        // "what fits on screen this frame" is what fixes that: the wide value
        // survives the narrow interval untouched and reasserts itself the
        // moment there is room again.
        float previewPaneWidth = kAssetsPreviewPaneDefaultWidth;

        // Task 10: session-only fold/group open state, MIRRORING
        // AssetPanelModel's own private m_groupOpen/m_childrenOpen (same
        // defaults: a folder absent from `groupOpen` is OPEN, a texture
        // guid absent from `childrenOpen` is COLLAPSED). The model exposes
        // no getter for either -- Rows() already bakes the effective result
        // into which rows exist -- but the panel still needs to know which
        // glyph to draw (chevron open/closed) and what to flip, so it keeps
        // its own copy and pushes every toggle through
        // AssetPanelModel::SetGroupOpen/SetChildrenOpen (the only two
        // writers of the model's maps), which keeps the two in lockstep by
        // construction rather than by convention.
        //
        // Panel-split spec s6: these mirrors tolerate stale keys across a
        // project switch exactly as they always have -- absent == default,
        // and a stale key is never queried -- which is why the project-switch
        // seam still resets only the Graph panel's canvas state.
        std::unordered_map<std::string, bool>  groupOpen;
        std::unordered_map<Arcane::Guid, bool> childrenOpen;
    };

    // Draw the Browse lens body: the rail (all/per-kind counts + hover
    // create affordance, spec s6), the grouped/folded asset table, the
    // table<->preview drag splitter, and the resizable preview pane (thumb,
    // name/pills, path/guid/cook rows, Derived list, action buttons).
    // `model`/`project` are read; every effect travels through `actions`,
    // gated by `services`; `docs` routes a non-scene open the same way the
    // Graph panel's node double-click does (both call the SAME OpenAssetRow,
    // AssetPanelCommon.hpp). Called only by DrawAssetBrowserPanel below,
    // which owns the window and the two chrome bands around it -- kept as a
    // separate function so the body's own extent stays legible.
    void DrawAssetBrowserBody(AssetBrowserPanelState& state, AssetPanelModel& model,
                              const Arcane::Project* project, DocumentHost& docs,
                              const AssetPanelServices& services,
                              AssetPanelActions& actions);

    // Draw the "Asset Browser" window (panel-split spec s5/s9): toolbar
    // (`+ Create` + search, spec s9.1's R2 minimum) · body · bottom bar
    // ("N assets - M selected" / the health digest chip, spec s9.2).
    // `model` is rebuilt by the caller (RebuildIfDirty) BEFORE this runs
    // every frame, whether or not this window is even visible -- this panel
    // only reads it, plus feeds this frame's toolbar edits back in
    // (SetSearch/SetKindFilter). `open` is forwarded to ImGui::Begin (the
    // tab's X button; null = no X).
    AssetPanelActions DrawAssetBrowserPanel(AssetBrowserPanelState& state, AssetPanelModel& model,
                                            const Arcane::Project* project, DocumentHost& docs,
                                            const AssetPanelServices& services,
                                            bool* open = nullptr);
}
