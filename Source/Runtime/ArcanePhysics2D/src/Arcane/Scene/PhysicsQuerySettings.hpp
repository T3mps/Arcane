#pragma once

// physics.ground.* and physics.events.*. Cvar strings are unchanged.

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    struct PhysicsGroundSettings2D
    {
        float minNormalY    = 0.5f;
        float probeDistance = 0.05f;
    };

    ARC_REFLECT_TYPE(PhysicsGroundSettings2D)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics.ground", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(PhysicsGroundSettings2D, minNormalY)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Smallest contact-normal Y that counts as floor (0.5 = slopes up to 60 degrees are walkable).")
        ARC_REFLECT_FIELD(PhysicsGroundSettings2D, probeDistance)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "How far (m) below a resting body the grounded probe looks for floor.")
    ARC_END_REFLECT_TYPE()

    struct PhysicsEventSettings2D
    {
        float hitThreshold = 1.0f;
    };

    ARC_REFLECT_TYPE(PhysicsEventSettings2D)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics.events", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(PhysicsEventSettings2D, hitThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Approach speed (m/s) an impact must exceed to report a hit event (Fixture2D::hitEvents).")
    ARC_END_REFLECT_TYPE()
}
