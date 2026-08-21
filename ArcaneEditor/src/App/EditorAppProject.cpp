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
#include <Arcane/Render/Nri/NriDiagnostics.hpp>   // NriDiagnostics::FireFault (--crash-gpu on the graph arm)

#include <algorithm>   // std::ranges::find (SwitchProject's take() cherry-pick)
#include <cstddef>     // std::size_t (project_open's switch-local scan-progress callback)
#include <filesystem>
#include <memory>
#include <optional>    // take()'s fail-loud return (2026-08-11 review finding 3)
#include <span>
#include <string>
#include <string_view>   // take()'s id parameter

namespace Arcane::Editor
{
    // A document gets the GRAPH SEAM (nriDevice/hostConfig/chromeHud) rather
    // than a device handle of its own: one that needs a render target builds
    // its own small NriGraphContext::CreateOffscreen over the process's one
    // device.
    //
    // `backend` has no RenderDevice to ask, so it reads the config --
    // the same substitution UpdateWindowTitle and RuntimeApp::
    // StageSpriteTables make (m_config.backend is by construction the value
    // GpuContext::Create would have passed into a RenderDeviceDesc).
    Arcane::Editor::DocServices EditorApp::MakeDocServices()
    {
        Arcane::Editor::DocServices s;
        s.compiler = m_shaderCompiler.get();
        s.sources  = &m_shaderSources;
        s.runtime  = &*m_runtime;
        s.undo     = m_undo ? &*m_undo : nullptr;
        s.clock    = &m_editorClock;
        s.backend  = m_config.backend;
        if (m_graphChrome)
        {
            // THE PROCESS'S ONE DEVICE is owned by the chrome context: a
            // document's preview context BORROWS it, exactly as the viewport
            // context does, and must therefore be destroyed before the chrome
            // context is.
            //
            // THE DECLARATION ORDER THAT MAKES THAT TRUE:
            // m_graphChrome is declared FIRST (EditorApp.hpp:350) and
            // m_documents LAST (:886), with m_retiredDocPreviews (:380)
            // deliberately between them. Reverse-order destruction therefore
            // runs ~m_documents -> ~m_retiredDocPreviews -> ~m_graphChrome:
            // every borrower dies before the owner of the device it borrowed.
            //
            // BUT DESTRUCTION ORDER IS NOT WHAT ACTUALLY CLOSES THESE.
            // EditorApp::ShutdownGraphPath destroys both contexts EXPLICITLY,
            // long before any member destructor runs, so it does its own
            // CloseAll + drain first -- and a project switch owes the same
            // sequence, which is what EditorApp::TeardownGraphForSwitch is:
            // ResetPerProjectState's CloseAll retires every open
            // document's preview vehicle, and that function drains the retire
            // list inside the same stage, while the chrome context whose node
            // the drain invalidates against is still alive.
            s.nriDevice  = &m_graphChrome->Device();
            s.hostConfig = &m_config;
            // The backend that will CACHE the preview texture when
            // ImGui::Image draws it, and therefore the one owed an
            // InvalidateUserTextureNow before that texture dies. The document
            // makes that call from its destructor.
            s.chromeHud  = m_graphChrome->ImGuiHud();
            // ...and the one-frame retire, which is what makes closing a
            // document safe at all on this arm. See DocServices'
            // retireGraphPreview for why the destroy cannot happen inline.
            s.retireGraphPreview = [this](std::unique_ptr<Arcane::NriGraphContext> v)
            {
                RetireDocPreview(std::move(v));
            };
        }
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
    // m_pendingReports / m_reportDiagnostics (GPU crash diagnostics arc,
    // Task 9): m_pendingReports is a report path already queued against the
    // outgoing project by OnReportWritten -- draining it post-switch would
    // RegisterCreatedAsset it into the WRONG (incoming) project's registry.
    // m_reportDiagnostics's rows carry DiagLocator::Asset(guid) values that
    // exist only in the outgoing project's registry (m_consoleDiag.store
    // above clears the PUBLISHED "diagnostics:reports" set, but this is the
    // accumulator PollDiagnosticReports republishes THE WHOLE OF on the next
    // report -- clearing only the store would let the very next post-switch
    // report resurrect every stale row alongside it).
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

        // m_pendingReports / m_reportDiagnostics (Task 9) -- see the member-
        // rationale block above. The mutex is taken even here: OnReportWritten
        // can still push from the watchdog thread while a switch runs on the
        // main thread, so this must not be a bare `.clear()` racing that push.
        {
            std::lock_guard<std::mutex> lock(m_pendingReportsMutex);
            m_pendingReports.clear();
        }
        m_reportDiagnostics.clear();
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

        // THE OUTGOING VIEWPORT'S EXTENT, carried from "switch_teardown" (which
        // destroys the viewport context) to "render_bridge" (which builds its
        // replacement). Locals rather than members for the same reason
        // lockedRoot above is one: their lifetime is exactly this call, and
        // both stages capture by reference. 0/0 on a session whose viewport
        // context is already gone -- the rebuild reads that as "use the boot
        // default", so a lost extent costs one deferred resize (phase 8
        // re-measures the panel every frame) and never a wrong picture.
        std::uint32_t keepViewportW = 0, keepViewportH = 0;

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

        // Cherry-pick the reopen subset by id, in switch order -- looked up and
        // VALIDATED UP FRONT, before switch_teardown or any other stage is even
        // constructed, so a missing id refuses the switch with the session
        // completely untouched (2026-08-11 review finding 3): the shape mirrors
        // PatchHostStages' own table-drift refusal a few lines above (fail loud,
        // return early) instead of an ARC_ASSERT, which compiles to a Release
        // no-op (MOSAIC_ASSERT) and would walk off std::vector::end() -- unlike
        // render_bridge/plugin_load, project_open is NOT in PatchHostStages'
        // kHostStages table (its body stays CoreStages-shared, or is overridden
        // below), so it has no other guard against EditorStages() ever dropping
        // it. Boot-only stages (window/GPU/fonts/shell/finalize/splash) are
        // skipped by omission.
        auto take = [&all](std::string_view id) -> std::optional<Arcane::BootStage>
        {
            const auto it = std::ranges::find(all, id,
                [](const Arcane::BootStage& s) { return std::string_view(s.id); });
            if (it == all.end())
                return std::nullopt;
            return std::move(*it);
        };

        std::optional<Arcane::BootStage> takenProjectOpen  = take("project_open");
        std::optional<Arcane::BootStage> takenRenderBridge = take("render_bridge");
        std::optional<Arcane::BootStage> takenPluginLoad   = take("plugin_load");
        if (!takenProjectOpen || !takenRenderBridge || !takenPluginLoad)
        {
            ARC_ERROR("EditorApp::SwitchProject: EditorStages() no longer provides a stage "
                      "the switch needs (project_open/render_bridge/plugin_load) -- renamed "
                      "or removed in ProjectBoot.cpp without updating SwitchProject's "
                      "cherry-pick list");
            m_modalErrors.Push("Open Project Failed",
                "Internal error: the host stage table no longer matches EditorStages() "
                "(see Console). The current session is unchanged.");
            return;
        }

        std::vector<Arcane::BootStage> stages;

        // switch_teardown stays switch-LOCAL: boot has no equivalent (nothing to
        // tear down at boot), so there is no shared body to reuse.
        //
        // ===== THE IDLE =====================================================
        // Tearing the viewport context down is NOT a bare release but the
        // ordered idle -> invalidate -> release (whose own drain closes the
        // sequence), and it lives in EditorApp::TeardownGraphForSwitch --
        // beside ShutdownGraphPath, which is the sequence it is the sibling of
        // and the one it has to be read against. Skipping the idle would tear
        // a plugin down under a GPU still reading its resources.
        //
        // WHAT THAT FUNCTION DECIDES, stated here because this stage is the
        // site of the decision: the CHROME context and m_gameImgui are KEPT
        // (nothing they hold is project-scoped, and one of them owns the
        // process's only graphics device and the host window's swapchain),
        // while the VIEWPORT context is DESTROYED here and REBUILT by
        // "render_bridge" below. Its definition carries the whole argument,
        // including why "keep the object and flush its content" is not on the
        // menu -- Batch2DNode's write-once sprite sets name the texture
        // cache's views, so a cache flush under a live node is a fault rather
        // than a stale pixel.
        //
        // ===== THE TWO GAME-UI OBLIGATIONS, both of which are SILENT when got
        // wrong and neither of which any headless case can reach -- discharged,
        // and where:
        //
        //   (a) ORDER: the viewport context's game ImGuiNriNode ADOPTED
        //       m_gameImgui's ImGui context, and ImGuiNri::Release PINS the
        //       adopted context to walk its platform texture list -- a
        //       dereference. So a teardown that releases that context must
        //       leave m_gameImgui ALIVE across it. At process exit that is
        //       member declaration order (EditorApp.hpp); here it has to be
        //       written, because this stage destroys a graph context while
        //       every ImGui context in the process SURVIVES the switch.
        //       TeardownGraphForSwitch touches m_gameImgui NOWHERE -- said in
        //       its own header block and at that member's declaration.
        //
        //   (b) RE-ADOPT: the rebuilt viewport context's ImGuiGame() node is a
        //       NEW ImGuiNri that installed its backend flags on whatever
        //       context was current -- not the game one. Without a fresh
        //       ImGuiNriNode::AdoptImGuiContext(m_gameImgui->Context()) after
        //       the rebuild, the game context's draw lists carry no atlas for
        //       the node to upload and the plugin HUD renders as NOTHING, with
        //       no error anywhere. Discharged by the rebuild going through
        //       EditorApp::BuildGraphViewportContext -- the SAME body boot
        //       uses, which is what makes the two provably identical rather
        //       than merely similar.
        //
        // ImGuiNri::AdoptContext and its m_imguiContext member carry the full
        // statement of both.
        {
            Arcane::BootStage teardown;
            teardown.id = "switch_teardown";
            teardown.thread = Arcane::BootThread::Main;
            teardown.policy = Arcane::BootPolicy::Fatal;
            teardown.weight = 2;
            teardown.run = [&]
            {
                ResetPerProjectState();
                TeardownGraphForSwitch(keepViewportW, keepViewportH);
                m_plugin.reset();
                return true;
            };
            stages.push_back(std::move(teardown));
        }

        // project_open: id/thread/weight/detail box taken from the shared
        // CoreStages stage, but `.run` is a SWITCH-LOCAL override, NOT the
        // shared body "as-is" (2026-08-11 review finding 2): the shared body
        // (ProjectBoot.cpp's CoreStages, project_open) returns true on EVERY
        // path -- a failed open there ARC_WARNs "using data/ + --plugin
        // fallback" and still reports success, because at BOOT "the open
        // failed" correctly degrades to "stay/start project-less". That
        // fallback is nonsense mid-switch: switch_teardown above has ALREADY
        // closed the outgoing project, so with the shared body's
        // unconditional `return true` a failed open here would silently
        // "succeed" -- CurrentProject() stays the OLD project, render_bridge
        // below re-locks the OLD root, plugin_load reloads the OLD module,
        // recents re-record the OLD project, no banner ever shows, and the
        // failure fallback past seq.Run() below would be dead code. This
        // override calls OpenProject directly and returns its REAL result, so
        // `policy = Fatal` immediately below actually bites now. It still
        // reuses the SAME BootStageDetail box CoreStages attached (`.detail`,
        // carried over by take()'s move) for the "Scanning content... N / M"
        // presenter text -- the switch overlay now shows content-scan
        // progress, which the old hand-rolled body never did -- reimplementing
        // the shared body's own throttle (stride 32, plus always the first and
        // final tick) inline, since ReportScanProgress itself is anonymous-
        // namespace-private to ProjectBoot.cpp and not host-visible.
        {
            Arcane::BootStage projectOpen = std::move(*takenProjectOpen);
            projectOpen.dependsOn = { "switch_teardown" };
            projectOpen.policy    = Arcane::BootPolicy::Fatal;
            const std::shared_ptr<Arcane::BootStageDetail> scanDetail = projectOpen.detail;
            projectOpen.run = [this, &path, scanDetail]
            {
                // scanDetail is unconditionally attached by CoreStages' own
                // Make("project_open", ...) call, so this is never null in
                // practice; still guarded rather than asserted, since a null
                // detail box here is harmless (OpenProject just runs without a
                // progress callback) and not worth a hard failure.
                if (!scanDetail)
                    return m_runtime->OpenProject(path);
                return m_runtime->OpenProject(path,
                    [scanDetail](std::size_t done, std::size_t total)
                    {
                        constexpr std::size_t kStride = 32;
                        if (done != 1 && done != total && done % kStride != 0)
                            return;
                        scanDetail->Set("Scanning content... " + std::to_string(done) +
                                         " / " + std::to_string(total));
                    });
            };
            stages.push_back(std::move(projectOpen));
        }

        // render_bridge: switch-LOCAL body. It hands the editor lock over,
        // loads the new project's input config, and REBUILDS THE VIEWPORT TRIO.
        //
        // The offscreen NriGraphContext IS that trio (canvas, picker and
        // outline are NODES inside its frame -- ViewportTargets::graph);
        // "switch_teardown" above destroyed it because its caches are keyed by
        // the OUTGOING project's asset Guids, and this is the boot stage whose
        // job those three objects are. The chrome context and the game ImGui
        // context are NOT rebuilt: neither is project-scoped, and the chrome
        // one owns the device this borrows.
        {
            Arcane::BootStage bridge = std::move(*takenRenderBridge);
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

                // THE GRAPH TRIO'S REBUILD, through the SAME body boot uses --
                // which is what re-wires everything boot wired (the game
                // context's AdoptImGuiContext, the asset resolver and the
                // pixel supply) without a second copy of the list. At the
                // OUTGOING extent, so the panel does not snap back to the boot
                // default for a frame; 0/0 (nothing was there to measure)
                // means "the boot extent", which that function names.
                if (!BuildGraphViewportContext(keepViewportW, keepViewportH))
                {
                    // Already logged. Fail the stage -- plugin_load is
                    // skipped and the fallback below converges the session
                    // on project-less -- AND request the exit, because a
                    // graph session with no viewport context is not a
                    // degraded editor, it is one whose phase 10 has
                    // nothing to render into. Same code and same meaning
                    // as CreateGraphVehicles failing at boot (exit 1); the
                    // frame loop reads m_requestExit before any phase
                    // runs, and this frame's remaining phases already
                    // tolerate a null viewport context (they must: they
                    // are reached with one only here).
                    NoteGraphFrameFailure("the viewport graph context could not be rebuilt "
                                          "for the project switch");
                    return false;
                }
                return true;
            };
            stages.push_back(std::move(bridge));
        }

