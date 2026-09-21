#include "Backend.hpp"

#include <Arcane/Build/Toolchain.hpp>

namespace arcbuild
{
    namespace
    {
        std::optional<std::filesystem::path> NonEmpty(
            std::filesystem::path path)
        {
            if (path.empty())
                return std::nullopt;

            return path;
        }

        std::filesystem::path ResolveSolutionPath(
            const ProjectLayout& project,
            const std::filesystem::path& discovered)
        {
            if (!discovered.empty())
            {
                return discovered.is_absolute()
                    ? discovered
                    : (project.root / discovered)
                    .lexically_normal();
            }

            return
                project.root /
                (project.name + ".slnx");
        }
    }

    std::optional<std::filesystem::path>
        BackendResolver::ResolvePremake(
            const std::filesystem::path& sdkRoot) const
    {
        return NonEmpty(
            Arcane::Toolchain::ResolvePremake(
                sdkRoot));
    }

    std::optional<std::filesystem::path>
        BackendResolver::ResolveBuilder(
            BuildBackend backend) const
    {
        switch (backend)
        {
        case BuildBackend::MsBuild:
            return NonEmpty(
                Arcane::Toolchain::ResolveMsBuild());

        case BuildBackend::Make:
        case BuildBackend::Ninja:
        case BuildBackend::XcodeBuild:
            // Concrete discovery for these backends lands in the next stage.
            return std::nullopt;

        case BuildBackend::None:
            return std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<BackendContext>
        BackendResolver::ResolveBackendContext(
            BuildBackend backend,
            const ProjectLayout& project) const
    {
        std::error_code ec;

        switch (backend)
        {
        case BuildBackend::MsBuild:
        {
            const auto discovered =
                Arcane::Toolchain::DiscoverSolution(
                    project.root);

            const auto solution =
                ResolveSolutionPath(
                    project,
                    discovered);

            if (!std::filesystem::is_regular_file(
                solution,
                ec) ||
                ec)
            {
                return std::nullopt;
            }

            return BackendContext
            {
                solution,
                std::nullopt
            };
        }

        case BuildBackend::Make:
        {
            const auto makefile =
                project.root / "Makefile";

            if (!std::filesystem::is_regular_file(
                makefile,
                ec) ||
                ec)
            {
                return std::nullopt;
            }

            return BackendContext
            {
                project.root,
                std::nullopt
            };
        }

        case BuildBackend::Ninja:
        {
            const auto ninjaFile =
                project.root / "build.ninja";

            if (!std::filesystem::is_regular_file(
                ninjaFile,
                ec) ||
                ec)
            {
                return std::nullopt;
            }

            return BackendContext
            {
                project.root,
                std::nullopt
            };
        }

        case BuildBackend::XcodeBuild:
        {
            const auto xcodeProject =
                project.root /
                (project.name + ".xcodeproj");

            if (!std::filesystem::is_directory(
                xcodeProject,
                ec) ||
                ec)
            {
                return std::nullopt;
            }

            return BackendContext
            {
                xcodeProject,
                std::nullopt
            };
        }

        case BuildBackend::None:
            return std::nullopt;
        }

        return std::nullopt;
    }
}
