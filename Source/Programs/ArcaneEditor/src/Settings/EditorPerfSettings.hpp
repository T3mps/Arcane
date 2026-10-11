#pragma once

// editor.perf.* (settings arc S6-24; reconciliation R1). backgroundFps holds
// an unfocused editor to that many frames per second; 0 (the default) never
// throttles, which is the editor's behaviour before the setting existed. It is
// kept apart from app.window.minimizedSleepMs (the minimized sleep) on purpose.

#include <Arcane/Config/Settings.hpp>

#include <chrono>
#include <cstdint>

namespace Arcane::Editor
{
    struct EditorPerfSettings
    {
        std::uint32_t backgroundFps = 0;
    };

    ARC_REFLECT_TYPE(EditorPerfSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.perf", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorPerfSettings, backgroundFps)
            ARC_REFLECT_ATTR(Range, 0.0, 240.0)
            ARC_REFLECT_ATTR(Tooltip, "Frame-rate cap while the editor window is not focused, to save power and GPU time. "
                                      "0 never throttles.")
    ARC_END_REFLECT_TYPE()

    // The wait that holds an unfocused frame to backgroundFps: what is left of
    // the 1000 / backgroundFps ms frame budget after `elapsed` (the time since
    // the previous frame's pump). 0 fps never waits.
    inline std::chrono::milliseconds BackgroundFrameWait(std::uint32_t backgroundFps, std::chrono::milliseconds elapsed)
    {
        if (backgroundFps == 0)
            return std::chrono::milliseconds(0);
        const std::chrono::milliseconds budget(1000 / backgroundFps);
        return elapsed >= budget ? std::chrono::milliseconds(0) : budget - elapsed;
    }
}
