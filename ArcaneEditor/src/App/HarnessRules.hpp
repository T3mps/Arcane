#pragma once

// Harness rules (node-page phase s8.2, drafting pick 9.28): machine-dependent
// UI -- the Problems/Console tab badges and the strip chip -- is suppressed
// under the verify/screenshot harness, so this desk's foreign-module warnings
// never reach a golden. A superset of EditorAppFrame.cpp's CaptureWanted.

#include <Arcane/Host/HostConfig.hpp>

namespace Arcane::Editor
{
    [[nodiscard]] inline bool UnderVerifyHarness(const Arcane::HostConfig& c) noexcept
    {
        return c.headless || !c.screenshotPath.empty() || !c.reportPath.empty();
    }
}
