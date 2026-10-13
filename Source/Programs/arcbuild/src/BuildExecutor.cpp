#include "BuildExecutor.hpp"

#include "Exit.hpp"
#include "ProjectLayout.hpp"
#include "Stage.hpp"

#include <Arcane/Base/Engine.hpp>         // ExecutablePathUtf8: arccook sits beside arcbuild
#include <Arcane/Build/Toolchain.hpp>
#include <Arcane/Platform/Platform.hpp>   // ExecutableFileName

#include <string>

namespace arcbuild
{
    namespace
    {
        // Ninja is the one backend whose link output is NOT the slot (see
        // Stage.hpp for why the copy lives here and not in a post-build
        // step). Runs only after the backend's own Build/Rebuild plan
        // succeeded -- never after a Clean, which produced nothing to stage;
        // a staging failure is the driver's own refusal (kExitRefused),
        // never dressed up as a child exit code.
        int StageIfNinja(
            const DriverContext& context,
            BuildOperation operation,
            IOutput& output)
        {
            if (context.backend != BuildBackend::Ninja ||
                operation == BuildOperation::Clean)
            {
                return kExitOk;
            }

            const auto staged =
                StageBuiltModule(
                    NinjaLinkOutput(
                        context.project,
                        context.request.config),
                    SlotPath(
                        context.project));

            if (!staged)
            {
                output.Error(staged.error());
                return kExitRefused;
            }

            for (const std::filesystem::path& copied : staged->copied)
                output.Info("staged " + copied.generic_string());

            return kExitOk;
        }
    }

    int IBuildExecutor::Package(
        const DriverContext&) const
    {
        return kExitRefused;
    }

    namespace
    {
        // The build context for a monolithic workspace directory: its one
        // solution (MSBuild) or the directory itself (Make, `make -C`).
        std::expected<BackendContext, std::string> MonolithicContext(
            BuildBackend backend,
            const std::filesystem::path& dir)
        {
            std::error_code ec;
            if (backend == BuildBackend::MsBuild)
            {
                const std::filesystem::path solution = Arcane::Toolchain::DiscoverSolution(dir);
                if (solution.empty())
                    return std::unexpected("package: no monolithic workspace (.slnx) in " +
                                           dir.generic_string() + " after generate");
                return BackendContext{ solution, std::nullopt };
            }
            if (!std::filesystem::is_regular_file(dir / "Makefile", ec))
                return std::unexpected("package: no monolithic Makefile in " + dir.generic_string() +
                                       " after generate");
            return BackendContext{ dir, std::nullopt };
        }

        const char* PackagePlatformName()
        {
            switch (CurrentHostPlatform())
            {
            case HostPlatform::Windows: return "windows";
            case HostPlatform::Linux:   return "linux";
            case HostPlatform::MacOS:   return "macosx";
            }
            return "unknown";
        }
    }

    int BuildExecutor::Package(
        const DriverContext& context) const
    {
        // The package directory is named after the manifest: refuse a name
        // that is not one plain path component (it would leave Packages/).
        if (const std::string& n = context.project.name;
            n.empty() || n == "." || n == ".." || n.find_first_of("/\\:") != std::string::npos)
        {
            output_.Error("package: the project name '" + n + "' is not a plain file name");
            return kExitRefused;
        }

        if (context.backend != BuildBackend::MsBuild && context.backend != BuildBackend::Make)
        {
            output_.Error(
                std::string("package: the '") + BuildBackendName(context.backend) +
                "' backend is not wired for packages (use an MSBuild or Make action)");
            return kExitRefused;
        }

        const auto premake = backends_.ResolvePremake(*context.sdkRoot);
        if (!premake)
        {
            output_.Error(premake.error());
            return kExitRefused;
        }
        const auto builder = backends_.ResolveBuilder(context.backend);
        if (!builder)
        {
            output_.Error(builder.error());
            return kExitRefused;
        }
        const BuildOperation operation =
            context.request.forceRebuild ? BuildOperation::Rebuild : BuildOperation::Build;

        // 1. The engine's monolithic layout (static modules + ArcaneRuntimeHost).
        {
            ProcessPlan plan;
            plan.steps.push_back(ComposeMonolithicGenerate(*context.sdkRoot, *premake, context.request.action));
            if (const int exit = ExecutePlan(plan, processes_, output_, "[premake]"); exit != kExitOk)
                return exit;

            const auto workspace =
                MonolithicContext(context.backend, EngineMonolithicWorkspaceDir(*context.sdkRoot));
            if (!workspace)
            {
                output_.Error(workspace.error());
                return kExitRefused;
            }
            output_.Info("package: engine (monolithic) " + workspace->path.generic_string());
            const ProcessPlan build = ComposeBuild(context.backend, *builder, *workspace,
                                                   context.request.config, operation);
            if (const int exit = ExecutePlan(build, processes_, output_, BuildBackendPrefix(context.backend));
                exit != kExitOk)
                return exit;
        }

        // 2. The project's package workspace.
        {
            ProcessPlan plan;
            plan.steps.push_back(ComposeMonolithicGenerate(context.project.root, *premake, context.request.action));
            if (const int exit = ExecutePlan(plan, processes_, output_, "[premake]"); exit != kExitOk)
                return exit;

            const auto workspace = MonolithicContext(context.backend, PackageWorkspaceDir(context.project));
            if (!workspace)
            {
                output_.Error(workspace.error());
                return kExitRefused;
            }
            output_.Info("package: project (monolithic) " + workspace->path.generic_string());
            const ProcessPlan build = ComposeBuild(context.backend, *builder, *workspace,
                                                   context.request.config, operation);
            if (const int exit = ExecutePlan(build, processes_, output_, BuildBackendPrefix(context.backend));
                exit != kExitOk)
                return exit;
        }

        // 3. Cook the project's content, so the package stages current
        //    artifacts (a project never cooked would boot into an
        //    ArtifactMissing refusal). arccook is built beside arcbuild
        //    (bin/<cfg>/arccook/); without it the existing artifacts stage.
        {
            const std::string self = Arcane::ExecutablePathUtf8();
            const std::filesystem::path cook =
                std::filesystem::path(std::u8string(self.begin(), self.end())).parent_path().parent_path() /
                "arccook" / Arcane::Platform::ExecutableFileName("arccook");
            std::error_code ec;
            if (std::filesystem::is_regular_file(cook, ec))
            {
                ProcessSpec spec;
                spec.executable       = cook;
                spec.arguments        = { "--project", context.project.root.generic_string() };
                spec.workingDirectory = context.project.root;
                ProcessPlan plan;
                plan.steps.push_back(spec);
                if (const int exit = ExecutePlan(plan, processes_, output_, "[arccook]"); exit != kExitOk)
                    return exit;
            }
            else
            {
                output_.Info("package: no arccook beside arcbuild (" + cook.generic_string() +
                             ") -- staging the project's existing cooked artifacts");
            }
        }

        // 4. Stage.
        const std::filesystem::path outDir =
            PackageOutputDir(context.project, PackagePlatformName(), context.request.config);
        const auto staged = StagePackage(PackageManifestPath(context.project), context.request.config, outDir);
        if (!staged)
        {
            output_.Error(staged.error());
            return kExitRefused;
        }
        output_.Always("package: staged " + std::to_string(staged->copied) + " entr" +
                       (staged->copied == 1 ? "y" : "ies") + " -> " + outDir.generic_string());
        return kExitOk;
    }

