#pragma once

#include "Backend.hpp"
#include "Compose.hpp"
#include "DriverContext.hpp"
#include "Output.hpp"
#include "Process.hpp"

namespace arcbuild
{
    class IBuildExecutor
    {
    public:
        virtual ~IBuildExecutor() = default;

        [[nodiscard]]
        virtual int Generate(
            const DriverContext& context) const = 0;

        [[nodiscard]]
        virtual int Build(
            const DriverContext& context,
            BuildOperation operation) const = 0;

        [[nodiscard]]
        virtual int CleanBackend(
            const DriverContext& context) const = 0;

        // `arcbuild package` (engine modularization s3.3). Not pure: the
        // recording fakes the pipeline tests use have no package path.
        [[nodiscard]]
        virtual int Package(
            const DriverContext& context) const;
    };

    class BuildExecutor final : public IBuildExecutor
    {
    public:
        BuildExecutor(
            BackendResolver& backends,
            IProcessRunner& processes,
            IOutput& output)
            : backends_(backends),
            processes_(processes),
            output_(output)
        {
        }

        [[nodiscard]]
        int Generate(
            const DriverContext& context) const override;

        [[nodiscard]]
        int Build(
            const DriverContext& context,
            BuildOperation operation) const override;

        [[nodiscard]]
        int CleanBackend(
            const DriverContext& context) const override;

        // 1. the SDK's monolithic layout: generate + build (incremental);
        // 2. the project's monolithic workspace (game static lib + the
        //    <Project> exe, build/arcane.lua): generate + build;
        // 3. cook the project's content (arccook beside arcbuild, when built);
        // 4. stage Packages/<Project>-<platform>-<config>/ from the stage list.
        // MSBuild and Make (set MAKEFLAGS=-jN for a parallel engine build).
        [[nodiscard]]
        int Package(
            const DriverContext& context) const override;

    private:
        BackendResolver& backends_;
        IProcessRunner&  processes_;
        IOutput&         output_;
    };
}
