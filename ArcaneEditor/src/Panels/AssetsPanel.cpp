#include "Panels/AssetsPanel.hpp"

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetActivityLog.hpp"    // AssetActivityEntry/Kind (Task 8's feed, the first reader)
#include "Panels/CreateAssetDialog.hpp"   // CreateAssetKind + the AssetKind bridge (Task 12)
#include "Widgets/CanvasPopupScope.hpp"   // ed::Suspend/Resume around the Graph lens's node menu
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiSelectableFlags_NoPadWithHalfSpacing (ruling 4, 2026-09-07)
// Plan 3 ruling 1: THE ONE TU that may name ax::NodeEditor for this panel.
// AssetsPanel.hpp holds the context as a void* and EditorWidgets stays
// node-editor-free precisely so this include never has to leave this file.
#include <imgui_node_editor.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // Toolbar / bottom bar band heights (spec s11.2's values table:
        // "toolbar wells / bottom bar | 24px / 24px"). The default ImGui
        // frame (Inter 16px body over the theme's untouched FramePadding.y=3,
        // EditorTheme.hpp's own comment) stands 22px tall; bumping
        // FramePadding.y to 4 for just the toolbar row (pushed/popped around
        // its controls) is what closes the last 2px to the pinned 24.
        constexpr float kToolbarFramePadY = 4.0f;
        constexpr float kBottomBarHeight  = 24.0f;
        // 2026-09-07 fix (mock parity, automation-measured): the vertical
        // gap between the toolbar row's bottom edge and the Browse body's
        // top edge, pixel-scanned off `OptionBC-Browse-FINAL.png` (7px of
        // pure background between the toolbar's own bottom border and the
        // body's own top border -- 61->69 border-to-border at the mock's
        // native resolution). No §5/§11.2 value was previously pinned for
        // this seam -- see DrawAssetsPanel's own comment for why it had
        // silently collapsed to 0px live.
        constexpr float kToolbarBodyGapPx = 7.0f;

        // Plan 3 Task 5: the width of the toolbar's PER-LENS slot when the
        // Graph lens fills it with its focus combo (spec s5 -- the slot is
        // empty on every other lens, so this width leaves the layout too).
        // A fixed width, not a content-derived one: the search well's flex
        // math subtracts it BEFORE the combo is drawn, and a label-derived
        // width would make the search box jump every time the user picked a
        // differently-named scene. 230px is the BOARD's own value, read off
        // `OptionD.dc.html`'s focus well (`width: 230px`) rather than
        // guessed -- longer names ellipsize inside the combo rather than
        // stealing the search well's room.
        constexpr float kGraphFocusComboWidth = 230.0f;

        // The Graph lens's "no scope root" label -- spelled ONCE, because
        // the combo's preview, the combo's own first entry and the bottom
        // bar's "focus:" clause must all read identically (ruling 6's nil
        // focus, in words).
        constexpr const char* kGraphFocusEverything = "everything";
        // ...and what the same three places say when `graphFocus` names an
        // asset the model no longer has an entry for -- a scene deleted
        // while it was the focus. NOT "everything": the projection does not
        // fall back to everything-mode there (AssetGraphViewModel::Build
        // either builds a tombstone-rooted view or, for a guid the reference
        // index cannot explain either, nothing at all), so saying
        // "everything" would describe a graph that is not on screen.
        constexpr const char* kGraphFocusMissing = "(missing)";

        // The lens strip's three labels, fixed regardless of which plan has
        // landed (spec s5: "Plan 1 ships the full three-button strip ...
        // layout pinned from day one, later plans enable, nothing shifts").
        // constexpr on a non-reference array makes every element itself
        // const, so the decayed pointer is `const char* const*` --
        // SegmentedStrip's exact parameter type, no cast needed.
        constexpr const char* kLensLabels[] = { "Browse", "Graph", "Status" };
        constexpr int kLensCount = 3;
        // All three bits: Browse (0) since Plan 1, Status (2) since Plan 2
        // Task 7, Graph (1) since Plan 3 Task 5 -- the mask is now saturated
        // and there is no fourth lens to gate. The strip's LAYOUT never
        // changed across any of the three: the same three buttons have been
        // drawn since Plan 1 at the same widths in the same order (spec
        // s5's "later plans enable, nothing shifts"), and only this mask ever
        // moved. Kept as a named constant rather than folded away, because
        // SegmentedStrip's own signature takes one and a future lens would
        // otherwise have nowhere to say "not yet".
        constexpr unsigned kLensEnabledMask = 0b111u;

        // Task 10 (spec s6/s11.2) fixed geometry.
        constexpr float kRailWidth        = 180.0f;
        constexpr float kRailRowHeight    = 26.0f;
        constexpr float kTableRowHeight   = 24.0f;
        constexpr float kChildIndent      = 20.0f;
        // 2026-09-07 nested folder groups (spec s6/s11.2): 20px per nesting
        // depth, stacked with kChildIndent above rather than merged into it --
        // the two are independently-motivated 20px units that happen to share
        // a value and COMPOUND (a fold child inside a depth-1 group sits at
        // depth*kGroupIndent + kChildIndent from the row's own base).
        constexpr float kGroupIndent      = 20.0f;
        constexpr float kTooltipWidth     = 210.0f;
        constexpr float kTooltipThumbSize = 64.0f;

        // Task 11 (spec s5/s6/s11.2) fixed geometry: the preview pane is
        // hidden below a 720px panel width (the table never drops below
        // readable width -- spec s5); its thumb is 140px; its action
        // buttons are full-width and 24px tall (§11.2's table row height,
        // reused rather than inventing a new pinned value).
        //
        // 2026-09-07 follow-up (spec s5/s11.2 addendum): the pane's width
        // is no longer a single pinned constant -- it is user-resizable
        // via a drag splitter (AssetsPanelState::previewPaneWidth, the
        // DESIRED width, session-only, matching every other field on that
        // struct). What was `kPreviewPaneWidth = 330.0f` becomes a default
        // (AssetsPanel.hpp's kAssetsPreviewPaneDefaultWidth, half the old
        // pinned width) + a clamp range, resizable within [min, max].
        constexpr float kPreviewPaneMinWidth     = 120.0f;
        constexpr float kPreviewPaneMaxWidth     = 480.0f;
        // The table's own readable-width floor: the splitter clamps the
        // pane down (rather than letting it squeeze the table into a
        // sliver) before the <720px hide rule would otherwise have to do
        // that job wholesale (requirement 3 of the follow-up brief).
        constexpr float kMinReadableTableWidth   = 200.0f;
        // The divider's hit width -- same recipe as ShaderEditorDocument.cpp's
        // PaneSplitter (kSplitBarPx), a few-px InvisibleButton strip.
        constexpr float kPreviewSplitBarPx       = 6.0f;
        constexpr float kPreviewHidePanelWidth   = 720.0f;
        constexpr float kPreviewThumbSize        = 140.0f;
        constexpr float kActionButtonHeight      = 24.0f;

        // 2026-09-07 user-directed, fourth revision (spec s6/s17): compact
        // side-by-side preview header -- thumb left, name/pills/path/guid/
        // cook stacked beside it, instead of always stacking thumb-above-
        // metadata. kPreviewCompactHeaderMinWidth is the PANE width (this
        // function's own `width` parameter, same units as
        // kPreviewPaneMinWidth/kPreviewPaneMaxWidth above) at and above
        // which the compact header draws; below it, today's stacked form is
        // unchanged. The spec calls the exact number an "implementer tuning
        // value, not a pinned constant" -- 250px is its own suggested
        // figure, kept verbatim rather than re-deriving a different one; the
        // shipped 165px default pane stays comfortably below it (spec's own
        // "stays on the stacked fallback" requirement), see the impl report
        // for the measured breakpoint math. kPreviewCompactTextColumnMin is
        // the floor the thumb yields to when the pane is between this
        // breakpoint and comfortably wide -- the "≥~110px text column"
        // figure from the same directive.
        constexpr float kPreviewCompactHeaderMinWidth = 250.0f;
        constexpr float kPreviewCompactTextColumnMin  = 110.0f;

        // Plan 2 Task 7 (spec s9.2/s11.2, redline
        // `renders/OptionE-Status-FINAL.png`) fixed geometry for the Status
        // lens.
        //
        // kStatusTileHeight is measured off the board (the tile band spans
        // y=84..148 at the render's native size) and is also exactly what the
        // tile's own content needs: StatTile's 8px pad + the 24px number's
        // line + its 2px gap + the 13px label's line + 8px pad lands just
        // inside 64. kStatusTileMinWidth is a floor for a very narrow panel,
        // so four tiles never collapse to nothing.
        //
        // kStatusSectionGap is an EXPLICIT gap, on top of ImGui's own
        // ItemSpacing.y on each side of it (4px + 6px + 4px = 14px between
        // one section's last item and the next section's label -- the board's
        // ~13px). The pill line height this file needs, to vertically centre a
        // pill it positions BY HAND rather than by SameLine, is the widget
        // layer's own exported kPillLineHeight (EditorWidgets.hpp) -- it used
        // to be restated here as a second 16px constant nothing kept in step.
        constexpr float kStatusTileHeight      = 64.0f;
        constexpr float kStatusTileMinWidth    = 72.0f;
        constexpr float kStatusSectionGap      = 6.0f;
        constexpr float kStatusProgressHeight  = 4.0f;   // queued card's strip
        constexpr float kStatusSelectionBorder = 2.0f;   // spec s10's node rule, applied to cards

        // Plan 2 Task 8 additions to the same fixed-geometry block above.
        //
        // kStatusRightColumnWidth: the board's two-column split below the
        // meter -- "Needs attention"/"Unreferenced" left, "Activity"/
        // "Scenes" right, side by side at the SAME starting Y (the render's
        // own layout, not this plan's invention). The spec does not pin an
        // exact split -- an implementer tuning value, not a §11.2 figure,
        // same footing as kPreviewCompactHeaderMinWidth's own precedent
        // comment above -- chosen wide enough for a feed row's longest
        // realistic line ("crate_albedo.png source changed -> queued")
        // without crowding the left column on a normal panel width.
        // kStatusProgressCaptionGap is the small vertical gap between the
        // queued card's progress strip and its new "N of M cooked" caption
        // (Task 7 review ruling B) -- the same 2px register as
        // TimelineFeed's own kLineGap and MeterBar's own kSegmentGap.
        constexpr float kStatusRightColumnWidth    = 300.0f;
        constexpr float kStatusProgressCaptionGap  = 2.0f;

        // The absolute sane-range clamp ONLY -- [kPreviewPaneMinWidth,
        // kPreviewPaneMaxWidth] -- and nothing else. This is the ONLY clamp
        // ever applied to a value before it is written into
        // AssetsPanelState::previewPaneWidth (the splitter's drag and its
        // double-click reset, both below, are the field's only two
        // writers). Keeping the table-floor cap OUT of this function is
        // exactly what a 2026-09-07 review fix required: that cap (see
        // ClampPreviewForLayout) depends on `panelWidth`, which changes on
        // every window resize, so folding it into the STORED desired width
        // would silently and PERMANENTLY forget the user's real preference
        // the instant the panel transiently narrows, with no way back once
        // it widens again -- a ratchet, not a clamp.
        float ClampPreviewSaneRange(float desired)
        {
            return std::clamp(desired, kPreviewPaneMinWidth, kPreviewPaneMaxWidth);
        }

        // The full LAYOUT clamp: the sane range above, THEN a further cap on
        // the pane so the table (rail + the splitter bar + the pane, all
        // inside `panelWidth` -- see the 2026-09-07 flush-gutters note below,
        // no ItemSpacing gaps are budgeted any more) never drops below
        // kMinReadableTableWidth. Used every frame to compute a purely
        // local, throwaway DRAWN width -- never fed back into the stored
        // desired width (see ClampPreviewSaneRange's own comment on why
        // not). The two floors cannot actually fight in practice --
        // showPreview only ever calls this at panelWidth >= 720, where even
        // the pane's own max clamp (kPreviewPaneMaxWidth) leaves the table
        // comfortably above its floor.
        //
        // 2026-09-07 (user nitpick, mock parity): rail|table and
        // table|splitter|pane now sit FLUSH (DrawBrowseLens's SameLine(0,0)
        // calls) -- OptionBC.dc.html has no gap between these regions, only
        // 1px hairline borders the rail and the table each own on their own
        // right edge. This budget must stay in lockstep with that layout:
        // panelWidth == kRailWidth + tableWidth + kPreviewSplitBarPx +
        // drawnWidth EXACTLY now (no `ItemSpacing.x * 3.0f` term), matching
        // DrawBrowseLens's own `tableWidth` formula term-for-term.
        float ClampPreviewForLayout(float desired, float panelWidth)
        {
            float w = ClampPreviewSaneRange(desired);
            // How far the pane's DRAWN width can grow this frame before the
            // table would drop under its own readable floor -- a cap ON THE
            // PANE (not a floor under the table; kMinReadableTableWidth is
            // that floor, this is the same constraint expressed in the
            // pane's own units).
            const float previewWidthCap = panelWidth - kRailWidth
                                         - kPreviewSplitBarPx - kMinReadableTableWidth;
            if (previewWidthCap < w)
                w = std::max(kPreviewPaneMinWidth, previewWidthCap);
            return w;
        }

        // KindIcon/KindLabel (the row icon glyph / the peek tooltip's kind
        // pill text): Panels/AssetPanelModel.hpp's shared definitions, as of
        // Task 15 -- this file's own copies (originally lifted from
        // AssetBrowser.cpp's internal-linkage duplicates) are retired in
        // favor of the one canonical source every representation now shares.

        const char* CookStateLabel(CookState cook)
        {
            switch (cook)
            {
                case CookState::Cooked:  return "Cooked";
                case CookState::Queued:  return "Queued";
                case CookState::Refused: return "Refused";
                case CookState::Unknown: return "Unknown";
            }
            return "Unknown";
        }

        // Materials-only subkind pill text (spec s3.1/s6: Fullscreen -> "post",
        // Sprite -> "sprite", Mesh -> "mesh"). nullptr when there is nothing to
        // show (non-material, or a material whose surface could not be read).
        const char* SubkindPillText(const AssetPanelEntry& e)
        {
            if (e.kind != AssetKind::Material || !e.surface)
                return nullptr;
            switch (*e.surface)
            {
                case Arcane::MaterialSurface::Sprite:     return "sprite";
                case Arcane::MaterialSurface::Mesh:       return "mesh";
                case Arcane::MaterialSurface::Fullscreen: return "post";
            }
            return nullptr;
        }

        // Ruling 5 (desk pass, 2026-09-07): "We can shorten it and have a
        // hover tooltip for the full path?" -- mount paths carry a
        // "scheme://" prefix (MountTable.hpp: "game", "engine",
        // "plugin/<name>", "diag", ...; the preview pane's own mountPath
        // field comment: `"game://materials/glow.arcmat"`), which at the
        // pane's 165px default width ate most of the ellipsis budget
        // (COMPARISON.md: `game://textures/uv...` vs the mock's clean
        // `textures/uv_marker.png`). Strips whatever precedes "://" -- not
        // just the literal "game" scheme -- so every mount stays readable.
        // Falls back to the whole string unchanged if there is no "://" at
        // all (should not happen for a real mount path, but this is display
        // code, not a parser -- never assert on it).
        std::string_view ContentRelativePath(std::string_view mountPath)
        {
            const std::size_t sep = mountPath.find("://");
            return (sep == std::string_view::npos) ? mountPath : mountPath.substr(sep + 3);
        }

        // Rail "+" gate (spec s6): only kinds with a Create-menu entry get
        // the hover create affordance. Textures/Data/Audio/Font/Diagnostic/
        // Other get none.
        bool RailKindCreatable(int kind)
        {
            switch (static_cast<AssetKind>(kind))
            {
                case AssetKind::Material:
                case AssetKind::Sprite:
                case AssetKind::Mesh:
                case AssetKind::Scene:
                    return true;
                default:
                    return false;
            }
        }

        // See AssetsPanelState::groupOpen/childrenOpen's own doc comment:
        // these mirror the model's private defaults exactly (open, except the
        // diag:// mount root -- GroupDefaultOpen, spec s5 third revision;
        // children collapsed) so the panel can pick the right chevron glyph
        // and compute the flipped value to push through Set*Open.
        bool GroupIsOpen(const AssetsPanelState& state, const std::string& folder)
        {
            const auto it = state.groupOpen.find(folder);
            return it == state.groupOpen.end() ? GroupDefaultOpen(folder) : it->second;
        }

        bool ChildrenAreOpen(const AssetsPanelState& state, const Arcane::Guid& texture)
        {
            const auto it = state.childrenOpen.find(texture);
            return it == state.childrenOpen.end() ? false : it->second;
        }

        // The project's recorded boot scene as a guid -- the ONE parse of
        // `Manifest().bootScene` this file does, for every lens that marks the
        // boot scene (the Browse lens's "boot" pill, the Status lens's scene
        // cards, spec s6). No project, or an empty/unparseable bootScene,
        // resolves to the NIL guid, which no real asset guid ever equals, so
        // the marker simply never lights up rather than needing a second
        // "is there one at all" flag at each call site.
        //
        // Cheap enough to call once per lens body per frame; deliberately NOT
        // called per ROW (the lens bodies hoist it into a local first).
        Arcane::Guid BootSceneGuid(const Arcane::Project* project)
        {
            return project
                 ? Arcane::Guid::FromString(project->Manifest().bootScene).value_or(Arcane::Guid::Nil())
                 : Arcane::Guid::Nil();
        }

        // Every Scene entry, name-sorted (ties broken on mount path, so the
        // order is total even for two scenes with the same stem). Two
        // consumers: the Status lens's Scenes rollup and the Graph lens's
        // focus combo -- the same list in the same order in both, which is
        // exactly what makes "the scene I just saw in Status" findable in the
        // combo. The sort is over POINTERS into model.Entries(), whose
        // iteration order is an unordered_map's and therefore not stable
        // frame-to-frame; sorting is what makes the combo's contents
        // deterministic at all, not merely tidy.
        std::vector<const AssetPanelEntry*> ScenesByName(const AssetPanelModel& model)
        {
            std::vector<const AssetPanelEntry*> scenes;
            for (const auto& [guid, entry] : model.Entries())
                if (entry.kind == AssetKind::Scene)
                    scenes.push_back(&entry);
            std::sort(scenes.begin(), scenes.end(),
                      [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                      { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; });
            return scenes;
        }

        // What the Graph lens's current scope root is CALLED -- the combo's
        // preview text and the bottom bar's "focus:" clause, one spelling so
        // the two bands can never disagree about what is on screen. The
        // returned pointer is either a literal or borrowed from the model's
        // entry (stable for the frame -- Find()'s own doc comment; nothing
        // between here and the draw mutates the model).
        //
        // fileName, not name: the render comparison against
        // `OptionD-Graph-FINAL.png` caught the stem spelling naming the SAME
        // scene two ways one band apart -- the graph's own node header says
        // "main.arcscene" (DrawGraphNode) and the Status lens's scene cards
        // say "main.arcscene" (DrawSceneCard), so a toolbar reading "main"
        // was the panel's only dissenting voice. The board agrees
        // (`focus: main.arcscene`).
        const char* GraphFocusLabel(const AssetPanelModel& model, const Arcane::Guid& focus)
        {
            if (!focus.IsValid())
                return kGraphFocusEverything;
            const AssetPanelEntry* e = model.Find(focus);
            return e ? e->fileName.c_str() : kGraphFocusMissing;
        }

        // Resolve + route a double-click / Enter-open. Copied VERBATIM from
        // AssetBrowser.cpp:162-179's routing: a scene is not a DocumentHost
        // document (it replaces the editing session), so its path comes back
        // in `actions.openScene` for the host to load under the unsaved-
        // changes guard; every other kind opens through `docs`.
        void OpenAssetRow(const AssetPanelEntry& e, const Arcane::Project* project,
                          DocumentHost& docs, AssetsPanelActions& actions)
        {
            if (!project)
                return;
            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
            if (path)
            {
                if (e.kind == AssetKind::Scene)
                    actions.openScene = *path;
                else
                    docs.OpenPath(*path);
            }
            else
                ARC_WARN("Assets: '{}' did not resolve to a file", e.mountPath);
        }

        // The unified Create menu's entries (spec s7), spelled ONCE and shared
        // by the toolbar's `+ Create` popup and every row's context-menu
        // "Create" submenu -- the invariant ("no creation path may bypass
        // CreateAssetRequest") is only cheap to hold if there is one list.
        //
        // `enabled` was the only difference between the two call sites while
        // Mesh/Sprite/Scene had no dialog fields to land on (Task 12): the
        // toolbar's entries went live then, the row context menu's stayed
        // disabled. Both are live as of Task 13 -- kept as a parameter rather
        // than collapsed to a bare call so a future producer (Plan 3's graph
        // pin-drag) can still gate itself the same way without a third copy
        // of this list. No per-row prefill flows through here: a row's own
        // "Create ▸ Sprite..." does not pre-pick THIS row's texture (the
        // dedicated "Create Sprite" quick action above it already covers
        // that exact case, mint-or-reuse and open included) -- the generic
        // submenu opens the SAME dialog the toolbar's `+ Create` does, empty
        // texture field and all.
        void DrawCreateMenuEntries(AssetsPanelActions& actions, bool enabled)
        {
            ImGui::BeginDisabled(!enabled);
            const auto entry = [&](const char* label, CreateAssetKind kind)
            {
                if (ImGui::MenuItem(label))
                    actions.requestCreateKind = static_cast<int>(kind);
            };
            entry(ICON_LC_PALETTE " Material...",         CreateAssetKind::Material);
            entry(ICON_LC_LAYERS  " Material Instance...", CreateAssetKind::MaterialInstance);
            ImGui::Separator();
            entry(ICON_LC_BOX          " Mesh...",   CreateAssetKind::Mesh);
            entry(ICON_LC_STICKER      " Sprite...", CreateAssetKind::Sprite);
            entry(ICON_LC_CLAPPERBOARD " Scene...",  CreateAssetKind::Scene);
            ImGui::EndDisabled();
        }

        void DrawCreateMenu(AssetsPanelActions& actions)
        {
            if (!ImGui::BeginPopup("##createmenu"))
                return;
            DrawCreateMenuEntries(actions, /*enabled=*/true);
            ImGui::EndPopup();
        }

        // Toolbar band: + Create -> search (flex) -> [per-lens slot: Graph's
        // focus combo, Plan 3 Task 5; EMPTY on Browse and Status] -> lens
        // strip anchored right-most (spec s5). Mutates `state` in place; the
        // create popup's entries are LIVE from Task 12 -- they set
        // `actions.requestCreateKind`, which EditorApp routes to the one
        // BeginCreateAsset entry.
        void DrawToolbar(AssetsPanelState& state, AssetPanelModel& model, AssetsPanelActions& actions)
        {
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(style.FramePadding.x, kToolbarFramePadY));

            if (ImGui::Button(ICON_LC_PLUS " Create " ICON_LC_CHEVRON_DOWN))
                ImGui::OpenPopup("##createmenu");
            DrawCreateMenu(actions);

            ImGui::SameLine();

            // Search well width = remaining minus the lens strip minus the
            // per-lens slot (0 unless the Graph lens is showing its focus
            // combo). Each subtracted widget also costs the ItemSpacing.x
            // that its own SameLine inserts BEFORE it, so the slot's charge
            // is its width PLUS one spacing -- the strip's is already
            // spelled the same way on the line below.
            float stripWidth = 0.0f;
            for (const char* label : kLensLabels)
                stripWidth += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
            const bool  showFocusCombo = (state.lens == AssetLens::Graph);
            const float focusSlotWidth = showFocusCombo
                                       ? kGraphFocusComboWidth + style.ItemSpacing.x
                                       : 0.0f;
            // The 80px floor is what keeps the search well usable (and its
            // width non-negative) on a panel too narrow to pay for all three
            // bands -- ImGui then simply lets the row overflow to the right
            // rather than this code handing it a negative width.
            const float searchWidth = std::max(80.0f,
                ImGui::GetContentRegionAvail().x - stripWidth - style.ItemSpacing.x - focusSlotWidth);

            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##assetssearch", ICON_LC_SEARCH " search...",
                                     state.search, sizeof(state.search));
            model.SetSearch(state.search);
            model.SetKindFilter(state.railKind);

            // ---- the per-lens slot: Graph's focus combo (spec s5/s10) -----
            // Scope the graph to ONE scene, or to "everything" (ruling 6's
            // nil focus). Writing state.graphFocus is all this takes: the
            // lens's own dirty check compares graphBuiltFocus and rebuilds
            // the projection on the next frame, so there is no rebuild call
            // to make here.
            if (showFocusCombo)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(kGraphFocusComboWidth);
                // "focus: <name>" -- the BOARD's exact preview string
                // (`OptionD.dc.html`: `<span>focus:</span> main.arcscene`),
                // per the controller's board-strings-win ruling. The board
                // paints its "focus:" half in kTextDim and the name in kText;
                // BeginCombo's preview is a single string in a single colour,
                // so the two-tone half of that is not expressible here without
                // replacing the combo with a hand-drawn widget -- not invented,
                // see the Task 5 fix report.
                char focusPreview[160];
                std::snprintf(focusPreview, sizeof(focusPreview), "focus: %s",
                              GraphFocusLabel(model, state.graphFocus));
                if (ImGui::BeginCombo("##graphfocus", focusPreview))
                {
                    if (ImGui::Selectable(kGraphFocusEverything, !state.graphFocus.IsValid()))
                        state.graphFocus = Arcane::Guid{};
                    // PushID per row, keyed by the guid: two scenes may share
                    // a stem ("main.arcscene" in two folders), and ImGui would
                    // otherwise give both Selectables the SAME id -- clicking
                    // either would activate the first.
                    for (const AssetPanelEntry* s : ScenesByName(model))
                    {
                        ImGui::PushID(s->guid.ToString().c_str());
                        // fileName for the same reason GraphFocusLabel uses
                        // it: this list and the Status lens's scene cards are
                        // the same scenes, and they read identically there.
                        if (ImGui::Selectable(s->fileName.c_str(), s->guid == state.graphFocus))
                            state.graphFocus = s->guid;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::SameLine();
            const int clickedLens = SegmentedStrip("##lens", kLensLabels, kLensCount,
                                                    static_cast<int>(state.lens), kLensEnabledMask);
            if (clickedLens >= 0)
                state.lens = static_cast<AssetLens>(clickedLens);

            ImGui::PopStyleVar();
        }

        // Bottom bar band: left = context ("N assets - S selected", becoming
        // "X of N shown" once rail or search filters) -- right = the digest
        // chip (amber refused count + dim cooking/unused). All three numbers
        // are LIVE as of Plan 2 Task 4: `unused` used to render as a literal
        // em-dash (spec s13 -- the digest never fabricates a 0 for a number it
        // cannot know) because HealthCounts had no such field; the model's
        // AssetReferenceIndex now supplies it.
        //
        // Plan 2 Task 8: `state` arrives non-const (not just to READ
        // state.lens for the left context below, but to WRITE it -- the
        // digest click-through switches lens directly, the same "the Draw*
        // function mutates state in place" convention DrawToolbar's own
        // SegmentedStrip handling already uses a few lines above this one).
        // This function has no header declaration to keep in step (it is
        // file-local, like every other Draw* helper above) -- only its
        // single call site in DrawAssetsPanel changes.
        void DrawBottomBar(AssetsPanelState& state, const AssetPanelModel& model)
        {
            if (!ImGui::BeginChild("##assetsbottombar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                ImGui::EndChild();
                return;
            }

            // A hairline divider from the body above, painted directly
            // rather than via ImGui::Separator() -- that call consumes its
            // own layout row, which would push this child past the 24px the
            // caller already reserved for it (DrawAssetsPanel's
            // BeginChild("##assetsbody", ImVec2(0, -kBottomBarHeight))).
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetWindowPos();
                dl->AddLine(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y),
                           ImGui::GetColorU32(ImGuiCol_Separator));
            }

            // The right edge of the content region, captured before drawing
            // anything -- GetCursorPosX() + GetContentRegionAvail().x is
            // invariant here (no columns/tables in play), so it is safe to
            // read once and reuse for the right-aligned digest below.
            const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            const float padY = std::max(0.0f, (kBottomBarHeight - ImGui::GetTextLineHeight()) * 0.5f);

            const HealthCounts health = model.Health();
            const bool filtered = model.Filtered();

            ImGui::SetCursorPosY(padY);
            // 128, not 64: Graph's form below embeds a SCENE NAME, and a real
            // one ("prototype_courtyard_lighting") overflows 64 on its own --
            // snprintf would then truncate mid-name with no other symptom.
            char left[128];
            // Graph reports what the SCOPED projection is showing out of the
            // whole project, plus the scope root itself -- N is
            // realNodeCount, which counts real ASSET nodes only (ruling 12:
            // neither the synthetic "+N more" companions nor tombstones are
            // assets). The wording is the BOARD's, verbatim (`OptionD.dc.html`:
            // `6 of 15 assets &middot; focus: main.arcscene`) per the
            // controller's board-strings-win ruling -- the word "assets" was
            // missing from the brief's format string.
            //
            // THE GATE (review finding I1). N is printed ONLY when the
            // projection on screen was built for the focus and the entries
            // this bar is about to name. It is not always: the Status lens's
            // "Focus in Graph" button flips `state.lens` from INSIDE the
            // already-dispatched Status body, so DrawGraphLens does not run
            // that frame at all -- yet this bar, which runs after the body,
            // already reads the NEW lens and would otherwise pair the PREVIOUS
            // build's realNodeCount (often 0 -- the lens may never have been
            // opened) with the new focus name. Spec §13: the bar never renders
            // an unknown as a zero; unknown is an em dash. Self-corrects on
            // the following frame, when the Graph body has actually run.
            //
            // The predicate itself is AssetsGraphProjectionIsCurrent (declared
            // in the header, defined at the bottom of this file) rather than a
            // conjunction spelled here, so the canvas test can ask the panel's
            // own question instead of restating it.
            const bool graphCurrent = AssetsGraphProjectionIsCurrent(state, model);
            // Status keeps ONE fixed form regardless of filter state (the lens
            // has no search box of its own to filter against); every other
            // lens keeps Browse's existing forms VERBATIM (plan doc Step 4).
            if (state.lens == AssetLens::Graph)
            {
                char shown[16];
                if (graphCurrent)
                    std::snprintf(shown, sizeof(shown), "%d", state.graph.realNodeCount);
                else
                    std::snprintf(shown, sizeof(shown), "\xE2\x80\x94");   // U+2014 EM DASH
                std::snprintf(left, sizeof(left), "%s of %d assets \xC2\xB7 focus: %s",
                              shown, health.total,
                              GraphFocusLabel(model, state.graphFocus));
            }
            else if (state.lens == AssetLens::Status)
                std::snprintf(left, sizeof(left), "%d assets \xC2\xB7 %d need attention",
                              health.total, health.refused + health.queued);
            else if (filtered)
                std::snprintf(left, sizeof(left), "%d of %d shown",
                              model.ShownAssetCount(), health.total);
            else
                std::snprintf(left, sizeof(left), "%d assets \xC2\xB7 %d selected",
                              health.total, model.selected.IsValid() ? 1 : 0);
            ImGui::TextUnformatted(left);

            // Digest chip, right-aligned. `refusedPart` + `restPart`
            // concatenated character-for-character is what gets DRAWN below
            // (two colored segments, zero SameLine spacing between them), so
            // measuring their concatenation is exactly the width that draw
            // occupies.
            char refusedPart[48];
            std::snprintf(refusedPart, sizeof(refusedPart), "%s %d refused",
                          ICON_LC_TRIANGLE_ALERT, health.refused);
            char restPart[96];
            std::snprintf(restPart, sizeof(restPart),
                          " \xC2\xB7 %d cooking \xC2\xB7 %d unused", health.queued, health.unused);
            char digestFull[160];
            std::snprintf(digestFull, sizeof(digestFull), "%s%s", refusedPart, restPart);
            const float digestWidth = ImGui::CalcTextSize(digestFull).x;

            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - digestWidth));
            ImGui::SetCursorPosY(padY);
            const ImVec2 digestScreenPos = ImGui::GetCursorScreenPos();
            ImGui::TextColored(Theme::kAmber, "%s", refusedPart);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s", restPart);

            // Digest click-through (spec s5): an InvisibleButton laid over
            // the rect just drawn -- captured BEFORE the two TextColored/
            // TextDisabled calls above, since neither is meant to change
            // appearance on hover/press, a bare hit-test overlay is the
            // smaller change (this file already overlays a full-body
            // InvisibleButton for the identical reason in DrawAttentionCard).
            // A no-op when already on Status, per spec.
            ImGui::SetCursorScreenPos(digestScreenPos);
            if (ImGui::InvisibleButton("##digestclick", ImVec2(digestWidth, ImGui::GetTextLineHeight())))
                state.lens = AssetLens::Status;

            ImGui::EndChild();
        }

        // ---- Plan 3 Task 4: the Graph tooltip's edge-summary lines ---------
        // Read off the SAME AssetReferenceIndex the graph itself is projected
        // from, so the numbers can never disagree with the wires on screen.
        // Two dim lines: the counts, then up to three of the outbound targets
        // BY NAME (Interactions-FINAL's "one extra line" carries both a count
        // clause and named neighbours; splitting them is what keeps a
        // three-filename list inside the 210px tooltip).
        //
        // "M out" counts DISTINCT targets, not raw refs: one source may name
        // one target twice (two material slots pointing at one material), and
        // "2 out" beside a single listed name reads as a bug in the panel
        // rather than as a fact about the asset. `inbound` needs no such care
        // -- AssetReferenceIndex keeps it sorted-unique by contract.
        void DrawGraphEdgeSummary(const AssetPanelModel& model, const Arcane::Guid& guid)
        {
            const AssetReferenceIndex::Node* node = model.RefIndex().Find(guid);
            if (!node)
                return;   // never walked and never named as a target

            std::vector<Arcane::Guid> targets;
            targets.reserve(node->outbound.size());
            for (const Arcane::AssetRef& r : node->outbound)
                if (std::find(targets.begin(), targets.end(), r.target) == targets.end())
                    targets.push_back(r.target);

            ImGui::TextDisabled("%d in \xC2\xB7 %d out",
                                static_cast<int>(node->inbound.size()),
                                static_cast<int>(targets.size()));
            if (targets.empty())
                return;

            constexpr std::size_t kNamedTargets = 3;
            std::string line;
            for (std::size_t i = 0; i < targets.size() && i < kNamedTargets; ++i)
            {
                if (i != 0)
                    line += ", ";
                // A target with no entry is a tombstone (or an asset this
                // walk has not reached yet): the short-guid form is the same
                // name the graph's own ghost node wears for it.
                const AssetPanelEntry* t = model.Find(targets[i]);
                line += t ? t->fileName : targets[i].ToString().substr(0, 8);
            }
            if (targets.size() > kNamedTargets)
            {
                char more[24];
                std::snprintf(more, sizeof(more), " +%d more",
                              static_cast<int>(targets.size() - kNamedTargets));
                line += more;
            }
            // Wrapped, unlike the fixed-width lines above it: three file names
            // routinely overrun 210px, and the tooltip's SetNextWindowSize
            // pins the WIDTH only (height is auto), so wrapping grows the box
            // instead of clipping the text.
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        // ---- Task 10: the peek tooltip (spec s8) ---------------------------
        // File-local per the brief: rows, child rows and (later, Task 11) the
        // preview pane's Derived list all hover the same asset. Text-and-
        // images only -- never a button (a tooltip is not interactable).
        //
        // `forceShow` (Plan 2 Task 8): TimelineFeed draws every row's hover
        // hit-test INSIDE its own per-row loop (EditorWidgets.cpp), so by
        // the time this file's caller can react to the result, ImGui's
        // "last submitted item" is whichever row TimelineFeed drew LAST --
        // never necessarily the hovered one. The activity feed already
        // knows (from TimelineFeedResult::hoveredIndex, computed at the
        // right moment) that a specific row IS hovered, so it passes true
        // here to skip the (now-wrong) IsItemHovered() re-check entirely.
        // BeginTooltip() itself positions near the mouse regardless of
        // "last item", so this is safe. Every existing call site keeps the
        // default and is unaffected.
        //
        // `withEdgeSummary` (Plan 3 Task 4): the Graph lens's one extra line
        // (Interactions-FINAL's Graph column -- "same tooltip + one extra
        // line"). Off for every other caller, which is why it is a defaulted
        // parameter rather than a second helper: the anatomy above is spec
        // s8's and must stay ONE list, not two that drift.
        void DrawAssetPeekTooltip(const AssetPanelModel& model, const AssetsPanelServices& services,
                                  const Arcane::Guid& guid, bool forceShow = false,
                                  bool withEdgeSummary = false)
        {
            if (!forceShow && !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                return;
            const AssetPanelEntry* e = model.Find(guid);
            if (!e)
                return;

            ImGui::SetNextWindowSize(ImVec2(kTooltipWidth, 0.0f));
            ImGui::BeginTooltip();

            const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(guid) : 0;
            if (thumb != 0)
            {
                ImGui::Image(static_cast<ImTextureID>(thumb), ImVec2(kTooltipThumbSize, kTooltipThumbSize));
            }
            else
            {
                const ImVec2 boxMin = ImGui::GetCursorScreenPos();
                const char* icon = KindIcon(e->kind);
                const ImVec2 iconSize = ImGui::CalcTextSize(icon);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(boxMin.x + (kTooltipThumbSize - iconSize.x) * 0.5f,
                           boxMin.y + (kTooltipThumbSize - iconSize.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), icon);
                ImGui::Dummy(ImVec2(kTooltipThumbSize, kTooltipThumbSize));
            }

            ImGui::TextUnformatted(e->fileName.c_str());
            ImGui::SameLine();
            AssetPill(KindLabel(e->kind));
            if (const char* sub = SubkindPillText(*e))
            {
                ImGui::SameLine();
                AssetPill(sub);
            }
            if (e->isInstance)
            {
                ImGui::SameLine();
                AssetPill("inst");
            }

            ImGui::TextDisabled("%s", e->mountPath.c_str());
            ImGui::TextDisabled("%s", CookStateLabel(e->cook));
            ImGui::TextDisabled("%s", e->guid.ToString().c_str());
            if (withEdgeSummary)
                DrawGraphEdgeSummary(model, guid);

            ImGui::EndTooltip();
        }

        // ---- Task 10: the unified asset context menu's ITEMS (spec s6) -----
        // The menu BODY, with no popup bracket of its own, so that every
        // representation of an asset raises the SAME menu from the SAME code:
        // a Browse row opens it through BeginPopupContextItem
        // (DrawRowContextMenu just below), a Graph node opens it through
        // ed::ShowNodeContextMenu + BeginPopup inside a CanvasPopupScope
        // (DrawGraphLens, Plan 3 Task 4). Extracted in Task 4 for exactly that
        // second caller -- spec s6 says "the unified context menu, every
        // lens", and two copies of a list is how "unified" quietly stops being
        // true.
        //
        // `kindSpecific` gates the Material/Scene/Texture leading entries --
        // Type::Child rows are always folded 1:1 sprites, so none of those
        // three ever apply to one and the caller passes false to skip them.
        //
        // Deliberately does NOT touch the selection: "right-click selects" is
        // the OPENING gesture's business (it must happen once, when the menu
        // opens, not on every frame the popup is drawn), so each caller does
        // it at its own open site.
        void DrawAssetMenuItems(AssetsPanelActions& actions, const AssetPanelEntry& e,
                                bool kindSpecific)
        {
            if (kindSpecific)
            {
                bool any = false;
                if (e.kind == AssetKind::Material)
                {
                    any = true;
                    if (ImGui::MenuItem("New Instance..."))
                        actions.createInstanceOf = e.guid;
                }
                if (e.kind == AssetKind::Scene)
                {
                    any = true;
                    if (ImGui::MenuItem("Set as Boot Scene"))
                        actions.setBootScene = e.guid;
                }
                if (e.kind == AssetKind::Texture)
                {
                    any = true;
                    if (ImGui::MenuItem("Create Sprite"))
                        actions.createSpriteFrom = e.guid;
                }
                if (any)
                    ImGui::Separator();
            }

            if (ImGui::BeginMenu("Create"))
            {
                // Live since Task 13 -- see DrawCreateMenuEntries's own
                // comment on the `enabled` parameter.
                DrawCreateMenuEntries(actions, /*enabled=*/true);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Show in Explorer"))
                actions.showInExplorer = e.guid;
            if (ImGui::MenuItem("Copy Path"))
                actions.copyPath = e.guid;
            if (ImGui::MenuItem("Copy Guid"))
                actions.copyGuid = e.guid;
        }

        // ---- Task 10: shared row context menu (spec s6) --------------------
        // The Browse-side bracket around DrawAssetMenuItems above.
        void DrawRowContextMenu(AssetPanelModel& model, AssetsPanelActions& actions,
                                const AssetPanelEntry& e, bool kindSpecific)
        {
            if (!ImGui::BeginPopupContextItem())
                return;

            // Right-click acts on this row: make it the tracked selection so
            // the highlight + the Inspector/Assets-menu follow (matches
            // AssetBrowser.cpp's own old behavior). Idempotent per
            // AssetPanelModel::Select (no stamp bump when the guid is already
            // the selection), so re-running it every open frame is free.
            model.Select(e.guid);

            DrawAssetMenuItems(actions, e, kindSpecific);

            ImGui::EndPopup();
        }

        // ---- Task 10 fix round 1: shared per-row interaction attachment ----
        // Called IMMEDIATELY after RowWithThumb returns, with NOTHING else
        // submitted in between: BeginDragDropSource/BeginPopupContextItem/
        // IsItemHovered(ForTooltip) all key off ImGui's "last submitted
        // item", which at this exact point is the row's own Selectable
        // (RowWithThumb's own doc comment explains why it stays that way).
        // No anchor widget of any kind -- the original design's full-row
        // InvisibleButton overlay is DELETED, not replaced: it was the
        // Critical 1 bug (an AllowOverlap item is hoverable only when
        // g.HoveredIdPreviousFrame already names it, imgui.cpp:5112-5118 --
        // a same-size overlay submitted every frame starved the row's real
        // Selectable of that permanently, so `model.Select()` never fired
        // from a left-click).
        void AttachRowInteractions(AssetPanelModel& model, const Arcane::Project* project,
                                   DocumentHost& docs, const AssetsPanelServices& services,
                                   AssetsPanelActions& actions, const AssetPanelEntry& e,
                                   bool kindSpecificMenu)
        {
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                OpenAssetRow(e, project, docs, actions);

            DrawAssetPeekTooltip(model, services, e.guid);

            if (ImGui::BeginDragDropSource())
            {
                AssetDragPayload payload{ e.guid, e.kind };
                ImGui::SetDragDropPayload(kAssetDragType, &payload, sizeof(payload));
                ImGui::Text("%s %s", KindIcon(e.kind), e.name.c_str());
                ImGui::EndDragDropSource();
            }

            DrawRowContextMenu(model, actions, e, kindSpecificMenu);
        }

        // ---- Task 10: the rail (spec s6/s11.2) -----------------------------
        void DrawRail(AssetsPanelState& state, AssetPanelModel& model, AssetsPanelActions& actions)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::kChrome);
            if (ImGui::BeginChild("##assetsrail", ImVec2(kRailWidth, 0.0f), ImGuiChildFlags_None))
            {
                for (const RailEntry& re : model.Rail())
                {
                    ImGui::PushID(re.kind);

                    const bool selected = (state.railKind == re.kind);
                    const char* icon = (re.kind < 0) ? ICON_LC_LAYOUT_GRID
                                                     : KindIcon(static_cast<AssetKind>(re.kind));

                    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                    const float rowWidth = ImGui::GetContentRegionAvail().x;
                    const AssetRowResult res = RowWithThumb("##rail", 0, icon, re.label.c_str(),
                                                            selected, 0.0f, kRailRowHeight);
                    if (res.clicked)
                    {
                        state.railKind = re.kind;
                        model.SetKindFilter(re.kind);
                    }

                    // Trailing block, right-aligned: an optional hover "+"
                    // (creatable kinds only) then the dim count (spec s6).
                    //
                    // Fix round 2 (re-review): gating the "+" widget's
                    // SUBMISSION on `res.hovered` (fix round 1's shape) was
                    // its own new bug -- `res.hovered` is
                    // `IsItemHovered()` on a Selectable RowWithThumb now
                    // ALWAYS flags AllowOverlap, and `IsItemHovered`'s own
                    // AllowOverlap clause (imgui.cpp:5031-5035) has the
                    // identical "only when HoveredIdPreviousFrame == me"
                    // precondition ButtonBehavior's does. Once the "+" is
                    // submitted and the mouse sits over IT specifically, it
                    // (unflagged, submitted after the Selectable) legally
                    // steals HoveredId for that frame -- so
                    // HoveredIdPreviousFrame going into the NEXT frame is
                    // the "+"'s id, not the Selectable's, so the
                    // Selectable's own precondition fails THAT frame,
                    // `res.hovered` goes false, the "+" is not submitted,
                    // nothing steals HoveredId, the Selectable re-settles
                    // true next frame, the "+" reappears -- a permanent
                    // 2-frame oscillation (a fresh trace is in the fix
                    // report) that also breaks a press: SmallButton is
                    // PressedOnClickRelease, so a mouse-down frame sets
                    // ActiveId, and the very next frame the button is gone,
                    // so imgui.cpp:5796-5800 clears the stale ActiveId
                    // before the release ever lands.
                    //
                    // Fix: submit the "+" hit-region UNCONDITIONALLY every
                    // frame for a creatable kind (matching the texture
                    // expander's own already-correct shape in
                    // DrawAssetRow -- ITS submission is gated only on the
                    // frame-invariant `hasChildren`, never on a hover
                    // flag), and gate only the PAINT on hover. The paint
                    // condition ORs the row's own hover with the "+"
                    // hit-region's own hover: the two rects partition the
                    // row, so exactly one of the two is ever true for a
                    // given mouse position, and the combined signal never
                    // itself oscillates (this also incidentally fixes the
                    // reported horizontal jitter in the trailing count's
                    // position -- its own anchor no longer depends on
                    // whether the "+" happens to be painted this frame).
                    char countBuf[16];
                    std::snprintf(countBuf, sizeof(countBuf), "%d", re.count);
                    const float countW = ImGui::CalcTextSize(countBuf).x;
                    const float padX = ImGui::GetStyle().FramePadding.x;
                    const float rowCenterY = rowMin.y + kRailRowHeight * 0.5f;

                    // Count anchors flush to the row's right edge ALWAYS --
                    // never shifted by whether the "+" exists or is
                    // painted, so its position is fixed regardless.
                    const float countX = std::max(res.trailingPos.x, rowMin.x + rowWidth - padX - countW);

                    if (RailKindCreatable(re.kind))
                    {
                        const float plusBtnW = ImGui::CalcTextSize(ICON_LC_PLUS).x
                                              + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float btnH = ImGui::GetFrameHeight();
                        const ImVec2 btnMin(countX - ImGui::GetStyle().ItemSpacing.x - plusBtnW,
                                           rowCenterY - btnH * 0.5f);

                        ImGui::SetCursorScreenPos(btnMin);
                        const bool plusClicked = ImGui::InvisibleButton("##plus", ImVec2(plusBtnW, btnH));
                        const bool plusHovered = ImGui::IsItemHovered();
                        // TASK 12 FIX: this used to write `re.kind` -- an
                        // ASSETKIND int -- straight into a field whose
                        // contract is a CREATEASSETKIND value. The two enums
                        // do not share a numbering (AssetKind::Sprite is 6,
                        // CreateAssetKind::Sprite is 3), so the rail's Sprite
                        // "+" would have asked for a kind that does not exist
                        // and its Mesh/Scene "+" for the wrong one. Reconciled
                        // at the PRODUCER through the one sanctioned bridge
                        // (CreateAssetDialog.hpp's CreateKindForAssetKind), so
                        // the field's contract stays clean and the consumer
                        // needs no tagged-source branch. nullopt cannot
                        // happen here -- RailKindCreatable above gates this
                        // block to exactly the four kinds the bridge maps --
                        // but it is checked rather than asserted, because a
                        // future kind added to one list and not the other
                        // should silently do nothing, not raise a bogus
                        // request.
                        if (plusClicked)
                            if (const auto createKind =
                                    CreateKindForAssetKind(static_cast<AssetKind>(re.kind)))
                                actions.requestCreateKind = static_cast<int>(*createKind);

                        if (res.hovered || plusHovered)
                        {
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImVec2 btnMax(btnMin.x + plusBtnW, btnMin.y + btnH);
                            dl->AddRect(btnMin, btnMax,
                                       ImGui::GetColorU32(plusHovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
                            const ImVec2 glyphSize = ImGui::CalcTextSize(ICON_LC_PLUS);
                            dl->AddText(ImVec2(btnMin.x + (plusBtnW - glyphSize.x) * 0.5f,
                                              btnMin.y + (btnH - glyphSize.y) * 0.5f),
                                       ImGui::GetColorU32(ImGuiCol_Text), ICON_LC_PLUS);
                        }
                    }

                    ImGui::SetCursorScreenPos(ImVec2(countX, rowCenterY - ImGui::GetTextLineHeight() * 0.5f));
                    ImGui::TextDisabled("%s", countBuf);

                    // Absolute-position the NEXT row explicitly rather than
                    // trusting ImGui's own newline bookkeeping to recover
                    // the right Y from wherever the trailing content above
                    // was manually placed (RowWithThumb's own internal
                    // "put the cursor back" reset gets overwritten by every
                    // SetCursorScreenPos call this loop body makes after it
                    // returns) -- this is what SameLine() used to do for
                    // free when the trailing content was SameLine-chained;
                    // absolute positioning has to restate it explicitly.
                    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + kRailRowHeight));

                    ImGui::PopID();
                }

                // User nitpick (2026-09-07, mock parity): OptionBC.dc.html's
                // rail `<div>` carries `border-right: 1px solid #333333` --
                // the ONLY separator the mock draws between rail and table
                // (DrawBrowseLens's SameLine(0,0) removed the ItemSpacing.x
                // gutter that used to read as unwanted padding there).
                // Theme::kSeparator IS that exact hex (same mapping the
                // header-band line and AssetPill's border already use).
                // Drawn 1px INSIDE the child's own right edge, not exactly on
                // it -- a line submitted flush against a child window's own
                // ClipRect boundary is a coin flip on whether it survives
                // clipping, where an inset pixel reads identically to a
                // CSS border-box border and is never at risk.
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 wp = ImGui::GetWindowPos();
                    const float lineX = wp.x + ImGui::GetWindowWidth() - 1.0f;
                    dl->AddLine(ImVec2(lineX, wp.y), ImVec2(lineX, wp.y + ImGui::GetWindowHeight()),
                               ImGui::GetColorU32(Theme::kSeparator));
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ---- Task 10: one folder-group chrome row (spec s6/s11.2) ----------
        void DrawGroupRow(AssetsPanelState& state, AssetPanelModel& model, const AssetPanelRow& row)
        {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(Theme::kChrome));

            ImGui::PushID(row.groupName.c_str());
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();

            const bool open = GroupIsOpen(state, row.groupName);
            // Ruling 4 (desk pass, 2026-09-07): plain SpanAllColumns padded
            // the Selectable's highlight bb by half of style.ItemSpacing.y on
            // each side (imgui_widgets.cpp's Selectable(), the
            // NoPadWithHalfSpacing-gated block) -- 28px paint on this 24px
            // row. NoPadWithHalfSpacing turns that padding off, same fix as
            // RowWithThumb below.
            const bool clicked = ImGui::Selectable("##grouprow", false,
                                                   ImGuiSelectableFlags_SpanAllColumns |
                                                   ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                                   ImVec2(0.0f, kTableRowHeight));
            if (clicked)
            {
                const bool newOpen = !open;
                state.groupOpen[row.groupName] = newOpen;
                model.SetGroupOpen(row.groupName, newOpen);
            }

            // Nested-groups review fix round 1, Important 2: the MODEL shows this group's content
            // regardless of `open` while search is active (RebuildRows' own
            // override, immediately above this row's own asset rows in
            // Rows()) -- painting the real (possibly stale/closed) `open`
            // flag here would draw a right-pointing "collapsed" chevron
            // directly above rows that are visibly right there, and make the
            // toggle look inert. `effectiveOpen` is DISPLAY ONLY: the click
            // above still flips the REAL `open` flag unconditionally (a
            // harmless write while searching -- it takes effect the moment
            // the search box clears).
            const bool searchActive = state.search[0] != '\0';
            const bool effectiveOpen = open || searchActive;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float padX = ImGui::GetStyle().FramePadding.x;
            const float textY = rowMin.y + (kTableRowHeight - ImGui::GetTextLineHeight()) * 0.5f;

            // 2026-09-07 nested folder groups: the whole row (chevron, label,
            // count) shifts right by 20px per nesting depth (spec s6/s11.2).
            // Top-level groups keep depth 0 -> groupIndent 0 -> pixel-identical
            // to before this pass.
            const float groupIndent = static_cast<float>(row.groupDepth) * kGroupIndent;

            const char* chevron = effectiveOpen ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT;
            dl->AddText(ImVec2(rowMin.x + groupIndent + padX, textY), ImGui::GetColorU32(ImGuiCol_Text), chevron);
            const float chevronW = ImGui::CalcTextSize(chevron).x;

            // groupLabel is the LEAF segment only ("patterns/" for
            // "textures/patterns/") -- groupName (the full path) stays the
            // open-state key just above and in PushID, unchanged.
            const float nameX = rowMin.x + groupIndent + padX + chevronW + padX;
            dl->AddText(ImVec2(nameX, textY), ImGui::GetColorU32(ImGuiCol_Text), row.groupLabel.c_str());
            const float nameW = ImGui::CalcTextSize(row.groupLabel.c_str()).x;

            // Ruling 3 (desk pass, 2026-09-07: "I liked the mock") -- the
            // count sits INLINE immediately after the group name, dim, a
            // small gap, rather than right-aligned at the row's far edge
            // (the §11.1 "RowWithThumb: ... right-aligned extras" reading
            // this row no longer follows; RowWithThumb's own trailing-pill
            // convention is untouched -- this is DrawGroupRow only).
            //
            // Nested-groups review fix round 1, rider 8: a BRIDGE row (Critical 1 -- an ancestor
            // synthesized with zero of its own visible rows, present only
            // because a descendant matches) would otherwise paint a literal
            // "0" here, reading as "this group is empty" when its subtree
            // plainly is not (the very rows under it prove that). Suppressed
            // for groupCount == 0 only -- a real, populated group's count
            // still always shows, including a single-item "1".
            if (row.groupCount > 0)
            {
                constexpr float kGroupCountGap = 6.0f;
                char countBuf[16];
                std::snprintf(countBuf, sizeof(countBuf), "%d", row.groupCount);
                dl->AddText(ImVec2(nameX + nameW + kGroupCountGap, textY),
                           ImGui::GetColorU32(ImGuiCol_TextDisabled), countBuf);
            }

            ImGui::PopID();
        }

        // ---- 2026-09-07 desk-pass ruling 2: the "Name" column header band --
        // The mock draws a chrome-toned band above the table with a dim
        // "Name" label (COMPARISON.md: "absent -- BeginTable(\"##assets\", 1,
        // ...), no TableSetupColumn/TableHeadersRow"). Painted with the SAME
        // idiom DrawGroupRow above already uses -- TableSetBgColor(RowBg0,
        // Theme::kChrome) + drawlist text -- rather than ImGui's own
        // TableSetupColumn/TableHeadersRow mechanism, because that mechanism
        // computes its row height from CellPadding/font metrics, not the
        // pinned kTableRowHeight every other row (and the clipper, and the
        // scroll-position arithmetic in DrawTable) assumes exactly; this way
        // the header shares the identical 24px pitch with zero risk of it
        // drifting from the body rows it sits above. Static and
        // non-interactive (no Selectable) -- the mock shows a plain label,
        // no sort affordance -- so it paints no hover/click highlight.
        //
        // Scroll-fixed via ImGui's native row-freeze (TableSetupScrollFreeze
        // in DrawTable, called once, before this row is submitted -- its own
        // IsLayoutLocked assert requires that ordering), NOT by hoisting this
        // draw outside the table entirely: freezing keeps it column-aligned
        // with the body for free (same TableSetColumnIndex(0) cell, same
        // horizontal clip/scroll), where a separate sibling child window
        // would have to re-derive that alignment by hand.
        void DrawNameHeaderRow()
        {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(Theme::kChrome));

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const float padX = ImGui::GetStyle().FramePadding.x;
            const float textY = rowMin.y + (kTableRowHeight - ImGui::GetTextLineHeight()) * 0.5f;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(rowMin.x + padX, textY),
                       ImGui::GetColorU32(ImGuiCol_TextDisabled), "Name");

            // User nitpick (2026-09-07): the mock's header <div> carries
            // `border-bottom: 1px solid #333333` (OptionBC.dc.html) --
            // Theme::kSeparator IS that exact hex (AssetPill's own comment
            // makes the same mapping for its border), so no new token is
            // needed. Full row width, drawn at the row's own bottom edge
            // (rowMin.y + kTableRowHeight is already pixel-integral -- same
            // "no +0.5" convention DrawBottomBar's own hairline divider
            // uses just above this file, and it measures crisp there too).
            const float lineY = rowMin.y + kTableRowHeight;
            dl->AddLine(ImVec2(rowMin.x, lineY), ImVec2(rowMin.x + rowWidth, lineY),
                       ImGui::GetColorU32(Theme::kSeparator));
        }

        // ---- Task 10: one top-level asset row (spec s6/s11.2) --------------
        void DrawAssetRow(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetsPanelServices& services, AssetsPanelActions& actions,
                          const AssetPanelEntry& e, const Arcane::Guid& bootGuid, int groupDepth)
        {
            ImGui::PushID(e.guid.ToString().c_str());

            const bool hasChildren = (e.kind == AssetKind::Texture) && !e.derivedChildren.empty();
            const bool childrenOpen = hasChildren && ChildrenAreOpen(state, e.guid);
            // Nested-groups review fix round 1, Important 2's consistency
            // twin (not itself named by the review, but the identical bug
            // class): since Important 4 the MODEL renders a matching derived
            // child regardless of `childrenOpen` while search is active, so
            // painting the real (possibly closed) flag here would show a
            // right-pointing "collapsed" chevron and a stale count pill
            // directly above a child row that is visibly right there.
            // `effectiveChildrenOpen` is DISPLAY ONLY -- the toggle below
            // still flips the REAL flag, same reasoning as DrawGroupRow's
            // own `effectiveOpen` just above it in this file.
            const bool searchActive = state.search[0] != '\0';
            const bool effectiveChildrenOpen = childrenOpen || searchActive;
            const bool refused = (e.cook == CookState::Refused);
            // 2026-09-07 nested folder groups: the row's own group-nesting
            // indent (20px/depth, spec s6/s11.2) stacks UNDER the existing
            // expander gutter -- the whole row (expander included, see below)
            // shifts right by groupIndentPx first, then reserves its own
            // kChildIndent for the expander exactly as before.
            //
            // 2026-09-07 user-directed follow-up ("for the rows to be
            // indented starting at their icons, so the row is farther
            // indented than it already is"): asset rows now indent ONE FULL
            // LEVEL beneath their own group's band, not flush with it -- the
            // `+ 1` is the entire change. A row under a band at the band's
            // own indent X (still `groupDepth * kGroupIndent`, DrawGroupRow
            // above -- UNCHANGED) now starts at X+20; this is draw-side
            // geometry only, `groupDepth` itself (the DATA) is untouched.
            const float groupIndentPx = static_cast<float>(groupDepth + 1) * kGroupIndent;
            // The expander gutter is reserved only for textures with a
            // folded child -- refused now wears its OWN corner badge on the
            // thumb below (fix round 1, Important 5), so it never competes
            // with the expander for the same slot.
            const float indent = groupIndentPx + (hasChildren ? kChildIndent : 0.0f);

            const std::uint64_t thumbId = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            const char* icon = KindIcon(e.kind);
            const bool selected = (model.selected == e.guid);

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, indent, kTableRowHeight);
            if (res.clicked)
                model.Select(e.guid);

            // Fix round 1 (Critical 1): attach drag/context-menu/tooltip/
            // double-click HERE, immediately -- the row's Selectable is
            // still ImGui's last submitted item at this exact point (see
            // RowWithThumb's own doc comment). Everything drawn below this
            // line (the expander, the refused badge, the pills) must come
            // AFTER this call, not before -- each is either pure drawlist
            // (doesn't touch "last item") or a real item that would
            // otherwise steal that title away from the Selectable.
            AttachRowInteractions(model, project, docs, services, actions, e, /*kindSpecificMenu=*/true);

            // Expander: a REAL item submitted AFTER the row's Selectable
            // (which RowWithThumb flags AllowOverlap for exactly this), so
            // it genuinely receives its own clicks through ImGui's
            // front-to-back overlap arbitration (imgui.cpp:5112-5118)
            // rather than a manual screen-rect hit-test that a same-
            // coordinate popup from an unrelated row could fool (fix round
            // 1, Important 7).
            if (hasChildren)
            {
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x + groupIndentPx, rowMin.y));
                const std::string expId = "##exp_" + e.guid.ToString();
                if (ImGui::InvisibleButton(expId.c_str(), ImVec2(kChildIndent, kTableRowHeight)))
                {
                    const bool newOpen = !childrenOpen;
                    state.childrenOpen[e.guid] = newOpen;
                    model.SetChildrenOpen(e.guid, newOpen);
                }
                const char* chevron = effectiveChildrenOpen ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT;
                const ImVec2 cs = ImGui::CalcTextSize(chevron);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rowMin.x + groupIndentPx + (kChildIndent - cs.x) * 0.5f,
                          rowMin.y + (kTableRowHeight - cs.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), chevron);
            }

            // Refused marker: its OWN slot, a small badge overlaid on the
            // thumb's bottom-right corner -- independent of the expander
            // gutter above, so a refused texture with a folded child still
            // reads as refused rather than losing the marker to the
            // expander (fix round 1, Important 5; spec s6: refused rows
            // wear the amber triangle, unconditionally).
            //
            // Fix round 2 (minor, corrects §8.6): the original "-11px"
            // inset was a guess, not a measurement, and corner-anchoring a
            // glyph flush against a boundary has zero slack to absorb a
            // per-glyph render offset -- unlike the expander/group chevrons
            // above, which are CENTERED in their own cell and so have slack
            // on both sides. The merged Lucide icons carry a FIXED
            // GlyphOffset.y=3.0f baked in at atlas-build time
            // (EditorFonts.cpp:58); whether ImGui's dynamic font sizing
            // rescales that offset when a PushFont call overrides the size
            // (as here) is not asserted anywhere in this codebase, so this
            // fix assumes the WORST case (an absolute, non-scaling 3px
            // shift) rather than guess a second time: a smaller badge font
            // (10px, under the 12px pills) plus a 3px inward margin on top
            // of the CalcTextSize-measured extent keeps the glyph's
            // rendered pixels inside the 18px thumb cell EVEN IF the offset
            // does not shrink with the font size -- worst case its bottom
            // edge lands exactly at the thumb boundary, never past it.
            if (refused)
            {
                constexpr float kBadgeFontSize = 10.0f;
                constexpr float kBadgeMargin    = 3.0f;
                ImGui::PushFont(GetEditorFonts().interRegular, kBadgeFontSize);
                const ImVec2 badgeSize = ImGui::CalcTextSize(ICON_LC_TRIANGLE_ALERT);
                const float thumbY      = rowMin.y + (kTableRowHeight - kAssetRowThumbSize) * 0.5f;
                const float thumbRight  = rowMin.x + indent + kAssetRowThumbSize;
                const float thumbBottom = thumbY + kAssetRowThumbSize;
                const ImVec2 badgePos(thumbRight  - badgeSize.x - kBadgeMargin,
                                      thumbBottom - badgeSize.y - kBadgeMargin);
                ImGui::GetWindowDrawList()->AddText(badgePos, ImGui::GetColorU32(Theme::kAmber),
                                                    ICON_LC_TRIANGLE_ALERT);
                ImGui::PopFont();
            }

            // Trailing pills, in spec order: subkind, inst, boot, sliced,
            // derived-count. Positioned from `res.trailingPos` (the FIRST
            // pill only) rather than a bare SameLine() -- RowWithThumb's
            // name is pure drawlist overdraw now, so there is no real name
            // ITEM left for SameLine to inherit line-metrics from; ordinary
            // SameLine() chaining resumes correctly for every pill AFTER
            // the first (AssetPill's own Dummy is a real item).
            bool firstPill = true;
            const auto placePill = [&](const char* text, int variant = 0)
            {
                if (firstPill) { ImGui::SetCursorScreenPos(res.trailingPos); firstPill = false; }
                else           ImGui::SameLine();
                AssetPill(text, variant);
            };
            if (const char* sub = SubkindPillText(e))
                placePill(sub);
            if (e.isInstance)
                placePill("inst");
            if (e.kind == AssetKind::Scene && bootGuid.IsValid() && e.guid == bootGuid)
                placePill("boot", 1);
            if (e.kind == AssetKind::Sprite && e.sliced)
                placePill("sliced");
            if (hasChildren && !effectiveChildrenOpen)
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(e.derivedChildren.size()));
                placePill(buf);
            }

            ImGui::PopID();
        }

        // ---- Task 10: one derived-child row (spec s6/s11.2) ----------------
        void DrawChildRow(AssetsPanelState& /*state*/, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetsPanelServices& services, AssetsPanelActions& actions,
                          const AssetPanelEntry& e, int groupDepth)
        {
            ImGui::PushID(e.guid.ToString().c_str());

            const std::uint64_t thumbId = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            const char* icon = KindIcon(e.kind);
            const bool selected = (model.selected == e.guid);

            // 2026-09-07 nested folder groups: the fold-child's own +20px
            // indent (kChildIndent, unchanged) stacks ON TOP of its group's
            // 20px/depth indent -- the compound case spec s6/s11.2 calls out
            // explicitly.
            //
            // 2026-09-07 user-directed follow-up: "Child rows follow" the
            // same one-level-beneath-the-band shift DrawAssetRow's own
            // `groupIndentPx` just got -- the `+ 1` is the entire change. A
            // fold child under a band at indent X now sits at X+40 (X+20 for
            // the level shift, +20 more for its own existing fold indent).
            const float indent = static_cast<float>(groupDepth + 1) * kGroupIndent + kChildIndent;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, indent, kTableRowHeight);
            ImGui::PopStyleColor();
            if (res.clicked)
                model.Select(e.guid);

            // Fix round 1 (Critical 1): attach interactions before drawing
            // the pill -- see DrawAssetRow's own comment on ordering.
            AttachRowInteractions(model, project, docs, services, actions, e, /*kindSpecificMenu=*/false);

            ImGui::SetCursorScreenPos(res.trailingPos);
            AssetPill("derived");

            ImGui::PopID();
        }

        // ---- Task 10: the table (spec s6/s11.2) ----------------------------
        // `width` is 0.0f (ImGui's own "fill everything left on this line")
        // when Task 11's preview pane is hidden; otherwise the caller passes
        // the exact remainder after reserving the rail and the pinned 330px
        // preview column, so the three stay side by side without the table
        // fighting the preview for space.
        void DrawTable(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost& docs, const AssetsPanelServices& services, AssetsPanelActions& actions,
                       const Arcane::Guid& bootGuid, float width)
        {
            if (!ImGui::BeginChild("##assetscenter", ImVec2(width, 0.0f)))
            {
                ImGui::EndChild();
                return;
            }

            const std::vector<AssetPanelRow>& rows = model.Rows();

            // Step 4: scroll-to-selection, exactly once per external
            // selection change. Find the target row's INDEX up front so the
            // clipper can be told to include it even when it lies outside
            // the naturally visible range (ImGuiListClipper::IncludeItemByIndex
            // -- must be called before the first Step()).
            const bool wantsScroll = (state.seenSelectionStamp != model.selectionStamp);
            int scrollTargetIndex = -1;
            if (wantsScroll && model.selected.IsValid())
            {
                for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                    if (rows[i].type != AssetPanelRow::Type::Group && rows[i].guid == model.selected)
                    {
                        scrollTargetIndex = i;
                        break;
                    }
            }
            // Nothing to scroll to (filtered out, or selection cleared) --
            // stop retrying every frame.
            if (wantsScroll && scrollTargetIndex < 0)
                state.seenSelectionStamp = model.selectionStamp;

            // Fix round 1 (Important 2): TableNextRow(_, 24) actually grows
            // to 24 + CellPadding.y*2 (imgui_tables.cpp:1936-1937) -- the
            // theme leaves CellPadding at ImGui's stock (x, 3-ish) default,
            // so the real row PITCH was ~28-30px against the clipper's
            // (and every row-index/scroll-position computation's) 24px
            // assumption. Zeroing only the VERTICAL padding for the
            // duration of this table restores the pinned 24px pitch
            // (spec §11.2) exactly, without touching the horizontal
            // padding anything else in this cell might still want.
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                                ImVec2(ImGui::GetStyle().CellPadding.x, 0.0f));

            if (ImGui::BeginTable("##assets", 1,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings))
            {
                // Ruling 2 (desk pass, 2026-09-07): the "Name" header band,
                // frozen at the table's top edge. TableSetupScrollFreeze MUST
                // be called before the first row is submitted (its own
                // IsLayoutLocked assert) -- hence first, ahead of even the
                // scroll-target bookkeeping below. Submitted directly, not
                // through the clipper: `rows` (and the clipper over it) is
                // exactly the data rows, unchanged by this header.
                // Review fix (2026-09-07): ImGuiTableRowFlags_Headers here --
                // NOT for height (that's TableHeadersRow()'s job, deliberately
                // avoided, see DrawNameHeaderRow's own comment) but because
                // imgui_tables.cpp's TableEndRow only advances
                // table->RowBgColorCounter for rows WITHOUT this flag. Passing
                // None left this header consuming a zebra-parity slot, so
                // every asset/child row's alternating RowBg1 tint landed one
                // row off from where it did before the header existed.
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers, kTableRowHeight);
                ImGui::TableSetColumnIndex(0);
                DrawNameHeaderRow();

                // Fix round 1 (Important 4): a row a MOUSE just selected
                // (left-click, or the right-click that is about to open its
                // context menu) is by definition already on-screen -- you
                // cannot click what is not rendered. Re-centering it anyway
                // was the bug: it slides the row out from under a popup that
                // is anchored to screen coordinates at OPEN time and does
                // not follow the list. Gating the actual SetScrollHereY call
                // on "is the target genuinely outside the visible scroll
                // window" (read directly off the table's own inner scroll
                // region, which is the CURRENT window right after
                // BeginTable) fixes every origin uniformly: a mouse click
                // (always already-visible) never re-centers, while keyboard
                // Up/Down walking past the visible edge -- or a future
                // external selector (Task 11's preview-pane Derived list)
                // picking something scrolled away -- still correctly
                // recenters. This subsumes tagging each panel-side
                // model.Select() call site individually: there is exactly
                // one thing that actually needs to be true (was the row
                // visible already), and checking it directly cannot drift
                // out of sync the way remembering to tag every call site
                // could.
                bool targetAlreadyVisible = false;
                if (scrollTargetIndex >= 0)
                {
                    const float scrollY = ImGui::GetScrollY();
                    const float viewH = ImGui::GetWindowHeight();
                    const int firstVisible = static_cast<int>(scrollY / kTableRowHeight);
                    const int lastVisible = static_cast<int>((scrollY + viewH) / kTableRowHeight);
                    targetAlreadyVisible = (scrollTargetIndex >= firstVisible && scrollTargetIndex <= lastVisible);
                }

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(rows.size()), kTableRowHeight);
                if (scrollTargetIndex >= 0 && !targetAlreadyVisible)
                    clipper.IncludeItemByIndex(scrollTargetIndex);

                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        const AssetPanelRow& row = rows[i];
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, kTableRowHeight);
                        ImGui::TableSetColumnIndex(0);

                        switch (row.type)
                        {
                            case AssetPanelRow::Type::Group:
                                DrawGroupRow(state, model, row);
                                break;
                            case AssetPanelRow::Type::Asset:
                                if (const AssetPanelEntry* e = model.Find(row.guid))
                                    DrawAssetRow(state, model, project, docs, services, actions, *e, bootGuid, row.groupDepth);
                                break;
                            case AssetPanelRow::Type::Child:
                                if (const AssetPanelEntry* e = model.Find(row.guid))
                                    DrawChildRow(state, model, project, docs, services, actions, *e, row.groupDepth);
                                break;
                        }

                        if (i == scrollTargetIndex)
                        {
                            if (!targetAlreadyVisible)
                                ImGui::SetScrollHereY();
                            state.seenSelectionStamp = model.selectionStamp;
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();

            // Step 3 tail: keyboard, minimal v1 (spec s8). Up/Down move
            // Select through the VISIBLE rows (group rows are not navigable
            // targets); Enter opens. ChildWindows so focus anywhere inside
            // the table's own implicit scroll region (BeginTable's ScrollY
            // wraps itself in one) counts as "the table is focused".
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
            {
                std::vector<Arcane::Guid> nav;
                nav.reserve(rows.size());
                for (const AssetPanelRow& r : rows)
                    if (r.type != AssetPanelRow::Type::Group)
                        nav.push_back(r.guid);

                if (!nav.empty())
                {
                    const bool up   = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
                    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
                    if (up || down)
                    {
                        int idx = -1;
                        for (std::size_t i = 0; i < nav.size(); ++i)
                            if (nav[i] == model.selected) { idx = static_cast<int>(i); break; }
                        int next = (idx < 0) ? 0 : idx + (down ? 1 : -1);
                        next = std::clamp(next, 0, static_cast<int>(nav.size()) - 1);
                        model.Select(nav[static_cast<std::size_t>(next)]);
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) && model.selected.IsValid())
                {
                    if (const AssetPanelEntry* e = model.Find(model.selected))
                        OpenAssetRow(*e, project, docs, actions);
                }
            }

            ImGui::EndChild();
        }

        // ---- Task 11: one Derived-list row (spec s6/s11.2) -----------------
        // "Derived (N) list (each row: sprite icon + name; click Selects the
        // child)". `derivedChildren` is always a 1:1 folded sprite (the
        // model's own fold rule, AssetPanelEntry::derivedChildren's doc
        // comment: "1:1 sprites folded under me"), so `KindIcon(child->kind)`
        // reads as the sprite glyph unconditionally -- resolved through the
        // entry rather than hardcoding the icon so a future fold rule change
        // cannot silently desync this row from what it actually names.
        // Shares the same peek tooltip every other representation uses
        // (spec s8).
        void DrawDerivedRow(AssetPanelModel& model, const AssetsPanelServices& services,
                            const Arcane::Guid& childGuid)
        {
            const AssetPanelEntry* child = model.Find(childGuid);
            if (!child)
                return;

            ImGui::PushID(child->guid.ToString().c_str());
            const std::string label = std::string(KindIcon(child->kind)) + " " + child->fileName;
            if (ImGui::Selectable(label.c_str(), model.selected == child->guid))
                model.Select(child->guid);
            DrawAssetPeekTooltip(model, services, child->guid);
            ImGui::PopID();
        }

        // ---- 2026-09-07 follow-up: the table<->preview drag splitter --------
        // Mirrors ShaderEditorDocument.cpp's `PaneSplitter` recipe -- an
        // InvisibleButton owns the gap, and because ImGui holds ActiveId for
        // as long as the button is held, MouseDelta keeps arriving every
        // frame even after the cursor leaves the strip -- but works directly
        // in PIXELS rather than a 0..1 fraction (this pane's width is
        // already a pixel value, same convention as kRailWidth/kPreviewPane*
        // above) and never calls MarkIniSettingsDirty: spec s5 keeps this
        // panel's state session-only, unlike the Material panel's persisted
        // split ratio. Double-click restores the default width, same as
        // PaneSplitter's own reset gesture.
        //
        // 2026-09-07 review fix, round 2: the drag write baselines off the
        // PRIOR `desiredWidth` itself -- NOT off `drawnWidth` (this frame's
        // already-clamped layout width). Round 1's fix baselined off
        // `drawnWidth` specifically so a capped drag would track the mouse
        // from wherever the bar visually sat, but that reopened the same
        // ratchet bug class through the write path instead of the read
        // path: `IsItemActive()` goes true on the PRESS frame with
        // `MouseDelta == (0,0)`, so `desiredWidth = Clamp(drawnWidth - 0) =
        // drawnWidth` -- a bare, zero-motion click silently snapped the
        // stored desired width down to whatever the table-floor cap
        // currently was, and a sustained drag while capped re-baselined off
        // that same (unchanging, while still capped) `drawnWidth` every
        // frame instead of accumulating, so it never moved past one
        // frame's delta. Baselining off `desiredWidth` fixes both: a
        // zero-delta press is a no-op (`desiredWidth - 0 == desiredWidth`),
        // and a multi-frame drag accumulates against the field's own
        // running value exactly the way `ShaderEditorDocument.cpp`'s
        // `PaneSplitter` accumulates its ratio, frame over frame, for as
        // long as ActiveId is held. This function is `desiredWidth`'s ONLY
        // writer (drag below, double-click reset below that), and both
        // writes go through `ClampPreviewSaneRange` ONLY -- never
        // `ClampPreviewForLayout` -- so the table-floor cap still never
        // touches the stored value, only the caller's throwaway
        // `drawnWidth` local (DrawBrowseLens). The visible trade-off: a
        // drag that SHRINKS the pane while it is already capped needs to
        // first travel however many pixels separate the stale desired
        // value from today's cap before the pane visibly moves (there is
        // no way around this without re-corrupting the stored value on
        // every capped frame, which is the exact bug this fixes) -- widening
        // the panel afterward still snaps back to wherever that drag
        // actually left `desiredWidth`, not to the cap.
        void PreviewPaneSplitter(float& desiredWidth)
        {
            const ImVec2 size(kPreviewSplitBarPx, ImGui::GetContentRegionAvail().y);
            if (size.x <= 0.0f || size.y <= 0.0f)
                return;   // degenerate region -- InvisibleButton asserts on zero

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##previewsplit", size);
            const bool held    = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();
            if (held || hovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            if (held)
            {
                // Dragging the splitter LEFT (negative MouseDelta.x) hands
                // the table's space to the pane -- width grows by the same
                // distance the mouse moved, hence the sign flip. Baselined
                // off `desiredWidth` itself (see the function comment) --
                // a zero-motion press is a no-op, and a held multi-frame
                // drag accumulates correctly instead of re-snapping to a
                // capped value every frame.
                desiredWidth = ClampPreviewSaneRange(desiredWidth - ImGui::GetIO().MouseDelta.x);
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                desiredWidth = kAssetsPreviewPaneDefaultWidth;

            // Same three-tone ramp as ShaderEditorDocument's PaneSplitter
            // and ImGui's own docking splitter: hairline at rest, one step
            // brighter and one pixel wider on hover, brightest while held.
            const ImU32 col = ImGui::GetColorU32(held    ? ImGuiCol_SeparatorActive
                                               : hovered ? ImGuiCol_SeparatorHovered
                                                         : ImGuiCol_Separator);
            const float line = (held || hovered) ? 2.0f : 1.0f;
            const ImVec2 a(p0.x + (size.x - line) * 0.5f, p0.y);
            const ImVec2 b(a.x + line, p0.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, col);
        }

        // ---- Task 11: the preview pane (spec s5/s6/s11.2) -------------------
        // Layout order, pinned by the brief: 140px thumb -> name + kind/
        // subkind/inst pills -> path row -> guid row (click copies) -> cook
        // row -> separator -> Derived (N) list -> separator -> full-width
        // action buttons (Open, Show in Explorer, Copy Path, + one
        // kind-specific action). Empty selection is a dim "no selection"
        // line -- no other row renders in that state.
        void DrawPreviewPane(AssetPanelModel& model, const Arcane::Project* project, DocumentHost& docs,
                            const AssetsPanelServices& services, AssetsPanelActions& actions, float width)
        {
            if (!ImGui::BeginChild("##assetspreview", ImVec2(width, 0.0f), ImGuiChildFlags_None))
            {
                ImGui::EndChild();
                return;
            }

            const AssetPanelEntry* e = model.selected.IsValid() ? model.Find(model.selected) : nullptr;
            if (!e)
            {
                ImGui::TextDisabled("No selection");
                ImGui::EndChild();
                return;
            }

            // ---- 140px thumb: real thumb when resolvable, else the kind
            // icon centered over a `kWell` backdrop with a `kSeparator`
            // border seam (spec s6.1: "the Lucide kind icon on a well
            // background") -- the same image/icon composition
            // `DrawAssetPeekTooltip` uses at 64px, scaled up and framed.
            // Factored into a lambda (2026-09-07, compact-header revision)
            // since it is now drawn from two call sites (compact/stacked
            // below) with only `thumbSize` differing -- the drawing itself
            // is byte-identical to every prior revision.
            auto drawThumb = [&](float thumbSize)
            {
                const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
                const ImVec2 thumbMax(thumbMin.x + thumbSize, thumbMin.y + thumbSize);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(e->guid) : 0;
                if (thumb != 0)
                {
                    dl->AddImage(static_cast<ImTextureID>(thumb), thumbMin, thumbMax);
                }
                else
                {
                    dl->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(Theme::kWell));
                    const char* icon = KindIcon(e->kind);
                    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
                    dl->AddText(ImVec2(thumbMin.x + (thumbSize - iconSize.x) * 0.5f,
                                       thumbMin.y + (thumbSize - iconSize.y) * 0.5f),
                               ImGui::GetColorU32(ImGuiCol_Text), icon);
                }
                dl->AddRect(thumbMin, thumbMax, ImGui::GetColorU32(Theme::kSeparator));
                ImGui::Dummy(ImVec2(thumbSize, thumbSize));
            };

            // ---- name/pills + path/guid/cook rows. Factored into a lambda
            // (2026-09-07, compact-header revision) for the same reason as
            // `drawThumb` -- identical content and logic at both call sites,
            // only the surrounding container differs. `EllipsisToWidth`'s
            // `GetContentRegionAvail().x` call is UNCHANGED from every prior
            // revision -- in the stacked branch it still measures the whole
            // pane child, and in the compact branch it measures the
            // `##previewMeta` child's own (zero-padding) width instead,
            // simply by virtue of which window is current when this runs.
            // That's the ImGui-native equivalent of the mock's own
            // `min-width: 0` + `overflow: hidden` ellipsis fix (design
            // report, fourth revision) -- a bounding container, not a width
            // argument threaded through.
            auto drawMeta = [&]()
            {
                // ---- name (stem) + kind pill + subkind/inst pills
                //
                // 2026-09-07 review note: unlike the `path` row below, the
                // name here has NO EllipsisToWidth clamp in either branch --
                // pre-existing (Task 11), not introduced by the compact
                // header. It reads as a bigger risk now: the compact
                // column can be as narrow as kPreviewCompactTextColumnMin
                // (110px), and a long stem plus its trailing kind/subkind/
                // inst pills (all SameLine-chained) has less room to
                // overflow into than the old full-pane-width stacked row
                // did. Deferred rather than fixed here: a correct clamp
                // has to measure the pill run's own width FIRST and budget
                // the name against what's left, not reuse EllipsisToWidth's
                // single-string recipe -- a small feature of its own, out
                // of scope for a geometry-only padding pass with the
                // editor's own exe unavailable to re-capture against.
                ImGui::TextUnformatted(e->name.c_str());
                ImGui::SameLine();
                AssetPill(KindLabel(e->kind));
                if (const char* sub = SubkindPillText(*e))
                {
                    ImGui::SameLine();
                    AssetPill(sub);
                }
                if (e->isInstance)
                {
                    ImGui::SameLine();
                    AssetPill("inst");
                }

                // ---- path row: the content-relative path (scheme prefix
                // stripped -- ruling 5, 2026-09-07), ellipsized to whatever's
                // left on the line after the "path" label (EllipsisToWidth
                // stays the fallback for a still-long relative path at the
                // pane's narrower widths). A plain text hover tooltip carries
                // the FULL mount path -- this is NOT the §8 210px peek-tooltip
                // contract (no thumb, no kind/cook rows), just a path reveal.
                ImGui::TextDisabled("path");
                ImGui::SameLine();
                const std::string_view relPath = ContentRelativePath(e->mountPath);
                ImGui::TextUnformatted(EllipsisToWidth(relPath, ImGui::GetContentRegionAvail().x).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", e->mountPath.c_str());

                // ---- guid row: dim, click copies (spec s6: "guid
                // (click-to-copy)"). Routed through `actions.copyGuid` -- the
                // SAME field the row context menu's "Copy Guid" entry already
                // sets (AssetsPanel.cpp's DrawRowContextMenu) -- so the host's
                // one existing consumer (EditorAppFrame.cpp's
                // `ImGui::SetClipboardText(browserActions.copyGuid...)`) needs
                // no new wiring; "panel reports, app performs" stays intact.
                ImGui::TextDisabled("guid");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", e->guid.ToString().c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsItemClicked())
                    actions.copyGuid = e->guid;

                // ---- cook row: state string, refused in kAmber
                ImGui::TextDisabled("cook");
                ImGui::SameLine();
                if (e->cook == CookState::Refused)
                    ImGui::TextColored(Theme::kAmber, "%s", CookStateLabel(e->cook));
                else
                    ImGui::TextDisabled("%s", CookStateLabel(e->cook));
            };

            // 2026-09-07 user-directed, fourth revision (spec s6/s17):
            // side-by-side header at/above kPreviewCompactHeaderMinWidth,
            // today's stacked form (thumb above, metadata below -- every
            // prior revision, unchanged) below it. Measured against the
            // pane's own drawn `width` (this function's parameter, the same
            // units as kPreviewPaneMinWidth/kPreviewHidePanelWidth), not the
            // post-padding avail below -- so the breakpoint reads the same
            // number the splitter drag/double-click reset already use.
            const bool compactHeader = width >= kPreviewCompactHeaderMinWidth;
            if (compactHeader)
            {
                // §11.2's 140px thumb is unchanged; it only yields (via the
                // same std::min clamp every revision has used) when the
                // pane is too narrow to also leave a
                // kPreviewCompactTextColumnMin-wide text column beside it --
                // exactly the pinned "scaled down via the existing min()
                // logic" rule.
                const float avail = ImGui::GetContentRegionAvail().x;
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float thumbSize = std::min(kPreviewThumbSize,
                    std::max(0.0f, avail - spacing - kPreviewCompactTextColumnMin));
                const float textColumnWidth = std::max(0.0f, avail - thumbSize - spacing);

                ImGui::BeginGroup();
                drawThumb(thumbSize);
                ImGui::EndGroup();
                ImGui::SameLine();

                // Zero WindowPadding on this bounding-only column: it exists
                // purely to give `drawMeta`'s GetContentRegionAvail() calls a
                // column-width answer instead of a whole-pane one (see
                // `drawMeta`'s own comment); a visible inset was never part
                // of the mock.
                //
                // 2026-09-07 review note: this child's HEIGHT is `thumbSize`
                // (116-140px at this breakpoint), coupled to the thumb, not
                // to `drawMeta`'s own content -- at today's metrics (Inter
                // 16px body, this row's four lines) the real content stands
                // ~80px, comfortably inside even the smallest compact
                // thumbSize, so this is a no-op in practice. A future
                // larger body font or display scale could grow that content
                // past `thumbSize` and start clipping/scrolling the `cook`
                // row inside the box -- not exercised by any capture in
                // this arc, flagged here rather than sized defensively
                // against a metrics change nothing today asks for.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                if (ImGui::BeginChild("##previewMeta", ImVec2(textColumnWidth, thumbSize), ImGuiChildFlags_None))
                    drawMeta();
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
            else
            {
                // 2026-09-07 follow-up: the pane can now be dragged down to
                // kPreviewPaneMinWidth (120px), which minus this child's own
                // WindowPadding does not clear 140px. Rather than add a
                // second centering codepath, the thumb SCALES to whatever is
                // actually available (min-clamped against the 140px pinned
                // size). At the shipped 165px default (avail ~= 149px after
                // padding) this is a no-op: 140 < avail, so `thumbSize` is
                // still exactly 140 and nothing about the Task 11 layout
                // changes.
                const float thumbSize = std::min(kPreviewThumbSize, ImGui::GetContentRegionAvail().x);
                drawThumb(thumbSize);
                drawMeta();
            }

            ImGui::Separator();

            // ---- Derived (N) list
            char derivedHeader[32];
            std::snprintf(derivedHeader, sizeof(derivedHeader), "Derived (%d)",
                          static_cast<int>(e->derivedChildren.size()));
            ImGui::TextUnformatted(derivedHeader);
            for (const Arcane::Guid& childGuid : e->derivedChildren)
                DrawDerivedRow(model, services, childGuid);

            ImGui::Separator();

            // ---- action buttons: full-width, 24px tall. Open reuses the
            // SAME routing helper double-click/Enter use (spec: "Open (same
            // routing as double-click)"); the trailing kind-specific action
            // mirrors DrawRowContextMenu's own kind-specific entries exactly
            // (same label text, same action field).
            const ImVec2 btnSize(-FLT_MIN, kActionButtonHeight);
            if (ImGui::Button(ICON_LC_EXTERNAL_LINK " Open", btnSize))
                OpenAssetRow(*e, project, docs, actions);
            if (ImGui::Button(ICON_LC_FOLDER_OPEN " Show in Explorer", btnSize))
                actions.showInExplorer = e->guid;
            if (ImGui::Button(ICON_LC_COPY " Copy Path", btnSize))
                actions.copyPath = e->guid;

            if (e->kind == AssetKind::Material)
            {
                if (ImGui::Button(ICON_LC_LAYERS " New Instance...", btnSize))
                    actions.createInstanceOf = e->guid;
            }
            else if (e->kind == AssetKind::Scene)
            {
                if (ImGui::Button(ICON_LC_FLAG " Set as Boot Scene", btnSize))
                    actions.setBootScene = e->guid;
            }
            else if (e->kind == AssetKind::Texture)
            {
                if (ImGui::Button(ICON_LC_STICKER " Create Sprite", btnSize))
                    actions.createSpriteFrom = e->guid;
            }

            ImGui::EndChild();
        }

        // ---- Task 10/11: the Browse lens body (rail + table + preview) -----
        void DrawBrowseLens(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                           DocumentHost& docs, const AssetsPanelServices& services, AssetsPanelActions& actions)
        {
            // The project's recorded boot scene, resolved once per draw
            // (rather than per row) for the "boot" pill (spec s6).
            const Arcane::Guid bootGuid = BootSceneGuid(project);

            // Spec s5: preview pane hidden below a 720px PANEL width so the
            // table never drops below readable width. Measured here, before
            // anything in this body has drawn -- at this exact point
            // ImGui's content-region-avail IS the whole rail+table+preview
            // budget for the frame, uncontested by anything this function
            // itself has submitted yet.
            const float panelWidth = ImGui::GetContentRegionAvail().x;
            const bool showPreview = panelWidth >= kPreviewHidePanelWidth;

            // 2026-09-07 review fix: `drawnWidth` is a purely LOCAL, per-frame
            // clamp of `state.previewPaneWidth` (the stored DESIRED width) --
            // it is what the layout below actually draws against, and it is
            // thrown away at the end of this function. The first cut of this
            // feature instead reassigned `state.previewPaneWidth` here
            // directly, which meant a transient panel-narrowing (a plain
            // window resize, no splitter interaction at all) silently and
            // PERMANENTLY reduced whatever the user had actually dragged to,
            // with no way back once the panel widened again -- a ratchet,
            // not a clamp. Keeping the two separate means a WINDOW resize
            // still reclamps the DRAWN width every frame (so the table never
            // gets crushed), while the DESIRED width survives the narrow
            // interval untouched and reasserts itself the moment there is
            // room again.
            const float drawnWidth = showPreview
                ? ClampPreviewForLayout(state.previewPaneWidth, panelWidth)
                : 0.0f;

            // 2026-09-07 (user nitpick, mock parity): rail|table and
            // table|splitter|pane sit FLUSH -- SameLine(0.0f, 0.0f) zeroes
            // the ItemSpacing.x gutter SameLine() would otherwise insert.
            // OptionBC.dc.html has no gap here either: the rail's own
            // `border-right: 1px solid #333333` (DrawRail's new hairline,
            // below) and the table's own `border-right` (the splitter's
            // existing at-rest paint, already a 1px hairline centered in its
            // hit strip -- PreviewPaneSplitter, untouched) are the ONLY
            // separators, not an 8px void on each side of them. The width
            // budget below is updated in lockstep -- see ClampPreviewForLayout's
            // own 2026-09-07 comment for the identity this must hold.
            DrawRail(state, model, actions);
            ImGui::SameLine(0.0f, 0.0f);

            // Reserve the (resizable) preview pane plus the splitter bar,
            // with NO ItemSpacing gutters any more (see above); 0.0f keeps
            // DrawTable's own "fill everything left on this line" default
            // when the pane is hidden.
            const float tableWidth = showPreview
                ? std::max(0.0f, panelWidth - kRailWidth - kPreviewSplitBarPx - drawnWidth)
                : 0.0f;
            DrawTable(state, model, project, docs, services, actions, bootGuid, tableWidth);

            if (showPreview)
            {
                ImGui::SameLine(0.0f, 0.0f);
                PreviewPaneSplitter(state.previewPaneWidth);
                ImGui::SameLine(0.0f, 0.0f);
                DrawPreviewPane(model, project, docs, services, actions, drawnWidth);
            }
        }

        // ---- Plan 2 Task 7: AssetPill's own width, WITHOUT drawing it ------
        // 12px text plus the two FramePadding.x cheeks (EditorWidgets.cpp's
        // AssetPill, verbatim). The attention card positions its trailing
        // pill by hand and has to ellipsize the NAME against whatever room is
        // left after it, so it needs the pill's width one item early.
        float PillWidth(const char* text)
        {
            ImGui::PushFont(GetEditorFonts().interRegular, 12.0f);
            const float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::PopFont();
            return w;
        }

        // ---- Plan 2 Task 7: one needs-attention card (spec s9.2) -----------
        // Two shapes, ONE function, because everything except the trailing
        // content, the frame variant and the second line is identical:
        //
        //   * REFUSED wears the muted-amber acting-on frame (BeginCardFrame
        //     variant 1 -- the same `#7a5a20` spec s11.2 pins for the amber
        //     pill), the amber warning glyph, its refusal detail line, and
        //     the Recook/Problems pair.
        //   * QUEUED is a neutral frame with a dim clock, the dim "arccook
        //     running..." note the board puts on the NAME ROW beside the
        //     name (spec s11: "the mocks are the redline"), and the derived
        //     progress strip beneath.
        //
        // `queuedProgress` is Ruling 12's DERIVED fraction --
        // cooked / (cooked + queued) from HealthCounts, the very numbers the
        // meter above already shows. There is no per-asset cook progress to
        // read anywhere in the engine, and this card refuses to fabricate one.
        //
        // `queuedCooked`/`queuedCookedAndQueued` (Task 7 review ruling B):
        // the SAME two HealthCounts numbers `queuedProgress` was already
        // derived from, passed through a second time so the queued card can
        // spell the fraction out in words ("N of M cooked") instead of
        // leaving the bare strip to speak for itself. Meaningless when
        // `refused` -- callers pass 0/0 for the refused card.
        void DrawAttentionCard(AssetPanelModel& model, const AssetsPanelServices& services,
                               AssetsPanelActions& actions, const AssetPanelEntry& e,
                               bool refused, float queuedProgress,
                               int queuedCooked, int queuedCookedAndQueued)
        {
            const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
            const float  cardWidth = ImGui::GetContentRegionAvail().x;
            // The guid string is the card's id scope, same convention every
            // row in this file uses (DrawAssetRow/DrawChildRow's PushID).
            const std::string cardId = e.guid.ToString();
            if (!BeginCardFrame(cardId.c_str(), refused ? 1 : 0, cardWidth))
                return;   // SkipItems: nothing was pushed, so nothing to End

            const ImGuiStyle& style = ImGui::GetStyle();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // The card's inner padding, DERIVED rather than duplicated:
            // BeginCardFrame seats the cursor exactly one padding in from the
            // frame's top-left corner, so this difference IS
            // EditorWidgets.cpp's kCardFramePadding without a second copy of
            // that constant living here to drift from it.
            const ImVec2 innerMin = ImGui::GetCursorScreenPos();
            const float  pad      = innerMin.x - cardMin.x;
            const float  innerW   = std::max(1.0f, cardWidth - pad * 2.0f);

            // Line 1 stands as tall as the buttons it hosts (the refused
            // card); the queued card keeps the same pitch so the two card
            // shapes line up in a mixed list.
            const float rowH  = ImGui::GetFrameHeight();
            const float line2 = refused ? ImGui::GetTextLineHeight() : kStatusProgressHeight;
            // Task 7 review ruling B: the queued card grows a THIRD line --
            // the "N of M cooked" caption beneath the progress strip --
            // measured at StatTile's own 13px label size. A brief
            // PushFont/PopFont pair purely to read GetTextLineHeight(); the
            // refused card never carries this line, so it costs it nothing.
            float line3 = 0.0f;
            if (!refused)
            {
                ImGui::PushFont(GetEditorFonts().interRegular, 13.0f);
                line3 = ImGui::GetTextLineHeight();
                ImGui::PopFont();
            }
            const float bodyH = rowH + style.ItemSpacing.y + line2
                              + (refused ? 0.0f : (kStatusProgressCaptionGap + line3));

            // ONE body hit target, submitted FIRST and covering the whole card
            // body, with SetNextItemAllowOverlap so the two buttons submitted
            // AFTER it still take the hover and the click where they overlap
            // -- imgui.h's own documented use of that flag ("covering an area
            // where subsequent items may need to be added"), and the shape
            // RowWithThumb + the rail's hover "+" already run on. Everything
            // else this function draws is either pure drawlist paint or a
            // non-interactive Dummy (AssetPill), so the rest of the card body
            // stays clickable.
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##cardbody", ImVec2(innerW, bodyH)))
                model.Select(e.guid);
            // Immediately after the hit item, exactly like AttachRowInteractions
            // does for a table row -- the peek tooltip keys off the LAST
            // submitted item.
            DrawAssetPeekTooltip(model, services, e.guid);

            // ---- buttons: right-aligned on line 1, drawn BEFORE the name so
            // the name's ellipsis budget can be measured against where they
            // actually start.
            float buttonsLeft = innerMin.x + innerW;
            if (refused)
            {
                const float recookW   = ImGui::CalcTextSize("Recook").x + style.FramePadding.x * 2.0f;
                const float problemsW = ImGui::CalcTextSize("Problems").x + style.FramePadding.x * 2.0f;
                buttonsLeft = innerMin.x + innerW - recookW - problemsW - style.ItemSpacing.x;
                ImGui::SetCursorScreenPos(ImVec2(buttonsLeft, innerMin.y));
                // "Panel reports, app performs": neither button does any work
                // here -- EditorApp::ConsumeBrowserActions owns both effects.
                if (ImGui::Button("Recook"))
                    actions.recook = e.guid;
                ImGui::SameLine();
                if (ImGui::Button("Problems"))
                    actions.showProblems = true;
            }

            // ---- line 1: 18px thumb, state glyph, name, trailing content.
            // Drawlist paint (plus one positioned AssetPill), the same
            // technique RowWithThumb uses, so none of it competes with the hit
            // target above for ImGui's "last item".
            float x = innerMin.x;
            const float thumbY = innerMin.y + (rowH - kAssetRowThumbSize) * 0.5f;
            const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            if (thumb != 0)
            {
                dl->AddImage(static_cast<ImTextureID>(thumb), ImVec2(x, thumbY),
                            ImVec2(x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize));
            }
            else
            {
                // The same well-plus-centred-kind-icon fallback the preview
                // pane's own thumb uses, at the row's 18px size.
                dl->AddRectFilled(ImVec2(x, thumbY),
                                  ImVec2(x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize),
                                  ImGui::GetColorU32(Theme::kWell));
                const char* kindIcon = KindIcon(e.kind);
                const ImVec2 ks = ImGui::CalcTextSize(kindIcon);
                dl->AddText(ImVec2(x + (kAssetRowThumbSize - ks.x) * 0.5f,
                                   thumbY + (kAssetRowThumbSize - ks.y) * 0.5f),
                           ImGui::GetColorU32(ImGuiCol_Text), kindIcon);
            }
            x += kAssetRowThumbSize + style.ItemInnerSpacing.x;

            // State glyph: amber triangle for refused, dim clock for queued.
            // Amber never carries the meaning ALONE -- the glyph shape, the
            // detail line and the card's own frame all say the same thing
            // (spec s11.2's amber rule).
            const char* stateIcon = refused ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CLOCK;
            const ImVec2 stateSize = ImGui::CalcTextSize(stateIcon);
            dl->AddText(ImVec2(x, innerMin.y + (rowH - stateSize.y) * 0.5f),
                       ImGui::GetColorU32(refused ? Theme::kAmber : Theme::kTextDim), stateIcon);
            x += stateSize.x + style.ItemInnerSpacing.x;

            // What follows the name on this line, measured BEFORE it so the
            // name can be ellipsized against what is genuinely left.
            constexpr const char* kRunningText = "arccook running...";
            const char* kindText   = KindLabel(e.kind);
            const float trailingW  = refused ? PillWidth(kindText)
                                             : ImGui::CalcTextSize(kRunningText).x;
            // buttonsLeft is only the LEFT EDGE OF THE BUTTONS on the refused
            // shape (Recook/Problems); the queued shape has no buttons, so
            // buttonsLeft there is already the card's plain right edge and
            // needs no extra gap subtracted before it. Applying the
            // button-row's ItemSpacing unconditionally would over-ellipsize
            // the queued name by that many px for a gap that doesn't exist.
            const float nameBudget = std::max(0.0f, buttonsLeft - (refused ? style.ItemSpacing.x : 0.0f)
                                                     - trailingW - style.ItemInnerSpacing.x - x);

            const std::string name = EllipsisToWidth(e.fileName, nameBudget);
            const ImVec2 nameSize  = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2(x, innerMin.y + (rowH - nameSize.y) * 0.5f),
                       ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            x += nameSize.x + style.ItemInnerSpacing.x;

            if (refused)
            {
                ImGui::SetCursorScreenPos(ImVec2(x, innerMin.y + (rowH - kPillLineHeight) * 0.5f));
                AssetPill(kindText);
            }
            else
            {
                dl->AddText(ImVec2(x, innerMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                           ImGui::GetColorU32(Theme::kTextDim), kRunningText);
            }

            // ---- line 2
            const float line2Y = innerMin.y + rowH + style.ItemSpacing.y;
            if (refused)
            {
                // The refusal reason, from the HOST's own cook-diagnostic row
                // (services.cookDetailFor) -- this panel never reads
                // diagnostics itself. Composed as the board writes it,
                // "cook refused - <detail>"; the bare "cook refused" is the
                // fallback when the host has no permanent row for this guid
                // (possible: CookStateOf can also reach Refused through a
                // provider answer this session's map no longer backs).
                std::string line = "cook refused";
                if (services.cookDetailFor)
                {
                    if (const std::optional<std::string> detail = services.cookDetailFor(e.guid);
                        detail && !detail->empty())
                    {
                        line += " \xE2\x80\x94 ";   // em dash, the board's own separator
                        line += *detail;
                    }
                }
                // Task 7 review ruling C: the board's own tail, pointing at
                // the Problems pane for the full diagnostic -- appended
                // UNCONDITIONALLY, whether or not a per-guid detail resolved
                // above. Already in TextDisabled tone: the whole line below
                // draws in Theme::kTextDim, tail included, so no separate
                // color segment is needed for "in TextDisabled tone".
                //
                // Review fix (Important 2): the tail must be MEASURED
                // before the clamp, not appended before it -- a long
                // refusal detail is exactly the case EllipsisToWidth's own
                // "..." would otherwise cut the tail from first (it sits at
                // the string's end), silently dropping the ONE thing this
                // line exists to point the user at. Same "measure trailing
                // content, then budget the rest" order PillWidth's callers
                // already use elsewhere in this file.
                constexpr const char* kProblemsTail = " \xC2\xB7 details in Problems";
                const float tailWidth = ImGui::CalcTextSize(kProblemsTail).x;
                const std::string shown = EllipsisToWidth(line, std::max(0.0f, innerW - tailWidth))
                                        + kProblemsTail;
                dl->AddText(ImVec2(innerMin.x, line2Y), ImGui::GetColorU32(Theme::kTextDim),
                           shown.c_str());
            }
            else
            {
                // Ruling 12's derived strip: kGrab fill over a kWell track.
                const float frac = std::clamp(queuedProgress, 0.0f, 1.0f);
                dl->AddRectFilled(ImVec2(innerMin.x, line2Y),
                                  ImVec2(innerMin.x + innerW, line2Y + kStatusProgressHeight),
                                  ImGui::GetColorU32(Theme::kWell));
                if (frac > 0.0f)
                    dl->AddRectFilled(ImVec2(innerMin.x, line2Y),
                                      ImVec2(innerMin.x + innerW * frac, line2Y + kStatusProgressHeight),
                                      ImGui::GetColorU32(Theme::kGrab));

                // Task 7 review ruling B: the strip alone never said WHAT
                // fraction it was a fraction OF -- this caption makes the
                // pipeline semantics explicit.
                char caption[32];
                std::snprintf(caption, sizeof(caption), "%d of %d cooked",
                             queuedCooked, queuedCookedAndQueued);
                ImGui::PushFont(GetEditorFonts().interRegular, 13.0f);
                dl->AddText(ImVec2(innerMin.x, line2Y + kStatusProgressHeight + kStatusProgressCaptionGap),
                           ImGui::GetColorU32(Theme::kTextDim), caption);
                ImGui::PopFont();
            }

            EndCardFrame();

            // Selection: a 2px kSelection border over the frame EndCardFrame
            // just painted (spec s8's interaction contract -- the Status lens
            // highlights its CARDS, the way the table highlights its rows).
            // Drawn after the fact against the full-card rect EndCardFrame
            // reserves as its closing item, so it needs no separate measure.
            if (model.selected == e.guid)
            {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                    ImGui::GetColorU32(Theme::kSelection),
                                                    0.0f, 0, kStatusSelectionBorder);
            }
        }

        // ---- Plan 2 Task 8: activity-feed age/title formatting --------------
        // File-local, TimelineFeed's sole caller: the widget itself stays
        // model-free (EditorWidgets.hpp's own doc comment), so every bit of
        // "what does this entry MEAN" policy lives here instead.
        std::string FormatActivityAge(std::chrono::steady_clock::time_point when)
        {
            const auto elapsed = std::chrono::steady_clock::now() - when;
            const long long secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            if (secs < 60)
                return "just now";
            char buf[32];
            const long long mins = secs / 60;
            if (mins < 60)
            {
                std::snprintf(buf, sizeof(buf), "%lld min ago", mins);
                return buf;
            }
            std::snprintf(buf, sizeof(buf), "%lld h ago", mins / 60);
            return buf;
        }

        // Title per AssetActivityKind (plan doc Step 2). SourceChanged is the
        // one kind whose title depends on live model state rather than the
        // entry alone: "-> queued" only when the guid STILL resolves AND its
        // kind still cooks (Texture/Sprite, the exact CookStateOf rule) --
        // an entry whose asset has since vanished, or that never had a real
        // cook pipeline, reads as the plain form.
        std::string ActivityTitle(const AssetPanelModel& model, const AssetActivityEntry& entry)
        {
            switch (entry.kind)
            {
                case AssetActivityKind::Cooked:      return "cooked";
                case AssetActivityKind::CookRefused: return "cook refused";
                case AssetActivityKind::Created:     return "created";
                case AssetActivityKind::Deleted:     return "deleted";
                case AssetActivityKind::SourceChanged:
                {
                    const AssetPanelEntry* e = model.Find(entry.guid);
                    const bool cooks = e && (e->kind == AssetKind::Texture || e->kind == AssetKind::Sprite);
                    return cooks ? "source changed \xE2\x86\x92 queued" : "source changed";
                }
            }
            return "";
        }

        // ---- Plan 2 Task 8: the Unreferenced card (spec s9.2, step 1) ------
        // One CardFrame holding an inset Theme::kWell well of rows -- 18px
        // thumb + a chip-style fileName (AssetPill, the same chip idiom every
        // other row in this file uses for a name label) + a small Reveal
        // button -- one row per model.UnusedGuids(), that ordering already
        // pinned (Task 4) so this card never needs its own sort. ItemSpacing.y
        // is zeroed for the row loop so the drawn well height (rowH * count,
        // computed up front so the fill can be painted BEHIND the rows)
        // matches the rows' own actual pitch exactly -- the same "vertical-
        // only, don't trust automatic per-item spacing" fix DrawAssetsPanel's
        // own toolbar-gap comment applies elsewhere in this file.
        void DrawUnreferencedCard(AssetsPanelState& state, AssetPanelModel& model,
                                  const AssetsPanelServices& services)
        {
            const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
            const float  cardWidth = ImGui::GetContentRegionAvail().x;
            if (!BeginCardFrame("##unreferenced", 0, cardWidth))
                return;

            const std::vector<Arcane::Guid> unused = model.UnusedGuids();
            if (unused.empty())
            {
                ImGui::TextDisabled("everything is referenced");
            }
            else
            {
                const ImGuiStyle& style = ImGui::GetStyle();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 wellMin = ImGui::GetCursorScreenPos();
                // Review fix (Important 1): GetContentRegionAvail() at the
                // INNER cursor measures to the ambient window's right edge,
                // not the card's own right border -- BeginCardFrame doesn't
                // constrain caller content width (its own doc comment), so
                // that avail is `cardWidth - pad`, one pad short of what a
                // caller actually wants. `pad`, DERIVED the same way
                // DrawAttentionCard's own `innerW` is (cardMin vs the seated
                // cursor), then subtracted TWICE -- once for each side --
                // is what actually stops the well/Reveal button at the
                // card's inner content edge instead of its outer border.
                const float  pad       = wellMin.x - cardMin.x;
                const float  wellWidth = std::max(1.0f, cardWidth - pad * 2.0f);
                const float  rowH      = kTableRowHeight;
                dl->AddRectFilled(wellMin,
                                  ImVec2(wellMin.x + wellWidth, wellMin.y + rowH * static_cast<float>(unused.size())),
                                  ImGui::GetColorU32(Theme::kWell));

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
                for (const Arcane::Guid& guid : unused)
                {
                    const AssetPanelEntry* e = model.Find(guid);
                    if (!e)
                        continue;   // pruned between UnusedGuids() and now -- skip, don't fabricate a row

                    ImGui::PushID(guid.ToString().c_str());
                    const ImVec2 rowMin = ImGui::GetCursorScreenPos();

                    // Thumb: same 18px well-plus-kind-icon fallback
                    // DrawAttentionCard's own line 1 uses.
                    const float thumbY = rowMin.y + (rowH - kAssetRowThumbSize) * 0.5f;
                    const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(guid) : 0;
                    if (thumb != 0)
                    {
                        dl->AddImage(static_cast<ImTextureID>(thumb), ImVec2(rowMin.x, thumbY),
                                    ImVec2(rowMin.x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize));
                    }
                    else
                    {
                        dl->AddRectFilled(ImVec2(rowMin.x, thumbY),
                                          ImVec2(rowMin.x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize),
                                          ImGui::GetColorU32(Theme::kWell));
                        const char* kindIcon = KindIcon(e->kind);
                        const ImVec2 ks = ImGui::CalcTextSize(kindIcon);
                        dl->AddText(ImVec2(rowMin.x + (kAssetRowThumbSize - ks.x) * 0.5f,
                                           thumbY + (kAssetRowThumbSize - ks.y) * 0.5f),
                                   ImGui::GetColorU32(ImGuiCol_Text), kindIcon);
                    }

                    // Reveal, right-aligned within the well.
                    const float revealW = ImGui::CalcTextSize("Reveal").x + style.FramePadding.x * 2.0f;
                    const float revealX = wellMin.x + wellWidth - revealW;

                    // Name, chip-style -- AssetPill, vertically centered the
                    // same way DrawAttentionCard positions its own trailing
                    // pill (rowH - kPillLineHeight, halved).
                    ImGui::SetCursorScreenPos(ImVec2(rowMin.x + kAssetRowThumbSize + style.ItemInnerSpacing.x,
                                                     rowMin.y + (rowH - kPillLineHeight) * 0.5f));
                    AssetPill(e->fileName.c_str());

                    ImGui::SetCursorScreenPos(ImVec2(revealX, rowMin.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
                    if (ImGui::Button("Reveal"))
                    {
                        // Ruling 10 (plan doc): clear every filter, switch to
                        // Browse, select the guid -- DrawTable's own scroll-
                        // to-selection machinery (:1100-1114-ish, keyed off
                        // state.seenSelectionStamp vs model.selectionStamp)
                        // does the rest once Browse redraws next frame.
                        model.SetSearch("");
                        state.search[0] = '\0';
                        model.SetKindFilter(-1);
                        state.railKind = -1;   // -1 = All, the rail's own spelling

                        // Controller ruling (Task 8 review, Ruling 10's gap):
                        // clearing filters alone does not guarantee the row
                        // is VISIBLE -- a collapsed ancestor group (or a
                        // collapsed mount root, e.g. diag://'s own default-
                        // closed state, GroupDefaultOpen) still hides it.
                        // Walk `e->folder` up through every ancestor
                        // (GroupParentOf -- the same chain DrawGroupRow's own
                        // nesting walks, terminating at "" for a mount's own
                        // root) and force each one open, through BOTH
                        // writers DrawGroupRow's own toggle uses: state's
                        // mirror (GroupIsOpen's source of truth for the
                        // chevron glyph) and the model (SetGroupOpen -- the
                        // actual Rows() rebuild trigger). The mount root
                        // itself is included: it is simply the LAST non-empty
                        // value this loop visits before GroupParentOf finally
                        // returns "".
                        for (std::string folder = e->folder; !folder.empty(); folder = GroupParentOf(folder))
                        {
                            state.groupOpen[folder] = true;
                            model.SetGroupOpen(folder, true);
                        }

                        // Addendum (coordinator ruling, extending Ruling 10):
                        // reachable -- a folded 1:1 derived sprite with zero
                        // inbound IS unused-eligible (kind Sprite), so it can
                        // be a `Reveal` target while still living under its
                        // texture's own CLOSED fold. The group chain above
                        // opens every ANCESTOR GROUP but says nothing about
                        // fold state, which is a separate flag keyed by the
                        // PARENT TEXTURE's guid (`foldedUnder`), not by
                        // folder -- so it needs its own write, same two-map
                        // spelling the fold chevron's own toggle uses
                        // (DrawAssetRow's expander handler: state.childrenOpen
                        // + model.SetChildrenOpen, both keyed by the PARENT's
                        // guid). Consistent with the tree arc's uniform-
                        // reveal precedent, where search already overrides
                        // both group and fold collapse -- Reveal now forces
                        // the same two collapse dimensions open explicitly.
                        if (e->foldedUnder.IsValid())
                        {
                            state.childrenOpen[e->foldedUnder] = true;
                            model.SetChildrenOpen(e->foldedUnder, true);
                        }

                        state.lens = AssetLens::Browse;
                        model.Select(guid);
                    }

                    // Reserve the FULL row as one item -- EndCardFrame's own
                    // EndGroup measures the union of real items, so every row
                    // needs at least one spanning the whole (wellWidth, rowH)
                    // rect, not just the Reveal button's own small one.
                    ImGui::SetCursorScreenPos(rowMin);
                    ImGui::Dummy(ImVec2(wellWidth, rowH));

                    ImGui::PopID();
                }
                ImGui::PopStyleVar();

                ImGui::TextDisabled("nothing points at these");
            }

            EndCardFrame();
        }

        // ---- Plan 2 Task 8: one Scenes-rollup card (spec s9.2, step 3) -----
        // name (+ boot pill, the SAME source DrawAssetRow's own pill uses) ·
        // a count line derived from the reference index · a "Focus in Graph"
        // button, LIVE as of Plan 3 Task 5 (it was a BeginDisabled
        // placeholder for exactly as long as the Graph lens itself was
        // unreachable -- never a stub that pretended to do something).
        //
        // `state` for that button alone: it is the only thing on this card
        // that writes panel state, and it writes it DIRECTLY rather than
        // through AssetsPanelActions -- the panel/app split those actions
        // exist for is about effects the HOST must perform (file IO,
        // dialogs, scene loads), and switching which lens this same panel
        // draws is not one. The precedent is the digest chip's own
        // click-through in DrawBottomBar.
        void DrawSceneCard(AssetsPanelState& state, AssetPanelModel& model,
                           const AssetPanelEntry& e, const Arcane::Guid& bootGuid)
        {
            if (!BeginCardFrame(e.guid.ToString().c_str(), 0, ImGui::GetContentRegionAvail().x))
                return;

            ImGui::Text("%s %s", KindIcon(e.kind), e.fileName.c_str());
            if (bootGuid.IsValid() && e.guid == bootGuid)
            {
                ImGui::SameLine();
                AssetPill("boot", 1);
            }

            // n = distinct outbound targets (spec s9.1's dual-edge
            // accounting -- References AND DerivesFrom both count towards a
            // scene's own dependency count). The index's manifest already
            // dedups; this counts distinct DEFENSIVELY rather than trust
            // that invariant a second time from a display-only consumer.
            std::vector<Arcane::Guid> targets;
            if (const AssetReferenceIndex::Node* node = model.RefIndex().Find(e.guid))
            {
                targets.reserve(node->outbound.size());
                for (const Arcane::AssetRef& ref : node->outbound)
                    targets.push_back(ref.target);
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            }

            int needsAttention = 0;
            for (const Arcane::Guid& target : targets)
                if (const AssetPanelEntry* t = model.Find(target);
                    t && (t->cook == CookState::Refused || t->cook == CookState::Queued))
                    ++needsAttention;

            char line[64];
            if (needsAttention > 0)
                std::snprintf(line, sizeof(line), "%d assets \xC2\xB7 %d need attention",
                             static_cast<int>(targets.size()), needsAttention);
            else
                std::snprintf(line, sizeof(line), "%d assets \xC2\xB7 all cooked",
                             static_cast<int>(targets.size()));
            ImGui::TextDisabled("%s", line);

            if (ImGui::Button("Focus in Graph"))
            {
                // Focus BEFORE the lens, deliberately: the very next frame is
                // the Graph lens's first, and its projection is built from
                // whatever `graphFocus` holds when that frame runs. Setting
                // the lens first would still land the same focus in the same
                // frame (both writes happen here, before any draw), but the
                // ordering states the dependency the way it actually reads --
                // scope, then show -- so a later edit that moves either line
                // cannot quietly build one unscoped frame first.
                state.graphFocus = e.guid;
                state.lens       = AssetLens::Graph;
                // ...and select it, which is what makes the graph CENTER on
                // this scene rather than merely contain it: the lens's
                // selection bridge (Task 4) centers the canvas on an EXTERNAL
                // selection change, and this is one.
                model.Select(e.guid);
            }

            EndCardFrame();
        }

        // ---- Plan 2 Task 7/8: the Status lens body (spec s9.2) -------------
        // Tiles row, cook-pipeline meter, then a two-column split matching
        // the board's own layout (`renders/OptionE-Status-FINAL.png`):
        // "Needs attention"/"Unreferenced" left, "Activity"/"Scenes" right,
        // both starting at the same Y. A table rather than hand-rolled
        // column math -- ImGui's own per-cell auto-height handles two
        // UNEQUAL-height columns without this file inventing a second
        // version of that logic; NoSavedSettings for the same reason every
        // other table in this file carries it (session-only layout, nothing
        // to persist to imgui.ini).
        void DrawStatusLens(AssetsPanelState& state, AssetPanelModel& model,
                            const Arcane::Project* project, DocumentHost& /*docs*/,
                            const AssetsPanelServices& services,
                            AssetsPanelActions& actions)
        {
            // AlwaysUseWindowPadding: a bordered-less child gets NO padding by
            // default, and the dashboard -- unlike the Browse lens's flush
            // rail/table/pane chain -- is a padded page (the board insets its
            // whole content from the panel edge).
            if (!ImGui::BeginChild("##statusbody", ImVec2(0.0f, 0.0f),
                                   ImGuiChildFlags_AlwaysUseWindowPadding))
            {
                ImGui::EndChild();
                return;
            }

            const HealthCounts health = model.Health();
            const ImGuiStyle& style = ImGui::GetStyle();

            // ---- tiles row: four equal-width tiles carved out of the content
            // region (spec s9.2's "assets / cook refused / awaiting cook /
            // unreferenced"). Only the refused tile is amber, and only its
            // ICON is -- StatTile's variant 1 keeps the number in text tokens
            // (spec s11.2).
            {
                const float tileW = std::max(kStatusTileMinWidth,
                    (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 3.0f) * 0.25f);
                const ImVec2 tileSize(tileW, kStatusTileHeight);
                char num[16];
                // ImDrawList::AddText rasterizes at the call, so one scratch
                // buffer serves all four tiles.
                const auto tile = [&](const char* id, int value, const char* label,
                                      const char* icon, int variant)
                {
                    std::snprintf(num, sizeof(num), "%d", value);
                    StatTile(id, num, label, icon, variant, tileSize);
                };
                tile("##tileassets",  health.total,   "assets",        nullptr,                 0);
                ImGui::SameLine();
                tile("##tilerefused", health.refused, "cook refused",  ICON_LC_TRIANGLE_ALERT,  1);
                ImGui::SameLine();
                tile("##tilequeued",  health.queued,  "awaiting cook", ICON_LC_CLOCK,           0);
                ImGui::SameLine();
                tile("##tileunused",  health.unused,  "unreferenced",  ICON_LC_CIRCLE_SLASH,    0);
            }

            // ---- cook pipeline meter. Grays plus amber, and the icon/label
            // pair carries the meaning in every case -- colour alone never
            // does (MeterBar draws a swatch AND the label AND the count).
            ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
            ImGui::TextDisabled("Cook pipeline");
            const MeterSegment segments[] = {
                { "cooked",  health.cooked,  ImGui::GetColorU32(Theme::kGrab)    },
                { "queued",  health.queued,  ImGui::GetColorU32(Theme::kTextDim) },
                { "refused", health.refused, ImGui::GetColorU32(Theme::kAmber)   },
            };
            MeterBar("##cookmeter", segments, static_cast<int>(std::size(segments)),
                     ImGui::GetContentRegionAvail().x);

            // ---- two-column body: LEFT (Needs attention -> Unreferenced),
            // RIGHT (Activity -> Scenes) -- the board's own side-by-side
            // placement (OptionE-Status-FINAL.png: "Needs attention" and
            // "Activity" sit at the same Y).
            ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
            // Review fix (Important 3): kStatusRightColumnWidth is an
            // implementer tuning value, not a floor -- unclamped, a narrow
            // dock could let the fixed column crush (or exceed) the whole
            // available width, starving the stretch column and leaving
            // TimelineFeed's per-row hit target with a zero/negative avail
            // (ImGui::InvisibleButton asserts on exactly zero). Same
            // "sane-range clamp" discipline ClampPreviewForLayout already
            // uses for the preview pane -- capped to a fraction of what is
            // actually available THIS frame, floored so it is never <= 0.
            const float rightColumnWidth = std::max(1.0f,
                std::min(kStatusRightColumnWidth, ImGui::GetContentRegionAvail().x * 0.45f));
            if (ImGui::BeginTable("##statuscolumns", 2, ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableSetupColumn("##left",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed, rightColumnWidth);
                ImGui::TableNextRow();

                // ---- LEFT: needs attention, then Unreferenced.
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Needs attention");

                // model.Entries() is an unordered_map -- its own doc comment
                // requires a displaying consumer to sort. Name, then mount
                // path as the tie-break: the exact ordering
                // AssetPanelModel::UnusedGuids already pins for the
                // Unreferenced card, so the lists cannot read as sorted by
                // different rules.
                std::vector<const AssetPanelEntry*> refused, queued;
                for (const auto& [guid, entry] : model.Entries())
                {
                    if (entry.cook == CookState::Refused)     refused.push_back(&entry);
                    else if (entry.cook == CookState::Queued) queued.push_back(&entry);
                }
                const auto byName = [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; };
                std::sort(refused.begin(), refused.end(), byName);
                std::sort(queued.begin(), queued.end(), byName);

                if (refused.empty() && queued.empty())
                {
                    // Empty state (a desk item -- the board only draws the
                    // populated form, so this one line is the whole design).
                    ImGui::TextDisabled("nothing needs attention");
                }
                else
                {
                    // Ruling 12: ONE fraction for every queued card, derived
                    // from the same HealthCounts the meter shows. Zero-safe
                    // -- a non-empty `queued` list implies health.queued > 0,
                    // but the guard costs nothing and does not depend on
                    // that reasoning.
                    const int cookedAndQueued = health.cooked + health.queued;
                    const float queuedProgress = cookedAndQueued > 0
                        ? static_cast<float>(health.cooked) / static_cast<float>(cookedAndQueued)
                        : 0.0f;

                    for (const AssetPanelEntry* e : refused)
                        DrawAttentionCard(model, services, actions, *e, /*refused=*/true, 0.0f, 0, 0);
                    for (const AssetPanelEntry* e : queued)
                        DrawAttentionCard(model, services, actions, *e, /*refused=*/false, queuedProgress,
                                          health.cooked, cookedAndQueued);
                }

                ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
                ImGui::TextDisabled("Unreferenced");
                DrawUnreferencedCard(state, model, services);

                // ---- RIGHT: Activity, then Scenes.
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Activity");
                if (!services.activity || services.activity->Size() == 0)
                {
                    ImGui::TextDisabled("no activity yet");
                }
                else
                {
                    // Frame-lifetime string storage (plan doc Step 2): build
                    // every row's std::strings into a vector reserved to the
                    // EXACT final count first (Size(), never re-grown after),
                    // so the vector never reallocates once we start taking
                    // .c_str() pointer views into it below -- the SSO trap
                    // this file's brief calls out by name.
                    struct ActivityRow { std::string age, title, detail; Arcane::Guid guid; };
                    std::vector<ActivityRow> rows;
                    rows.reserve(services.activity->Size());
                    services.activity->ForEachNewestFirst([&](const AssetActivityEntry& entry)
                    {
                        ActivityRow row;
                        row.age   = FormatActivityAge(entry.when);
                        row.title = ActivityTitle(model, entry);
                        const std::string base = entry.name.empty() ? entry.guid.ToString() : entry.name;
                        row.detail = entry.detail.empty() ? base : (base + " \xE2\x80\x94 " + entry.detail);
                        row.guid  = entry.guid;
                        rows.push_back(std::move(row));
                    });

                    std::vector<TimelineEntry> feedEntries;
                    feedEntries.reserve(rows.size());
                    for (const ActivityRow& row : rows)
                        feedEntries.push_back(TimelineEntry{ row.age.c_str(), row.title.c_str(), row.detail.c_str() });

                    const TimelineFeedResult feedResult = TimelineFeed("##activityfeed", feedEntries.data(),
                                                                       static_cast<int>(feedEntries.size()));
                    if (feedResult.hoveredIndex >= 0)
                        DrawAssetPeekTooltip(model, services,
                                            rows[static_cast<std::size_t>(feedResult.hoveredIndex)].guid,
                                            /*forceShow=*/true);
                    if (feedResult.clickedIndex >= 0)
                        model.Select(rows[static_cast<std::size_t>(feedResult.clickedIndex)].guid);
                }

                ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
                ImGui::TextDisabled("Scenes");

                // bootGuid: the SAME helper DrawBrowseLens reads for
                // DrawAssetRow's "boot" pill, never a second parse.
                const Arcane::Guid bootGuid = BootSceneGuid(project);

                // The SAME list, in the same order, the Graph lens's focus
                // combo offers (Task 5 hoisted it out of here).
                const std::vector<const AssetPanelEntry*> scenes = ScenesByName(model);

                if (scenes.empty())
                    ImGui::TextDisabled("no scenes");
                else
                    for (const AssetPanelEntry* e : scenes)
                        DrawSceneCard(state, model, *e, bootGuid);

                ImGui::EndTable();
            }

            ImGui::EndChild();
        }

        // ===================================================================
        // Plan 3 (spec §10): the Graph lens's ax::NodeEditor canvas
        // ===================================================================
        // Ruling 1: spec §10's "reuses the shader editor's canvas vocabulary"
        // IS the vendored ax::NodeEditor -- the shader editor has no
        // hand-rolled canvas -- and every `ed::` call for this lens lives
        // HERE, in this one TU. Nothing about the node editor reaches
        // AssetsPanel.hpp (the context is a `void*` there) or the shared
        // widget layer (CanvasPopupScope.hpp:16-19 makes the same refusal).
        // The graph-framework extraction stays deferred: no schema, no undo,
        // no serialization layer is invented for this canvas.
        //
        // The shader editor is the IDIOM SOURCE, cited per helper below --
        // copied in SHAPE, never by including its header.
        namespace ed = ax::NodeEditor;

        // ---- Fixed geometry: spec §11.2's "graph nodes" row, VERBATIM -----
        // "graph nodes | w 180-220, header 24px, accent bar 3px, pins 9px"
        constexpr float kGraphNodeMinWidth   = 180.0f;
        constexpr float kGraphNodeMaxWidth   = 220.0f;
        constexpr float kGraphHeaderHeight   = 24.0f;
        constexpr float kGraphAccentBarWidth = 3.0f;
        // 9px ACROSS: the radius is half the spec's diameter, exactly as the
        // plan spells out ("DrawPinDot -- radius becomes 4.5f for §11.2's
        // 9px").
        constexpr float kGraphPinRadius      = 4.5f;
        constexpr int   kGraphPinSegments    = 12;
        constexpr float kGraphPinRingWidth   = 1.6f;

        // ---- Layout pitch (tuning values; Task 5's render comparison against
        // OptionD-Graph-FINAL.png arbitrates the final numbers).
        //
        // COLUMN PITCH is measured off the board rather than guessed: its
        // three columns sit at x = 40 / 330 / 660, i.e. pitches of 290 and
        // 330. 300 sits between them and -- unlike the plan's ~260 starting
        // suggestion -- leaves a real gutter at the §11.2 CEILING too (a
        // 220px node in a 260px column leaves 40px, which is not enough air
        // for the wire's own bulge, let alone the mid-edge label that has to
        // sit in it: at that gap the eased control points still reach ~30px
        // each way).
        //
        // ROW PITCH is the plan's 90 unchanged: a node stands 54px
        // (GraphNodeHeight), so that is 36px of air between stacked rows.
        // Tighter than the board's ~70, deliberately -- the board shows three
        // nodes in a column and a real project's "everything" mode shows
        // dozens.
        constexpr float kGraphColumnPitch = 300.0f;
        constexpr float kGraphRowPitch    = 90.0f;

        // ---- Node internals, read off the board (OptionD.dc.html) ---------
        // `.nhead { height: 24px; padding: 0 8px 0 11px; gap: 6px }` -- the
        // 11px left inset is the 3px accent bar plus 8px of air, which is why
        // it is spelled as those two terms rather than as a bare literal.
        constexpr float kGraphNodePadLeft  = kGraphAccentBarWidth + 8.0f;
        constexpr float kGraphNodePadRight = 8.0f;
        constexpr float kGraphNodeBodyPadY = 6.0f;
        constexpr float kGraphNodeIconGap  = 6.0f;
        constexpr float kGraphHeaderFontPx = 14.0f;   // §11.3: "13-14px secondary via PushFont"
        constexpr float kGraphMetaFontPx   = 13.0f;

        // ---- Canvas palette ----------------------------------------------
        // CONTROLLER RULING (Task 5 render comparison, 2026-09-08): the BOARD
        // WINS over the plan's Theme::kPanel pin. `OptionD.dc.html`'s graph
        // canvas is `background: #121212` -- which is EXACTLY Theme::kWell
        // (EditorTheme.hpp: kWell = 0.071f = #121212), so the board's value
        // and the theme's field-well token are the same colour, not merely
        // close. The plan's kPanel pin was derived from the shader editor's
        // own kCanvasColor precedent (ShaderEditorDocument.cpp:233-238), not
        // from the board; spec §11 makes the mocks the redline and the plan
        // itself appointed the render comparison as the arbiter. Measured
        // before the switch: board canvas (18,18,18) vs its chrome (30,30,30)
        // -- a recessed well; the editor's canvas was (30,30,30), identical
        // to its own toolbar and bottom bar, so the graph field had no edge
        // at all.
        //
        // One constant drives the whole surface family: the grid wash, the
        // ghost/overflow body wash and the un-emphasized wire dim all pull
        // TOWARD this colour, so moving it moves them coherently.
        //
        // FOLLOW-UP RULING (same session): the redline authority covers the
        // node-over-canvas RELATIONSHIPS too, not the canvas alone. Moving the
        // canvas by itself had left the nodes reading as more RAISED than the
        // board's -- measured: board 18 -> band 25 (+7) -> body 30 (+12),
        // against this lens's 18 -> 35 (+17) -> 45 (+27).
        //
        // So the three node surfaces below are the BOARD's, read out of
        // `OptionD.dc.html`'s own CSS rather than sampled off the render:
        //     .node  { background: #1e1e1e; border: 1px solid #0d0d0d; }
        //     .nhead { background: #191919; }
        // and every one has an EXACT EditorTheme token -- the same happy
        // accident kWell was for the canvas -- so all three are spelled as
        // TOKENS, never as literals that would drift off the ramp later:
        //     #1e1e1e = Theme::kPanel  (0.118f)
        //     #191919 = Theme::kChrome (0.098f)
        //     #0d0d0d = Theme::kBorder (0.051f)
        // kBorder landing DARKER than the canvas it outlines is not an
        // oversight: that is the token's stated job ("kBorder is DARKER than
        // every surface it outlines" -- EditorTheme.hpp), and the board draws
        // exactly this (#0d0d0d hairline on a #121212 field).
        //
        // LENS-LOCAL, deliberately. These are this file's own constants
        // feeding this lens's own ApplyAssetGraphCanvasStyle; the SHADER
        // editor's shared canvas constants are UNTOUCHED, so the ruling moves
        // the Graph lens onto its board without dragging a second canvas --
        // which has its own board, its own review history and no such ruling
        // -- along with it. The accepted cost is that the editor's two
        // canvases no longer read as identically-toned material; recorded here
        // so it reads as a decision rather than as drift.
        //
        // NOT covered by either ruling, so NOT changed: the grid colours below
        // (the board's single dot grid is #242424; this lens keeps its
        // minor/major two-tier grid) and the pill/label colours. See the fix
        // report.
        constexpr ImVec4 kGraphCanvasColor    = Theme::kWell;                          // #121212
        constexpr ImVec4 kGraphGridMinorColor = ImVec4(0.180f, 0.180f, 0.196f, 0.55f);
        constexpr ImVec4 kGraphGridMajorColor = ImVec4(0.235f, 0.235f, 0.255f, 0.90f);
        constexpr ImVec4 kGraphNodeBodyColor  = Theme::kPanel;                         // #1e1e1e
        constexpr ImVec4 kGraphNodeTitleColor = Theme::kChrome;                        // #191919
        constexpr ImVec4 kGraphNodeBorder     = Theme::kBorder;                        // #0d0d0d
        // Selection amber / hover cyan: the editor-wide outline language
        // (ShaderEditorDocument.cpp:246-250, itself the viewport outline
        // composite's kSelectColor/kHoverColor).
        constexpr ImVec4 kGraphNodeSelBorder  = ImVec4(1.0f,  0.65f, 0.10f, 1.0f);
        constexpr ImVec4 kGraphNodeHovBorder  = ImVec4(0.25f, 0.70f, 1.0f,  1.0f);
        constexpr float  kGraphNodeRounding      = 4.0f;   // the canvas's own language -- kept
        constexpr float  kGraphNodeBorderWidth   = 1.0f;
        constexpr float  kGraphNodeHovBorderW    = 1.5f;
        constexpr float  kGraphNodeSelBorderW    = 2.0f;   // spec §10: "selection = 2px"

        constexpr float kGraphWireThickness = 2.0f;
        // The subtle anchor -> "+N more" connector: thinner than a data edge
        // on purpose (it is NOT one -- see DrawGraphLens's own comment).
        constexpr float kGraphOverflowWireThickness = 1.5f;
        // How far a wire's colour is pulled toward the canvas when the edge
        // is NOT emphasized. The board's edges read as a mid-gray against the
        // backdrop; dimming the source kind's accent this far lands in the
        // same tonal band while still saying which kind the edge leaves.
        constexpr float kGraphWireDim         = 0.62f;
        constexpr float kGraphOverflowWireDim = 0.78f;
        // The ghost/overflow body wash: the canvas tone laid back over the
        // node body at partial alpha, which pulls a tombstone or a "+N more"
        // chip toward the backdrop without inventing a second body colour.
        constexpr float kGraphGhostWash = 0.55f;

        // ---- Task 6: the dashed in-flight wire ----------------------------
        // `stroke-dasharray: 6 5` on the board's amber drag path
        // (OptionD.dc.html / Demo.dc.html: the `M230,330 C320,330 390,402
        // 462,402` path), in SCREEN pixels -- the walk below divides by the
        // view scale so a dash keeps that reading at every zoom stop.
        constexpr float kGraphDashOnPx  = 6.0f;
        constexpr float kGraphDashOffPx = 5.0f;
        // The board's `<circle cx="462" cy="402" r="4">` -- the cursor end of
        // the drag wears a solid amber dot, which is what makes the free end
        // read as "attached to the pointer" rather than as a wire that just
        // stops.
        constexpr float kGraphDashEndDotRadius = 4.0f;
        // LOD floor. The dash walk splits the curve at every on/off boundary,
        // so the CELL COUNT -- not the segment count -- is what bounds its
        // work. Zoomed far in, 11 screen pixels is a vanishing distance in
        // canvas units and the pattern is unresolvable anyway; the cap
        // stretches the cell (ratio preserved) rather than letting the walk
        // grind. Screen length is bounded by the viewport in practice, so this
        // is a guard against a pathological view scale, not the common path.
        constexpr int kGraphDashMaxCells = 256;

        // Mid-edge labels (ruling 9) stop being legible long before the nodes
        // do, so they are the first thing the canvas drops on zoom-out. The
        // threshold is the shader editor's own LOD table, ported: its
        // kLodLowMax = 0.250 is the last stop of the LowDetail tier
        // (FFixedZoomLevelsContainer, SNodePanel.cpp:56-75, via
        // ShaderEditorDocument.cpp's NodeLODForScale). At or below that,
        // labels are skipped; MediumDetail and up draw them.
        constexpr float kGraphLabelMinScale = 0.250f;
        constexpr float kGraphLabelFontPx   = 12.0f;   // §11.2's pill/label text size

        // c_LinkChannel_Links, reproduced. It is a file-static in the
        // vendored TU (imgui_node_editor.cpp:130-131) so it cannot be named
        // from here; the derivation and the WHOLE two-layer rationale (why a
        // transparent ed::Link costs nothing, why hover/selection halos
        // survive, and why channel 7 is the only layer that puts a
        // hand-drawn wire where the flat one was) are written out once at
        // ShaderEditorDocument.cpp:311-357. Read that block before touching
        // anything here.
        constexpr int kGraphLinkChannel = 7;

        // Spec §11.3's kind-color table, VERBATIM, as a panel-local function
        // in PinColorForWidth's shape (ShaderEditorDocument.cpp:491) --
        // ruling 5: EditorTheme.hpp:27-32 rules domain colour-coding out of
        // the theme, and the kPillAmberBorder precedent (EditorWidgets.cpp:305)
        // covers a spec-pinned hex with no token.
        //
        // The five rows §11.3 pins are the only five it pins. Every OTHER
        // kind -- Audio/Font/Data/Diagnostic/Other -- gets the theme's
        // neutral grab gray rather than an invented hue. That fallback is
        // also what a SYNTHETIC OVERFLOW node lands on: it carries
        // AssetKind::Other ALWAYS, never its anchor's kind, precisely so this
        // table cannot paint it as one more instance of whatever it
        // overflowed from (AssetGraphViewModel.hpp's own field comment).
        ImVec4 KindAccentColor(AssetKind kind) noexcept
        {
            switch (kind)
            {
                case AssetKind::Texture:  return ImVec4(0.6902f, 0.4157f, 0.3569f, 1.0f); // #b06a5b
                case AssetKind::Material: return ImVec4(0.4157f, 0.6078f, 0.3569f, 1.0f); // #6a9b5b
                case AssetKind::Mesh:     return ImVec4(0.3569f, 0.6078f, 0.6902f, 1.0f); // #5b9bb0
                case AssetKind::Sprite:   return ImVec4(0.6078f, 0.3569f, 0.6902f, 1.0f); // #9b5bb0
                case AssetKind::Scene:    return ImVec4(0.6902f, 0.6078f, 0.3569f, 1.0f); // #b09b5b
                default: break;
            }
            return Theme::kGrab;   // #9a9a9a -- no §11.3 row, so no invented hue
        }

        ImVec4 GraphLerpColor(const ImVec4& a, const ImVec4& b, float t) noexcept
        {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                          a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }

        // Toward the canvas: "the same hue, further back".
        ImVec4 GraphDimColor(const ImVec4& c, float t) noexcept
        {
            return GraphLerpColor(c, ImVec4(kGraphCanvasColor.x, kGraphCanvasColor.y,
                                            kGraphCanvasColor.z, c.w), t);
        }

        // BrightenColor's idiom (ShaderEditorDocument.cpp:387): a quarter of
        // the way to white, alpha untouched.
        ImVec4 GraphBrightenColor(const ImVec4& c) noexcept
        {
            return GraphLerpColor(c, ImVec4(1.0f, 1.0f, 1.0f, c.w), 0.25f);
        }

        // ---- Id encoding --------------------------------------------------
        // Node ids are the view model's own node INDEX + 1, never the guid: a
        // synthetic overflow node REUSES its anchor's guid by construction
        // (AssetGraphViewModel.hpp), so a guid is not a unique node key here,
        // and hashing one would swap a collision-free scheme for a merely
        // improbable one. Ids therefore shuffle when the projection is
        // rebuilt -- which costs nothing: a rebuild also re-writes every node
        // position (ruling 2), and the MODEL, not the canvas, is the
        // selection authority (Task 4).
        std::uint64_t GraphNodeIdOf(std::size_t index) noexcept
        {
            return static_cast<std::uint64_t>(index) + 1ull;
        }
        // Two pins per node. LEFT is the node's OUTBOUND (refs / "what I
        // use") side and RIGHT is its INBOUND (referencers / "who uses me")
        // side -- that way round, and not the other, because the layout puts
        // sources on the LEFT (spec §10: "layered left-to-right by dependency
        // depth, sources left, scenes right", and the view model's layer() is
        // the longest OUTBOUND path to a leaf, so a target always sits in a
        // lower column than its referencer). The board agrees: uv_marker.png
        // -- a pure target -- carries a right-hand pin only, and main.arcscene
        // -- a pure referencer -- carries a left-hand pin only.
        //
        // It also falls straight out of the library's curve convention: a
        // link leaves its START pin along SourceDirection (+1,0) and arrives
        // at its END pin along TargetDirection (-1,0), so a wire has to start
        // at the LEFT node's right-hand pin and end at the RIGHT node's
        // left-hand pin. Hence RIGHT pins are ed::PinKind::Output and LEFT
        // pins are ed::PinKind::Input.
        std::uint64_t GraphLeftPinId(std::uint64_t nodeId) noexcept  { return nodeId * 4ull + 1ull; }
        std::uint64_t GraphRightPinId(std::uint64_t nodeId) noexcept { return nodeId * 4ull + 2ull; }

        // One-time style for this lens's node-editor context, in
        // ApplyGraphCanvasStyle's shape (ShaderEditorDocument.cpp:576).
        // Written to the PERSISTENT style (ed::GetStyle returns a mutable
        // reference) rather than pushed per frame, because every value here
        // is latched into the object at BeginNode/BeginPin time -- one
        // assignment covers every node for the context's life.
        void ApplyAssetGraphCanvasStyle()
        {
            ed::Style& s = ed::GetStyle();
            // The vendored grid AND background fill are switched off; our own
            // lattice is drawn underneath instead (DrawGraphGridFallback).
            // Wholesale replacement is the only option: the built-in grid is
            // a hardcoded 32px line pair with no StyleVar and no LOD fade.
            s.Colors[ed::StyleColor_Grid] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.Colors[ed::StyleColor_Bg]   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.Colors[ed::StyleColor_NodeBg]        = kGraphNodeBodyColor;
            s.Colors[ed::StyleColor_NodeBorder]    = kGraphNodeBorder;
            s.Colors[ed::StyleColor_HovNodeBorder] = kGraphNodeHovBorder;
            s.Colors[ed::StyleColor_SelNodeBorder] = kGraphNodeSelBorder;
            // A pin draws nothing of its own except a hover rect -- that
            // rectangle would fight the dot, so its alpha goes to zero and
            // the dot IS the pin visual.
            s.Colors[ed::StyleColor_PinRect]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.Colors[ed::StyleColor_PinRectBorder] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            s.NodeRounding            = kGraphNodeRounding;
            s.NodeBorderWidth         = kGraphNodeBorderWidth;
            s.HoveredNodeBorderWidth  = kGraphNodeHovBorderW;
            s.SelectedNodeBorderWidth = kGraphNodeSelBorderW;
            // ZERO node padding, unlike the shader editor's: this lens lays
            // its own rows out by hand (SetCursorScreenPos + explicit
            // Dummies) so the 24px header band and the node's total height
            // are EXACT rather than whatever the ambient font metrics plus a
            // padding pair happen to add up to. With no padding the node's
            // content origin IS ed::GetNodePosition, which is also what lets
            // the pin geometry be computed without a frame of readback lag.
            s.NodePadding = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        // The canvas's view scale, in the same units as a zoom stop. THE
        // TRAP (ViewScale, ShaderEditorDocument.cpp:441): ed::GetCurrentZoom
        // returns InvScale -- canvas units per screen pixel -- the RECIPROCAL
        // of the scale everything else means by "zoom".
        float GraphViewScale() noexcept
        {
            const float invScale = ed::GetCurrentZoom();
            return invScale > 0.0001f ? 1.0f / invScale : 1.0f;
        }

        // Cubic bezier at t -- the same evaluation the library tessellates.
        ImVec2 GraphCubicBezierAt(const ImVec2& p0, const ImVec2& p1,
                                  const ImVec2& p2, const ImVec2& p3, float t) noexcept
        {
            const float u = 1.0f - t;
            const float w0 = u * u * u;
            const float w1 = 3.0f * u * u * t;
            const float w2 = 3.0f * u * t * t;
            const float w3 = t * t * t;
            return ImVec2(p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                          p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3);
        }

        // The two control points for a wire between `p0` (a left-hand
        // endpoint, leaving rightward) and `p3` (a right-hand endpoint,
        // arriving leftward). Reproduces Link::GetCurve exactly, reading the
        // style rather than assuming it, so a later LinkStrength or direction
        // change moves our curve and the library's together
        // (DrawGradientWire's convention, ShaderEditorDocument.cpp:5427-5446).
        void GraphWireControlPoints(const ImVec2& p0, const ImVec2& p3,
                                    ImVec2& p1, ImVec2& p2) noexcept
        {
            const ed::Style& st = ed::GetStyle();
            const float dx = p3.x - p0.x;
            const float dy = p3.y - p0.y;
            const float halfDistance = std::sqrt(dx * dx + dy * dy) * 0.5f;
            const auto ease = [halfDistance](float strength)
            {
                // Guarded against a zero strength the library never divides
                // by (its own branch is only entered when halfDistance <
                // strength, which a zero strength cannot satisfy).
                constexpr float kPi = 3.14159265358979323846f;
                if (strength > 0.0f && halfDistance < strength)
                    return strength * std::sin(kPi * 0.5f * halfDistance / strength);
                return strength;
            };
            const float s = ease(st.LinkStrength);
            p1 = ImVec2(p0.x + st.SourceDirection.x * s, p0.y + st.SourceDirection.y * s);
            p2 = ImVec2(p3.x + st.TargetDirection.x * s, p3.y + st.TargetDirection.y * s);
        }

        // Hand-drawn wire in the LINKS channel. Returns the curve's midpoint
        // (canvas space) so a caller can hang a label off it.
        //
        // Retargeting the channel is not optional -- between ed::Begin and
        // ed::End but outside a node the current channel is the BOTTOM of the
        // merge, under the grid's own background fill, so a wire drawn there
        // would simply be painted over. See kGraphLinkChannel.
        ImVec2 DrawGraphWire(const ImVec2& p0, const ImVec2& p3,
                             const ImVec4& color, float thickness)
        {
            ImVec2 p1, p2;
            GraphWireControlPoints(p0, p3, p1, p2);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Defensive: the link channels exist from Begin, but never index
            // past a splitter that has not been grown.
            if (dl->_Splitter._Count > kGraphLinkChannel)
            {
                const int prevChannel = dl->_Splitter._Current;
                dl->ChannelsSetCurrent(kGraphLinkChannel);
                dl->AddBezierCubic(p0, p1, p2, p3, ImGui::GetColorU32(color), thickness);
                dl->ChannelsSetCurrent(prevChannel);
            }
            return GraphCubicBezierAt(p0, p1, p2, p3, 0.5f);
        }

        // The DASHED in-flight wire (Task 6; plan ruling 8 -- spec §11.1's
        // "one new technique" for this plan). Same curve as DrawGraphWire, in
        // the same channel and the same canvas space, walked with an on/off
        // ARC-LENGTH PHASE ACCUMULATOR so the pattern is measured along the
        // curve rather than along t (which would bunch the dashes wherever
        // the bezier is dense).
        //
        // The per-segment loop skeleton is DrawGradientWire's
        // (ShaderEditorDocument.cpp:5484-5496) -- and so is its cap
        // reasoning, which :5480-5483 states: consecutive samples on a curve
        // this smooth are near-collinear, so butt caps meet without visible
        // notches. A dash is a separate stroke by definition here (a shared
        // PathStroke cannot lift its pen), which is the same reason that one
        // could not use one either.
        //
        // ed::Flow's marching dots are deliberately NOT used: they are an
        // animation over an EXISTING link, and this curve has no link behind
        // it -- nor is a travelling dot the board's language (ruling 8).
        void DrawGraphDashedWire(const ImVec2& p0, const ImVec2& p3, const ImVec4& color,
                                 float thickness, float viewScale)
        {
            ImVec2 p1, p2;
            GraphWireControlPoints(p0, p3, p1, p2);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Defensive, exactly as DrawGraphWire is: never index past a
            // splitter that has not been grown.
            if (dl->_Splitter._Count <= kGraphLinkChannel)
                return;

            const auto len = [](float ax, float ay) { return std::sqrt(ax * ax + ay * ay); };
            // The control polygon is a cheap upper bound on arc length --
            // DrawGradientWire's own approximation, kept so both wires spend
            // vertices the same way.
            const float polyLen = len(p1.x - p0.x, p1.y - p0.y) +
                                  len(p2.x - p1.x, p2.y - p1.y) +
                                  len(p3.x - p2.x, p3.y - p2.y);
            const float scale     = viewScale > 0.0f ? viewScale : 1.0f;
            const float screenLen = polyLen * scale;
            const int segments = static_cast<int>(
                (std::min)(64.0f, (std::max)(12.0f, screenLen / 6.0f)));

            // Cell lengths in CANVAS units, so the dash reads 6-on/5-off on
            // screen at any zoom -- then the LOD floor (kGraphDashMaxCells).
            float on  = kGraphDashOnPx  / scale;
            float off = kGraphDashOffPx / scale;
            if (const float floorLen = polyLen / static_cast<float>(kGraphDashMaxCells);
                on + off < floorLen && on + off > 0.0f)
            {
                const float k = floorLen / (on + off);
                on  *= k;
                off *= k;
            }

            const int prevChannel = dl->_Splitter._Current;
            dl->ChannelsSetCurrent(kGraphLinkChannel);
            const ImU32 col = ImGui::GetColorU32(color);

            // `cellLeft` is the distance still owed to the current on/off
            // cell; it carries ACROSS segment boundaries, which is the whole
            // point of accumulating phase rather than dashing each segment.
            bool  ink      = true;
            float cellLeft = on;
            ImVec2 prev = p0;
            for (int i = 1; i <= segments; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const ImVec2 cur = GraphCubicBezierAt(p0, p1, p2, p3, t);
                float segLeft = len(cur.x - prev.x, cur.y - prev.y);
                ImVec2 a = prev;
                // A degenerate segment (both control points coincident, or a
                // zero-length drag) has no length to spend and would divide by
                // zero below.
                while (segLeft > 0.0f && cellLeft > 0.0f)
                {
                    const float step = (std::min)(segLeft, cellLeft);
                    // `a` lies ON the straight run a->cur, so advancing by
                    // step/segLeft of what REMAINS of it is exact.
                    const float u = step / segLeft;
                    const ImVec2 b(a.x + (cur.x - a.x) * u, a.y + (cur.y - a.y) * u);
                    if (ink)
                        dl->AddLine(a, b, col, thickness);
                    a = b;
                    segLeft  -= step;
                    cellLeft -= step;
                    if (cellLeft <= 0.0f)
                    {
                        ink      = !ink;
                        cellLeft = ink ? on : off;
                    }
                }
                prev = cur;
            }

            dl->ChannelsSetCurrent(prevChannel);
        }

        // The free (pointer) end's solid dot, in the same channel as the wire.
        // Separate from the walk above because only the CALLER knows which end
        // the pointer holds -- it orients the curve, so the pin end is p0 for a
        // right-pin drag and p3 for a left-pin one.
        void DrawGraphWireEndDot(const ImVec2& centre, const ImVec4& color)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (dl->_Splitter._Count <= kGraphLinkChannel)
                return;
            const int prevChannel = dl->_Splitter._Current;
            dl->ChannelsSetCurrent(kGraphLinkChannel);
            dl->AddCircleFilled(centre, kGraphDashEndDotRadius,
                                ImGui::GetColorU32(color), kGraphPinSegments);
            dl->ChannelsSetCurrent(prevChannel);
        }

        // One port dot, DrawPinDot's reading (ShaderEditorDocument.cpp:507):
        // FILLED when something is attached, a hollow ring when not. Unlike
        // that one this does NOT advance the cursor -- this lens positions
        // its pins on the node's own edge by hand, so the dot is pure
        // drawlist paint and the pin's layout contribution is nil.
        void DrawGraphPinDot(ImDrawList* dl, const ImVec2& centre,
                             const ImVec4& color, bool connected)
        {
            const ImU32 col = ImGui::GetColorU32(color);
            if (connected)
            {
                dl->AddCircleFilled(centre, kGraphPinRadius, col, kGraphPinSegments);
            }
            else
            {
                dl->AddCircleFilled(centre, kGraphPinRadius,
                                    ImGui::GetColorU32(kGraphNodeBodyColor), kGraphPinSegments);
                dl->AddCircle(centre, kGraphPinRadius, col, kGraphPinSegments, kGraphPinRingWidth);
            }
        }

        // Trim `text` to fit `maxWidth` under the CURRENT font, appending a
        // real ellipsis when it had to cut. Never cuts inside a UTF-8
        // sequence. A graph node label is a file stem, so the linear walk is
        // cheap; the point is that the node's WIDTH is pinned by §11.2 and
        // the label has to yield to it, not the other way round.
        std::string GraphEllipsize(const std::string& text, float maxWidth)
        {
            if (maxWidth <= 0.0f)
                return std::string();
            if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
                return text;
            const char* kEllipsis = "\xE2\x80\xA6";   // U+2026
            const float ellipsisW = ImGui::CalcTextSize(kEllipsis).x;
            std::size_t cut = text.size();
            while (cut > 0)
            {
                --cut;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                    --cut;
                if (ImGui::CalcTextSize(text.c_str(), text.c_str() + cut).x + ellipsisW <= maxWidth)
                    break;
            }
            return text.substr(0, cut) + kEllipsis;
        }

        // Everything one node needs, computed BEFORE submission so the pin
        // geometry (and therefore every wire endpoint) is exact from frame
        // one -- no waiting on ed::GetNodeSize to report a measurement.
        // Honest only because this lens lays the node out by hand and pins
        // its bottom-right corner with an explicit Dummy; see DrawGraphNode.
        struct GraphNodeVisual
        {
            ImVec2 pos;                 // canvas space, top-left
            float  width  = 0.0f;
            float  height = 0.0f;
            bool   hasLeftPin  = false;  // outbound / refs side
            bool   hasRightPin = false;  // inbound / referencers side
            bool   leftConnected  = false;
            bool   rightConnected = false;
        };

        // The node body's one content row: an 18px thumb (or the kind icon
        // in the same cell) plus whatever pills fit, else a dim meta line.
        struct GraphNodeBody
        {
            std::uint64_t thumb = 0;
            const char*   icon  = nullptr;
            std::vector<std::pair<const char*, int>> pills;   // text, AssetPill variant
            std::string   meta;                                // used when there are no pills
        };

        // Node body row height: the 18px thumb cell is the tallest thing in
        // it, so it sets the row.
        float GraphBodyRowHeight()
        {
            return kAssetRowThumbSize;
        }

        float GraphNodeHeight()
        {
            return kGraphHeaderHeight + kGraphNodeBodyPadY + GraphBodyRowHeight() + kGraphNodeBodyPadY;
        }

        // Chrome drawn AFTER ed::EndNode, in the node's own user-background
        // channel -- above the library's body fill, below its content and pin
        // chrome -- which is exactly where a header band and an accent bar
        // belong (DrawNodeTitleBand's rationale, ShaderEditorDocument.cpp:555).
        // Coordinates are canvas space, the space both ed::GetNodePosition
        // and plain ImGui use inside ed::Begin/End.
        //
        // `wash` > 0 lays the canvas tone back over the whole body at that
        // alpha, which is how a tombstone and a "+N more" chip read as ghosts
        // without a second body colour existing; `borderAccent` non-null
        // paints a 1px inset border INSIDE the library's own, so the
        // library's hover/selection border still shows through around it.
        void DrawGraphNodeChrome(std::uint64_t nodeId, const GraphNodeVisual& v,
                                 bool drawBand, const ImVec4* accent,
                                 float wash, const ImVec4* borderAccent)
        {
            ImDrawList* bg = ed::GetNodeBackgroundDrawList(ed::NodeId(nodeId));
            if (!bg)
                return;

            const float b = kGraphNodeBorderWidth;
            const ImVec2 innerMin(v.pos.x + b, v.pos.y + b);
            const ImVec2 innerMax(v.pos.x + v.width - b, v.pos.y + v.height - b);

            if (drawBand)
                bg->AddRectFilled(innerMin, ImVec2(innerMax.x, v.pos.y + kGraphHeaderHeight),
                                  ImGui::GetColorU32(kGraphNodeTitleColor),
                                  kGraphNodeRounding, ImDrawFlags_RoundCornersTop);

            if (accent)
                // Drawn AFTER the band so it runs the node's FULL height, the
                // way the board's `.accent { top: 0; bottom: 0 }` does -- the
                // header is not a separate region the bar stops at.
                bg->AddRectFilled(innerMin, ImVec2(innerMin.x + kGraphAccentBarWidth, innerMax.y),
                                  ImGui::GetColorU32(*accent),
                                  kGraphNodeRounding, ImDrawFlags_RoundCornersLeft);

            if (wash > 0.0f)
                bg->AddRectFilled(innerMin, innerMax,
                                  ImGui::GetColorU32(Theme::WithAlpha(kGraphCanvasColor, wash)),
                                  kGraphNodeRounding);

            if (borderAccent)
                bg->AddRect(innerMin, innerMax, ImGui::GetColorU32(*borderAccent),
                            kGraphNodeRounding, ImDrawFlags_RoundCornersAll,
                            kGraphNodeBorderWidth);
        }

        // Submit one node: the §10 anatomy (header row + 3px kind accent bar
        // + body row + the two edge pins), laid out by hand against the
        // zero-padding node style so the header band is EXACTLY 24px and the
        // node's measured size is exactly `v.width` x `v.height`.
        //
        // Every text run is pure ImDrawList overdraw rather than a real ImGui
        // item -- the RowWithThumb fix's reasoning (EditorWidgets.cpp),
        // applied for a second reason here: a text item's own extent would
        // feed the node's group rect and let a long label push the node past
        // §11.2's 220px ceiling. The only real items submitted are the two
        // width/height Dummies and the pills, all of which are sized against
        // a budget this function computed.
        void DrawGraphNode(std::uint64_t nodeId, const GraphNodeVisual& v,
                           const GraphNodeBody& body, const char* headerIcon,
                           const std::string& headerLabel, const char* headerPill,
                           const ImVec4& accent, bool ghost)
        {
            ed::BeginNode(ed::NodeId(nodeId));

            // With NodePadding zeroed, the cursor at this point IS the node's
            // top-left in canvas space.
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Pins the node's WIDTH. A zero-height item so it costs no rows.
            ImGui::Dummy(ImVec2(v.width, 0.0f));

            // ---- pins, submitted FIRST -----------------------------------
            // Order is load-bearing, not taste: ed::BeginPin opens an ImGui
            // group, and EndGroup contributes a group rect measured from the
            // cursor AT BeginGroup. Submitted after the node's last row that
            // cursor sits one ItemSpacing.y BELOW the node's bottom edge, and
            // the node silently grows by 4px. Submitted here -- with the
            // cursor explicitly parked back at `origin` -- the group is
            // degenerate and contributes exactly nothing, which is what keeps
            // the measured node size equal to `v.height` and therefore keeps
            // the pin centres (and every wire endpoint derived from them)
            // exact from frame one.
            //
            // Each pin's HIT rect straddles the node's border (the board's
            // `left: -5px` / `right: -5px`), which ed::PinRect lets us state
            // outright instead of inferring it from an item rect that would
            // drag the node's own bounds out with it. ed::PinPivotRect then
            // makes the dot's centre the wire anchor, so the curve we
            // hand-draw and the curve the library hit-tests cannot drift
            // apart (SetPinPivot's rationale, ShaderEditorDocument.cpp:5387).
            {
                const float pinY = origin.y + v.height * 0.5f;
                const auto submitPin = [&](std::uint64_t pinId, ed::PinKind kind,
                                           const ImVec2& centre, bool connected)
                {
                    ImGui::SetCursorScreenPos(origin);
                    ed::BeginPin(ed::PinId(pinId), kind);
                    ed::PinRect(ImVec2(centre.x - kGraphPinRadius, centre.y - kGraphPinRadius),
                                ImVec2(centre.x + kGraphPinRadius, centre.y + kGraphPinRadius));
                    ed::PinPivotRect(centre, centre);
                    // BeginPin's group needs one item to close over. Zero
                    // size, at the node's own origin, so it can add nothing
                    // to either rect -- PinRect already pinned the hit rect
                    // explicitly. It also clears ImGui's IsSetPos flag, which
                    // is what keeps EndGroup's
                    // "SetCursorPos to extend boundaries" check quiet.
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                    DrawGraphPinDot(ImGui::GetWindowDrawList(), centre, accent, connected);
                    ed::EndPin();
                };
                if (v.hasLeftPin)
                    submitPin(GraphLeftPinId(nodeId), ed::PinKind::Input,
                              ImVec2(origin.x, pinY), v.leftConnected);
                if (v.hasRightPin)
                    submitPin(GraphRightPinId(nodeId), ed::PinKind::Output,
                              ImVec2(origin.x + v.width, pinY), v.rightConnected);
            }

            const ImU32 textCol = ImGui::GetColorU32(ghost ? Theme::kTextDim : Theme::kText);
            const ImU32 dimCol  = ImGui::GetColorU32(Theme::kTextDim);

            // ---- header row ----
            {
                ImGui::PushFont(GetEditorFonts().interRegular, kGraphHeaderFontPx);
                const float lineH  = ImGui::GetTextLineHeight();
                const float rowY   = origin.y + (kGraphHeaderHeight - lineH) * 0.5f;
                float x = origin.x + kGraphNodePadLeft;

                float rightEdge = origin.x + v.width - kGraphNodePadRight;
                if (headerPill)
                    rightEdge -= PillWidth(headerPill) + kGraphNodeIconGap;

                if (headerIcon)
                {
                    dl->AddText(ImVec2(x, rowY), dimCol, headerIcon);
                    x += ImGui::CalcTextSize(headerIcon).x + kGraphNodeIconGap;
                }
                const std::string shown = GraphEllipsize(headerLabel, rightEdge - x);
                dl->AddText(ImVec2(x, rowY), textCol, shown.c_str());
                ImGui::PopFont();

                if (headerPill)
                {
                    // The one real item in the header. Placed by cursor, so
                    // AssetPill's own Dummy lands inside the node's width --
                    // `rightEdge` above already reserved its slot.
                    ImGui::SetCursorScreenPos(
                        ImVec2(origin.x + v.width - kGraphNodePadRight - PillWidth(headerPill),
                               origin.y + (kGraphHeaderHeight - kPillLineHeight) * 0.5f));
                    AssetPill(headerPill, 1);
                }
            }

            // ---- body row ----
            {
                const float rowTop = origin.y + kGraphHeaderHeight + kGraphNodeBodyPadY;
                const float rowH   = GraphBodyRowHeight();
                float x = origin.x + kGraphNodePadLeft;

                if (body.thumb != 0)
                {
                    dl->AddImage(static_cast<ImTextureID>(body.thumb), ImVec2(x, rowTop),
                                 ImVec2(x + kAssetRowThumbSize, rowTop + kAssetRowThumbSize));
                }
                else if (body.icon)
                {
                    const ImVec2 iconSize = ImGui::CalcTextSize(body.icon);
                    dl->AddText(ImVec2(x + (kAssetRowThumbSize - iconSize.x) * 0.5f,
                                       rowTop + (rowH - iconSize.y) * 0.5f),
                                dimCol, body.icon);
                }
                if (body.thumb != 0 || body.icon)
                    x += kAssetRowThumbSize + ImGui::GetStyle().ItemInnerSpacing.x;

                const float budgetEnd = origin.x + v.width - kGraphNodePadRight;
                if (!body.pills.empty())
                {
                    bool first = true;
                    for (const auto& [text, variant] : body.pills)
                    {
                        const float w = PillWidth(text);
                        const float gap = first ? 0.0f : ImGui::GetStyle().ItemSpacing.x;
                        if (x + gap + w > budgetEnd)
                            break;   // never let a pill push the node past §11.2's width
                        x += gap;
                        ImGui::SetCursorScreenPos(ImVec2(x, rowTop + (rowH - kPillLineHeight) * 0.5f));
                        AssetPill(text, variant);
                        x += w;
                        first = false;
                    }
                }
                else if (!body.meta.empty())
                {
                    ImGui::PushFont(GetEditorFonts().interRegular, kGraphMetaFontPx);
                    const std::string shown = GraphEllipsize(body.meta, budgetEnd - x);
                    dl->AddText(ImVec2(x, rowTop + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                                dimCol, shown.c_str());
                    ImGui::PopFont();
                }
            }

            // Pins the node's HEIGHT (and re-asserts its width), which is
            // what makes `v.height` the measured height rather than a guess.
            // LAST, so nothing after it can push the group's bottom edge
            // further down.
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + v.height));
            ImGui::Dummy(ImVec2(v.width, 0.0f));

            ed::EndNode();
        }

        // The Graph lens's node context menu. A distinct id from the shader
        // editor's "##graphnodemenu" even though ImGui scopes popup ids to the
        // current window's id stack (they could never collide): a metrics
        // window listing both by name should say which is which.
        constexpr const char* kGraphNodeMenuId = "##assetgraphnodemenu";

        // The pin-drag ghost menu (Task 6). Its own id for the same reason.
        constexpr const char* kGraphCreateMenuId = "##assetgraphderivemenu";

        // ---- The canvas legend (Task 6, controller rider) -----------------
        // TRANSCRIBED from OptionD.dc.html's `<!-- legend -->` block, value for
        // value -- not designed here:
        //   position: absolute; left: 12px; bottom: 12px;
        //   display: flex; align-items: center; gap: 14px;
        //   background: #191919; border: 1px solid #0d0d0d;
        //   padding: 5px 10px; font-size: 13px; color: #737373;
        //   each entry: inline-flex; gap: 6px
        //     [18x2 solid #5c5c5c]                "derives / samples"
        //     [18x2 solid #4a4a4a]                "used by"
        //     [18px, border-top: 2px dashed #ffa61a] "drag a pin = create"
        // Three of the four tones are exact EditorTheme tokens (#191919 =
        // kChrome, #0d0d0d = kBorder, #737373 = kTextDim, #ffa61a = kAmber),
        // so they are spelled as tokens; the two edge greys have no token and
        // are kept as board literals, the kPillAmberBorder precedent.
        //
        // TWO DELIBERATE DEPARTURES FROM THE TRANSCRIPTION, both COPY only, by
        // CONTROLLER RULING (Task 6 review): a legend must not contradict the
        // lens it describes, so where the board's wording is untrue HERE the
        // minimal truthful edit wins over the transcription. Recorded so the
        // board and the shipped strings can be reconciled at a glance:
        //   1. "used by" -> "uses". Spec §10 / ruling 9 pin References ->
        //      "uses", and that is what the mid-edge labels on this very
        //      canvas say; the legend saying otherwise about the same wire is
        //      simply wrong.
        //   2. "drag a pin = create" -> "drag a material pin = derive". The
        //      board's phrasing promises something every NON-material pin
        //      refuses (the ghost menu's one entry is disabled there), and
        //      "derive" is the word the entry itself uses.
        // The GEOMETRY, the TONES and entry 1's string stay exactly as
        // transcribed.
        //
        // ONE KNOWN MISMATCH REMAINS, deliberately, for the user's desk pass:
        // the two greys legend the board's TWO-TONE edge scheme (asset->asset
        // vs asset->scene), while this lens colours edges by the SOURCE KIND's
        // accent dimmed toward the canvas (ruling 3/§11.3) -- so no drawn edge
        // is exactly either swatch. Left as transcribed by the same ruling:
        // the swatches read as "a line", not as a colour code, and retinting
        // them would be designing rather than transcribing.
        //
        // Chrome, NOT a node: drawn after ed::End in SCREEN space, so it does
        // not pan, zoom or sort against the graph.
        constexpr float kGraphLegendInset      = 12.0f;
        constexpr float kGraphLegendPadX       = 10.0f;
        constexpr float kGraphLegendPadY       = 5.0f;
        constexpr float kGraphLegendEntryGap   = 14.0f;
        constexpr float kGraphLegendSwatchGap  = 6.0f;
        constexpr float kGraphLegendSwatchW    = 18.0f;
        constexpr float kGraphLegendSwatchH    = 2.0f;
        constexpr float kGraphLegendFontPx     = 13.0f;
        constexpr ImVec4 kGraphLegendEdgeColor   = ImVec4(0.361f, 0.361f, 0.361f, 1.0f); // #5c5c5c
        constexpr ImVec4 kGraphLegendUsedByColor = ImVec4(0.290f, 0.290f, 0.290f, 1.0f); // #4a4a4a

        void DrawGraphLegend(const ImVec2& canvasMin, const ImVec2& canvasSize)
        {
            struct Entry { const char* text; ImVec4 color; bool dashed; };
            const Entry entries[] = {
                { "derives / samples", kGraphLegendEdgeColor,   false },
                { "uses",              kGraphLegendUsedByColor, false },
                { "drag a material pin = derive", Theme::kAmber, true },
            };

            ImGui::PushFont(GetEditorFonts().interRegular, kGraphLegendFontPx);
            const float lineH = ImGui::GetTextLineHeight();

            float contentW = 0.0f;
            for (int i = 0; i < IM_ARRAYSIZE(entries); ++i)
            {
                if (i > 0)
                    contentW += kGraphLegendEntryGap;
                contentW += kGraphLegendSwatchW + kGraphLegendSwatchGap +
                            ImGui::CalcTextSize(entries[i].text).x;
            }

            // SNAPPED TO WHOLE PIXELS. A 2px rule and a 1px border are the two
            // things here a half-pixel origin visibly softens (ImGui gives a
            // fractional rect fractional coverage), and the board's are crisp.
            // Safe to snap, unlike anything inside the canvas: the legend is
            // chrome in SCREEN space, with no zoom to make the rounding lie.
            const float boxW = std::floor(contentW) + kGraphLegendPadX * 2.0f;
            const float boxH = std::floor(lineH) + kGraphLegendPadY * 2.0f;
            const ImVec2 boxMin(std::floor(canvasMin.x + kGraphLegendInset),
                                std::floor(canvasMin.y + canvasSize.y - kGraphLegendInset - boxH));
            const ImVec2 boxMax(boxMin.x + boxW, boxMin.y + boxH);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(Theme::kChrome));
            dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(Theme::kBorder));

            const ImU32 textCol = ImGui::GetColorU32(Theme::kTextDim);
            const float midY = boxMin.y + boxH * 0.5f;
            float x = boxMin.x + kGraphLegendPadX;
            for (int i = 0; i < IM_ARRAYSIZE(entries); ++i)
            {
                if (i > 0)
                    x += kGraphLegendEntryGap;
                const ImU32 swatch = ImGui::GetColorU32(entries[i].color);
                x = std::floor(x);
                const float y0 = std::floor(midY - kGraphLegendSwatchH * 0.5f);
                if (entries[i].dashed)
                {
                    // The same 6-on/5-off cell the in-flight wire uses, walked
                    // straight across the 18px rule -- the legend IS the key to
                    // that wire, so it cannot pick its own pattern.
                    float cx = x;
                    bool ink = true;
                    while (cx < x + kGraphLegendSwatchW)
                    {
                        const float step = (std::min)(ink ? kGraphDashOnPx : kGraphDashOffPx,
                                                      x + kGraphLegendSwatchW - cx);
                        if (ink)
                            dl->AddRectFilled(ImVec2(cx, y0),
                                              ImVec2(cx + step, y0 + kGraphLegendSwatchH), swatch);
                        cx += step;
                        ink = !ink;
                    }
                }
                else
                {
                    dl->AddRectFilled(ImVec2(x, y0),
                                      ImVec2(x + kGraphLegendSwatchW, y0 + kGraphLegendSwatchH),
                                      swatch);
                }
                x += kGraphLegendSwatchW + kGraphLegendSwatchGap;
                dl->AddText(ImVec2(x, midY - lineH * 0.5f), textCol, entries[i].text);
                x += ImGui::CalcTextSize(entries[i].text).x;
            }
            ImGui::PopFont();
        }

        // ---- Task 3: the Graph lens body (spec §10) ------------------------
        // Canvas foundation + layered nodes + two-layer kind-coloured edges,
        // plus (Task 4) the interaction surface: the selection bridge, the
        // peek tooltip with its edge summary, double-click open and the
        // unified context menu. The focus combo and the lens-strip mask are
        // Task 5's; the pin-drag "Derive Instance..." gesture, its dashed
        // in-flight wire and the canvas legend are Task 6's (section 7b and
        // the ghost menu in 8e).
        //
        // THE ONE RULE THE CREATE BRACKET CARRIES: ed::EndCreate() is called
        // UNCONDITIONALLY. CreateItemAction::Begin() arms m_InActive even when
        // it returns false (the idle frame), so an EndCreate skipped inside
        // the `if` asserts on the NEXT frame's BeginCreate
        // (ShaderEditorDocument.cpp:5559-5561 -- the desk crash that was fine
        // on frame 1 and aborted on frame 2). That is also the crash class the
        // device-less test below the panel exists to keep closed.
        void DrawGraphLens(AssetsPanelState& state, AssetPanelModel& model,
                           const Arcane::Project* project, DocumentHost& docs,
                           const AssetsPanelServices& services,
                           AssetsPanelActions& actions)
        {
            // ---- 1. Rebuild the projection, and ONLY when it moved --------
            // The trigger is AssetPanelModel::entriesStamp (bumped exactly
            // when the entries map or the reference index changed content)
            // plus the focus guid. Deliberately NOT RebuildIfDirty's return
            // value, which is also true for a rows-only rebuild -- a search
            // keystroke -- that the graph does not read. A per-frame rebuild
            // is not acceptable (it is a whole BFS + layering pass).
            if (!state.graphBuilt ||
                state.graphBuiltStamp != model.entriesStamp ||
                state.graphBuiltFocus != state.graphFocus)
            {
                GraphBuildInput in;
                in.entries = &model.Entries();
                in.index   = &model.RefIndex();
                in.focus   = state.graphFocus;
                state.graph.Build(in);
                state.graphBuilt      = true;
                state.graphBuiltStamp = model.entriesStamp;
                state.graphBuiltFocus = state.graphFocus;
                state.graphLayoutDirty = true;
            }

            // ---- 2. The canvas context, created lazily -------------------
            if (!state.graphCanvas)
            {
                ed::Config cfg;
                // Ruling 2: NO canvas persistence. The library would
                // otherwise write node positions to an ini of its own, and
                // spec §10 pins the layout as computed each build, never
                // persisted.
                cfg.SettingsFile = nullptr;
                state.graphCanvas = ed::CreateEditor(&cfg);
                // The style is per-context state, so a freshly created
                // context applies it -- including the switch that kills the
                // vendored grid.
                ed::SetCurrentEditor(static_cast<ed::EditorContext*>(state.graphCanvas));
                ApplyAssetGraphCanvasStyle();
                ed::SetCurrentEditor(nullptr);
                state.graphLayoutDirty = true;
            }
            ed::SetCurrentEditor(static_cast<ed::EditorContext*>(state.graphCanvas));

            // ---- 3. The backdrop, before ed::Begin ------------------------
            // Exactly DrawCanvasBackdrop's shape
            // (ShaderEditorDocument.cpp:5281): the canvas rect is measured
            // HERE because this is the one place per frame that holds it
            // BEFORE ed::Begin, which is where ScreenToCanvas still means
            // what it says -- inside Begin/End the editor moves ImGui itself
            // into canvas space.
            const ImVec2 canvasMin  = ImGui::GetCursorScreenPos();
            const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
            if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
            {
                ed::SetCurrentEditor(nullptr);
                return;
            }

            {
                GraphGridView view;
                view.width  = static_cast<std::uint32_t>(canvasSize.x);
                view.height = static_cast<std::uint32_t>(canvasSize.y);
                view.scale  = GraphViewScale();   // owns the reciprocal flip
                const ImVec2 originCanvas = ed::ScreenToCanvas(canvasMin);
                view.originX = originCanvas.x;
                view.originY = originCanvas.y;

                GraphGridColors colors;
                const auto fill = [](float (&dst)[4], const ImVec4& c)
                { dst[0] = c.x; dst[1] = c.y; dst[2] = c.z; dst[3] = c.w; };
                fill(colors.canvas, kGraphCanvasColor);
                fill(colors.minor,  kGraphGridMinorColor);
                fill(colors.major,  kGraphGridMajorColor);

                DrawGraphGridFallback(ImGui::GetWindowDrawList(), canvasMin, canvasSize,
                                      view, colors, state.graphGrid);
            }

            if (state.graph.nodes.empty())
            {
                // Still a live canvas (it pans and zooms) -- just an empty
                // one. Drawn into the window's own draw list, on top of the
                // backdrop and before ed::Begin, so it stays in SCREEN space
                // and does not scale away with the view.
                const char* msg = state.graphFocus.IsValid()
                                      ? "nothing references, and nothing is referenced by, the focused asset"
                                      : "no assets to graph";
                const ImVec2 size = ImGui::CalcTextSize(msg);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(canvasMin.x + (canvasSize.x - size.x) * 0.5f,
                           canvasMin.y + (canvasSize.y - size.y) * 0.5f),
                    ImGui::GetColorU32(Theme::kTextDim), msg);
            }

            ed::Begin("##assetgraphcanvas", ImVec2(0.0f, canvasSize.y));

            const std::vector<GraphNode>& nodes = state.graph.nodes;
            const std::vector<GraphEdge>& edges = state.graph.edges;

            // Read here rather than at its point of use in section 4 (below)
            // because the id-resolution guard needs it: nothing between
            // ed::Begin and there writes `graphLayoutDirty`, so this is the
            // same value section 4 always saw. See section 4 for what it MEANS.
            const bool applyLayout = state.graphLayoutDirty;

            // ---- Task 4: the node under the cursor ------------------------
            // Latched by the PREVIOUS frame's ed::End
            // (imgui_node_editor.cpp:1278) -- and zeroed there whenever a
            // canvas action is running (`m_CurrentAction == nullptr`), so
            // neither the highlight below nor the tooltip after ed::End
            // flickers while the view is being panned or a node dragged. One
            // frame of lag on a hover is imperceptible; reading it HERE (and
            // once) is what lets the edge pass below act on the same answer
            // the tooltip does.
            const std::uint64_t hoveredNodeId = ed::GetHoveredNode().Get();

            // A node id -> the projection index it names, or `nodes.size()`
            // for "none" (id 0) and for a stale id left over from an earlier
            // build (ids are index+1 into the CURRENT build's node vector).
            //
            // THE STALENESS GUARD, AND WHY IT LIVES HERE (fix round 1). Every
            // id fed to this function was latched by the PREVIOUS frame's
            // ed::End -- the hovered node, the double-clicked node, the
            // context-menu node. On a rebuild frame those name the OLD node
            // vector, and an old id that happens to be in range for the new
            // one resolves to whatever asset now occupies that index: a
            // different document opened, the unified menu raised about (and
            // selecting) the wrong asset, the wrong peek rendered. Bounds
            // checking cannot catch that -- only knowing the ids are from a
            // previous generation can. So a rebuild frame resolves NOTHING,
            // once, at the single point where a latched id becomes an index,
            // rather than at each of the four call sites where the next one
            // added would forget. The cost is one frame of inert
            // hover/open/menu, which is exactly the (safe) shape of the
            // in-flight click 8c already drops for the same reason.
            const auto nodeIndexOf = [&nodes, applyLayout](std::uint64_t id) -> std::size_t
            {
                if (applyLayout)
                    return nodes.size();
                return (id >= 1ull && id <= nodes.size())
                           ? static_cast<std::size_t>(id - 1ull)
                           : nodes.size();
            };
            // The guid a node id names for VISUAL purposes -- any non-overflow
            // node, tombstones included (a ghost's wires are exactly the "who
            // still points at this dead guid" answer a highlight is for).
            // Overflow companions are excluded because their guid ALIASES
            // their anchor's by construction (AssetGraphViewModel.hpp), so
            // lighting up "their" edges would light the ANCHOR's -- a lie
            // about what is under the cursor.
            const auto visualGuidOf = [&](std::uint64_t id) -> Arcane::Guid
            {
                const std::size_t i = nodeIndexOf(id);
                if (i == nodes.size() || nodes[i].isOverflow)
                    return Arcane::Guid{};
                return nodes[i].guid;
            };
            // ...and the entry a node id names for ACTING purposes: select,
            // peek, open, context menu. Stricter than visualGuidOf on both
            // synthetic node kinds, and this is the single place that
            // decision is spelled:
            //   * OVERFLOW ("+N more") nodes are INERT in v1 (controller
            //     ruling). Their guid aliases the anchor's, so a bridge off
            //     one would silently select/open/menu the ANCHOR -- and
            //     "expand this +N more" is deliberately unspecified design
            //     territory, not something to invent from a draw path.
            //   * TOMBSTONES have no AssetPanelEntry by definition, so there
            //     is nothing for the entry-keyed menu or the open routing to
            //     act on, and nothing the peek tooltip's spec-s8 anatomy
            //     (thumb, kind pill, mount path, cook state) could honestly
            //     render. They stay interaction-inert too, selection
            //     included: `model.selected` is the ONE guid every lens and
            //     the Inspector point at, and aiming it at a guid no file
            //     backs would leave all of them pointing at nothing.
            const auto entryForNodeId = [&](std::uint64_t id) -> const AssetPanelEntry*
            {
                const Arcane::Guid g = visualGuidOf(id);
                return g.IsValid() ? model.Find(g) : nullptr;
            };

            // Real (non-overflow) guid -> node index. A tombstone counts as
            // real here: ruling 11 wants a dangling reference's edge to have
            // pixels, and `edges` references tombstones exactly like any
            // other node (AssetGraphViewModel.hpp's own `edges` comment).
            std::unordered_map<Arcane::Guid, std::size_t> indexOfGuid;
            indexOfGuid.reserve(nodes.size());
            for (std::size_t i = 0; i < nodes.size(); ++i)
                if (!nodes[i].isOverflow)
                    indexOfGuid.emplace(nodes[i].guid, i);

            // Which sides actually carry a drawn edge -- what decides both
            // whether a pin exists at all and whether its dot is filled.
            std::vector<GraphNodeVisual> visuals(nodes.size());
            for (const GraphEdge& e : edges)
            {
                const auto from = indexOfGuid.find(e.from);
                const auto to   = indexOfGuid.find(e.to);
                if (from == indexOfGuid.end() || to == indexOfGuid.end())
                    continue;
                // `from` is the referencer: the edge leaves its OUTBOUND
                // (left) side. `to` is the target: it arrives on that node's
                // INBOUND (right) side.
                visuals[from->second].hasLeftPin   = true;
                visuals[from->second].leftConnected = true;
                visuals[to->second].hasRightPin    = true;
                visuals[to->second].rightConnected = true;
            }
            // A "+N more" companion means the anchor has undrawn connections
            // on that side, so the anchor keeps the pin even when no drawn
            // edge uses it -- hollow, because nothing is attached to it.
            for (const GraphNode& n : nodes)
            {
                if (!n.isOverflow)
                    continue;
                const auto anchor = indexOfGuid.find(n.guid);
                if (anchor == indexOfGuid.end())
                    continue;
                if (n.overflowInbound) visuals[anchor->second].hasRightPin = true;
                else                   visuals[anchor->second].hasLeftPin  = true;
            }
            // THE DERIVE AFFORDANCE (Task 6). A material's RIGHT (dependents)
            // pin is the handle the pin-drag gesture starts from, so a
            // material carries that pin even when nothing is attached to it --
            // which is exactly the material you most want to derive a first
            // instance from, and exactly what the board draws: OptionD's (and
            // Demo's) `reference_mesh` is a MATERIAL with no wires at all and
            // a single right-hand pin, wearing the drag's amber glow. Without
            // this the arc's signature gesture would be unreachable on any
            // material nothing references yet.
            //
            // The pin stays HOLLOW while nothing is attached (DrawGraphPinDot's
            // filled-vs-ring rule, unchanged) -- the board paints it solid, but
            // it paints it mid-drag; the ring/fill distinction is this lens's
            // own shipped language and one unconnected pin is not a reason to
            // drop it. Noted for the desk pass.
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].isOverflow || nodes[i].isTombstone)
                    continue;
                if (const AssetPanelEntry* e = model.Find(nodes[i].guid))
                    if (e->kind == AssetKind::Material)
                        visuals[i].hasRightPin = true;
            }

            const Arcane::Guid bootGuid = BootSceneGuid(project);
            const float nodeHeight = GraphNodeHeight();

            // ---- 4. Positions -- written on REBUILD, never per frame ------
            // Ruling 2: the computed layout is authoritative at every
            // rebuild, and the library's own node dragging stays enabled in
            // between. A reposition is therefore TRANSIENT BY DESIGN -- the
            // next rebuild snaps it back. That is intended behavior, not a
            // bug: spec §10 pins the layout as computed each build and not
            // persisted, so there is nowhere for a drag to live.
            //
            // `applyLayout` is read up at the id-resolution lambdas, which
            // need the same answer -- a frame that re-writes every node
            // position is exactly a frame whose incoming node ids are from
            // the previous generation.

            // ---- 5. Nodes -------------------------------------------------
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                const GraphNode& n = nodes[i];
                const std::uint64_t nodeId = GraphNodeIdOf(i);
                GraphNodeVisual& v = visuals[i];

                const AssetPanelEntry* entry = n.isOverflow ? nullptr : model.Find(n.guid);

                // Body content + the width it wants.
                GraphNodeBody body;
                const char* headerIcon = nullptr;
                const char* headerPill = nullptr;
                std::string headerLabel = n.label;

                if (n.isOverflow)
                {
                    // Ruling 6's "+N more": no pins, no accent, no thumb --
                    // it is not an asset, it is a count of connections this
                    // node's own breadth cap did not draw.
                    headerIcon = ICON_LC_ELLIPSIS;
                    body.meta  = n.overflowInbound ? "referencers not shown" : "references not shown";
                }
                else if (n.isTombstone)
                {
                    // Ruling 11: a dangling target finally has pixels -- a
                    // ghost node whose name is the short guid the view model
                    // already chose for it, wearing the amber attention pill.
                    headerIcon = ICON_LC_FILE_QUESTION;
                    body.pills.push_back({ "missing", 1 });
                }
                else if (entry)
                {
                    headerIcon = KindIcon(entry->kind);
                    headerLabel = entry->fileName;
                    body.thumb = services.resolveAssetThumb ? services.resolveAssetThumb(n.guid) : 0;
                    body.icon  = KindIcon(entry->kind);
                    // The same pill vocabulary the Browse rows use, in the
                    // same spec order -- subkind, inst, sliced -- so one
                    // asset reads identically in both lenses. "boot" moves to
                    // the header, where the board puts it.
                    if (const char* sub = SubkindPillText(*entry))
                        body.pills.push_back({ sub, 0 });
                    if (entry->isInstance)
                        body.pills.push_back({ "inst", 0 });
                    if (entry->kind == AssetKind::Sprite && entry->sliced)
                        body.pills.push_back({ "sliced", 0 });
                    if (body.pills.empty())
                        body.meta = KindLabel(entry->kind);
                    if (entry->kind == AssetKind::Scene && bootGuid.IsValid() && n.guid == bootGuid)
                        headerPill = "boot";
                }
                else
                {
                    headerIcon = KindIcon(n.kind);
                    body.icon  = KindIcon(n.kind);
                }

                // Width: what the content wants, clamped into §11.2's band.
                float wantHeader = kGraphNodePadLeft + kGraphNodePadRight;
                {
                    ImGui::PushFont(GetEditorFonts().interRegular, kGraphHeaderFontPx);
                    if (headerIcon)
                        wantHeader += ImGui::CalcTextSize(headerIcon).x + kGraphNodeIconGap;
                    wantHeader += ImGui::CalcTextSize(headerLabel.c_str()).x;
                    ImGui::PopFont();
                    if (headerPill)
                        wantHeader += kGraphNodeIconGap + PillWidth(headerPill);
                }
                float wantBody = kGraphNodePadLeft + kGraphNodePadRight;
                if (body.thumb != 0 || body.icon)
                    wantBody += kAssetRowThumbSize + ImGui::GetStyle().ItemInnerSpacing.x;
                if (!body.pills.empty())
                {
                    bool first = true;
                    for (const auto& [text, variant] : body.pills)
                    {
                        (void)variant;
                        wantBody += (first ? 0.0f : ImGui::GetStyle().ItemSpacing.x) + PillWidth(text);
                        first = false;
                    }
                }
                else if (!body.meta.empty())
                {
                    ImGui::PushFont(GetEditorFonts().interRegular, kGraphMetaFontPx);
                    wantBody += ImGui::CalcTextSize(body.meta.c_str()).x;
                    ImGui::PopFont();
                }

                v.width  = std::clamp((std::max)(wantHeader, wantBody),
                                      kGraphNodeMinWidth, kGraphNodeMaxWidth);
                v.height = nodeHeight;
                v.pos    = ImVec2(static_cast<float>(n.layer) * kGraphColumnPitch,
                                  static_cast<float>(n.row)   * kGraphRowPitch);
                if (n.isOverflow)
                {
                    v.hasLeftPin = v.hasRightPin = false;
                    v.leftConnected = v.rightConnected = false;
                }

                if (applyLayout)
                {
                    ed::SetNodePosition(ed::NodeId(nodeId), v.pos);
                }
                else
                {
                    // The node may have been dragged since the last rebuild
                    // (transient, but it has to draw where it IS). An id the
                    // editor has never seen answers (FLT_MAX, FLT_MAX), which
                    // would fling the node off the canvas -- fall back to the
                    // computed layout for it instead.
                    const ImVec2 live = ed::GetNodePosition(ed::NodeId(nodeId));
                    if (live.x < FLT_MAX * 0.5f && live.y < FLT_MAX * 0.5f)
                        v.pos = live;
                    else
                        ed::SetNodePosition(ed::NodeId(nodeId), v.pos);
                }

                // A tombstone wears the editor's amber attention language
                // rather than a kind accent it does not have -- ruling 11's
                // "kAmber border accent", applied to the bar and the border
                // alike (its AssetKind is Other by construction, so the §11.3
                // table has nothing to say about it either way).
                const ImVec4 amber  = Theme::kAmber;
                const ImVec4 accent = n.isTombstone ? amber : KindAccentColor(n.kind);
                const bool ghost = n.isOverflow || n.isTombstone;
                DrawGraphNode(nodeId, v, body, headerIcon, headerLabel, headerPill,
                              accent, ghost);

                // Chrome, after EndNode -- see DrawGraphNodeChrome.
                DrawGraphNodeChrome(nodeId, v,
                                    /*drawBand=*/!n.isOverflow,
                                    /*accent=*/n.isOverflow ? nullptr : &accent,
                                    /*wash=*/ghost ? kGraphGhostWash : 0.0f,
                                    /*borderAccent=*/n.isTombstone ? &amber : nullptr);
            }
            state.graphLayoutDirty = false;

            // ---- 6. Edges -------------------------------------------------
            // The two-layer trick (ruling 7): a FULLY TRANSPARENT ed::Link
            // carries hit-testing, selection, rect-select and the delete flow
            // (alpha 0 costs nothing -- the library's draw helper returns
            // immediately on it, and registration ignores colour entirely),
            // while the visible curve is drawn by hand into the links
            // channel. That is what per-kind colour, mid-edge labels and
            // selection brightening need; the library's flat uniform links
            // can do none of them. ShaderEditorDocument.cpp:311-357 is the
            // long form of every clause in this paragraph.
            const float viewScale = GraphViewScale();
            const bool  drawLabels = viewScale > kGraphLabelMinScale;
            // Read-only: `selected` is a plain public member of the model, so
            // brightening needs no interaction plumbing at all. The rest of
            // the selection story -- clicking a node, centering on an
            // external change -- is section 8 below.
            const Arcane::Guid& selectedGuid = model.selected;
            // Task 4 / Interactions-FINAL: "the node's own edges brighten, the
            // rest stay dim" on HOVER as well, through this same one mechanism.
            const Arcane::Guid hoveredGuid = visualGuidOf(hoveredNodeId);

            for (std::size_t ei = 0; ei < edges.size(); ++ei)
            {
                const GraphEdge& e = edges[ei];
                const auto from = indexOfGuid.find(e.from);
                const auto to   = indexOfGuid.find(e.to);
                if (from == indexOfGuid.end() || to == indexOfGuid.end())
                    continue;

                const GraphNodeVisual& fv = visuals[from->second];
                const GraphNodeVisual& tv = visuals[to->second];

                // The TARGET sits in the lower column, so its right-hand
                // (inbound) pin starts the wire and the REFERENCER's
                // left-hand (outbound) pin ends it -- see GraphLeftPinId.
                const std::uint64_t startPin = GraphRightPinId(GraphNodeIdOf(to->second));
                const std::uint64_t endPin   = GraphLeftPinId(GraphNodeIdOf(from->second));
                ed::Link(ed::LinkId(ei + 1), ed::PinId(startPin), ed::PinId(endPin),
                         ImVec4(0.0f, 0.0f, 0.0f, 0.0f), kGraphWireThickness);

                const ImVec2 p0(tv.pos.x + tv.width, tv.pos.y + tv.height * 0.5f);
                const ImVec2 p3(fv.pos.x,            fv.pos.y + fv.height * 0.5f);

                // Colour = the SOURCE kind's accent, dimmed; brightened when
                // either endpoint is the selected asset (spec §10: "selected
                // node's edges brighten") or the hovered one (Task 4).
                const bool emphasize = (selectedGuid.IsValid() &&
                                        (e.from == selectedGuid || e.to == selectedGuid)) ||
                                       (hoveredGuid.IsValid() &&
                                        (e.from == hoveredGuid || e.to == hoveredGuid));
                const ImVec4 base = KindAccentColor(nodes[from->second].kind);
                const ImVec4 col  = emphasize ? GraphBrightenColor(base)
                                              : GraphDimColor(base, kGraphWireDim);
                const ImVec2 mid = DrawGraphWire(p0, p3, col, kGraphWireThickness);

                if (drawLabels && e.label)
                {
                    // Ruling 9's mid-edge label, 12px and dim, on the small
                    // plate the board gives it so the wire does not run
                    // through the glyphs.
                    ImGui::PushFont(GetEditorFonts().interRegular, kGraphLabelFontPx);
                    const ImVec2 size = ImGui::CalcTextSize(e.label);
                    const ImVec2 tl(mid.x - size.x * 0.5f, mid.y - size.y * 0.5f);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (dl->_Splitter._Count > kGraphLinkChannel)
                    {
                        const int prevChannel = dl->_Splitter._Current;
                        dl->ChannelsSetCurrent(kGraphLinkChannel);
                        dl->AddRectFilled(ImVec2(tl.x - 3.0f, tl.y), ImVec2(tl.x + size.x + 3.0f, tl.y + size.y),
                                          ImGui::GetColorU32(kGraphCanvasColor));
                        dl->AddText(tl, ImGui::GetColorU32(Theme::kTextDim), e.label);
                        dl->ChannelsSetCurrent(prevChannel);
                    }
                    ImGui::PopFont();
                }
            }

            // ---- 7. Anchor -> "+N more" connectors ------------------------
            // NOT a GraphEdge, and inexpressible as one: an overflow node
            // reuses its anchor's guid, and GraphEdge is guid-keyed, so
            // anchor -> companion would be a self-edge. The connector is
            // therefore synthesized here from `isOverflow` /
            // `overflowInbound` / the shared guid, drawn dim and thin so it
            // reads as "and more that way" rather than as a reference the
            // index actually holds. No ed::Link either -- there is nothing to
            // select, hover or delete.
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                const GraphNode& n = nodes[i];
                if (!n.isOverflow)
                    continue;
                const auto anchor = indexOfGuid.find(n.guid);
                if (anchor == indexOfGuid.end())
                    continue;
                const GraphNodeVisual& av = visuals[anchor->second];
                const GraphNodeVisual& ov = visuals[i];
                const ImVec4 col = GraphDimColor(Theme::kGrab, kGraphOverflowWireDim);
                if (n.overflowInbound)
                    // Truncated on the anchor's INBOUND side: the companion
                    // stacks one column to the RIGHT.
                    DrawGraphWire(ImVec2(av.pos.x + av.width, av.pos.y + av.height * 0.5f),
                                  ImVec2(ov.pos.x,            ov.pos.y + ov.height * 0.5f),
                                  col, kGraphOverflowWireThickness);
                else
                    // Truncated on the anchor's OUTBOUND side: one column to
                    // the LEFT. (When the anchor is already in column 0 the
                    // view model clamps the companion into the SAME column,
                    // and this connector doubles back on itself -- a layout
                    // fact of the projection, drawn honestly rather than
                    // hidden.)
                    DrawGraphWire(ImVec2(ov.pos.x + ov.width, ov.pos.y + ov.height * 0.5f),
                                  ImVec2(av.pos.x,            av.pos.y + av.height * 0.5f),
                                  col, kGraphOverflowWireThickness);
            }

            // ---- 7b. The pin-drag create query (Task 6) -------------------
            // THE ARC'S SIGNATURE GESTURE: drag off a material's DEPENDENTS
            // pin, release over empty canvas, get a ghost menu whose one entry
            // opens the Create dialog already parented to that material.
            //
            // WHICH PIN, geometrically (the vocabulary hazard, settled by the
            // board): a node's RIGHT pin is its inbound/referencers side --
            // wires EXIT right pins toward the assets that depend on this one
            // (see GraphLeftPinId's block). Deriving an instance MAKES a new
            // dependent, so the gesture is the material's RIGHT pin. Both
            // boards agree outright: OptionD/Demo's `reference_mesh` material
            // carries one pin at `right: -5px` with a
            // `box-shadow: 0 0 0 3px rgba(255,166,26,0.35)` amber glow, and
            // the dashed drag path leaves exactly that point.
            //
            // The gesture SPANS FRAMES (drag ... release ... popup open for as
            // long as the user leaves it up), so the only thing stashed is a
            // GUID -- never a pin or node id, which are index-derived and
            // renumber on every rebuild. A rebuild landing mid-drag cancels
            // the gesture: `nodeIndexOf` refuses every id on an applyLayout
            // frame, the query is rejected, and nothing is stashed.
            //
            // Three hops, copied in SHAPE from the shader editor's own
            // (ShaderEditorDocument.cpp:5510-5562 and :4019-4038): query and
            // accept HERE, inside the canvas; stash; open the popup in a
            // Suspend block (8e below), because a popup lives in screen space.
            bool wireCreateRequest = false;

            // A PIN id -> the projection index of the node that owns it, or
            // `nodes.size()`. Routed through nodeIndexOf so it inherits the
            // rebuild-staleness refusal in one place rather than re-deriving
            // it. Pin ids are nodeId*4 + {1,2} and node ids start at 1, so the
            // smallest legal pin id is 5.
            const auto pinNodeIndex = [&](std::uint64_t pinId) -> std::size_t
            {
                if (pinId < GraphLeftPinId(1ull))
                    return nodes.size();
                return nodeIndexOf(pinId / 4ull);
            };
            const auto isRightPin = [](std::uint64_t pinId)
            { return pinId >= GraphLeftPinId(1ull) && (pinId % 4ull) == 2ull; };
            // Remember WHICH asset the live drag is leaving, by guid. Silent on
            // a rebuild frame (nothing resolvable) and on a synthetic node --
            // in both cases the previous answer stands, which is right: the
            // drag did not change, only our ability to name it this frame.
            const auto noteDragSource = [&](std::uint64_t pinId)
            {
                const std::size_t i = pinNodeIndex(pinId);
                if (i == nodes.size() || nodes[i].isOverflow)
                    return;
                state.graphDragGuid  = nodes[i].guid;
                state.graphDragRight = isRightPin(pinId);
            };

            // The colour handed to BeginCreate is the one the LIBRARY would
            // paint its own candidate link with -- fully transparent here, the
            // same two-layer trick section 6 uses for real edges, because the
            // visible in-flight curve is the hand-drawn dashed one below.
            //
            // The return value is also the honest "is a drag live at all"
            // answer: CreateItemAction reports true for every frame of a drag
            // (stage Possible) and for the release frame (stage Create), and
            // false once it is over -- which is what retires the curve.
            if (ed::BeginCreate(ImVec4(0.0f, 0.0f, 0.0f, 0.0f), kGraphWireThickness))
            {
                ed::PinId aId, bId;
                if (ed::QueryNewLink(&aId, &bId))
                {
                    // Dragged onto another PIN. This lens never AUTHORS a
                    // reference -- the graph is a projection of the reference
                    // index, and a reference is made by editing an asset, not
                    // by dragging a wire -- so the link is refused outright.
                    // `aId` is always the DRAGGED pin (DragStart fills
                    // m_LinkStart; DropPin only ever fills m_LinkEnd).
                    noteDragSource(aId.Get());
                    ed::RejectNewItem();
                }
                else if (ed::QueryNewNode(&aId))
                {
                    // Dragged over EMPTY canvas. True on every frame of the
                    // drag; AcceptNewItem returns true only on the RELEASE
                    // frame (CreateItemAction::AcceptItem answers True only in
                    // the Create stage), which is the one frame that stashes.
                    noteDragSource(aId.Get());
                    const std::size_t i = pinNodeIndex(aId.Get());
                    if (i == nodes.size() || nodes[i].isOverflow)
                    {
                        // Nothing to derive FROM: an overflow companion's guid
                        // aliases its anchor's, and an id nodeIndexOf refused
                        // is either out of range or from a previous build. A
                        // rebuild that lands exactly on the release frame
                        // therefore CANCELS the gesture rather than deriving
                        // from a stranger.
                        ed::RejectNewItem();
                    }
                    else if (ed::AcceptNewItem())
                    {
                        const AssetPanelEntry* src = model.Find(nodes[i].guid);
                        state.graphWireGuid = nodes[i].guid;
                        // Derivable = a live MATERIAL, dragged off its
                        // DEPENDENTS pin. A tombstone has no entry, so it
                        // fails this by construction.
                        state.graphWireDerivable = isRightPin(aId.Get()) && src &&
                                                   src->kind == AssetKind::Material;
                        wireCreateRequest = true;
                    }
                }
                // NO `else`: the pointer is over a NODE BODY, where the library
                // reports neither query. The drag is still live and
                // `graphDragGuid` still names its source, which is exactly why
                // that source is session state -- see its declaration.
            }
            else
            {
                // No create action at all: whatever drag there was is over
                // (released, cancelled, or consumed by the accept above one
                // frame ago). Retire the curve.
                state.graphDragGuid  = Arcane::Guid{};
                state.graphDragRight = false;
            }
            // UNCONDITIONAL -- see this function's header comment and
            // ShaderEditorDocument.cpp:5559-5561. Nothing between BeginCreate
            // and here returns, breaks or throws: the block above is a plain
            // if/else-if chain over library calls.
            ed::EndCreate();

            // The in-flight curve, drawn AFTER EndCreate so it is back in
            // CANVAS space: QueryNewLink/QueryNewNode suspend the editor into
            // global (screen) space to answer, and CreateItemAction::End
            // resumes it. Which also means ImGui's mouse position is the
            // canvas-space one again out here -- the same space the pin pivots
            // below are in.
            //
            // The SOURCE is re-resolved from its guid through THIS build's
            // index map, so a rebuild mid-drag re-anchors the curve on the
            // node's new position instead of aiming it at whatever now sits at
            // an old index -- and a source that the rebuild dropped entirely
            // simply stops drawing.
            if (state.graphDragGuid.IsValid())
            {
                if (const auto it = indexOfGuid.find(state.graphDragGuid);
                    it != indexOfGuid.end())
                {
                    const GraphNodeVisual& wv = visuals[it->second];
                    const bool right = state.graphDragRight;
                    const ImVec2 pivot(right ? wv.pos.x + wv.width : wv.pos.x,
                                       wv.pos.y + wv.height * 0.5f);
                    const ImVec2 tip = ImGui::GetMousePos();
                    // Orientation matters: GraphWireControlPoints assumes p0
                    // leaves rightward and p3 arrives leftward (the style's
                    // SourceDirection/TargetDirection). A right-pin drag LEAVES
                    // the pin; a left-pin drag ARRIVES at it. The board's path
                    // (`M230,330 C320,330 390,402 462,402`) is the former.
                    DrawGraphDashedWire(right ? pivot : tip, right ? tip : pivot,
                                        Theme::kAmber, kGraphWireThickness, viewScale);
                    DrawGraphWireEndDot(tip, Theme::kAmber);
                }
            }

            // ---- 8. Interactions (Task 4) ---------------------------------
            // THE SELECTION AUTHORITY IS THE MODEL. The canvas keeps its own
            // selection set (it has to -- it draws the 2px selected border and
            // owns rect-select), but that set is never the truth: it is a
            // MIRROR the steps below re-establish every frame, in a fixed
            // order chosen so neither direction can read back its own write.
            //
            //   8a rebuild guard  : node ids are index+1 into the CURRENT
            //                       build, so a rebuild renumbers everything
            //                       and the mirror now names strangers. Drop
            //                       it and re-derive it from the model.
            //   8b model -> canvas: a stamp nobody here acknowledged came from
            //                       somewhere else (a Browse row, the
            //                       Inspector, another lens). Point the mirror
            //                       at it and center ONCE -- and when the guid
            //                       has no node in this scope, CLEAR the
            //                       mirror rather than leave it pointing at
            //                       the previous asset. Acknowledge either
            //                       way, or the stamp re-arms forever.
            //   8c canvas -> model: only now, with the mirror known to agree
            //                       with the model, is a DISAGREEMENT
            //                       necessarily the user's own click. Push it
            //                       into the model and acknowledge the stamp
            //                       it raises in the same statement -- the
            //                       node is under the cursor already and must
            //                       not then be yanked to the middle of the
            //                       view by 8b on the next frame.
            //
            // The order is load-bearing and was caught by the device-less test
            // rather than reasoned out: with 8c first, a model selection that
            // is OUT of this scope leaves the mirror holding the previous
            // in-scope node, and 8c reads that stale mirror back over the
            // model -- the canvas silently out-voting the authority.
            //
            // Everything read here (the selection set, double-click, the
            // context-menu gesture) was decided by the PREVIOUS frame's
            // ed::End, which is where the library processes its actions. Same
            // position, same one-frame lag, as the shader editor's own read of
            // GetDoubleClickedNode (ShaderEditorDocument.cpp:3125-3129); the
            // alternative, reading after ed::End, cannot then call back INTO
            // the canvas at all.

            // 8a. Rebuild renumber guard.
            if (applyLayout)
            {
                ed::ClearSelection();
                if (model.selected.IsValid())
                {
                    const auto it = indexOfGuid.find(model.selected);
                    if (it != indexOfGuid.end())
                        ed::SelectNode(ed::NodeId(GraphNodeIdOf(it->second)));
                }
                // No navigation: the view did not RECEIVE a new selection, it
                // is the same one wearing a new id.
            }

            // 8b. Model -> canvas: center once on an externally-changed
            // selection (Browse's own idiom, `wantsScroll` at DrawTable).
            if (state.seenSelectionStampGraph != model.selectionStamp)
            {
                const auto it = model.selected.IsValid() ? indexOfGuid.find(model.selected)
                                                         : indexOfGuid.end();
                if (it != indexOfGuid.end())
                {
                    ed::SelectNode(ed::NodeId(GraphNodeIdOf(it->second)));
                    // Default zoomIn=false: NavigateTo's ZoomMode::None
                    // centers at the CURRENT scale
                    // (imgui_node_editor.cpp:3524-3532). Interactions-FINAL
                    // says "the graph centers it" -- fitting one ~110x66 node
                    // to the whole canvas instead would be a zoom nobody asked
                    // for.
                    ed::NavigateToSelection();
                }
                else
                {
                    // Selected out of this scope (filtered by the focus, a
                    // tombstone, never an asset at all): nothing to center on,
                    // and the mirror must stop claiming the PREVIOUS node is
                    // the selection -- see the ordering note above.
                    ed::ClearSelection();
                }
                state.seenSelectionStampGraph = model.selectionStamp;
            }

            // 8c. Canvas -> model. Skipped on a rebuild frame: 8a just wrote
            // that mirror itself, so there is nothing of the user's in it --
            // and the ids in it would be the previous build's anyway, which
            // nodeIndexOf now refuses centrally. The explicit test stays
            // because it also short-circuits the canvas query.
            if (!applyLayout && ed::GetSelectedObjectCount() == 1)
            {
                // Exactly one object, and it has to be a NODE (a selected link
                // makes GetSelectedNodes return 0). A rect-select of several
                // nodes leaves the model alone on purpose: `model.selected` is
                // one guid, and silently picking one of N would be a guess.
                // Clicking empty canvas clears the CANVAS selection only --
                // deselecting inside one lens does not clear the selection
                // every other lens and the Inspector are pointing at, exactly
                // as clicking below the last Browse row does not.
                ed::NodeId picked;
                if (ed::GetSelectedNodes(&picked, 1) == 1)
                    if (const AssetPanelEntry* e = entryForNodeId(picked.Get()))
                        if (e->guid != model.selected)
                        {
                            model.Select(e->guid);
                            state.seenSelectionStampGraph = model.selectionStamp;
                        }
            }

            // 8d. Double-click opens, routed EXACTLY as a Browse row's is
            // (spec §6's verbatim-behavior clause): a scene comes back through
            // actions.openScene for the host's unsaved-changes guard, every
            // other kind opens through the DocumentHost. Overflow companions,
            // tombstones AND a rebuild frame's previous-generation ids are all
            // filtered by entryForNodeId -- the last of those matters most
            // here, since acting on a renumbered id would open a document the
            // user never double-clicked.
            if (const AssetPanelEntry* e = entryForNodeId(ed::GetDoubleClickedNode().Get()))
                OpenAssetRow(*e, project, docs, actions);

            // 8e. Right-click -> the unified asset context menu, the SAME
            // items a Browse row raises (DrawAssetMenuItems is the one copy).
            // `menuOpen` is read by the tooltip after ed::End -- see there.
            bool menuOpen = false;
            {
                const CanvasPopupScope canvasPopup;   // ed::Suspend/Resume, see the header
                ed::NodeId ctxNode;
                if (ed::ShowNodeContextMenu(&ctxNode))
                {
                    if (const AssetPanelEntry* e = entryForNodeId(ctxNode.Get()))
                    {
                        state.graphMenuGuid = e->guid;
                        // Right-click acts on this node -- DrawRowContextMenu's
                        // own first statement, for the same reason. The MIRROR
                        // moves with it: the context-menu action does not touch
                        // the canvas selection itself, so without this the next
                        // frame's 8c would read the old node back over the
                        // model and revert the right-click's selection.
                        // Acknowledged like 8c's click -- the node is under the
                        // cursor already, nothing to center.
                        ed::ClearSelection();
                        ed::SelectNode(ctxNode);
                        model.Select(e->guid);
                        state.seenSelectionStampGraph = model.selectionStamp;
                        ImGui::OpenPopup(kGraphNodeMenuId);
                    }
                    // No `else`: an overflow companion, a tombstone, or a
                    // rebuild frame's previous-generation id raises no menu at
                    // all rather than one about the wrong asset.
                }
                // Once OPEN the popup is already generation-proof: it re-reads
                // `state.graphMenuGuid`, a guid, never the id it came from.
                if (ImGui::BeginPopup(kGraphNodeMenuId))
                {
                    menuOpen = true;
                    if (const AssetPanelEntry* e = model.Find(state.graphMenuGuid))
                        DrawAssetMenuItems(actions, *e, /*kindSpecific=*/true);
                    else
                        // The asset went away underneath an open menu (deleted
                        // on disk, or a rebuild dropped it): close rather than
                        // draw a menu about nothing.
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                // 8f. Task 6's ghost menu, opened in this SAME Suspend
                // bracket the shader editor opens its own wire-create popup
                // in (ShaderEditorDocument.cpp:4019-4038 -- one bracket, both
                // popups). ImGui records the popup's position from the mouse
                // AT OpenPopup TIME, and out here that is the SCREEN mouse,
                // which is why the menu lands at the drag's release point (the
                // board's `left: 474px; top: 380px` beside the curve's
                // `462,402` end) with no explicit placement call.
                if (wireCreateRequest)
                    ImGui::OpenPopup(kGraphCreateMenuId);
                if (ImGui::BeginPopup(kGraphCreateMenuId))
                {
                    menuOpen = true;
                    // Generation-proof for the same reason the node menu is:
                    // it re-reads a stashed GUID, never the pin id it came
                    // from. If the asset went away underneath an open menu
                    // (deleted on disk, a rebuild dropped it) the entry simply
                    // goes dead rather than promising a parent that is gone.
                    //
                    // DISABLED, not CLOSED -- deliberately unlike 8e's node
                    // menu. That one draws a whole list of per-asset actions
                    // that would all be meaningless, so closing is the honest
                    // answer; this one has a single entry whose disabled state
                    // already says exactly that. It is also the state a
                    // TOMBSTONE source lands in (no entry, by definition), and
                    // "cannot derive from this" reads better there than a menu
                    // that flashes up and vanishes.
                    const AssetPanelEntry* src = model.Find(state.graphWireGuid);
                    ImGui::BeginDisabled(!src || !state.graphWireDerivable);
                    // ONE entry, DISABLED rather than hidden when the drag did
                    // not come off a material's dependents pin: the gesture
                    // stays discoverable everywhere it is possible to make it,
                    // and says plainly that this particular source cannot
                    // answer it. (The board's second entry, "Assign to
                    // selection", is a different feature and not in this
                    // plan's scope -- deliberately not invented here.)
                    if (ImGui::MenuItem(ICON_LC_LAYERS " Derive Instance\xE2\x80\xA6"))
                    {
                        // THE FIRST REAL PRODUCER of the createPrefillParent
                        // limb. Routed through the ONE unified-create request
                        // every other creation path uses (spec §7: "no
                        // creation path may bypass CreateAssetRequest") --
                        // EditorAppFrame.cpp:2328-2334 turns the pair into
                        // BeginCreateAsset({MaterialInstance, parent}), whose
                        // MaterialInstance arm (:2490-2499) lands the guid in
                        // the dialog's `parent` field and leaves the picker
                        // CLOSED because the parent is already known.
                        //
                        // The kind is a CreateAssetKind, per the field's own
                        // contract -- and it is MaterialInstance outright, not
                        // a bridged source kind: CreateKindForAssetKind maps a
                        // material to CreateAssetKind::Material (the thing the
                        // source IS), while this entry creates the thing that
                        // DERIVES from it.
                        actions.requestCreateKind =
                            static_cast<int>(CreateAssetKind::MaterialInstance);
                        actions.createPrefillParent = state.graphWireGuid;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
            }

            ed::End();
            ed::SetCurrentEditor(nullptr);

            // ---- The canvas legend (Task 6, controller rider) -------------
            // AFTER ed::End, so it is chrome in SCREEN space: it does not pan,
            // zoom, or sort against the nodes. See DrawGraphLegend for the
            // board transcription and the two flagged mismatches.
            DrawGraphLegend(canvasMin, canvasSize);

            // ---- 9. The peek tooltip (Task 4, plan ruling 14) -------------
            // WHERE it landed, and why HERE:
            //
            //  * AFTER ed::End, and outside the editor entirely. Inside
            //    ed::Begin/End the editor has moved ImGui into the canvas's
            //    transformed (pan+zoom) space, and a tooltip positions itself
            //    in SCREEN space -- CanvasPopupScope.hpp states the rule. Out
            //    here ImGui is already back in screen space (Canvas::End ->
            //    LeaveLocalSpace, imgui_canvas.cpp), so no Suspend bracket is
            //    needed at all; that the editor is not even current any more
            //    is the proof. This is "do not fight the canvas": the one
            //    thing the tooltip needs from the canvas -- WHICH node is
            //    hovered -- was captured up top, so the drawing needs nothing
            //    else from it.
            //
            //  * `forceShow=true`. ImGui's "last submitted item" at this point
            //    is the canvas's own full-rect Dummy (imgui_canvas.cpp:182),
            //    never the hovered node, so IsItemHovered() would answer a
            //    question about the CANVAS. That is precisely the situation
            //    the parameter was added for in Plan 2 Task 8 (TimelineFeed);
            //    the node editor's own hit test is the honest authority here.
            //
            //  * ...which costs the ForTooltip delay, so the dwell below
            //    replaces it: the same node must stay hovered for
            //    style.HoverStationaryDelay before the peek appears. Without
            //    it a tooltip would flash on every node the pointer crosses on
            //    its way somewhere -- a peek, not a commit. The node editor
            //    already suppresses hover outright while an action is running,
            //    which covers the drag/pan half of spec §8's tooltip rules.
            //
            // Overflow companions, tombstones and a rebuild frame's
            // previous-generation ids get no peek (entryForNodeId), and
            // neither does a node while EITHER canvas popup is up -- its own
            // context menu (8e) or Task 6's ghost create menu (8f): the peek
            // and the menu are two answers to one hover, and ImGui would stack
            // them at the same mouse position. Tracked through `menuOpen`
            // rather than IsPopupOpen because the popup's id was hashed
            // against the ID stack ed::Begin pushes, which is gone by here.
            //
            // The dwell is keyed on the resolved GUID, not on the node id it
            // came from (fix round 1). A guid is generation-independent, so
            // "is this still the same thing I was hovering?" stays a true
            // question across a rebuild -- whereas an id compares numerically
            // equal while the asset behind it changes, which would have
            // silently carried an elapsed dwell onto a different asset.
            const AssetPanelEntry* hovered = menuOpen ? nullptr : entryForNodeId(hoveredNodeId);
            if (hovered)
            {
                if (state.graphHoverGuid != hovered->guid)
                {
                    state.graphHoverGuid    = hovered->guid;
                    state.graphHoverSeconds = 0.0f;
                }
                else
                {
                    state.graphHoverSeconds += ImGui::GetIO().DeltaTime;
                }
                if (state.graphHoverSeconds >= ImGui::GetStyle().HoverStationaryDelay)
                    DrawAssetPeekTooltip(model, services, hovered->guid,
                                         /*forceShow=*/true, /*withEdgeSummary=*/true);
            }
            else if (!applyLayout)
            {
                // Nothing hovered -> drop the dwell. NOT on a rebuild frame
                // though: there the ids are merely unreadable for one frame,
                // which is not evidence the pointer left the node. Holding the
                // dwell is what makes the guid key pay -- the peek pauses for
                // that frame and resumes on the next one instead of making the
                // user wait out the delay again for a node they never left.
                state.graphHoverGuid    = Arcane::Guid{};
                state.graphHoverSeconds = 0.0f;
            }
        }
    }

    bool AssetsGraphProjectionIsCurrent(const AssetsPanelState& state, const AssetPanelModel& model)
    {
        // All three conjuncts earn their place, and each closes a gate the
        // other two leave open:
        //   * graphBuilt      -- the lens was never opened at all, so there is
        //                        no projection behind the numbers.
        //   * graphBuiltFocus -- the focus moved without a Graph body running
        //                        since (Focus in Graph; also the toolbar combo
        //                        on a frame where the lens body early-returns
        //                        on a non-positive canvas region).
        //   * graphBuiltStamp -- the model's entries moved under a frame whose
        //                        body was NOT the Graph lens, so the build is
        //                        about a different set of assets than the
        //                        totals beside it.
        // Exactly the same three inputs DrawGraphLens's own rebuild trigger
        // uses -- by construction this is "would the lens rebuild if it ran
        // right now", asked from outside it.
        return state.graphBuilt
            && state.graphBuiltFocus == state.graphFocus
            && state.graphBuiltStamp == model.entriesStamp;
    }

    void DestroyAssetsPanelCanvas(AssetsPanelState& state)
    {
        if (state.graphCanvas)
        {
            ed::DestroyEditor(static_cast<ed::EditorContext*>(state.graphCanvas));
            state.graphCanvas = nullptr;
        }
        // Everything derived from the context or the outgoing project goes
        // with it: a stale projection would otherwise be re-drawn (against
        // brand-new node ids) on the first frame after a project switch,
        // before the model has rebuilt.
        state.graph.Clear();
        state.graphBuilt = false;
        state.graphBuiltStamp = 0;
        state.graphBuiltFocus = Arcane::Guid{};
        state.graphLayoutDirty = false;
        state.graphFocus = Arcane::Guid{};
        // ...and re-arm the boot-scene seed with it (Task 5): the incoming
        // project has its OWN boot scene, and this is the seam that tells the
        // panel a new one is coming. Clearing the focus without clearing this
        // flag would leave the next project permanently scoped to
        // "everything"; clearing this flag without clearing the focus would
        // leave the outgoing project's scene guid readable for one frame.
        state.graphFocusSeeded = false;
        state.graphGrid = GraphGridPhase{};
        state.seenSelectionStampGraph = 0;
        // Task 4's interaction state is derived from the context and the
        // projection too: a node id (the hover dwell) means nothing once the
        // ids are gone, and a menu guid names an asset of the OUTGOING
        // project. Leaving either behind would let the first frame of the
        // next project answer with the last one's.
        state.graphMenuGuid = Arcane::Guid{};
        state.graphHoverGuid = Arcane::Guid{};
        state.graphHoverSeconds = 0.0f;
        // Task 6's gesture stash goes with them, and for the same reason: it
        // names an asset of the OUTGOING project, and the popup it feeds is
        // closed by the context's destruction anyway.
        state.graphWireGuid = Arcane::Guid{};
        state.graphWireDerivable = false;
        state.graphDragGuid = Arcane::Guid{};
        state.graphDragRight = false;
    }

    AssetsPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                       const Arcane::Project* project, DocumentHost& docs,
                                       const AssetsPanelServices& services,
                                       bool* open)
    {
        AssetsPanelActions actions;
        ImGui::Begin("Assets", open);

        // Plan 3 Task 5: the Graph lens opens scoped to the project's BOOT
        // SCENE, not to "everything" (spec s10 -- the boot scene is the one
        // root every project has, and an everything-mode first view of a real
        // project is a hairball). Seeded HERE, on the first panel frame of a
        // project, rather than at the host's project-open seam, because the
        // panel is the only place that has both the project and the state --
        // and it is seeded ONCE (graphFocusSeeded), so the combo's own
        // "everything" entry stays pickable afterwards. A project-less boot
        // seeds nothing and leaves the flag armed for the first real project;
        // DestroyAssetsPanelCanvas re-arms it on every switch after that.
        if (project && !state.graphFocusSeeded)
        {
            state.graphFocus       = BootSceneGuid(project);
            state.graphFocusSeeded = true;
        }

        DrawToolbar(state, model, actions);

        // 2026-09-07 fix (mock parity): DrawToolbar's own trailing widget is
        // SegmentedStrip (the lens strip), which pushes ItemSpacing to
        // (x,0) for its OWN internal buttons so they sit flush against each
        // other ("collapsed shared borders", EditorWidgets.cpp) and pops it
        // correctly before returning. But ImGui bakes each item's "next
        // line" cursor advance in AT PLACEMENT TIME using whatever
        // ItemSpacing was active THEN -- popping a style var afterward
        // restores the STYLE STRUCT, not a cursor position that already
        // advanced under the zeroed value. So the toolbar's own trailing
        // edge silently inherited that zero too, and the body below sat
        // flush against it with NO gap, live, even though nothing here ever
        // asked for that -- confirmed by an automation pixel-scan of the
        // live capture (0px) against the redline (7px, kToolbarBodyGapPx's
        // own comment). Fix: an EXPLICIT Dummy for the gap, itself wrapped
        // in a zeroed ItemSpacing so nothing implicit adds to either side
        // of it -- deliberately not trusting ImGui's automatic per-item
        // spacing a second time for this exact seam. Vertical-only; the
        // horizontal flush gutters DrawBrowseLens's own SameLine(0,0) chain
        // established are untouched.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, kToolbarBodyGapPx));
        ImGui::PopStyleVar();

        if (ImGui::BeginChild("##assetsbody", ImVec2(0.0f, -kBottomBarHeight)))
        {
            if (!project)
                ImGui::TextDisabled("No project open (data/-next-to-exe)");
            else if (state.lens == AssetLens::Browse)
                DrawBrowseLens(state, model, project, docs, services, actions);
            else if (state.lens == AssetLens::Graph)
                // Plan 3 Task 3, USER-REACHABLE as of Task 5: the toolbar's
                // Graph button is enabled (kLensEnabledMask == 0b111) and the
                // Status lens's scene cards jump straight here. The
                // device-less canvas test (AssetsGraphCanvasTest.cpp) still
                // sets `state.lens` directly, which is now one route among
                // three rather than the branch's only reachability.
                DrawGraphLens(state, model, project, docs, services, actions);
            else if (state.lens == AssetLens::Status)
                DrawStatusLens(state, model, project, docs, services, actions);
        }
        ImGui::EndChild();

        DrawBottomBar(state, model);

        ImGui::End();
        return actions;
    }
}
