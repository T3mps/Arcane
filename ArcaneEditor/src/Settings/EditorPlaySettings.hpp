#pragma once

// editor.play.* and editor.layout.factory.* (settings arc S6-32; inventory
// Part 3 EditorApp.hpp:943, DefaultLayout.hpp:41-47, "Persistence stores"
// [EditorPlayMode][State]).
//
// The Play launch mode is a per-project editor preference (Pref-P), read by
// the transport each frame and taking effect at the next Play (NextWorld).
// It used to live in the layout ini as "[EditorPlayMode][State] Mode=<n>";
// an old section is read once (ReadPlayModeIniLine), imported into the User
// rung where the user has not chosen (ImportLegacyPlayMode) and never
// written again.
//
// The factory layout is the geometry Reset Layout (and a first run) builds
// the default dock layout from -- DefaultLayout.hpp's pure maths, fed by
// Settings<LayoutFactorySettings>() at build time. Per machine, Dev.

#include "App/PlayMode.hpp"   // PlayLaunchMode (reflected there)

#include <optional>

namespace Arcane { class CVarRegistry; }

namespace Arcane::Editor
{
    struct EditorPlaySettings
    {
        PlayLaunchMode launchMode = PlayLaunchMode::Viewport;
    };

    // The default layout's targets at 1920x1080, from the user's own saved
    // layout (USER DECISION 2026-09-30; DefaultLayout.hpp has the full story).
    struct LayoutFactorySettings
    {
        float inspectorWidth       = 380.0f;    // the main Inspector's column (px)
        float outlinerWidth        = 270.0f;    // the Outliner (px)
        float bottomBand           = 350.0f;    // the asset/console band's height (px)
        float centralMinFraction   = 0.40f;     // the central node keeps >= this of each axis
        // The band's browser : Inspector 2 proportion (a ratio, not pixels:
        // ImGui re-divides that split by ratio on every resize).
        float browserRefPx         = 1144.0f;
        float assetsInspectorRefPx = 392.0f;
    };

    // One line of an old [EditorPlayMode][State] section: "Mode=<n>" with n a
    // PlayLaunchMode ordinal. True, and `out` set, when it parses and is in
    // range; a malformed or out-of-range line leaves `out` untouched (exactly
    // the old handler's validation).
    [[nodiscard]] bool ReadPlayModeIniLine(const char* line, std::optional<PlayLaunchMode>& out);

    // Sets editor.play.launchMode at SetBy::User, but only when its history
    // holds no User record (the user already chose: keep it). True when it was
    // applied. The caller publishes and queues the archive write.
    bool ImportLegacyPlayMode(CVarRegistry& reg, PlayLaunchMode mode);

    // The Play menu's mode choice: Set at SetBy::User and queue the debounced
    // archive write. Visible at the next frame's publish.
    void SetPlayLaunchMode(PlayLaunchMode mode);
}
