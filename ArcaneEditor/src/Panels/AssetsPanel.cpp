#include "Panels/AssetsPanel.hpp"

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetActivityLog.hpp"    // AssetActivityEntry/Kind (Task 8's feed, the first reader)
#include "Panels/CreateAssetDialog.hpp"   // CreateAssetKind + the AssetKind bridge (Task 12)
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

#include <algorithm>
#include <cfloat>
#include <chrono>
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

        // The lens strip's three labels, fixed regardless of which plan has
        // landed (spec s5: "Plan 1 ships the full three-button strip ...
        // layout pinned from day one, later plans enable, nothing shifts").
        // constexpr on a non-reference array makes every element itself
        // const, so the decayed pointer is `const char* const*` --
        // SegmentedStrip's exact parameter type, no cast needed.
        constexpr const char* kLensLabels[] = { "Browse", "Graph", "Status" };
        constexpr int kLensCount = 3;
        // Bits 0 (Browse) and 2 (Status) -- Status went live with Plan 2
        // Task 7; Graph (bit 1) stays disabled until Plan 3 lands (spec
        // s5/s10). The strip's LAYOUT never changed for either: the three
        // buttons have been drawn since Plan 1, only the mask moves.
        constexpr unsigned kLensEnabledMask = 0b101u;

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

        // Toolbar band: + Create -> search (flex) -> [per-lens slot, EMPTY in
        // Plan 1 -- only Graph's focus combo uses it, Plan 3] -> lens strip
        // anchored right-most (spec s5). Mutates `state` in place; the
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
            // per-lens slot (0 in Plan 1).
            float stripWidth = 0.0f;
            for (const char* label : kLensLabels)
                stripWidth += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
            const float searchWidth = std::max(80.0f,
                ImGui::GetContentRegionAvail().x - stripWidth - style.ItemSpacing.x);

            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##assetssearch", ICON_LC_SEARCH " search...",
                                     state.search, sizeof(state.search));
            model.SetSearch(state.search);
            model.SetKindFilter(state.railKind);

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
            char left[64];
            // Status keeps ONE fixed form regardless of filter state (the
            // lens has no search box of its own to filter against); every
            // other lens keeps Browse's existing forms VERBATIM (plan doc
            // Step 4).
            if (state.lens == AssetLens::Status)
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
        void DrawAssetPeekTooltip(const AssetPanelModel& model, const AssetsPanelServices& services,
                                  const Arcane::Guid& guid, bool forceShow = false)
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

            ImGui::EndTooltip();
        }

        // ---- Task 10: shared row context menu (spec s6) --------------------
        // `kindSpecific` gates the Material/Scene/Texture leading entries --
        // Type::Child rows are always folded 1:1 sprites, so none of those
        // three ever apply to one and the caller passes false to skip them.
        void DrawRowContextMenu(AssetPanelModel& model, AssetsPanelActions& actions,
                                const AssetPanelEntry& e, bool kindSpecific)
        {
            if (!ImGui::BeginPopupContextItem())
                return;

            // Right-click acts on this row: make it the tracked selection so
            // the highlight + the Inspector/Assets-menu follow (matches
            // AssetBrowser.cpp's own old behavior).
            model.Select(e.guid);

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
        // a count line derived from the reference index · a disabled "Focus
        // in Graph" placeholder (Plan 3 wires it up -- BeginDisabled, not a
        // stub that pretends to do something).
        void DrawSceneCard(AssetPanelModel& model, const AssetPanelEntry& e, const Arcane::Guid& bootGuid)
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

            ImGui::BeginDisabled();
            ImGui::Button("Focus in Graph");
            ImGui::EndDisabled();

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

                std::vector<const AssetPanelEntry*> scenes;
                for (const auto& [guid, entry] : model.Entries())
                    if (entry.kind == AssetKind::Scene)
                        scenes.push_back(&entry);
                std::sort(scenes.begin(), scenes.end(), byName);

                if (scenes.empty())
                    ImGui::TextDisabled("no scenes");
                else
                    for (const AssetPanelEntry* e : scenes)
                        DrawSceneCard(model, *e, bootGuid);

                ImGui::EndTable();
            }

            ImGui::EndChild();
        }
    }

    AssetsPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                       const Arcane::Project* project, DocumentHost& docs,
                                       const AssetsPanelServices& services,
                                       bool* open)
    {
        AssetsPanelActions actions;
        ImGui::Begin("Assets", open);

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
            else if (state.lens == AssetLens::Status)
                DrawStatusLens(state, model, project, docs, services, actions);
            else
                // Graph only, now that Status has landed (Plan 2 Task 7).
                // Unreachable in practice -- kLensEnabledMask keeps the Graph
                // button disabled, so `state.lens` can never BE Graph -- kept
                // as the backstop for the one lens still to come.
                ImGui::TextDisabled("Graph lens lands in Plan 3.");
        }
        ImGui::EndChild();

        DrawBottomBar(state, model);

        ImGui::End();
        return actions;
    }
}
