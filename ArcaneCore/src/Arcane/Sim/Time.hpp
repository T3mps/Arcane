#pragma once

// Arcane::Time -- the simulation clock as a registry RESOURCE (input-seam spec
// 2026-10-02 s3). RunLoop republishes it before every fixed step and again
// before the Update scheduler, so it is always present in a world a host
// advances and survives every registry swap (Play/Stop, scene open, hot
// reload) by construction. Systems declare it as a parameter:
//
//     void operator()(Arcane::ECS::Res<Arcane::Time> time)   // time->fixedDt, time->fixedStep, ...
//
// Code outside a system reads Registry().GetResource<Arcane::Time>().
//
// TRANSIENT (IN-8 ruling, spec s4 amendment): never written into a registry
// snapshot, so a restore never revives a stale clock. RunLoop::Rebind
// republishes it (step 0) the moment a registry is swapped in.
//
// Plain data with no Core includes: RunLoop.hpp sits on the plugin-facing
// include chain, which stays Core-free (RunLoop.hpp's own note).

#include <cstdint>

namespace Arcane
{
    struct Time
    {
        static constexpr bool AstraTransientResource = true;

        double        realDt      = 0.0;   // this frame's wall-clock dt, unscaled
        double        dt          = 0.0;   // realDt * timeScale; 0 while paused
        double        fixedDt     = 0.0;   // 1 / fixedHz, the canonical fixed step
        double        alpha       = 0.0;   // render interpolation; inside a fixed step it is the PREVIOUS frame's
        double        elapsed     = 0.0;   // sim time: fixedStep * fixedDt (stays monotonic across a SetFixedHz change)
        std::uint64_t fixedStep   = 0;     // fixed steps since the registry was bound (first step = 1)
        double        timeScale   = 1.0;
        bool          paused      = false;
        bool          inFixedStep = false; // true while the fixed phase (plugin hook + fixedUpdate) runs
    };
}
