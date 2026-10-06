#pragma once

// assets.cache.* and assets.material.* (settings arc S6-5; inventory Part 1
// "Assets"). The Core asset facade's tunables:
// - assets.cache.byteBudget is Restart: a Runtime reads it once, when it
//   creates its Assets facade (Runtime.cpp), so it is in place after
//   HostBoot::ApplyEarlyConfigRungs. AssetsDesc's own default is this
//   struct's.
// - assets.material.maxParentDepth is Live: MaterialSurfaceFor reads it per
//   walk. Its inventory row is "Editor Dev"; declared in Core, it is Game +
//   Dev (compiled out of Dist), as the plan's S6 Open questions record.
// assets.sprite.defaultPixelsPerUnit is an Editor setting and lives in the
// editor (ArcaneEditor/src/Settings/AssetsSpriteSettings.hpp);
// assets.cook.pendingRepollInterval is the texture cache's, in ArcaneClient
// (Render/Nri/AssetsCookSettings.hpp).

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct AssetsCacheSettings
    {
        std::uint64_t byteBudget = 256ull * 1024 * 1024;
    };

    struct AssetsMaterialSettings
    {
        std::int32_t maxParentDepth = 8;
    };

    ARC_REFLECT_TYPE(AssetsCacheSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "assets.cache", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(AssetsCacheSettings, byteBudget)
            ARC_REFLECT_ATTR(Range, 0.0, 17179869184.0)
            ARC_REFLECT_ATTR(Tooltip, "Byte budget across every asset cache (textures, files, JSON, decoded pixels); "
                                      "least-recently-used entries are evicted past it. 0 = unbounded.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(AssetsMaterialSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "assets.material", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(AssetsMaterialSettings, maxParentDepth)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 64.0)
            ARC_REFLECT_ATTR(Tooltip, "How many material files a parent-chain walk reads before it gives up "
                                      "(the cycle guard). Raise it for deeper instance chains.")
    ARC_END_REFLECT_TYPE()
}
