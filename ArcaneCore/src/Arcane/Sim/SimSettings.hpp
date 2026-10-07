#pragma once

// sim.* and server.tickHz (settings arc S2, completed in S6-8; inventory R1):
// the fixed step, the step cap and the ONE spiral-of-death clamp, which the
// runtime and editor Play share. Deterministic: every field changes a
// simulation's outcome.
// - sim.fixedHz is NextWorld: the Runtime reads it into its loop config when it
//   is built, and PhysicsSystem captures 1/fixedHz then (inventory Part 1 note 4).
// - sim.maxStepsPerFrame and sim.maxFrameDeltaSeconds are Live: both host
//   frames read them each frame (ApplySimStepCap and ClampFrameDelta below),
//   and editor Play's embedded server world takes the same cap each tick
//   (PlaySession::TickServer), so the two worlds never step differently.
//   --fixed-dt runs bypass the clamp.
// - server.tickHz is the dedicated server's tick, separate from sim.fixedHz so
//   a client and a server can differ; ArcaneServer's --fixed-dt overrides it.

// The structs themselves are plain data in SimSettingsData.hpp.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SimSettingsData.hpp>

namespace Arcane
{
    // The spiral-of-death guard both hosts apply to a wall-clock frame delta.
    [[nodiscard]] inline double ClampFrameDelta(double dt) noexcept
    {
        const double cap = Settings<SimSettings>().maxFrameDeltaSeconds;
        return dt > cap ? cap : dt;
    }

    // sim.maxStepsPerFrame is Live: ArcaneRuntime's and editor Play's frames
    // call this before Advance (editor Play on BOTH its loops: the primary and,
    // under EmbeddedServer, the server world's), so a changed cap takes effect
    // next frame.
    inline void ApplySimStepCap(RunLoop& loop) noexcept
    {
        loop.SetMaxStepsPerFrame(Settings<SimSettings>().maxStepsPerFrame);
    }

    ARC_REFLECT_TYPE(SimSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "sim", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(SimSettings, fixedHz)
            ARC_REFLECT_ATTR(Range, 10.0, 480.0) ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Fixed simulation steps per second. Applies when the world is next created.")
        ARC_REFLECT_FIELD(SimSettings, maxStepsPerFrame)
            ARC_REFLECT_ATTR(Range, 1.0, 64.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Most fixed steps one frame may run (the spiral-of-death clamp).")
        ARC_REFLECT_FIELD(SimSettings, maxFrameDeltaSeconds)
            ARC_REFLECT_ATTR(Range, 0.01, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Longest wall-clock frame the simulation catches up on; longer hitches are dropped. Runtime and Play-in-editor alike.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(ServerSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "server", SettingScope::Project, ApplyMode::Restart, Audience::Server)
        ARC_REFLECT_FIELD(ServerSettings, tickHz)
            ARC_REFLECT_ATTR(Range, 1.0, 240.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Dedicated-server fixed ticks per second (--fixed-dt overrides).")
    ARC_END_REFLECT_TYPE()
}
