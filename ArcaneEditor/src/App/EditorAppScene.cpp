// EditorApp, scene lifecycle: the .arcscene effects (new/open/save), the scene
// dialog launch helper, the editor-state teardown they share, and the
// viewport-framing helpers. Split out of EditorApp.cpp as a pure move.
//
// The effects here are called ONLY from the frame loop's top-of-frame phases or
// its deferred sceneAction (EditorAppFrame.cpp) -- never mid-render. The scene
// dialogs launch through the shared PathPickedThunk trampoline (EditorAppFrame.cpp),
// which Stash()es into m_dialogs.sceneOpen/sceneSave -- the background-thread
// half of that contract.

#include "App/EditorApp.hpp"
#include "Viewport/EditorCamera.hpp"
#include "Project/RuntimeLaunch.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // Arcane::SceneRoot (DoSaveScene's empty-scene guard)
#include <Arcane/Scene/TransformSystems.hpp>   // Edit-mode derived-transform refresh
#include <Arcane/Serialization/SceneAsset.hpp>   // .arcscene read/apply/save (New/Open/Save Scene)

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane::Editor
{
    namespace
    {
        // Same FILE, not the same spelling. The Open-Scene dialog and the Save-Scene
        // dialog can hand back different-but-equivalent paths for one file
        // (separator style, casing, a non-canonical prefix), and DoSaveScene keys
        // the scene's asset Guid off this compare -- a false "different file"
        // re-mints the Guid and dangles the id the asset registry already holds for
        // that file (which boot-scene references store BY Guid). weakly_canonical
        // does not require the file to exist; the error_code overload keeps this
        // exception-free, and a filesystem error falls back to the lexical compare
        // rather than reporting a bogus match.
        bool SameSceneFile(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::error_code ea, eb;
            const std::filesystem::path ca = std::filesystem::weakly_canonical(a, ea);
            const std::filesystem::path cb = std::filesystem::weakly_canonical(b, eb);
            if (ea || eb) return a == b;
            return ca == cb;
        }

        // The editor exe's OWN directory -- RuntimeLaunch::ExeCandidates wants
        // this to find ArcaneRuntime.exe beside or as a sibling of it. Same
        // GetModuleFileNameW pattern as EditorFonts.cpp's private ExeDir()
        // (not shared: each TU that needs it keeps its own small copy, same
        // as that file's own comment on the precedent).
        std::filesystem::path CurrentExeDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) != 0)
                return std::filesystem::path(buf).parent_path();
