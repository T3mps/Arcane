// EditorApp, asset file operations (spec 2026-09-30 s7.3, s7.12): the
// AssetFileOpHost adapter -- the executor's only door into the app --, the
// AssetOpFacts builder PlanAssetOp reads, the s7.1 gate predicate, the
// RunAssetOp front door, and InvalidateAssetCaches (the s7.12 table, factored
// from the existing per-kind invalidation sequences).
//
// The executor itself (m_assetFileOps) is per project: RetargetUndoCache
// (EditorAppProject.cpp) builds it on project open and resets it on close.

#include "App/EditorApp.hpp"
#include "Panels/AssetPanelCommon.hpp"       // BootSceneGuid
#include "Panels/AssetReferenceIndex.hpp"    // AssetReferenceIndex::Node (InvalidateAssetCaches)

#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>   // SaveJson (the live scene's asset list)

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    namespace AE = Arcane::Editor;

    // ---- the host adapter: one-line forwards -------------------------------

    Arcane::RebindResult EditorApp::AssetOpHost::Rebind(const Arcane::Guid& g, const std::filesystem::path& p)
    { return m_app.m_runtime->Core().RebindMovedAsset(g, p); }

    bool EditorApp::AssetOpHost::Unregister(const Arcane::Guid& g)
    { return m_app.m_runtime->Core().UnregisterAsset(g); }

    std::optional<Arcane::Guid> EditorApp::AssetOpHost::Register(const std::filesystem::path& p)
    { return m_app.m_runtime->RegisterCreatedAsset(p); }

    AE::OsShell::RecycleResult EditorApp::AssetOpHost::Recycle(std::span<const std::filesystem::path> files)
    {
        // The owner window keeps the shell's "delete permanently?" prompt in front (R11).
        return AE::OsShell::ShellRecycle(files, m_app.m_gpu ? m_app.m_gpu->Win().NativeHandle() : nullptr);
    }

    void EditorApp::AssetOpHost::Invalidate(const Arcane::Guid& g, AE::AssetKind k)
    { m_app.InvalidateAssetCaches(g, k); }

    void EditorApp::AssetOpHost::EvictPaths(std::span<const std::filesystem::path> ps)
    { for (const auto& p : ps) m_app.m_runtime->AssetsFacade().EvictPath(p); }

    void EditorApp::AssetOpHost::Activity(AE::AssetActivityEntry e)
    { m_app.m_assetActivity.Push(std::move(e)); }

    void EditorApp::AssetOpHost::ReportError(std::string title, std::string message)
    {
        ARC_ERROR("{}: {}", title, message);
        m_app.m_modalErrors.Push(std::move(title), std::move(message));
    }

    // ---- the host adapter: the rest ----------------------------------------

    bool EditorApp::AssetOpHost::CloseDocumentFor(const Arcane::Guid& g, bool discardDirty)
    {
        AE::EditorDocument* d = m_app.m_documents.FindByGuid(g);
        if (!d) return true;
        if (d->Dirty() && !discardDirty) return false;   // "Close or save <title> first"
        m_app.m_documents.CloseForAssetRemoval(d);
        return true;
    }

    void EditorApp::AssetOpHost::NoteMoved(const Arcane::Guid& g, const std::filesystem::path& from, const std::filesystem::path& to)
    {
        m_app.m_documents.NoteAssetMoved(g, to);
        m_app.m_scene.NoteMoved(from, to);
        std::string ext = to.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".arcscene") m_app.m_recents.NoteSceneMoved(m_app.m_runtime->CurrentProject(), from, to);
    }

    void EditorApp::AssetOpHost::AssetsChanged(std::span<const Arcane::Guid> removed, std::span<const Arcane::Guid> added)
    {
        m_app.m_assetModel.MarkAllDirty();               // grouping changes (the registry-change sites in EditorAppProject.cpp)
        bool erased = false;
        for (const auto& g : removed) erased |= m_app.m_cookDiagnostics.erase(g) > 0;
        if (erased) m_app.PublishCookDiagnostics();      // "diagnostics:cook" retracts the row
        for (const auto& g : added) m_app.m_runtime->AssetsFacade().ForgetUnresolved(g);
    }

    AE::AssetOpGates EditorApp::AssetOpHost::Gates() const   // "project open" includes the per-project executor
    { return { m_app.m_runtime && m_app.m_runtime->CurrentProject() && m_app.m_assetFileOps, !m_app.InPlayMode() }; }

    // ---- gates, facts, the front door --------------------------------------

    std::string EditorApp::AssetOpGateReason()               // T5-A9's ONE predicate; the executor re-checks it
    { return AE::AssetOpGateRefusal(m_assetOpHost.Gates(), m_undo && m_undo->InTransaction()).value_or(std::string{}); }

    // Precondition: a project is open (callers pass AssetOpGateReason first).
    AE::AssetOpFacts EditorApp::GatherAssetOpFacts(AssetOpFactsStore& s, bool withLiveScene)
    {
        AE::AssetOpFacts f;
        const Arcane::Project* pr = m_runtime->CurrentProject();
        f.contentDir = pr->Root() / "Content";
        f.diagDir    = pr->Root() / "Saved" / "Diagnostics";
        s.registry = pr->Registry().All();
        f.registry = s.registry;
        f.refs     = &m_assetModel.RefIndex();
        s.sceneAssets.clear();
        if (withLiveScene)                                // Delete only (s7.5 fact 2); Edit mode is gated
        {
            // A named local: never range-for over a member of the SaveJson temporary.
            const nlohmann::json live = Arcane::Scene::SaveJson(m_runtime->Registry());
            if (const auto it = live.find("assets"); it != live.end() && it->is_array())
                for (const nlohmann::json& a : *it)
                    if (a.is_string())
                        if (const auto g = Arcane::Guid::FromString(a.get<std::string>())) s.sceneAssets.push_back(*g);
        }
        f.openSceneAssets = s.sceneAssets;
        f.openScene       = m_scene.Id();
        f.bootScene       = AE::BootSceneGuid(pr);
        if (const auto ia = Arcane::Guid::FromString(pr->Manifest().inputActions)) f.inputActions = *ia;
        s.docs.clear();
        m_documents.ForEach([&](AE::EditorDocument& d) { s.docs.push_back({ d.AssetGuid(), d.Dirty(), d.Dirty() ? d.LiveReferences() : std::vector<Arcane::Guid>{} }); });   // T5 s7.5: only a dirty doc's unsaved refs count
        f.docs = s.docs;
        f.exists       = [](const std::filesystem::path& p) { std::error_code ec; return std::filesystem::exists(p, ec); };
        f.peekId       = [](const std::filesystem::path& p) { return Arcane::AssetRegistry::PeekId(p); };
        f.gltfUris     = [](const std::filesystem::path& p) { return AE::ReadGltfUris(p); };                  // T5-A8's provider
        f.diagSiblings = [](const std::filesystem::path&) { return std::vector<std::filesystem::path>{}; }; // B12
        return f;
    }

    std::optional<AE::AssetOpPlan> EditorApp::RunAssetOp(const AE::AssetOpRequest& req)
    {
        if (const std::string why = AssetOpGateReason(); !why.empty()) { m_assetOpHost.ReportError("Asset operation refused", why); return std::nullopt; }
        AE::AssetOpPlan plan = AE::PlanAssetOp(req, GatherAssetOpFacts(m_assetOpFacts, false));
        if (!plan.refusals.empty()) { m_assetOpHost.ReportError("Can't " + plan.label, plan.refusals.front().reason); return std::nullopt; }
        if (plan.moves.empty() && req.kind != AE::AssetOpKind::NewFolder) return std::nullopt;   // same name/folder: nothing pushed
        if (!m_assetFileOps->Execute(plan, *m_undo).ok) return std::nullopt;                     // reported once by the executor
        return plan;
    }

    // T5: post-op selection. Duplicate selects the copies (primary = last); the stamp bump scrolls to it after the rebuild AssetsChanged armed.
    void EditorApp::AfterAssetOp(const AE::AssetOpPlan& plan)
    { if (plan.kind == AE::AssetOpKind::Duplicate && !plan.newGuids.empty()) m_assetModel.Select(plan.newGuids.back()); }

    // ---- s7.12: what a moved/removed/restored asset invalidates ------------

    void EditorApp::InvalidateAssetCaches(const Arcane::Guid& g, AE::AssetKind k)
    {
        using K = AE::AssetKind;
        if (k == K::Material) { if (m_resolver) m_resolver->InvalidateMaterial(g); InvalidateMaterialThumb(g); }
        if (k == K::Mesh) { if (m_resolver) m_resolver->InvalidateMesh(g); if (m_materialThumbs) m_materialThumbs->InvalidateMesh(g); }
        if (k == K::Sprite && m_resolver) m_resolver->InvalidateSprite(g);
        if (k == K::Texture || k == K::Model)            // the texture/model cook-landed sequence (EditorAppProject.cpp)
        {
            m_runtime->AssetsFacade().InvalidateArtifact(g);
            m_runtime->AssetsFacade().InvalidateMeshArtifact(g);
            if (m_viewportTargets.graph) { m_viewportTargets.graph->InvalidateContentTexture(g); m_viewportTargets.graph->InvalidateMeshAlbedoSlot(g); }
            if (Arcane::NriGraphContext* chrome = ChromeGraph()) chrome->InvalidateContentTexture(g);
        }
        if (k == K::Model)                               // the companion .arcmesh(es) importing it
            if (const auto* n = m_assetModel.RefIndex().Find(g))
                for (const Arcane::Guid& r : n->inbound)
                    if (const auto* e = m_assetModel.Find(r); e && e->kind == K::Mesh)
                    {
                        if (m_resolver) m_resolver->InvalidateMeshArtifact(r);
                        if (m_materialThumbs) m_materialThumbs->InvalidateMesh(r);
                    }
    }
}
