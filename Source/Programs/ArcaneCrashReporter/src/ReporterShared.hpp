// The pieces of ReporterMain.cpp that monitor mode reuses (crash window plan
// 2, task 9). Win32-side, like ReporterWindow.hpp: never compiled into
// ArcaneTests.
//
// R99: the monitor's window handles its buttons with the SAME OnButton the
// report window uses -- a failed Relaunch stays on screen and logs (R90), a
// failed clipboard or folder open is logged (R91) -- rather than a second,
// simpler handler that would drift from it. The monitor has no hang to
// manage, so it passes `hang = nullptr`; the hang buttons are hidden in its
// view and no host events are posted to it.
#pragma once
#include "ReporterWindow.hpp"

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Platform/NativeWindow.hpp>

namespace Arcane::Reporter
{
    // Defined in ReporterMain.cpp (task 8): the attended hang view's watch on
    // its host. Opaque here -- OnButton only forwards it to the hang table.
    struct HangWatch;

    // Runs on the window thread (it IS ReporterWindow's `onCommand`). `hang`
    // is null for every view that is not an attended hang: then the two hang
    // buttons and the three host events are ignored.
    void OnButton(int id, ReporterWindow& ui, NativeWindow& window, HangWatch* hang);

    // R67: Install/Shutdown bound by SCOPE, not by control flow.
    // Diagnostics.hpp is explicit that Install creates the crash thread and
    // its events even with installCrashHandler and startHangWatchdog both
    // off, and the death fixture's own comment records what skipping Shutdown
    // cost last time: a joinable thread at static destruction, whose
    // destructor calls std::terminate.
    //
    // R80: a TerminateProcess exit (task 5's kDeadline) never runs this guard,
    // and that is CORRECT -- TerminateProcess runs no destructors by design,
    // and calling Shutdown() under an already-expired deadline could itself
    // block on the very worker the deadline just gave up on.
    struct ArmedDiagnostics
    {
        explicit ArmedDiagnostics(const Arcane::Diagnostics::Config& cfg) { Arcane::Diagnostics::Install(cfg); }
        ~ArmedDiagnostics() { Arcane::Diagnostics::Shutdown(); }

        ArmedDiagnostics(const ArmedDiagnostics&)            = delete;
        ArmedDiagnostics& operator=(const ArmedDiagnostics&) = delete;
    };
}
