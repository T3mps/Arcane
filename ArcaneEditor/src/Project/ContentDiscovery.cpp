#include "Project/ContentDiscovery.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace Arcane::Editor
{
    namespace
    {
        // Lowercased extension -- same helper shape as AssetRegistry.cpp's
        // and CookSession.cpp's own LowerExt (each private to its TU, hence
        // three copies rather than one shared one).
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        // Same shape as CookSession.cpp's MatchesAnyExtension (private to
        // that TU, deliberately mirrored rather than shared -- this file's
        // own header comment explains why).
        bool MatchesAnyExtension(const std::filesystem::path& p,
                                  std::span<const std::string_view> extensions)
        {
            const std::string ext = LowerExt(p);
            for (const std::string_view& candidate : extensions)
                if (ext == candidate)
                    return true;
            return false;
        }
    }

    std::vector<std::filesystem::path> EnumerateContentSourceFiles(
        const std::filesystem::path& contentDir,
        std::span<const std::string_view> extensions)
    {
        std::vector<std::filesystem::path> files;

        std::error_code ec;
        if (!std::filesystem::exists(contentDir, ec) || ec)
            return files;

        std::filesystem::recursive_directory_iterator it(contentDir, ec);
        if (ec)
            return files;
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec))
        {
            if (ec)
                break;

            const std::filesystem::directory_entry& entry = *it;
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc)
                continue;
            if (!MatchesAnyExtension(entry.path(), extensions))
                continue;

            files.push_back(entry.path());
        }

        std::sort(files.begin(), files.end());
        return files;
    }

    std::vector<std::filesystem::path> UnknownPaths(
        const std::vector<std::filesystem::path>& candidates,
        const std::unordered_set<std::string>& knownPaths)
    {
        std::vector<std::filesystem::path> unknown;
        for (const std::filesystem::path& candidate : candidates)
        {
            if (knownPaths.find(candidate.generic_string()) == knownPaths.end())
                unknown.push_back(candidate);
        }
        return unknown;
    }

    std::vector<std::filesystem::path> DiscoverUnknownSources(
        const std::filesystem::path& contentDir,
        std::span<const std::string_view> extensions,
        const std::unordered_set<std::string>& knownPaths)
    {
        return UnknownPaths(EnumerateContentSourceFiles(contentDir, extensions), knownPaths);
    }
}
