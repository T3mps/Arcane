// The monitor's decision rule (crash window plan 2, task 9; spec §5.8, D15).
//
// PURE and std-only on purpose, like ReporterArgs/ReportView/HangSession:
// MonitorRule.cpp source-compiles into ArcaneTests (premake5.lua, ArcaneTests'
// `files` list), and that list is NOT gated on the target OS (Foundation Risk
// 4) -- so nothing here may reach windows.h. The Win32 half (the wait on the
// host's handle, the report-directory scan, the recovered-event probe, the
// synthesized report) lives in Monitor.cpp and only APPLIES what is decided
// here.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace Arcane::Reporter
{
    enum class MonitorVerdict { Silent, Synthesize };

    // UE's rule (CrashReportAnalyticsSessionSummary.cpp:238-257, 415-417): the
    // host RECORDS its clean exit by deleting its session record; a record
    // still present when the process is gone is an abnormal exit, whatever the
    // exit code -- an external kill's code 1 included. `crashPathSpoke` means
    // the crash path already told this death's story (a crash reporter, or a
    // hang window, owns that view -- see the two helpers below for exactly
    // what counts). The exit code only NAMES the reason (UE :1090-1137).
    [[nodiscard]] MonitorVerdict ClassifyHostExit(bool sessionRecordPresent, bool crashPathSpoke);

    // "STATUS_STACK_BUFFER_OVERRUN" for 0xC0000409, ... ; "" when unknown. The
    // table is UE's (CrashReportClientApp.cpp:1093-1112) plus a few.
    [[nodiscard]] std::string_view NtStatusName(std::uint32_t code);

    // "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN" (name omitted when unknown)
    [[nodiscard]] std::string AbnormalExitReason(std::uint32_t code);

    // R100: whether `fileName` is one of THIS host's reports -- Diagnostics.cpp
    // names every stem "<app>-<stamp>-pid<n>", so the app prefix AND the pid
    // suffix must both match. The reporter installs its own Diagnostics into
    // the same directory (D13: "ArcaneCrashReporter-...") and must never be
    // mistaken for the host speaking; neither may another process of the
    // same app (a second editor) whose reports share the folder. An empty
    // app matches nothing.
    [[nodiscard]] bool IsHostReportName(std::string_view fileName, std::string_view app, std::uint32_t pid);

    // Whether one of the host's reports speaks for its DEATH: only a FATAL one
    // (envelope exitCode != 0 -- the crash path ended the process with it).
    // A survivable report (hang, gpu-stall, gpu-crash, manual: exitCode 0)
    // was written by a host that went on living, so a hang reported minutes
    // before an external kill says nothing about the kill (task 9's
    // hang-then-kill decision).
    [[nodiscard]] bool ReportSpeaksForExit(int reportExitCode);

    // Whether a hang window already on screen owns this death: an attended
    // hang reporter is still alive and its host never recovered (the D11
    // event is unsignalled), AND HangSession's table keeps that window up as
    // the crash view for this exit code -- every code but 10/12/13, 11
    // (Terminate and Collect) included. For 10/12/13 the hang window closes
    // itself, so it cannot be what the user sees.
    [[nodiscard]] bool HangWindowOwnsExit(bool unrecoveredHangReporterAlive, std::uint32_t hostExitCode);
}
