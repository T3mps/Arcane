#include "Panels/AssetsPanel.hpp"

#include "Documents/DocumentHost.hpp"
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

        // ---- Task 10: shared per-row interaction anchor --------------------
        // BeginDragDropSource/BeginPopupContextItem/IsItemHovered(ForTooltip)
        // all key off "the last submitted item" -- and RowWithThumb's own
        // last item is its NAME text (drawn after the row's real Selectable),
        // not the full-row Selectable itself. So this draws ONE invisible
        // button back over the FULL row rect (rowMin.."row width"x
        // kTableRowHeight), submitted AFTER RowWithThumb, and hangs every
        // interaction that needs "last item" semantics off THAT instead.
        //
        // For this to receive hover at all despite sitting on top of
        // RowWithThumb's already-hovered Selectable, the CALLER must have
        // called ImGui::SetNextItemAllowOverlap() immediately before invoking
        // RowWithThumb (flagging ITS internal Selectable as overlappable) --
        // see DrawAssetRow/DrawChildRow. Selectable's OWN click (-> Select())
        // is unaffected: it already returned its `clicked` value at ITS OWN
        // submission time, before this button even exists.
        void DrawRowInteractions(AssetPanelModel& model, const Arcane::Project* project,
                                 DocumentHost& docs, const AssetsPanelServices& services,
                                 AssetsPanelActions& actions, const AssetPanelEntry& e,
                                 ImVec2 rowMin, bool kindSpecificMenu)
        {
            ImGui::SetCursorScreenPos(rowMin);
            const std::string hitId = "##hit_" + e.guid.ToString();
            ImGui::InvisibleButton(hitId.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, kTableRowHeight));

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

                    const AssetRowResult res = RowWithThumb("##rail", 0, icon, re.label.c_str(),
                                                            selected, 0.0f, kRailRowHeight);
                    if (res.clicked)
                    {
                        state.railKind = re.kind;
                        model.SetKindFilter(re.kind);
                    }

                    // Trailing block, right-aligned: an optional hover "+"
                    // (creatable kinds only) then the dim count (spec s6).
                    char countBuf[16];
                    std::snprintf(countBuf, sizeof(countBuf), "%d", re.count);
                    const float countW = ImGui::CalcTextSize(countBuf).x;
                    const bool showPlus = res.hovered && RailKindCreatable(re.kind);
                    const float plusW = showPlus
                        ? (ImGui::CalcTextSize(ICON_LC_PLUS).x + ImGui::GetStyle().FramePadding.x * 2.0f
                           + ImGui::GetStyle().ItemSpacing.x)
                        : 0.0f;
                    const float trailingW = countW + plusW;

                    ImGui::SameLine();
                    const float avail = ImGui::GetContentRegionAvail().x;
                    if (avail > trailingW)
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - trailingW);

                    if (showPlus)
                    {
                        if (ImGui::SmallButton(ICON_LC_PLUS))
                            actions.requestCreateKind = re.kind;
                        ImGui::SameLine();
                    }
                    ImGui::TextDisabled("%s", countBuf);

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
            // One leading gutter, used for EITHER the expander OR the
            // refused marker (a row needing both -- rare: a texture that is
            // both refused and has a folded child -- shows the expander;
            // the functional affordance wins over the status marker).
            const bool needsGutter = hasChildren || refused;
            const float indent = needsGutter ? kChildIndent : 0.0f;

            const std::uint64_t thumbId = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            const char* icon = KindIcon(e.kind);
            const bool selected = (model.selected == e.guid);

            ImGui::SetNextItemAllowOverlap();
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, indent, kTableRowHeight);
            if (res.clicked)
                model.Select(e.guid);

            if (needsGutter)
            {
                const char* glyph = hasChildren ? (childrenOpen ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT)
                                                : ICON_LC_TRIANGLE_ALERT;
                const ImU32 color = hasChildren ? ImGui::GetColorU32(ImGuiCol_Text)
                                                : ImGui::GetColorU32(Theme::kAmber);
                const ImVec2 gs = ImGui::CalcTextSize(glyph);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(rowMin.x + (kChildIndent - gs.x) * 0.5f, rowMin.y + (kTableRowHeight - gs.y) * 0.5f),
                    color, glyph);

                if (hasChildren &&
                    ImGui::IsMouseHoveringRect(ImVec2(rowMin.x, rowMin.y),
                                              ImVec2(rowMin.x + kChildIndent, rowMin.y + kTableRowHeight)) &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    const bool newOpen = !childrenOpen;
                    state.childrenOpen[e.guid] = newOpen;
                    model.SetChildrenOpen(e.guid, newOpen);
                }
            }

            // Trailing pills, in spec order: subkind, inst, boot, sliced,
            // derived-count.
            if (const char* sub = SubkindPillText(e))
            {
                ImGui::SameLine();
                AssetPill(sub);
            }
            if (e.isInstance)
            {
                ImGui::SameLine();
                AssetPill("inst");
            }
            if (e.kind == AssetKind::Scene && bootGuid.IsValid() && e.guid == bootGuid)
            {
                ImGui::SameLine();
                AssetPill("boot", 1);
            }
            if (e.kind == AssetKind::Sprite && e.sliced)
            {
                ImGui::SameLine();
                AssetPill("sliced");
            }
            if (hasChildren && !childrenOpen)
            {
                ImGui::SameLine();
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(e.derivedChildren.size()));
                AssetPill(buf);
            }

            DrawRowInteractions(model, project, docs, services, actions, e, rowMin, /*kindSpecificMenu=*/true);

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

            ImGui::SetNextItemAllowOverlap();
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const AssetRowResult res = RowWithThumb("##row", static_cast<ImTextureID>(thumbId), icon,
                                                    e.fileName.c_str(), selected, kChildIndent, kTableRowHeight);
            ImGui::PopStyleColor();
            if (res.clicked)
                model.Select(e.guid);

            ImGui::SameLine();
            AssetPill("derived");

            DrawRowInteractions(model, project, docs, services, actions, e, rowMin, /*kindSpecificMenu=*/false);

            ImGui::PopID();
        }

        // ---- Task 10: the table (spec s6/s11.2) ----------------------------
        void DrawTable(AssetsPanelState& state, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost& docs, const AssetsPanelServices& services, AssetsPanelActions& actions,
                       const Arcane::Guid& bootGuid)
        {
            if (!ImGui::BeginChild("##assetscenter", ImVec2(0.0f, 0.0f)))
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

            if (ImGui::BeginTable("##assets", 1,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings))
            {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(rows.size()), kTableRowHeight);
                if (scrollTargetIndex >= 0)
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
                            ImGui::SetScrollHereY();
                            state.seenSelectionStamp = model.selectionStamp;
                        }
                    }
                }
                ImGui::EndTable();
            }

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

        // ---- Task 10: the Browse lens body (rail + table) ------------------
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

            DrawRail(state, model, actions);
            ImGui::SameLine();
            DrawTable(state, model, project, docs, services, actions, bootGuid);
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
