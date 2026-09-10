#include "Panels/AssetBrowserPanel.hpp"

#include "Documents/DocumentHost.hpp"      // the open route a row's double-click hands to OpenAssetRow
#include "Panels/AssetPanelModel.hpp"      // AssetPanelModel/AssetPanelEntry/AssetPanelRow -- this panel's whole read surface
#include "Panels/CreateAssetDialog.hpp"    // CreateKindForAssetKind -- the rail's per-kind "+" (AssetKind -> CreateAssetKind bridge)
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiSelectableFlags_NoPadWithHalfSpacing (ruling 4, 2026-09-07)

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// AssetBrowserPanel (panel-split arc): the "Asset Browser" window. Task 6
// moved the BODY here as pure motion out of AssetsPanel.cpp's DrawBrowseLens
// (renamed DrawAssetBrowserBody) -- the rail, the grouped/folded asset table
// (scroll-to-selection + arrow-key nav), the table<->preview drag splitter
// and the resizable preview pane, plus that body's private helpers (the
// row-interaction attachment, the row/rail/group/header painters, the
// derived-list row, the preview-pane splitter and its clamp math) and the
// Browser-only geometry constants they share, none of which any other view
// ever called.
//
// Task 7 added the SHELL at the bottom of this file -- DrawAssetBrowserPanel,
// the window itself: its own ImGui::Begin("Asset Browser"), the `+ Create` +
// search toolbar (spec s9.1's R2 minimum, which is the old shared toolbar
// minus the lens strip and minus the Graph focus slot), and the bottom bar
// (spec s9.2), built on AssetPanelCommon's shared band skeleton + digest chip
// so the three panels' bars cannot drift apart. `state` retargeted to
// AssetBrowserPanelState& in the same task.
//
// BootSceneGuid, DrawAssetPeekTooltip, OpenAssetRow, DrawAssetMenuItems,
// SubkindPillText and CookStateLabel are NOT here: all six are genuinely
// cross-panel (the Graph and/or Status panels call them too), so their
// declarations live on AssetPanelCommon.hpp and -- as of Task 7, which
// retired AssetsPanel.cpp where they used to sit -- their bodies live in
// AssetPanelCommon.cpp. This TU reaches them the same way AssetGraphPanel.cpp
// and AssetStatusPanel.cpp do.
//
// kTooltipWidth/kTooltipThumbSize went to AssetPanelCommon.cpp with
// DrawAssetPeekTooltip, their only reader. kRailWidth/kRailRowHeight/
// kChildIndent/kGroupIndent and every Task 11 preview-pane geometry constant
// (plus ClampPreviewSaneRange/ClampPreviewForLayout) live here in full:
// nothing outside this panel ever read any of them.
namespace Arcane::Editor
{
    namespace
    {
        constexpr float kRailWidth        = 180.0f;
        constexpr float kRailRowHeight    = 26.0f;
        constexpr float kChildIndent      = 20.0f;
        // 2026-09-07 nested folder groups (spec s6/s11.2): 20px per nesting
        // depth, stacked with kChildIndent above rather than merged into it --
        // the two are independently-motivated 20px units that happen to share
        // a value and COMPOUND (a fold child inside a depth-1 group sits at
        // depth*kGroupIndent + kChildIndent from the row's own base).
        constexpr float kGroupIndent      = 20.0f;

        // Task 11 (spec s5/s6/s11.2) fixed geometry: the preview pane is
        // hidden below a 720px panel width (the table never drops below
        // readable width -- spec s5); its thumb is 140px; its action
        // buttons are full-width and 24px tall (§11.2's table row height,
        // reused rather than inventing a new pinned value).
        //
        // 2026-09-07 follow-up (spec s5/s11.2 addendum): the pane's width
        // is no longer a single pinned constant -- it is user-resizable
        // via a drag splitter (AssetBrowserPanelState::previewPaneWidth, the
        // DESIRED width, session-only, matching every other field on that
        // struct). What was `kPreviewPaneWidth = 330.0f` becomes a default
        // (AssetBrowserPanel.hpp's kAssetsPreviewPaneDefaultWidth, half the old
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

