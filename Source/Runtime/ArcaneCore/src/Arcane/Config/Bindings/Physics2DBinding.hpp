#pragma once

// physics (2D) settings: the project WorldDef fields, bound in
// Runtime::EnsurePhysics. Cvar strings stay "physics" and "physics.*".
// ToWorldDef lives in Physics2DDetail.hpp (it names the solver).

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    enum class PhysicsBroadphase2D : std::uint8_t { Tree = 0, Hash = 1, Sap = 2 };

    ARC_REFLECT_ENUM(PhysicsBroadphase2D)
        ARC_REFLECT_ENUM_VALUE(PhysicsBroadphase2D, Tree)
        ARC_REFLECT_ENUM_VALUE(PhysicsBroadphase2D, Hash)
        ARC_REFLECT_ENUM_VALUE(PhysicsBroadphase2D, Sap)
    ARC_END_REFLECT_ENUM()

    struct PhysicsWorldSettings2D
    {
        CVarVec2  gravity{0.0f, -9.81f};
        PhysicsBroadphase2D broadphase             = PhysicsBroadphase2D::Tree;
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

    ARC_REFLECT_TYPE(PhysicsWorldSettings2D)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, gravity)
            ARC_REFLECT_ATTR(Range, -1000.0, 1000.0)
            ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Project gravity in metres per second squared (+Y is up); a scene-root PhysicsSettings2D component overrides it.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, broadphase)
            ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "2D broadphase: dynamic tree, spatial hash or sweep-and-prune.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, hashCellSize)
            ARC_REFLECT_ATTR(Range, 0.05, 100.0) ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Spatial-hash cell size in metres (Hash broadphase only).")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, substepCount)
            ARC_REFLECT_ATTR(Range, 1.0, 16.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Solver sub-steps per fixed step.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, contactHertz)
            ARC_REFLECT_ATTR(Range, 1.0, 240.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Soft-contact stiffness in Hz.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, contactDampingRatio)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Soft-contact damping ratio.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, restitutionThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Approach speed (m/s) below which bounces are suppressed.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, contactPushMaxVelocity)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Clamp (m/s) on the penetration push-out speed.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, maxLinearVelocity)
            ARC_REFLECT_ATTR(Range, 1.0, 1e5) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Hard cap on body speed (m/s).")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, sleepThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Speed (m/s) under which a body may sleep.")
        ARC_REFLECT_FIELD(PhysicsWorldSettings2D, parallelSolver)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Solve on the job system. Off = the serial solver. Results are thread-count invariant.")
    ARC_END_REFLECT_TYPE()
}
