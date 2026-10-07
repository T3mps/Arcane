#pragma once

// physics.ground.* (settings arc S6-9; inventory Part 1 "Scene / Physics"):
// what PhysicsResource::Motion counts as standing on the floor.
// - Live: Physics2D.cpp reads Settings<PhysicsGroundSettings>() once per query.
// - Deterministic: both change a grounded verdict, so they change gameplay,
//   replays and the [physics2d] grounded fixtures.
// - The defaults are the pre-sweep literals (Phys::Real(0.5), Phys::Real(0.05));
//   SweepPhysicsQueryTest pins them bit for bit.

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    struct PhysicsGroundSettings
    {
        float minNormalY    = 0.5f;    // contact normal.y at or above which a contact supports (0.5 = a 60 degree slope)
        float probeDistance = 0.05f;   // metres the sleeping-body probe reaches below the feet
    };

    ARC_REFLECT_TYPE(PhysicsGroundSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics.ground", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(PhysicsGroundSettings, minNormalY)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Smallest contact-normal Y that counts as floor (0.5 = slopes up to 60 degrees are walkable).")
        ARC_REFLECT_FIELD(PhysicsGroundSettings, probeDistance)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "How far (m) below a resting body the grounded probe looks for floor.")
    ARC_END_REFLECT_TYPE()
}
