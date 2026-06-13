#pragma once

// The engine's phase layer over Astra's flat SystemScheduler. Astra has no
// stage/phase concept; the engine maintains one scheduler per phase and runs
// them in order. All share one (optional) enkiTS-backed executor.
//
// Header-only by design: the simulation Registry is owned by the host module,
// and Astra's TypeID/MetaRegistry are per-module -- a registry must be touched
// by exactly one module, so the schedulers that touch it live where it lives.

#include <Astra/System/SystemScheduler.hpp>
#include <Astra/System/SystemExecutor.hpp>
#include <Astra/Core/WorkScheduler.hpp>

#include <memory>

namespace Arcane
{
    struct SystemSchedulers
    {
        Astra::SystemScheduler fixedUpdate;   // fixed-rate sim (60 Hz)
        Astra::SystemScheduler update;        // once per rendered frame (variable dt)
        Astra::SystemScheduler render;        // render submission (single-threaded)
        Astra::ParallelExecutor executor;     // enkiTS-backed; null scheduler -> sequential

        // sched may be null (sequential inline execution -- fine for tests/headless).
        explicit SystemSchedulers(std::shared_ptr<Astra::IWorkScheduler> sched)
            : executor(std::move(sched)) {}
    };
}
