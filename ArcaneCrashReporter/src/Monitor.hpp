// Monitor mode (crash window plan 2, task 9; spec §5.8, D15/D16): the
// reporter, pre-launched by a windowed host, waits on that host's process
// handle and turns a death the in-process crash path never saw -- a
// __fastfail, /GS, heap corruption, a stack overflow with no room for SEH, an
// external kill -- into an abnormal-exit report. Win32; the decision itself is
// MonitorRule.hpp's, pure.
#pragma once
#include "ReporterArgs.hpp"

namespace Arcane::Reporter
{
    // Reporter::ExitCode::kOk whether or not the monitor had anything to say:
    // a monitor line that parsed is a monitor that did its job.
    [[nodiscard]] int RunMonitor(const Args& a);
}
