#pragma once

// The diagnostics.* settings struct as plain data (settings arc S6-2): no
// reflection, no registry, std only. Diagnostics.hpp takes its Config defaults
// from it, and ArcaneCrashReporter includes Diagnostics.hpp without the Astra
// include path, so the reflection block and Settings<T>() live one header up,
// in DiagnosticsSettings.hpp. Include THAT one to read or register settings.
//
// The member initializers are the defaults (inventory Part 1 "Base /
// Diagnostics / Log"); SweepDiagnosticsTest pins them.

#include <cstdint>
#include <string>

namespace Arcane
{
    enum class MinidumpKind : std::uint8_t { Small = 0, Default = 1, Full = 2 };

    struct DiagnosticsSettings
    {
        bool          drawMarkers = false;              // ArcaneClient's per-draw GPU markers (S2)
        std::string   dumpDir;                          // "" = <exe dir>/diagnostics
        std::uint32_t hangSeconds = 12;
        std::uint32_t gpuStallSeconds = 8;
        bool          installCrashHandler = true;
        bool          hangWatchdog = true;
        std::string   reporterPath;                     // "" = <exe dir>/ArcaneCrashReporter.exe
        bool          spawnReporter = true;
        std::uint32_t exitSeconds = 30;                 // 0 disables the exit sentinel
        std::uint32_t crashHandlingTimeoutSeconds = 60;
        MinidumpKind  minidumpKind = MinidumpKind::Default;
        std::uint32_t logFlushTimeoutMs = 2000;
        std::uint32_t watchdogPollMs = 250;
        std::uint32_t watchdogJoinTimeoutMs = 5000;
        std::uint32_t minFatalWaitMs = 5000;
        bool          perfLog = false;
        std::uint32_t perfLogIntervalFrames = 60;
        std::uint32_t logTailLines = 200;               // the log excerpt in a crash report: reporter + editor (S6-4)
    };

    namespace Detail
    {
        // The defaults as one object, so a default elsewhere reads
        // `DiagnosticsDefaults().field` instead of repeating a literal. A
        // function rather than `DiagnosticsSettings{}` in a default member
        // initializer: MSVC (v18) ICEs constant-evaluating those std::string
        // temporaries for a `const Diagnostics::Config c{};`.
        inline const DiagnosticsSettings& DiagnosticsDefaults() noexcept
        {
            static const DiagnosticsSettings defaults{};
            return defaults;
        }
    }
}
