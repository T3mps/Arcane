#include "HangSession.hpp"

namespace Arcane::Reporter
{
    HangOutcome DecideHang(HangEvent e, std::uint32_t hostExitCode)
    {
        // The literals are the two contracts' numbers, spelled out rather than
        // pulled from Diagnostics.hpp / ReporterArgs.hpp: this TU compiles into
        // ArcaneTests on every OS and must stay std-only (see the header). The
        // host codes are spec §4's (Diagnostics::ExitCode: 10 crashed, 11 hang
        // terminated, 12 exit sentinel, 13 crash in the crash path); 4 is
        // Reporter::ExitCode::kHostMismatch.
        HangOutcome o;
        switch (e)
        {
        case HangEvent::HostRecovered:     o.closeWindow = true; break;
        case HangEvent::KeepWaitingChosen: o.closeWindow = true; break;
        case HangEvent::IdentityMismatch:  o.closeWindow = true; o.exitCode = 4; break;
        case HangEvent::TerminateChosen:   o.terminateHost = true; o.becomeCrashView = true; break;
        case HangEvent::HostExited:
            if (hostExitCode == 10 || hostExitCode == 12 || hostExitCode == 13) o.closeWindow = true;   // the crash reporter owns that view
            else o.becomeCrashView = true;
            break;
        }
        return o;
    }
}
