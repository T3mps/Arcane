#pragma once

// The ui.* settings struct as plain data (settings arc S6-4; inventory R1).
// UI feedback shared by every Arcane surface that has one: the editor's
// panels and documents, and ArcaneCrashReporter, which has no registry and
// gets the published value on its command line
// (Diagnostics::ReporterSettingsArgs). std only, so the reporter (no Astra
// include path) can take its default from here; the reflection block and
// Settings<T>() live in UiSettings.hpp.

namespace Arcane
{
    struct UiSettings
    {
        double copyFlashSeconds = 0.75;   // how long a Copy button reads "Copied" / "Copy failed"
    };
}
