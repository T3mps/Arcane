#include "Backend.hpp"

#include <Arcane/Build/Toolchain.hpp>

#include <string_view>

namespace arcbuild
{
    namespace
    {
        std::expected<std::filesystem::path, std::string> RequireTool(
            std::filesystem::path path,
            std::string_view description)
        {
            if (path.empty())
            {
                return std::unexpected(
                    "could not locate " +
                    std::string(description));
            }

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

    std::expected<std::filesystem::path, std::string>
        BackendResolver::ResolvePremake(
            const std::filesystem::path& sdkRoot) const
    {
        return RequireTool(
            Arcane::Toolchain::ResolvePremake(
                sdkRoot),
            "Premake (checked '" +
                (sdkRoot / "ThirdParty" / "premake5").generic_string() +
                "' and PATH)");
    }

    std::expected<std::filesystem::path, std::string>
        BackendResolver::ResolveBuilder(
            BuildBackend backend) const
    {
        switch (backend)
        {
        case BuildBackend::MsBuild:
            return RequireTool(
                Arcane::Toolchain::ResolveMsBuild(),
                "MSBuild (checked vswhere and PATH)");

        case BuildBackend::Make:
            return RequireTool(
                Arcane::Toolchain::ResolveMake(),
                "Make (checked PATH)");

        case BuildBackend::Ninja:
            return RequireTool(
                Arcane::Toolchain::ResolveNinja(),
                "Ninja (checked PATH)");

        case BuildBackend::XcodeBuild:
            return RequireTool(
                Arcane::Toolchain::ResolveXcodeBuild(),
                "xcodebuild (macOS only)");

        case BuildBackend::None:
            return std::unexpected(
                "no build backend is available for this action");
        }

        return std::unexpected(
            "unknown build backend");
    }

    std::expected<BackendContext, std::string>
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
                return std::unexpected(
                    "no generated solution file at '" +
                    solution.generic_string() +
                    "' -- run generate first");
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
                return std::unexpected(
                    "no generated Makefile at '" +
                    makefile.generic_string() +
                    "' -- run generate first");
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
                return std::unexpected(
                    "no generated build.ninja at '" +
                    ninjaFile.generic_string() +
                    "' -- run generate first");
            }

            // beta8 also emits <module-stem>.ninja beside the root
            // build.ninja (the module's own compile/link rules); Compose
            // needs the stem to derive the real workspace target
            // (<module-stem>_<Config>, see ComposeNinja), so both files
            // must exist before this context is usable. The stem rides in
            // BackendContext::scheme for Compose to read back.
            const auto moduleNinja =
                project.root / (project.name + ".ninja");

            if (!std::filesystem::is_regular_file(
                moduleNinja,
                ec) ||
                ec)
            {
                return std::unexpected(
                    "no generated '" +
                    moduleNinja.generic_string() +
                    "' -- run generate first");
            }

            return BackendContext
            {
                project.root,
                project.name
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
                return std::unexpected(
                    "no generated Xcode project at '" +
                    xcodeProject.generic_string() +
                    "' -- run generate first");
            }

            // beta8's xcode4 generator creates no shared scheme (Global
            // Constraints, multibackend hardening plan) -- the module stem
            // rides in BackendContext::scheme so Compose can drive
            // `-target <stem>` instead of a scheme that does not exist.
            return BackendContext
            {
                xcodeProject,
                project.name
            };
        }

        case BuildBackend::None:
            return std::unexpected(
                "no build backend is available for this action");
        }

        return std::unexpected(
            "unknown build backend");
    }
}
