// EditorApp, project + asset plumbing: the Open-Project soft restart, the
// material/instance creation flows, the DocServices the shader documents are
// built from, and the ~1 Hz asset watcher (.arcmat edits; since F2b Task 12
// also texture sources + their .meta sidecars, feeding the background cook
// queue). Split out of EditorApp.cpp as a pure move.
//
// SwitchProject is called ONLY from the frame loop's top-of-frame phases or its
// deferred sceneAction (EditorAppFrame.cpp) -- never mid-render, because it
// tears down plugin/document GPU resources that this frame's already-built
// ImGui draw lists may still reference.
//
// The CreateXAt effects are NOT in that class and no longer share its rule:
// since Task 12 they run from ConsumeCreateResult, inside the ImGui pass
// (DrawModals) -- they create a file, register it and OPEN a document, which is
// exactly what ConsumeAssetPanelActions' sprite mint and the Problems panel's
// locator routing already do mid-pass. Only project/scene TEARDOWN needs the
// frame-boundary deferral.
//
// The project and material-OPEN dialogs launch through the shared
// PathPickedThunk trampoline (EditorAppFrame.cpp), which Stash()es into
// m_dialogs -- the background-thread half of that contract. The material/
// instance CREATE dialogs that used to do the same are gone: the unified
// create dialog is in-editor ImGui and returns its result synchronously.

#include "App/EditorApp.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Project/ContentDiscovery.hpp"   // F2b desk-checkpoint fix: mid-session Content/ drop discovery
#include "Project/MeshImportWave.hpp"   // F2c Task 13: embedded-texture extraction at discovery

#include <Arcane/AssetPipeline/ArtifactStore.hpp>   // SweepArtifactOrphans (F2b Task 12)
#include <Arcane/AssetPipeline/CookSession.hpp>   // IsCookPending's artifact-store oracle (2026-09-08 desk fix)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Mesh/MeshAsset.hpp>   // Save/LoadMeshAsset (MintMeshAsset)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (pre-teardown ABI gate)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>   // Save/LoadSpriteAsset (MintOrReuseSpriteForTexture)

#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Publish/Clear (the Build failure row)
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Render/Nri/NriDiagnostics.hpp>   // NriDiagnostics::FireFault (--crash-gpu on the graph arm)

