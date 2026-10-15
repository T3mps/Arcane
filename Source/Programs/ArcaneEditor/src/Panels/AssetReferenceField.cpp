#include "Panels/AssetReferenceField.hpp"

#include "Panels/AssetPanelCommon.hpp"    // PillWidth
#include "Panels/CreateAssetDialog.hpp"   // MaterialSurfacePillText (the Create dialog's own pill text)
#include "Widgets/EditorTheme.hpp"        // Theme::kError (a dangling reference)
#include "Widgets/EditorWidgets.hpp"      // EllipsisToWidth, RowWithThumb, AssetPill, BeginPopupBelow
#include "Widgets/UiScale.hpp"            // UiStyle: editor.ui.assetRefThumbPx (settings S6-28)
#include "Widgets/IconsLucide.h"
#include "Widgets/PropertyGrid.hpp"

#include <cmath>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // ImRect, BeginDragDropTargetCustom (the whole-cell drop target)

#include <algorithm>
#include <cfloat>

namespace Arcane::Editor
{
    namespace
    {
        // spec s4.2's thumb: editor.ui.assetRefThumbPx (settings S6-28) at the
        // UI scale and font size, whole pixels; 20 px at the defaults.
        float AssetRefThumbSize() { return std::floor(Ui::TextPx(UiStyle().assetRefThumbPx)); }

        // One cache: one picker is open at a time. Refilled only when the
        // model's entries, the filters or the search change; a new popup
        // session (IsWindowAppearing) drops it.
        struct CandidateCache
        {
            const AssetPanelModel* model = nullptr;
            std::uint32_t entriesStamp = 0;
            int kind = -2, surface = -2;
            std::string search;
            std::vector<const AssetPanelEntry*> rows;
        };
        CandidateCache& Cache() { static CandidateCache c; return c; }
        char s_pickSearch[64] = {};   // the one open picker's search text

        const std::vector<const AssetPanelEntry*>& Candidates(const AssetPanelModel& model, int kind, int surface,
                                                              std::string_view search)
        {
            CandidateCache& c = Cache();
            if (c.model != &model || c.entriesStamp != model.entriesStamp || c.kind != kind
                || c.surface != surface || c.search != search)
            {
                c.model = &model;
                c.entriesStamp = model.entriesStamp;
                c.kind = kind;
                c.surface = surface;
                c.search.assign(search);
                c.rows = AssetRefCandidates(model, kind, surface, search);
            }
            return c.rows;
        }

