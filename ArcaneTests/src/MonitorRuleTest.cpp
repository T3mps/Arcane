// The monitor's decision rule, as pure functions (crash window plan 2, task 9;
// spec §5.8, D15).
//
// MonitorRule.cpp source-compiles into this exe (premake5.lua, ArcaneTests'
// `files` list) exactly as ReporterArgs.cpp and HangSession.cpp do -- and that
// list is NOT gated on the target OS, so this TU and the one it drives must
// stay free of windows.h (Foundation Risk 4). The Win32 half -- the wait on
// the host's handle, the directory scan, the recovered-event probe -- lives in
// Monitor.cpp and is proven by the [diag] death-fixture cases and the desk.

#include "MonitorRule.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Reporter;

TEST_CASE("monitor rule: a clean exit deletes the session record; a record left behind is abnormal unless the crash path spoke", "[reporter]")
{
    CHECK(ClassifyHostExit(/*record*/false, /*fresh*/false) == MonitorVerdict::Silent);      // clean exit, whatever the code
    CHECK(ClassifyHostExit(/*record*/false, /*fresh*/true)  == MonitorVerdict::Silent);
    CHECK(ClassifyHostExit(/*record*/true,  /*fresh*/true)  == MonitorVerdict::Silent);      // exit 10/12/13: a crash reporter owns it
    CHECK(ClassifyHostExit(/*record*/true,  /*fresh*/false) == MonitorVerdict::Synthesize);  // fail-fast, external kill, /GS, heap
}

TEST_CASE("monitor rule: NTSTATUS names (UE's table and ours) and the reason line", "[reporter]")
{
    CHECK(NtStatusName(0xC0000409u) == "STATUS_STACK_BUFFER_OVERRUN");
    CHECK(NtStatusName(0xC0000374u) == "STATUS_HEAP_CORRUPTION");
    CHECK(NtStatusName(0xC00000FDu) == "STATUS_STACK_OVERFLOW");
    CHECK(NtStatusName(0xC0000005u) == "STATUS_ACCESS_VIOLATION");
    CHECK(NtStatusName(0xC0000602u) == "STATUS_FAIL_FAST_EXCEPTION");
    CHECK(NtStatusName(0xC000041Du) == "STATUS_FATAL_USER_CALLBACK_EXCEPTION");   // UE :1100
    CHECK(NtStatusName(0xC000012Du) == "STATUS_FATAL_MEMORY_EXHAUSTION");         // UE :1103
    CHECK(NtStatusName(0xC000000Du) == "STATUS_INVALID_PARAMETER");               // UE :1108
    CHECK(NtStatusName(0xC0000006u) == "STATUS_IN_PAGE_ERROR");                   // UE :1109
    CHECK(NtStatusName(0xC000013Au) == "STATUS_CONTROL_C_EXIT");
    CHECK(NtStatusName(0xE06D7363u) == "MSVC_CPP_EXCEPTION");
    CHECK(NtStatusName(42u).empty());
    CHECK(AbnormalExitReason(0xC0000409u) == "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN");
    CHECK(AbnormalExitReason(1u) == "abnormal-exit: 0x00000001");   // an external kill: named by number only
}

// R100 + the hang-then-kill decision: what counts as "the crash path already
// spoke". Only THIS host's reports (never the reporter's own, which share the
// directory under D13), only FATAL ones (a survivable hang report written
// minutes earlier says nothing about a later kill), and a live hang window
// that never saw its host recover owns the death exactly where HangSession's
// table keeps that window up as the crash view.
TEST_CASE("monitor rule: only this host's fatal reports or its unrecovered hang window speak for the exit", "[reporter]")
{
    // The stem is Diagnostics.cpp's "<app>-<stamp>-pid<n>".
    CHECK(IsHostReportName("DeathFixture-20260923-101112-123-pid4242.arcdiag", "DeathFixture", 4242));
    CHECK_FALSE(IsHostReportName("ArcaneCrashReporter-20260923-101112-123-pid77.arcdiag", "DeathFixture", 4242));   // the reporter's own
    CHECK_FALSE(IsHostReportName("DeathFixture-20260923-101112-123-pid4243.arcdiag", "DeathFixture", 4242));        // another process
    CHECK_FALSE(IsHostReportName("DeathFixture-20260923-101112-123-pid42421.arcdiag", "DeathFixture", 4242));       // a pid that merely starts the same
    CHECK_FALSE(IsHostReportName("DeathFixtureX-20260923-101112-123-pid4242.arcdiag", "DeathFixture", 4242));       // an app that merely starts the same
    CHECK_FALSE(IsHostReportName("DeathFixture-20260923-101112-123-pid4242.txt", "DeathFixture", 4242));
    CHECK_FALSE(IsHostReportName("DeathFixture-20260923-101112-123-pid4242.arcdiag", "", 4242));                   // no app: nothing is ours

    CHECK(ReportSpeaksForExit(10));        // kCrashed: the crash path wrote this death
    CHECK(ReportSpeaksForExit(12));        // the exit sentinel
    CHECK(ReportSpeaksForExit(1));         // the D3D12 device-loss fail-fast exits 1 through the crash path
    CHECK_FALSE(ReportSpeaksForExit(0));   // hang / gpu-stall / gpu-crash / manual: the host SURVIVED that report

    // HangSession.cpp: HostExited 10/12/13 closes the hang window (another
    // reporter owns the view); every other code, 11 included, keeps it up.
    CHECK(HangWindowOwnsExit(true, 11u));            // Terminate and Collect
    CHECK(HangWindowOwnsExit(true, 1u));             // killed while the hang window was up
    CHECK(HangWindowOwnsExit(true, 0xC0000409u));
    CHECK_FALSE(HangWindowOwnsExit(true, 13u));      // the window closed: a crash in the crash path may have written nothing
    CHECK_FALSE(HangWindowOwnsExit(true, 10u));
    CHECK_FALSE(HangWindowOwnsExit(false, 11u));     // no live, unrecovered hang window: nothing on screen speaks
}
