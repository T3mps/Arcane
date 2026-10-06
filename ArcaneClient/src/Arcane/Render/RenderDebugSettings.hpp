#pragma once

// render.* debug switches (settings arc S2). The category is "render", so the
// cvar keeps its name (render.meshCull). S6's RenderSettings sweep folds or
// keeps this struct; the name does not move.

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    struct RenderDebugSettings
    {
        bool meshCull = true;
    };

    ARC_REFLECT_TYPE(RenderDebugSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(RenderDebugSettings, meshCull)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Frustum-cull mesh instances on the GPU.")
    ARC_END_REFLECT_TYPE()
}
