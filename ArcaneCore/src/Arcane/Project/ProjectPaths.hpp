#pragma once

#include <Arcane/Core/Api.hpp>
#include <Arcane/Platform/Paths.hpp>

#include <filesystem>

namespace Arcane
{
    class Project;

    inline constexpr bool kDistBuild =
#if defined(ARC_BUILD_DIST)
        true;
#else
        false;
#endif

    [[nodiscard]] ARC_CORE_API Paths::Config PathsConfigFor(const Project& project,
                                                              std::filesystem::path engineDir, bool dist);
    [[nodiscard]] ARC_CORE_API Paths::Config PathsConfigWithoutProject(std::filesystem::path engineDir, bool dist);
}
