#pragma once

// The settings windows' PURE model (settings arc S3, spec s6.2): the
// category tree a window shows -- built from each cvar's categoryPath, else
// its dotted name, plus the registered custom pages -- and (S3-5) search and
// filters. No ImGui; ArcaneTests drives it directly.
//
// Paths are "/"-joined display segments ("Engine/Physics/Solver"). Roots:
// a path that does not start with a known root is filed under "Engine";
// a game module's cvars hang under "Game/<module>", a plugin's under
// "Plugins/<name>".

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/CVarTypes.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { struct ProjectManifest; }

namespace Arcane::Editor
{
    struct SettingsTreeNode
    {
        std::string label;                        // one path segment, as shown
        std::string path;                         // "Engine/Physics/Solver"; "" for the root
        std::vector<SettingsTreeNode> children;   // known roots first, then alphabetical
        std::vector<std::string> cvars;           // this node's own rows, by (order, display name)
    };

    // A custom page as the model sees it (the draw half lives in SettingsHost).
    struct SettingsPageRef
    {
        SettingScope scope = SettingScope::Project;
        std::string  categoryPath;   // the node it draws on (created if absent)
        std::string  title;
    };

    // The project's game module and plugins, matched case-insensitively
    // against CVarDescInfo::module (S1: the declaring DLL's module name).
    struct SettingsModuleRoles
    {
        std::string gameModule;                // the manifest gameModule's stem; "" = none
        std::vector<std::string> plugins;      // enabled plugin names
    };

    [[nodiscard]] SettingsModuleRoles RolesForManifest(const Arcane::ProjectManifest& manifest);
    // "maxSteps" -> "Max Steps", "undo" -> "Undo", "hdr_enabled" -> "Hdr Enabled".
    [[nodiscard]] std::string DisplayWord(std::string_view segment);
    // The row label: displayName, else DisplayWord(the last name segment).
    [[nodiscard]] std::string SettingDisplayName(const CVarDescInfo& desc);
    [[nodiscard]] std::string SettingCategoryPath(const CVarDescInfo& desc, const SettingsModuleRoles& roles);
    // Preferences window = PreferencesMachine + PreferencesProject; Project window = Project.
    [[nodiscard]] bool InWindow(SettingScope settingScope, SettingScope window) noexcept;

    class SettingsModel
    {
    public:
        enum class Filter : std::uint8_t { All, Modified, Overridden, ProjectOverrides };

        void SetModuleRoles(SettingsModuleRoles roles) { m_roles = std::move(roles); }
        void SetPages(std::vector<SettingsPageRef> pages) { m_pages = std::move(pages); }

        void Rebuild(const CVarRegistry&, SettingScope window);   // Preferences window = PreferencesMachine + PreferencesProject
        const SettingsTreeNode& Tree() const { return m_root; }

        // Names in this window whose name, display name, help or keywords
        // hold EVERY whitespace-separated token of `query`, case-insensitive.
        std::vector<std::string> Search(std::string_view query) const;
        // The rows a window draws: Search(query), then `filter` (Modified:
        // value != default; Overridden: a rung above the row's target wins;
        // ProjectOverrides: the User rung holds a value, Preferences only),
        // then the advanced gate (Dev and Hidden rows only when showAdvanced).
        [[nodiscard]] std::vector<std::string> Visible(const CVarRegistry& registry, std::string_view query,
                                                       Filter filter, bool showAdvanced) const;
        // The tree cut down to nodes that hold a `keep` row, their ancestors,
        // and the page nodes (a page always stays).
        [[nodiscard]] SettingsTreeNode Pruned(const std::vector<std::string>& keep) const;

        [[nodiscard]] const SettingsTreeNode* Find(std::string_view path) const;
        [[nodiscard]] const CVarDescInfo* Desc(std::string_view name) const;
        [[nodiscard]] SettingScope Window() const noexcept { return m_window; }
        [[nodiscard]] std::uint64_t BuiltRevision() const noexcept { return m_revision; }

    private:
        SettingsModuleRoles m_roles;
        std::vector<SettingsPageRef> m_pages;
        SettingScope m_window = SettingScope::Project;
        std::uint64_t m_revision = ~0ull;   // never built: any registry revision differs
        SettingsTreeNode m_root;
        std::vector<CVarDescInfo> m_descs;  // this window's cvars, sorted by name
    };
}
