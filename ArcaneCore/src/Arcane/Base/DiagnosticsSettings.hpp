#pragma once

// diagnostics.* (settings arc S2 drawMarkers; S6-2 the crash/hang capture
// tunables, inventory Part 1 "Base / Diagnostics / Log"). Declared in Core,
// where Diagnostics lives. The struct itself is plain data in
// DiagnosticsSettingsData.hpp (Diagnostics.hpp's Config takes its defaults
// from it); this header adds the reflection block and Settings<T>().
//
// Evidence capture cannot be switched off in a shipped build (inventory R2):
// installCrashHandler and hangWatchdog are Dev (compiled out of Dist, so Dist
// always captures), never archived, and CommandLineOnly (`--set` for a
// debugger session). The capture fields are Restart: read once, at
// Diagnostics::Install, after HostBoot::ApplyEarlyConfigRungs, and snapshotted
// into Diagnostics::Config (the crash path never reads the registry).
// drawMarkers keeps its S2 scope (Project) and apply (Live); perfLog and
// perfLogIntervalFrames are Live (FramePerf reads them every frame) and NOT
// Dev: --perf stays usable in Dist, where the frame-time floor is measured
// (user decision 2026-10-06).
//
// reporterPath names the program the crash path launches, so it is
// LaunchesProgram (settings S7-SEC): only --set and the machine-wide
// EditorUser rung set it, never a project's Config/ or Saved/Config, and
// Install falls back to the bundled reporter when it is not a launchable file.

#include <Arcane/Base/DiagnosticsSettingsData.hpp>
#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    ARC_REFLECT_ENUM(MinidumpKind)
        ARC_REFLECT_ENUM_VALUE(MinidumpKind, Small)
        ARC_REFLECT_ENUM_VALUE(MinidumpKind, Default)
        ARC_REFLECT_ENUM_VALUE(MinidumpKind, Full)
    ARC_END_REFLECT_ENUM()

    ARC_REFLECT_TYPE(DiagnosticsSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "diagnostics", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(DiagnosticsSettings, drawMarkers)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Scope, SettingScope::Project) ARC_REFLECT_ATTR(Apply, ApplyMode::Live)
            ARC_REFLECT_ATTR(Tooltip, "Per-draw GPU markers for PIX/RenderDoc. Pass-level scopes stay on.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, dumpDir)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Widget, "path:dir")
            ARC_REFLECT_ATTR(Tooltip, "Where crash and hang reports land. Empty = <exe dir>/diagnostics.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, hangSeconds)
            ARC_REFLECT_ATTR(Range, 1.0, 600.0)
            ARC_REFLECT_ATTR(Tooltip, "A main-thread stall this long (seconds) is reported as a hang. Raise it on slow machines or for long cold shader compiles.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, gpuStallSeconds)
            ARC_REFLECT_ATTR(Range, 1.0, 599.0)
            ARC_REFLECT_ATTR(Tooltip, "A GPU-progress stall this long (seconds) is reported as a GPU hang. Keep it below hangSeconds.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, installCrashHandler)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev | CVarFlags::CommandLineOnly)
            ARC_REFLECT_ATTR(Tooltip, "Install the unhandled-exception filter. --set only (debugger sessions); always on in Dist.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, hangWatchdog)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev | CVarFlags::CommandLineOnly)
            ARC_REFLECT_ATTR(Tooltip, "Start the hang watchdog. --set only (debugger sessions); always on in Dist.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, reporterPath)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev | CVarFlags::LaunchesProgram) ARC_REFLECT_ATTR(Widget, "path:file")
            ARC_REFLECT_ATTR(Tooltip, "The crash reporter executable. Empty = <exe dir>/ArcaneCrashReporter.exe. "
                                      "Machine-wide only: project config never sets it.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, spawnReporter)
            ARC_REFLECT_ATTR(Tooltip, "Open the crash reporter window after a crash or hang report. The report is written either way.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, exitSeconds)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 600.0)
            ARC_REFLECT_ATTR(Tooltip, "A requested exit that takes longer than this (seconds) is reported as a hang at exit. 0 = off.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, crashHandlingTimeoutSeconds)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 5.0, 600.0)
            ARC_REFLECT_ATTR(Tooltip, "How long (seconds) a crashing thread waits for its report to be written before terminating anyway.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, minidumpKind)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Minidump detail. Default = stacks, handles and referenced memory; Full adds the whole heap for deep debugging (hundreds of MB).")
        ARC_REFLECT_FIELD(DiagnosticsSettings, logFlushTimeoutMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 100.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Crash-path budget (ms) for flushing the log file sink after a report.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, watchdogPollMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 10.0, 1000.0)
            ARC_REFLECT_ATTR(Tooltip, "How often (ms) the hang watchdog checks the heartbeats: its detection resolution.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, watchdogJoinTimeoutMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 100.0, 30000.0)
            ARC_REFLECT_ATTR(Tooltip, "How long (ms) shutdown waits for the hang watchdog to stop before orphaning it.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, minFatalWaitMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1000.0, 60000.0)
            ARC_REFLECT_ATTR(Tooltip, "The least time (ms) a fatal report is given to finish, even past the crash-handling deadline.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, perfLog)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live)
            ARC_REFLECT_ATTR(Tooltip, "Log per-phase frame timings ([PERF]) on the runtime host. --perf sets it.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, perfLogIntervalFrames)
            ARC_REFLECT_ATTR(Range, 1.0, 10000.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Live)
            ARC_REFLECT_ATTR(Tooltip, "Frames averaged into each [PERF] line.")
        ARC_REFLECT_FIELD(DiagnosticsSettings, logTailLines)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 10000.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Live)
            ARC_REFLECT_ATTR(Tooltip, "Log lines shown with a crash report, in the crash reporter and the editor's crash document. "
                                      "A change reaches the next crash reporter; a crash monitor already running keeps its launch value.")
    ARC_END_REFLECT_TYPE()
}
