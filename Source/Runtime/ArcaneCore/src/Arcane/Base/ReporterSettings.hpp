#pragma once

// diagnostics.reporter.* (settings arc S6-4, inventory Part 2 "Crash
// reporter"): the out-of-process crash reporter's tunables. The plain struct
// is in ReporterSettingsData.hpp; this header adds the reflection block and
// Settings<T>().
//
// The reporter has no registry. Diagnostics::Install formats these (plus
// diagnostics.logTailLines and ui.copyFlashSeconds) into the reporter's
// command line, off the crash path, so they are Restart: a reporter or crash
// monitor already launched keeps the values it was launched with. Everything
// is a Dev tuning knob except the window size, an ordinary preference.

#include <Arcane/Base/ReporterSettingsData.hpp>
#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    ARC_REFLECT_TYPE(DiagnosticsReporterSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "diagnostics.reporter", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, deadlineSeconds)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 600.0)
            ARC_REFLECT_ATTR(Tooltip, "How long (seconds) the reporter may spend symbolizing a crash dump before it "
                                         "writes the unsymbolized stack and exits.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, maxFramesPerThread)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 8.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Stack frames walked per thread in a crash report (the faulting thread has its own limit).")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, maxFramesFaultingThread)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 64.0, 65536.0)
            ARC_REFLECT_ATTR(Tooltip, "Stack frames walked on the faulting thread. Deep, so a stack overflow is reported in full.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, maxThreads)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 1024.0)
            ARC_REFLECT_ATTR(Tooltip, "Threads walked per crash report; the report says when it stops early.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, dbgengWaitMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1000.0, 300000.0)
            ARC_REFLECT_ATTR(Tooltip, "How long (ms) the debugger engine may take to open a crash dump.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, hangLogTailLines)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Log lines the crash monitor copies beside an abnormal-exit report.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, flushTimeoutMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Budget (ms) for the reporter's own log flush when its deadline expires.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, uiPollMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 10.0, 1000.0)
            ARC_REFLECT_ATTR(Tooltip, "How often (ms) the open reporter window checks whether it was closed while symbolizing.")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, windowWidth)
            ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject) ARC_REFLECT_ATTR(Range, 320.0, 7680.0)
            ARC_REFLECT_ATTR(Tooltip, "Crash reporter window width (px).")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, windowHeight)
            ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject) ARC_REFLECT_ATTR(Range, 240.0, 4320.0)
            ARC_REFLECT_ATTR(Tooltip, "Crash reporter window height (px).")
        ARC_REFLECT_FIELD(DiagnosticsReporterSettings, windowReadyMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 100.0, 60000.0)
            ARC_REFLECT_ATTR(Tooltip, "How long (ms) the reporter waits for its window to open before carrying on without it.")
    ARC_END_REFLECT_TYPE()
}
