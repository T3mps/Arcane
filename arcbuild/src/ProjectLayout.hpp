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

    [[nodiscard]]
    std::filesystem::path SolutionPath(
        const ProjectLayout& project,
        const std::filesystem::path& discovered);

    [[nodiscard]]
    std::vector<std::filesystem::path> CleanTargets(
        const ProjectLayout& project,
        std::string_view config);
}