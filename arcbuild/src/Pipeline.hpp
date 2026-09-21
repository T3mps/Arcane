#pragma once

#include "BuildExecutor.hpp"
#include "DriverContext.hpp"
#include "Output.hpp"
#include "Probe.hpp"
#include "ProjectCleaner.hpp"

namespace arcbuild
{
    [[nodiscard]]
    BuildOperation OperationForBuild(
        bool forceRebuild,
        const Verdict& verdict);

    [[nodiscard]]
    int MergeCleanResults(
        int backendExit,
        int filesystemExit) noexcept;

    class BuildPipeline
    {
    public:
        BuildPipeline(
            IBuildExecutor& executor,
            ISlotInspector& slots,
            IProjectCleaner& cleaner,
            IOutput& output)
            : executor_(executor),
            slots_(slots),
            cleaner_(cleaner),
            output_(output)
        {}

        [[nodiscard]]
        int Run(
            const DriverContext& context) const;

    private:
        [[nodiscard]]
        int Build(
            const DriverContext& context) const;

        [[nodiscard]]
        int Rebuild(
            const DriverContext& context) const;

        [[nodiscard]]
        int Clean(
            const DriverContext& context) const;

        [[nodiscard]]
        int Probe(
            const DriverContext& context) const;

        [[nodiscard]]
        bool RequireBuildBackend(
            const DriverContext& context) const;

        IBuildExecutor&  executor_;
        ISlotInspector&  slots_;
        IProjectCleaner& cleaner_;
        IOutput&         output_;
    };
}
