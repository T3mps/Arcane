#pragma once

// Start page (spec 2026-09-30 s8.4): the pure model behind EditorApp::DrawStartPage,
// the RecentSelection/ConsoleBuffer precedent. It READS the shared recents only.

#include "Project/RecentProjects.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    struct StartPageRow   { std::string name, path, opened; };
    struct StartPageModel { std::vector<StartPageRow> rows; std::string hiddenLine; };

    // "" for 0; "opened just now" (future or < 60 s); "opened N minute(s) ago";
    // "opened N hour(s) ago"; "opened yesterday" (< 48 h); "opened N days ago"
    // (< 30 days); "opened on YYYY-MM-DD" (local date) after that (9.28 #39).
    [[nodiscard]] std::string RelativeOpened(std::uint64_t lastOpenedUnix, std::uint64_t nowUnix);
    // The menu's Select already filtered it: other ABIs counted, current and missing dropped, capped at 10.
    [[nodiscard]] StartPageModel BuildStartPage(const RecentSelection& selection, std::uint64_t nowUnix);
    [[nodiscard]] std::string DialogStartDir(const RecentSelection& selection);   // parent of the first row's project dir, "" if none

    // The page's focus edge: once per appearance, spent on the first frame a
    // focus can land -- the page DOCKED into the Viewport's node and no popup
    // (the boot error modals) holding focus. On a bare launch's first frame
    // the dock id is not known yet, and a focus spent on that floating frame,
    // or under a modal, left the Viewport tab in front (T6-GATE capture 14b).
    struct StartPageFocus { bool wasVisible = false; bool pending = false; };
    struct StartPageStep  { bool appearing = false; bool focusNow = false; };
    [[nodiscard]] StartPageStep StepStartPageFocus(StartPageFocus& state, bool visible, bool canFocus);
}
