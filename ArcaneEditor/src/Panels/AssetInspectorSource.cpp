#include "Panels/AssetInspectorSource.hpp"
#include "Panels/AssetPanelCommon.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Panels/TextureImportSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Project/AssetId.hpp>   // AssetId::FromGuid (ResolveAsset's key)
#include <Arcane/Project/Project.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        std::optional<Arcane::Guid> ParseKey(std::string_view key)
        {
            return key.empty() ? std::nullopt : Arcane::Guid::FromString(key);
        }

        // The page's fixed geometry, carried over from the Asset Browser's
        // old preview pane (inspector filters s6: the pane and its constants
        // are gone; these are the only copy). The thumb is at most 140px;
        // how far it shrinks in a short Inspector is the two cvars below.
        constexpr float kAssetPageThumbSize = 140.0f;

        ARC_CVAR_RANGED("editor.inspector.assetThumbMinPx", "editor", Int32,
                        ::Arcane::CVarValue::Int32(64), ::Arcane::CVarValue::Int32(32), ::Arcane::CVarValue::Int32(140),
                        ::Arcane::CVarFlags::Archive, "Smallest the asset page's thumbnail shrinks to in a short Inspector");
        ARC_CVAR_RANGED("editor.inspector.assetThumbHeightFraction", "editor", Float32,
                        ::Arcane::CVarValue::Float32(0.30f), ::Arcane::CVarValue::Float32(0.1f), ::Arcane::CVarValue::Float32(0.6f),
                        ::Arcane::CVarFlags::Archive, "Share of the Inspector's height the asset page's thumbnail may take");

        float ThumbFloor()
        {
            const Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
            const auto v = reg.Get(reg.Find("editor.inspector.assetThumbMinPx"));
            return (v && v->type == Arcane::CVarType::Int32) ? static_cast<float>(v->AsInt32()) : 64.0f;
        }
        float ThumbHeightFraction()
        {
            const Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
            const auto v = reg.Get(reg.Find("editor.inspector.assetThumbHeightFraction"));
            return (v && v->type == Arcane::CVarType::Float32) ? v->AsFloat32() : 0.30f;
        }

        // Compact side-by-side header (spec s6/s17): thumb left, name/pills/
        // path/guid/cook stacked beside it, instead of thumb-above-metadata.
        // kPreviewCompactHeaderMinWidth is the page's content width at and
        // above which the compact header draws; below it, the stacked form.
        // The spec calls the exact number an "implementer tuning value, not a
        // pinned constant" -- 250px is its own suggested figure.
        // kPreviewCompactTextColumnMin is the floor the thumb yields to when
        // the page is between this breakpoint and comfortably wide -- the
        // "~110px text column" figure from the same directive.
        constexpr float kPreviewCompactHeaderMinWidth = 250.0f;
        constexpr float kPreviewCompactTextColumnMin  = 110.0f;

        // Ruling 5 (2026-09-07): the path row shows the CONTENT-RELATIVE
        // path -- the mount's scheme prefix stripped. Strips whatever
        // precedes "://" -- not just the literal "game" scheme -- so every
        // mount stays readable. Falls back to the whole string unchanged if
        // there is no "://" at all (should not happen for a real mount path,
        // but this is display code, not a parser -- never assert on it).
        std::string_view ContentRelativePath(std::string_view mountPath)
        {
            const std::size_t sep = mountPath.find("://");
            return (sep == std::string_view::npos) ? mountPath : mountPath.substr(sep + 3);
        }

        // One Derived-list row (spec s6/s11.2): "Derived (N) list (each row:
        // sprite icon + name; click Selects the child)". `derivedChildren` is
        // always a 1:1 folded sprite (the model's own fold rule,
        // AssetPanelEntry::derivedChildren's doc comment: "1:1 sprites folded
        // under me"), so `KindIcon(child->kind)` reads as the sprite glyph
        // unconditionally -- resolved through the entry rather than
        // hardcoding the icon so a future fold rule change cannot silently
        // desync this row from what it actually names. Shares the same peek
        // tooltip every other representation uses (spec s8).
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
    }

    std::size_t ActionsThatFit(std::span<const float> widths, float moreWidth, float spacing, float avail) noexcept
    {
        float total = 0.0f;
        for (std::size_t i = 0; i < widths.size(); ++i)
            total += widths[i] + (i > 0 ? spacing : 0.0f);
        if (total <= avail)
            return widths.size();
        float used = moreWidth;
        std::size_t k = 0;
        while (k < widths.size() && used + widths[k] + spacing <= avail)
            used += widths[k++] + spacing;
        return k;
    }

    float AssetPageThumbSize(bool compact, float availX, float spacing, float innerHeight,
                             float heightFraction, float floorPx) noexcept
    {
        const float byHeight = std::clamp(heightFraction * innerHeight, floorPx, kAssetPageThumbSize);
        const float byWidth = compact ? std::max(0.0f, availX - spacing - kPreviewCompactTextColumnMin)
                                      : std::max(0.0f, availX);
        return std::min({ kAssetPageThumbSize, byWidth, byHeight });
    }

    std::string AssetInspectorSource::SelectionKey() const
    {
        return (m_deps.model && m_deps.model->selected.IsValid()) ? m_deps.model->selected.ToString() : std::string{};
    }

    std::uint64_t AssetInspectorSource::SelectionEpoch() const
    {
        return m_deps.model ? m_deps.model->selectionGesture : 0;
    }

    bool AssetInspectorSource::Resolves(std::string_view key) const
    {
        const auto g = ParseKey(key);
        return g && m_deps.model && m_deps.model->Find(*g) != nullptr;
    }

    bool AssetInspectorSource::RestoreSelection(std::string_view key)
    {
        if (!Resolves(key)) return false;
        m_deps.model->Select(*ParseKey(key));
        return true;
    }

    InspectorPage* AssetInspectorSource::Page()
    {
        if (!m_deps.model || !m_deps.model->selected.IsValid() || !m_deps.model->Find(m_deps.model->selected))
            return nullptr;
        m_drawGuid = m_deps.model->selected;
        return this;
    }

    InspectorPage* AssetInspectorSource::PageFor(std::string_view key)
    {
        if (!Resolves(key)) return nullptr;
        m_drawGuid = *ParseKey(key);
        return this;
    }

    std::vector<InspectorCrumb> AssetInspectorSource::Breadcrumb() const
    {
        std::vector<InspectorCrumb> crumbs;
        crumbs.push_back({ "Assets", [m = m_deps.model] { if (m) m->Select(Arcane::Guid{}); }, std::nullopt });
        if (m_deps.model)
            if (const AssetPanelEntry* e = m_deps.model->Find(m_drawGuid))
                crumbs.push_back({ e->fileName, [m = m_deps.model, g = e->guid] { m->Select(g); }, e->guid.ToString() });
        return crumbs;
    }

    void AssetInspectorSource::Draw(PropertyGrid& grid)
    {
        if (!m_deps.model || !m_deps.services || !m_deps.actions) return;
        if (const AssetPanelEntry* e = m_deps.model->Find(m_drawGuid))
            DrawAssetPage(grid, *e, *m_deps.model, m_deps.project, *m_deps.services, *m_deps.actions);
    }

    void DrawActionRow(std::span<const PageAction> list)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        // s5.6 at 1080p (T3 gate): the row's buttons run one pixel less
        // FramePadding.y than the stock frame (22 -> 20 px at the 16 px font;
        // the icon ink keeps 2 px clear of the border). Local to this row: the
        // shared PropertyGrid row metrics and Section bands are untouched.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(style.FramePadding.x, std::max(0.0f, style.FramePadding.y - 1.0f)));
        std::vector<float> widths;
        widths.reserve(list.size());
        for (const PageAction& a : list)
            widths.push_back(ImGui::CalcTextSize(a.icon).x + style.FramePadding.x * 2.0f);
        const float moreWidth = ImGui::CalcTextSize(ICON_LC_ELLIPSIS).x + style.FramePadding.x * 2.0f;
        const std::size_t shown = ActionsThatFit(widths, moreWidth, style.ItemSpacing.x, ImGui::GetContentRegionAvail().x);
        for (std::size_t i = 0; i < shown; ++i)
        {
            const PageAction& a = list[i];
            if (i > 0) ImGui::SameLine();
            ImGui::BeginDisabled(!a.enabled);
            const std::string label = std::string(a.icon) + a.id;
            if (ImGui::Button(label.c_str()) && a.run) a.run();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", a.tooltip);
        }
        if (shown == list.size())
        {
            ImGui::PopStyleVar();
            return;
        }
        if (shown > 0) ImGui::SameLine();
        if (ImGui::Button(ICON_LC_ELLIPSIS "##asset_more"))
            ImGui::OpenPopup("##asset_more");
        ImGui::PopStyleVar();   // before the popup: its menu items keep the stock frame
        ImGui::SetItemTooltip("More actions");
        const PopupAnchor anchor = LastItemAnchor();
        if (BeginPopupBelow("##asset_more", anchor))
        {
            for (std::size_t i = shown; i < list.size(); ++i)
            {
                const PageAction& a = list[i];
                const std::string label = std::string(a.icon) + " " + a.tooltip + a.id;
                if (ImGui::MenuItem(label.c_str(), nullptr, false, a.enabled) && a.run) a.run();
            }
            ImGui::EndPopup();
        }
    }

    // ---- the Asset page (the old preview pane's content, spec s5/s6/s11.2,
    // fitted to the Assets-only Inspector at 1080p by node-page s5.6)
    // Layout order: thumb (height-aware) beside or above name + kind/subkind/
    // inst pills -> path row -> guid row (click copies) -> cook row -> ONE icon
    // row (Open, Show in Explorer, Open as text, Copy Path, + one kind-specific
    // action; what does not fit overflows into ##asset_more) -> "Derived (N)"
    // section -> (texture) "Import" section. Drawn only for a resolved entry:
    // the Inspector window is the container, and it owns the "No selection"
    // state. Every action is REPORTED (AssetPanelActions); the app performs it
    // next frame -- Open included (ConsumeAssetPanelActions -> OpenAssetRow).
    void DrawAssetPage(PropertyGrid& grid, const AssetPanelEntry& entry, AssetPanelModel& model,
                       const Arcane::Project* project, const AssetPanelServices& services, AssetPanelActions& actions)
    {
        const AssetPanelEntry* e = &entry;
        // Read before anything is laid out: the page child's height (s5.7).
        const float innerHeight = ImGui::GetWindowHeight();

        // ---- the thumb (at most 140px): real thumb when resolvable, else the kind
        // icon centered over a `kWell` backdrop with a `kSeparator`
        // border seam (spec s6.1: "the Lucide kind icon on a well
        // background") -- the same image/icon composition
        // `DrawAssetPeekTooltip` uses at 64px, scaled up and framed.
        // A lambda because it is drawn from two call sites (compact/stacked
        // below) with only `thumbSize` differing.
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
            grid.ProbeItem("##asset_thumb");   // TEST SEAM: the drawn side (no-op in production)
        };

        // ---- name/pills + path/guid/cook rows. A lambda for the same
        // reason as `drawThumb` -- identical content and logic at both call
        // sites, only the surrounding container differs. Every
        // `GetContentRegionAvail().x` call below measures the whole page in
        // the stacked branch, and the `##previewMeta` child's own (zero-
        // padding) width in the compact branch, simply by virtue of which
        // window is current when this runs -- a bounding container, not a
        // width argument threaded through.
        auto drawMeta = [&]()
        {
            // Name budget = the line minus the pill run, measured first (the
            // 2026-09-07 deferred clamp); a cut name carries the full name as tooltip.
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const char* sub = SubkindPillText(*e);
            const float pills = PillWidth(KindLabel(e->kind)) + spacing
                              + (sub ? PillWidth(sub) + spacing : 0.0f)
                              + (e->isInstance ? PillWidth("inst") + spacing : 0.0f);
            const std::string name = EllipsisToWidth(e->name, std::max(0.0f, ImGui::GetContentRegionAvail().x - pills));
            ImGui::TextUnformatted(name.c_str());
            if (name != e->name) ImGui::SetItemTooltip("%s", e->name.c_str());
            ImGui::SameLine();
            AssetPill(KindLabel(e->kind));
            if (sub) { ImGui::SameLine(); AssetPill(sub); }
            if (e->isInstance) { ImGui::SameLine(); AssetPill("inst"); }

            // ---- path row: the content-relative path (scheme prefix
            // stripped -- ruling 5, 2026-09-07), ellipsized to what is left on
            // the line after the "path" label. A plain text hover tooltip
            // carries the FULL mount path -- NOT the s8 210px peek-tooltip
            // contract, just a path reveal.
            ImGui::TextDisabled("path");
            ImGui::SameLine();
            const std::string_view relPath = ContentRelativePath(e->mountPath);
            ImGui::TextUnformatted(EllipsisToWidth(relPath, ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", e->mountPath.c_str());

            // ---- guid row: dim, ellipsized, click copies. Routed through
            // `actions.copyGuid` -- the SAME field the row context menu's
            // "Copy Guid" entry sets -- so the host's one existing consumer
            // needs no new wiring; "panel reports, app performs" stays intact.
            ImGui::TextDisabled("guid");
            ImGui::SameLine();
            const std::string guid = e->guid.ToString();
            ImGui::TextDisabled("%s", EllipsisToWidth(guid, ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetItemTooltip("%s\nclick to copy", guid.c_str());
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

        // Side-by-side header at/above kPreviewCompactHeaderMinWidth, the
        // stacked form (thumb above, metadata below) below it -- measured
        // against the page's own content width. The thumb follows the s5.6
        // formula (AssetPageThumbSize): never above 140, never wider than the
        // form allows, and a share of the Inspector's height clamped up to the
        // cvar floor.
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const bool compactHeader = avail >= kPreviewCompactHeaderMinWidth;
        float thumbSize = AssetPageThumbSize(compactHeader, avail, spacing, innerHeight,
                                             ThumbHeightFraction(), ThumbFloor());
        if (compactHeader)
        {
            // The compact header row is never taller than its text column (T3
            // gate, s5.6 at 1080p): the thumb is capped at the ##previewMeta
            // column's measured content height, but never below the cvar
            // floor. The height is measured inside the column (it does not
            // depend on the column's width: every line ellipsizes) and kept in
            // this window's state storage, so the first frame draws the plain
            // formula and every later frame -- from the second on -- the cap.
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID metaHeightKey = ImGui::GetID("##previewMetaHeight");
            if (const float metaHeight = storage->GetFloat(metaHeightKey, 0.0f); metaHeight > 0.0f)
                thumbSize = std::min(thumbSize, std::max(ThumbFloor(), metaHeight));
            ImGui::BeginGroup();
            drawThumb(thumbSize);
            ImGui::EndGroup();
            ImGui::SameLine();
            // Zero WindowPadding on this bounding-only column: it exists to give
            // `drawMeta`'s GetContentRegionAvail() calls a column-width answer.
            // Sized by its content, not the thumb (s5.6): a shrunken thumb never
            // clips the cook row.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            float measured = 0.0f;
            if (ImGui::BeginChild("##previewMeta", ImVec2(std::max(0.0f, avail - thumbSize - spacing), 0.0f),
                                  ImGuiChildFlags_AutoResizeY))
            {
                drawMeta();
                // Zero padding: the cursor starts at y 0 and sits one ItemSpacing
                // below the last (cook) line.
                measured = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            if (measured > 0.0f)
                storage->SetFloat(metaHeightKey, measured);
        }
        else
        {
            drawThumb(thumbSize);
            drawMeta();
        }

        // One icon row; actions stay REPORTED (the app drains them next frame).
        // The trailing kind-specific action mirrors the row context menu's own
        // kind-specific entries (same label text, same action field).
        std::vector<PageAction> row;
        row.push_back({ ICON_LC_EXTERNAL_LINK, "Open", "##asset_open", true, [&] { actions.openAsset = e->guid; } });
        row.push_back({ ICON_LC_FOLDER_OPEN, "Show in Explorer", "##asset_explorer", true, [&] { actions.showInExplorer = e->guid; } });
        row.push_back({ ICON_LC_FILE_TEXT, "Open as text", "##asset_astext", true, [&] { actions.openAsText = e->guid; } });
        row.push_back({ ICON_LC_COPY, "Copy Path", "##asset_copypath", true, [&] { actions.copyPath = e->guid; } });
        if (e->kind == AssetKind::Material)
            row.push_back({ ICON_LC_LAYERS, "New Instance...", "##asset_newinstance", true, [&] { actions.createInstanceOf = e->guid; } });
        else if (e->kind == AssetKind::Scene)
            row.push_back({ ICON_LC_FLAG, "Set as Boot Scene", "##asset_bootscene", true, [&] { actions.setBootScene = e->guid; } });
        else if (e->kind == AssetKind::Texture)
            row.push_back({ ICON_LC_STICKER, "Create Sprite", "##asset_createsprite", true, [&] { actions.createSpriteFrom = e->guid; } });
        // s5.6 at 1080p (T3 gate): half the stock ItemSpacing.y between the
        // header (thumb + text column, or the stacked meta) and the action row
        // (4 -> 2 px). Local to this one gap, like the row's FramePadding.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.5f);
        DrawActionRow(row);

        // Section bands replace the separators (critique Inspector #10).
        char derivedHeader[48];
        std::snprintf(derivedHeader, sizeof(derivedHeader), "Derived (%d)###derived",
                      static_cast<int>(e->derivedChildren.size()));   // ###: the open state survives a count change
        if (grid.Section(derivedHeader))
            for (const Arcane::Guid& childGuid : e->derivedChildren)
                DrawDerivedRow(model, services, childGuid);

        // ---- texture import settings (the old Inspector texture-asset
        // panel's four .meta knobs, F2b Task 13), resolved through the
        // project to the SOURCE file; nothing without a project.
        if (e->kind == AssetKind::Texture && project)
            if (const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e->guid)))
                if (grid.Section("Import"))
                    DrawTextureImportSettings(grid, *path);
    }
}
