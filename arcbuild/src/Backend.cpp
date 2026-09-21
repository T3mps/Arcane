#include "Backend.hpp"

#include <Arcane/Build/Toolchain.hpp>

#include <string_view>

namespace arcbuild
{
    namespace
    {
        // `describe` is a callable, not a string: the descriptive text (which
        // for Premake concatenates the checked path) is only built on the
        // failure path, never on the success path that discards it.
        template <typename Describe>
        std::expected<std::filesystem::path, std::string> RequireTool(
            std::filesystem::path path,
            Describe&& describe)
        {
            if (path.empty())
            {
                return std::unexpected(
                    "could not locate " +
                    std::string(describe()));
            }

            return path;
        }
    }

    std::expected<std::filesystem::path, std::string>
        BackendResolver::ResolvePremake(
            const std::filesystem::path& sdkRoot) const
    {
        return RequireTool(
            Arcane::Toolchain::ResolvePremake(
                sdkRoot),
            [&]
            {
                return "Premake (checked '" +
                    (sdkRoot / "ThirdParty" / "premake5").generic_string() +
                    "' and PATH)";
            });
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
                [] { return "MSBuild (checked vswhere and PATH)"; });

        case BuildBackend::Make:
            return RequireTool(
                Arcane::Toolchain::ResolveMake(),
                [] { return "Make (checked PATH)"; });

        case BuildBackend::Ninja:
            return RequireTool(
                Arcane::Toolchain::ResolveNinja(),
                [] { return "Ninja (checked PATH)"; });

        case BuildBackend::XcodeBuild:
            return RequireTool(
                Arcane::Toolchain::ResolveXcodeBuild(),
                [] { return "xcodebuild (macOS only)"; });

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
                SolutionPath(
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
            // BackendContext::target for Compose to read back.
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
            // rides in BackendContext::target so Compose can drive
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