        // plugin_load: REUSES the boot body (StagePluginLoad -- module resolve,
        // host engage, failure banner, SetPaused are byte-identical needs), then
        // runs the shared success tail PLUS RetargetLayoutIni() (2026-08-11
        // review finding 1: OnProjectOpened() deliberately does NOT call this --
        // EditorApp.hpp's comment: every call site owns it separately -- and the
        // deleted hand-rolled switch_plugin_load stage always called it
        // immediately after OnProjectOpened(); dropping it left a successful
        // A->B switch with io.IniFilename still pointed at A's layout file, so
        // B's layout would write into A's file). Policy stays Optional (a failed
        // module load leaves the same safe disengaged state boot produces on
        // purpose -- see the 2026-07-30 ruling in this stage's old comment).
        // DELIBERATE LOG-TEXT DELTA: the boot body's "no --project/--plugin"
        // INFO line now also serves the switch (was "no game module / plugins
        // for this project") -- ledgered, not hidden.
        {
            Arcane::BootStage plugin = std::move(*takenPluginLoad);
            plugin.dependsOn = { "render_bridge" };
            plugin.run = [this]
            {
                // StagePluginLoad reads m_runtime->CurrentProject() + m_config.pluginPath
                // directly and ignores its BootContext& parameter (verified against
                // EditorApp.cpp) -- so passing m_bootCtx here (the BOOT context, not a
                // switch-local one) is safe: nothing this call reads is boot-specific.
                const bool ok = StagePluginLoad(m_bootCtx);   // ignores its ctx argument
                OnProjectOpened();
                // plugin_load is a Main-thread stage, so this ImGui-adjacent call
                // (retargets io.IniFilename) is safe here, same as StageFinalize's
                // own call immediately after its OnProjectOpened() (EditorApp.cpp).
                RetargetLayoutIni();
                // Same call-site family (GPU crash diagnostics arc, Task 8):
                // a switch that lands on a NEW project must not keep filing
                // crash/hang reports under the OLD one's Saved/Diagnostics.
                RetargetDumpDir();
                return ok;
            };
            stages.push_back(std::move(plugin));
        }

