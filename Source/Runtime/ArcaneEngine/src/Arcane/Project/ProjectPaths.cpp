#include <Arcane/Project/ProjectPaths.hpp>

#include <Arcane/Project/Project.hpp>

#include <utility>

namespace Arcane
{
    Paths::Config PathsConfigFor(const Project& project, std::filesystem::path engineDir, bool dist)
    {
        Paths::Config c;
        c.engineDir = std::move(engineDir);
        c.projectDir = project.Root();
        c.companyName = project.Manifest().company;
        c.gameName = project.Manifest().name;
        c.dist = dist;
        return c;
    }

    Paths::Config PathsConfigWithoutProject(std::filesystem::path engineDir, bool dist)
    {
        Paths::Config c;
        c.engineDir = std::move(engineDir);
        c.dist = dist;
        return c;
    }
}