#include <algorithm>   // std::ranges::find (SwitchProject's take() cherry-pick)
#include <chrono>      // Asset-manager Plan 2 Task 5: m_assetActivity's now() stamp
#include <cstddef>     // std::size_t (project_open's switch-local scan-progress callback)
#include <filesystem>
#include <memory>
#include <optional>    // take()'s fail-loud return (2026-08-11 review finding 3)
#include <span>
#include <string>
#include <string_view>   // take()'s id parameter
#include <unordered_set>   // SweepArtifactOrphans' live-guid set (F2b Task 12)
#include <utility>       // std::move (F2b Task 12's cook-diagnostics bookkeeping)
#include <vector>

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
        if (ChromeGraph())
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
            s.nriDevice  = &ChromeGraph()->Device();
            s.hostConfig = &m_config;
            // The backend that will CACHE the preview texture when
            // ImGui::Image draws it, and therefore the one owed an
            // InvalidateUserTextureNow before that texture dies. The document
            // makes that call from its destructor.
            s.chromeHud  = ChromeGraph()->ImGuiHud();
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
            // Asset-manager Task 8: the material's THUMBNAIL is now wrong too
            // -- and so is every instance's that derives from it. THIS is the
            // material-save invalidation site: every ShaderEditorDocument
            // SaveMaterialAsset call routes through onAssetSaved (its own Save
            // at :1858 and the assisted-rename rewrite at :4122), so one hook
            // here covers both rather than two calls at the document.
            InvalidateMaterialThumb(id);
            // Final fix wave (I1): the panel model's providers cached this
            // material's surface/refs, and the save may have changed both (kind
            // edited, parent re-pointed, a texture param added). The re-baseline
            // just below deliberately makes PollAssetWatch blind to our OWN
            // save, so this is the ONLY site that can dirty the model for it.
            // MarkAllDirty rather than MarkDirty(id): an INSTANCE's surface
            // resolves through its parent chain, so a saved base invalidates
            // every descendant's answer, not just its own -- and at the current
            // scale (tens of assets, one rebuild) the whole-model rebuild is
            // cheaper than the dependency walk that would narrow it.
            m_assetModel.MarkAllDirty();
            // Asset-manager Plan 2 Task 5: an in-editor save is activity too
            // -- SourceChanged, same kind PollAssetWatch's external-edit
            // branch below logs, since from the feed's perspective "this
            // material's bytes just changed" reads identically either way.
            m_assetActivity.Push({ std::chrono::steady_clock::now(), id,
                                    NameOfAsset(id),
                                    Arcane::Editor::AssetActivityKind::SourceChanged, {} });
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

    // ---- Asset-manager Plan 1 Task 8: the two thumbnail fan-outs -----------
    //
    // BOTH LIVE HERE, not in MaterialPreviewHarvester, because both are
    // questions about the ASSET GRAPH -- "who derives from this?", "who
    // references this texture?" -- and this class is the one that owns the
    // registry and the providers seam that answers them. The harvester knows
    // only Guids and pixels, which is what keeps it testable and what keeps
    // exactly one path from a guid to its outgoing refs in the editor
    // (m_assetPanelProviders.refsFor, the same seam AssetPanelModel and
    // FirstTextureRefOf already read through).

    namespace
    {
        // ONE registry walk, both fan-outs. `seedFromRefs` decides whether a
        // material is itself a seed (the texture variant asks "does it
        // reference one of these cooked guids?"; the material variant seeds
        // explicitly and passes nothing here), and the DerivesFrom edges are
        // collected in the SAME pass so the parent -> children index costs no
        // second walk. Nothing is cached between calls: a cached child index
        // would need its own invalidation, which is the exact class of bug
        // this function exists to fix one level up.
        void FanOutMaterialThumbs(
            Arcane::Editor::MaterialPreviewHarvester& thumbs,
            const Arcane::AssetRegistry& registry,
            const Arcane::Editor::AssetPanelProviders& providers,
            std::vector<Arcane::Guid> seeds,
            const std::function<bool(const std::vector<Arcane::AssetRef>&)>& seedFromRefs)
        {
            if (!providers.refsFor)
                return;
            std::unordered_map<Arcane::Guid, std::vector<Arcane::Guid>> children;
            std::unordered_set<Arcane::Guid> materials;
            for (const Arcane::Editor::AssetEntry& e : Arcane::Editor::BuildAssetEntries(registry))
            {
                if (e.kind != Arcane::Editor::AssetKind::Material)
                    continue;
                materials.insert(e.guid);
                const auto refs = providers.refsFor(e.guid);
                if (!refs)
                    continue;
                for (const Arcane::AssetRef& r : *refs)
                    if (r.kind == Arcane::AssetRefKind::DerivesFrom && r.target.IsValid())
                        children[r.target].push_back(e.guid);
                if (seedFromRefs && seedFromRefs(*refs))
                    seeds.push_back(e.guid);
            }

            // BFS down the derivation tree. `seen` doubles as the CYCLE GUARD
            // -- a cyclic parent chain is a real, already-diagnosed failure
            // shape (LoadMaterialParentChain owns the loud version of it) and
            // this sweep must not hang on one.
            std::unordered_set<Arcane::Guid> seen;
            std::vector<Arcane::Guid> frontier;
            for (const Arcane::Guid& s : seeds)
                // `materials` gates the SEED, not just the walk: onAssetSaved
                // is a general asset hook, so a caller can legitimately hand
                // this a guid that is not a material at all -- and queueing
                // one would cost a doomed LoadMaterialAsset and a WARN that
                // says nothing true. Free here (the walk already collected the
                // set), which is why it is a filter rather than a caller
                // obligation.
                if (s.IsValid() && materials.contains(s) && seen.insert(s).second)
                {
                    thumbs.Invalidate(s);
                    frontier.push_back(s);
                }
            while (!frontier.empty())
            {
                const Arcane::Guid parent = frontier.back();
                frontier.pop_back();
                const auto it = children.find(parent);
                if (it == children.end())
                    continue;
                for (const Arcane::Guid& child : it->second)
                    if (seen.insert(child).second)
                    {
                        thumbs.Invalidate(child);
                        frontier.push_back(child);
                    }
            }
        }
    }

    // Invalidate `material`'s thumbnail AND every registry material whose
    // parent chain passes through it.
    //
    // WHY THE FAN-OUT IS NOT OPTIONAL: a material INSTANCE is "a parent Guid
    // + sparse overrides" (MaterialAsset.hpp), so its picture is its base's
    // picture plus whatever it overrode -- editing the base changes the
    // instance's thumbnail while touching the instance's file not at all.
    // Nothing else would ever re-harvest it, and the persisted PNG's mtime
    // comparison cannot catch it either (that file genuinely did not change).
    // This is the identical hazard SceneRenderResolver::InvalidateMaterial
    // documents for the mesh-material cache, answered the same way: from a
    // base's Guid alone you cannot tell which instances inherit from it, so
    // walk.
    void EditorApp::InvalidateMaterialThumb(const Arcane::Guid& material)
    {
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!m_materialThumbs || !material.IsValid() || !project)
            return;
        FanOutMaterialThumbs(*m_materialThumbs, project->Registry(), m_assetPanelProviders,
                             { material }, nullptr);
    }

    // Textures finished cooking: re-harvest every material that names one of
    // them as a declared `texture` param (an AssetRefKind::References edge on
    // an .arcmat -- Assets.cpp's ListAssetReferences), plus everything derived
    // from those.
    void EditorApp::InvalidateMaterialThumbsForTextures(
        const std::vector<Arcane::Guid>& textures)
    {
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!m_materialThumbs || textures.empty() || !project)
            return;
        const std::unordered_set<Arcane::Guid> cooked(textures.begin(), textures.end());
        FanOutMaterialThumbs(*m_materialThumbs, project->Registry(), m_assetPanelProviders, {},
                             [&cooked](const std::vector<Arcane::AssetRef>& refs)
                             {
                                 for (const Arcane::AssetRef& r : refs)
                                     if (cooked.contains(r.target))
                                         return true;
                                 return false;
                             });
    }

    void EditorApp::PollAssetWatch()
    {
        if (m_editorClock < m_materialWatchNext)
            return;
        m_materialWatchNext = m_editorClock + 1.0;
        const Arcane::Project* project =
            m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return;

        // Desk-fix 2: still-settling -- ask for a pass on every tick until the
        // FIRST one completes (OnCookCompleted flips m_cookQueueSettling to
        // false; see its own comment). Unconditional, not gated on any
        // texture entry actually changing: this is what closes the settling
        // window for a project with ZERO texture sources, where the loop
        // below never finds one to trigger the C2 first-sighting branch, so
        // nothing else here would ever produce a CookResult at all.
        // CookQueue::NoteChanged() coalesces repeat calls while a pass is
        // already running (or resubmits a cheap upToDate-only pass if none
        // is), so ticking this every ~1 Hz costs nothing extra once settled.
        //
        // DELIBERATELY NOT forced any earlier than this (e.g. at
        // OnProjectOpened, during boot stages): a first cut did exactly that
        // and a desk check caught a reproducible DEVICE_LOST when the forced
        // background cook's real work (import + BC7 compression + a file
        // write) raced Main() -> CreateGraphVehicles()'s device creation.
        // PollAssetWatch only ever runs from inside the main loop's
        // PumpEditorDocuments, i.e. always after the device exists and always
        // after THIS frame's own scene render -- see OnProjectOpened's own
        // comment for the fuller account.
        if (m_cookQueueSettling && m_cookQueue)
            m_cookQueue->NoteChanged();

        // F2b desk-checkpoint fix: mid-session Content/ drop discovery. The
        // loop below can only watch what the registry ALREADY knows -- a
        // .png/.gltf/.glb dropped into Content/ after project open has no
        // registry entry (AssetRegistry::ScanContent runs exactly once, at
        // open) and is invisible to it. Own, slower gate than
        // m_materialWatchNext (~2s vs. ~1s -- see m_contentDiscoveryNext's
        // declaration for why): this step pays for a full recursive walk of
        // Content/, a different cost shape than the loop below's per-KNOWN-
        // file stat() calls. Runs BEFORE BuildAssetEntries below (not after)
        // so a discovery this tick registers the new guid in time for the
        // SAME tick's Texture/Model first-sighting branch to fire -- drop ->
        // registered -> cook-triggered lands inside ONE poll interval.
        //
        // F2c s4.1, Task 9: widened from Texture-only to Texture + Model.
        // ContentDiscovery.hpp's EnumerateContentSourceFiles/
        // DiscoverUnknownSources now take an explicit extension set
        // (generalized here from the old hardcoded ".png" scan, the
        // identical widening CookSession::EnumerateSources went through in
        // Task 8 on the pipeline side) -- kDiscoveryExtensions below is that
        // set, both kinds' extensions in one array.
        if (m_editorClock >= m_contentDiscoveryNext)
        {
            m_contentDiscoveryNext = m_editorClock + 2.0;

            static constexpr std::string_view kDiscoveryExtensions[] = {
                ".png",           // Texture
                ".gltf", ".glb",  // Model (F2c s4.1)
            };

            std::unordered_set<std::string> knownSourcePaths;
            for (const Arcane::Editor::AssetEntry& known :
                 Arcane::Editor::BuildAssetEntries(project->Registry()))
            {
                if (known.kind != Arcane::Editor::AssetKind::Texture &&
                    known.kind != Arcane::Editor::AssetKind::Model)
                    continue;
                if (const auto p = project->ResolveAsset(Arcane::AssetId::FromGuid(known.guid)))
                    knownSourcePaths.insert(p->generic_string());
            }

            // F2c Task 13 (s5.5, A4): a newly-discovered .gltf/.glb has its embedded
            // textures extracted to loose .png siblings BEFORE any registration --
            // deliberately its own pass, ahead of the registration loop below, so the
            // extracted .pngs are already sitting on disk when THAT loop's own
            // DiscoverUnknownSources call sweeps the folder. That is what lets the
            // model AND its textures both register inside this SAME poll interval,
            // the identical one-interval property this block's header comment already
            // claims for a plain drop -- waiting for the NEXT tick's sweep to notice
            // the .pngs would make a model's own textures lag its own registration by
            // a full m_contentDiscoveryNext gate.
            auto isModelExtension = [](const std::filesystem::path& p)
            {
                std::string ext = p.extension().string();
                for (char& c : ext)
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                return ext == ".gltf" || ext == ".glb";
            };
            for (const std::filesystem::path& dropped :
                 Arcane::Editor::DiscoverUnknownSources(project->Root() / "Content",
                                                         kDiscoveryExtensions, knownSourcePaths))
            {
                if (!isModelExtension(dropped))
                    continue;
                const std::vector<std::filesystem::path> extracted =
                    Arcane::Editor::ExtractEmbeddedTextures(dropped);
                if (!extracted.empty())
                    ARC_INFO("Assets: extracted {} embedded texture(s) from '{}'",
                             extracted.size(), dropped.filename().string());
            }

            for (const std::filesystem::path& dropped :
                 Arcane::Editor::DiscoverUnknownSources(project->Root() / "Content",
                                                         kDiscoveryExtensions, knownSourcePaths))
            {
                ARC_INFO("Assets: discovered new content file '{}' -- registering",
                         dropped.generic_string());
                // Asset-manager Plan 2 Task 5: RegisterCreatedAsset's return
                // was discarded above (the MarkAllDirty rebuild below covers
                // the model either way) -- captured here ONLY so the
                // activity feed can carry the guid it just minted.
                const std::optional<Arcane::Guid> droppedId =
                    m_runtime->RegisterCreatedAsset(dropped);
                // A new registry entry changes folder grouping (a brand-new
                // folder, or an existing group's count) -- MarkDirty(guid)
                // alone would miss that, so the whole model rebuilds.
                m_assetModel.MarkAllDirty();
                if (droppedId)
                    m_assetActivity.Push({ std::chrono::steady_clock::now(), *droppedId,
                                            dropped.filename().string(),
                                            Arcane::Editor::AssetActivityKind::Created, {} });
            }
        }

        for (const Arcane::Editor::AssetEntry& e :
             Arcane::Editor::BuildAssetEntries(project->Registry()))
        {
            if (e.kind == Arcane::Editor::AssetKind::Material)
            {
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
                // The panel model's cached surface/refs (isInstance, fold
                // target, ...) may have just changed underneath it -- ask
                // the providers again next rebuild.
                //
                // Final fix wave (I1): WIDENED from MarkDirty(e.guid) to the
                // whole model, for the same reason as onAssetSaved's mark (see
                // its comment) -- an instance's surface resolves THROUGH this
                // material's parent chain, so an edit here changes what every
                // descendant answers, and a per-guid mark leaves those rows
                // showing the old subkind until something unrelated dirties
                // them. Replaced rather than augmented: MarkAllDirty subsumes
                // the per-guid mark, and keeping both would just read as though
                // this guid needed something the others don't.
                m_assetModel.MarkAllDirty();
                // Asset-manager Plan 2 Task 5: an external .arcmat edit is
                // exactly the activity feed's SourceChanged case.
                m_assetActivity.Push({ std::chrono::steady_clock::now(), e.guid, e.name,
                                        Arcane::Editor::AssetActivityKind::SourceChanged, {} });
                if (m_resolver)
                    m_resolver->InvalidateMaterial(e.guid);
                // Asset-manager Task 8: an external .arcmat edit changes what
                // the material LOOKS like, so its thumbnail (and every
                // instance's below it) is re-harvested.
                InvalidateMaterialThumb(e.guid);
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
                continue;
            }

            // F2c s4.1, Task 9: widened from Texture-only to Texture + Model --
            // a re-exported .glb/.gltf triggers a recook the same way a
            // re-saved .png does, including the C2 first-sighting-counts-as-
            // change rule just below (which is what heals an uncooked clone).
            if (e.kind != Arcane::Editor::AssetKind::Texture &&
                e.kind != Arcane::Editor::AssetKind::Model)
                continue;

            // F2b Task 12: a Texture/Model source AND its .meta sidecar are
            // BOTH watched -- a hand-edited or inspector-written .meta (the
            // four cook-setting knobs, textures today) is a cook trigger
            // exactly like editing the pixels/geometry themselves (spec s7
            // as amended). Deliberately no self-save re-baseline for the
            // .meta half the way the material branch above has one: the
            // editor's own inspector write to a .meta IS a legitimate cook
            // trigger, not a false-positive reload to suppress -- there is
            // no "our own edit, ignore it" case for a source setting the way
            // there is for a material's in-memory document state.
            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
            if (!path)
                continue;
            std::filesystem::path metaPath = *path;
            metaPath += ".meta";

            bool changed = false;
            for (const std::filesystem::path& watched : { *path, metaPath })
            {
                std::error_code ec;
                const auto mtime = std::filesystem::last_write_time(watched, ec);
                if (ec)
                    continue;   // e.g. no .meta sidecar yet -- not an error
                const auto [it, inserted] =
                    m_materialMtimes.try_emplace(watched.generic_string(), mtime);
                if (inserted)
                {
                    // C2 FIX (final-review wave, 2026-09-04; widened to Model by F2c
                    // s4.1, Task 9): for a Texture/Model entry, first sighting COUNTS
                    // as a change -- it does NOT for a material (the branch above),
                    // because a material's first sighting really is a baseline
                    // (nothing is ever "cooked" for a material). A Texture/Model's
                    // first sighting can be a source this project has NEVER cooked (a
                    // fresh clone with no Intermediate/Artifacts yet, or a .png/.gltf/
                    // .glb just dropped into Content/ mid-session) -- treating that as
                    // "nothing happened" left "the editor heals it in-process on open"
                    // a dead path: nothing ever called CookQueue::NoteChanged() for it,
                    // so an uncooked project showed checkerboards (or a missing mesh)
                    // forever. This makes the very first watcher tick after open
                    // coalesce into ONE hash-gated CookProject pass (CookQueue's own
                    // coalescing, see CookQueue.hpp) -- free (upToDate, zero actual
                    // cooks) on an already-fully-cooked project, the heal on one that
                    // isn't.
                    it->second = mtime;
                    changed = true;
                    continue;
                }
                if (it->second == mtime)
                    continue;   // unchanged since the last tick -- not an event
                it->second = mtime;
                changed = true;
            }

            if (changed)
            {
                // A fresh source/.meta means a fresh cook-state answer once
                // the (possibly async) recook lands -- and .meta's four
                // cook-setting knobs can flip References/DerivesFrom-adjacent
                // classification too, so the whole entry is re-asked, not
                // just its cook state.
                m_assetModel.MarkDirty(e.guid);
                // Asset-manager Plan 2 Task 5: a Texture/Model source or
                // .meta sidecar changing on disk is SourceChanged too, same
                // kind as the material branch above -- the feed does not
                // distinguish "will recook" from "already re-baked".
                m_assetActivity.Push({ std::chrono::steady_clock::now(), e.guid, e.name,
                                        Arcane::Editor::AssetActivityKind::SourceChanged, {} });
            }

            if (changed && m_cookQueue)
            {
                ARC_INFO("{} '{}' (or its .meta) changed on disk",
                         e.kind == Arcane::Editor::AssetKind::Model ? "model" : "texture",
                         e.name);
                // Watcher-triggered, hash-decided, NEVER BLOCKS: NoteChanged
                // only submits a background CookSession::CookProject pass
                // (JobSystem::Submit) and returns immediately -- the actual
                // hash-gate decision (is anything really stale) and the
                // import both happen on the worker. PollCookQueue (called
                // every frame, PumpEditorDocuments) delivers the result.
                m_cookQueue->NoteChanged();
            }
        }
    }

    // ---- Background texture cook (F2b Task 12) -----------------------------

    void EditorApp::PollCookQueue()
    {
        if (m_cookQueue)
            m_cookQueue->Pump();   // -> OnCookCompleted, once per finished pass
    }

    void EditorApp::OnCookCompleted(const Arcane::AssetPipeline::CookResult& result)
    {
        // Desk-fix 2: the FIRST CookProject pass for this project has now
        // finished (this callback fires once per finished pass, from inside
        // Pump() -- see this class's own threading contract at
        // m_cookDiagnostics' declaration). Unconditional and idempotent: once
        // this flips false it stays false until the next OnProjectOpened
        // re-arms it, so every LATER pass's completion is a harmless no-op
        // write here. This is what closes the settling window even for a
        // project with ZERO texture sources -- PollAssetWatch's own forced
        // NoteChanged() call (gated on m_cookQueueSettling, unconditional on
        // any texture entry actually changing -- see its own comment)
        // guarantees SOME pass, possibly a trivial nothing-to-cook one,
        // always reaches this function.
        m_cookQueueSettling = false;

        bool diagnosticsChanged = false;

        // Freshly cooked guids: the un-latch. Assets' own memo FIRST
        // (InvalidateArtifact), then the render-side caches that separately
        // memoize their own view of the same guid downstream of it -- so a
        // Resolve()/ResolveMeshAlbedoSlot call the very next render phase
        // sees the FRESH artifact rather than replaying a stale memo.
        if (m_runtime)
        {
            for (const Arcane::Guid& guid : result.cookedGuids)
            {
                m_runtime->AssetsFacade().InvalidateArtifact(guid);
                if (m_viewportTargets.graph)
                {
                    m_viewportTargets.graph->InvalidateContentTexture(guid);
                    m_viewportTargets.graph->InvalidateMeshAlbedoSlot(guid);
                }
                // I2 fix (final-review wave, 2026-09-04): the Inspector's texture preview
                // (EditorApp.cpp's `resolveTexturePreview` service) reads through
                // ChromeGraph()'s OWN texture cache in Display colour space -- a SEPARATE
                // NriGraphContext from m_viewportTargets.graph above -- so invalidating
                // only the viewport left a (guid, Display) memo in the chrome cache stuck
                // until app exit even after a fresh cook landed: the preview never
                // updated. NriTextureCache::Invalidate already sweeps BOTH colour spaces
                // for a guid, so one more call here closes exactly the entry the preview
                // reads.
                if (Arcane::NriGraphContext* chrome = ChromeGraph())
                    chrome->InvalidateContentTexture(guid);
                // A guid that just cooked successfully is fixed now, even if
                // it previously had a refusal/failure row (a `.meta` edit
                // that corrects a bad setting, or a source re-saved after a
                // corrupt drop) -- drop its Problems-pane row.
                if (m_cookDiagnostics.erase(guid) > 0)
                    diagnosticsChanged = true;
                // The panel model's cook-state answer for this guid may have
                // just changed (Queued -> Cooked, or a stale Refused just
                // erased above) -- ask again next rebuild. (Subsumed by the
                // MarkAllDirty below whenever this batch is non-empty, which
                // it is inside this loop; kept because it states the per-guid
                // fact this loop is responsible for, and because a MarkDirty
                // after a MarkAllDirty is a no-op by the model's own contract
                // -- not the other way round.)
                m_assetModel.MarkDirty(guid);
                // Asset-manager Plan 2 Task 5: only a guid is in hand here
                // (no AssetEntry with a name already resolved), so this
                // goes through the shared NameOfAsset resolver.
                m_assetActivity.Push({ std::chrono::steady_clock::now(), guid, NameOfAsset(guid),
                                        Arcane::Editor::AssetActivityKind::Cooked, {} });
            }
            // Asset-manager Task 8: a MATERIAL that names one of these
            // textures as a declared param has been rendering the pre-cook
            // stand-in in its thumbnail until this moment -- re-harvest it,
            // and everything derived from it. OUTSIDE the loop, once per
            // BATCH: each call is a registry walk, and a first-cook pass over
            // a fresh clone reports every texture in the project at once.
            InvalidateMaterialThumbsForTextures(result.cookedGuids);

            // REVIEW FIX (round 1, IMPORTANT 2): the per-guid marks above name
            // only the TEXTURES that cooked -- but a SPRITE's cook state is
            // derived from its texture's artifact (IsCookPending's own Sprite
            // branch), and nothing dirties a dependent sprite when its texture
            // lands (the model's cascade fires for REMOVED guids only). Left
            // per-guid, a sprite stuck in Queued on an uncooked tree, or after
            // a Recook, would stay Queued until some unrelated MarkAllDirty
            // happened by -- the exact symptom this arc's desk fix removed,
            // displaced one hop down the dependency edge.
            //
            // MarkAllDirty on a non-empty batch, the SAME subsumption trade
            // the material-save seam already makes and justifies (onAssetSaved,
            // this file: a derived answer invalidates more than its own guid,
            // and at current scale the whole-model rebuild is cheaper than the
            // dependency walk that would narrow it). Bounded by the batch: a
            // pass that cooked nothing marks nothing, so the steady state --
            // PollAssetWatch's periodic trivial passes -- costs no rebuilds.
            if (!result.cookedGuids.empty())
                m_assetModel.MarkAllDirty();
        }

        // Cook FAILURES -- CookSession's own vocabulary (corrupt source /
        // disk write error), distinct from Assets' artifact-refusal
        // vocabulary (OnArtifactRefused below), but the SAME Problems pane
        // per the brief ("alongside cook failures"). PERMANENT: none of
        // these resolve on their own -- the source needs a real fix.
        //
        // DELIBERATE LAST-KNOWN-GOOD RULING (controller, post-review): a
        // failing guid is NEVER passed to InvalidateArtifact/
        // InvalidateContentTexture/InvalidateMeshAlbedoSlot above -- only
        // result.cookedGuids (successes) is. So a texture that was already
        // Resident and gets edited into a now-undecodable/failing state
        // KEEPS RENDERING ITS OLD, STILL-GOOD ARTIFACT: the Problems-pane
        // row below is the only signal, the viewport is not nuked to a
        // checkerboard/white-texel mid-iteration. This is the editor's
        // posture, NOT the runtime's -- ArcaneRuntime refuses hard on any
        // artifact problem (Task 6/8's exit-nonzero contract) because a
        // shipped build has no "keep iterating" use case. The editor's
        // is the opposite: an artist mid-edit on a source that transiently
        // fails to decode should not lose their last-good preview, matching
        // the "serve stale until recooked" precedent other engines use for
        // exactly this workflow. If this ever needs to change, it is a
        // policy decision (probably a new invalidate-on-failure toggle),
        // not a bug fix.
        for (const auto& [guid, reason] : result.failures)
        {
            Arcane::Diagnostic d;
            d.severity = Arcane::DiagSeverity::Error;
            d.scope    = Arcane::DiagScope::Assets;
            d.code     = "assets.cook.failed";
            d.message  = "Texture cook failed";
            d.detail   = reason;
            d.locator  = Arcane::DiagLocator::Asset(guid);
            m_cookDiagnostics[guid] = CookDiagRow{ std::move(d), /*permanent=*/true };
            diagnosticsChanged = true;
            // Controller ruling (ledgered plan defect, brief omitted this
            // loop): a cook FAILURE is a permanent refusal exactly like
            // OnArtifactRefused's HashMismatch/VersionNewerThanEngine rows --
            // "cook refusals stay loud" means the panel model must not wait
            // for an unrelated event to notice. Ask again next rebuild.
            m_assetModel.MarkDirty(guid);
            // Asset-manager Plan 2 Task 5: CookRefused, same Problems-pane
            // twin the file header above already documents -- `reason` is
            // CookSession's own failure string.
            m_assetActivity.Push({ std::chrono::steady_clock::now(), guid, NameOfAsset(guid),
                                    Arcane::Editor::AssetActivityKind::CookRefused, reason });
        }

        if (diagnosticsChanged)
            PublishCookDiagnostics();
    }

    void EditorApp::OnArtifactRefused(const Arcane::Guid& id, const char* kind, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!self)
            return;

        Arcane::Diagnostic d;
        d.severity = Arcane::DiagSeverity::Error;
        d.scope    = Arcane::DiagScope::Assets;
        d.code     = "assets.artifact.refused";
        d.message  = std::string("Content artifact refused (") + kind + ")";
        d.detail   = id.ToString();
        d.locator  = Arcane::DiagLocator::Asset(id);

        // ArtifactMissing is presumed still-cooking (a fresh drop, or a cook
        // that just hasn't run yet), which is why this row is permanent=false:
        // it makes IsCookPending answer "pending" WITHOUT consulting the
        // artifact store at all (the 2026-09-08 desk fix made row-ABSENCE the
        // case that asks the store; a present transient row still short-
        // circuits to pending, and a Missing resolution is exactly the
        // condition the store would confirm anyway). HashMismatch/
        // VersionNewerThanEngine are permanent: neither resolves without a
        // user fixing the source, so the cook-pending oracle must say "no,
        // this is refused" the moment either fires -- see IsCookPending.
        const bool permanent = (std::string_view(kind) != "ArtifactMissing");
        self->m_cookDiagnostics[id] = CookDiagRow{ std::move(d), permanent };
        self->PublishCookDiagnostics();
        // Controller ruling (ledgered plan defect, brief omitted this site):
        // this fires from scene resolution / NriTextureCache::Resolve --
        // main-thread-only, same contract m_cookDiagnostics itself documents
        // -- so it's safe to mark directly. Whether this particular row is
        // permanent or not (ArtifactMissing vs. Hash/Version), the model's
        // cook-state answer for `id` may have just changed; ask again next
        // rebuild rather than waiting on an unrelated event to notice.
        self->m_assetModel.MarkDirty(id);
        // Asset-manager Plan 2 Task 5: CookRefused -- detail carries the
        // refusal KIND string (ArtifactMissing/HashMismatch/
        // VersionNewerThanEngine), distinct from OnCookCompleted's failure
        // `reason` text but the same Problems-pane vocabulary either way.
        self->m_assetActivity.Push({ std::chrono::steady_clock::now(), id, self->NameOfAsset(id),
                                      Arcane::Editor::AssetActivityKind::CookRefused, kind });
    }

    // DESK-PASS FIX (asset-manager Plan 2, 2026-09-08, user-found): the
    // Assets panel showed "2 awaiting cook" FOREVER on ReferenceProject --
    // its two cooking-kind assets (uv_marker.png, uv_marker.arcsprite).
    // Root cause was this function's old no-row answer, "presume pending":
    // m_cookDiagnostics records FAILURES and REFUSALS only (see
    // CookDiagRow's own declaration) and a cook SUCCESS ERASES the row, so
    // a HEALTHY asset never has one -- nothing could ever transition it to
    // Cooked, and every successful re-cook dropped it straight back into
    // Queued. The tile, the meter and the Status-lens cards were all wrong
    // in the same direction, permanently.
    //
    // The honest answer for a guid with no row is a question for the
    // ARTIFACT STORE, not a presumption: CookSession::
    // ResolveCurrentArtifactPath recomputes TODAY's cook key from the
    // source's CURRENT bytes + settings + importer version and answers only
    // if a file already sits at that key's path -- the exact same staleness
    // test CookProject/CheckProject use, so this function and the cook agree
    // on "cooked" by construction. Resolves -> NOT pending (Cooked); does
    // not resolve (uncooked, or stale after a source/.meta edit that moved
    // the key) -> pending (Queued).
    //
    // NOT through m_runtime->AssetsFacade().ArtifactFor(): the facade's own
    // Missing branch consults QuietlyPending -> the installed
    // SetCookPendingProbe (EditorApp.cpp's OnProjectOpened), which is an
    // editor closure in the same object -- routing a cook-pending answer
    // back through the facade would ask the cook-pending seam to answer
    // itself. A local CookSession is the read-only, side-effect-free oracle
    // that owes nothing to either seam. It is also deliberately NOT
    // m_cookQueue's own session: that one is touched ONLY from inside a
    // worker job (CookQueue.hpp's threading contract), and this runs on the
    // main thread.
    //
    // SPRITES DERIVE THEIR ANSWER FROM THEIR TEXTURE (pre-ruled, and
    // verified against the pipeline: CookSession::EnumerateTextureSources
    // takes ".png with a .meta sidecar" and nothing else, and
    // ReferenceProject/Intermediate/Artifacts holds exactly ONE .arcart for
    // its one .png -- a sprite has no artifact of its own, it renders
    // through its texture's). FirstTextureRefOf is the editor's single
    // guid -> outgoing-refs path (see its own declaration); a sprite whose
    // texture ref cannot resolve keeps the old presume-pending default,
    // since nothing here can prove otherwise.
    //
    // `kind` IS A REQUIRED ARGUMENT, not a lookup this does for itself
    // (review round 1, IMPORTANT 1): the caller already knows the kind, and
    // the store ask below is the expensive half of this function -- one
    // Content/ walk that stats a .meta per .png and JSON-parses each one
    // hunting a guid. Before the gate, cookStateFor evaluated this EAGERLY
    // for EVERY asset and CookStateOf discarded the answer for non-cooking
    // kinds AFTER the work was done, which made a MarkAllDirty rebuild
    // O(assets x content-tree-walk) file I/O -- invisible on ReferenceProject,
    // a multi-second hitch on a real project. Taking the kind also drops the
    // duplicate registry Resolve this used to do for the sprite test.
    //
    // COST, now that the gate is real: one Content/ enumeration + a hash of
    // the matching source's bytes per ask, and asks happen once per
    // COOKING-KIND entry (Texture/Sprite only) per model REBUILD
    // (AssetPanelModel is invalidation-driven -- MarkDirty/MarkAllDirty --
    // not per-frame), plus the render oracle's own already-throttled
    // PendingCook re-poll. Fine at current project scale; if a cold rebuild
    // over a large Content/ ever shows up in a frame trace, memoize per
    // (guid, source mtime) rather than making this answer any less honest.
    bool EditorApp::IsCookPending(const Arcane::Guid& id, Arcane::Editor::AssetKind kind) const
    {
        // Row present: UNCHANGED, and kind-independent. A permanent row
        // (HashMismatch/VersionNewerThanEngine/a cook failure) is not
        // pending -- it is refused, and CookStateOf checks
        // HasPermanentCookDiag first anyway; a transient ArtifactMissing row
        // is still-cooking by definition.
        const auto it = m_cookDiagnostics.find(id);
        if (it != m_cookDiagnostics.end())
            return !it->second.permanent;

        // THE GATE: only Texture/Sprite have a cook pipeline at all
        // (CookStateOf's own kind list, and CookSession's enumeration is the
        // authority behind it), so for every other kind there is nothing to
        // be pending about and nothing worth walking Content/ to discover.
        // CookStateOf discards this answer for those kinds regardless; false
        // is simply the honest value to discard.
        const bool cooks = (kind == Arcane::Editor::AssetKind::Texture)
                        || (kind == Arcane::Editor::AssetKind::Sprite);
        if (!cooks)
            return false;

        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return true;   // no project in hand -- the old presume-pending default

        Arcane::Guid cookGuid = id;
        if (kind == Arcane::Editor::AssetKind::Sprite)
        {
            // HONEST COST NOTE (review round 1, IMPORTANT 3): refsFor is
            // PARSE-ON-CALL (Assets.hpp's own contract), so a sprite costs a
            // SECOND read+parse of its .arcsprite here, beside the one
            // AssetPanelModel::RebuildIfDirty already did for the same guid
            // in the same rebuild. Accepted rather than memoized: post-gate
            // this is sprites only, a small population, and one parse of a
            // tiny JSON file. Trigger for revisiting: if sprite counts reach
            // the hundreds, memoize per (guid, source mtime) -- the same
            // trigger the store ask above carries -- or thread the model's
            // already-fetched refs through the provider seam.
            cookGuid = FirstTextureRefOf(id);
            if (!cookGuid.IsValid())
                return true;   // no resolvable texture -- presume pending
        }

        Arcane::AssetPipeline::CookSession oracle;
        return !oracle.ResolveCurrentArtifactPath(project->Root(), cookGuid).has_value();
    }

    // Asset-manager redesign, Plan 1 Task 5: the OTHER reading of
    // m_cookDiagnostics -- an ABSENT row here means "no refusal", full stop.
    // (IsCookPending above no longer answers an absent row from this map at
    // all: since the 2026-09-08 desk fix it asks the artifact store instead.)
    // CookStateOf's `permanentDiag` parameter is exactly this question.
    bool EditorApp::HasPermanentCookDiag(const Arcane::Guid& id) const
    {
        const auto it = m_cookDiagnostics.find(id);
        return it != m_cookDiagnostics.end() && it->second.permanent;
    }

    // Asset-manager redesign, Plan 2 Task 5: m_assetActivity's guid->name
    // resolver for the seams that only ever have a guid in hand (cook
    // completion/failure, artifact refusal) -- see this method's own
    // declaration (EditorApp.hpp) for why it is a named member rather than
    // an inline lambda at each call site. project->Registry().Resolve(g)
    // returns the registry's mount path ("game://materials/glow.arcmat");
    // this takes only the filename, matching AssetPanelModel's own `name`
    // derivation (AssetPanelModel.hpp's BuildAssetEntries). Empty string,
    // never a placeholder, when there is no current project or the guid
    // resolves to nothing -- the activity feed's own fallback (Task 8) is
    // to print the guid itself when `name` is empty.
    std::string EditorApp::NameOfAsset(const Arcane::Guid& guid) const
    {
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return {};
        const auto mountPath = project->Registry().Resolve(guid);
        if (!mountPath)
            return {};
        return std::filesystem::path(*mountPath).filename().string();
    }

    // Builds the three facade-backed callables AssetPanelModel is driven
    // through -- called once per project open (OnProjectOpened) and cached
    // in m_assetPanelProviders; the model itself never touches the facade
    // directly (AssetPanelModel.hpp's own header comment).
    Arcane::Editor::AssetPanelProviders EditorApp::MakeAssetPanelProviders()
    {
        Arcane::Editor::AssetPanelProviders p;
        p.surfaceFor = [this](const Arcane::Guid& g) -> std::optional<Arcane::MaterialSurface>
        {
            return m_runtime ? m_runtime->AssetsFacade().MaterialSurfaceFor(g) : std::nullopt;
        };
        p.refsFor = [this](const Arcane::Guid& g) -> std::optional<std::vector<Arcane::AssetRef>>
        {
            return m_runtime ? m_runtime->AssetsFacade().ListAssetReferences(g) : std::nullopt;
        };
        p.cookStateFor = [this](const Arcane::Guid& g) -> Arcane::Editor::CookState
        {
            // CookStateOf needs the ASSET KIND (only Texture/Sprite cook) --
            // a single O(1) Resolve() against the open project's registry,
            // not the O(n) BuildAssetEntries sweep PollAssetWatch uses (that
            // one needs every entry every tick; this needs one guid's kind).
            const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
            Arcane::Editor::AssetKind kind = Arcane::Editor::AssetKind::Other;
            if (project)
                if (const auto mountPath = project->Registry().Resolve(g))
                    kind = Arcane::Editor::AssetKindOf(*mountPath);
            // `kind` is handed to IsCookPending as well as CookStateOf
            // (review round 1, IMPORTANT 1): CookStateOf's kind gate discards
            // the pending answer for a non-cooking kind, but only AFTER it is
            // computed -- and computing it walks Content/. The oracle needs
            // the same gate on its own side, not a filter downstream of it.
            return Arcane::Editor::CookStateOf(kind, HasPermanentCookDiag(g), IsCookPending(g, kind));
        };
        return p;
    }

    void EditorApp::PublishCookDiagnostics()
    {
        // KEY OWNERSHIP: "diagnostics:cook" -- this is the ONLY publisher,
        // and it republishes m_cookDiagnostics' ENTIRE current contents
        // every time (the Diagnostics publication-group contract), so an
        // erased row (a guid that cooked successfully) actually disappears
        // from the Problems pane rather than lingering until some unrelated
        // later publish happens to overwrite it.
        std::vector<Arcane::Diagnostic> rows;
        rows.reserve(m_cookDiagnostics.size());
        for (const auto& [guid, row] : m_cookDiagnostics)
            rows.push_back(row.diagnostic);
        Arcane::Diagnostics::Publish("diagnostics:cook", rows);
    }

    void EditorApp::SweepArtifactOrphans()
    {
        // Task 5 deferral, closed here: a `.meta` settings edit leaves the
        // OLD artifact on disk under its OLD cook key forever until
        // something sweeps it. That specific case is NOT what this function
        // fixes (SweepOrphans only drops artifacts for a guid ABSENT from
        // the live set entirely -- see its own header comment); this is
        // garbage collection for a source REMOVED from the project, run
        // once at project open, not on every cook pass.
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return;

        std::unordered_set<Arcane::Guid> liveGuids;
        for (const auto& [guid, mountPath] : project->Registry().All())
            liveGuids.insert(guid);

        Arcane::AssetPipeline::ArtifactStore store(project->Root() / "Intermediate");
        // The in-memory index starts EMPTY every call (a fresh ArtifactStore
        // here) -- SweepOrphans' own header comment: "Call RebuildIndexFromScan
        // first for a sweep grounded in the current disk state."
        store.RebuildIndexFromScan();
        const std::size_t removed = store.SweepOrphans(liveGuids);
        if (removed > 0)
            ARC_INFO("Assets: swept {} orphaned artifact(s) at project open", removed);
    }

    Arcane::Guid EditorApp::CreateInstanceAt(std::filesystem::path path, Arcane::Guid parent)
    {
        if (!parent.IsValid())
            return {};
        if (path.extension() != ".arcmat")
            path += ".arcmat";

        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.parent = parent;
        data.name = path.stem().string();
        if (!Arcane::SaveMaterialAsset(path, data))
        {
            ARC_WARN("Arcane Editor: could not create instance at '{}'", path.generic_string());
            return {};
        }
        // Register immediately -- ResolveParentChain on the new document needs the
        // registry to know BOTH this instance and its parent right now.
        const auto registered = m_runtime->RegisterCreatedAsset(path);
        if (!registered)
            return {};
        // TASK 5 CARRY-FORWARD, PAID HERE: the four other mint sites already
        // dirtied the panel model on a successful register, and this one did
        // not -- so a created instance stayed invisible in the Browse lens
        // until some unrelated event happened to dirty the model. A new
        // registry entry changes folder grouping (PollAssetWatch's own
        // drop-discovery comment), so it is MarkAllDirty, not MarkDirty.
        m_assetModel.MarkAllDirty();
        m_documents.OpenPath(path);
        return *registered;
    }

    // Reuse-or-mint policy (sprite-asset spec, Section 3): exactly one existing
    // .arcsprite referencing this texture -> reuse it; zero or several -> mint a
    // fresh sibling (never guess among duplicates).
    //
    // WHY the linear scan: this loads every registered .arcsprite off disk on
    // EACH call to find matches by `texture`, same shape as PollAssetWatch's
    // sweep above -- registries are small today (dozens, not thousands, of
    // sprite assets per project), so a per-call scan is the simplest correct
    // thing. It would need a texture->sprites index (built once, invalidated on
    // sprite save/delete like m_materialMtimes) if per-project sprite counts
    // grow large enough for this to show up as a hitch; that index does not
    // exist yet and is not built here.
    Arcane::Guid EditorApp::MintOrReuseSpriteForTexture(const Arcane::Guid& textureGuid,
                                                        const std::filesystem::path* target)
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

        // Fresh-mint branch. Task 13: a caller-supplied `target` (the dialog's
        // already-unique Name+Location) replaces the auto-placed-sibling
        // default; a null one (the quick "Create Sprite" row/context action's
        // call) keeps the original placement, "-N" loop included.
        std::filesystem::path mintPath;
        if (target)
        {
            mintPath = *target;
        }
        else
        {
            const auto texPath = project->ResolveAsset(Arcane::AssetId::FromGuid(textureGuid));
            if (!texPath)
            {
                ARC_WARN("Arcane Editor: could not mint a sprite -- texture '{}' did not "
                         "resolve to a file", textureGuid.ToString());
                return {};
            }
            mintPath = texPath->parent_path() / (texPath->stem().string() + ".arcsprite");
            for (int i = 1; std::filesystem::exists(mintPath); ++i)   // never clobber an existing file
                mintPath = texPath->parent_path() /
                          (texPath->stem().string() + "-" + std::to_string(i) + ".arcsprite");
        }

        Arcane::SpriteAssetData data;
        data.id      = Arcane::Guid::Generate();
        data.name    = mintPath.stem().string();
        data.texture = textureGuid;
        if (!Arcane::SaveSpriteAsset(mintPath, data))
        {
            ARC_WARN("Arcane Editor: could not mint a sprite at '{}'", mintPath.generic_string());
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
        if (!m_runtime->RegisterCreatedAsset(mintPath))
            return {};
        // A new registry entry changes folder grouping -- see PollAssetWatch's
        // drop-discovery comment above for the same reasoning.
        m_assetModel.MarkAllDirty();
        return data.id;
    }

    // F2a, Task 9: always mints (never reuses -- there is no source asset to
    // key a reuse policy off, unlike MintOrReuseSpriteForTexture above).
    // Task 13: `target` is now the CALLER's (ConsumeCreateResult's) dialog-
    // validated Name+Location -- the hardcoded "Content/New Mesh[-N]" default
    // and its "-N" uniquify loop are gone, the same shortcut
    // MintOrReuseSpriteForTexture's own dialog branch takes above (Name+
    // Location already proved unique at Create-click time).
    Arcane::Guid EditorApp::MintMeshAsset(const std::filesystem::path& target)
    {
        const Arcane::Project* project = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return {};

        // MeshAssetData's own defaults (source = Cube, everything else at its
        // struct default) are already a complete, valid asset -- see
        // MeshAsset.hpp. Only identity needs setting here.
        Arcane::MeshAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = target.stem().string();
        if (!Arcane::SaveMeshAsset(target, data))
        {
            ARC_WARN("Arcane Editor: could not create a mesh at '{}'", target.generic_string());
            return {};
        }
        // Register immediately so the browser sees it right away -- same
        // reasoning as CreateMaterialAt/CreateInstanceAt.
        if (!m_runtime->RegisterCreatedAsset(target))
            return {};
        m_assetModel.MarkAllDirty();
        return data.id;
    }

    Arcane::Guid EditorApp::CreateMaterialAt(std::filesystem::path path, Arcane::MaterialSurface surface)
    {
        if (path.extension() != ".arcmat")
            path += ".arcmat";

        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.name = path.stem().string();

        if (surface == Arcane::MaterialSurface::Mesh)
        {
            // Mesh materials stitch NO shader source (MaterialSurface's own
            // comment, Material/MaterialSource.hpp) -- no snippet, no graph.
            // The F2a-declared params ride as saved VALUES instead:
            // MeshMaterialCache reads `baseColor`/`albedo` straight out of
            // `params` by name (OwnBaseColor/OwnAlbedo), never through
            // MaterialSource/ShaderCompiler. A LoadMaterialAsset that later
            // finds a snippet or graph on a "mesh"-kind file treats it as
            // dead content and ignores it with one diagnostic
            // (MaterialAsset.cpp's KindIgnoresSnippetGraph) -- so this path
            // never writes either field, keeping a freshly-created mesh
            // material clean of that diagnostic.
            data.kind = "mesh";
            data.params.emplace_back("baseColor",
                                     Arcane::MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f));
            data.params.emplace_back("albedo",
                                     Arcane::MatParamValue::MakeTexture(Arcane::Guid{}));
        }
        else
        {
            // UE-model: every new material is GRAPH-owned (freeform HLSL
            // lives in Custom nodes; legacy text-owned .arcmat files still
            // open fine). Starter = a Color wired to the Output -- never an
            // empty canvas.
            //
            // ASSET-MANAGER PLAN 1 TASK 12: Sprite shares this branch with
            // Fullscreen rather than getting a third one, because the two
            // differ in EXACTLY two places and nothing else -- the `kind`
            // string written to the .arcmat, and the surface the starter
            // graph is generated against. Everything else (the two-node
            // Color -> Output graph, its positions, its link) is identical, so
            // a copied branch would be two bodies that must be kept in step by
            // hand for no gain.
            //
            // BOTH SURFACES HAVE A REAL TEMPLATE, verified rather than
            // assumed: MaterialTemplateFile (MaterialSource.cpp:313-336)
            // returns "materials/sprite_material.hlsl" for Sprite and
            // "materials/fullscreen_material.hlsl" for Fullscreen, and it
            // ARC_ENSUREs loudly ONLY for Mesh -- the one surface with no
            // template yet, which is why Mesh takes the snippet-less branch
            // above and never reaches here. GenerateGraphSnippet's own
            // surface guard is likewise Mesh-only (MaterialGraph.cpp:397),
            // and its two sprite-gated node types (VertexColor,
            // SpriteTexture) are ADDITIONAL capabilities on the sprite
            // surface -- a plain ConstColor -> Output graph is legal on
            // either. So this call is a real sprite compile, not a fullscreen
            // one wearing a sprite label.
            const bool sprite = (surface == Arcane::MaterialSurface::Sprite);
            data.kind = sprite ? "sprite" : "fullscreen";
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
            auto gen = Arcane::GenerateGraphSnippet(
                g, sprite ? Arcane::MaterialSurface::Sprite : Arcane::MaterialSurface::Fullscreen);
            if (!gen.errors.empty())
            {
                // A starter graph this function AUTHORED failing to compile is
                // a bug in this function, not in user content -- so it is said
                // out loud instead of writing a silently snippet-less .arcmat
                // that would open as an empty-looking material. The asset is
                // still written (the graph is intact and re-generates on open);
                // only the cached snippet is missing.
                ARC_WARN("Arcane Editor: the {} starter graph produced {} codegen error(s) "
                         "for '{}' -- the material is saved with its graph but no snippet",
                         data.kind, gen.errors.size(), path.generic_string());
            }
            data.snippet = std::move(gen.snippet);
            data.graph = std::move(g);
        }

        if (!Arcane::SaveMaterialAsset(path, data))
        {
            ARC_WARN("Arcane Editor: could not create material at '{}'", path.generic_string());
            return {};
        }
        // Register with the open project's registry so the new asset appears in
        // the browser and resolves by GUID IMMEDIATELY (not on next project open).
        const auto registered = m_runtime->RegisterCreatedAsset(path);
        if (!registered)
            return {};
        m_assetModel.MarkAllDirty();
        m_documents.OpenPath(path);
        return *registered;
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
    // m_createDialog (asset-manager Task 12): an in-flight create dialog's
    // parent/texture Guids and folder index all name the OUTGOING project.
    // m_createDiagnostics (asset-manager final fix wave): the failed-create
    // Problems accumulator ConsumeCreateResult republishes THE WHOLE OF on the
    // next failure -- its rows' messages and File locators name paths under the
    // outgoing project's Content tree, the same staleness class
    // m_reportDiagnostics and m_cookDiagnostics below already document.
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
    // m_contentDiscoveryNext (F2b desk-checkpoint fix): gates a walk of the
    // OUTGOING project's Content/ tree -- reset alongside m_materialWatchNext
    // so the incoming project's first tick discovers immediately rather than
    // waiting out whatever cadence the outgoing project had reached.
    // (the two launch-modal flags this entry used to name are gone entirely --
    // a parked LaunchStandalone now lives in m_scene, covered by the m_scene
    // entry above; see the comment ahead of m_scene.Reset below.)
    // m_cookQueue / m_cookDiagnostics (F2b Task 12): m_cookQueue's
    // CookSession is rooted at the OUTGOING project's directory -- surviving
    // a switch would cook the WRONG project's Content/ the next time
    // something notices a change. m_cookDiagnostics carries
    // DiagLocator::Asset(guid) rows for guids that exist only in the
    // outgoing project's registry, the identical staleness class
    // m_reportDiagnostics's own comment already documents (m_consoleDiag.
    // store.ClearAll() above clears the PUBLISHED "diagnostics:cook" set,
    // but the accumulator itself needs its own clear or the next post-switch
    // publish would resurrect every stale row alongside it).
    void EditorApp::ResetPerProjectState()
    {
        m_documents.CloseAll();
        if (m_resolver)
            m_resolver->Clear();
        m_consoleDiag.store.ClearAll();
        ClearSceneReferences();
        if (m_undo) m_scene.Reset(*m_undo);
        m_recents.scenes = {};
        // Asset-manager Task 12: an in-flight create dialog names the OUTGOING
        // project -- its parent/texture Guids belong to that registry and its
        // folder index points into a combo built from that project's folders.
        // Cleared, not carried: the popup is closed by the switch's own frame
        // discontinuity, and a stale `open` would re-raise it against the new
        // project with the old project's fields.
        m_createDialog = {};
        // Final fix wave (I3): the failed-create Problems accumulator. Same
        // staleness class m_reportDiagnostics/m_cookDiagnostics document --
        // every row's message and File locator name a path under the OUTGOING
        // project's Content tree, and m_consoleDiag.store.ClearAll() above only
        // clears the PUBLISHED "assets:create" set, so without this the next
        // post-switch failure would republish and resurrect all of them.
        m_createDiagnostics.clear();
        m_dialogs.ClearAll();
        m_modalErrors.Clear();
        m_materialMtimes.clear();
        m_materialWatchNext = 0.0;
        m_contentDiscoveryNext = 0.0;
        // Asset-manager Task 8: a Guid means something ELSE in a different
        // project, and the harvester's preview batcher holds the OUTGOING
        // project's registered materials -- so it drops everything, vehicle
        // included. Safe here: TeardownGraphForSwitch keeps the chrome
        // context (whose device the vehicle borrows) alive across a switch,
        // and Clear() is idempotent besides.
        if (m_materialThumbs)
            m_materialThumbs->Clear();
        m_cookQueue.reset();
        m_cookDiagnostics.clear();
        // Desk-fix 2: the settling window is meaningless without a live
        // m_cookQueue -- reset alongside it so a stray call to the Assets
        // facade's probe between this reset and the next OnProjectOpened (a
        // project-less gap, or mid-switch) reads a defined, non-stale value.
        // The probe closure itself also guards on `!m_cookQueue` (see
        // OnProjectOpened's own comment), so this is belt-and-suspenders, not
        // load-bearing on its own.
        m_cookQueueSettling = false;
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
        // Same shared rule the boot path uses (EditorApp::Init) -- a live
        // in-session switch must not re-mount diag:// under a verify run
        // either, or the opt-out would hold only until the first switch.
        // Forwarded explicitly by the project_open body below, which is
        // REPLACED here rather than inherited from CoreStages.
        ctx.openOptions = Arcane::HostBoot::OpenOptionsFor(m_config);

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
            // openOptions is captured BY VALUE (ctx is a local of this function and
            // this stage body replaces CoreStages', so it cannot read ctx.openOptions
            // the way the inherited body does) -- the struct is a bool, copying it is
            // free, and a value capture cannot dangle.
            const Arcane::ProjectOpenOptions openOptions = ctx.openOptions;
            projectOpen.run = [this, &path, scanDetail, openOptions]
            {
                // scanDetail is unconditionally attached by CoreStages' own
                // Make("project_open", ...) call, so this is never null in
                // practice; still guarded rather than asserted, since a null
                // detail box here is harmless (OpenProject just runs without a
                // progress callback) and not worth a hard failure.
                if (!scanDetail)
                    return m_runtime->OpenProject(path, {}, openOptions);
                return m_runtime->OpenProject(path,
                    [scanDetail](std::size_t done, std::size_t total)
                    {
                        constexpr std::size_t kStride = 32;
                        if (done != 1 && done != total && done % kStride != 0)
                            return;
                        scanDetail->Set("Scanning content... " + std::to_string(done) +
                                         " / " + std::to_string(total));
                    },
                    openOptions);
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

            // Final fix wave (I2): a .arcdiag just entered the registry, so the
            // panel model's entry list is short by one -- the same
            // drop-discovery reasoning as every other RegisterCreatedAsset
            // consumer (Task 5). Without this the row appears only when
            // something unrelated dirties the model, while the Problems row
            // published just below already points the user AT the browser.
            m_assetModel.MarkAllDirty();
            // Asset-manager Plan 2 Task 5: a crash report landing in the
            // registry is a Created event too -- same "new registry entry"
            // shape as drop discovery above, just a different producer.
            m_assetActivity.Push({ std::chrono::steady_clock::now(), *id,
                                    diagPath.filename().string(),
                                    Arcane::Editor::AssetActivityKind::Created, {} });

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
        if (!ChromeGraph())
        {
            ARC_ERROR("Crash GPU: the graph vehicle is not up -- nothing dispatched");
            return;
        }
        nri::Queue* const queue = ChromeGraph()->Device().GraphicsQueue();
        if (!queue || !Arcane::NriDiagnostics::FireFault(ChromeGraph()->Device(), *queue))
            ARC_ERROR("Crash GPU: fault injector unavailable -- nothing dispatched");
    }
#endif
}
