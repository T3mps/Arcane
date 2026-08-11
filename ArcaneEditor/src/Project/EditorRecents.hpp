#pragma once
// Both recents lists behind one facade (architecture pass sec 10): the Hub's
// shared machine-wide project list + the per-project scene list were two
// fully parallel ~10-site call ladders, always invoked in adjacent lines.
// A third recents kind is one method here, not ten call-site edits.
#include "Project/RecentProjects.hpp"
#include "Project/SceneRecents.hpp"
#include <filesystem>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    struct EditorRecents
    {
        RecentSelection    projects;         // Hub chain (RecentProjects.hpp)
        SceneRecents::List scenes;           // <root>/Saved/recent_scenes.json
        bool               fileMenuWasOpen = false;

        // File-menu rising edge + any explicit refresh point.
        void RefreshAll(const Arcane::Project* proj);
        // SUCCESS paths only -- a refused open must never reorder either list.
        // Records the project in the Hub file, refreshes, and reloads the
        // scene list (the two calls every site already made adjacently).
        void NoteProjectOpened(const Arcane::Project* proj);
        void NoteSceneOpened(const Arcane::Project* proj,
                             const std::filesystem::path& file);
    };
}
