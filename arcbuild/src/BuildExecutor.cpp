#include "BuildExecutor.hpp"

#include "Exit.hpp"
#include "ProjectLayout.hpp"
#include "Stage.hpp"

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
