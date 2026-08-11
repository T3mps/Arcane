// EditorApp, project + asset plumbing: the Open-Project soft restart, the
// material/instance creation flows, the DocServices the shader documents are
// built from, and the ~1 Hz .arcmat file watcher. Split out of EditorApp.cpp as
// a pure move.
//
// SwitchProject and the CreateXAt effects are called ONLY from the frame loop's
// top-of-frame phases or its deferred sceneAction (EditorAppFrame.cpp) -- never
// mid-render, because they tear down plugin/document GPU resources that this
// frame's already-built ImGui draw lists may still reference. The project/
// material/instance dialogs launch through the shared PathPickedThunk /
// InstancePickedThunk trampolines (EditorAppFrame.cpp), which Stash() into
// m_dialogs -- the background-thread half of that contract.

#include "App/EditorApp.hpp"
#include "Panels/AssetBrowser.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (pre-teardown ABI gate)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>   // Save/LoadSpriteAsset (MintOrReuseSpriteForTexture)

#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Publish/Clear (the Build failure row)
#include <Arcane/Host/ProjectBoot.hpp>

#include <filesystem>
#include <memory>
#include <span>
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
            if (m_resolver)
                m_resolver->InvalidateMaterial(id);
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

    // ---- Problems-panel navigation (Task 5) --------------------------------
    // DocumentHost only indexes documents by asset Guid (its own header
    // comment: "open/dirty/save lifecycle over one GUID asset"), so the two
    // wrappers below live here rather than being renamed onto DocumentHost.

    Arcane::Editor::EditorDocument* EditorApp::OpenAssetDocument(const Arcane::Guid& guid)
    {
        if (!guid.IsValid())
            return nullptr;
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return nullptr;
        const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(guid));
        if (!path)
            return nullptr;
        return m_documents.OpenPath(*path);
    }

    Arcane::Editor::ShaderEditorDocument* EditorApp::FindByPath(const std::filesystem::path& path)
    {
        Arcane::Editor::ShaderEditorDocument* found = nullptr;
        m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
        {
            if (found)
                return;
            if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d);
                doc && doc->Path() == path)
                found = doc;
        });
        return found;
    }

    void EditorApp::RouteLocator(const Arcane::DiagLocator& locator)
    {
        switch (locator.kind)
        {
            case Arcane::DiagLocator::Kind::Entity:
            {
                // Selecting is enough: the Inspector follows the selection, and
                // the Outliner scrolls to it on the next frame. locator.entity is
                // the entity's raw packed value (id+version) widened to
                // uint64_t by the producer; Astra::Entity's StorageType is the
                // narrower type that value was minted from (32-bit by this
                // project's ASTRA_ENTITY_BITS default), so this narrows back
                // rather than using a nonexistent Astra::Entity::IDType.
                m_selection.Select(Astra::Entity(
                    static_cast<Astra::Entity::StorageType>(locator.entity)));
                break;
            }
            case Arcane::DiagLocator::Kind::Asset:
            {
                OpenAssetDocument(locator.asset);
                break;
            }
            case Arcane::DiagLocator::Kind::File:
            {
                // Shader/material documents are the only ROUTABLE File target --
                // FindByPath only ever matches an open ShaderEditorDocument. Other
                // File-locator producers (plugin dll load failures, assets outside
                // every content root, project manifest errors) point at paths that
                // are never an open document, so this is a deliberate no-op for
                // them today, not a bug.
                if (auto* doc = FindByPath(locator.file))
                    doc->RequestJumpToLine(locator.line);
                break;
            }
            case Arcane::DiagLocator::Kind::GraphNode:
            {
                if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(
                        OpenAssetDocument(locator.ownerAsset)))
                    doc->RequestFocusGraphNode(locator.nodeId);
                break;
            }
            case Arcane::DiagLocator::Kind::None:
                break;
        }
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
            if (m_resolver)
                m_resolver->InvalidateMaterial(e.guid);
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
        // every content root (Project.cpp:334-338, which already ARC_WARNs
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

    // EVERY mutable EditorApp member whose value refers to the current project
    // must appear in this function -- or carry a comment at its declaration
    // saying why it survives a switch (architecture pass sec 3; audit defect
    // A3 was three implicit reset lists, one of them unowned). Called from
    // SwitchProject's switch_teardown stage only: boot has no prior project
    // to reset.
    //
    // m_documents: every open document (material/shader/etc.) is opened
    // against the outgoing project's registry.
    // m_resolver: sprites, materials and the post chain all resolve against
    // the OUTGOING project's registry, so all three caches drop together
    // (one Clear since the sprite-resolution lift).
    // m_consoleDiag.store: Problems is current STATE, rebuilt by producers on
    // load (spec sec 10), not a log -- stale rows from the outgoing project
    // (asset/plugin/material/scene diagnostics keyed by paths and Guids that
    // belong to THAT project's registry) must not survive into the incoming
    // one. m_consoleDiag.console/.ui are NOT cleared here: the log stream is
    // process-wide, not project-scoped.
    // ClearSceneReferences(): editor state naming entities of the outgoing
    // scene, torn down before any registry swap.
    // m_scene: the scene the session named belonged to the OUTGOING project,
    // and the new project's registry is built by its plugin, not loaded from
    // an .arcscene -- so the session goes back to Untitled/clean here rather
    // than at the end, where a later failure would skip it and leave a stale
    // path with a spurious dirty marker.
    // m_recents.scenes: the outgoing project's scene history means nothing to
    // the incoming one. EditorRecents::NoteProjectOpened (switch_plugin_load)
    // repopulates it for whichever project ends up open -- including the
    // project-less fallback, which correctly leaves this empty.
    // m_assetBrowser: selection, search, and kind filter all belong to the
    // outgoing project's registry -- a Guid from it must not survive as the
    // Assets menu's tracked row.
    // -- previously in NO list (the A3 gap): --
    // m_dialogs: in-flight dialogs die with their project (sec 2).
    // m_modalErrors: a dead project's modal must not pop post-switch.
    // m_materialMtimes / m_materialWatchNext: the outgoing project's
    // path-keyed watch cache -- grew unbounded across switches before this.
    // (the two launch-modal flags this entry used to name are gone entirely --
    // a parked LaunchStandalone now lives in m_scene, covered by the m_scene
    // entry above; see the comment ahead of m_scene.Reset below.)
    void EditorApp::ResetPerProjectState()
    {
        m_documents.CloseAll();
        if (m_resolver)
            m_resolver->Clear();
        m_consoleDiag.store.ClearAll();
        ClearSceneReferences();
        if (m_undo) m_scene.Reset(*m_undo);
        m_recents.scenes = {};
        m_assetBrowser = {};
        m_dialogs.ClearAll();
        m_modalErrors.Clear();
        m_materialMtimes.clear();
        m_materialWatchNext = 0.0;
        // A parked LaunchStandalone cannot survive into a switch: OpenProject's
        // own Request is ignored while any intent is parked, so the modal
        // resolves first.
    }

    void EditorApp::SwitchProject(const std::filesystem::path& path)
    {
        // Another live editor already holds it -> refuse and surface THAT
        // editor; the current session stays untouched. The same guard as the
        // boot gate (main.cpp) -- SwitchProject is just the second door into
        // the same room. RivalPid is self-exempt, so re-opening the project
        // we already hold sails past our own lock. Checked BEFORE the probe:
        // Project::Open scans the project's content tree, and a refusal
        // should not pay for (or side-effect) any of that.
        {
            std::filesystem::path lockRoot = path;
            if (lockRoot.extension() == ".arcproj")
                lockRoot = lockRoot.parent_path();
            if (const auto rival = Arcane::EditorLock::RivalPid(lockRoot))
            {
                ARC_ERROR("Open Project: '{}' is already open in another editor (pid {})",
                          path.generic_string(), *rival);
                m_modalErrors.Push("Open Project Failed", "'" + path.generic_string() +
                                     "' is already open in another Arcane Editor.\n"
                                     "That editor has been brought to the front.");
                Arcane::EditorLock::FocusWindowOfProcess(*rival);
                return;
            }
        }

        // Validate FIRST -- never tear down a live session for a project we cannot
        // fully open. This mirrors BOTH checks Runtime::OpenProject will do (open +
        // ABI gate) so the post-teardown OpenProject below cannot fail for those
        // reasons, and a bad pick leaves the current session completely untouched.
        auto probe = Arcane::Project::Open(path);
        if (!probe)
        {
            ARC_ERROR("Open Project: '{}' is not a valid Arcane project", path.generic_string());
            m_modalErrors.Push("Open Project Failed", "'" + path.generic_string() +
                                 "' is not a valid Arcane project (no readable .arcproj).");
            return;
        }
        if (probe->Manifest().engineAbi != static_cast<int>(Arcane::kGamePluginABIVersion))
        {
            // This used to be a hard refusal of the WHOLE project, which locked
            // the user out of their own data across every engine ABI bump. The
            // stamp only describes what the game DLL was built against, and the
            // one genuinely dangerous case -- loading that stale DLL -- is
            // already refused cleanly by the plugin ABI gate (Plugin.cpp), which
            // surfaces plugin.abi.mismatch in the Problems panel naming both
            // versions and the fix.
            if (probe->Manifest().gameModule.empty() && probe->Manifest().plugins.empty())
            {
                // Content-only: no compiled code exists behind the stamp, so the
                // old number is inert metadata. Self-heal it (guid-self-heal
                // precedent) and open; best-effort -- an unwritable manifest
                // must not block the open it does not endanger.
                // Captured BEFORE the restamp mirrors the new value in memory.
                const int oldAbi = probe->Manifest().engineAbi;
                if (probe->RestampEngineAbi(static_cast<int>(Arcane::kGamePluginABIVersion)))
                    ARC_INFO("Open Project: '{}' upgraded from engine ABI {} to {} (content-only project)",
                             path.generic_string(), oldAbi,
                             static_cast<int>(Arcane::kGamePluginABIVersion));
                else
                    ARC_WARN("Open Project: '{}' targets engine ABI {} (this editor: {}) and its "
                             "manifest could not be restamped; opening anyway (content-only)",
                             path.generic_string(), oldAbi,
                             static_cast<int>(Arcane::kGamePluginABIVersion));
            }
            else
            {
                // Module project: open it -- the data is ABI-agnostic. Do NOT
                // restamp: the manifest must keep telling the Hub the truth
                // about the DLL until it is actually rebuilt.
                ARC_WARN("Open Project: '{}' targets engine ABI {} but this editor is ABI {} -- "
                         "opening; its game module will be refused until rebuilt (see Problems)",
                         path.generic_string(), probe->Manifest().engineAbi,
                         static_cast<int>(Arcane::kGamePluginABIVersion));
            }
        }

        // Documents belong to the outgoing project (their texture params and
        // parent chains resolve through ITS registry). Refuse to switch over
        // unsaved edits -- no silent loss -- and close the rest (review m5).
        if (m_documents.AnyDirty())
        {
            ARC_ERROR("Open Project: unsaved material documents -- save or close them "
                      "before switching projects");
            m_modalErrors.Push("Open Project Failed", "There are unsaved material documents.\n"
                                 "Save or close them before switching projects.");
            return;
        }
        // The OUTGOING project's root, captured before teardown replaces it:
        // its editor lock must be released whichever way the switch ends.
        // `lockedRoot` tracks whichever root WE currently hold the lock for --
        // starts as the outgoing root and is advanced to the new project's
        // root the moment switch_render_bridge hands it over below, so the
        // failure fallback (any stage, including one AFTER that handover)
        // always releases the lock we are actually holding, never a stale one.
        const std::filesystem::path outgoingRoot =
            m_runtime->CurrentProject() ? m_runtime->CurrentProject()->Root()
                                        : std::filesystem::path{};
        std::filesystem::path lockedRoot = outgoingRoot;

        // Amendment 1 (2026-07-30 human ruling): the brief's stub assigned
        // `ctx.projectPath = path.string().c_str()`, which dangles the moment
        // that statement ends (path.string() is a temporary). No BootContext
        // is built here at all -- this sequence is NOT CoreStages/EditorStages
        // (it never calls HostBoot::CoreStages/EditorStages/RuntimeStages, so
        // no shared stage body ever reads a ctx), and every stage below
        // captures `path`/`this` directly via `[&]`, exactly like the brief's
        // own stub stages already did. A ctx with nothing that reads it was
        // dead weight carrying a live dangling pointer; dropping it removes
        // the hazard at the source instead of papering over it with a named
        // std::string local.
        // Policy is an explicit parameter (not a hardcoded Fatal default) --
        // matching BootStage's own field order (id, deps, thread, policy,
        // weight, run) -- because switch_plugin_load below is Optional, not
        // Fatal (2026-07-30 review correction; see that stage's own comment).
        auto stage = [](std::string id, std::vector<std::string> deps,
                        Arcane::BootThread thread, Arcane::BootPolicy policy,
                        std::uint32_t weight, std::function<bool()> run)
        {
            Arcane::BootStage s;
            s.id = std::move(id); s.dependsOn = std::move(deps);
            s.thread = thread; s.policy = policy;
            s.weight = weight; s.run = std::move(run);
            return s;
        };

        // NOT CoreStages: the process is already booted. This is the reopen
        // subset only. switch_teardown/switch_project_open/switch_render_bridge
        // are Fatal -- the caller already validated the project (the guards
        // above) and is about to tear down the old one, so switch_project_open
        // genuinely has nothing to fall back to; a failure there converges on
        // the defined project-less fallback below (Runtime::CloseProject +
        // EditorLock + EnsureScene). switch_plugin_load is Optional (2026-07-30
        // review correction to the first draft's "unify every failure" call):
        // a failed game-module load leaves m_plugin disengaged, which is
        // ALREADY the exact same safe, defined state the "no game module for
        // this project" branch two lines below it produces on purpose --
        // every m_plugin-> use elsewhere is optional-guarded. Amendment 2
        // requires a clean fallback when a stage genuinely has nothing to
        // fall back to; it does not require turning an already-recoverable
        // outcome (project open, no plugin, a detailed error banner) into a
        // forced full revert that costs the whole project and a re-run of
        // File -> Open Project over what could be a colleague's WIP DLL or a
        // stale build.
        std::vector<Arcane::BootStage> stages;
        stages.push_back(stage("switch_teardown", {}, Arcane::BootThread::Main,
                               Arcane::BootPolicy::Fatal, 2, [&]
        {
            ResetPerProjectState();
            // Idle the GPU before freeing plugin-owned GPU resources, then
            // unload the plugin (dtor: Unload -> ClearSystems + ResetRegistry,
            // DLL still mapped).
            m_gpu->Device().Nvrhi()->waitForIdle();
            m_plugin.reset();
            return true;
        }));
        stages.push_back(stage("switch_project_open", {"switch_teardown"}, Arcane::BootThread::Worker,
                               Arcane::BootPolicy::Fatal, 6, [&]
        {
            // Disjoint-ownership proof: this stage touches exactly two things
            // -- `path` (a caller-local, read-only, and this stage's only
            // input) and m_runtime->OpenProject, which itself only mutates
            // Runtime-owned state (Impl::project/assets/config -- see
            // Runtime::OpenProject/CloseProject). None of that is read or
            // written by switch_teardown (already complete by construction --
            // BootSequence does not start a Worker stage until its
            // dependencies are done) or by switch_render_bridge/
            // switch_plugin_load (Main stages that do not become ready until
            // AFTER this one completes -- BootSequence is sequential-and-join
            // per the DAG, not concurrent Main+Worker access to the same
            // state). The one thing this overlaps with in practice is
            // BootPresenter::Present pumping the window/ImGui on the Main
            // thread while this runs -- disjoint by construction, since
            // Present never touches Runtime. Same shape as CoreStages'
            // project_open/gpu_core overlap (BootSequence.hpp's header
            // comment: "the DAG exists for exactly ONE overlap").
            return m_runtime->OpenProject(path);
        }));
        stages.push_back(stage("switch_render_bridge", {"switch_project_open"}, Arcane::BootThread::Main,
                               Arcane::BootPolicy::Fatal, 1, [&]
        {
            // Hand the lock over: the old project is closed, the new one is
            // ours. (A same-project re-open just rewrites its own lock --
            // harmless.)
            if (!lockedRoot.empty())
                Arcane::EditorLock::Clear(lockedRoot);
            lockedRoot.clear();
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                Arcane::EditorLock::Write(proj->Root());
                lockedRoot = proj->Root();   // now holding the NEW project's lock
            }
            if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
                ARC_WARN("Open Project: input actions failed to load");
            return true;
        }));
        stages.push_back(stage("switch_plugin_load", {"switch_render_bridge"}, Arcane::BootThread::Main,
                               Arcane::BootPolicy::Optional, 4, [&]
        {
            // Load the new game module (and/or the project's plugin modules)
            // through the same ABI-versioned plugin host. An empty gameModule
            // with plugins = a plugins-only host.
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
                    // Optional, not Fatal (2026-07-30 review correction -- see
                    // this stage's push_back comment above for the full
                    // reasoning): m_plugin.reset() below leaves EXACTLY the
                    // same safe, disengaged state the "no game module" branch
                    // two lines below produces on purpose, so this falls
                    // through to the SAME common tail (paused, boot scene,
                    // EnsureScene, title) rather than aborting the switch.
                    // Original SwitchProject's exact detailed banner restored
                    // here -- it survives to the user because this stage
                    // returning true (below) keeps BootSequence's overall
                    // r.ok == true, so the generic "switching to X failed at
                    // stage Y" message past seq.Run() never overwrites it.
                    ARC_ERROR("Open Project: failed to load the game module / project plugins");
                    m_modalErrors.Push("Open Project Failed",
                                         "The project opened, but its game module / plugins "
                                         "failed to load (see Console).\nCheck the DLL paths in "
                                         "the manifest and that they are built against ABI " +
                                         std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) + ".");
                    m_plugin.reset();
                }
            }
            else
            {
                ARC_INFO("Open Project: no game module / plugins for this project -- starting with no game loaded");
            }

            // Task 8: Arcane Editor boots in Edit mode -- the sim starts paused.
            m_runtime->Loop().SetPaused(true);

            // Same boot-scene handoff / EnsureScene / title / recents-record
            // as the boot path's StageFinalize -- see OnProjectOpened. m_scene
            // was already reset to Untitled in switch_teardown, so Adopt() here
            // (inside OnProjectOpened) retargets it onto the new project's boot
            // scene when it has one, using THIS project's plugin load (not the
            // outgoing project's) so a component type the new game module
            // registers deserializes rather than being dropped.
            OnProjectOpened();

            // Retarget the appdata layout ini at the incoming project (saves
            // the outgoing project's layout on the way -- see the method). Not
            // part of OnProjectOpened -- see StageFinalize's own call for why.
            RetargetLayoutIni();
            return true;
        }));

        // Overlay, not Fullscreen: the editor window is already up and this
        // must not erase its last frame (BootPresenter.cpp: only the
        // Fullscreen branch clears the backbuffer).
        Arcane::BootPresenter overlay(*m_gpu, Arcane::BootPresenterMode::Overlay);
        Arcane::BootSequence  seq(std::move(stages));
        const Arcane::BootResult r = seq.Run(&overlay);
        if (!r.ok)
        {
            // Important 1 (2026-07-31 review): a window close mid-switch is
            // NOT a failure -- the overlay presenter's Present() returned false
            // because it saw the OS quit event (BootPresenter.cpp), and
            // BootSequence::Run turned that into quitRequested + failedStage =
            // "quit requested". Reporting that through m_modalErrors would
            // show a bogus "failed at stage 'quit requested'" banner AND leave
            // the editor running -- the quit event is already consumed by the
            // presenter's own pump, so PumpFrameEvents' SDL_EVENT_QUIT check
            // will never see it on a later frame; without this branch the user
            // has to click the X a second time. m_requestExit hands the exit
            // back to the normal frame loop instead (see PumpFrameEvents).
            // The project-less convergence below still runs unconditionally --
            // it is correct regardless of why r.ok is false, per this block's
            // own "either way" comment further down.
            if (r.quitRequested)
            {
                ARC_WARN("Open Project: the switch to '{}' was aborted by a quit at stage '{}' -- "
                         "exiting. If you did not close the window, this is a spurious quit and the "
                         "BootPresenter line above names the stage it landed on.",
                         path.generic_string(), r.failedStage);
                m_requestExit = true;
            }
            else
            {
                ARC_ERROR("Open Project: switching to '{}' failed at stage '{}'",
                          path.generic_string(), r.failedStage);
                m_modalErrors.Push("Open Project Failed", "Switching to '" + path.generic_string() +
                                     "' failed at stage '" + r.failedStage +
                                     "'.\nThe editor was returned to a clean, project-less "
                                     "state (see Console) -- open another project to continue.");
            }

            // AMENDMENT 2 (2026-07-30 human ruling): converge on the SAME
            // project-less state the boot path itself uses when there is no
            // project (ProjectBoot.cpp's plugin_load "no game loaded" branch,
            // StageFinalize's EnsureScene()), not a second, ad hoc definition.
            //
            // Reachability (2026-07-30 review correction): with
            // switch_plugin_load now Optional (see its own push_back comment
            // above), the ONLY ways `r.ok` can be false here are
            // switch_project_open failing (Fatal -- it genuinely has nothing
            // to fall back to, per the guards above having already validated
            // the project and switch_teardown having already torn the old one
            // down) or the presenter itself requesting quit
            // (`r.quitRequested`, e.g. the window closing mid-switch). Either
            // way, switch_render_bridge/switch_plugin_load never ran (Fatal
            // failure skips dependents), so `m_plugin` is still exactly what
            // switch_teardown left it (nullopt) and the registry is still
            // exactly what that same PluginHost::Unload -> ResetRegistry left
            // it (empty) -- this fallback's own m_plugin.reset()/
            // ResetRegistry() calls below are therefore always no-ops TODAY,
            // kept as a structural invariant-guard (this block converges on
            // "project-less" regardless of HOW it was reached, not because a
            // reader has to prove which stage failed) rather than because
            // they currently do anything. switch_teardown above unconditionally
            // already closed the documents, cleared the resolver caches,
            // cleared the scene references, and reset the session to
            // Untitled -- that alone gets most of the way there. What is
            // still missing:
            //   - Runtime::CurrentProject() can still be the OUTGOING project
            //     (Runtime::OpenProject's OWN contract is "leaves ALL state
            //     untouched" on failure -- fine at boot, where "untouched"
            //     means "was already nullopt"; here it means STALE).
            //     CloseProject() is the call that makes "project-less after a
            //     failed switch" the same state as "project-less at boot"
            //     rather than a second definition (see its own header
            //     comment). It also drops the Assets content root/resolver
            //     and the Config project/plugin layers, so nothing about the
            //     torn-down project lingers there either.
            //   - The outgoing project's editor lock: `lockedRoot` still
            //     equals `outgoingRoot` here (switch_render_bridge, the only
            //     stage that ever advances it, never ran) -- named via
            //     `lockedRoot` rather than `outgoingRoot` directly so this
            //     block stays correct even if a future stage is added after
            //     the handover point.
            //   - EnsureScene(): the boot path's finalize stage calls this
            //     unconditionally, project or not, so the registry has a
            //     SceneRoot the Outliner/save walk can root at instead of
            //     silently refusing the first thing anyone does (now reached
            //     via OnProjectOpened below, same as every other call site).
            m_runtime->CloseProject();
            m_plugin.reset();
            m_runtime->ResetRegistry();
            if (!lockedRoot.empty())
                Arcane::EditorLock::Clear(lockedRoot);
            m_runtime->Loop().SetPaused(true);   // back to Edit

            // Converge on project-less (or kept-the-old-project) is complete
            // above; the tail below re-establishes it as "an open" the same
            // way a successful one would (EnsureScene/UpdateWindowTitle),
            // MINUS recording it in Open Recent -- a refused switch must never
            // reorder that list. See OnProjectOpened.
            OnProjectOpened(/*recordRecents=*/false);
            // The failed switch converged on project-less (or kept the old
            // project) -- keep the layout ini's key honest about it. Not part
            // of OnProjectOpened -- see StageFinalize's own call for why.
            RetargetLayoutIni();
        }
    }

    // ---- Build -> Rebuild Game Module (see EditorApp.hpp's Build section) ---

    void EditorApp::StartModuleRebuild()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        // The menu item is greyed for all three of these; re-checked here so
        // a future keybind or other caller cannot slip past the gates.
        if (!proj || proj->Manifest().gameModule.empty())
        {
            ARC_ERROR("Build: no open project with a game module -- nothing to rebuild");
            return;
        }
        if (InPlayMode())
        {
            ARC_ERROR("Build: refused while Play is running -- stop to rebuild");
            return;
        }
        if (m_moduleBuild.Running())
        {
            ARC_WARN("Build: a rebuild is already running");
            return;
        }

        // The RUNNING editor's SDK wins over any machine-wide ARCANE_SDK: the
        // point of the button is "rebuild against the engine you are looking
        // at", and the project's premake5.lua resolves the engine through
        // this variable (build/arcane.lua).
        const std::filesystem::path sdkRoot =
            ModuleBuild::SdkRootFromExeDir(ModuleBuild::ExeDir());
        ModuleBuild::SetSdkEnv(sdkRoot);

        ModuleBuild::ComposeInputs in;
        in.projectRoot   = proj->Root();
        in.premakeExe    = ModuleBuild::ResolvePremake(sdkRoot);
        in.msbuildExe    = ModuleBuild::ResolveMsBuild();
        in.solution      = ModuleBuild::DiscoverSolution(proj->Root());
        in.configuration = ModuleBuild::Configuration();
        if (in.solution.empty())
        {
            // Nothing generated yet (fresh clone): premake -- which always
            // runs first, every build -- is about to write it. Name it by the
            // committed convention (the workspace in a project's premake5.lua
            // is named after the project, e.g. Aphelyon.slnx); if a project
            // breaks that convention, msbuild fails loudly with the missing
            // path in the Console, which is the honest failure.
            in.solution = proj->Root() / (proj->Manifest().name + ".slnx");
        }

        m_moduleBuildRoot = proj->Root();
        const std::string cmd = ModuleBuild::ComposeRebuildCommands(in);
        ARC_INFO("Build: rebuilding {} ({}) against SDK {}",
                 proj->Manifest().gameModule, in.configuration, sdkRoot.generic_string());
        ARC_INFO("Build: {}", cmd);
        if (!m_moduleBuild.Start(cmd))
            ARC_WARN("Build: a rebuild is already running");
    }

    void EditorApp::PollModuleBuild()
    {
        for (std::string& line : m_moduleBuild.DrainLines())
        {
            // Severity COLORING only -- v1 deliberately does not parse MSVC
            // diagnostics into per-line locators (arc non-goal); these
            // contains-checks just pick the Console severity lane for the
            // raw line.
            const bool isError = line.find(": error") != std::string::npos ||
                                 line.find(": fatal") != std::string::npos ||
                                 line.rfind("Error:", 0) == 0;
            const bool isWarn  = line.find(": warning") != std::string::npos;
            if (isError)     ARC_ERROR("Build: {}", line);
            else if (isWarn) ARC_WARN("Build: {}", line);
            else             ARC_INFO("Build: {}", line);
        }

        const std::optional<int> exit = m_moduleBuild.TakeExit();
        if (!exit)
            return;

        // KEY OWNERSHIP: "build:<project root>" -- THIS finish path is the
        // only publisher, and it replaces the key's ENTIRE set every build
        // (the Diagnostics publication-group contract). The root is the one
        // the build STARTED for (m_moduleBuildRoot), so a project switch
        // mid-build cannot strand a row under a key nobody will ever clear.
        const std::string key = "build:" + m_moduleBuildRoot.generic_string();

        if (*exit != 0)
        {
            ARC_ERROR("Build: rebuild failed (exit code {})", *exit);
            Arcane::Diagnostic d;
            d.severity = Arcane::DiagSeverity::Error;
            d.scope    = Arcane::DiagScope::Plugin;
            d.code     = "build.module.failed";
            d.message  = "Rebuild Game Module failed (exit code " +
                         std::to_string(*exit) + ")";
            d.detail   = "See the Console's Build lines";
            // File locator = the project root: clicking the row is a
            // DOCUMENTED no-op (RouteLocator's File branch only matches open
            // shader documents) -- the row exists to persist the failure
            // state; the Console's Build lines carry the detail.
            d.locator  = Arcane::DiagLocator::File(m_moduleBuildRoot.generic_string());
            Arcane::Diagnostics::Publish(key, std::span<const Arcane::Diagnostic>(&d, 1));
            return;
        }

        ARC_INFO("Build: rebuild succeeded");
        Arcane::Diagnostics::Clear(key);

        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj || proj->Root() != m_moduleBuildRoot)
        {
            ARC_WARN("Build: the project changed while the build ran -- "
                     "skipping the restamp/reload half");
            return;
        }

        // The module was JUST rebuilt against this engine, so the manifest's
        // engine.abi may finally be restamped -- the one legitimate module-
        // project moment (Project::RestampEngineAbi's own contract). The
        // Hub's compatibility badge heals off this.
        if (proj->Manifest().engineAbi != static_cast<int>(Arcane::kGamePluginABIVersion))
        {
            const int oldAbi = proj->Manifest().engineAbi;
            if (m_runtime->RestampProjectEngineAbi(static_cast<int>(Arcane::kGamePluginABIVersion)))
                ARC_INFO("Build: manifest engine.abi restamped {} -> {} (module rebuilt)",
                         oldAbi, static_cast<int>(Arcane::kGamePluginABIVersion));
            else
                ARC_WARN("Build: manifest engine.abi is stale ({}) and could not be restamped",
                         oldAbi);
        }

        if (m_plugin)
        {
            // A live host needs nothing from us: PluginHost::Poll (EndFrame)
            // sees the fresh DLL mtime and hot-reloads with state after its
            // own debounce. Forcing a reload here would race that debounce.
            ARC_INFO("Build: the module watcher will hot-reload the fresh DLL");
            return;
        }

        // No host is watching: the module was REFUSED at open (stale ABI),
        // and StagePluginLoad/switch_plugin_load left the host disengaged.
        // Re-engage exactly the way StagePluginLoad does.
        const std::string gameModule =
            Arcane::HostBoot::GameModule(proj, m_config.pluginPath);
        const auto pluginModules = Arcane::HostBoot::PluginModules(proj);
        if (gameModule.empty() && pluginModules.empty())
            return;
        m_plugin.emplace(*m_runtime,
            gameModule.empty() ? std::filesystem::path{}
                               : std::filesystem::path(gameModule));
        for (const auto& dll : pluginModules)
            m_plugin->AddPlugin(dll);
        if (!m_plugin->Load())
        {
            // Same defined-state failure shape as StagePluginLoad: a
            // disengaged host plus the loud cause (Plugin.cpp's gate already
            // published plugin.abi.mismatch if that is what refused it).
            ARC_ERROR("Build: the rebuilt module still failed to load (see Problems)");
            m_plugin.reset();
            return;
        }
        ARC_INFO("Build: game module loaded");
        // The open scene was deserialized WITHOUT the module's components
        // (unknown components drop with warnings at load) -- re-open it from
        // disk so they come back now that the types exist. Clean, on-disk
        // scenes only: a reload must never discard edits. Not during Play
        // either -- a build started in Edit mode can finish after the user
        // pressed Play, and Stop's registry restore must stay authoritative.
        if (!InPlayMode() && !m_scene.Path().empty() && !m_scene.IsDirty(*m_undo))
            DoOpenScene(m_scene.Path());
    }
}
