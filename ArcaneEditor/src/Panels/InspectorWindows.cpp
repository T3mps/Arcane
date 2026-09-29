#include "Panels/InspectorWindows.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName (the primary's dock node for a new instance)

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // What the header's clicks ASK for. DrawHeader takes the host by
        // const reference and only records here; ApplyHeaderActions performs
        // them after ImGui::End(). Every one of these (GoBack/GoForward/JumpTo
        // -> RefreshCursorLabel -> Page(); SetPinned -> CanPin -> PageFor; a
        // crumb's select -> the source's selection) can re-target the view
        // the instance resolved, so none may run between the page resolve
        // and page->Draw (InspectorSource.hpp's page-view contract).
        struct HeaderActions
        {
            bool back = false;
            bool forward = false;
            std::optional<std::size_t> jump;          // a history entry picked from a right-click list
            bool togglePin = false;
            bool unpin = false;                        // the pinned page-less note's "click to follow"
            std::optional<std::string> repinKey;       // a pinned instance's crumb
            std::function<void()> select;              // an unpinned instance's crumb
        };

        void ApplyHeaderActions(InspectorHost& host, const InspectorHost::Instance& inst, HeaderActions& a)
        {
            if (a.back) (void)host.GoBack();
            if (a.forward) (void)host.GoForward();
            if (a.jump) (void)host.JumpTo(*a.jump);
            if (a.repinKey) host.RepinKey(inst.id, std::move(*a.repinKey));
            if (a.select) a.select();
            if (a.togglePin) host.SetPinned(inst.id, !inst.pinned);
            if (a.unpin) host.SetPinned(inst.id, false);
        }

        void DrawHeader(const InspectorHost& host, const InspectorHost::Instance& inst, InspectorSource* src,
                        InspectorPage* page, bool canPin, HeaderActions& actions)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float pinWidth = ImGui::CalcTextSize(ICON_LC_PIN).x + style.FramePadding.x * 2.0f;

            // Back / forward over the selection history: each arrow names its
            // target, and a right-click lists that side's entries nearest-first
            // (UE's Content Browser history). The disabled state suppresses both.
            ImGui::BeginDisabled(!host.CanGoBack());
            if (ImGui::SmallButton(ICON_LC_CHEVRON_LEFT "##back")) actions.back = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                if (const auto* e = host.BackEntry()) ImGui::SetTooltip("Back to %s", e->label.c_str());
            if (ImGui::BeginPopupContextItem("##back_history"))
            {
                const std::size_t cur = host.HistoryCursor();
                for (std::size_t i = cur; i-- > 0;)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(host.History()[i].label.c_str())) actions.jump = i;
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!host.CanGoForward());
            if (ImGui::SmallButton(ICON_LC_CHEVRON_RIGHT "##forward")) actions.forward = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                if (const auto* e = host.ForwardEntry()) ImGui::SetTooltip("Forward to %s", e->label.c_str());
            if (ImGui::BeginPopupContextItem("##forward_history"))
            {
                for (std::size_t i = host.HistoryCursor() + 1; i < host.History().size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(host.History()[i].label.c_str())) actions.jump = i;
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            // Breadcrumb: a horizontal strip clipped to the width left of the pin
            // and scrolled to its END, so the LEAF stays visible and the head
            // scrolls off (UE's SBreadcrumbTrail); the pin can never overdraw the
            // leaf in a narrow docked Inspector. Every crumb is a link-styled
            // button; a dimmed chevron sits only BETWEEN crumbs.
            std::vector<InspectorCrumb> crumbs;
            if (page) crumbs = page->Breadcrumb();
            else if (src) crumbs.push_back({ src->SourceName(), {}, std::nullopt });
            const float crumbWidth = ImGui::GetContentRegionAvail().x - pinWidth - style.ItemSpacing.x;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##crumbs", ImVec2(std::max(crumbWidth, 1.0f), ImGui::GetFrameHeight()), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // link-styled: the theme's 1 px frame border would box every crumb
            for (std::size_t i = 0; i < crumbs.size(); ++i)
            {
                if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled(ICON_LC_CHEVRON_RIGHT); ImGui::SameLine(); }
                ImGui::PushID(static_cast<int>(i));
                if (inst.pinned)
                {
                    // A pinned instance navigates ITSELF: re-target the pin, never the source.
                    ImGui::BeginDisabled(!crumbs[i].key.has_value());
                    if (ImGui::SmallButton(crumbs[i].label.c_str()) && crumbs[i].key)
                        actions.repinKey = *crumbs[i].key;
                    ImGui::EndDisabled();
                }
                else if (ImGui::SmallButton(crumbs[i].label.c_str()) && crumbs[i].select)
                    actions.select = crumbs[i].select;
                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if (ImGui::GetScrollMaxX() > 0.0f) ImGui::SetScrollX(ImGui::GetScrollMaxX());   // last frame's width: one-frame lag, invisible at 90 frames + settle
            ImGui::EndChild();   // always, whatever BeginChild returned
            ImGui::PopStyleVar();
            ImGui::SameLine();   // the child's fixed width right-aligns the pin

            // The pin. Amber = pinned (the editor's acting-on hue). Offered only
            // for a resolvable page (UE's details lock exists only while objects
            // are viewed); the "closed"/"gone" notes stay for the page-less state.
            ImGui::BeginDisabled(!canPin);
            if (inst.pinned) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kAmber);
            if (ImGui::SmallButton(inst.pinned ? ICON_LC_PIN "##pin" : ICON_LC_PIN_OFF "##pin"))
                actions.togglePin = true;
            if (inst.pinned) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(inst.pinned ? "Pinned: this page stays while other things select. Click to follow."
                                  : canPin    ? "Pin this page"
                                              : "Nothing to pin");
            ImGui::EndDisabled();
            ImGui::Separator();
        }
    }

    InspectorWindowsResult DrawInspectorWindows(InspectorHost& host, InspectorWindowsState& state,
                                                bool* primaryOpen)
    {
        InspectorWindowsResult result;
        host.PruneStale();   // once per frame, <= kHistoryDepth pure lookups: the arrows below are truthful (spec s6 rule 3)
        // Snapshot the instances: the header actions applied after each End() mutate host state.
        const std::vector<InspectorHost::Instance> instances = host.Instances();
        for (const InspectorHost::Instance& inst : instances)
        {
            // Flush text drafts deactivated while this instance was hidden or its
            // page switched -- BEFORE Begin, whether or not the window shows.
            PropertyGrid(state.grids[inst.id]).CommitOrphans();
            if (inst.id == 0 && primaryOpen && !*primaryOpen) continue;
            const std::string title = inst.id == 0
                ? std::string("Inspector")
                : "Inspector " + std::to_string(inst.id + 1) + "###inspector_" + std::to_string(inst.id);
            bool open = true;
            if (inst.id != 0)
                if (ImGuiWindow* primary = ImGui::FindWindowByName("Inspector"))
                    if (primary->DockId != 0)
                        ImGui::SetNextWindowDockID(primary->DockId, ImGuiCond_FirstUseEver);
            // BEFORE resolving the page: CanPin calls PageFor on the current
            // source, which re-targets the scene source's draw selection; the
            // Page()/PageFor() call below restores it. From that resolve to
            // page->Draw NOTHING queries a source: the header only records its
            // clicks (HeaderActions), applied after End().
            const bool canPin = inst.pinned || host.CanPin();
            // NO `if (Begin)` on purpose: the scene page's EditGesture::ScopeGuard
            // must run on collapsed/background-tab frames too (EditGesture.hpp:
            // 263-274) or an abandoned drag transaction stays open for the next
            // consumer to JOIN. Every widget bails on SkipItems; Section/Rows/
            // RowWithThumb already guard it.
            (void)ImGui::Begin(title.c_str(), inst.id == 0 ? primaryOpen : &open);
            InspectorSource* src = host.SourceFor(inst.id);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) result.focusedSource = src;
            // Shortcut(), not IsKeyChordPressed(): RouteFocused means only the
            // focused instance fires, and an active InputText does not swallow
            // Ctrl+S (SpriteDocument.cpp:178-183 relies on the same fact).
            if (src && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) result.saveRequested.push_back(src);
            InspectorPage* page = nullptr;
            if (inst.pinned) page = src ? src->PageFor(inst.pinnedKey) : nullptr;
            else page = src ? src->Page() : nullptr;
            HeaderActions actions;
            DrawHeader(host, inst, src, page, canPin, actions);
            if (inst.pinned && !page)
            {
                // The pinned source closed or its selection went away: one line,
                // click to follow again (spec s3.3). NOT auto-released: registry
                // restore resurrects exact entity ids (delete->undo, Play->Stop),
                // so the page comes back by itself on the next PageFor.
                const std::string note = (inst.sourceClosed ? inst.pinnedName + " closed"
                                                            : "Pinned selection is gone")
                                         + " -- click to follow the selection";
                if (ImGui::Selectable(note.c_str())) actions.unpin = true;
            }
            else if (page)
            {
                PropertyGrid grid(state.grids[inst.id]);
                page->Draw(grid);
            }
            ImGui::End();
            ApplyHeaderActions(host, inst, actions);   // after the page is done with: see HeaderActions
            if (inst.id != 0 && !open) result.closed.push_back(inst.id);
        }
        return result;
    }
}
