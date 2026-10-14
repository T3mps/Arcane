// The reporter's hang protocol, as a decision table (crash window plan 2,
// task 8; spec §5.4, D6/D7).
//
// HangSession.cpp source-compiles into this exe (premake5.lua, ArcaneTests'
// `files` list) exactly as ReporterArgs.cpp, SymbolizedText.cpp and
// ReportView.cpp do -- and that list is NOT gated on the target OS, so this TU
// and the one it drives must stay free of windows.h (Foundation Risk 4). The
// Win32 half -- the waiter thread, OpenProcess/TerminateProcess, the identity
// check -- lives in ReporterMain.cpp and is proven by the [diag] cases and
// the desk.

#include "HangSession.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Reporter;

TEST_CASE("hang session: recovery and Keep Waiting close silently; a crash-exit yields to the crash reporter", "[reporter]")
{
    auto r = DecideHang(HangEvent::HostRecovered, 0);
    CHECK(r.closeWindow); CHECK_FALSE(r.terminateHost); CHECK_FALSE(r.becomeCrashView); CHECK(r.exitCode == 0);
    auto k = DecideHang(HangEvent::KeepWaitingChosen, 0);
    CHECK(k.closeWindow); CHECK(k.exitCode == 0);
    for (std::uint32_t code : { 10u, 12u, 13u })
    {
        auto e = DecideHang(HangEvent::HostExited, code);
        CHECK(e.closeWindow); CHECK_FALSE(e.becomeCrashView);
    }
}

TEST_CASE("hang session: Terminate and Collect kills the host and keeps the window as the crash view; other exits keep it too", "[reporter]")
{
    auto t = DecideHang(HangEvent::TerminateChosen, 0);
    CHECK(t.terminateHost); CHECK(t.becomeCrashView); CHECK_FALSE(t.closeWindow);
    auto own = DecideHang(HangEvent::HostExited, 11);
    CHECK(own.becomeCrashView); CHECK_FALSE(own.closeWindow);
    auto other = DecideHang(HangEvent::HostExited, 0xC0000005u);
    CHECK(other.becomeCrashView); CHECK_FALSE(other.terminateHost);
    auto bad = DecideHang(HangEvent::IdentityMismatch, 0);
    CHECK(bad.closeWindow); CHECK(bad.exitCode == 4);
}