        AssetRefEdit DrawPicker(const AssetRefArgs& args, const AssetRefServices& services)
        {
            AssetRefEdit edit;
            const Arcane::Project* project = services.project ? services.project() : nullptr;
            if (!project || !services.model)
            {
                ImGui::TextDisabled("No project open");
                return edit;
            }
            if (ImGui::IsWindowAppearing())
            {
                s_pickSearch[0] = '\0';
                Cache().model = nullptr;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##assetsearch", "Search...", s_pickSearch, sizeof(s_pickSearch));
            ImGui::Separator();
            if (ImGui::Selectable("(none)##assetnone", !args.guid.IsValid()))
                edit.op = AssetRefEdit::Op::Clear;
            const ImGuiStyle& st = ImGui::GetStyle();
            float widest = 0.0f;
            for (const AssetPanelEntry* e : Candidates(*services.model, args.kindFilter, args.surfaceFilter, s_pickSearch))
            {
                const std::uint64_t thumb = services.resolveThumb ? services.resolveThumb(e->guid) : 0;
                const char* pill = (e->kind == AssetKind::Material && e->surface)
                    ? MaterialSurfacePillText(*e->surface) : nullptr;
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                const AssetRowResult row = RowWithThumb(e->mountPath.c_str(), static_cast<ImTextureID>(thumb),
                                                        KindIcon(e->kind), e->fileName.c_str(), e->guid == args.guid, 0.0f);
                if (row.clicked) { edit.op = AssetRefEdit::Op::Set; edit.guid = e->guid; }
                if (pill)
                {
                    ImGui::SetCursorScreenPos(row.trailingPos);
                    AssetPill(pill);
                    // The row's TableRowHeight() Selectable + ItemSpacing.y (the
                    // pre-sweep 24 + spacing, now at the setting). NOTE a
                    // pill-less row pitches TableRowHeight() alone
                    // (RowWithThumb parks the cursor at its bottom), so a
                    // pill row sits ItemSpacing.y further down; kept as it
                    // was (identical at the defaults), owed a ruling.
                    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + TableRowHeight() + st.ItemSpacing.y));
                }
                widest = std::max(widest, AssetRowThumbSize() + st.ItemInnerSpacing.x * 2.0f
                                          + ImGui::CalcTextSize(e->fileName.c_str()).x + (pill ? PillWidth(pill) : 0.0f));
            }
            ImGui::Dummy(ImVec2(widest, 0.0f));   // the rows are overdraw: give the auto-fit their natural width
            return edit;
        }
    }

    AssetRefDisplay DescribeAssetRef(const AssetRefArgs& args, const AssetRefServices& services)
    {
        AssetRefDisplay d;
        if (args.kindFilter >= 0 && args.kindFilter < kAssetKindCount)
            d.kind = static_cast<AssetKind>(args.kindFilter);
        // Mixed FIRST, an Identity guid included: a multi-selection's ids always
        // differ, and the replaced arm read "--" for them (InspectorView.cpp's
        // refMixed branch ran before its identity check). Showing the primary's
        // id would present one entity's identity as the whole selection's.
        if (args.mixed) { d.text = "--"; return d; }
        // Identity guids are not references: the registry can never resolve
        // one, so asking would flag every healthy entity "(missing)".
        if (args.identityGuid)
        {
            d.text = args.guid.IsValid() ? args.guid.ToString() : std::string("(none)");
            d.kind = AssetKind::Other;
            return d;
        }
        if (!args.guid.IsValid()) { d.text = "(none)"; return d; }
        const Arcane::Project* project = services.project ? services.project() : nullptr;
        if (!project) { d.text = args.guid.ToString(); return d; }   // null services: raw guid, never dangling
        if (const std::optional<std::string> mount = project->Registry().Resolve(args.guid))
        {
            d.tooltip = *mount;
            d.browsable = true;
            if (const AssetPanelEntry* e = services.model ? services.model->Find(args.guid) : nullptr)
            {
                d.text = e->fileName;
                d.kind = e->kind;
            }
            else
            {
                // The model has not seen the guid yet (rebuilt next frame).
                const std::size_t slash = mount->rfind('/');
                d.text = slash == std::string::npos ? *mount : mount->substr(slash + 1);
                d.kind = AssetKindOf(*mount);
            }
            return d;
        }
        // Dangling: what it was called (T5's tombstone) else the raw guid.
        const std::optional<std::string> tomb =
            services.tombstoneName ? services.tombstoneName(args.guid) : std::nullopt;
        d.text = (tomb ? *tomb : args.guid.ToString()) + " (missing)";
        if (tomb) d.tooltip = args.guid.ToString() + " (missing)";
        d.dangling = true;
        return d;
    }

    AssetRefDropVerdict DecideAssetRefDrop(const AssetDragPayload& payload, const AssetRefArgs& args)
    {
        // Read-only means read-only on every path in: drag-drop acceptance
        // never consults ImGuiItemFlags_Disabled (InspectorView.cpp's history:
        // an Identity::id drop once wrote an asset guid into every selected
        // entity).
        if (args.readOnly || args.identityGuid) return AssetRefDropVerdict::Refuse;
        if (args.kindFilter < 0 || static_cast<int>(payload.kind) == args.kindFilter) return AssetRefDropVerdict::Set;
        if (args.kindFilter == static_cast<int>(AssetKind::Sprite) && payload.kind == AssetKind::Texture
            && args.allowTextureMint)
            return AssetRefDropVerdict::MintSprite;
        return AssetRefDropVerdict::Refuse;
    }

    std::vector<const AssetPanelEntry*> AssetRefCandidates(const AssetPanelModel& model, int kindFilter,
                                                           int surfaceFilter, std::string_view search)
    {
        std::vector<const AssetPanelEntry*> out;
        for (const auto& [guid, e] : model.Entries())
        {
            if (kindFilter >= 0 && static_cast<int>(e.kind) != kindFilter) continue;
            // "Show what we know, say nothing about what we don't": exclude only
            // a CONFIRMED differing surface (InspectorView.cpp:1241-1283's rule).
            if (surfaceFilter >= 0 && e.kind == AssetKind::Material && e.surface
                && static_cast<int>(*e.surface) != surfaceFilter)
                continue;
            if (!search.empty() && !MatchesFilter(AssetEntry{ e.guid, e.mountPath, e.name, e.kind }, -1, search))
                continue;
            out.push_back(&e);
        }
        std::sort(out.begin(), out.end(), [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                  { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; });
        return out;
    }

    AssetRefEdit AssetReferenceValue(const char* id, const AssetRefArgs& args, const AssetRefServices& services)
    {
        AssetRefEdit edit;
        // A caller's pending SetNextItemWidth (a PropertyGrid reserved reset
        // strip, s5.3) is honoured: read it here, before the thumb's Dummy would
        // consume it, then clear it. Without one the cell spans the available
        // width (CalcItemWidth would return the table's ItemWidth instead).
        ImGuiContext& g = *GImGui;
        const float cellW = std::max((g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth)
                                         ? ImGui::CalcItemWidth() : ImGui::GetContentRegionAvail().x, 1.0f);
        g.NextItemData.ClearFlags();
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return edit;
        ImGui::PushID(id);
        const AssetRefDisplay d = DescribeAssetRef(args, services);
        const ImGuiStyle& st = ImGui::GetStyle();
        const float frameH = ImGui::GetFrameHeight();
        ImGui::AlignTextToFramePadding();   // the name rides level with framed neighbours
        const ImVec2 cellMin = ImGui::GetCursorScreenPos();
        const ImRect cell(cellMin, ImVec2(cellMin.x + cellW, cellMin.y + frameH));

        const bool showPicker = !args.readOnly;
        const bool showReveal = d.browsable;
        const bool showClear = !args.readOnly && (args.guid.IsValid() || args.mixed);
        auto iconW = [&](const char* icon) { return ImGui::CalcTextSize(icon).x + st.FramePadding.x * 2.0f + st.ItemSpacing.x; };
        const float buttonsW = (showPicker ? iconW(ICON_LC_CHEVRON_DOWN) : 0.0f) + (showReveal ? iconW(ICON_LC_LOCATE) : 0.0f)
                             + (showClear ? iconW(ICON_LC_X) : 0.0f);

        // Thumb: overdraw on a Dummy that reserves its seat (no interactive item).
        ImGui::Dummy(ImVec2(AssetRefThumbSize(), frameH));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 lo = ImGui::GetItemRectMin();
            const bool real = args.guid.IsValid() && !args.mixed && !args.identityGuid;
            const std::uint64_t thumb = real && services.resolveThumb ? services.resolveThumb(args.guid) : 0;
            if (thumb != 0)
            {
                const float y = lo.y + (frameH - AssetRefThumbSize()) * 0.5f;
                dl->AddImage(static_cast<ImTextureID>(thumb), ImVec2(lo.x, y), ImVec2(lo.x + AssetRefThumbSize(), y + AssetRefThumbSize()));
            }
            else
            {
                const char* glyph = KindIcon(d.kind);
                const ImVec2 gs = ImGui::CalcTextSize(glyph);
                dl->AddText(ImVec2(lo.x + (AssetRefThumbSize() - gs.x) * 0.5f, lo.y + (frameH - gs.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), glyph);
            }
        }
        ImGui::SameLine();

        // Name: "###name" -- the id survives the text changing under a pick.
        const float nameW = std::max(cellMin.x + cellW - ImGui::GetCursorScreenPos().x - buttonsW, 16.0f);
        const std::string shown = EllipsisToWidth(d.text, nameW);
        edit.truncated = shown.size() != d.text.size();
        if (d.dangling) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kError);
        const bool nameClicked = ImGui::Selectable((shown + "###name").c_str(), false,
                                                   ImGuiSelectableFlags_AllowDoubleClick, ImVec2(nameW, 0.0f));
        if (d.dangling) ImGui::PopStyleColor();
        edit.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
        if (nameClicked && d.browsable && services.open && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            services.open(args.guid);   // QUEUED by the app: never opened mid-draw
        // The mount path when browsable (it moved off the face into the
        // tooltip), else the untruncated text when the face cut it.
        edit.fullText = !d.tooltip.empty() ? d.tooltip : (edit.truncated ? d.text : std::string{});
        if (args.ownTooltip && edit.hovered && !edit.fullText.empty())
            ImGui::SetTooltip("%s", edit.fullText.c_str());

        bool pickPressed = false;
        if (showPicker)
        {
            ImGui::SameLine();
            pickPressed = ImGui::SmallButton(ICON_LC_CHEVRON_DOWN "##pick");
            ImGui::SetItemTooltip("Pick an asset");
        }
        if (showReveal)
        {
            ImGui::SameLine();
            const bool canReveal = services.reveal && services.canReveal && services.canReveal();
            ImGui::BeginDisabled(!canReveal);
            if (ImGui::SmallButton(ICON_LC_LOCATE "##reveal")) services.reveal(args.guid);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(canReveal ? "Show in Asset Browser" : "Show in Asset Browser (the Asset Browser is closed)");
        }
        if (showClear)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_LC_X "##clear")) edit.op = AssetRefEdit::Op::Clear;
            ImGui::SetItemTooltip("Clear reference");
        }

        if (pickPressed) ImGui::OpenPopup("##assetpick");
        if (BeginPopupBelow("##assetpick", PopupAnchor{ cell.Min, cell.Max }, cellW))
        {
            const AssetRefEdit picked = DrawPicker(args, services);
            if (picked.op != AssetRefEdit::Op::None) { edit.op = picked.op; edit.guid = picked.guid; }
            ImGui::EndPopup();
        }

        // The whole cell is the drop target. A read-only cell opens none (the
        // disabled wrap does NOT gate drag-drop: imgui.cpp:15823-15834). A
        // refused kind is never accepted, so it never highlights.
        if (!args.readOnly && ImGui::BeginDragDropTargetCustom(cell, ImGui::GetID("##drop")))
        {
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            if (peek && peek->IsDataType(kAssetDragType) && peek->DataSize == static_cast<int>(sizeof(AssetDragPayload)))
            {
                const AssetDragPayload dragged = *static_cast<const AssetDragPayload*>(peek->Data);
                const AssetRefDropVerdict verdict = DecideAssetRefDrop(dragged, args);
                if (verdict != AssetRefDropVerdict::Refuse && ImGui::AcceptDragDropPayload(kAssetDragType))
                {
                    if (verdict == AssetRefDropVerdict::Set) { edit.op = AssetRefEdit::Op::Set; edit.guid = dragged.guid; }
                    else if (services.mintSpriteForTexture)
                    {
                        // Mints (or reuses) the .arcsprite OUTSIDE the caller's
                        // undo, as before: undo covers only the Guid edit.
                        if (const Arcane::Guid minted = services.mintSpriteForTexture(dragged.guid); minted.IsValid())
                        { edit.op = AssetRefEdit::Op::Set; edit.guid = minted; }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return edit;
    }

    AssetRefEdit AssetRefRow(PropertyGrid& grid, const char* label, const AssetRefArgs& args,
                             const AssetRefServices& services)
    {
        // The decorated-row cell (s5.3): honours SetNextRowDecor (the override
        // cell / reset slot) and resets LastRowEvents like the built-in rows.
        grid.BeginCustomRow(label, args.readOnly);
        const AssetRefEdit edit = AssetReferenceValue("##value", args, services);
        grid.EndCustomRow(label);   // probes `label`, pops the PushID: ids unchanged
        return edit;
    }
}
