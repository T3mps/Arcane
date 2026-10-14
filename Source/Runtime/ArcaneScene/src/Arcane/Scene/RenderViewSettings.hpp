#pragma once

// render.ortho2D.* (settings arc S6-9; inventory Part 1 "Scene / Physics",
// ViewTransform.hpp:130): the depth slab every 2D (orthographic) view keeps.
// - Live: Ortho2DView reads Settings<RenderOrtho2DSettings>() each time a 2D
//   view is built (the scene camera and the editor's 2D camera, every frame).
// - Not Deterministic: it changes what is drawn, not what is simulated.
// - The default is the old Orthographic(nearZ = -1000, farZ = 1000) pair;
//   ViewTransform::Orthographic takes the depth explicitly now, so this is the
//   one place it comes from.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    struct RenderOrtho2DSettings
    {
        float depthRange = 1000.0f;   // metres either side of Z = 0 a 2D view keeps (near = -range, far = +range)
    };

    ARC_REFLECT_TYPE(RenderOrtho2DSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.ortho2D", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(RenderOrtho2DSettings, depthRange)
            ARC_REFLECT_ATTR(Range, 1.0, 1e6) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Depth (m) either side of Z = 0 that 2D views keep; sprites beyond it are clipped.")
    ARC_END_REFLECT_TYPE()

    // The 2D view of `halfHeight` metres around `center`, with the depth slab
    // render.ortho2D.depthRange sets. Every 2D camera builds its view here.
    [[nodiscard]] inline ViewTransform Ortho2DView(glm::vec2 center, float halfHeight, glm::uvec2 viewport) noexcept
    {
        const float d = Settings<RenderOrtho2DSettings>().depthRange;
        return ViewTransform::Orthographic(center, halfHeight, viewport, -d, d);
    }
}
