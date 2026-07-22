#include <Arcane/Project/MountTable.hpp>

namespace Arcane
{
    void MountTable::Mount(std::string scheme, std::filesystem::path root)
    {
        m_roots[std::move(scheme)] = std::move(root);
    }

    bool MountTable::HasMount(std::string_view scheme) const
    {
        return m_roots.find(std::string(scheme)) != m_roots.end();
    }

    std::optional<std::filesystem::path> MountTable::Resolve(std::string_view mountPath) const
    {
        const std::size_t sep = mountPath.find("://");
        if (sep == std::string_view::npos)
            return std::nullopt;

        const std::string_view scheme = mountPath.substr(0, sep);
        const std::string_view rel    = mountPath.substr(sep + 3);
        if (scheme.empty() || rel.empty())
            return std::nullopt;

        const auto it = m_roots.find(std::string(scheme));
        if (it == m_roots.end())
            return std::nullopt;

        return it->second / std::filesystem::path(rel);
    }
}