    int BuildExecutor::Generate(
        const DriverContext& context) const
    {
        const auto premake =
            backends_.ResolvePremake(
                *context.sdkRoot);

        if (!premake)
        {
            output_.Error(
                premake.error());

            return kExitRefused;
        }

        ProcessPlan plan;
        plan.steps.push_back(
            ComposeGenerate(
                context.project,
                *premake,
                context.request.action));

        return ExecutePlan(
            plan,
            processes_,
            output_,
            "[premake]");
    }

    int BuildExecutor::Build(
        const DriverContext& context,
        BuildOperation operation) const
    {
        const auto builder =
            backends_.ResolveBuilder(
                context.backend);

        if (!builder)
        {
            output_.Error(
                builder.error());

            return kExitRefused;
        }

        const auto backendContext =
            backends_.ResolveBackendContext(
                context.backend,
                context.project);

        if (!backendContext)
        {
            output_.Error(
                backendContext.error());

            return kExitRefused;
        }

        const ProcessPlan plan =
            ComposeBuild(
                context.backend,
                *builder,
                *backendContext,
                context.request.config,
                operation);

        if (plan.steps.empty())
        {
            output_.Error(
                "build backend '" +
                std::string(
                    BuildBackendName(
                        context.backend)) +
                "' cannot compose this operation");

            return kExitRefused;
        }

        if (const int exit =
            ExecutePlan(
                plan,
                processes_,
                output_,
                BuildBackendPrefix(
                    context.backend));
            exit != kExitOk)
        {
            return exit;
        }

        return StageIfNinja(
            context,
            operation,
            output_);
    }

    int BuildExecutor::CleanBackend(
        const DriverContext& context) const
    {
        if (context.backend ==
            BuildBackend::None)
        {
            output_.Info(
                "action '" +
                context.request.action +
                "' has no known build backend -- "
                "running filesystem clean only");

            return kExitOk;
        }

        const auto builder =
            backends_.ResolveBuilder(
                context.backend);

        if (!builder)
        {
            output_.Info(
                builder.error() +
                " -- running filesystem clean only");

            return kExitOk;
        }

        const auto backendContext =
            backends_.ResolveBackendContext(
                context.backend,
                context.project);

        if (!backendContext)
        {
            output_.Info(
                backendContext.error() +
                " -- running filesystem clean only");

            return kExitOk;
        }

        const ProcessPlan plan =
            ComposeBuild(
                context.backend,
                *builder,
                *backendContext,
                context.request.config,
                BuildOperation::Clean);

        if (plan.steps.empty())
        {
            output_.Error(
                "build backend '" +
                std::string(
                    BuildBackendName(
                        context.backend)) +
                "' cannot compose clean");

            return kExitRefused;
        }

        return ExecutePlan(
            plan,
            processes_,
            output_,
            BuildBackendPrefix(
                context.backend));
    }
}
