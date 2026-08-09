#pragma once

// File -> Open Recent: the editor's view of the Arcane Hub's recent-projects list.
//
// STORAGE IS SHARED WITH THE HUB, deliberately. The Hub already records every
// project it launches in %LOCALAPPDATA%\Arcane\hub\recents.archub, and a second
// editor-owned list would drift from it the moment you opened a project in the
// other app. So this reads and writes the Hub's file directly.
//
// Everything here is a MIRROR of Hub behaviour defined in
// Arcane/Hub/src-tauri/src/{state,store,paths}.rs -- change together:
//   - the file lives at %LOCALAPPDATA%\Arcane\hub\recents.archub  (paths.rs)
//   - it is JSON inside a {version, items} envelope, format version 1 (store.rs)
//   - `items` is newest-first BY POSITION, not by timestamp (state.rs touch_recent)
//   - `lastOpenedUtc` is UNIX SECONDS as a decimal string despite the name
//     (launch.rs now_utc_iso -- a misnomer; it is not ISO-8601)
//   - identity for dedup is ProjectDirKey, which collapses "<dir>/X.arcproj"
//     and "<dir>" to the same key (state.rs project_dir_key)
//
// Three rules that keep the editor a good citizen of a file it does not own:
//   1. A file whose version is NEWER than we understand is read as empty and
//      never written -- exactly the Hub's own "written by a newer Hub" rule.
//   2. A file that exists but does not parse is read as empty and NEVER written.
//      Quarantine is the Hub's recovery policy; clobbering would destroy the
//      evidence it needs.
//   3. A touch mutates GENERIC json, so every field the editor does not model --
//      engineId, args, favorite, guid, and anything a future Hub adds -- rides
//      through untouched. A typed round-trip would silently drop them.
//
// The parsing/selection half is pure and headless (no ImGui, no engine) so the
// [editor] tests drive it directly, same pattern as ConsoleBuffer/EntityList.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // The display subset of the Hub's RecentProject. Everything else in the
    // on-disk entry is preserved on write but never modelled here.
    struct RecentProject
    {
        std::string   path;             // the Hub's ORIGINAL spelling (display + open)
        std::string   name;
        std::uint32_t engineAbi = 0;
    };

    struct RecentSelection
    {
        std::vector<RecentProject> visible;
        // How many entries were dropped ONLY because they target another engine
        // ABI. Surfaced as a disabled menu line so the list is never silently
        // short -- the Hub shows every project across every engine version, and
        // without this you cannot tell "wrong ABI" from "recents is broken".
        // Missing / currently-open exclusions are NOT counted: those are
        // expected, and Unreal hides both without comment
        // (FRecentProjectsMenu::MakeMenu).
        std::size_t hiddenForAbi = 0;
    };

    namespace Recents
    {
        // How many entries the menu shows. Unreal bounds its own list the same
        // way (a fixed pool of SwitchProjectCommands).
        inline constexpr std::size_t kMaxShown = 10;

        // --- Hub parity helpers (mirror state.rs) ---------------------------

        // Compare-only: '\' -> '/', drop trailing '/', lowercase. Windows paths
        // are case-insensitive and take either separator, so one project reached
        // two ways must collapse to one entry.
        [[nodiscard]] std::string NormalisePath(std::string_view p);

        // The DEDUP identity: NormalisePath, then fold "<dir>/x.arcproj" down to
        // "<dir>". Recents holds both folder-shaped and manifest-file entries and
        // both must resolve to the same project.
        [[nodiscard]] std::string ProjectDirKey(std::string_view p);

        // %LOCALAPPDATA%\Arcane\hub\recents.archub; empty if LOCALAPPDATA is unset.
        [[nodiscard]] std::filesystem::path DefaultFile();

        // --- read ----------------------------------------------------------

        // Entries in FILE ORDER (newest first). Empty -- never an exception --
        // for a missing, unreadable, unparseable, or newer-format document.
        [[nodiscard]] std::vector<RecentProject> Parse(const std::string& json);
        [[nodiscard]] std::vector<RecentProject> Load(const std::filesystem::path& file);

        // --- select (PURE) --------------------------------------------------

        // Drops entries built for another ABI (counted), the currently-open
        // project, and paths that are gone, then caps the result. `exists` is
        // injected so this runs headlessly in tests -- and because the Hub's own
        // `missing` flag is a snapshot that can be stale by the time we read it.
        [[nodiscard]] RecentSelection Select(const std::vector<RecentProject>& all,
                                             std::uint32_t thisAbi,
                                             std::string_view currentProjectPath,
                                             const std::function<bool(const std::string&)>& exists,
                                             std::size_t cap = kMaxShown);

        // --- write ----------------------------------------------------------

        // Move `projectPath`'s entry to the front and refresh its lastOpenedUtc,
        // inserting one if absent. Returns the new document text, or EMPTY if
        // `json` must not be overwritten (unparseable, or a newer format) --
        // callers must treat empty as "do not write".
        [[nodiscard]] std::string Touch(const std::string& json,
                                        const std::string& projectPath,
                                        const std::string& name,
                                        std::uint32_t abi,
                                        std::uint64_t nowUnixSeconds);

        // Write via a sibling temp file then rename over the target -- the same
        // primitive as the Hub's store::write_atomic, so a crash mid-write leaves
        // either the old document or the new one, never a truncated file.
        bool WriteAtomic(const std::filesystem::path& file, const std::string& text);

        // read -> Touch -> WriteAtomic. Never throws and never reports: a recent
        // list is not worth failing (or even warning) a successful project open
        // over. Silent no-op when the document must not be overwritten.
        void TouchFile(const std::filesystem::path& file,
                       const std::string& projectPath,
                       const std::string& name,
                       std::uint32_t abi);
    }
}
