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
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// AssetBrowserPanel (panel-split arc): the "Asset Browser" window. Task 6
// moved the BODY here as pure motion out of AssetsPanel.cpp's DrawBrowseLens
// (renamed DrawAssetBrowserBody) -- the rail and the grouped/folded asset
// table (scroll-to-selection + arrow-key nav), plus that body's private
// helpers (the row-interaction attachment, the row/rail/group/header
// painters) and the Browser-only geometry constants they share, none of
// which any other view ever called. The body is the rail + the table; the
// asset's details are the Assets Inspector's page (inspector filters spec
// 2026-09-29 s6: the old preview pane, its splitter and its clamp math
// moved to AssetInspectorSource.cpp or were deleted with the pane).
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
// kChildIndent/kGroupIndent live here in full: nothing outside this panel
// ever read any of them.
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

        // Rail "+" gate (spec s6): only kinds with a Create-menu entry get
        // the hover create affordance. Textures/Data/Audio/Font/Diagnostic/
        // Other get none. Source has one since the C++ Class wizard
        // (CreateKindForAssetKind bridges it to CppClass).
        bool RailKindCreatable(int kind)
        {
            switch (static_cast<AssetKind>(kind))
            {
                case AssetKind::Material:
                case AssetKind::Sprite:
                case AssetKind::Mesh:
                case AssetKind::Scene:
                case AssetKind::Source:
                    return true;
                default:
                    // Model: imported, never created -- no Create menu entry
                    // (s8's F2c/F4 line). Same for Texture/Data/Audio/Font/
                    // Diagnostic/Other.
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
        // T5 s7.6: a row-menu file-op verb, disabled with its dry-run refusal
        // as the hover tooltip ("" = enabled).
        bool MenuVerb(const char* label, const char* shortcut, const std::string& refusal)
        {
            ImGui::BeginDisabled(!refusal.empty()); const bool hit = ImGui::MenuItem(label, shortcut); ImGui::EndDisabled();
            if (!refusal.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", refusal.c_str());
            return hit;
        }

        void DrawRowContextMenu(AssetBrowserPanelState& state, AssetPanelModel& model, AssetPanelActions& actions,
                                const AssetPanelServices& services, const AssetPanelEntry& e,
                                bool kindSpecific)
        {
            if (!ImGui::BeginPopupContextItem())
                return;

            // Right-click acts on this row: make it the tracked selection so
            // the highlight + the Inspector/Assets-menu follow (matches
            // AssetBrowser.cpp's own old behavior). ONCE, on the popup's first
            // frame: Select bumps AssetPanelModel::selectionGesture on every
            // call (a re-selection IS an Inspector selection event, spec
            // 2026-09-29 s3), so a per-frame call would re-fire the Inspector's
            // asset edge for as long as the menu stays open.
            //
            // T5 s7.9: a right-click INSIDE the multi-selection keeps the set
            // and only re-points the primary; outside it, it selects the row
            // alone. Either way the verbs below act on `model.selection`.
            if (ImGui::IsWindowAppearing())
            {
                if (model.InSelection(e.guid)) model.SetPrimary(e.guid);
                else                           model.Select(e.guid);
            }

            DrawAssetMenuItems(actions, e, kindSpecific, services);

            // T5 s7.6: the file-op verbs, each disabled with its reason. ONE
            // dry-run per menu open (the appearing frame), not per frame.
            ImGui::Separator();
            if (ImGui::IsWindowAppearing())
                state.menuRefusal.rename = model.SelectionCount() > 1 ? std::string("Select one asset to rename")
                                         : services.fileOpRefusal ? services.fileOpRefusal({ .kind = AssetOpKind::Rename, .guids = { e.guid }, .newStem = e.name }) : "unavailable";
            if (MenuVerb("Rename", "F2", state.menuRefusal.rename)) BeginAssetRename(state, e);
            if (ImGui::IsWindowAppearing())   // T5 s7.7
                state.menuRefusal.duplicate = services.fileOpRefusal ? services.fileOpRefusal({ .kind = AssetOpKind::Duplicate, .guids = model.selection }) : "unavailable";
            if (MenuVerb("Duplicate", "Ctrl+D", state.menuRefusal.duplicate))
                actions.fileOp = AssetOpRequest{ .kind = AssetOpKind::Duplicate, .guids = model.selection };
            if (ImGui::IsWindowAppearing())   // T5 s7.5: the host's confirm modal re-plans with the live scene
                state.menuRefusal.del = services.fileOpRefusal ? services.fileOpRefusal({ .kind = AssetOpKind::Delete, .guids = model.selection }) : "unavailable";
            if (MenuVerb("Delete", "Del", state.menuRefusal.del)) actions.requestDelete = model.selection;

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
        void AttachRowInteractions(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
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

            DrawRowContextMenu(state, model, actions, services, e, kindSpecificMenu);
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
            constexpr float kGroupCountGap = 6.0f;
            if (row.groupCount > 0)
            {
                char countBuf[16];
                std::snprintf(countBuf, sizeof(countBuf), "%d", row.groupCount);
                dl->AddText(ImVec2(nameX + nameW + kGroupCountGap, textY),
                           ImGui::GetColorU32(ImGuiCol_TextDisabled), countBuf);
            }
            // T5 s7.8: an empty folder (no asset beneath it) has no count; it
            // says so, dim, in the count's place.
            if (row.empty)
                dl->AddText(ImVec2(nameX + nameW + kGroupCountGap, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), "(empty)");

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

        // ---- T5 s7.6: the inline rename box (a row's stand-in) -------------
        // Stem-only box + the dim, fixed extension; no drag source or menu
        // while it stands in for the row. A valid Enter, or an edit followed
        // by a click away, commits (fileOp); an invalid Enter keeps the box
        // and its refusal tooltip; Esc, or a click away without an edit,
        // cancels. The dry-run runs per frame through the host's memo.
        // The row's thumb cell is painted first as drawlist overdraw (no
        // ImGui item, so the 24px row pitch is unchanged), the same way
        // RowWithThumb paints it: the thumb when resolved, else the kind's
        // Lucide glyph centred in the cell (spec s7.6: "thumb + InputText").
        void DrawRenameBox(AssetBrowserPanelState& st, const AssetPanelEntry& e, float indent, std::uint64_t thumbId, const char* icon,
                           const AssetPanelServices& sv, AssetPanelActions& actions)
        {
            st.renameDrawn = true; const ImVec2 at = ImGui::GetCursorScreenPos();
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float thumbY = at.y + (kTableRowHeight - kAssetRowThumbSize) * 0.5f;
                if (thumbId != 0)
                    dl->AddImage(static_cast<ImTextureID>(thumbId), ImVec2(at.x + indent, thumbY),
                                 ImVec2(at.x + indent + kAssetRowThumbSize, thumbY + kAssetRowThumbSize));
                else
                {
                    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
                    dl->AddText(ImVec2(at.x + indent + (kAssetRowThumbSize - iconSize.x) * 0.5f,
                                       at.y + (kTableRowHeight - iconSize.y) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_Text), icon);
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + indent + kAssetRowThumbSize + ImGui::GetStyle().ItemSpacing.x, at.y + 2.0f));
            const std::string ext = std::filesystem::path(e.fileName).extension().string();
            ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ext.c_str()).x - 8.0f));
            if (st.renameFocusPending) { ImGui::SetKeyboardFocusHere(); st.renameFocusPending = false; }
            const bool enter = ImGui::InputText("##assetrename", st.renameBuf, sizeof(st.renameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false), off = ImGui::IsItemDeactivated(), active = ImGui::IsItemActive();
            const bool commit = enter || (ImGui::IsItemDeactivatedAfterEdit() && !esc);
            const AssetOpRequest req{ .kind = AssetOpKind::Rename, .guids = { e.guid }, .newStem = st.renameBuf };
            const std::string why = sv.fileOpRefusal ? sv.fileOpRefusal(req) : std::string{};
            if (!why.empty() && active) ImGui::SetTooltip("%s", why.c_str());
            ImGui::SameLine(0.0f, 2.0f); ImGui::TextDisabled("%s", ext.c_str());
            // Esc cancels when it deactivated the box, OR when it lands on the
            // box's activation frame: InputText skips key handling while
            // ActiveIdIsJustActivated (imgui_widgets.cpp:5113), and the
            // SetKeyboardFocusHere above activates two frames after F2, so an
            // early Esc would otherwise be swallowed and the box stay open.
            if (esc && (off || active)) { st.renameTarget = {}; return; }
            if (commit && why.empty()) { actions.fileOp = req; st.renameTarget = {}; return; }
            if (enter) { st.renameFocusPending = true; return; }
            if (off) st.renameTarget = {};
        }

        // T5 s7.9: tag the row's Selectable (RowWithThumb's, the next item) with
        // its Rows() INDEX for BeginMultiSelect. Skipped in a SkipItems window:
        // RowWithThumb returns before its Selectable there, and an armed
        // selection user data would leak to the next real item drawn this
        // frame (the Console's crash, EditorPanels.cpp's multi-select gate).
        void SetRowSelectionUserData(int rowIndex)
        {
            if (!ImGui::GetCurrentWindowRead()->SkipItems)
                ImGui::SetNextItemSelectionUserData(rowIndex);
        }

        // ---- Task 10: one top-level asset row (spec s6/s11.2) --------------
        void DrawAssetRow(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions,
                          const AssetPanelEntry& e, const Arcane::Guid& bootGuid, int groupDepth, int rowIndex)
        {
            ImGui::PushID(e.guid.ToString().c_str());

            const bool hasChildren = (e.kind == AssetKind::Texture || e.kind == AssetKind::Model)
                                  && !e.derivedChildren.empty();
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
            const bool selected = model.InSelection(e.guid);   // T5 s7.9: the whole multi-selection highlights

            if (state.renameTarget == e.guid) { DrawRenameBox(state, e, indent, thumbId, icon, services, actions); ImGui::PopID(); return; }
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            SetRowSelectionUserData(rowIndex);
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, indent, kTableRowHeight);
            if (res.clicked)
                state.msClicked = e.guid;   // T5 s7.9: the primary once DrawTable applies EndMultiSelect's requests

            // Fix round 1 (Critical 1): attach drag/context-menu/tooltip/
            // double-click HERE, immediately -- the row's Selectable is
            // still ImGui's last submitted item at this exact point (see
            // RowWithThumb's own doc comment). Everything drawn below this
            // line (the expander, the refused badge, the pills) must come
            // AFTER this call, not before -- each is either pure drawlist
            // (doesn't touch "last item") or a real item that would
            // otherwise steal that title away from the Selectable.
            AttachRowInteractions(state, model, project, docs, services, actions, e, /*kindSpecificMenu=*/true);

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
            if (refused || e.cook == CookState::Queued)
            {
                constexpr float kBadgeFontSize = 10.0f;
                constexpr float kBadgeMargin    = 3.0f;
                const char* badge = refused ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CLOCK;
                ImGui::PushFont(GetEditorFonts().interRegular, kBadgeFontSize);
                const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
                const float thumbY      = rowMin.y + (kTableRowHeight - kAssetRowThumbSize) * 0.5f;
                const float thumbRight  = rowMin.x + indent + kAssetRowThumbSize;
                const float thumbBottom = thumbY + kAssetRowThumbSize;
                const ImVec2 badgePos(thumbRight  - badgeSize.x - kBadgeMargin,
                                      thumbBottom - badgeSize.y - kBadgeMargin);
                ImGui::GetWindowDrawList()->AddText(badgePos,
                    ImGui::GetColorU32(refused ? Theme::kAmber : Theme::kTextDim), badge);
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
        void DrawChildRow(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions,
                          const AssetPanelEntry& e, int groupDepth, int rowIndex)
        {
            ImGui::PushID(e.guid.ToString().c_str());

            const std::uint64_t thumbId = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            const char* icon = KindIcon(e.kind);
            const bool selected = model.InSelection(e.guid);   // T5 s7.9

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
            if (state.renameTarget == e.guid) { DrawRenameBox(state, e, indent, thumbId, icon, services, actions); ImGui::PopID(); return; }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            SetRowSelectionUserData(rowIndex);
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, indent, kTableRowHeight);
            ImGui::PopStyleColor();
            if (res.clicked)
                state.msClicked = e.guid;   // T5 s7.9

            // Fix round 1 (Critical 1): attach interactions before drawing
            // the pill -- see DrawAssetRow's own comment on ordering.
            AttachRowInteractions(state, model, project, docs, services, actions, e, /*kindSpecificMenu=*/false);

            ImGui::SetCursorScreenPos(res.trailingPos);
            AssetPill("derived");

            ImGui::PopID();
        }

        // ---- Task 10: the table (spec s6/s11.2) ----------------------------
        // `width` is 0.0f (ImGui's own "fill everything left on this line"):
        // the table takes the whole width after the rail. (The preview pane
        // that used to share this line is the Assets Inspector's page since
        // inspector filters s6.)
        void DrawTable(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions,
                       const Arcane::Guid& bootGuid, float width)
        {
            state.renameDrawn = false;   // T5 s7.6: set again by DrawRenameBox if the target's row draws
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
            // A pending Reveal (RevealAssetInBrowser) asks for the same
            // scroll even when the stamp has nothing new to say.
            const bool wantsScroll = state.revealPending ||
                                     (state.seenSelectionStamp != model.selectionStamp);
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
            {
                state.seenSelectionStamp = model.selectionStamp;
                state.revealPending = false;
            }

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
                // Up/Down walking past the visible edge -- or an external
                // selector (the Assets Inspector page's Derived list)
                // picking something scrolled away -- still correctly
                // recenters. This subsumes tagging each panel-side
                // model.Select() call site individually: there is exactly
                // one thing that actually needs to be true (was the row
                // visible already), and checking it directly cannot drift
                // out of sync the way remembering to tag every call site
                // could.
                //
                // T3-D4: the test is in content space, and it counts the
                // frozen Name header. Data row i spans [h*(i+1), h*(i+2)),
                // because the header takes the first row height. The visible
                // band is [scrollY + h, scrollY + inner height), because the
                // frozen header covers the top of the view. The old
                // row-index test left the header out, so it called a row
                // visible while the row sat one or two rows below the bottom
                // edge.
                // An ordinary selection needs only part of the row visible:
                // a row the mouse clicked always is, so it never re-centres.
                // A Reveal needs the WHOLE row visible.
                bool targetAlreadyVisible = false;
                if (scrollTargetIndex >= 0)
                {
                    const float scrollY = ImGui::GetScrollY();
                    const float viewTop = scrollY + kTableRowHeight;
                    const float viewBottom = scrollY + ImGui::GetCurrentWindow()->InnerRect.GetHeight();
                    const float rowTop = kTableRowHeight * static_cast<float>(scrollTargetIndex + 1);
                    const float rowBottom = rowTop + kTableRowHeight;
                    targetAlreadyVisible = state.revealPending
                        ? (rowTop >= viewTop && rowBottom <= viewBottom)
                        : (rowBottom > viewTop && rowTop < viewBottom);
                }

                // T5 s7.9: the multi-select scope (the Console precedent,
                // EditorPanels.cpp's DrawConsolePanel). External storage: a
                // guid is not an ImGuiID. The adapter maps a row INDEX to its
                // guid and ignores group rows, because ApplyRequests visits
                // every range/SetAll index (imgui_widgets.cpp:8762-8773).
                struct MsAdapter { const std::vector<AssetPanelRow>* rows; std::vector<Arcane::Guid> next; bool changed = false; };
                MsAdapter ad{ &rows, model.selection };
                ImGuiSelectionExternalStorage storage;
                storage.UserData = &ad;
                storage.AdapterSetItemSelected = [](ImGuiSelectionExternalStorage* self, int idx, bool sel)
                {
                    auto& a = *static_cast<MsAdapter*>(self->UserData);
                    // A range anchor is an index from an earlier frame's rows
                    // (a fold or filter may have shrunk them since): ignore
                    // anything out of bounds.
                    if (idx < 0 || static_cast<std::size_t>(idx) >= a.rows->size()) return;
                    const AssetPanelRow& r = (*a.rows)[static_cast<std::size_t>(idx)];
                    if (r.type == AssetPanelRow::Type::Group) return;
                    const auto it = std::find(a.next.begin(), a.next.end(), r.guid);
                    if (sel && it == a.next.end()) { a.next.push_back(r.guid); a.changed = true; }
                    else if (!sel && it != a.next.end()) { a.next.erase(it); a.changed = true; }
                };
                state.msClicked = {};
                ImGuiMultiSelectIO* ms = ImGui::BeginMultiSelect(ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid | ImGuiMultiSelectFlags_BoxSelect1d,
                                                                 model.SelectionCount(), static_cast<int>(rows.size()));
                storage.ApplyRequests(ms);

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(rows.size()), kTableRowHeight);
                if (scrollTargetIndex >= 0 && !targetAlreadyVisible)
                    clipper.IncludeItemByIndex(scrollTargetIndex);
                // The Shift-range source must be submitted even when clipped
                // away, or a range from an off-screen anchor loses its start.
                if (ms->RangeSrcItem >= 0 && ms->RangeSrcItem < static_cast<ImGuiSelectionUserData>(rows.size()))
                    clipper.IncludeItemByIndex(static_cast<int>(ms->RangeSrcItem));

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
                                    DrawAssetRow(state, model, project, docs, services, actions, *e, bootGuid, row.groupDepth, i);
                                break;
                            case AssetPanelRow::Type::Child:
                                if (const AssetPanelEntry* e = model.Find(row.guid))
                                    DrawChildRow(state, model, project, docs, services, actions, *e, row.groupDepth, i);
                                break;
                        }

                        if (i == scrollTargetIndex)
                        {
                            if (!targetAlreadyVisible)
                                ImGui::SetScrollHereY();
                            state.seenSelectionStamp = model.selectionStamp;
                            state.revealPending = false;
                        }
                    }
                }
                // No trailing Dummy (unlike the Console's plain child): inside a
                // table the clipper's final seek already closed the last row
                // (ImGuiListClipper_SeekCursorAndSetupPrevLine sets RowPosY2 =
                // cursor), and an item submitted outside a row moves the cursor
                // past RowPosY2 -- EndTable's IM_ASSERT (imgui_tables.cpp:1444).
                ms = ImGui::EndMultiSelect();
                storage.ApplyRequests(ms);
                if (ad.changed || state.msClicked.IsValid()) model.ApplySelection(std::move(ad.next), state.msClicked);
                ImGui::EndTable();
                // T5 s7.6 (the Outliner wedge lesson): a rename target whose
                // row did not draw this frame (scrolled out, filtered away,
                // folded, deleted) cancels, so the box can never hold the
                // keys from off screen.
                if (state.renameTarget.IsValid() && !state.renameDrawn) state.renameTarget = {};
            }
            ImGui::PopStyleVar();

            // Step 3 tail: keyboard, minimal v1 (spec s8). Up/Down move
            // Select through the VISIBLE rows (group rows are not navigable
            // targets); Enter opens. T5 s7.10: the guard is the Outliner's
            // (focused incl. child windows, no popup over it, no text
            // field), computed ONCE on the top "Asset Browser" window by
            // DrawAssetBrowserPanel -- re-asking IsWindowFocused here, inside
            // the body child, would miss a focused toolbar or tab. Ctrl+X/C/V
            // are consumed with no action (v1 has no asset clipboard): owning
            // the keys is what keeps them off the entity clipboard.
            const bool keysLive = actions.ownsEditKeys;
            if (keysLive)
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
                // T5 s7.9: F2 renames ONE asset; Ctrl+D and Del act on the whole multi-selection.
                if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && model.SelectionCount() == 1 && model.selected.IsValid())   // T5 s7.6
                    if (const AssetPanelEntry* e = model.Find(model.selected)) BeginAssetRename(state, *e);
                if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && model.SelectionCount() > 0)   // T5 s7.7
                    actions.fileOp = AssetOpRequest{ .kind = AssetOpKind::Duplicate, .guids = model.selection };
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && model.SelectionCount() > 0)   // T5 s7.5: the confirm modal, never a direct delete
                    actions.requestDelete = model.selection;
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

    void BeginAssetRename(AssetBrowserPanelState& st, const AssetPanelEntry& e)
    { st.renameTarget = e.guid; std::snprintf(st.renameBuf, sizeof(st.renameBuf), "%s", e.name.c_str()); st.renameFocusPending = true; }

    // ---- Task 10: the Browse lens body (rail + table) ------------------
    void DrawAssetBrowserBody(AssetBrowserPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                              DocumentHost& docs, const AssetPanelServices& services, AssetPanelActions& actions)
    {
        // The project's recorded boot scene, resolved once per draw
        // (rather than per row) for the "boot" pill (spec s6).
        const Arcane::Guid bootGuid = BootSceneGuid(project);

        // 2026-09-07 (user nitpick, mock parity): rail|table sit FLUSH --
        // SameLine(0.0f, 0.0f) zeroes the ItemSpacing.x gutter SameLine()
        // would otherwise insert. OptionBC.dc.html has no gap here either:
        // the rail's own `border-right: 1px solid #333333` (DrawRail's
        // hairline) is the separator, not an 8px void beside it.
        DrawRail(state, model, actions);
        ImGui::SameLine(0.0f, 0.0f);

        // The table takes the width after the rail (0.0f = DrawTable's
        // "fill everything left on this line"). The asset's details are the
        // Assets Inspector's page (inspector filters s6), not a pane here.
        DrawTable(state, model, project, docs, services, actions, bootGuid, /*tableWidth*/ 0.0f);
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
            const PopupAnchor createAnchor = LastItemAnchor();
            // The selection is the menu's subject: a selected material is what
            // "Material Instance..." derives from (T3-D4), a selected texture
            // what "Sprite..." is cut from (T3-D5); with none selected the
            // dialog opens with an empty picker.
            DrawCreateMenu(actions, createAnchor,
                           model.selected.IsValid() ? model.Find(model.selected) : nullptr);

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

        // T5 s7.10: the Browser's key guard, computed ONCE per frame while
        // the current window is the TOP "Asset Browser" window, so focus on
        // its toolbar, tab or table all count (ChildWindows); a popup over it
        // (a row's context menu) does not (NoPopupHierarchy), nor does an
        // active text field (the search box), nor an open inline rename box
        // (T5 s7.6; also on its first frames, before it is active).
        // DrawTable's key block and the app's entity-clipboard fold both read
        // this one value.
        actions.ownsEditKeys = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_NoPopupHierarchy) && !ImGui::GetIO().WantTextInput
                            && !state.renameTarget.IsValid();   // T5 s7.6: an open rename box owns the keys

        // ---- body band -----------------------------------------------
        if (ImGui::BeginChild("##assetbrowserbody", ImVec2(0.0f, -kAssetPanelBottomBarHeight)))
        {
            if (!project)
                DrawAssetPanelNoProjectMessage();
            else
                DrawAssetBrowserBody(state, model, project, docs, services, actions);
        }
        ImGui::EndChild();

        // ---- bottom bar band (spec s9.2) -----------------------------
        // LEFT: this panel's own context line, in the two forms it has
        // always had ("X of N shown" while a rail or search filter is on,
        // "N assets - K selected" otherwise, K the live multi-selection
        // count since T5 s7.9: AssetBrowserContextLine). RIGHT: the health
        // digest chip.
        {
            const AssetPanelBottomBar bar = BeginAssetPanelBottomBar("##assetbrowserbottombar");
            if (bar.visible)
            {
                ImGui::TextUnformatted(AssetBrowserContextLine(model).c_str());

                DrawAssetPanelHealthDigest(bar, model, services, actions);
            }
            EndAssetPanelBottomBar();
        }

        ImGui::End();
        return actions;
    }
}
