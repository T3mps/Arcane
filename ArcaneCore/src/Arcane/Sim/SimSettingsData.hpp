#pragma once

// The sim.* and server.* settings structs as plain data (settings arc S6-8):
// no reflection, no registry, std only. RunLoop.hpp, HostConfig.hpp and
// OffscreenImGuiLayer.hpp take their defaults from it without pulling the
// registry (and, through the logger, <windows.h>) into headers that NRI's
// include-order rule covers. The reflection block, Settings<T>() readers and
// ClampFrameDelta / ApplySimStepCap live one header up, in SimSettings.hpp.
// Include THAT one to read or register settings.

#include <cstdint>

namespace Arcane
{
    struct SimSettings
    {
        double       fixedHz              = 60.0;
        std::int32_t maxStepsPerFrame     = 5;
        double       maxFrameDeltaSeconds = 0.25;
    };

    struct ServerSettings
    {
        double tickHz = 60.0;
    };
}
