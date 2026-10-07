#include <Arcane/Config/Bindings/JobsBinding.hpp>

#include <TaskScheduler.h>

ARC_SETTINGS(Arcane::JobsSettings);

namespace Arcane
{
    unsigned ResolveWorkerThreads(const JobsSettings& settings)
    {
        return settings.workerThreads != 0 ? settings.workerThreads : enki::GetNumHardwareThreads();
    }
}
