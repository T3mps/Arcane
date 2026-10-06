#pragma once

// The Asset Browser's and the Asset Status panel's tuning as cvars (settings
// sweep S6-38, inventory Part 3 "Asset Browser" + Part 1
// ProjectOpenOptions::mountDiagnostics): editor.assets.* -- the rail width, the
// peek tooltip's named targets, the activity log capacity (Restart), the new
// material's default surface (per-project), the asset-watch and content-
// discovery polls and the diag:// mount (Dev, NextWorld) -- and
// editor.assetStatus.* (the right column's share).
//
// A plain struct, no settings machinery: ARC_SETTINGS and the reflection live
// in AssetBrowserSettings.cpp. Every default is the pre-sweep literal it
// replaced. The panel's other pixel metrics are DERIVED px, not settings.

#include <Arcane/Project/ProjectOpenOptions.hpp>

#include <cstdint>

namespace Arcane::Editor
{
    // editor.assets.*
    struct AssetBrowserSettings
    {
        float        railWidth           = 180.0f; // the kind rail's width, px
        std::int32_t namedTargets        = 3;      // outbound targets the peek tooltip names before "+N"
        std::int32_t activityLogCapacity = 100;    // session activity ring size (Restart)
        // Per-project (PreferencesProject): the Create dialog's surface when
        // nothing pre-picked one -- 0 sprite, 1 mesh, 2 post (Fullscreen).
        std::int32_t newMaterialDefaultSurface = 2;
        double       watchPollSeconds     = 1.0;   // material/texture source mtime poll
        double       discoveryPollSeconds = 2.0;   // Content/ new-source discovery walk
        bool         mountDiagnostics     = true;  // diag:// at project open (Dev, NextWorld)
    };

    // editor.assetStatus.*
    struct AssetStatusSettings
    {
        float rightColumnMaxFraction = 0.45f;   // the right column's cap, as a share of the panel width
    };

    // The editor's project-open options: the host's shared rule
    // (HostBoot::OpenOptionsFor) with editor.assets.mountDiagnostics able only
    // to DECLINE the diag:// mount, never to force one the host declined.
    [[nodiscard]] ProjectOpenOptions EditorOpenOptions(ProjectOpenOptions hostRule);
}
