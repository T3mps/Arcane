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

    private:
        BackendResolver& backends_;
        IProcessRunner&  processes_;
        IOutput&         output_;
    };
}
