#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#include <cstdint>
#include <mutex>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// AM1: whether a fixed-tick host (the server, the runtime) advances its sim on
// this loop iteration. The host asks Consume once per iteration, after
// DrainFrame and before the sim step: Run and Step call Advance (and bump
// HostGate::frame), Idle skips it while the rest of the iteration (pump,
// publish, heartbeat, render) still runs. The remote play commands (Task 29)
// write it from the main thread inside Drain; the mutex keeps a reader on
// another thread (a test, a later status hook) consistent.
enum class TickDecision : std::uint8_t { Idle, Step, Run };

class ARC_REMOTE_API TickGate {
public:
    void SetRunning(bool running);
    void SetPaused(bool paused);
    // Adds n single steps to the pending count (saturating).
    void RequestSteps(std::uint32_t n);
    // running && !paused: Run, and a leftover step count is cleared.
    // Otherwise a pending step is Step (and the count drops by one), else Idle.
    TickDecision Consume();
    bool Running() const;
    bool Paused() const;
    std::uint32_t StepsRemaining() const;

private:
    mutable std::mutex m_mutex;
    bool m_running = true;
    bool m_paused = false;
    std::uint32_t m_steps = 0;
};

} // namespace Arcane::Remote

#endif
