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

    // The defaults the EngineConfig rung fills into Paths (settings S2-H, S7;
    // spec s8.2, s11.0), whoever configured it first. The engine dir becomes
    // `exeDir` only when no host named one -- a host's engine dir is kept. A
    // Dist build always resolves the player's user data under the OS per-user
    // dir, so `distBuild` sets `dist` even under a host's engine dir (HostBoot's
    // early User rung reads Paths::ForProject(Current()) right after). A dev
    // build never clears a `dist` a host or test set. True when `config` changed.
    [[nodiscard]] inline bool ApplyEngineDirDefaults(Paths::Config& config,
                                                     const std::filesystem::path& exeDir, bool distBuild)
    {
        bool changed = false;
        if (config.engineDir.empty())
        {
            config.engineDir = exeDir;
            changed = true;
        }
        if (distBuild && !config.dist)
        {
            config.dist = true;
            changed = true;
        }
        return changed;
    }
}
