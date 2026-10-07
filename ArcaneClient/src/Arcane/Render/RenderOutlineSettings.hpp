#pragma once

// render.outline.* (settings arc S6-21; inventory Part 2 "Pick and selection
// outline"): the selection outline's widths, its jump-flood ceiling and the id
// pass's supersample. Game audience, Dev: ArcaneClient's OutlineNode cannot
// declare an Editor-audience cvar (reconciled R1). The COLOURS are not here:
// they are DERIVED from editor.theme.amber and the graph hover border, and the
// editor pushes them through OutlineNode::SetColors every frame (ruling I5).
//
// maxThicknessPx and supersample are Restart: the render graph latches them
// once (OutlineMaxThicknessPx / PickSupersample), because the JFA schedule and
// the id target's extent are fixed for the graph's life.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct RenderOutlineSettings
    {
        float selectWidthPx = 3.0f;
        float hoverWidthPx = 3.0f;
        float edgeSoftnessPx = 1.0f;
        std::uint32_t maxThicknessPx = 32;
        std::uint32_t supersample = 2;
    };

    ARC_REFLECT_TYPE(RenderOutlineSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.outline", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderOutlineSettings, selectWidthPx)
            ARC_REFLECT_ATTR(DisplayName, "Selected width") ARC_REFLECT_ATTR(Range, 1.0, 256.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (px) of the selection outline, centred on the silhouette edge. Capped at the outline ceiling.")
        ARC_REFLECT_FIELD(RenderOutlineSettings, hoverWidthPx)
            ARC_REFLECT_ATTR(DisplayName, "Hovered width") ARC_REFLECT_ATTR(Range, 1.0, 32.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (px) of the hover outline. Capped at the outline ceiling.")
        ARC_REFLECT_FIELD(RenderOutlineSettings, edgeSoftnessPx)
            ARC_REFLECT_ATTR(DisplayName, "Edge softness") ARC_REFLECT_ATTR(Range, 0.0, 4.0)
            ARC_REFLECT_ATTR(Tooltip, "Width (px) of the outline's anti-aliasing ramp.")
        ARC_REFLECT_FIELD(RenderOutlineSettings, maxThicknessPx)
            ARC_REFLECT_ATTR(DisplayName, "Outline ceiling") ARC_REFLECT_ATTR(Range, 1.0, 256.0)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Largest outline width (px) the jump-flood field supports; sets its step count. Keep it at or above both widths.")
        ARC_REFLECT_FIELD(RenderOutlineSettings, supersample)
            ARC_REFLECT_ATTR(DisplayName, "Pick supersample") ARC_REFLECT_ATTR(Range, 1.0, 4.0)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Supersample factor of the entity-id pass. Visible in pixels: it moves the outline's sub-pixel edge.")
    ARC_END_REFLECT_TYPE()

    // The Restart pair, latched on first use (the render graph's node
    // creation), clamped to the registered ranges.
    [[nodiscard]] ARC_API std::uint32_t OutlineMaxThicknessPx() noexcept;
    [[nodiscard]] ARC_API std::uint32_t PickSupersample() noexcept;
}
