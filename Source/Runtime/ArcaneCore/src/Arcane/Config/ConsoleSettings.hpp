#pragma once

// console.* window and line caps (settings arc S6-24; inventory Part 2
// "Console"). console.historySize stays the registry's own built-in row
// (CVarRegistry.cpp), registered in every registry.
//
// - windowWidth / windowHeight: the runtime console overlay's first-open size
//   (ImGuiCond_FirstUseEver: its saved ImGui layout wins after that).
// - maxLineChars: the console input line's length cap, latched at its first
//   draw (Restart).

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct ConsoleSettings
    {
        std::uint32_t windowWidth  = 640;
        std::uint32_t windowHeight = 280;
        std::uint32_t maxLineChars = 512;
    };

    ARC_REFLECT_TYPE(ConsoleSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "console", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(ConsoleSettings, windowWidth)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 64.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Width in pixels of the runtime console window the first time it opens.")
        ARC_REFLECT_FIELD(ConsoleSettings, windowHeight)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 64.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Height in pixels of the runtime console window the first time it opens.")
        ARC_REFLECT_FIELD(ConsoleSettings, maxLineChars)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Range, 64.0, 65536.0)
            ARC_REFLECT_ATTR(Tooltip, "Longest line, in characters, the console input accepts.")
    ARC_END_REFLECT_TYPE()
}
