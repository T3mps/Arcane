#include "BuildExecutor.hpp"

#include "Exit.hpp"

#include <string>

namespace arcbuild
{
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

        return ExecutePlan(
            plan,
            processes_,
            output_,
            BuildBackendPrefix(
                context.backend));
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
