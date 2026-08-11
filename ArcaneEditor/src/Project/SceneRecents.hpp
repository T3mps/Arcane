#pragma once

// File -> Open Recent Scene: PER-PROJECT scene history, unlike RecentProjects
// (which mirrors the Hub's shared, machine-wide list). A scene only means
// something inside the project that owns it, so this list lives INSIDE the
// project at <root>/Saved/recent_scenes.json -- the same Saved/ convention
// EditorLock already uses (Project.cpp, EditorLock::FileFor) -- and is never
// shared across projects or machines.
//
// Unlike RecentProjects.hpp, the editor is the ONLY writer of this file, so
// there is no "never clobber a document another process owns" contract here:
// an unparseable or newer-version document is simply read as empty and the
// next Push/SaveFile happily starts a fresh one.
//
// Pure list ops (Parse/Serialize/Push/Prune) are headless (no ImGui, no
// engine) so the [editor] tests drive them directly, same pattern as
// RecentProjects/ConsoleModel/DiagnosticStore.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Arcane::Editor::SceneRecents
{
    // How many scenes the menu remembers, newest first.
    inline constexpr std::size_t kMaxEntries = 10;

    // Bumped only if the on-disk schema changes shape. A document numbered
    // above this was written by a newer editor build; read as empty rather
    // than misinterpreted.
    inline constexpr int kFormatVersion = 1;

    // Newest-first, lexically-normal generic strings (scenePath.
    // lexically_normal().generic_string()) -- comparable and displayable
    // without a second normalisation pass at every call site.
    struct List
    {
        std::vector<std::string> paths;
    };

    // <projectRoot>/Saved/recent_scenes.json -- mirrors EditorLock::FileFor's
    // Saved/ convention (Project.cpp).
    [[nodiscard]] std::filesystem::path FileFor(const std::filesystem::path& projectRoot);

    // --- read -------------------------------------------------------------

    // Entries in file order (newest first). Empty -- never an exception --
    // for empty/malformed text, a document missing the expected shape, or a
    // newer format version.
    [[nodiscard]] List Parse(const std::string& jsonText);

    // Schema: {"version":1,"scenes":["<path>",...]}.
    [[nodiscard]] std::string Serialize(const List& list);

    // --- mutate (PURE) ------------------------------------------------------

    // Normalise, move-or-insert at the front, cap at kMaxEntries. A duplicate
    // push (reopening/resaving the same scene) moves it to the front rather
    // than growing the list.
    void Push(List& list, const std::filesystem::path& scenePath);

    // Drop entries whose file no longer exists. `exists` is injected so this
    // runs headlessly in tests and so callers can use disk truth at call
    // time rather than a snapshot flag this format does not carry.
    template<typename ExistsFn>
    void Prune(List& list, ExistsFn exists)
    {
        std::vector<std::string> kept;
        kept.reserve(list.paths.size());
        for (const std::string& p : list.paths)
        {
            if (exists(p))
                kept.push_back(p);
        }
        list.paths = std::move(kept);
    }

    // --- file I/O -----------------------------------------------------------

    // read -> Parse. Empty list for a missing, unreadable, or unparseable file
    // -- never an exception.
    [[nodiscard]] List LoadFile(const std::filesystem::path& file);

    // Serialize -> write, creating the parent directory (Saved/) on the way.
    // Never throws and never fails the caller: a recents write failure is not
    // worth failing a scene open or save over. Logs ARC_WARN on failure.
    void SaveFile(const std::filesystem::path& file, const List& list);
}
