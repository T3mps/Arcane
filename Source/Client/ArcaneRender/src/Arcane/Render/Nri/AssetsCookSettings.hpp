#pragma once

// assets.cook.* (settings arc S6-5; inventory Part 2 "Texture and mesh
// caches", row NriTextureCache.hpp:302). The texture cache's PendingCook
// re-poll cadence: a still-cooking texture re-asks its artifact supply on
// every pendingRepollInterval-th Resolve (NriTextureCache::Resolve reads it
// live). Game Dev: compiled out of Dist, where the default applies.
//
// Why throttle at all: Resolve runs at DECLARATION TIME, every frame, per
// on-screen span, so polling on EVERY ask meant every still-uncooked texture
// drove a fresh Assets::ArtifactFor call -- and, downstream, a scan of
// Intermediate/Artifacts/** -- every single frame. 32 sits in the middle of
// the original ~16-64 range: at 60 Hz a re-poll roughly twice a second per
// pending texture, so a finished cook is promoted to Resident within about
// half a second, at 1/32 of the per-frame scan cost.

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct AssetsCookSettings
    {
        std::uint32_t pendingRepollInterval = 32;
    };

    ARC_REFLECT_TYPE(AssetsCookSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "assets.cook", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(AssetsCookSettings, pendingRepollInterval)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 1024.0)
            ARC_REFLECT_ATTR(Tooltip, "A texture still being cooked re-checks for its cooked artifact every this many "
                                      "lookups (about twice a second at 60 fps for 32). Lower = promoted sooner, more disk scans.")
    ARC_END_REFLECT_TYPE()
}
