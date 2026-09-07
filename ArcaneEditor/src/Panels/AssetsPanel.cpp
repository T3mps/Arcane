#include "Panels/AssetsPanel.hpp"

#include "Documents/DocumentHost.hpp"
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

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
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

        // The lens strip's three labels, fixed regardless of which plan has
        // landed (spec s5: "Plan 1 ships the full three-button strip ...
        // layout pinned from day one, later plans enable, nothing shifts").
        // constexpr on a non-reference array makes every element itself
        // const, so the decayed pointer is `const char* const*` --
        // SegmentedStrip's exact parameter type, no cast needed.
        constexpr const char* kLensLabels[] = { "Browse", "Graph", "Status" };
        constexpr int kLensCount = 3;
        // Bit 0 (Browse) only -- Graph/Status stay disabled until Plan 2/3
        // land (spec s5).
        constexpr unsigned kLensEnabledMask = 0b001u;

        // Task 10 (spec s6/s11.2) fixed geometry.
        constexpr float kRailWidth        = 180.0f;
        constexpr float kRailRowHeight    = 26.0f;
        constexpr float kTableRowHeight   = 24.0f;
        constexpr float kChildIndent      = 20.0f;
        constexpr float kTooltipWidth     = 210.0f;
        constexpr float kTooltipThumbSize = 64.0f;

        // Task 11 (spec s5/s6/s11.2) fixed geometry: the preview pane is a
        // pinned 330px column, hidden below a 720px panel width (the table
        // never drops below readable width -- spec s5); its thumb is 140px;
        // its action buttons are full-width and 24px tall (§11.2's table row
        // height, reused rather than inventing a new pinned value).
        constexpr float kPreviewPaneWidth      = 330.0f;
        constexpr float kPreviewHidePanelWidth = 720.0f;
        constexpr float kPreviewThumbSize      = 140.0f;
        constexpr float kActionButtonHeight    = 24.0f;

        // Copied from AssetBrowser.cpp:16-39 (internal linkage there, so it
        // cannot be reused across translation units -- same "copy the
        // switch" convention AssetPanelModel.cpp's RailKindLabel already
        // established for the same reason).
        const char* KindIcon(AssetKind kind)
        {
            switch (kind)
            {
                case AssetKind::Material: return ICON_LC_PALETTE;
                case AssetKind::Texture:  return ICON_LC_IMAGE;
                case AssetKind::Audio:    return ICON_LC_MUSIC;
                case AssetKind::Font:     return ICON_LC_TYPE;
                case AssetKind::Data:     return ICON_LC_FILE_JSON;
                case AssetKind::Scene:    return ICON_LC_CLAPPERBOARD;
                case AssetKind::Sprite:   return ICON_LC_STICKER;
                case AssetKind::Diagnostic: return ICON_LC_BUG;
                case AssetKind::Mesh:     return ICON_LC_BOX;
                case AssetKind::Other:    return ICON_LC_FILE;
            }
            return ICON_LC_FILE;
        }

        // For the peek tooltip's kind pill -- same labels AssetBrowser.cpp's
        // own (internal-linkage) KindLabel uses.
        const char* KindLabel(AssetKind kind)
        {
            switch (kind)
            {
                case AssetKind::Material: return "Material";
                case AssetKind::Texture:  return "Texture";
                case AssetKind::Audio:    return "Audio";
                case AssetKind::Font:     return "Font";
                case AssetKind::Data:     return "Data";
                case AssetKind::Scene:    return "Scene";
                case AssetKind::Sprite:   return "Sprite";
                case AssetKind::Diagnostic: return "Diagnostic";
                case AssetKind::Mesh:     return "Mesh";
                case AssetKind::Other:    return "Other";
            }
            return "Other";
        }

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
        // these mirror the model's private defaults exactly (group open,
        // children collapsed) so the panel can pick the right chevron glyph
        // and compute the flipped value to push through Set*Open.
        bool GroupIsOpen(const AssetsPanelState& state, const std::string& folder)
        {
            const auto it = state.groupOpen.find(folder);
            return it == state.groupOpen.end() ? true : it->second;
        }

        bool ChildrenAreOpen(const AssetsPanelState& state, const Arcane::Guid& texture)
        {
            const auto it = state.childrenOpen.find(texture);
            return it == state.childrenOpen.end() ? false : it->second;
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

        // The unified Create menu's entries (spec s7): every entry DISABLED --
        // CreateAssetRequest/requestCreateKind wiring is Task 12's. Shared by
        // the toolbar's popup and every row's context-menu "Create" submenu so
        // the list is spelled once.
        void DrawCreateMenuEntries()
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_LC_PALETTE " Material...");
            ImGui::MenuItem(ICON_LC_LAYERS  " Material Instance...");
            ImGui::Separator();
            ImGui::MenuItem(ICON_LC_BOX          " Mesh...");
            ImGui::MenuItem(ICON_LC_STICKER      " Sprite...");
            ImGui::MenuItem(ICON_LC_CLAPPERBOARD " Scene...");
            ImGui::EndDisabled();
        }

        void DrawCreateMenu()
        {
            if (!ImGui::BeginPopup("##createmenu"))
                return;
            DrawCreateMenuEntries();
            ImGui::EndPopup();
        }

        // Toolbar band: + Create -> search (flex) -> [per-lens slot, EMPTY in
        // Plan 1 -- only Graph's focus combo uses it, Plan 3] -> lens strip
        // anchored right-most (spec s5). Mutates `state` in place; the
        // create popup's disabled entries mean nothing populates `actions`
        // this task, but the call is wired here so Task 12 has one site to
        // extend rather than a new one.
        void DrawToolbar(AssetsPanelState& state, AssetPanelModel& model, AssetsPanelActions& /*actions*/)
        {
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(style.FramePadding.x, kToolbarFramePadY));

            if (ImGui::Button(ICON_LC_PLUS " Create " ICON_LC_CHEVRON_DOWN))
                ImGui::OpenPopup("##createmenu");
            DrawCreateMenu();

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
        // chip (amber refused count + dim cooking/unused). Spec s13: the
        // digest never renders an unknown as a zero -- HealthCounts has no
        // "unused" field yet (Plan 2's AssetReferenceIndex adds it), so that
        // segment is ALWAYS the literal em-dash here, never a fabricated 0.
        void DrawBottomBar(const AssetPanelModel& model)
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
            if (filtered)
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
                          " \xC2\xB7 %d cooking \xC2\xB7 \xE2\x80\x94 unused", health.queued);
            char digestFull[160];
            std::snprintf(digestFull, sizeof(digestFull), "%s%s", refusedPart, restPart);
            const float digestWidth = ImGui::CalcTextSize(digestFull).x;

            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - digestWidth));
            ImGui::SetCursorPosY(padY);
            ImGui::TextColored(Theme::kAmber, "%s", refusedPart);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s", restPart);

            ImGui::EndChild();
        }

        // ---- Task 10: the peek tooltip (spec s8) ---------------------------
        // File-local per the brief: rows, child rows and (later, Task 11) the
        // preview pane's Derived list all hover the same asset. Text-and-
        // images only -- never a button (a tooltip is not interactable).
        void DrawAssetPeekTooltip(const AssetPanelModel& model, const AssetsPanelServices& services,
                                  const Arcane::Guid& guid)
        {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
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
                DrawCreateMenuEntries();
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
                        if (plusClicked)
                            actions.requestCreateKind = re.kind;

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
            const float rowWidth = ImGui::GetContentRegionAvail().x;

            const bool open = GroupIsOpen(state, row.groupName);
            const bool clicked = ImGui::Selectable("##grouprow", false, ImGuiSelectableFlags_SpanAllColumns,
                                                   ImVec2(0.0f, kTableRowHeight));
            if (clicked)
            {
                const bool newOpen = !open;
                state.groupOpen[row.groupName] = newOpen;
                model.SetGroupOpen(row.groupName, newOpen);
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float padX = ImGui::GetStyle().FramePadding.x;
            const float textY = rowMin.y + (kTableRowHeight - ImGui::GetTextLineHeight()) * 0.5f;

            const char* chevron = open ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT;
            dl->AddText(ImVec2(rowMin.x + padX, textY), ImGui::GetColorU32(ImGuiCol_Text), chevron);
            const float chevronW = ImGui::CalcTextSize(chevron).x;

            dl->AddText(ImVec2(rowMin.x + padX + chevronW + padX, textY),
                       ImGui::GetColorU32(ImGuiCol_Text), row.groupName.c_str());

            char countBuf[16];
            std::snprintf(countBuf, sizeof(countBuf), "%d", row.groupCount);
            const float countW = ImGui::CalcTextSize(countBuf).x;
            dl->AddText(ImVec2(rowMin.x + rowWidth - padX - countW, textY),
                       ImGui::GetColorU32(ImGuiCol_TextDisabled), countBuf);

            ImGui::PopID();
        }

        // ---- Task 10: one top-level asset row (spec s6/s11.2) --------------
        void DrawAssetRow(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                          DocumentHost& docs, const AssetsPanelServices& services, AssetsPanelActions& actions,
                          const AssetPanelEntry& e, const Arcane::Guid& bootGuid)
        {
            ImGui::PushID(e.guid.ToString().c_str());

            const bool hasChildren = (e.kind == AssetKind::Texture) && !e.derivedChildren.empty();
            const bool childrenOpen = hasChildren && ChildrenAreOpen(state, e.guid);
            const bool refused = (e.cook == CookState::Refused);
            // The expander gutter is reserved only for textures with a
            // folded child -- refused now wears its OWN corner badge on the
            // thumb below (fix round 1, Important 5), so it never competes
            // with the expander for the same slot.
            const float indent = hasChildren ? kChildIndent : 0.0f;

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
                ImGui::SetCursorScreenPos(rowMin);
                const std::string expId = "##exp_" + e.guid.ToString();
                if (ImGui::InvisibleButton(expId.c_str(), ImVec2(kChildIndent, kTableRowHeight)))
                {
                    const bool newOpen = !childrenOpen;
                    state.childrenOpen[e.guid] = newOpen;
                    model.SetChildrenOpen(e.guid, newOpen);
                }
                const char* chevron = childrenOpen ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT;
                const ImVec2 cs = ImGui::CalcTextSize(chevron);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rowMin.x + (kChildIndent - cs.x) * 0.5f, rowMin.y + (kTableRowHeight - cs.y) * 0.5f),
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
            if (hasChildren && !childrenOpen)
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
                          const AssetPanelEntry& e)
        {
            ImGui::PushID(e.guid.ToString().c_str());

            const std::uint64_t thumbId = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            const char* icon = KindIcon(e.kind);
            const bool selected = (model.selected == e.guid);

            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, kChildIndent, kTableRowHeight);
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
                                    DrawAssetRow(state, model, project, docs, services, actions, *e, bootGuid);
                                break;
                            case AssetPanelRow::Type::Child:
                                if (const AssetPanelEntry* e = model.Find(row.guid))
                                    DrawChildRow(state, model, project, docs, services, actions, *e);
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

        // ---- Task 11: the preview pane (spec s5/s6/s11.2) -------------------
        // Layout order, pinned by the brief: 140px thumb -> name + kind/
        // subkind/inst pills -> path row -> guid row (click copies) -> cook
        // row -> separator -> Derived (N) list -> separator -> full-width
        // action buttons (Open, Show in Explorer, Copy Path, + one
        // kind-specific action). Empty selection is a dim "no selection"
        // line -- no other row renders in that state.
        void DrawPreviewPane(AssetPanelModel& model, const Arcane::Project* project, DocumentHost& docs,
                            const AssetsPanelServices& services, AssetsPanelActions& actions)
        {
            if (!ImGui::BeginChild("##assetspreview", ImVec2(kPreviewPaneWidth, 0.0f), ImGuiChildFlags_None))
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
            {
                const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
                const ImVec2 thumbMax(thumbMin.x + kPreviewThumbSize, thumbMin.y + kPreviewThumbSize);
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
                    dl->AddText(ImVec2(thumbMin.x + (kPreviewThumbSize - iconSize.x) * 0.5f,
                                       thumbMin.y + (kPreviewThumbSize - iconSize.y) * 0.5f),
                               ImGui::GetColorU32(ImGuiCol_Text), icon);
                }
                dl->AddRect(thumbMin, thumbMax, ImGui::GetColorU32(Theme::kSeparator));
                ImGui::Dummy(ImVec2(kPreviewThumbSize, kPreviewThumbSize));
            }

            // ---- name (stem) + kind pill + subkind/inst pills
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

            // ---- path row (mount path, ellipsized to whatever's left on
            // the line after the "path" label)
            ImGui::TextDisabled("path");
            ImGui::SameLine();
            ImGui::TextUnformatted(EllipsisToWidth(e->mountPath, ImGui::GetContentRegionAvail().x).c_str());

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
            // (rather than per row) for the "boot" pill (spec s6). Empty/
            // unparseable bootScene resolves to the nil guid, which no real
            // asset can equal, so the pill simply never lights up.
            const Arcane::Guid bootGuid = project
                ? Arcane::Guid::FromString(project->Manifest().bootScene).value_or(Arcane::Guid::Nil())
                : Arcane::Guid::Nil();

            // Spec s5: preview pane fixed 330px, hidden below a 720px PANEL
            // width so the table never drops below readable width. Measured
            // here, before anything in this body has drawn -- at this exact
            // point ImGui's content-region-avail IS the whole rail+table+
            // preview budget for the frame, uncontested by anything this
            // function itself has submitted yet.
            const float panelWidth = ImGui::GetContentRegionAvail().x;
            const bool showPreview = panelWidth >= kPreviewHidePanelWidth;

            DrawRail(state, model, actions);
            ImGui::SameLine();

            // Reserve exactly kPreviewPaneWidth (plus both SameLine gaps)
            // for the preview when it is shown; 0.0f keeps DrawTable's own
            // "fill everything left on this line" default when it is not.
            const float tableWidth = showPreview
                ? std::max(0.0f, panelWidth - kRailWidth
                                  - ImGui::GetStyle().ItemSpacing.x * 2.0f - kPreviewPaneWidth)
                : 0.0f;
            DrawTable(state, model, project, docs, services, actions, bootGuid, tableWidth);

            if (showPreview)
            {
                ImGui::SameLine();
                DrawPreviewPane(model, project, docs, services, actions);
            }
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

        if (ImGui::BeginChild("##assetsbody", ImVec2(0.0f, -kBottomBarHeight)))
        {
            if (!project)
                ImGui::TextDisabled("No project open (data/-next-to-exe)");
            else if (state.lens == AssetLens::Browse)
                DrawBrowseLens(state, model, project, docs, services, actions);
            else
                ImGui::TextDisabled("Lens not available in Plan 1.");
        }
        ImGui::EndChild();

        DrawBottomBar(model);

        ImGui::End();
        return actions;
    }
}
