#pragma once

// Named editor layouts (settings arc S4, spec s7.4): plain ImGui ini files
// under Paths EditorUserDir/Layouts, machine-wide. "Layouts are files": they
// are documents, not cvars. The CURRENT layout stays the per-project session
// ini that ImGui loads and auto-saves (the imgui.ini veto rule), now in
// Layouts/Session so the two families never share a name.

#include "Panels/PanelRegistry.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    [[nodiscard]] std::optional<std::string> ValidateLayoutName(std::string_view name);   // the reason it is refused, or nullopt

    class LayoutLibrary
    {
    public:
        explicit LayoutLibrary(std::filesystem::path dir) : m_dir(std::move(dir)) {}
        [[nodiscard]] const std::filesystem::path& Dir() const noexcept { return m_dir; }
        // *.ini stems directly in Dir() that are valid layout names, case-insensitively
        // sorted. A refused stem (a pre-S4 session file, default.ini or <guid>.ini,
        // left in the same folder on Windows) is never listed.
        [[nodiscard]] std::vector<std::string> List() const;
        [[nodiscard]] std::filesystem::path PathOf(std::string_view name) const { return m_dir / (std::string(name) + ".ini"); }
        [[nodiscard]] bool Exists(std::string_view name) const;
        bool Save(std::string_view name, std::string_view iniText, std::string* error = nullptr) const;   // atomic
        [[nodiscard]] std::optional<std::string> Load(std::string_view name) const;
        bool Delete(std::string_view name) const;
    private:
        std::filesystem::path m_dir;
    };

    // Paths EditorUserDir / "Layouts"; EMPTY when there is no editor user dir
    // (LOCALAPPDATA unset, a Dist build) -- never a relative path.
    [[nodiscard]] std::filesystem::path NamedLayoutDir();
    [[nodiscard]] std::filesystem::path SessionLayoutDir();   // NamedLayoutDir() / "Session", or empty

    enum class LayoutSeed : std::uint8_t { None, PreS4File, NamedDefault, LegacyExeIni };
    // Seed a project's missing session layout: the pre-S4 per-project file, else
    // the named default layout, else the legacy exe-dir imgui.ini. Copies.
    LayoutSeed SeedSessionLayout(const std::filesystem::path& target, const std::filesystem::path& preS4File,
                                 const LayoutLibrary& named, std::string_view defaultName,
                                 const std::filesystem::path& legacyExeIni);

    // editor.layout.openPanelsAtStart: "*" = every panel; else a comma list of
    // kPanels names (case-insensitive; unknown ignored). Permanent panels are always visible.
    [[nodiscard]] PanelVisibility ParseOpenPanels(std::string_view list);
    [[nodiscard]] std::string FormatOpenPanels(const PanelVisibility& vis);
}
