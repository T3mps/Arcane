#include "Pipeline.hpp"

#include "Build.hpp"
#include "Exit.hpp"
#include "Slot.hpp"

#include <string>

namespace arcbuild
{
    BuildOperation OperationForBuild(
        bool forceRebuild,
        const Verdict& verdict)
    {
        return forceRebuild || verdict.rebuild
            ? BuildOperation::Rebuild
            : BuildOperation::Build;
    }

    int MergeCleanResults(
        int backendExit,
        int filesystemExit) noexcept
    {
        return filesystemExit != kExitOk
            ? filesystemExit
            : backendExit;
    }

    bool BuildPipeline::RequireBuildBackend(
        const DriverContext& context) const
    {
        if (context.backend !=
            BuildBackend::None)
        {
            return true;
        }

        output_.Error(
            "no build backend available for action '" +
            context.request.action +
            "'");

        return false;
    }

    int BuildPipeline::Build(
        const DriverContext& context) const
    {
        if (!RequireBuildBackend(context))
            return kExitRefused;

        if (const int exit =
            executor_.Generate(context);
            exit != kExitOk)
        {
            return exit;
        }

        if (context.request.forceRebuild)
        {
            return executor_.Build(
                context,
                BuildOperation::Rebuild);
        }

        const SlotProbe probe =
            slots_.Inspect(
                context.project,
                context.request.config);

        const Verdict verdict =
            Decide(
                probe.state);

        output_.Info(
            slots_.Describe(
                probe,
                context.request.config,
                verdict));

        return executor_.Build(
            context,
            OperationForBuild(
                false,
                verdict));
    }

    int BuildPipeline::Rebuild(
        const DriverContext& context) const
    {
        if (!RequireBuildBackend(context))
            return kExitRefused;

        if (const int exit =
            executor_.Generate(context);
            exit != kExitOk)
        {
            return exit;
        }

        return executor_.Build(
            context,
            BuildOperation::Rebuild);
    }

    int BuildPipeline::Clean(
        const DriverContext& context) const
    {
        const int backendExit =
            executor_.CleanBackend(
                context);

        const int projectExit =
            cleaner_.Clean(
                context.project,
                context.request.config);

        return MergeCleanResults(
            backendExit,
            projectExit);
    }

    int BuildPipeline::Probe(
        const DriverContext& context) const
    {
        const SlotProbe probe =
            slots_.Inspect(
                context.project,
                context.request.config);

        const Verdict verdict =
            Decide(
                probe.state);

        output_.Always(
            slots_.Describe(
                probe,
                context.request.config,
                verdict));

        return ProbeExitCode(
            probe.state);
    }

    int BuildPipeline::Run(
        const DriverContext& context) const
    {
        output_.Info(
            std::string(
                CommandName(
                    context.request.command)) +
            " " +
            context.project.name +
            " (" +
            context.request.config +
            ") in " +
            context.project.root.generic_string() +
            " against SDK " +
            (context.sdkRoot
                ? context.sdkRoot->generic_string()
                : std::string("<none>")));

        switch (context.request.command)
        {
        case Command::Generate:
            return executor_.Generate(
                context);

        case Command::Build:
            return Build(
                context);

        case Command::Rebuild:
            return Rebuild(
                context);

        case Command::Clean:
            return Clean(
                context);

        case Command::Probe:
            return Probe(
                context);
        }

        output_.Error(
            "unreachable command");

        return kExitRefused;
    }
}
