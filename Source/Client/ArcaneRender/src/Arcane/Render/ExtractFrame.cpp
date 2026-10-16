#include <Arcane/Render/ExtractFrame.hpp>

#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/GpuSceneSync.hpp>        // GpuSceneSync, BuildGpuSceneFrame, MeshSectionRanges
#include <Arcane/Render/RenderSystems.hpp>      // RenderSubmissionSystem
#include <Arcane/Scene/SceneCamera.hpp>          // ActiveSceneCamera, ActivePerspectiveSceneCamera
#include <Arcane/Scene/SceneResources.hpp>       // RenderContext2D, MeshTable

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    // Defined in Nri/GpuScene.cpp. Declared here, not via GpuScene.hpp: that
    // header includes NRI.h, and this TU already pulled windows.h through
    // Astra (the ERROR macro would corrupt nri::Message::ERROR).
    std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;

    namespace
    {
        // GpuSceneHost.hpp's SameView. That header is ArcaneClient's;
        // ArcaneRender does not include it.
        [[nodiscard]] bool SameView(const ViewTransform& a, const ViewTransform& b) noexcept
        {
            return a.view == b.view && a.projection == b.projection && a.viewport == b.viewport;
        }

        // The sprite batch, copied. The host already called Begin, SetGlobals
        // and SetRenderContext on its persistent batcher, and the sprite
        // materials are registered on that batcher, so extract submits into
        // it rather than a fresh one: Begin would clear the host's geometry
        // and reset its view-projection, SetGlobals(GlobalParams{}) would wipe
        // Time/DeltaTime, and QuadWorld on a fresh batcher rewrites every
        // registered id to kMaterialSprite. A null batcher submits nothing
        // and leaves the packet's batch empty.
        void ExtractSprites(Astra::Registry& reg, const ExtractRequest& req, RenderFramePacket& packet)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            Batcher2D* batcher = ctx ? ctx->batcher : nullptr;
            if (!batcher)
                return;

            glm::mat4 vp{ 1.0f };
            if (req.editorView)
                vp = req.editorView->ViewProjection();
            else if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
                vp = cam->view.ViewProjection();
            else
                vp = ctx->view.ViewProjection();   // no scene camera: the host's stored view (a plugin's SetView), never identity
            batcher->SetViewProjection(vp);   // does not clear geometry
            RenderSubmissionSystem{}(reg);
            const Batch2DDrained drained = batcher->Drain();
            packet.batchVertices.assign(drained.vertices.begin(), drained.vertices.end());
            packet.batchIndices.assign(drained.indices.begin(), drained.indices.end());
            packet.batchSpans.assign(drained.spans.begin(), drained.spans.end());
            if (drained.globals)
                packet.globals = *drained.globals;
            packet.viewProjection = drained.viewProjection;
        }
    }

    const RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring)
    {
        NoteExtractFrameCall();
        RenderFramePacket& packet = ring.BeginExtract();
        packet.viewport = req.viewport;
        packet.origin.translation = { 0.0f, 0.0f, 0.0f };

        // The two views the hosts pass PrepareSceneForRender today: the main (sprite)
        // view and the mesh view. Task 7 publishes both on the packet.
        std::optional<ViewTransform> mainView;
        std::optional<ViewTransform> meshView;
        if (req.editorView)
        {
            mainView = *req.editorView;
            meshView = *req.editorView;   // Edit: one camera for both (EditorAppFrame.cpp)
        }
        else
        {
            if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
                mainView = cam->view;
            else if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
                mainView = rc->view;   // no scene camera: the host's stored view (a plugin's SetView)
            meshView = ActivePerspectiveSceneCamera(reg, req.viewport);
        }
        ViewTransform cullView = mainView.value_or(ViewTransform{});
        cullView.viewport = req.viewport;

        SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        if (!sv)
            sv = reg.EmplaceResource<SceneVisibility>();
        const bool twoViews = meshView && !SameView(*meshView, cullView);
        sv->views.resize(twoViews ? 2 : 1);
        BuildVisibleSet(reg, cullView, sv->views[0]);
        if (twoViews)
            BuildVisibleSet(reg, *meshView, sv->views[1]);
        packet.visible = sv->views[0];
        if (twoViews)
            packet.meshVisible = sv->views[1];

        GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
        if (!mirror)
            mirror = reg.EmplaceResource<GpuSceneMirror>();
        GpuSceneStage stage;
        GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(req.device), stage);
        if (meshView)
        {
            const std::vector<MeshSectionRange> sections = MeshSectionRanges(reg.GetResource<MeshTable>());
            BuildGpuSceneFrame(*mirror, twoViews ? &packet.meshVisible : &packet.visible, sections, *meshView, packet.mesh);
        }
        else
        {
            // PrepareSceneForRender's no-mesh-view arm (GpuSceneHost.hpp): the
            // stage still lands; only the batch table is empty.
            packet.mesh.batches.clear();
            packet.mesh.args.clear();
            packet.mesh.visibleIndices.clear();
            packet.mesh.rowCount = stage.rowCapacity;
            packet.mesh.stats = {};
        }
        packet.mesh.stage = stage;

        ExtractSprites(reg, req, packet);
        return packet;
    }
}
