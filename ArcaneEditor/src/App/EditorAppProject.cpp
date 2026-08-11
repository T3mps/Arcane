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

#include <Arcane/Base/Assert.hpp>   // ARC_ASSERT (SwitchProject's EditorStages cherry-pick tripwire)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (pre-teardown ABI gate)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>   // Save/LoadSpriteAsset (MintOrReuseSpriteForTexture)

#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Publish/Clear (the Build failure row)
#include <Arcane/Host/ProjectBoot.hpp>

#include <algorithm>   // std::ranges::find (SwitchProject's take() cherry-pick)
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>   // take()'s id parameter

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
    // the incoming one. EditorRecents::NoteProjectOpened (SwitchProject's
    // "plugin_load" stage, reused from EditorStages since Task 12) repopulates
    // it for whichever project ends up open -- including the project-less
    // fallback, which correctly leaves this empty.
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
        // root the moment the "render_bridge" stage hands it over below (the
        // shared EditorStages id, reused since Task 12 -- was switch_render_bridge),
        // so the failure fallback (any stage, including one AFTER that handover)
        // always releases the lock we are actually holding, never a stale one.
        const std::filesystem::path outgoingRoot =
            m_runtime->CurrentProject() ? m_runtime->CurrentProject()->Root()
                                        : std::filesystem::path{};
        std::filesystem::path lockedRoot = outgoingRoot;

        // ONE shared stage source (architecture pass sec 5). The ctx and pathStr are
        // NAMED locals -- ctx.projectPath is a c_str view and BootSequence::Run is
        // synchronous inside this scope (Amendment 1's dangling-temporary hazard is
        // why these are not inline temporaries).
        const std::string pathStr = path.string();
        Arcane::HostBoot::BootContext ctx{};
        ctx.runtime     = &*m_runtime;
        ctx.projectPath = pathStr.c_str();
        ctx.pluginPath  = m_config.pluginPath.c_str();
        ctx.moduleName  = "ArcaneEditor.exe";

        std::vector<Arcane::BootStage> all = Arcane::HostBoot::EditorStages(ctx);
        if (!PatchHostStages(all))
        {
            // Table drift -- the same fail-loud contract Create() has. Refuse the
            // switch; the session is still untouched (nothing torn down yet).
            m_modalErrors.Push("Open Project Failed",
                "Internal error: the host stage table no longer matches EditorStages() "
                "(see Console). The current session is unchanged.");
            return;
        }

        // Cherry-pick the reopen subset by id, in switch order. Boot-only stages
        // (window/GPU/fonts/shell/finalize/splash) are skipped by omission.
        auto take = [&all](std::string_view id) -> Arcane::BootStage
        {
            const auto it = std::ranges::find(all, id,
                [](const Arcane::BootStage& s) { return std::string_view(s.id); });
            ARC_ASSERT(it != all.end(), "EditorStages lost a stage the switch needs");
            return std::move(*it);
        };

        std::vector<Arcane::BootStage> stages;

        // switch_teardown stays switch-LOCAL: boot has no equivalent (nothing to
        // tear down at boot), so there is no shared body to reuse.
        {
            Arcane::BootStage teardown;
            teardown.id = "switch_teardown";
            teardown.thread = Arcane::BootThread::Main;
            teardown.policy = Arcane::BootPolicy::Fatal;
            teardown.weight = 2;
            teardown.run = [&]
            {
                ResetPerProjectState();
                m_gpu->Device().Nvrhi()->waitForIdle();
                m_plugin.reset();
                return true;
            };
            stages.push_back(std::move(teardown));
        }

        // project_open: the SHARED CoreStages body, as-is (ctx.runtime->OpenProject
        // over ctx.projectPath + scan-progress detail -- the switch overlay now
        // shows content-scan progress, which the hand-rolled body never did).
        // Policy tightened to Fatal: at boot a failed open falls back to
        // project-less startup; here the old project is already torn down, so
        // there is genuinely nothing to fall back to except the failure fallback
        // below (unchanged).
        {
            Arcane::BootStage projectOpen = take("project_open");
            projectOpen.dependsOn = { "switch_teardown" };
            projectOpen.policy    = Arcane::BootPolicy::Fatal;
            stages.push_back(std::move(projectOpen));
        }

        // render_bridge: switch-LOCAL body (the boot body builds the viewport
        // canvas/picker/outline, which already exist). The delta IS the switch:
        // hand the editor lock over and load the new project's input config.
        {
            Arcane::BootStage bridge = take("render_bridge");
            bridge.dependsOn = { "project_open" };
            bridge.run = [&]
            {
                if (!lockedRoot.empty())
                    Arcane::EditorLock::Clear(lockedRoot);
                lockedRoot.clear();
                if (const Arcane::Project* proj = m_runtime->CurrentProject())
                {
                    Arcane::EditorLock::Write(proj->Root());
                    lockedRoot = proj->Root();
                }
                if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
                    ARC_WARN("Open Project: input actions failed to load");
                return true;
            };
            stages.push_back(std::move(bridge));
        }

        // plugin_load: REUSES the boot body (StagePluginLoad -- module resolve,
        // host engage, failure banner, SetPaused are byte-identical needs), then
        // runs the shared success tail. Policy stays Optional (a failed module
        // load leaves the same safe disengaged state boot produces on purpose --
        // see the 2026-07-30 ruling in this stage's old comment). DELIBERATE
        // LOG-TEXT DELTA: the boot body's "no --project/--plugin" INFO line now
        // also serves the switch (was "no game module / plugins for this
        // project") -- ledgered, not hidden.
        {
            Arcane::BootStage plugin = take("plugin_load");
            plugin.dependsOn = { "render_bridge" };
            plugin.run = [this]
            {
                // StagePluginLoad reads m_runtime->CurrentProject() + m_config.pluginPath
                // directly and ignores its BootContext& parameter (verified against
                // EditorApp.cpp) -- so passing m_bootCtx here (the BOOT context, not a
                // switch-local one) is safe: nothing this call reads is boot-specific.
                const bool ok = StagePluginLoad(m_bootCtx);   // ignores its ctx argument
                OnProjectOpened();
                return ok;
            };
            stages.push_back(std::move(plugin));
        }

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
            // Reachability (2026-07-30 review correction; ids updated for
            // Task 12's cherry-pick-from-EditorStages unification -- this
            // stage is now literally the SAME "plugin_load"/"project_open"/
            // "render_bridge" ids EditorStages() and BootStageParityTest use,
            // not switch-local aliases, so the watchdog/hang-report phase
            // label and this fallback's own "failed at stage 'X'" banner now
            // read e.g. "project_open" instead of "switch_project_open" --
            // an accepted, ledgered delta, not a bug): with "plugin_load"
            // still Optional (inherited from EditorStages' own override --
            // see its own push_back comment above), the ONLY ways `r.ok` can
            // be false here are "project_open" failing (tightened to Fatal
            // just above -- it genuinely has nothing to fall back to, per the
            // guards above having already validated the project and
            // switch_teardown having already torn the old one down) or the
            // presenter itself requesting quit (`r.quitRequested`, e.g. the
            // window closing mid-switch). Either way, "render_bridge"/
            // "plugin_load" never ran (Fatal failure skips dependents), so
            // `m_plugin` is still exactly what switch_teardown left it
            // (nullopt) and the registry is still exactly what that same
            // PluginHost::Unload -> ResetRegistry left it (empty) -- this
            // fallback's own m_plugin.reset()/ResetRegistry() calls below are
            // therefore always no-ops TODAY, kept as a structural
            // invariant-guard (this block converges on "project-less"
            // regardless of HOW it was reached, not because a reader has to
            // prove which stage failed) rather than because they currently do
            // anything. switch_teardown above unconditionally already closed
            // the documents, cleared the resolver caches, cleared the scene
            // references, and reset the session to Untitled -- that alone
            // gets most of the way there. What is still missing:
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
            //     equals `outgoingRoot` here ("render_bridge", the only
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
        // and StagePluginLoad -- reused directly by SwitchProject's "plugin_load"
        // stage since Task 12 (was switch_plugin_load) -- left the host disengaged.
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
