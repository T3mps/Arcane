#include "Panels/AssetBrowser.hpp"

#include "Documents/DocumentHost.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>

namespace Arcane::Editor
{
    namespace
    {
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
                // ICON_LC_STICKER exists in IconsLucide.h (grepped; ICON_LC_GHOST is
                // also present but STICKER reads as "sprite" and is preferred by the
                // brief's fallback order).
                case AssetKind::Sprite:   return ICON_LC_STICKER;
                // ICON_LC_BUG exists in IconsLucide.h (grepped: IconsLucide.h:307) --
                // no nearest-match fallback needed.
                case AssetKind::Diagnostic: return ICON_LC_BUG;
                case AssetKind::Other:    return ICON_LC_FILE;
            }
            return ICON_LC_FILE;
        }

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
                case AssetKind::Other:    return "Other";
            }
            return "Other";
        }
    }

    AssetBrowserActions DrawAssetBrowserPanel(AssetBrowserState& state,
                                              const Arcane::Project* project,
                                              DocumentHost& docs,
                                              bool* open)
    {
        AssetBrowserActions actions;
        ImGui::Begin("Assets", open);
        if (!project)
        {
            ImGui::TextDisabled("No project open (data/-next-to-exe)");
            ImGui::End();
            return actions;
        }

        // Filter row: kind combo + search box.
        ImGui::SetNextItemWidth(120.0f);
        const char* filterLabel = state.kindFilter < 0
                                      ? "All"
                                      : KindLabel(static_cast<AssetKind>(state.kindFilter));
        if (ImGui::BeginCombo("##kindfilter", filterLabel))
        {
            if (ImGui::Selectable("All", state.kindFilter < 0))
                state.kindFilter = -1;
            for (int k = 0; k < kAssetKindCount; ++k)
                if (ImGui::Selectable(KindLabel(static_cast<AssetKind>(k)),
                                      state.kindFilter == k))
                    state.kindFilter = k;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "search...", state.search, sizeof(state.search));

        // Entry table. Rebuilt per frame (small registries; cache when they grow).
        const std::vector<AssetEntry> entries = BuildAssetEntries(project->Registry());
        if (ImGui::BeginTable("##assets", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 260.0f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const AssetEntry& e : entries)
            {
                if (!MatchesFilter(e, state.kindFilter, state.search))
                    continue;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string label = std::string(KindIcon(e.kind)) + "  " + e.name +
                                          "##" + e.mountPath;
                if (ImGui::Selectable(label.c_str(), state.selected == e.guid,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick))
                    state.selected = e.guid;

                // Drag source: any asset row; drop targets filter by kind.
                if (ImGui::BeginDragDropSource())
                {
                    AssetDragPayload payload{ e.guid, e.kind };
                    ImGui::SetDragDropPayload(kAssetDragType, &payload, sizeof(payload));
                    ImGui::Text("%s %s", KindIcon(e.kind), e.name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Row context menu: kind-specific action first (Material ->
                // derive an instance, Scene -> set as boot scene, Texture ->
                // mint/reuse a .arcsprite wrapping it), then Assets-menu
                // parity (Show in Explorer / Copy Path) for every kind.
                if (ImGui::BeginPopupContextItem())
                {
                    // Right-click acts on this row: make it the tracked
                    // selection so the highlight + the Assets menu follow.
                    state.selected = e.guid;
                    if (e.kind == AssetKind::Material && ImGui::MenuItem("New Instance..."))
                        actions.createInstanceOf = e.guid;
                    if (e.kind == AssetKind::Scene && ImGui::MenuItem("Set as Boot Scene"))
                        actions.setBootScene = e.guid;
                    if (e.kind == AssetKind::Texture && ImGui::MenuItem("Create Sprite"))
                        actions.createSpriteFrom = e.guid;
                    if (e.kind == AssetKind::Material || e.kind == AssetKind::Scene ||
                        e.kind == AssetKind::Texture)
                        ImGui::Separator();
                    // Assets-menu parity (Show in Explorer / Copy Path).
                    if (ImGui::MenuItem("Show in Explorer"))
                        actions.showInExplorer = e.guid;
                    if (ImGui::MenuItem("Copy Path"))
                        actions.copyPath = e.guid;
                    ImGui::EndPopup();
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    // Resolve the physical file either way; a scene is NOT a
                    // DocumentHost document (it replaces the editing session, it
                    // does not open beside the Viewport), so it hands the path
                    // back to the host instead of going through docs.OpenPath.
                    const auto path =
                        project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
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
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", e.mountPath.c_str(), e.guid.ToString().c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", e.mountPath.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::End();
        return actions;
    }
}
