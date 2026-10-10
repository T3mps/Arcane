#pragma once

// physics.ground.* and physics.events.*. Cvar strings are unchanged.

#include <Arcane/Config/Settings.hpp>

namespace Arcane::Physics2D
{
    struct GroundSettings
    {
        float minNormalY    = 0.5f;
        float probeDistance = 0.05f;
    };

    ARC_REFLECT_TYPE(GroundSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics.ground", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(GroundSettings, minNormalY)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Smallest contact-normal Y that counts as floor (0.5 = slopes up to 60 degrees are walkable).")
        ARC_REFLECT_FIELD(GroundSettings, probeDistance)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "How far (m) below a resting body the grounded probe looks for floor.")
    ARC_END_REFLECT_TYPE()

    struct EventSettings
    {
        float hitThreshold = 1.0f;
    };

    ARC_REFLECT_TYPE(EventSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics.events", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(EventSettings, hitThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Approach speed (m/s) an impact must exceed to report a hit event (Fixture::hitEvents).")
    ARC_END_REFLECT_TYPE()
}
