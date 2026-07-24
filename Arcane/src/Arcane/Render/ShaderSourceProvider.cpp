#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <Arcane/Base/Log.hpp>

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Mirrors the Assets/ShaderLibrary anchor: relative paths resolve against
        // the executable directory so lookups work regardless of CWD.
        std::filesystem::path ExeRelative(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#endif
            return path;
        }
    }

    void ShaderSourceProvider::AddRoot(const std::filesystem::path& root)
    {
        m_roots.push_back(root);
    }

    std::optional<std::string> ShaderSourceProvider::Get(std::string_view logicalName) const
    {
        const std::filesystem::path rel(logicalName);
        if (rel.empty() || rel.is_absolute())
            return std::nullopt;
        for (const auto& part : rel)
        {
            if (part == "..")
            {
                ARC_WARN("ShaderSourceProvider: rejected escaping name '{}'",
                         std::string(logicalName));
                return std::nullopt;
            }
        }

        for (const std::filesystem::path& root : m_roots)
        {
            std::ifstream file(ExeRelative(root) / rel, std::ios::binary);
            if (!file)
                continue;
            std::string text{ std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>() };
            return text;
        }
        return std::nullopt;
    }
}
