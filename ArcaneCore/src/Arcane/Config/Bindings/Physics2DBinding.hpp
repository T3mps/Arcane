#pragma once

// physics (2D) settings: the project WorldDef fields, bound in
// Runtime::EnsurePhysics. Cvar strings stay "physics" and "physics.*".
// ToWorldDef lives in Physics2DDetail.hpp (it names the solver).

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane::Physics2D
{
    enum class Broadphase : std::uint8_t { Tree = 0, Hash = 1, Sap = 2 };

    ARC_REFLECT_ENUM(Broadphase)
        ARC_REFLECT_ENUM_VALUE(Broadphase, Tree)
        ARC_REFLECT_ENUM_VALUE(Broadphase, Hash)
        ARC_REFLECT_ENUM_VALUE(Broadphase, Sap)
    ARC_END_REFLECT_ENUM()

    struct WorldSettings
    {
        CVarVec2  gravity{0.0f, -9.81f};
        Broadphase broadphase             = Broadphase::Tree;
        float     hashCellSize           = 1.0f;
        std::uint32_t substepCount           = 4u;
        float     contactHertz           = 30.0f;
        float     contactDampingRatio    = 10.0f;
        float     restitutionThreshold   = 1.0f;
        float     contactPushMaxVelocity = 3.0f;
        float     maxLinearVelocity      = 400.0f;
        float     sleepThreshold         = 0.05f;
        bool      parallelSolver         = false;
    };

    ARC_REFLECT_TYPE(WorldSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
        ARC_REFLECT_FIELD(WorldSettings, gravity)
            ARC_REFLECT_ATTR(Range, -1000.0, 1000.0)
            ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Project gravity in metres per second squared (+Y is up); a scene-root SceneSettings component overrides it.")
        ARC_REFLECT_FIELD(WorldSettings, broadphase)
            ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "2D broadphase: dynamic tree, spatial hash or sweep-and-prune.")
        ARC_REFLECT_FIELD(WorldSettings, hashCellSize)
            ARC_REFLECT_ATTR(Range, 0.05, 100.0) ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Spatial-hash cell size in metres (Hash broadphase only).")
        ARC_REFLECT_FIELD(WorldSettings, substepCount)
            ARC_REFLECT_ATTR(Range, 1.0, 16.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Solver sub-steps per fixed step.")
        ARC_REFLECT_FIELD(WorldSettings, contactHertz)
            ARC_REFLECT_ATTR(Range, 1.0, 240.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Soft-contact stiffness in Hz.")
        ARC_REFLECT_FIELD(WorldSettings, contactDampingRatio)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Soft-contact damping ratio.")
        ARC_REFLECT_FIELD(WorldSettings, restitutionThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Approach speed (m/s) below which bounces are suppressed.")
        ARC_REFLECT_FIELD(WorldSettings, contactPushMaxVelocity)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Clamp (m/s) on the penetration push-out speed.")
        ARC_REFLECT_FIELD(WorldSettings, maxLinearVelocity)
            ARC_REFLECT_ATTR(Range, 1.0, 1e5) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Hard cap on body speed (m/s).")
        ARC_REFLECT_FIELD(WorldSettings, sleepThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Speed (m/s) under which a body may sleep.")
        ARC_REFLECT_FIELD(WorldSettings, parallelSolver)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Solve on the job system. Off = the serial solver. Results are thread-count invariant.")
    ARC_END_REFLECT_TYPE()
}
