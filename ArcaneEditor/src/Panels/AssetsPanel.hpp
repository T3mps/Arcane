#pragma once

// AssetsPanel (asset-manager redesign, Plan 1 Task 9): the panel that
// REPLACES AssetBrowser.hpp's DrawAssetBrowserPanel as what the "Assets" tab
// draws. This task ships the SHELL ONLY -- the three fixed bands spec S5
// pins (toolbar / body / bottom bar) and the verbatim state/actions/services
// contracts Tasks 10-13 extend IN PLACE. The Browse lens's real body (rail +
// grouped table + preview pane) lands in Task 10; until then the body is a
// placeholder child region.
//
// Panel identity is UNCHANGED from the old panel: same "Assets" ImGui::Begin
// title (PanelRegistry.hpp:37, imgui.ini keys untouched), same PanelId::Assets
// visibility flags -- no dock churn.
//
// AssetBrowser.hpp's AssetKind/AssetEntry/MatchesFilter etc. stay the
// classification vocabulary underneath AssetPanelModel (see that header's own
// comment) -- this file adds no new classification, only the panel shell.

#include "Panels/AssetPanelModel.hpp"   // AssetPanelModel (current before every panel draw)

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class DocumentHost;

    // Which lens the panel shows. Plan 1 ships Browse only -- Graph (Plan 3)
    // and Status (Plan 2) exist in the enum and in the toolbar's lens strip
    // from day one (layout pinned per spec s5: "later plans enable, nothing
    // shifts"), but both stay disabled until their own plan lands.
    enum class AssetLens : std::uint8_t { Browse, Graph, Status };

    // Session-only UI state (spec s5: panel state is session-only in v1).
    // `search` feeds AssetPanelModel::SetSearch every frame. `railKind` is
    // unused until Task 10 wires the rail -- left at -1 (All) so feeding it
    // to SetKindFilter today is a no-op. `seenSelectionStamp` lets a later
    // task scroll-to-selection exactly once by comparing against
    // model.selectionStamp.
    struct AssetsPanelState
    {
        AssetLens lens = AssetLens::Browse;
        char search[128] = {};
        int  railKind = -1;                 // -1 = All
        std::uint32_t seenSelectionStamp = 0; // scroll-to-selection once
    };

    // Row/menu actions the APP resolves after the draw -- same "panel
    // reports, app performs" split as the old AssetBrowserActions (dialogs
    // and file IO never happen inside the panel draw; see AssetBrowser.hpp's
    // own comment on AssetBrowserActions). Superset of today's
    // AssetBrowserActions: adds `copyGuid` (spec s6's new context-menu entry)
    // and the unified-create pair (`requestCreateKind`/`createPrefillParent`,
    // Task 12). Task 9's placeholder body never raises the row-action fields
    // (no rows exist yet -- Task 10 draws them), but the struct carries the
    // full shape now so later tasks extend this exact contract rather than a
    // new one.
    struct AssetsPanelActions   // superset of today's AssetBrowserActions
    {
        Arcane::Guid createInstanceOf, createSpriteFrom, setBootScene,
                     showInExplorer, copyPath, copyGuid;
        std::filesystem::path openScene;
        // Unified create (Task 12): request the create dialog for a kind.
        // -1 = none. Values = CreateAssetKind (Task 12).
        int  requestCreateKind = -1;
        Arcane::Guid createPrefillParent;   // instance parent / sprite texture prefill
    };

    // The Assets panel's thumbnail-resolver seam (Task 7's AssetServices,
    // consumed here per its own header comment: "Task 9's AssetsPanelServices
    // consumes this exact callable"). Guid -> an ImGui texture id via the
    // chrome context's texture cache; 0 = unavailable, caller falls back to
    // the kind icon. Task 9's placeholder body never calls this; Task 10's
    // rows are the first consumer.
    struct AssetsPanelServices
    {
        std::function<std::uint64_t(const Arcane::Guid&)> resolveAssetThumb;
    };

    // Draw the "Assets" panel: toolbar (+ Create / search / lens strip) ·
    // body (the active lens; Browse is a placeholder child until Task 10) ·
    // bottom bar (context + digest, spec s5). `model` is rebuilt by the
    // caller (RebuildIfDirty) BEFORE this runs every frame -- this panel only
    // reads it, plus feeds this frame's toolbar edits back in
    // (SetSearch/SetKindFilter). `open` is forwarded to ImGui::Begin (the
    // tab's X button; null = no X).
    AssetsPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                       const Arcane::Project* project, DocumentHost& docs,
                                       const AssetsPanelServices& services,
                                       bool* open = nullptr);
}
