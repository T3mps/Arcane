#pragma once

// MountTable: maps a mount scheme ("game", "engine", "plugin/<name>", "diag") to a
// physical root directory, and resolves a logical mount path ("game://a/b.png") to a
// filesystem path. Decouples logical asset addresses from where the project sits on disk.

#include <Arcane/Base/Api.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Arcane
{
    class ARCANE_API MountTable
    {
    public:
        // Register (or replace) a mount root. `scheme` is the text before "://".
        void Mount(std::string scheme, std::filesystem::path root);

        bool HasMount(std::string_view scheme) const;

        // "scheme://relative" -> root / relative. nullopt if the string is not a
        // well-formed mount path (needs a non-empty scheme, "://", and a non-empty
        // relative part) or the scheme is not mounted.
        std::optional<std::filesystem::path> Resolve(std::string_view mountPath) const;

    private:
        std::unordered_map<std::string, std::filesystem::path> m_roots;
    };
}
