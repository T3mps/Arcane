#pragma once

// The editor Console panel's and the recents menus' tuning as cvars (settings
// sweep S6-41, inventory Part 3 "Console/Problems" and "Hub/Recents"):
// editor.console.* (the log ring, the drawn cap, the toolbar toggles, the
// command replies shown, the category column) and editor.recents.* (how many
// projects / scenes the menus keep). Per-machine editor preferences.
//
// Plain structs, no settings machinery: ARC_SETTINGS and the reflection live in
// EditorConsoleSettings.cpp. Every default is the pre-sweep literal it replaced.

#include <cstdint>
#include <string_view>

namespace Arcane::Editor
{
    // editor.console.*
    struct EditorConsoleSettings
    {
        std::int32_t ringLines      = 512;    // the log ring's capacity at boot (Restart)
        std::int32_t displayLineCap = 512;    // the ring is resized to this while the editor runs (Live)
        bool         collapse       = false;  // toolbar "Collapse": identical consecutive rows fold into one
        bool         autoScroll     = true;   // toolbar "Scroll": follow new rows while at the bottom
        bool         wrap           = true;   // toolbar "Wrap"
        std::int32_t replyLines     = 6;      // command-line replies shown above the input line
        std::int32_t categoryWidth  = 8;      // the category column's minimum width in characters
    };

    // editor.recents.*
    struct RecentsSettings
    {
        std::int32_t maxProjectsShown = 10;   // File > Open Recent Project entries
        std::int32_t maxScenes        = 10;   // scenes the Open Recent Scene menu remembers
    };

    // The toolbar toggles' cvar names: a click writes the User rung so the
    // choice persists (archived with the other per-machine preferences).
    inline constexpr std::string_view kConsoleCollapseCVar   = "editor.console.collapse";
    inline constexpr std::string_view kConsoleAutoScrollCVar = "editor.console.autoScroll";
    inline constexpr std::string_view kConsoleWrapCVar       = "editor.console.wrap";

    // A toolbar toggle click: the User rung, one name lookup (a click, not a hot path).
    void SetConsoleToggle(std::string_view cvar, bool value);
}
