#pragma once

// The render look (settings arc S6-19; inventory Part 2 "Batch2D", "Mesh
// pass", "Visibility"). The rest of it -- the canvas clear colour, texture
// anisotropy and the canvas and depth formats -- are RenderSettings fields
// (RenderDeviceSettings.hpp).
//
// - render.sprite.*             RenderSpriteSettings: the sprite sampler's
//                               filter, read when Batch2DNode creates it
//                               (Restart).
// - render.mesh.defaultLight.*  RenderMeshDefaultLightSettings: the one
//                               directional light and the ambient term a
//                               scene view gets until a light component
//                               exists, each a colour plus an intensity. The runtime and the editor's scene
//                               view read it every frame (Live); a
//                               MeshSceneDesc built in code defaults to it.
// - render.cull.*               RenderCullSettings: how far the CPU coarse
//                               cull widens the frustum (Live, read per
//                               pass by BuildVisibleSet and the GPU scene).

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // RenderSettings (the look's device-level fields)

#include <cstdint>

namespace Arcane
{
    enum class SamplerFilter : std::uint8_t { Linear = 0, Point = 1 };

    ARC_REFLECT_ENUM(SamplerFilter)
        ARC_REFLECT_ENUM_VALUE(SamplerFilter, Linear)
        ARC_REFLECT_ENUM_VALUE(SamplerFilter, Point)
    ARC_END_REFLECT_ENUM()

    struct RenderSpriteSettings
    {
        SamplerFilter filter = SamplerFilter::Linear;
    };

    ARC_REFLECT_TYPE(RenderSpriteSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.sprite", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(RenderSpriteSettings, filter)
            ARC_REFLECT_ATTR(Tooltip, "How sprites are sampled. Linear scales them smoothly; Point keeps hard texel edges, "
                                      "for pixel art.")
    ARC_END_REFLECT_TYPE()

    // COLOUR + INTENSITY (the UE light-authoring model): a CVarColor is
    // linear but spelled "#RRGGBBAA" in files -- 8-bit sRGB, 0..1 -- so it
    // cannot carry an HDR light on its own. The effective light is
    // colour x intensity (and ambient x ambientIntensity); at intensity 1.0
    // that product is the colour bit for bit, so the defaults shade exactly
    // as the pre-sweep literals did.
    struct RenderMeshDefaultLightSettings
    {
        CVarVec3  direction{ 0.0f, 0.0f, 1.0f };
        CVarColor color{ 1.0f, 1.0f, 1.0f, 1.0f };
        float     intensity = 1.0f;
        CVarColor ambient{ 0.05f, 0.05f, 0.05f, 1.0f };
        float     ambientIntensity = 1.0f;
    };

    ARC_REFLECT_TYPE(RenderMeshDefaultLightSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.mesh.defaultLight", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(RenderMeshDefaultLightSettings, direction)
            ARC_REFLECT_ATTR(Tooltip, "Direction TOWARD the scene's one directional light, in world space; it need not be unit "
                                      "length. A zero vector turns the light off and leaves only the ambient term.")
        ARC_REFLECT_FIELD(RenderMeshDefaultLightSettings, color)
            ARC_REFLECT_ATTR(Tooltip, "Colour of the scene's directional light (linear); its brightness is the intensity below.")
        ARC_REFLECT_FIELD(RenderMeshDefaultLightSettings, intensity)
            ARC_REFLECT_ATTR(Range, 0.0, 16.0)
            ARC_REFLECT_ATTR(Tooltip, "Brightness of the scene's directional light: its colour is multiplied by this. Above 1 "
                                      "is an HDR light; 0 turns it off.")
        ARC_REFLECT_FIELD(RenderMeshDefaultLightSettings, ambient)
            ARC_REFLECT_ATTR(Tooltip, "Colour of the flat ambient light added to every lit mesh surface (linear); the whole of "
                                      "the indirect lighting until a light component exists.")
        ARC_REFLECT_FIELD(RenderMeshDefaultLightSettings, ambientIntensity)
            ARC_REFLECT_ATTR(Range, 0.0, 16.0)
            ARC_REFLECT_ATTR(Tooltip, "Brightness of the ambient light: its colour is multiplied by this.")
    ARC_END_REFLECT_TYPE()

    struct RenderCullSettings
    {
        float frustumSlackMeters = 0.25f;
    };

    ARC_REFLECT_TYPE(RenderCullSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.cull", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderCullSettings, frustumSlackMeters)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Metres the view frustum is widened by before culling, so a fast sprite drawn between "
                                      "physics steps is not culled at the screen edge. Larger is more conservative.")
    ARC_END_REFLECT_TYPE()
}
