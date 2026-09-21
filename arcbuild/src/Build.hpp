#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace arcbuild
{
    enum class BuildOperation : std::uint8_t
    {
        Build,
        Rebuild,
        Clean
    };

    struct BackendContext
    {
        std::filesystem::path       path;
        std::optional<std::string> scheme;
    };
}