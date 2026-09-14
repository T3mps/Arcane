#pragma once

// Shared exit codes (spec s3 / ruling R4). Both target kinds (--project today,
// --engine later) use the same table: 0 ok, 2 driver refusal, 3 probe-would-
// rebuild, anything else a child's own status.

#include <optional>

namespace arcbuild
{
    constexpr int kExitOk           = 0;
    constexpr int kExitRefused      = 2;   // no SDK / no project / bad flags (s3)
    constexpr int kExitProbeRebuild = 3;   // `probe`: the slot row would force /t:Rebuild (ruling R4)

    // A child's own status passes through (premake/msbuild exit 1 on failure,
    // cmd 9009 when the exe is not found); a pipe that could not open is a
    // driver refusal.
    [[nodiscard]] inline int ExitFromChild(std::optional<int> childExit)
    {
        return childExit ? *childExit : kExitRefused;
    }
}
