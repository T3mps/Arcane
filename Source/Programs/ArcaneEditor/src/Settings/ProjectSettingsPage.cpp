#include "Settings/ProjectSettingsPage.hpp"

#include "Panels/AssetPanelModel.hpp"        // AssetKind
#include "Panels/AssetReferenceField.hpp"    // AssetRefRow
#include "Widgets/IconsLucide.h"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Plugin/PluginABI.hpp>       // kGamePluginABIVersion
#include <Arcane/Project/Project.hpp>

#include <imgui.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    void DrawProjectIdentityPage(const Arcane::Project* project, PropertyGrid& grid,
                                 const AssetRefServices* assetRefs, ProjectSettingsRequests& requests)
    {
        if (!project)
        {
            ImGui::TextDisabled("Open a project to edit its settings.");
            return;
        }
        ImGui::TextDisabled(ICON_LC_INFO "  This page edits the project file (.arcproj), not cvar settings.");
        const Arcane::ProjectManifest& m = project->Manifest();

        if (grid.Section("Identity"))
        {
            PropertyGrid::Rows rows(grid, "##project-identity");
            if (rows)
            {
                grid.ReadOnlyRow("Name", m.name);
                grid.ReadOnlyRow("Description", m.description.empty() ? std::string_view("(none)") : std::string_view(m.description));
                grid.ReadOnlyRow("GUID", m.guid);
                const int engineAbi = static_cast<int>(Arcane::kGamePluginABIVersion);
                const std::string abi = m.engineAbi == engineAbi
                    ? std::to_string(m.engineAbi)
                    : std::to_string(m.engineAbi) + "  (this editor: " + std::to_string(engineAbi) + ")";
                grid.ReadOnlyRow("Engine ABI", abi);
                grid.ReadOnlyRow("Game module", m.gameModule.empty() ? std::string_view("(content-only)") : std::string_view(m.gameModule));
                grid.ReadOnlyRow("Source folder", m.sourceDir);
                std::string plugins;
                for (const auto& p : m.plugins)
                {
                    if (!plugins.empty()) plugins += ", ";
                    plugins += p.name;
                    if (!p.enabled) plugins += " (disabled)";
                }
                grid.ReadOnlyRow("Plugins", plugins.empty() ? std::string_view("(none)") : std::string_view(plugins));
                grid.ReadOnlyRow("Folder", project->Root().generic_string());
                grid.ReadOnlyRow("Content", "game:// -> " + (project->Root() / "Content").generic_string());
            }
        }

        if (grid.Section("Startup"))
        {
            PropertyGrid::Rows rows(grid, "##project-startup");
            if (rows)
            {
                const Guid boot = Guid::FromString(m.bootScene).value_or(Guid{});
                if (assetRefs)
                {
                    AssetRefArgs args;
                    args.guid = boot;
                    args.kindFilter = static_cast<int>(AssetKind::Scene);
                    const AssetRefEdit e = AssetRefRow(grid, "Boot scene", args, *assetRefs);
                    if (e.op == AssetRefEdit::Op::Set) requests.bootScene = e.guid;
                    else if (e.op == AssetRefEdit::Op::Clear) requests.bootScene = Guid::Nil();
                }
                else
                    grid.ReadOnlyRow("Boot scene", boot.IsValid() ? boot.ToString() : std::string("(none)"));
            }
        }

        if (grid.Section("Gameplay Input"))
        {
            // Moved from the old read-out (EditorPanels.cpp:2986-3034 on main).
            const Guid selected = Guid::FromString(m.inputActions).value_or(Guid{});
            std::vector<std::pair<Guid, std::string>> choices;
            for (const auto& [id, mountPath] : project->Registry().All())
                if (mountPath.size() >= 9 && mountPath.substr(mountPath.size() - 9) == ".arcinput")
                    choices.emplace_back(id, mountPath);
            const bool missing = selected.IsValid() &&
                std::none_of(choices.begin(), choices.end(), [&](const auto& c) { return c.first == selected; });
            {
                PropertyGrid::Rows rows(grid, "##project-input");
                if (rows)
                {
                    std::vector<std::string> labels{ "None" };
                    for (const auto& [id, path] : choices) labels.push_back(path);
                    int current = 0;
                    for (std::size_t i = 0; i < choices.size(); ++i)
                        if (choices[i].first == selected) current = static_cast<int>(i) + 1;
                    if (missing)
                    {
                        labels.push_back("Missing asset");
                        current = static_cast<int>(labels.size()) - 1;
                    }
                    std::vector<const char*> items;
                    for (const std::string& s : labels) items.push_back(s.c_str());
                    const int picked = grid.ComboRow("Input asset", items.data(), static_cast<int>(items.size()), current);
                    if (picked == 0) requests.clear = true;
                    else if (picked > 0 && picked <= static_cast<int>(choices.size()))
                    {
                        requests.selection = choices[static_cast<std::size_t>(picked) - 1].first;
                        requests.select = true;
                    }
                    const char* buttons[] = { "Open Asset", "Clear Selection", "Create Input Actions Asset" };
                    const unsigned mask = (selected.IsValid() && !missing ? 0b011u : 0b000u) | 0b100u;
                    const int b = grid.ButtonRow("Actions", buttons, 3, mask);
                    if (b == 0) requests.open = true;
                    else if (b == 1) requests.clear = true;
                    else if (b == 2) requests.create = true;
                }
            }
            if (missing)
                ImGui::TextWrapped("Selected asset %s is missing from the project registry. Choose another asset or clear the selection.",
                                   selected.ToString().c_str());
            if (choices.empty())
                ImGui::TextDisabled("No .arcinput assets are registered in this project.");
        }
    }
}
