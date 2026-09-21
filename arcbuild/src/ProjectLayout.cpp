#include "ProjectLayout.hpp"

namespace arcbuild
{
    std::filesystem::path SlotPath(
        const ProjectLayout& project)
    {
        if (project.gameModule.empty())
            return {};

        return project.root / "Binaries" / project.gameModule;
    }

    std::filesystem::path SolutionPath(
        const ProjectLayout& project,
        const std::filesystem::path& discovered)
    {
        if (!discovered.empty())
        {
            if (discovered.is_absolute())
                return discovered;

            return (project.root / discovered).lexically_normal();
        }

        return project.root / (project.name + ".slnx");
    }

    std::vector<std::filesystem::path> CleanTargets(
        const ProjectLayout& project,
        std::string_view config)
    {
        return
        {
            project.root / "Binaries",
            project.root / "Intermediate" / std::string(config)
        };
    }
}