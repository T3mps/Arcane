#include "Settings/SettingsModel.hpp"

#include <Arcane/Project/ProjectManifest.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>

namespace Arcane::Editor
{
    namespace
    {
        // Roots in window order; a path starting with none of them goes under "Engine".
        constexpr std::string_view kRootOrder[] = { "Project", "Appearance", "Keyboard", "Layout",
                                                    "Editor", "Engine", "Game", "Plugins" };

        bool IEquals(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        }

        std::vector<std::string_view> Split(std::string_view s, char sep)
        {
            std::vector<std::string_view> out;
            std::size_t start = 0;
            while (start <= s.size())
            {
                const std::size_t end = s.find(sep, start);
                const std::string_view part = s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
                if (!part.empty()) out.push_back(part);
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
            return out;
        }

        std::size_t RootRank(std::string_view label)
        {
            for (std::size_t i = 0; i < std::size(kRootOrder); ++i)
                if (label == kRootOrder[i]) return i;
            return std::size(kRootOrder);
        }

        SettingsTreeNode& Ensure(SettingsTreeNode& root, std::string_view path)
        {
            SettingsTreeNode* node = &root;
            std::string prefix;
            for (std::string_view seg : Split(path, '/'))
            {
                prefix = prefix.empty() ? std::string(seg) : prefix + "/" + std::string(seg);
                auto it = std::find_if(node->children.begin(), node->children.end(),
                                       [&](const SettingsTreeNode& c) { return c.label == seg; });
                if (it == node->children.end())
                {
                    node->children.push_back(SettingsTreeNode{ std::string(seg), prefix, {}, {} });
                    it = std::prev(node->children.end());
                }
                node = &*it;
            }
            return *node;
        }

        void SortTree(SettingsTreeNode& node, bool top)
        {
            std::sort(node.children.begin(), node.children.end(), [top](const SettingsTreeNode& a, const SettingsTreeNode& b)
            {
                if (top)
                {
                    const std::size_t ra = RootRank(a.label), rb = RootRank(b.label);
                    if (ra != rb) return ra < rb;
                }
                return a.label < b.label;
            });
            for (SettingsTreeNode& c : node.children) SortTree(c, false);
        }

        const SettingsTreeNode* FindNode(const SettingsTreeNode& node, std::string_view path)
        {
            if (node.path == path) return &node;
            for (const SettingsTreeNode& c : node.children)
                if (path.starts_with(c.path))
                    if (const SettingsTreeNode* hit = FindNode(c, path)) return hit;
            return nullptr;
        }
    }

    SettingsModuleRoles RolesForManifest(const Arcane::ProjectManifest& manifest)
    {
        SettingsModuleRoles roles;
        if (!manifest.gameModule.empty())
            roles.gameModule = std::filesystem::path(manifest.gameModule).stem().string();
        for (const auto& plugin : manifest.plugins)
            if (plugin.enabled) roles.plugins.push_back(plugin.name);
        return roles;
    }

    std::string DisplayWord(std::string_view segment)
    {
        std::string out;
        bool startWord = true;
        for (std::size_t i = 0; i < segment.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(segment[i]);
            if (c == '_' || c == '-' || c == ' ')
            {
                if (!out.empty() && out.back() != ' ') out.push_back(' ');
                startWord = true;
                continue;
            }
            if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(segment[i - 1])) &&
                !out.empty() && out.back() != ' ')
            {
                out.push_back(' ');
                startWord = true;
            }
            out.push_back(startWord ? static_cast<char>(std::toupper(c)) : static_cast<char>(c));
            startWord = false;
        }
        return out;
    }

    std::string SettingDisplayName(const CVarDescInfo& desc)
    {
        if (!desc.displayName.empty()) return desc.displayName;
        const std::size_t dot = desc.name.rfind('.');
        return DisplayWord(dot == std::string::npos ? std::string_view(desc.name) : std::string_view(desc.name).substr(dot + 1));
    }

    std::string SettingCategoryPath(const CVarDescInfo& desc, const SettingsModuleRoles& roles)
    {
        std::string base = desc.categoryPath;
        if (base.empty())
        {
            const std::vector<std::string_view> segs = Split(desc.name, '.');
            for (std::size_t i = 0; i + 1 < segs.size(); ++i)
            {
                if (!base.empty()) base += '/';
                base += DisplayWord(segs[i]);
            }
            if (base.empty()) base = "General";
        }
        if (!roles.gameModule.empty() && IEquals(desc.module, roles.gameModule))
            return "Game/" + roles.gameModule + "/" + base;
        for (const std::string& plugin : roles.plugins)
            if (IEquals(desc.module, plugin))
                return "Plugins/" + plugin + "/" + base;
        const std::string_view first = Split(base, '/').front();
        if (RootRank(first) < std::size(kRootOrder)) return base;
        return "Engine/" + base;
    }

    bool InWindow(SettingScope settingScope, SettingScope window) noexcept
    {
        return (settingScope == SettingScope::Project) == (window == SettingScope::Project);
    }

    void SettingsModel::Rebuild(const CVarRegistry& registry, SettingScope window)
    {
        m_window = window;
        m_revision = registry.Revision();
        m_descs.clear();
        m_root = SettingsTreeNode{};
        for (const std::string& name : registry.Names(/*includeHidden=*/true))
            if (std::optional<CVarDescInfo> d = registry.Describe(name); d && InWindow(d->scope, window))
                m_descs.push_back(std::move(*d));
        std::sort(m_descs.begin(), m_descs.end(), [](const CVarDescInfo& a, const CVarDescInfo& b) { return a.name < b.name; });
        std::vector<const CVarDescInfo*> rows;
        rows.reserve(m_descs.size());
        for (const CVarDescInfo& d : m_descs) rows.push_back(&d);
        std::stable_sort(rows.begin(), rows.end(), [](const CVarDescInfo* a, const CVarDescInfo* b)
        {
            if (a->order != b->order) return a->order < b->order;
            return SettingDisplayName(*a) < SettingDisplayName(*b);
        });
        for (const CVarDescInfo* d : rows)
            Ensure(m_root, SettingCategoryPath(*d, m_roles)).cvars.push_back(d->name);
        for (const SettingsPageRef& page : m_pages)
            if (InWindow(page.scope, window)) (void)Ensure(m_root, page.categoryPath);
        SortTree(m_root, true);
    }

    const SettingsTreeNode* SettingsModel::Find(std::string_view path) const
    {
        if (path.empty()) return nullptr;
        return FindNode(m_root, path);
    }

    const CVarDescInfo* SettingsModel::Desc(std::string_view name) const
    {
        const auto it = std::lower_bound(m_descs.begin(), m_descs.end(), name,
                                         [](const CVarDescInfo& d, std::string_view n) { return d.name < n; });
        return it != m_descs.end() && it->name == name ? &*it : nullptr;
    }
}
