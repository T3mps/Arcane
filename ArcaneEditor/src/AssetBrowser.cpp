#include "AssetBrowser.hpp"

#include "DocumentHost.hpp"
#include "IconsLucide.h"

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
                case AssetKind::Other:    return "Other";
            }
            return "Other";
        }
    }

    AssetBrowserActions DrawAssetBrowserPanel(AssetBrowserState& state,
                                              const Arcane::Project* project,
                                              DocumentHost& docs)
    {
        AssetBrowserActions actions;
        ImGui::Begin("Assets");
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
                ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick);

                // Drag source: any asset row; drop targets filter by kind.
                if (ImGui::BeginDragDropSource())
                {
                    AssetDragPayload payload{ e.guid, e.kind };
                    ImGui::SetDragDropPayload(kAssetDragType, &payload, sizeof(payload));
                    ImGui::Text("%s %s", KindIcon(e.kind), e.name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Material rows: context menu -> derive an instance (Slice 7).
                if (e.kind == AssetKind::Material && ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("New Instance..."))
                        actions.createInstanceOf = e.guid;
                    ImGui::EndPopup();
                }
                // Scene rows: context menu -> make this the project's boot scene.
                if (e.kind == AssetKind::Scene && ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Set as Boot Scene"))
                        actions.setBootScene = e.guid;
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
