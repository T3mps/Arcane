#include "Application.hpp"
#include "Backend.hpp"
#include "Bootstrap.hpp"
#include "BuildExecutor.hpp"
#include "Environment.hpp"
#include "Output.hpp"
#include "Pipeline.hpp"
#include "Probe.hpp"
#include "Process.hpp"
#include "ProjectCleaner.hpp"

// Compose the concrete adapters at the entry point. The pipeline itself
// uses narrow interfaces so its ordering can be tested without child processes.
int main(int argc, char** argv)
{
    using namespace arcbuild;

    Output output;
    Environment environment;

    ProcessRunner processes { output };

    BackendResolver backends;

    Bootstrap bootstrap { environment };

    SlotInspector slots;

    ProjectCleaner cleaner { output };

    BuildExecutor executor { backends, processes, output };

    BuildPipeline pipeline { executor, slots, cleaner, output };

    Application application { bootstrap, pipeline, output };

    return application.Run(argc, argv);
}
