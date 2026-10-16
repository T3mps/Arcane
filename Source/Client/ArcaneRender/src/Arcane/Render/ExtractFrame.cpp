#include <Arcane/Render/ExtractFrame.hpp>

#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/RenderSystems.hpp>      // RenderSubmissionSystem
#include <Arcane/Scene/SceneCamera.hpp>          // ActiveSceneCamera
#include <Arcane/Scene/SceneResources.hpp>       // RenderContext2D

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <optional>

namespace Arcane
{
    namespace
    {
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

        ExtractSprites(reg, req, packet);
        return packet;
    }
}
