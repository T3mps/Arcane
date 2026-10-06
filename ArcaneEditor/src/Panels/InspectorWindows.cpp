#include "Panels/InspectorWindows.hpp"
#include "Input/EditorActions.hpp"

#include "Panels/InspectorKinds.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"
#include "Widgets/EditorWidgets.hpp"   // EllipsisToWidth, BeginPopupBelow, LastItemAnchor

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName (the primary's dock node for a new instance); ImGuiSettingsHandler

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
            std::optional<InspectorFilter> filter;     // the kind dropdown's new filter (spec s5)
        };

        void ApplyHeaderActions(InspectorHost& host, const InspectorHost::Instance& inst, HeaderActions& a)
        {
            // The arrows walk the instance's FILTERED view of the shared
            // history from the entry it SHOWS (spec s4); a landing is still
            // one selection for everyone.
            if (a.back) (void)host.GoBack(inst.id);
            if (a.forward) (void)host.GoForward(inst.id);
            if (a.jump) (void)host.JumpTo(*a.jump);
            if (a.repinKey) host.RepinKey(inst.id, std::move(*a.repinKey));
            if (a.select) a.select();
            if (a.togglePin) host.SetPinned(inst.id, !inst.pinned);
            if (a.unpin) host.SetPinned(inst.id, false);
            if (a.filter) (void)host.SetFilter(inst.id, *a.filter);   // an all-excluded set is refused (the dropdown never offers one)
        }

        void DrawPin(const InspectorHost::Instance& inst, bool canPin, HeaderActions& actions)
        {
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
        }

        // The filter FACE, measured: at most `maxIcons` icons (InspectorFilterFace),
        // ItemInnerSpacing apart, then the "+N" overflow -- built HERE, once,
        // and drawn from `more` as measured.
        struct MeasuredFace
        {
            FilterFace face;
            std::string more;       // "+N", empty when nothing overflows
            float width = 0.0f;     // icons + gaps + "+N" (no frame chrome)
        };
        MeasuredFace MeasureFilterFace(const InspectorFilter& filter, int maxIcons, float gap)
        {
            MeasuredFace m;
            m.face = InspectorFilterFace(filter, maxIcons);
            for (const char* icon : m.face.icons) m.width += ImGui::CalcTextSize(icon).x;
            m.width += gap * static_cast<float>(m.face.icons.size() - 1);   // never empty (floor 1)
            if (m.face.overflow > 0)
            {
                m.more = "+" + std::to_string(m.face.overflow);
                m.width += gap + ImGui::CalcTextSize(m.more.c_str()).x;
            }
            return m;
        }

        void DrawHeader(const InspectorHost& host, const InspectorHost::Instance& inst, InspectorSource* src,
                        InspectorPage* page, bool canPin, HeaderActions& actions)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float pinWidth = std::max(ImGui::CalcTextSize(ICON_LC_PIN).x, ImGui::CalcTextSize(ICON_LC_PIN_OFF).x)
                                 + style.FramePadding.x * 2.0f;
            const std::string label = InspectorFilterLabel(inst.filter);
            // The filter face (user request 2026-09-30): the ticked kinds' icons
            // (All = one glyph), as many as fit, then "+N". The combo's width is
            // the face plus its chrome (the arrow + the face's padding).
            const float iconGap = style.ItemInnerSpacing.x;
            const float chrome = ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f;
            const MeasuredFace fullFace = MeasureFilterFace(inst.filter, static_cast<int>(kInspectorKinds.size()), iconGap);
            // The responsive layout (final fix H), measured before anything draws.
            InspectorHeaderMetrics metrics;
            metrics.avail = ImGui::GetContentRegionAvail().x;
            metrics.arrows = ImGui::CalcTextSize(ICON_LC_CHEVRON_LEFT).x + ImGui::CalcTextSize(ICON_LC_CHEVRON_RIGHT).x
                           + style.FramePadding.x * 4.0f + style.ItemSpacing.x;
            metrics.comboFull = fullFace.width + chrome;
            metrics.comboMin = MeasureFilterFace(inst.filter, 1, iconGap).width + chrome;
            metrics.pin = pinWidth;
            // The breadcrumb, built and MEASURED before the layout (spec 2026-09-30
            // s4.3): row 1 reserves its natural width, and the same widths fit it
            // when it gets a row of its own.
            std::vector<InspectorCrumb> crumbs;
            if (page) crumbs = page->Breadcrumb();
            else if (src) crumbs.push_back({ src->SourceName(), {}, std::nullopt });
            const float chevronW = ImGui::CalcTextSize(ICON_LC_CHEVRON_RIGHT).x + style.ItemSpacing.x * 2.0f;
            std::vector<float> crumbWidths;
            crumbWidths.reserve(crumbs.size());
            for (const InspectorCrumb& c : crumbs)
            {
                crumbWidths.push_back(ImGui::CalcTextSize(c.label.c_str(), nullptr, true).x + style.FramePadding.x * 2.0f);
                metrics.crumbsNatural += crumbWidths.back();
            }
            if (!crumbs.empty()) metrics.crumbsNatural += chevronW * static_cast<float>(crumbs.size() - 1);
            metrics.spacing = style.ItemSpacing.x;
            const InspectorHeaderLayout layout = LayoutInspectorHeader(metrics);
            const float rowStartX = ImGui::GetCursorPosX();

            // Back / forward over the selection history: each arrow names its
            // target, and a right-click lists that side's entries nearest-first
            // (UE's Content Browser history). The disabled state suppresses both.
            // Both walk only the entries this instance's filter admits (spec s4).
            ImGui::BeginDisabled(!host.CanGoBack(inst.id));
            if (ImGui::SmallButton(ICON_LC_CHEVRON_LEFT "##back")) actions.back = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                if (const auto* e = host.BackEntry(inst.id)) ImGui::SetTooltip("Back to %s", e->label.c_str());
            if (ImGui::BeginPopupContextItem("##back_history"))
            {
                for (const std::size_t i : host.BackIndices(inst.id))
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(host.History()[i].label.c_str())) actions.jump = i;
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!host.CanGoForward(inst.id));
            if (ImGui::SmallButton(ICON_LC_CHEVRON_RIGHT "##forward")) actions.forward = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                if (const auto* e = host.ForwardEntry(inst.id)) ImGui::SetTooltip("Forward to %s", e->label.c_str());
            if (ImGui::BeginPopupContextItem("##forward_history"))
            {
                for (const std::size_t i : host.ForwardIndices(inst.id))
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(host.History()[i].label.c_str())) actions.jump = i;
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();

            // The kind filter (spec s5): one checkbox per catalog kind, ticked =
            // admitted. The last ticked kind cannot be unticked (an instance
            // that can show nothing reads as broken). Drawn BEFORE the crumb
            // child, so the crumbs' GetContentRegionAvail() below already
            // excludes it (only the pin, drawn after, is subtracted by hand).
            // The FACE is icons, not text (user request 2026-09-30): the ticked
            // kinds' glyphs centered in the frame (All = one glyph), as many as
            // the layout's combo width allows, then "+N". The full text label
            // is the tooltip. (Final fix H's icon-only collapse below 200 px and
            // its ellipsized text preview are superseded: the face is always
            // icons, and at one icon + "+N" it is already the narrow form.)
            ImGui::SameLine();
            MeasuredFace face = fullFace;
            for (int fit = static_cast<int>(face.face.icons.size()); fit > 1 && face.width + chrome > layout.comboWidth; )
                face = MeasureFilterFace(inst.filter, --fit, iconGap);
            ImGui::SetNextItemWidth(layout.comboWidth);
            if (ImGui::BeginCombo("##filter", nullptr, ImGuiComboFlags_CustomPreview))
            {
                for (const InspectorKind& k : kInspectorKinds)
                {
                    bool ticked = inst.filter.Admits(k.id);
                    InspectorFilter next = inst.filter;
                    if (ticked) next.excluded.emplace_back(k.id);
                    else std::erase(next.excluded, std::string(k.id));
                    const bool lastTicked = ticked && next.ExcludesEveryKind();
                    // Checkbox, icon, name: one label, so the icon and the name toggle too.
                    const std::string row = std::string(k.icon) + " " + std::string(k.displayName);
                    ImGui::BeginDisabled(lastTicked);
                    if (ImGui::Checkbox(row.c_str(), &ticked)) actions.filter = next;
                    ImGui::EndDisabled();
                    if (lastTicked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("An Inspector must show at least one kind");
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginComboPreview())
            {
                // RenderText (not items): the preview hosts no interactive
                // elements, and the combo frame stays the last item for the tooltip.
                const ImRect preview = ImGui::GetCurrentContext()->ComboPreviewData.PreviewRect;
                float x = preview.Min.x + std::max((preview.GetWidth() - face.width) * 0.5f, style.FramePadding.x);
                const float y = preview.Min.y + style.FramePadding.y;
                float right = x;
                for (const char* icon : face.face.icons)
                {
                    ImGui::RenderText(ImVec2(x, y), icon);
                    right = x + ImGui::CalcTextSize(icon).x;
                    x = right + iconGap;
                }
                if (!face.more.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::RenderText(ImVec2(x, y), face.more.c_str());
                    ImGui::PopStyleColor();
                    right = x + ImGui::CalcTextSize(face.more.c_str()).x;
                }
                // RenderText does not advance the cursor: report the face's
                // extent so EndComboPreview keeps the preview clip when the
                // face overflows (its CursorMaxPos test), instead of dropping
                // it as for an empty preview. The fit loop keeps it inside today.
                ImGuiWindow* window = ImGui::GetCurrentWindow();
                window->DC.CursorMaxPos = ImMax(window->DC.CursorMaxPos, ImVec2(right, y + ImGui::GetTextLineHeight()));
                ImGui::EndComboPreview();
            }
            // The face carries no text: the tooltip is the full label (the
            // combo frame is the last item whether or not it is open).
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Filter: %s", label.c_str());

            // Wrapped header: the pin ends row 1 (right-aligned), and the
            // breadcrumb takes the whole of row 2 -- or the pin leads row 2
            // when even arrows + combo + pin do not fit. Never clipped.
            float crumbWidth = 0.0f;
            if (layout.crumbsOwnRow)
            {
                if (!layout.pinOnCrumbRow)
                {
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowStartX + metrics.avail - pinWidth));
                    DrawPin(inst, canPin, actions);
                    crumbWidth = metrics.avail;
                }
                else
                {
                    DrawPin(inst, canPin, actions);
                    ImGui::SameLine();
                    crumbWidth = metrics.avail - pinWidth - style.ItemSpacing.x;
                }
            }
            else
            {
                ImGui::SameLine();
                crumbWidth = ImGui::GetContentRegionAvail().x - pinWidth - style.ItemSpacing.x;
            }

            // Breadcrumb (s4.3). The LEAF is never hidden: when the trail is wider
            // than its row, head crumbs hide behind "..." (its popup lists them
            // head-first) and a leaf that still does not fit is ellipsized with
            // its full label as the tooltip. The child is only a clip safety net
            // now -- nothing scrolls it. Every crumb is a link-styled button; a
            // dimmed chevron sits only BETWEEN shown crumbs.
            const float stripW = std::max(crumbWidth, 1.0f);
            const float overflowW = ImGui::CalcTextSize(ICON_LC_ELLIPSIS).x + style.FramePadding.x * 2.0f;
            const CrumbFit fit = FitCrumbs(crumbWidths, chevronW, overflowW, stripW);
            const ImVec2 popupPadding = style.WindowPadding;   // read before the strip zeroes it: the "..." popup is a window
            // A crumb's click: a pinned instance re-targets ITSELF, an unpinned one selects.
            auto takeCrumb = [&](const InspectorCrumb& c)
            {
                if (inst.pinned) { if (c.key) actions.repinKey = *c.key; }
                else if (c.select) actions.select = c.select;
            };
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##crumbs", ImVec2(stripW, ImGui::GetFrameHeight()), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // link-styled: the theme's 1 px frame border would box every crumb
            if (fit.overflow)
            {
                if (ImGui::SmallButton(ICON_LC_ELLIPSIS "##crumbmore_btn")) ImGui::OpenPopup("##crumbmore");
                ImGui::SetItemTooltip("Show the hidden levels");
                const PopupAnchor anchor = LastItemAnchor();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);   // Begin reads it; pop right after
                const bool open = BeginPopupBelow("##crumbmore", anchor);
                ImGui::PopStyleVar();
                if (open)
                {
                    for (std::size_t i = 0; i < fit.firstShown; ++i)
                    {
                        const std::string row = crumbs[i].label + "##crumbhidden" + std::to_string(i);
                        ImGui::BeginDisabled(inst.pinned && !crumbs[i].key.has_value());   // the buttons' pinned branch
                        if (ImGui::Selectable(row.c_str())) takeCrumb(crumbs[i]);
                        ImGui::EndDisabled();
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine(); ImGui::TextDisabled(ICON_LC_CHEVRON_RIGHT); ImGui::SameLine();
            }
            for (std::size_t i = fit.firstShown; i < crumbs.size(); ++i)
            {
                if (i > fit.firstShown) { ImGui::SameLine(); ImGui::TextDisabled(ICON_LC_CHEVRON_RIGHT); ImGui::SameLine(); }
                ImGui::PushID(static_cast<int>(i));
                const bool leaf = i + 1 == crumbs.size();
                const std::string shown = leaf
                    ? EllipsisToWidth(crumbs[i].label, std::max(fit.leafMax - style.FramePadding.x * 2.0f, ImGui::CalcTextSize("...").x))
                    : crumbs[i].label;
                ImGui::BeginDisabled(inst.pinned && !crumbs[i].key.has_value());
                if (ImGui::SmallButton((shown + "###crumb").c_str())) takeCrumb(crumbs[i]);   // ### : the id survives the cut
                ImGui::EndDisabled();
                if (shown != crumbs[i].label) ImGui::SetItemTooltip("%s", crumbs[i].label.c_str());
                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::EndChild();   // always, whatever BeginChild returned
            ImGui::PopStyleVar();
            if (!layout.crumbsOwnRow)
            {
                ImGui::SameLine();   // the child's fixed width right-aligns the pin
                DrawPin(inst, canPin, actions);
            }
            ImGui::Separator();
        }
    }

    InspectorHeaderLayout LayoutInspectorHeader(const InspectorHeaderMetrics& m)
    {
        InspectorHeaderLayout l;
        const float sp = m.spacing;
        // Row 1 reserves the trail's natural width (s4.3): a 145 px trail that
        // only got 120 px stayed on row 1 and lost its head.
        const float crumbs = m.crumbsNatural > 0.0f ? m.crumbsNatural : kInspectorHeaderMinCrumbWidth;
        l.crumbsOwnRow = m.avail < m.arrows + sp + m.comboFull + sp + crumbs + sp + m.pin;
        // The icon face gives way before the pin does: on a wrapped header it
        // takes what row 1 leaves beside the arrows and the pin (DrawHeader
        // drops icons into "+N" to fit), never less than one icon + "+N".
        const float room = m.avail - m.arrows - m.pin - 2.0f * sp;
        l.comboWidth = std::max(std::min(m.comboFull, room), m.comboMin);
        l.pinOnCrumbRow = m.arrows + sp + l.comboWidth + sp + m.pin > m.avail;
        if (l.pinOnCrumbRow) l.crumbsOwnRow = true;
        return l;
    }

    CrumbFit FitCrumbs(std::span<const float> widths, float chevron, float overflowButton, float avail)
    {
        CrumbFit fit;
        if (widths.empty()) return fit;
        const std::size_t n = widths.size();
        float total = chevron * static_cast<float>(n - 1);
        for (const float w : widths) total += w;
        if (total <= avail || n == 1)
        {
            fit.leafMax = std::min(widths.back(), std::max(avail, 0.0f));
            return fit;
        }
        fit.overflow = true;
        fit.leafMax = widths.back();
        for (std::size_t k = 1; k < n; ++k)   // show k..n-1 behind the button
        {
            float need = overflowButton + chevron;
            for (std::size_t i = k; i < n; ++i) need += widths[i] + (i > k ? chevron : 0.0f);
            if (need <= avail) { fit.firstShown = k; return fit; }
        }
        fit.firstShown = n - 1;
        fit.leafMax = std::max(avail - overflowButton - chevron, 0.0f);
        return fit;
    }

    std::string InspectorWindowTitle(const InspectorHost::Instance& inst)
    {
        std::string title = inst.id == 0 ? std::string("Inspector") : "Inspector " + std::to_string(inst.id + 1);
        if (!inst.filter.IsAll()) title += " - " + InspectorFilterLabel(inst.filter);
        // The ### suffix is the window's whole id (ImHashStr restarts at it),
        // so the label may change freely. Instance 0's "###Inspector" hashes
        // to the legacy bare "Inspector" id: old layouts carry over as-is.
        title += inst.id == 0 ? std::string(kPrimaryInspectorWindowId) : "###inspector_" + std::to_string(inst.id);
        return title;
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
            const std::string title = InspectorWindowTitle(inst);
            bool open = true;
            if (inst.id != 0)
                if (ImGuiWindow* primary = ImGui::FindWindowByName(kPrimaryInspectorWindowId))
                    if (primary->DockId != 0)
                        ImGui::SetNextWindowDockID(primary->DockId, ImGuiCond_FirstUseEver);
            // BEFORE resolving the page: CanPin calls PageFor on the instance's
            // routed source, which re-targets the scene source's draw selection; the
            // Page()/PageFor() call below restores it. From that resolve to
            // page->Draw NOTHING queries a source: the header only records its
            // clicks (HeaderActions), applied after End().
            const bool canPin = inst.pinned || host.CanPin(inst.id);
            // NO `if (Begin)` on purpose: the scene page's EditGesture::ScopeGuard
            // must run on collapsed/background-tab frames too (EditGesture.hpp:
            // 263-274) or an abandoned drag transaction stays open for the next
            // consumer to JOIN. Every widget bails on SkipItems; Section/Rows/
            // RowWithThumb already guard it.
            (void)ImGui::Begin(title.c_str(), inst.id == 0 ? primaryOpen : &open);
            InspectorSource* src = host.SourceFor(inst.id);
            const bool instFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (instFocused)
            {
                result.focusedSource = src;
                EditorActions::Get().MarkContextActive(ActionContext::Inspector);
            }
            if (src && instFocused && EditorActions::Get().Pressed("document.save")) result.saveRequested.push_back(src);
            InspectorPage* page = nullptr;
            if (inst.pinned) page = src ? src->PageFor(inst.pinnedKey) : nullptr;
            else page = src ? src->Page() : nullptr;
            HeaderActions actions;
            DrawHeader(host, inst, src, page, canPin, actions);
            // s5.7: everything below the header scrolls in its own child, so a
            // tall page never takes the crumbs, the filter or the pin with it.
            // NO `if (BeginChild)`, for the same reason as the Begin above: the
            // page's EditGesture::ScopeGuard must run on collapsed, refused and
            // background frames; widgets bail on SkipItems. EndChild always.
            // NavFlattened keeps the child in the parent's focus route, so the
            // Shortcut(Ctrl+S) and IsWindowFocused above still see a focused page.
            (void)ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);
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
            else if (src == nullptr)
            {
                // Unpinned and nothing routed: the filter admits no registered
                // source (spec s5). A single-kind filter names its kind in the
                // singular ("No Material document open"). A string_view:
                // %.*s, never %s through "...".
                std::string_view onlyName;
                int admitted = 0;
                for (const InspectorKind& k : kInspectorKinds)
                    if (inst.filter.Admits(k.id)) { onlyName = k.singular; ++admitted; }
                if (admitted == 1)
                    ImGui::TextDisabled("No %.*s document open", static_cast<int>(onlyName.size()), onlyName.data());
                else
                    ImGui::TextDisabled("Nothing to show for this filter");
            }
            else
            {
                // Unpinned and routed, but the source has no page (the Asset
                // Browser with nothing selected): the one empty-state string,
                // the scene body's own (EditorPanels.cpp's "No selection").
                ImGui::TextDisabled("No selection");
            }
            ImGui::EndChild();
            ImGui::End();
            ApplyHeaderActions(host, inst, actions);   // after the page is done with: see HeaderActions
            if (inst.id != 0 && !open) result.closed.push_back(inst.id);
        }
        return result;
    }

    // ---- [EditorInspector][Instances]: the Inspector instance ID LIST -------
    // Extra ids only (instance 0 is implicit); an empty `Ids=` line restores
    // {0}. Ids are dock slots (ImGui keys `###inspector_<id>` settings on the
    // id), so a closed slot stays closed across a restart and Window > New
    // Inspector reopens the lowest free one. Filters are LAYOUT too and are
    // persisted on the line after it (inspector filters spec s7):
    // `Filters=<id>:<kind>+<kind>,...` lists each instance's EXCLUDED kinds;
    // an instance absent from it is All, and the line is always written, so a
    // load WITHOUT it (a pre-feature section, or no section at all) flags the
    // one-time legacy upgrade (spec s6; an empty `Filters=` is a real answer).
    // Pins are deliberately NOT persisted: a pin names a selection, and a
    // selection does not survive a restart (the reason history is never
    // persisted either, spec s6.3). Moved here from EditorApp (arc-1 debt F)
    // so the test exe can drive it.
    namespace
    {
        constexpr const char* kInstancesIniType = "EditorInspector";
        constexpr const char* kInstancesIniName = "Instances";

        void* InstancesSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
        {
            return std::strcmp(name, kInstancesIniName) == 0 ? handler->UserData : nullptr;
        }
        void InstancesSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
        {
            auto* host = static_cast<InspectorHost*>(entry);
            if (std::strncmp(line, "Ids=", 4) == 0)
            {
                std::vector<int> ids;
                for (const char* p = line + 4; *p;)
                {
                    char* end = nullptr;
                    const long v = std::strtol(p, &end, 10);
                    if (end == p) break;
                    ids.push_back(static_cast<int>(v));
                    p = (*end == ',') ? end + 1 : end;
                }
                host->SetInstanceIds(ids);   // 0, duplicates and out-of-range ids are dropped inside
            }
            else if (std::strncmp(line, "Filters=", 8) == 0)
            {
                // Read AFTER Ids= (it is written after it): SetFilter refuses
                // an id that does not exist yet.
                host->NoteFiltersLine();
                // Spec s7: an instance absent from the line is All. Reset every
                // existing instance first -- instance 0 and every survivor of
                // Ids= keep their old filter through SetInstanceIds otherwise.
                std::vector<int> existing;
                for (const auto& inst : host->Instances()) existing.push_back(inst.id);
                for (const int id : existing) (void)host->SetFilter(id, InspectorFilter{});
                std::string_view rest(line + 8);
                while (!rest.empty())
                {
                    const std::size_t comma = rest.find(',');
                    const std::string_view item = rest.substr(0, comma);
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                    const std::size_t colon = item.find(':');
                    if (colon == std::string_view::npos || colon == 0) continue;
                    int id = -1;
                    const auto [p, ec] = std::from_chars(item.data(), item.data() + colon, id);
                    if (ec != std::errc{} || p != item.data() + colon) continue;
                    InspectorFilter f;
                    std::string_view kinds = item.substr(colon + 1);
                    while (!kinds.empty())
                    {
                        const std::size_t plus = kinds.find('+');
                        if (plus != 0) f.excluded.emplace_back(kinds.substr(0, plus));
                        kinds = plus == std::string_view::npos ? std::string_view{} : kinds.substr(plus + 1);
                    }
                    f = f.Sanitized();                 // unknown kinds dropped; all-excluded -> All
                    (void)host->SetFilter(id, f);      // unknown id: refused, harmless
                }
            }
        }
        void InstancesSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
        {
            const auto* host = static_cast<const InspectorHost*>(handler->UserData);
            buf->appendf("[%s][%s]\nIds=", handler->TypeName, kInstancesIniName);
            bool first = true;
            for (const auto& inst : host->Instances())
                if (inst.id != 0) { buf->appendf(first ? "%d" : ",%d", inst.id); first = false; }
            // Always written, even empty: its absence is what marks a
            // pre-feature layout (spec s6/s7).
            buf->append("\nFilters=");
            bool firstF = true;
            for (const auto& inst : host->Instances())
            {
                if (inst.filter.IsAll()) continue;
                buf->appendf(firstF ? "%d:" : ",%d:", inst.id);
                firstF = false;
                for (std::size_t k = 0; k < inst.filter.excluded.size(); ++k)
                    buf->appendf(k ? "+%s" : "%s", inst.filter.excluded[k].c_str());
            }
            buf->append("\n\n");
        }
    }

    void RegisterInspectorInstancesSettings(InspectorHost& host)
    {
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kInstancesIniType) != nullptr)
            return;   // same idempotence guard as EditorApp's other ini handlers

        ImGuiSettingsHandler handler;
        handler.TypeName   = kInstancesIniType;
        handler.TypeHash   = ImHashStr(kInstancesIniType);
        handler.UserData   = &host;
        handler.ReadOpenFn = &InstancesSettingsReadOpen;
        handler.ReadLineFn = &InstancesSettingsReadLine;
        handler.WriteAllFn = &InstancesSettingsWriteAll;
        // Every ini load brackets its lines with ReadInit / ApplyAll: a load
        // that never saw a Filters= line (no section at all, or a pre-feature
        // one) flags the one-time legacy upgrade, which the app consumes
        // through InspectorHost::TakeLegacyLayoutUpgrade (spec s6).
        handler.ReadInitFn = [](ImGuiContext*, ImGuiSettingsHandler* h)
        { static_cast<InspectorHost*>(h->UserData)->NoteLayoutReadBegin(); };
        handler.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h)
        { static_cast<InspectorHost*>(h->UserData)->NoteLayoutReadEnd(); };
        // ImGui::ClearIniSettings (a windowed project switch, EditorApp::
        // RetargetLayoutIni) resets the list to exactly {0} and instance 0's
        // filter to All, so an incoming file without this section never
        // inherits the outgoing project's extra instances or filters. ONLY
        // here: the list and the filters are layout, not project state, so
        // InspectorHost::ReleaseAll deliberately keeps them.
        handler.ClearAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h)
        {
            auto* host = static_cast<InspectorHost*>(h->UserData);
            host->SetInstanceIds({});
            (void)host->SetFilter(0, InspectorFilter{});
        };
        ImGui::AddSettingsHandler(&handler);
    }
}