        // ===== NO PRESENTER AT ALL FOR THE SWITCH ===========================
        // `seq.Run(nullptr)`: the switch runs with no progress overlay. There
        // is no BootPresenter to construct, and the rule below is why nothing
        // replaced it -- it binds anyone who wants one here.
        //
        // A PRESENTER MUST NOT RUN HERE. TeardownGraphForSwitch evicts the
        // chrome backend's cache entry for the viewport output and then
        // destroys that output, and the rule those two lines live under is
        // that NOTHING MAY RENDER BETWEEN THEM (NriGraphContext::
        // ResizeOffscreen, clause (iii)): a frame recorded in that window
        // takes ImGuiNri's CREATE path and builds a fresh view over a texture
        // about to die. A per-stage presenter is exactly such a frame. Passing
        // null makes the whole switch one uninterrupted operation -- the
        // strongest form of that adjacency rule, not a weaker one.
        //
        // WHAT IS GIVEN UP, stated rather than discovered at a desk: no
        // progress overlay and NO WINDOW PUMP for the duration of the switch,
        // so a long content scan can leave the window briefly unresponsive and
        // `r.quitRequested` can never be set (the branch below is simply
        // unreachable -- it is a presenter that consumes a quit).
        // Pumping without presenting is NOT a middle ground and was rejected:
        // Window::PumpEvents CONSUMES the events it reads, so a bare pump
        // would swallow a resize (leaving the chrome swapchain mismatched
        // until the next one) and swallow a quit into nothing at all. An
        // overlay -- a real chrome frame per stage tick -- is possible but
        // must be built against the rule above, not around it.
        //
        // WHAT IS *NOT* GIVEN UP, and the distinction is what a desk operator
        // reading a diagnostics report needs: Diagnostics::Heartbeat() beats
        // from BootSequence's OWN worker-park loop (BootSequence.cpp:287),
        // independently of any presenter, so a briefly frozen-looking window
        // during a switch cannot escalate into a FALSE hang report. A hang
        // report raised across a switch describes a real stall.
        Arcane::BootSequence  seq(std::move(stages));
        const Arcane::BootResult r = seq.Run(nullptr);
        if (!r.ok)
        {
            // Important 1 (2026-07-31 review): a window close mid-switch is
            // NOT a failure -- BootSequence::Run reports it as quitRequested +
            // failedStage = "quit requested". Reporting that through
            // m_modalErrors would show a bogus "failed at stage 'quit
            // requested'" banner AND leave the editor running, so this branch
            // hands the exit back to the normal frame loop via m_requestExit
            // (see PumpFrameEvents).
            //
            // HOW THE QUIT REACHES HERE HAS CHANGED, and the original reasoning
            // no longer holds. This was written when Run() took a LIVE
            // presenter, whose Present() saw the OS quit event and CONSUMED it,
            // so PumpFrameEvents could never see it on a later frame -- which
            // made this branch the only thing standing between the user and a
            // second click on the X. Run() is passed nullptr now (Task 11a
            // collapsed the ternary to the arm that was always taken; the
            // presenter class itself is deleted), so no Present() runs, nothing
            // consumes the quit, and PumpFrameEvents WOULD see it next frame.
            // The branch stays because it still exits on the FIRST click
            // instead of a frame later, and still suppresses the bogus banner
            // -- but it is no longer load-bearing for event consumption. It
            // becomes so again only if some presenter is restored here, which
            // is now a design decision rather than a pending cleanup.
            // The project-less convergence below still runs unconditionally --
            // it is correct regardless of why r.ok is false, per this block's
            // own "either way" comment further down.
            if (r.quitRequested)
            {
                ARC_WARN("Open Project: the switch to '{}' was aborted by a quit at stage '{}' -- "
                         "exiting. If you did not close the window, this is a spurious quit and the "
                         "stage named above is where it landed.",
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
            // see its own push_back comment above), the ways `r.ok` can be
            // false here are "project_open" failing -- genuinely able to
            // now, per its own comment above: `.run` is a SWITCH-LOCAL
            // override that calls OpenProject directly and returns its real
            // result, unlike the shared CoreStages body it is built from
            // (which always returns true, even on a failed open) -- with
            // policy tightened to Fatal because it genuinely has nothing to
            // fall back to, per the guards above having already validated
            // the project and switch_teardown having already torn the old
            // one down. Or the presenter itself requesting quit
            // (`r.quitRequested`, e.g. the window closing mid-switch), which
            // the graph arm cannot produce at all -- it runs with no
            // presenter (see the seq.Run call above).
            //
            // AND, SINCE TASK 12, A THIRD -- graph arm only: "render_bridge"
            // failing because the viewport graph context could not be REBUILT
            // after switch_teardown destroyed it. That one differs from the
            // other two in exactly one way worth stating here: it fails AFTER
            // the editor-lock handover, so `lockedRoot` is the INCOMING root
            // rather than the outgoing one -- which is precisely why the
            // fallback releases `lockedRoot` and not `outgoingRoot` (see that
            // variable's own comment). It also requests the exit before
            // returning false, so this fallback's project-less convergence is
            // the last thing that runs before the frame loop leaves.
            //
            // Either way, when the failure is project_open's,
            // "render_bridge"/"plugin_load" never ran (Fatal failure
            // skips dependents), so
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
            //   - The editor lock WE ACTUALLY HOLD: `lockedRoot` equals
            //     `outgoingRoot` when project_open failed ("render_bridge",
            //     the only stage that advances it, never ran) and the
            //     INCOMING root when render_bridge itself failed after the
            //     handover (Task 12's graph-arm case above) -- which is
            //     exactly why this is named via `lockedRoot` rather than
            //     `outgoingRoot` directly. That foresight is now load-bearing
            //     rather than defensive: a stage failing after the handover
            //     stopped being hypothetical.
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
            // Same call-site family (GPU crash diagnostics arc, Task 8): the
            // switch-failure fallback above already closed the outgoing
            // project (Runtime::CloseProject()), so CurrentProject() is null
            // here -- this converges dumpDir back onto the exe-relative
            // default the same way it converges everything else onto the
            // project-less baseline.
            RetargetDumpDir();
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
        //
        // ARM-INDEPENDENT, AND VERIFIED AS SUCH AT TASK 12 rather than
        // reworked: the seam is Runtime::RestampProjectEngineAbi ->
        // Project::RestampEngineAbi, which rewrites `engine.abi` in the
        // .arcproj and mirrors the new value into the in-memory manifest (so
        // the guard below cannot re-fire on the next build). It touches no
        // device, no context and no ImGui, so `--nri-graph` changes nothing
        // about it -- kGamePluginABIVersion is 13 as of this task, and 13 is
        // what a graph-mode rebuild stamps.
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
            //
            // AND IT DOES NOT RACE A RECORDED GRAPH FRAME EITHER -- pinned
            // here at Task 12 because "the DLL swap must not land mid-frame"
            // is the question this line hands off, and the answer is
            // structural rather than lucky. The swap happens in
            // PluginHost::Poll at PHASE 20 (EditorAppFrame.cpp's EndFrame),
            // i.e. after phase 10 submitted the viewport frame AND phase 19
            // submitted+presented the chrome frame; nothing is recorded-but-
            // unsubmitted at that instant, and the frame's own reload pin at
            // that site carries the rest of the argument (what the GPU is
            // still reading is host-owned, never plugin-owned). THIS
            // function's own effects -- the re-engage and DoOpenScene below --
            // land at the frame TOP instead (MainLoop calls PollModuleBuild at
            // the dialog-drain point, before any render phase), which is the
            // same safe point for the same reason.
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

    // ---- Report-written notify (GPU crash diagnostics arc, Task 9) --------

    void EditorApp::OnReportWritten(const std::filesystem::path& diagPath, void* user)
    {
        // Runs on whatever thread WriteReportImpl ran on (the hang/gpu-stall
        // watchdog, or a faulting thread about to terminate) -- see
        // Diagnostics.hpp's ReportWrittenHook doc comment. Touches nothing
        // but this mutex-guarded queue; PollDiagnosticReports (main thread,
        // per frame) does the real work -- same worker/main split as
        // ModuleBuild::Runner's line queue.
        auto* self = static_cast<EditorApp*>(user);
        std::lock_guard<std::mutex> lock(self->m_pendingReportsMutex);
        self->m_pendingReports.push_back(diagPath);
    }

    void EditorApp::PollDiagnosticReports()
    {
        std::vector<std::filesystem::path> pending;
        {
            std::lock_guard<std::mutex> lock(m_pendingReportsMutex);
            pending.swap(m_pendingReports);
        }
        if (pending.empty())
            return;

        // F-7's single-asset call: the same RegisterCreatedAsset ->
        // Project::RegisterAsset -> AssetRegistry::AddFile chain
        // CreateMaterialAt/CreateInstanceAt (above) use for an asset minted
        // mid-session. A report written before any project is open (a
        // boot-stage hang) or with the diag directory outside every content
        // root warns and is skipped -- RegisterCreatedAsset already logs
        // the specific cause; this is the crash-path boundary being honest
        // rather than special-cased -- there is usually no open project by
        // the time a boot-stage report reaches here, and RegisterAsset just
        // says so.
        for (const std::filesystem::path& diagPath : pending)
        {
            const std::optional<Arcane::Guid> id =
                m_runtime ? m_runtime->RegisterCreatedAsset(diagPath) : std::nullopt;
            if (!id)
                continue;

            // KEY OWNERSHIP: "diagnostics:reports" -- accumulate (never
            // clear here) across the whole session; each report gets its
            // own row with its own Asset locator, so RouteLocator's
            // Kind::Asset branch (OpenAssetDocument) opens exactly the
            // report that was clicked -- the same "open from the Assets
            // browser" action a double-click in the Asset Browser performs.
            Arcane::Diagnostic d;
            d.severity = Arcane::DiagSeverity::Info;
            d.scope    = Arcane::DiagScope::Assets;
            d.code     = "diagnostics.report.written";
            d.message  = "Crash report written -- open from the Assets browser";
            d.detail   = diagPath.filename().generic_string();
            d.locator  = Arcane::DiagLocator::Asset(*id);
            m_reportDiagnostics.push_back(std::move(d));
        }

        if (!m_reportDiagnostics.empty())
            Arcane::Diagnostics::Publish("diagnostics:reports", m_reportDiagnostics);
    }

#if !defined(ARCANE_DIST)
    // Build -> Diagnostics -> Crash GPU (diagnostics test). Task 11: the desk
    // battery's trigger, and the ONLY thing in this arc that causes a fault
    // rather than reacting to one.
    //
    // What happens after the dispatch is deliberately NOT handled here: the
    // device dies, NRI's callback interface reports it, the RenderErrorLatch device-removed hook runs
    // ObserveDeviceRemoved, that calls Diagnostics::WriteReport("gpu-crash:
    // device removed"), the GPU-section provider fills the envelope, and the
    // `.arcdiag`/`.gpudump` pair lands in the project's Saved/Diagnostics.
    // Whether THIS process survives long enough to drain PollDiagnosticReports
    // and show the Problems row is exactly what the battery item measures --
    // so nothing here tries to help it along.
    void EditorApp::FireDeliberateGpuFault()
    {
        if (!m_gpu)
        {
            ARC_ERROR("Crash GPU: no GPU context");
            return;
        }

        // This is RuntimeFrame::RenderGraph's `--crash-gpu` block verbatim in
        // intent: the SAME data/shaders/gpu_fault.hlsl TDR loop, dispatched as
        // a one-off NRI compute submit through NriDiagnostics::FireFault, with
        // the same "injector unavailable -- nothing dispatched" ERROR text so a
        // battery item reads identically in either host. It goes out on its OWN
        // command buffer rather than nested in a frame, for the reason stated
        // there: the graph's command buffers belong to RenderGraph::Execute,
        // and a deliberate TDR must not be threaded through the very machinery
        // the crash report has to survive to describe.
        //
        // ON THE CHROME CONTEXT'S QUEUE, not the viewport's -- they share one
        // device and one graphics queue, so the choice is nominal, but the
        // chrome context is the one that ARMED the crash chain and naming it
        // keeps "who armed it" and "who faulted it" the same object. Reached
        // from the menu item AND from the scheduled --crash-gpu N block at the
        // top of MainLoop, which is how this is scriptable at all.
        if (!m_graphChrome)
        {
            ARC_ERROR("Crash GPU: the graph vehicle is not up -- nothing dispatched");
            return;
        }
        nri::Queue* const queue = m_graphChrome->Device().GraphicsQueue();
        if (!queue || !Arcane::NriDiagnostics::FireFault(m_graphChrome->Device(), *queue))
            ARC_ERROR("Crash GPU: fault injector unavailable -- nothing dispatched");
    }
#endif
}
