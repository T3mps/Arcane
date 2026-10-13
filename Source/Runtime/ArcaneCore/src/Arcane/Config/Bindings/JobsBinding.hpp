#pragma once

// jobs.* (settings arc S2): the job system's size. Restart: a JobSystem is
// built once per Runtime, and its pool is shared by the registry, the
// schedulers and physics. The value a Runtime sees is whatever is published
// when it is constructed (Runtime.cpp applies the EngineConfig rung first).

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>

namespace Arcane
{
    struct JobsSettings
    {
        std::uint32_t workerThreads = 0;   // threads IN TOTAL (the calling thread included); 0 = hardware threads
        std::uint32_t externalThreads = 0;   // enkiTS numExternalTaskThreads (S6-8): non-enki threads that may submit tasks
        std::uint32_t shaderCompileThreads = 1;   // ShaderCompiler's DXC worker threads (S6-8)
    };

    ARC_REFLECT_TYPE(JobsSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "jobs", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(JobsSettings, workerThreads)
            ARC_REFLECT_ATTR(Range, 0.0, 256.0)
            ARC_REFLECT_ATTR(Tooltip, "Job-system threads in total, the main thread included. 0 = one per hardware thread.")
        ARC_REFLECT_FIELD(JobsSettings, externalThreads)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 64.0)
            ARC_REFLECT_ATTR(Tooltip, "Threads outside the job system that may submit tasks to it (enkiTS external task threads). 0 = none.")
        ARC_REFLECT_FIELD(JobsSettings, shaderCompileThreads)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 256.0)
            ARC_REFLECT_ATTR(Tooltip, "Background shader-compile threads. They share one queue; 1 compiles in submission order.")
    ARC_END_REFLECT_TYPE()

    // The JobSystem ctor's argument. 0 resolves to enkiTS' hardware count, so
    // JobSystem(ResolveWorkerThreads({})) == JobSystem(0) (JobSystem.cpp:102-104).
    ARC_CORE_API unsigned ResolveWorkerThreads(const JobsSettings& settings);
}
