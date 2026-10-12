#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace arcbuild
{
    struct ProjectLayout
    {
        std::filesystem::path root;
        std::filesystem::path manifest;
        std::string           name;
        std::string           gameModule;
    };

    [[nodiscard]]
    std::filesystem::path SlotPath(
        const ProjectLayout& project);

    // The MSBuild workspace file BackendResolver drives: a `discovered` file
    // (Toolchain::DiscoverSolution) wins, made absolute against the root when
    // relative; else the <name>.slnx convention. The one implementation of
    // this rule -- Backend.cpp calls it rather than carrying its own copy.
    [[nodiscard]]
    std::filesystem::path SolutionPath(
        const ProjectLayout& project,
        const std::filesystem::path& discovered);

    [[nodiscard]]
    std::vector<std::filesystem::path> CleanTargets(
        const ProjectLayout& project,
        std::string_view config);

    // Where the Ninja backend LINKS the module before arcbuild stages it into
    // the slot: <root>/Intermediate/<config>/Ninja/Binaries/<gameModule>.
    // This mirrors build/arcane.lua's `filter "action:ninja"` targetdir --
    // the two are one contract and the [build-generator] case pins the
    // generated Fixture.ninja's link edges against THIS function. Inside
    // Intermediate/<config>/ on purpose, so CleanTargets above removes it.
    // Empty when the project names no game module.
    // `arcbuild package` (engine modularization s3.3): the stage list
    // build/arcane.lua's arcane_package_exe writes at generate time, and the
    // staged package directory, Packages/<Project>-<platform>-<config>/.
    [[nodiscard]]
    std::filesystem::path PackageManifestPath(
        const ProjectLayout& project);

    [[nodiscard]]
    std::filesystem::path PackageOutputDir(
        const ProjectLayout& project,
        std::string_view platform,
        std::string_view config);

    [[nodiscard]]
    std::filesystem::path NinjaLinkOutput(
        const ProjectLayout& project,
        std::string_view config);
}
