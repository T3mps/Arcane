#pragma once

#include "Bootstrap.hpp"
#include "Output.hpp"
#include "Pipeline.hpp"

namespace arcbuild
{
    class Application
    {
    public:
        Application(
            Bootstrap& bootstrap,
            BuildPipeline& pipeline,
            IOutput& output)
            : bootstrap_(bootstrap),
            pipeline_(pipeline),
            output_(output)
        {
        }

        [[nodiscard]]
        int Run(
            int argc,
            char** argv);

    private:
        void PrintUsage() const;

        Bootstrap&     bootstrap_;
        BuildPipeline& pipeline_;
        IOutput&       output_;
    };
}
