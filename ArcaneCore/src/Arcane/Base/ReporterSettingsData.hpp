#pragma once

// The diagnostics.reporter.* settings struct as plain data (settings arc
// S6-4): std only, no reflection, no registry. ArcaneCrashReporter has no
// Astra include path and no registry of its own; its argument contract
// (ReporterArgs.hpp) takes its defaults from this struct, and the host hands
// it the published values on its command line
// (Diagnostics::ReporterSettingsArgs). The reflection block and Settings<T>()
// live one header up, in ReporterSettings.hpp. Include THAT one to read or
// register settings.
//
// The member initializers are the defaults (inventory Part 2 "Crash
// reporter"); SweepReporterTest pins them.

#include <cstdint>

namespace Arcane
{
    struct DiagnosticsReporterSettings
    {
        std::uint32_t deadlineSeconds = 60;            // the unattended symbolization deadline (--deadline)
        std::uint32_t maxFramesPerThread = 64;         // stack walk depth, every thread but the faulting one
        std::uint32_t maxFramesFaultingThread = 8192;  // ...and the faulting thread (UE's MaxFrames)
        std::uint32_t maxThreads = 64;                 // threads walked per report
        std::uint32_t dbgengWaitMs = 30000;            // dbgeng WaitForEvent on the dump open
        std::uint32_t hangLogTailLines = 512;          // the monitor's log copy for an abnormal exit
        std::uint32_t flushTimeoutMs = 1000;           // the reporter's own bounded log flush
        std::uint32_t uiPollMs = 250;                  // the attended wait's slice (notices the window closing)
        std::uint32_t windowWidth = 1000;              // outer window size, px
        std::uint32_t windowHeight = 640;
        std::uint32_t windowReadyMs = 5000;            // how long Show waits for the window thread
    };
}
