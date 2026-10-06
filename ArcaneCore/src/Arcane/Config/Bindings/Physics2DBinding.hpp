#pragma once

// physics (2D) (settings arc S2, spec s5.2): Manifold2D's WorldDef, bound in
// Runtime::EnsurePhysics.
// - Every field is NextWorld: Manifold2D has no setters, and a gravity change
//   already replaces the world.
// - Every field except parallelSolver is Deterministic: changing one changes
//   replays, the [trajectory] fixture and goldens.
// - Gravity is DERIVED (Runtime::ResolvedGravity: the .arcproj block, then the
//   scene's PhysicsSettings component; R3 moves the former to
//   Config/physics.json in S6), so ToWorldDef leaves it alone.
// - The literals are WorldDef's defaults; SettingsBindingsTest pins equality.

#include <Arcane/Config/Settings.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <cstdint>

namespace Arcane
{
    // Arcane's own spelling of Manifold2D's BroadphaseKind: reflection needs an
    // unqualified enum here, and vendored code never includes Arcane headers.
    enum class Physics2DBroadphase : std::uint8_t { Tree = 0, Hash = 1, Sap = 2 };

    ARC_REFLECT_ENUM(Physics2DBroadphase)
        ARC_REFLECT_ENUM_VALUE(Physics2DBroadphase, Tree)
        ARC_REFLECT_ENUM_VALUE(Physics2DBroadphase, Hash)
        ARC_REFLECT_ENUM_VALUE(Physics2DBroadphase, Sap)
    ARC_END_REFLECT_ENUM()

    struct Physics2DWorldSettings
    {
        Physics2DBroadphase broadphase             = Physics2DBroadphase::Tree;
        float               hashCellSize           = 1.0f;
        std::uint32_t       substepCount           = 4u;
        float               contactHertz           = 30.0f;
        float               contactDampingRatio    = 10.0f;
        float               restitutionThreshold   = 1.0f;
        float               contactPushMaxVelocity = 3.0f;
        float               maxLinearVelocity      = 400.0f;
        float               sleepThreshold         = 0.05f;
        bool                parallelSolver         = false;
    };

    ARC_REFLECT_TYPE(Physics2DWorldSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics", SettingScope::Project, ApplyMode::NextWorld, Audience::Game)
        ARC_REFLECT_FIELD(Physics2DWorldSettings, broadphase)
            ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "2D broadphase: dynamic tree, spatial hash or sweep-and-prune.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, hashCellSize)
            ARC_REFLECT_ATTR(Range, 0.05, 100.0) ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Spatial-hash cell size in metres (Hash broadphase only).")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, substepCount)
            ARC_REFLECT_ATTR(Range, 1.0, 16.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Solver sub-steps per fixed step.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, contactHertz)
            ARC_REFLECT_ATTR(Range, 1.0, 240.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Soft-contact stiffness in Hz.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, contactDampingRatio)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Soft-contact damping ratio.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, restitutionThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Approach speed (m/s) below which bounces are suppressed.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, contactPushMaxVelocity)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Clamp (m/s) on the penetration push-out speed.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, maxLinearVelocity)
            ARC_REFLECT_ATTR(Range, 1.0, 1e5) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Hard cap on body speed (m/s).")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, sleepThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Speed (m/s) under which a body may sleep.")
        ARC_REFLECT_FIELD(Physics2DWorldSettings, parallelSolver)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Solve on the job system. Off = the serial solver. Results are thread-count invariant.")
    ARC_END_REFLECT_TYPE()

    inline Manifold2D::Physics::BroadphaseKind ToBroadphaseKind(Physics2DBroadphase b) noexcept
    {
        switch (b)
        {
        case Physics2DBroadphase::Tree: return Manifold2D::Physics::BroadphaseKind::Tree;
        case Physics2DBroadphase::Hash: return Manifold2D::Physics::BroadphaseKind::Hash;
        case Physics2DBroadphase::Sap:  return Manifold2D::Physics::BroadphaseKind::Sap;
        }
        return Manifold2D::Physics::BroadphaseKind::Tree;
    }

    // Pure. parallelSolver is not a WorldDef field: EnsurePhysics applies it (SetExecutor).
    inline Manifold2D::Physics::WorldDef ToWorldDef(const Physics2DWorldSettings& s)
    {
        Manifold2D::Physics::WorldDef wd;
        wd.broadphase             = ToBroadphaseKind(s.broadphase);
        wd.hashCellSize           = s.hashCellSize;
        wd.substepCount           = s.substepCount;
        wd.contactHertz           = s.contactHertz;
        wd.contactDampingRatio    = s.contactDampingRatio;
        wd.restitutionThreshold   = s.restitutionThreshold;
        wd.contactPushMaxVelocity = s.contactPushMaxVelocity;
        wd.maxLinearVelocity      = s.maxLinearVelocity;
        wd.sleepThreshold         = s.sleepThreshold;
        return wd;
    }
}
