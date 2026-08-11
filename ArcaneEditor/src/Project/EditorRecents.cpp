#include "Project/EditorRecents.hpp"

#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion
#include <Arcane/Project/Project.hpp>

namespace Arcane::Editor
{
    void EditorRecents::RefreshAll(const Arcane::Project* proj)
    {
        const std::filesystem::path file = Recents::DefaultFile();
        const std::string current = proj ? proj->Root().string() : std::string();

        projects = Recents::Select(
            Recents::Load(file),
            static_cast<std::uint32_t>(Arcane::kGamePluginABIVersion),
            current,
            [](const std::string& p) {
                std::error_code ec;
                return std::filesystem::exists(p, ec);
            });

        // Project-less boot's File-menu prefill path: reached via
        // OnProjectOpened(nullptr) (EditorApp.cpp) so the very first File-menu
        // frame is already populated. A project-less call still needs both
        // lists in a defined state -- projects is refreshed unconditionally
        // above; scenes has nothing durable to reload without a project, but
        // still gets cleared here rather than left stale from a prior one.
        scenes = {};
        if (!proj)
            return;
        scenes = Arcane::Editor::SceneRecents::LoadFile(
            Arcane::Editor::SceneRecents::FileFor(proj->Root()));
        Arcane::Editor::SceneRecents::Prune(scenes,
            [](const std::string& p)
            {
                std::error_code ec;
                return std::filesystem::exists(p, ec);
            });
    }

    void EditorRecents::NoteProjectOpened(const Arcane::Project* proj)
    {
        if (!proj)
        {
            // Project-less boot (File -> Open Project is about to be raised).
            // Nothing to record, but the cache still wants filling so the very
            // first opening of the File menu is already correct.
            RefreshAll(proj);
            return;
        }

        const std::filesystem::path root = proj->Root();
        Recents::TouchFile(Recents::DefaultFile(),
                           root.string(),
                           root.filename().string(),
                           static_cast<std::uint32_t>(Arcane::kGamePluginABIVersion));
        RefreshAll(proj);
    }

    void EditorRecents::NoteSceneOpened(const Arcane::Project* proj,
                                        const std::filesystem::path& file)
    {
        if (!proj)
            return;   // project-less session: nowhere durable to record
        Arcane::Editor::SceneRecents::Push(scenes, file);
        Arcane::Editor::SceneRecents::SaveFile(
            Arcane::Editor::SceneRecents::FileFor(proj->Root()), scenes);
    }
}