        // The absolute sane-range clamp ONLY -- [kPreviewPaneMinWidth,
        // kPreviewPaneMaxWidth] -- and nothing else. This is the ONLY clamp
        // ever applied to a value before it is written into
        // AssetBrowserPanelState::previewPaneWidth (the splitter's drag and its
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
        // table|splitter|pane now sit FLUSH (DrawAssetBrowserBody's SameLine(0,0)
        // calls) -- OptionBC.dc.html has no gap between these regions, only
        // 1px hairline borders the rail and the table each own on their own
        // right edge. This budget must stay in lockstep with that layout:
        // panelWidth == kRailWidth + tableWidth + kPreviewSplitBarPx +
        // drawnWidth EXACTLY now (no `ItemSpacing.x * 3.0f` term), matching
        // DrawAssetBrowserBody's own `tableWidth` formula term-for-term.
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

        // See AssetBrowserPanelState::groupOpen/childrenOpen's own doc comment:
        // these mirror the model's private defaults exactly (open, except the
        // diag:// mount root -- GroupDefaultOpen, spec s5 third revision;
        // children collapsed) so the panel can pick the right chevron glyph
        // and compute the flipped value to push through Set*Open.
        bool GroupIsOpen(const AssetBrowserPanelState& state, const std::string& folder)
        {
            const auto it = state.groupOpen.find(folder);
            return it == state.groupOpen.end() ? GroupDefaultOpen(folder) : it->second;
        }

        bool ChildrenAreOpen(const AssetBrowserPanelState& state, const Arcane::Guid& texture)
        {
            const auto it = state.childrenOpen.find(texture);
            return it == state.childrenOpen.end() ? false : it->second;
        }

