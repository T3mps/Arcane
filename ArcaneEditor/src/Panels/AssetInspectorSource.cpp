#include "Panels/AssetInspectorSource.hpp"
#include "Panels/AssetPanelCommon.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Panels/TextureImportSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Project/AssetId.hpp>   // AssetId::FromGuid (ResolveAsset's key)
#include <Arcane/Project/Project.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
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
        // are gone; these are the only copy). The thumb is 140px;
        // the action buttons are full-width and 24px tall (spec s11.2's
        // table row height, reused rather than inventing a new pinned
        // value).
        constexpr float kAssetPageThumbSize = 140.0f;
        constexpr float kActionButtonHeight = 24.0f;

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

    void AssetInspectorSource::Draw(PropertyGrid&)
    {
        if (!m_deps.model || !m_deps.services || !m_deps.actions) return;
        if (const AssetPanelEntry* e = m_deps.model->Find(m_drawGuid))
            DrawAssetPage(*e, *m_deps.model, m_deps.project, m_deps.docs, *m_deps.services, *m_deps.actions);
    }

    // ---- the Asset page (the old preview pane's content, spec s5/s6/s11.2) --
    // Layout order: 140px thumb -> name + kind/subkind/inst pills -> path row
    // -> guid row (click copies) -> cook row -> separator -> Derived (N) list
    // -> separator -> full-width action buttons (Open, Show in Explorer, Open
    // as text, Copy Path, + one kind-specific action) -> (texture) separator +
    // import settings. Drawn only for a resolved entry: the Inspector window
    // is the container, and it owns the "No selection" state.
    void DrawAssetPage(const AssetPanelEntry& entry, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost* docs, const AssetPanelServices& services, AssetPanelActions& actions)
    {
        const AssetPanelEntry* e = &entry;

        // ---- 140px thumb: real thumb when resolvable, else the kind
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
        };

        // ---- name/pills + path/guid/cook rows. A lambda for the same
        // reason as `drawThumb` -- identical content and logic at both call
        // sites, only the surrounding container differs. `EllipsisToWidth`'s
        // `GetContentRegionAvail().x` call measures the whole page in the
        // stacked branch, and the `##previewMeta` child's own (zero-padding)
        // width in the compact branch, simply by virtue of which window is
        // current when this runs -- the ImGui-native equivalent of the mock's
        // own `min-width: 0` + `overflow: hidden` ellipsis fix: a bounding
        // container, not a width argument threaded through.
        auto drawMeta = [&]()
        {
            // ---- name (stem) + kind pill + subkind/inst pills
            //
            // 2026-09-07 review note: unlike the `path` row below, the
            // name here has NO EllipsisToWidth clamp in either branch.
            // The compact column can be as narrow as
            // kPreviewCompactTextColumnMin (110px), and a long stem plus
            // its trailing kind/subkind/inst pills (all SameLine-chained)
            // can overflow it. Deferred: a correct clamp has to measure the
            // pill run's own width FIRST and budget the name against what's
            // left, not reuse EllipsisToWidth's single-string recipe.
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
            // left on the line after the "path" label. A plain text hover
            // tooltip carries the FULL mount path -- this is NOT the s8
            // 210px peek-tooltip contract (no thumb, no kind/cook rows),
            // just a path reveal.
            ImGui::TextDisabled("path");
            ImGui::SameLine();
            const std::string_view relPath = ContentRelativePath(e->mountPath);
            ImGui::TextUnformatted(EllipsisToWidth(relPath, ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", e->mountPath.c_str());

            // ---- guid row: dim, click copies (spec s6: "guid
            // (click-to-copy)"). Routed through `actions.copyGuid` -- the
            // SAME field the row context menu's "Copy Guid" entry sets -- so
            // the host's one existing consumer (EditorAppFrame.cpp's
            // `ImGui::SetClipboardText(...copyGuid...)`) needs no new wiring;
            // "panel reports, app performs" stays intact.
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

        // Side-by-side header at/above kPreviewCompactHeaderMinWidth, the
        // stacked form (thumb above, metadata below) below it -- measured
        // against the page's own content width.
        const bool compactHeader = ImGui::GetContentRegionAvail().x >= kPreviewCompactHeaderMinWidth;
        if (compactHeader)
        {
            // The 140px thumb only yields (the std::min clamp) when the page
            // is too narrow to also leave a kPreviewCompactTextColumnMin-wide
            // text column beside it.
            const float avail = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float thumbSize = std::min(kAssetPageThumbSize,
                std::max(0.0f, avail - spacing - kPreviewCompactTextColumnMin));
            const float textColumnWidth = std::max(0.0f, avail - thumbSize - spacing);

            ImGui::BeginGroup();
            drawThumb(thumbSize);
            ImGui::EndGroup();
            ImGui::SameLine();

            // Zero WindowPadding on this bounding-only column: it exists
            // purely to give `drawMeta`'s GetContentRegionAvail() calls a
            // column-width answer instead of a whole-page one (see
            // `drawMeta`'s own comment); a visible inset was never part
            // of the mock.
            //
            // 2026-09-07 review note: this child's HEIGHT is `thumbSize`
            // (116-140px at this breakpoint), coupled to the thumb, not
            // to `drawMeta`'s own content -- at today's metrics (Inter
            // 16px body, this row's four lines) the real content stands
            // ~80px, comfortably inside even the smallest compact
            // thumbSize. A future larger body font or display scale could
            // grow that content past `thumbSize` and start clipping/
            // scrolling the `cook` row inside the box.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            if (ImGui::BeginChild("##previewMeta", ImVec2(textColumnWidth, thumbSize), ImGuiChildFlags_None))
                drawMeta();
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        else
        {
            // Stacked: the thumb SCALES to whatever is actually available
            // (min-clamped against the 140px pinned size) rather than adding
            // a second centering codepath.
            const float thumbSize = std::min(kAssetPageThumbSize, ImGui::GetContentRegionAvail().x);
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
        // routing as double-click)") -- only when a DocumentHost is bound;
        // the trailing kind-specific action mirrors the row context menu's
        // own kind-specific entries exactly (same label text, same action
        // field).
        const ImVec2 btnSize(-FLT_MIN, kActionButtonHeight);
        if (ImGui::Button(ICON_LC_EXTERNAL_LINK " Open", btnSize) && docs)
            OpenAssetRow(*e, project, *docs, actions);
        if (ImGui::Button(ICON_LC_FOLDER_OPEN " Show in Explorer", btnSize))
            actions.showInExplorer = e->guid;
        if (ImGui::Button(ICON_LC_FILE_TEXT " Open as text", btnSize))
            actions.openAsText = e->guid;
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

        // ---- texture import settings (the old Inspector texture-asset
        // panel's four .meta knobs, F2b Task 13), resolved through the
        // project to the SOURCE file; nothing without a project.
        if (e->kind == AssetKind::Texture && project)
        {
            if (const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e->guid)))
            {
                ImGui::Separator();
                DrawTextureImportSettings(*path);
            }
        }
    }
}