#endif
            return std::filesystem::current_path();
        }
    }

    // There is ALWAYS a scene open, the way Unity and UE always have a level open.
    //
    // Without this, a project with no bootScene whose game module does not spawn
    // anything (the correct shape for a data-driven project) opens with no SceneRoot
    // at all -- and since both the render walk and the save walk are subtree walks
    // rooted there, the editor would silently refuse the first thing anyone does:
    // Outliner > New Entity returns invalid and the menu item appears to do nothing.
    //
    // Deliberately does NOT reset the registry. A plugin that spawned its own entities
    // and published its own SceneRoot keeps them -- "no scene loaded means nothing
    // clears" is what makes data-driven scenes safe to adopt one project at a time.
    void EditorApp::EnsureScene()
    {
        Astra::Registry& reg = m_runtime->Registry();
        if (reg.GetResource<Arcane::SceneRoot>())
            return;

        Arcane::Scene::CreateEmpty(reg);
        m_scene.Reset(*m_undo);
        m_frameOnSceneOpen = true;
        ARC_INFO("No scene loaded -- started an empty one");
    }

    // Put the newly-opened scene on screen.
    //
    // Deferred rather than framed on the spot: a scene can become current before
    // the Viewport panel has ever been laid out (Init, and a project switch), and
    // framing into a zero-sized panel fits nothing. The default camera puts the
    // world ORIGIN at the panel's top-left, so without this, opening a project
    // shows mostly empty space until the user discovers Home -- which reads as
    // the content having failed to load.
    void EditorApp::FrameSceneIfPending()
    {
        if (!m_frameOnSceneOpen) return;
        if (m_viewport->Width() == 0 || m_viewport->Height() == 0) return;
        m_frameOnSceneOpen = false;

        const glm::vec2 panel((float)m_viewport->Width(), (float)m_viewport->Height());
        Arcane::TransformPropagationSystem{}(m_runtime->Registry());
        if (Arcane::Editor::SceneFramingBounds(m_runtime->Registry()).Valid())
        {
            FrameCamera(/*selectionOnly*/false);
            return;
        }
        // An empty scene has nothing to fit, but the origin is where the user is
        // about to build -- centre it rather than leaving it in the corner.
        m_camera.offset = panel * 0.5f;
    }

    // Scene dialogs start in the project's Content/scenes, created on demand:
    // a project scaffolded before scenes existed has no such folder, and the
    // dialog would silently fall back to the OS default.
    std::string EditorApp::SceneDialogDir()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj) return {};
        const std::filesystem::path dir = proj->Root() / "Content" / "scenes";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }

    void EditorApp::ShowSceneSaveDialog()
    {
        const std::string dir = SceneDialogDir();
        m_gpu->Win().ShowSaveFileDialog(&EditorApp::PathPickedThunk,
            new PathDialogRequest{ &m_dialogs.sceneSave, m_dialogs.sceneSave.Arm() },
            "Arcane Scene", "arcscene",
            dir.empty() ? nullptr : dir.c_str());
    }

    void EditorApp::ClearSceneReferences()
    {
        // Every entity handle the editor is holding names an entity of the OUTGOING
        // scene, and none of them survive the registry swap that follows (Runtime::
        // ResetRegistry, or PlaySession::Stop's RestoreRegistry). Play is stopped
        // FIRST because Stop restores the pre-Play snapshot: left running, it would
        // later overwrite whatever scene is loaded after this.
        if (InPlayMode())
            m_play.Stop(*m_runtime, m_plugin ? m_plugin->Vtable() : nullptr);
        m_selection.Clear();
        m_outliner = {};
        // A gizmo drag holds pre-drag poses for entities that are about to stop
        // existing, and both it and the Inspector park a CommandStack ownership
        // token that Clear() below retires.
        m_gizmoDrag = {};
        m_inspector = {};
        if (m_undo) m_undo->Clear();
    }

    bool EditorApp::DoNewScene()
    {
        ClearSceneReferences();
        m_runtime->ResetRegistry();
        Arcane::Scene::CreateEmpty(m_runtime->Registry());
        m_scene.Reset(*m_undo);
        // Unlike Open Scene, this path never calls LoadJson to republish (and
        // thereby retract) the "scene" key -- do it explicitly so the outgoing
        // scene's rows don't survive into the new, empty one.
        Arcane::Diagnostics::Clear("scene");
        m_frameOnSceneOpen = true;
        ARC_INFO("New scene");
        return true;
    }

    bool EditorApp::DoOpenScene(const std::filesystem::path& file)
    {
        // READ FIRST. A rejected file must leave the current scene exactly as it is,
        // not empty the editor and then report a failure -- which is what a
        // ResetRegistry-then-load would do. This is the whole reason ReadSceneFile
        // and ApplySceneDocument are separate calls (SceneAsset.hpp).
        std::string err;
        const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
        if (!doc)
        {
            ARC_ERROR("Open Scene: {}", err);
            m_modalErrors.Push("Scene Error", err);
            return false;
        }

        ClearSceneReferences();
        m_runtime->ResetRegistry();
        if (!Arcane::Scene::ApplySceneDocument(*doc, m_runtime->Registry()))
        {
            // Validated but unloadable -- the failure mode ReadSceneFile's structural
            // gate cannot see (a component whose reflected field type is unsupported).
            // The registry is already empty by contract, so nothing of the previous
            // scene was overwritten; give the user a well-formed empty scene rather
            // than the half-populated one this left behind.
            Arcane::Scene::CreateEmpty(m_runtime->Registry());
            m_scene.Reset(*m_undo);
            m_modalErrors.Push("Scene Error", "'" + file.generic_string() +
                           "' parsed but could not be loaded (see Console).");
            ARC_ERROR("Open Scene: ApplySceneDocument failed for {}", file.generic_string());
            return false;
        }

        m_scene.Adopt(file, doc->id, *m_undo);
        NoteSceneOpened(file);
        m_frameOnSceneOpen = true;
        ARC_INFO("Opened scene {}", file.generic_string());
        return true;
    }

    bool EditorApp::DoSaveScene(const std::filesystem::path& file)
    {
        // Play BACKSTOP, enforced here -- where the bytes are written -- rather than
        // only at the UI call sites. During Play the registry holds play-time
        // mutation that PlaySession::Stop exists to DISCARD; the authored scene is
        // the pre-Play snapshot. Serializing the live registry would therefore
        // overwrite the user's authored file with throwaway state AND report
        // success. The File-menu items and the Ctrl+S edge gate themselves, but
        // gating only there is what let this through: New Scene and Open Scene are
        // deliberately NOT disabled during Play, so the unsaved-changes confirm
        // modal is reachable mid-Play and its Save button lands straight here (it
        // now stops Play first, and satisfies this rather than tripping it), and
        // Play can still begin between a Save As dialog's launch and the frame its
        // result arrives on. From here, no call site can bypass the invariant.
        if (InPlayMode())
        {
            ARC_ERROR("Save Scene: refused -- play mode is running");
            m_modalErrors.Push("Scene Error", "Cannot save while play mode is running.\n"
                           "Stop play mode -- which restores the authored scene -- and save again.");
            return false;
        }

        // SaveJson walks the SceneRoot subtree and returns an EMPTY document when
        // that resource is absent (SceneSerializer.hpp), so without this guard a
        // rootless registry writes {version, entities: []} over the target file,
        // registers it, marks the session clean and logs success -- silent data
        // loss dressed as a save. SceneAsset::CreateEmpty documents the same hazard
        // for New Scene; the write path needs the same care.
        if (!m_runtime->Registry().GetResource<Arcane::SceneRoot>())
        {
            ARC_ERROR("Save Scene: refused -- the registry has no SceneRoot");
            m_modalErrors.Push("Scene Error", "There is no scene to save.\n"
                           "Create one with File -> New Scene, or open an existing scene.");
            return false;
        }

        // Reuse the scene's existing id when saving over itself; mint one for a new
        // file so the asset registry has something stable to register it under.
        // Identity is by canonical path, not by spelling -- see SameSceneFile.
        const bool sameFile = !m_scene.Path().empty() && SameSceneFile(m_scene.Path(), file);
        const Arcane::Guid id = (sameFile && m_scene.Id().IsValid())
                              ? m_scene.Id() : Arcane::Guid::Generate();

        std::string err;
        if (!Arcane::Scene::SaveSceneFile(file, m_runtime->Registry(), id, &err))
        {
            ARC_ERROR("Save Scene: {}", err);
            m_modalErrors.Push("Scene Error", err);
            return false;
        }

        // Register the written file so it resolves by Guid and lists in the browser.
        // AssetRegistry reads the id back out of the file, so the registered Guid is
        // the one just stamped. A scene saved outside the project's content root
        // cannot be registered -- Runtime/Project already log exactly why, and it is
        // not a save failure: the bytes are on disk either way.
        m_runtime->RegisterCreatedAsset(file);

        m_scene.Adopt(file, id, *m_undo);
        NoteSceneOpened(file);
        ARC_INFO("Saved scene {}", file.generic_string());

        // A save is the moment the project's cover thumbnail is most honest
        // -- what was just saved is on the viewport. Safe here: saves run
        // from top-of-frame phases, never mid-render (this file's header),
        // which is exactly what the capture's waitForIdle needs.
        WriteAutoScreenshot();
        return true;
    }

    // Play button's SeparateWindow branch (Task 6, runtime-host-fold arc):
    // fire-and-forget spawn of ArcaneRuntime.exe on the ACTIVE scene, as
    // SAVED (spec option b) -- never a snapshot of unsaved edits. The editor
    // takes no lock on the child and does not track it; m_play/the toolbar's
    // Play toggle are untouched either way (see DrawSimTimeToolbar).
    //
    // MANDATORY guard, not optional (Task 5 review finding): a never-saved
    // scene has a NIL guid (SceneSession.hpp -- "nil until saved"; the
    // id-reuse branch a few lines above DoSaveScene's SaveSceneFile call is
    // the other place this matters), and RuntimeLaunch::BuildArgs silently
    // OMITS --scene for a nil guid -- the spawned runtime would then boot the
    // project manifest's bootScene instead of what is on screen, with no
    // signal anything was skipped. So the gate below treats "no valid id yet"
    // exactly like "dirty": both park behind the SAME "Save and Play?" modal
    // (DrawModals, EditorAppFrame.cpp), and only a successful save (which
    // assigns the id, same as any other Save Scene) may proceed past it.
    //
    // Re-entrant: the modal's Save button (already-saved branch, synchronously)
    // and ConsumeSceneDialogResults' deferred branch (never-saved branch, once
    // its async Save-As dialog actually lands) both call this function again
    // once the guard is satisfied, and it falls straight through to the spawn.
    void EditorApp::LaunchStandalone()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj)
        {
            m_modalErrors.Push("Play in Separate Window Failed",
                                "Open a project before playing in a separate window.");
            return;
        }

        if (!m_scene.Id().IsValid() || m_scene.IsDirty(*m_undo))
        {
            m_launchModalPending = true;
            return;
        }

        // Packaged layout first (ArcaneRuntime.exe beside ArcaneEditor.exe),
        // dev bin layout second (premake's per-project sibling directories) --
        // see RuntimeLaunch::ExeCandidates. Existence is this caller's job by
        // that function's own contract; SpawnDetached would also refuse a
        // missing exe, but resolving here is what lets the failure message
        // name BOTH candidate paths instead of just the one SpawnDetached tried.
        const std::vector<std::filesystem::path> candidates =
            Arcane::Editor::RuntimeLaunch::ExeCandidates(CurrentExeDir());

        std::filesystem::path resolved;
        std::error_code ec;
        for (const std::filesystem::path& candidate : candidates)
        {
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                resolved = candidate;
                break;
            }
        }

        if (resolved.empty())
        {
            std::string looked;
            for (const std::filesystem::path& candidate : candidates)
            {
                if (!looked.empty()) looked += "\nand\n";
                looked += "'" + candidate.string() + "'";
            }
            ARC_ERROR("LaunchStandalone: ArcaneRuntime.exe not found ({})", looked);
            m_modalErrors.Push("Play in Separate Window Failed",
                                "ArcaneRuntime.exe was not found. Looked in:\n" + looked);
            return;
        }

        const std::vector<std::wstring> args = Arcane::Editor::RuntimeLaunch::BuildArgs(
            proj->Root(), m_scene.Id(), m_config.backend);

        if (!Arcane::Editor::RuntimeLaunch::SpawnDetached(resolved, args))
            m_modalErrors.Push("Play in Separate Window Failed",
                                "Failed to launch '" + resolved.string() +
                                "'. See the Console for details.");
    }

    void EditorApp::FrameCamera(bool selectionOnly)
    {
        // WorldTransform is DERIVED, and in Edit mode the fixed phase (which
        // owns TransformPropagationSystem) is paused -- the refresh that keeps
        // it current runs later in the frame than this input-time call. Refresh
        // it here too, so framing an entity created or moved this frame reads
        // its real world pose instead of a stale or absent one. Edit-mode only,
        // which is the only mode that reaches here.
        Astra::Registry& reg = m_runtime->Registry();
        Arcane::TransformPropagationSystem{}(reg);

        const Arcane::Editor::FramingBounds bounds =
            selectionOnly ? Arcane::Editor::SelectionFramingBounds(reg, m_selection.Entities())
                          : Arcane::Editor::SceneFramingBounds(reg);
        if (!bounds.Valid())
            return;   // nothing framable: leave the user's view where it is

        m_camera.Frame(bounds.min, bounds.max,
                       glm::vec2((float)m_viewport->Width(), (float)m_viewport->Height()));
    }
}
