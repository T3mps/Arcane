#pragma once

// Engine diagnostics (Base module): the post-mortem capture seam, sibling to
// Log.hpp and Assert.hpp. Two triggers, ONE report path:
//
//   1. CRASH -- SetUnhandledExceptionFilter. An access violation or an escaped
//      C++ exception writes a report instead of dying mute.
//   2. HANG  -- a watchdog thread. The main thread publishes a Heartbeat(); if
//      it stops advancing for longer than Config::hangSeconds, the watchdog
//      writes the SAME report for a process that is still very much alive.
//
// (2) is the reason this module exists. Windows Error Reporting's LocalDumps
// covers (1) already and is a fine backstop, but WER only ever fires on process
// DEATH -- a wedged main thread ("Not Responding") produces nothing, anywhere,
// forever. Neither does a debugger help much when the defect is intermittent:
// it requires a human attached at the exact moment. The watchdog does not.
//
// A report is:
//   - a MINIDUMP (.dmp), written FIRST because MiniDumpWriteDump is the
//     battle-tested path and must land even if the text walk below wedges;
//   - a SYMBOLIZED TEXT stack (.txt) of EVERY thread, also echoed into the
//     engine log. "Every thread" is deliberate: a main thread blocked on a
//     worker names the wrong culprit, and the answer is the other stack.
//
// Reading it needs no debugger, which is the whole point -- the text file is
// the artifact you paste into a bug report.
//
// Windows-only today; every entry point compiles and no-ops elsewhere so the
// planned Linux port keeps linking.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <string>

namespace Arcane::Diagnostics
{
    struct Config
    {
        // Names the report files. Set it per host ("ArcaneEditor").
        std::string appName = "Arcane";

        // Where reports land. Empty => "<exe dir>/diagnostics". Chosen over the
        // CWD on purpose: hosts are documented as cd-then-run, and a report that
        // lands wherever the user happened to be is a report nobody finds.
        std::string dumpDir;

        // Main-thread stall that counts as a hang. Generous by default: a cold
        // shader compile or a large project scan can legitimately block the main
        // thread for seconds, and a false report that cries wolf gets ignored.
        std::uint32_t hangSeconds = 12;

        bool installCrashHandler = true;
        bool startHangWatchdog   = true;
    };

    // Registers the CALLING thread as the main thread and arms both triggers.
    // Call once, early in main(), beside Log::Init(). Idempotent.
    ARCANE_API void Install(const Config& cfg);

    // Disarms both triggers and joins the watchdog. Idempotent; safe to skip
    // (the process exiting is also fine).
    ARCANE_API void Shutdown() noexcept;

    // "The main thread is alive." One relaxed atomic store -- cheap enough for
    // every frame, which is where it belongs. A hang is DEFINED as this not
    // being called, so it must sit on every path the main thread loops through:
    // the frame loop AND the boot/project-switch pump.
    ARCANE_API void Heartbeat() noexcept;

    // Optional label for what the main thread is currently doing (a boot stage
    // id, say). Reproduced verbatim at the top of a report -- it turns "it hung"
    // into "it hung in switch_plugin_load". Cheap but not free (takes a lock);
    // call it per phase, never per frame.
    ARCANE_API void SetPhase(std::string phase);

    // Writes a report immediately, whatever the process state. Returns the .txt
    // path, or an empty string if nothing could be written. Public because a
    // manual trigger is useful, and because it is how the self-test proves the
    // capture path works BEFORE an intermittent bug depends on it.
    ARCANE_API std::string WriteReport(const char* reason);

    // Reports written this process. The observable the watchdog test asserts on.
    [[nodiscard]] ARCANE_API std::uint32_t ReportCount() noexcept;
}
