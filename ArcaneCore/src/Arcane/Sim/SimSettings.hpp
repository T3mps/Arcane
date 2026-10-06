#pragma once

// sim.* (settings arc S2; inventory R1): the fixed step and the ONE
// spiral-of-death clamp, which the runtime and editor Play share.
// - sim.fixedHz is NextWorld: the Runtime reads it into its loop config when it
//   is built, and PhysicsSystem captures 1/fixedHz then. server.tickHz is a
//   separate Server setting (S6).
// - sim.maxFrameDeltaSeconds is Live: both host frames read it each frame.
//   --fixed-dt runs bypass it.

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    struct SimSettings
    {
        double fixedHz              = 60.0;
        double maxFrameDeltaSeconds = 0.25;
    };

    ARC_REFLECT_TYPE(SimSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "sim", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
        ARC_REFLECT_FIELD(SimSettings, fixedHz)
            ARC_REFLECT_ATTR(Range, 10.0, 480.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Fixed simulation rate in Hz.")
        ARC_REFLECT_FIELD(SimSettings, maxFrameDeltaSeconds)
            ARC_REFLECT_ATTR(Range, 0.01, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live)
            ARC_REFLECT_ATTR(Tooltip, "Longest wall-clock frame (s) the simulation catches up on after a stall.")
    ARC_END_REFLECT_TYPE()
}
