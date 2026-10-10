#pragma once

// Preferences > Layout (settings arc S4, spec s7.4). Layouts are files; the
// page header says so (s6.6). Loading and Reset To Factory happen at the
// frame boundary (EditorApp), never inside the page's ImGui frame.

#include "Panels/PanelRegistry.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct LayoutPageState
    {
        std::filesystem::path dir;                // empty = NamedLayoutDir()
        char nameBuf[65] = {};
        std::vector<std::string> names;
        bool listDirty = true;
        std::optional<std::string> loadRequest;   // EditorApp::ApplyPendingLayoutRequest takes it
        bool resetRequested = false;              // folded into Window > Reset Layout
        std::string status;
    };

    bool SaveLayoutAs(LayoutPageState& state, std::string_view name, std::string_view iniText);
    bool SetDefaultLayout(LayoutPageState& state, std::string_view name);   // "" = factory
    bool DeleteLayout(LayoutPageState& state, std::string_view name);
    void RequestLoadLayout(LayoutPageState& state, std::string_view name);
    bool SetOpenPanelsAtStart(const PanelVisibility& vis);
    void DrawLayoutPage(void* user);   // user = LayoutPageState*
}