        // ---- Task 10: shared row context menu (spec s6) --------------------
        // The Browse-side bracket around DrawAssetMenuItems above.
        void DrawRowContextMenu(AssetPanelModel& model, AssetPanelActions& actions,
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
                                   DocumentHost& docs, const AssetPanelServices& services,
                                   AssetPanelActions& actions, const AssetPanelEntry& e,
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
        void DrawRail(AssetBrowserPanelState& state, AssetPanelModel& model, AssetPanelActions& actions)
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
                // (DrawAssetBrowserBody's SameLine(0,0) removed the ItemSpacing.x
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
        void DrawGroupRow(AssetBrowserPanelState& state, AssetPanelModel& model, const AssetPanelRow& row)
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
        void DrawAssetRow(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions,
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
        void DrawChildRow(AssetBrowserPanelState& /*state*/, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions,
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
        void DrawTable(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions,
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
        void DrawDerivedRow(AssetPanelModel& model, const AssetPanelServices& services,
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
        // `drawnWidth` local (DrawAssetBrowserBody). The visible trade-off: a
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
                            const AssetPanelServices& services, AssetPanelActions& actions, float width)
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
                // sets (DrawRowContextMenu, this file) -- so the host's
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
    }   // end anonymous namespace: DrawAssetBrowserBody below is the one
        // exported entry point (Task 6, panel-split) -- everything above it
        // stays internal-linkage; an anonymous namespace's members remain
        // visible to code that follows it in the SAME enclosing scope (the
        // implicit using-directive), so the body below still reaches every
        // helper and constant unqualified. Same technique AssetGraphPanel.cpp/
        // AssetStatusPanel.cpp use for their own exported bodies.

    // ---- Task 10/11: the Browse lens body (rail + table + preview) -----
    void DrawAssetBrowserBody(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                              DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions)
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

    // ---- Panel-split Task 7: the window (spec s5/s9) -------------------
    AssetPanelActions DrawAssetBrowserPanel(AssetBrowserPanelState& state, AssetPanelModel& model,
                                            const Arcane::Project* project, DocumentHost& docs,
                                            const AssetPanelServices& services,
                                            bool* open)
    {
        AssetPanelActions actions;
        if (!ImGui::Begin("Asset Browser", open))
        {
            // Collapsed, or a docked tab that is not the selected one:
            // ImGui has skipped this window's contents entirely, so there
            // is nothing to draw and nothing the user could have asked for.
            // End is still owed (Begin/End pair unconditionally).
            ImGui::End();
            return actions;
        }

        // ---- toolbar band: + Create -> search (flex) ------------------
        // Spec s9.1 (R2, minimal): the lens strip and the Graph focus slot
        // are GONE with the lens vocabulary itself, so the search well's
        // flex math loses both subtracted terms -- including the strip's
        // one ItemSpacing.x charge, which existed only to pay for the
        // SameLine that placed the strip. What is left is the plain "take
        // the rest of the row", still floored at 80px so a panel too narrow
        // to pay for the Create button never hands ImGui a negative width.
        {
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(style.FramePadding.x, kAssetPanelToolbarFramePadY));

            if (ImGui::Button(ICON_LC_PLUS " Create " ICON_LC_CHEVRON_DOWN))
                ImGui::OpenPopup("##createmenu");
            DrawCreateMenu(actions);

            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x));
            ImGui::InputTextWithHint("##assetssearch", ICON_LC_SEARCH " search...",
                                     state.search, sizeof(state.search));
            // Spec s6: the every-frame filter push happens HERE and nowhere
            // else. Rows()/Rail()/ShownAssetCount() are the model's only
            // filtered views and only this panel reads them, so a search
            // keystroke cannot leak into the Graph or Status windows.
            model.SetSearch(state.search);
            model.SetKindFilter(state.railKind);

            ImGui::PopStyleVar();
        }

        // 2026-09-07 fix (mock parity): ImGui bakes each item's "next line"
        // cursor advance in AT PLACEMENT TIME using whatever ItemSpacing was
        // active THEN, and the retired lens strip zeroed ItemSpacing for its
        // own internal buttons -- so the toolbar's trailing edge silently
        // inherited that zero and the body below sat flush against it with
        // NO gap, live, even though nothing ever asked for that (confirmed by
        // an automation pixel-scan of the live capture: 0px against the
        // redline's 7px, kAssetPanelToolbarBodyGapPx's own comment). The
        // strip is gone, but the EXPLICIT Dummy stays: it is what states the
        // 7px seam outright instead of trusting ImGui's automatic per-item
        // spacing for it a second time. Itself wrapped in a zeroed
        // ItemSpacing so nothing implicit adds to either side of it.
        // Vertical-only; the horizontal flush gutters DrawAssetBrowserBody's
        // own SameLine(0,0) chain established are untouched.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, kAssetPanelToolbarBodyGapPx));
        ImGui::PopStyleVar();

        // ---- body band -----------------------------------------------
        if (ImGui::BeginChild("##assetbrowserbody", ImVec2(0.0f, -kAssetPanelBottomBarHeight)))
        {
            if (!project)
                ImGui::TextDisabled("No project open (data/-next-to-exe)");
            else
                DrawAssetBrowserBody(state, model, project, docs, services, actions);
        }
        ImGui::EndChild();

        // ---- bottom bar band (spec s9.2) -----------------------------
        // LEFT: this panel's own context line, in the two forms it has
        // always had ("X of N shown" while a rail or search filter is on,
        // "N assets - M selected" otherwise). RIGHT: the health digest chip.
        {
            const AssetPanelBottomBar bar = BeginAssetPanelBottomBar("##assetbrowserbottombar");
            if (bar.visible)
            {
                const HealthCounts health = model.Health();
                char left[64];
                if (model.Filtered())
                    std::snprintf(left, sizeof(left), "%d of %d shown",
                                  model.ShownAssetCount(), health.total);
                else
                    std::snprintf(left, sizeof(left), "%d assets \xC2\xB7 %d selected",
                                  health.total, model.selected.IsValid() ? 1 : 0);
                ImGui::TextUnformatted(left);

                DrawAssetPanelHealthDigest(bar, model, services, actions);
            }
            EndAssetPanelBottomBar();
        }

        ImGui::End();
        return actions;
    }
}
