// HangSession.hpp -- what the reporter does when something happens to a hung
// host (crash window plan 2, task 8; spec §5.4, D6/D7).
//
// PURE and std-only on purpose, like ReporterArgs/SymbolizedText/ReportView:
// HangSession.cpp source-compiles into ArcaneTests (premake5.lua, ArcaneTests'
// `files` list), and that list is NOT gated on the target OS (Foundation Risk
// 4) -- so nothing here may reach windows.h. The Win32 half (the waiter
// thread, OpenProcess/TerminateProcess, the creation-time check) lives in
// ReporterMain.cpp and only APPLIES the outcome decided here.
#pragma once
#include <cstdint>

namespace Arcane::Reporter
{
    enum class HangEvent { HostRecovered, HostExited, TerminateChosen, KeepWaitingChosen, IdentityMismatch };

    struct HangOutcome
    {
        bool closeWindow     = false;   // close silently and exit with `exitCode`
        bool terminateHost   = false;   // TerminateProcess(host, 11) first
        bool becomeCrashView = false;   // stay up as the crash view of the report already held
        int  exitCode        = 0;       // meaningful with closeWindow; Reporter::ExitCode values (0 or 4)
    };

    // hostExitCode is meaningful for HostExited only.
    //
    //   HostRecovered      -> close, 0         (the beat resumed: nothing to report on any more)
    //   KeepWaitingChosen  -> close, 0         (D6: nothing hides and lingers; the host re-arms on progress)
    //   IdentityMismatch   -> close, 4         (D7 / spec §9: the pid is not the host we were told about)
    //   TerminateChosen    -> terminate + crash view
    //   HostExited 10/12/13-> close, 0         (a fatal report of its own: ANOTHER reporter owns that view)
    //   HostExited 11      -> crash view       (we terminated it)
    //   HostExited other   -> crash view       (the host died under the window: say so)
    [[nodiscard]] HangOutcome DecideHang(HangEvent e, std::uint32_t hostExitCode);
}
