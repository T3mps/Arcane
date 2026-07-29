// EditorApp, project + asset plumbing: the Open-Project soft restart, the
// material/instance creation flows, the DocServices the shader documents are
// built from, and the ~1 Hz .arcmat file watcher. Split out of EditorApp.cpp as
// a pure move.
//
// SwitchProject and the CreateXAt effects are called ONLY from the frame loop's
// top-of-frame phases or its deferred sceneAction (EditorAppFrame.cpp) -- never
// mid-render, because they tear down plugin/document GPU resources that this
// frame's already-built ImGui draw lists may still reference. The dialog THUNKS
// below are the background-thread half: they stash a path under
// m_pendingProjectMutex / m_pendingMaterialMutex and nothing else.

#include "EditorApp.hpp"
#include "AssetBrowser.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (pre-teardown ABI gate)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>   // Save/LoadSpriteAsset (MintOrReuseSpriteForTexture)

#include <ProjectBoot.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace Arcane::Editor
{
    Arcane::Editor::DocServices EditorApp::MakeDocServices()
    {
        Arcane::Editor::DocServices s;
        s.device   = m_gpu->Device().Nvrhi();
        s.shaders  = &m_gpu->Shaders();
        s.compiler = m_shaderCompiler.get();
        s.sources  = &m_shaderSources;
        s.runtime  = &*m_runtime;
        s.undo     = m_undo ? &*m_undo : nullptr;
        s.clock    = &m_editorClock;
        s.backend  = m_gpu->Device().Backend();
        s.onAssetSaved = [this](const Arcane::Guid& id)
        {
            if (m_spriteMaterials)
                m_spriteMaterials->Invalidate(id);
            if (m_postChains)
                m_postChains->Invalidate(id);
            // Re-baseline the file watcher: our own save is not an external
            // edit and must not bounce back as a reload.
            if (const Arcane::Project* p = m_runtime ? m_runtime->CurrentProject()
                                                     : nullptr)
                if (const auto path = p->ResolveAsset(Arcane::AssetId::FromGuid(id)))
                {
                    std::error_code ec;
                    const auto t = std::filesystem::last_write_time(*path, ec);
                    if (!ec)
                        m_materialMtimes[path->generic_string()] = t;
                }
        };
        s.onParamRenamed = [this](const Arcane::Guid& id, const std::string& oldName,
                                  const std::string& newName)
        {
            // Assisted rename rewrote this instance's file; an OPEN document
            // for it gets patched in memory (never stomped -- re-key only).
            if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(
                    m_documents.FindByGuid(id)))
                doc->PatchParamRename(oldName, newName);
        };
        return s;
    }

    void EditorApp::PollMaterialWatch()
    {
        if (m_editorClock < m_materialWatchNext)
            return;
        m_materialWatchNext = m_editorClock + 1.0;
        const Arcane::Project* project =
            m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return;

        for (const Arcane::Editor::AssetEntry& e :
             Arcane::Editor::BuildAssetEntries(project->Registry()))
        {
            if (e.kind != Arcane::Editor::AssetKind::Material)
                continue;
            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
            if (!path)
                continue;
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(*path, ec);
            if (ec)
                continue;   // deleted/unreadable -- documents keep last-good
            const auto [it, inserted] =
                m_materialMtimes.try_emplace(path->generic_string(), mtime);
            if (inserted || it->second == mtime)
            {
                it->second = mtime;
                continue;   // first sighting is the baseline, not an event
            }
            it->second = mtime;

            // An EXTERNAL edit landed: scene sprites re-resolve, an open
            // document for the asset reloads (clean) or keeps its edits with
            // a warn (dirty -- never stomped), and open documents whose
            // PARENT chain contains it re-resolve + recompile.
            ARC_INFO("material '{}' changed on disk", e.name);
            if (m_spriteMaterials)
                m_spriteMaterials->Invalidate(e.guid);
            if (m_postChains)
                m_postChains->Invalidate(e.guid);
            m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
            {
                auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d);
                if (!doc)
                    return;
                if (doc->AssetGuid() == e.guid)
                {
                    if (doc->Dirty())
                        ARC_WARN("'{}' changed on disk but has unsaved edits here -- "
                                 "keeping yours (Save overwrites the disk version)",
                                 e.name);
                    else
                        doc->ReloadFromDisk();
                }
                else if (doc->DependsOn(e.guid))
                    doc->RefreshParentChain();
            });
        }
    }

    void EditorApp::MaterialNewPickedThunk(const char* path, void* user)
    {
        // SDL dialog callback thread (see ProjectPickedThunk).
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingMaterialMutex);
        self->m_pendingMaterialNewPath = path;
    }

    void EditorApp::MaterialOpenPickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingMaterialMutex);
        self->m_pendingMaterialOpenPath = path;
    }

    void EditorApp::InstanceNewPickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingMaterialMutex);
        self->m_pendingInstanceNewPath = path;
    }

    void EditorApp::CreateInstanceAt(std::filesystem::path path, Arcane::Guid parent)
    {
        if (!parent.IsValid())
            return;
        if (path.extension() != ".arcmat")
            path += ".arcmat";

        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.parent = parent;
        data.name = path.stem().string();
        if (!Arcane::SaveMaterialAsset(path, data))
        {
            ARC_WARN("Arcane Editor: could not create instance at '{}'", path.generic_string());
            return;
        }
        // Register immediately -- ResolveParentChain on the new document needs the
        // registry to know BOTH this instance and its parent right now.
        m_runtime->RegisterCreatedAsset(path);
        m_documents.OpenPath(path);
    }

    // Reuse-or-mint policy (sprite-asset spec, Section 3): exactly one existing
    // .arcsprite referencing this texture -> reuse it; zero or several -> mint a
    // fresh sibling (never guess among duplicates).
    //
    // WHY the linear scan: this loads every registered .arcsprite off disk on
    // EACH call to find matches by `texture`, same shape as PollMaterialWatch's
    // sweep above -- registries are small today (dozens, not thousands, of
    // sprite assets per project), so a per-call scan is the simplest correct
    // thing. It would need a texture->sprites index (built once, invalidated on
    // sprite save/delete like m_materialMtimes) if per-project sprite counts
    // grow large enough for this to show up as a hitch; that index does not
    // exist yet and is not built here.
    Arcane::Guid EditorApp::MintOrReuseSpriteForTexture(const Arcane::Guid& textureGuid)
    {
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project || !textureGuid.IsValid())
            return {};

        Arcane::Guid unique{};
        int matches = 0;
        for (const auto& [guid, mount] : project->Registry().All())
        {
            if (Arcane::Editor::AssetKindOf(mount) != Arcane::Editor::AssetKind::Sprite)
                continue;
            const auto p = project->ResolveAsset(Arcane::AssetId::FromGuid(guid));
            if (!p)
                continue;
            const auto data = Arcane::LoadSpriteAsset(*p);
            if (data && data->texture == textureGuid)
            {
                ++matches;
                unique = guid;
            }
        }
        if (matches == 1)
            return unique;   // exactly one match -- reuse it, never guess among duplicates

        const auto texPath = project->ResolveAsset(Arcane::AssetId::FromGuid(textureGuid));
        if (!texPath)
        {
            ARC_WARN("Arcane Editor: could not mint a sprite -- texture '{}' did not "
                     "resolve to a file", textureGuid.ToString());
            return {};
        }
        std::filesystem::path target = texPath->parent_path() / (texPath->stem().string() + ".arcsprite");
        for (int i = 1; std::filesystem::exists(target); ++i)   // never clobber an existing file
            target = texPath->parent_path() /
                     (texPath->stem().string() + "-" + std::to_string(i) + ".arcsprite");

        Arcane::SpriteAssetData data;
        data.id      = Arcane::Guid::Generate();
        data.name    = target.stem().string();
        data.texture = textureGuid;
        if (!Arcane::SaveSpriteAsset(target, data))
        {
            ARC_WARN("Arcane Editor: could not mint a sprite at '{}'", target.generic_string());
            return {};
        }
        // Register immediately -- same reasoning as CreateInstanceAt above: an
        // Inspector drop that mints and then assigns the Guid this same frame
        // needs the registry to already know the new asset. Checked, unlike
        // CreateInstanceAt's fire-and-forget call: RegisterCreatedAsset ->
        // Project::RegisterAsset returns nullopt when the target is outside
        // every content root (Project.cpp:222-226, which already ARC_WARNs
        // why) -- returning the freshly-minted id anyway would hand back a
        // Guid that can never resolve, writing a permanently broken reference
        // into whatever field triggered the mint. The file stays on disk
        // (never deleted) and the caller sees Nil, so the drop/menu action is
        // a diagnosable no-op instead.
        if (!m_runtime->RegisterCreatedAsset(target))
            return {};
        return data.id;
    }

    void EditorApp::CreateMaterialAt(std::filesystem::path path)
    {
        if (path.extension() != ".arcmat")
            path += ".arcmat";

        // UE-model: every new material is GRAPH-owned (freeform HLSL lives in
        // Custom nodes; legacy text-owned .arcmat files still open fine).
        // Starter = a Color wired to the Output -- never an empty canvas.
        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.name = path.stem().string();
        data.kind = "fullscreen";
        Arcane::MaterialGraph g;
        Arcane::GraphNode out;
        out.id = 1;
        out.type = Arcane::GraphNodeType::Output;
        out.posX = 420.0f;
        out.posY = 200.0f;
        Arcane::GraphNode color;
        color.id = 2;
        color.type = Arcane::GraphNodeType::ConstColor;
        color.posX = 160.0f;
        color.posY = 200.0f;
        color.value[0] = 0.2f; color.value[1] = 0.8f;
        color.value[2] = 1.0f; color.value[3] = 1.0f;
        g.nodes = { out, color };
        Arcane::GraphLink l;
        l.fromNode = 2;
        l.toNode = 1;
        g.links.push_back(l);
        g.nextId = 3;
        auto gen = Arcane::GenerateGraphSnippet(g, Arcane::MaterialSurface::Fullscreen);
        data.snippet = std::move(gen.snippet);
        data.graph = std::move(g);
        if (!Arcane::SaveMaterialAsset(path, data))
        {
            ARC_WARN("Arcane Editor: could not create material at '{}'", path.generic_string());
            return;
        }
        // Register with the open project's registry so the new asset appears in
        // the browser and resolves by GUID IMMEDIATELY (not on next project open).
        m_runtime->RegisterCreatedAsset(path);
        m_documents.OpenPath(path);
    }

    void EditorApp::ProjectPickedThunk(const char* path, void* user)
    {
        // Runs on an SDL-owned BACKGROUND thread (the Windows dialog backend
        // fires the callback from a detached worker, not PumpEvents/the main thread) --
        // m_pendingProjectPath must be synchronized against MainLoop's top-of-frame read.
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingProjectMutex);
        self->m_pendingProjectPath = path;
    }

    void EditorApp::SwitchProject(const std::filesystem::path& path)
    {
        // Validate FIRST -- never tear down a live session for a project we cannot
        // fully open. This mirrors BOTH checks Runtime::OpenProject will do (open +
        // ABI gate) so the post-teardown OpenProject below cannot fail for those
        // reasons, and a bad pick leaves the current session completely untouched.
        auto probe = Arcane::Project::Open(path);
        if (!probe)
        {
            ARC_ERROR("Open Project: '{}' is not a valid Arcane project", path.generic_string());
            m_projectOpenError = "'" + path.generic_string() +
                                 "' is not a valid Arcane project (no readable .arcproj).";
            return;
        }
        if (probe->Manifest().engineAbi != static_cast<int>(Arcane::kGamePluginABIVersion))
        {
            ARC_ERROR("Open Project: '{}' targets engine ABI {} but this engine is ABI {}",
                      path.generic_string(), probe->Manifest().engineAbi,
                      static_cast<int>(Arcane::kGamePluginABIVersion));
            m_projectOpenError = "'" + path.generic_string() + "' targets engine ABI " +
                                 std::to_string(probe->Manifest().engineAbi) +
                                 " but this editor is ABI " +
                                 std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) +
                                 ".\nRebuild the project's game DLL against this engine "
                                 "and update its manifest.";
            return;
        }

        // Documents belong to the outgoing project (their texture params and
        // parent chains resolve through ITS registry). Refuse to switch over
        // unsaved edits -- no silent loss -- and close the rest (review m5).
        if (m_documents.AnyDirty())
        {
            ARC_ERROR("Open Project: unsaved material documents -- save or close them "
                      "before switching projects");
            m_projectOpenError = "There are unsaved material documents.\n"
                                 "Save or close them before switching projects.";
            return;
        }
        m_documents.CloseAll();
        // Materials resolved against the outgoing project's registry.
        if (m_spriteMaterials)
            m_spriteMaterials->Clear();
        if (m_postChains)
            m_postChains->Clear();
        // Sprite-asset arc, Task 3 review fix (F2): sprites resolve through
        // the same outgoing-project registry, so they need the same Clear.
        if (m_sprites)
            m_sprites->Clear();

        // Return to Edit + clear editor state that references the outgoing scene.
        ClearSceneReferences();
        // The scene the session named belonged to the OUTGOING project, and the new
        // project's registry is built by its plugin, not loaded from an .arcscene --
        // so the session goes back to Untitled/clean here rather than at the end,
        // where the "OpenProject failed after validation" return would skip it and
        // leave a stale path with a spurious dirty marker.
        if (m_undo) m_scene.Reset(*m_undo);

        // The OUTGOING project's root, captured before teardown replaces it:
        // its editor lock must be released whichever way the switch ends --
        // success hands the lock to the new project, and the failure path
        // below leaves the editor project-less, which is not "open".
        const std::filesystem::path outgoingRoot =
            m_runtime->CurrentProject() ? m_runtime->CurrentProject()->Root()
                                        : std::filesystem::path{};

        // Idle the GPU before freeing plugin-owned GPU resources, then unload the plugin
        // (dtor: Unload -> ClearSystems + ResetRegistry, DLL still mapped).
        m_gpu->Device().Nvrhi()->waitForIdle();
        m_plugin.reset();

        // Commit the new project (sets Assets content-root) + reload input via its mount.
        if (!m_runtime->OpenProject(path))
        {
            ARC_ERROR("Open Project: OpenProject('{}') failed after validation", path.generic_string());
            m_projectOpenError = "Opening '" + path.generic_string() +
                                 "' failed after validation.\nThe previous session was torn "
                                 "down -- open another project to continue (see Console).";
            if (!outgoingRoot.empty())
                Arcane::EditorLock::Clear(outgoingRoot);
            return;   // editor left with no plugin; user can Open another project
        }
        // Hand the lock over: the old project is closed, the new one is ours.
        // (A same-project re-open just rewrites its own lock -- harmless.)
        if (!outgoingRoot.empty())
            Arcane::EditorLock::Clear(outgoingRoot);
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
            Arcane::EditorLock::Write(proj->Root());
        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
            ARC_WARN("Open Project: input actions failed to load");

        // Load the new game module (and/or the project's plugin modules) through the same
        // ABI-versioned plugin host. An empty gameModule with plugins = a plugins-only host.
        const std::string gameModule =
            Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
        const auto pluginModules = Arcane::HostBoot::PluginModules(m_runtime->CurrentProject());
        if (!gameModule.empty() || !pluginModules.empty())
        {
            m_plugin.emplace(*m_runtime,
                gameModule.empty() ? std::filesystem::path{} : std::filesystem::path(gameModule));
            for (const auto& dll : pluginModules)
                m_plugin->AddPlugin(dll);
            if (!m_plugin->Load())
            {
                ARC_ERROR("Open Project: failed to load the game module / project plugins");
                m_projectOpenError = "The project opened, but its game module / plugins "
                                     "failed to load (see Console).\nCheck the DLL paths in "
                                     "the manifest and that they are built against ABI " +
                                     std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) + ".";
            }
        }

        // Task 7: same boot-scene handoff as Init, after THIS project's plugin
        // load (not the outgoing project's) so a component type the new game
        // module registers deserializes rather than being dropped. m_scene was
        // already reset to Untitled above, before OpenProject -- Adopt() here
        // retargets it onto the new project's boot scene when it has one.
        if (m_undo)
        {
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                if (const auto boot = Arcane::HostBoot::BootScene(*m_runtime, *proj))
                {
                    m_scene.Adopt(boot->file, boot->id, *m_undo);
                    m_frameOnSceneOpen = true;
                }
            }
        }
        EnsureScene();

        m_runtime->Loop().SetPaused(true);   // back to Edit
        UpdateWindowTitle();
    }
}
