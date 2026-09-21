// Include order: NRI first, ALWAYS (NriCommon.hpp explains the ERROR clash).
#include <NRI.h>

#include <Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/GpuScene.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <memory>

namespace Arcane
{
    GpuSceneNodeInputs AddGpuSceneSyncNode(RenderGraph& graph, NriGraphContext* context,
                                           const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc)
    {
        GpuSceneNodeInputs in;
        in.readiness = std::make_shared<GpuSceneFrameReadiness>();
        GpuScene* scene = context ? context->Scene() : nullptr;
        const std::uint32_t slot = context ? context->FrameSlot() : 0;

        // Growth BEFORE the imports (ruling R-B): the handles minted below must
        // name the buffer this frame writes and reads. A refusal is logged and
        // the previous buffers stay -- the record then refuses too, loudly.
        if (scene)
        {
            const bool reserved = scene->Reserve(frame, adHoc.size(), slot, context->CurrentFence());
            in.readiness->registryReserved = frame && reserved;
            if (frame && !reserved)
                ARC_ERROR("[nri-graph] GpuSceneSyncNode: GpuScene::Reserve refused this frame's registry buffers");
        }

        graph.AddNode("gpuscene-sync", RenderGraph::NodeKind::Copy,
            [&in, scene, slot](RenderGraphBuilder& builder)
            {
                // Device-less declaration-shape drives (RenderGraphTest) pass null buffers; the graph tolerates them.
                in.instances      = builder.ImportBuffer("gpuscene.instances", scene ? scene->Instances() : nullptr,
                                                         scene ? scene->InstanceBytes() : 0);
                in.args           = builder.ImportBuffer("gpuscene.args", scene ? scene->Args(slot) : nullptr,
                                                         scene ? scene->ArgBytes(slot) : 0);
                in.visibleIndices = builder.ImportBuffer("gpuscene.visible", scene ? scene->VisibleIndices(slot) : nullptr,
                                                         scene ? scene->VisibleBytes(slot) : 0);
                in.cullBatches = builder.ImportBuffer("gpuscene.cull-batches", scene ? scene->CullBatches(slot) : nullptr,
                                                       scene ? scene->CullBatchBytes(slot) : 0);
                builder.Write(in.instances, RgUsage::CopyDst);
                builder.Write(in.args, RgUsage::CopyDst);
                builder.Write(in.visibleIndices, RgUsage::CopyDst);
                builder.Write(in.cullBatches, RgUsage::CopyDst);
            },
            [context, frame, adHoc, readiness = in.readiness](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                GpuScene* s = context->Scene();
                if (!s)
                    return;
                // A Reserve refusal still permits the independent scratch
                // upload. Registry data is omitted so stale slot buffers can
                // never reach the later compute/mesh callbacks.
                const GpuSceneFrame* registryFrame = readiness->registryReserved ? frame : nullptr;
                const GpuSceneApplyResult applied = s->Apply(registryFrame, adHoc, context->FrameSlot(), nodeContext);
                readiness->registryReady = readiness->registryReserved && applied.registryReady;
                readiness->adHocReady = applied.adHocReady;
                if (frame && readiness->registryReserved && !readiness->registryReady)
                    ARC_ERROR("[nri-graph] GpuSceneSyncNode: GpuScene::Apply refused this frame's rows");
            });
        return in;
    }

    void AddGpuSceneDebugReadbackNode(RenderGraph& graph, NriGraphContext* context,
                                      const GpuSceneNodeInputs& inputs)
    {
        GpuScene* scene = context ? context->Scene() : nullptr;
        if (!scene || !scene->DebugReadbackEnabled())
            return;
        // Sized to the instance buffer AFTER this frame's Reserve grew it.
        if (!scene->EnsureDebugReadback(context->CurrentFence()))
            return;   // already logged

        // Shared rather than captured by value for the same reason the capture
        // node's is: the handle is minted inside this node's own setup.
        auto readback = std::make_shared<RgBuffer>();
        const RgBuffer instances = inputs.instances;
        graph.AddNode("gpuscene-readback", RenderGraph::NodeKind::Copy,
            [scene, instances, readback](RenderGraphBuilder& builder)
            {
                *readback = builder.ImportBuffer("gpuscene.readback", scene->DebugReadbackBuffer(),
                                                 scene->DebugReadbackBytes());
                builder.Read(instances, RgUsage::CopySrc);
                builder.Write(*readback, RgUsage::ReadbackHost);
            },
            [scene, instances, readback](RenderGraphNodeContext& nodeContext)
            {
                scene->RecordDebugReadback(nodeContext, instances, *readback);
            });
    }

    void AddGpuSceneVisibilityReadbackNode(RenderGraph& graph, NriGraphContext* context,
                                           const GpuSceneNodeInputs& inputs, const GpuSceneFrame* frame)
    {
        GpuScene* scene = context ? context->Scene() : nullptr;
        if (!scene || !scene->VisibilityReadbackEnabled() || !frame || frame->rowCount == 0 || frame->args.empty())
            return;   // unarmed, or a frame with nothing the cull pass could have written
        const std::uint32_t slot = context->FrameSlot();
        // Sized -- and the publish parked -- at DECLARATION time, after
        // AddGpuSceneSyncNode's Reserve settled this slot's buffers.
        if (!scene->EnsureVisibilityReadback(slot, static_cast<std::uint32_t>(frame->args.size()),
                                             frame->rowCount, context->CurrentFence()))
            return;   // already logged, or this frame has nothing to copy

        // Shared rather than captured by value for the same reason the debug
        // readback node's is: the handle is minted inside this node's setup.
        auto readback = std::make_shared<RgBuffer>();
        graph.AddNode("gpuscene-visibility-readback", RenderGraph::NodeKind::Copy,
            [scene, inputs, readback, slot](RenderGraphBuilder& builder)
            {
                *readback = builder.ImportBuffer("gpuscene.visibility-readback",
                                                 scene->VisibilityReadbackBuffer(slot),
                                                 scene->VisibilityReadbackBytes(slot));
                builder.Read(inputs.args, RgUsage::CopySrc);
                builder.Read(inputs.visibleIndices, RgUsage::CopySrc);
                builder.Write(*readback, RgUsage::ReadbackHost);
            },
            [scene, inputs, slot, readiness = inputs.readiness](RenderGraphNodeContext& nodeContext)
            {
                // A REFUSED REGISTRY FRAME COPIES NOTHING: the slot buffers
                // then hold the previous frame's data, and publishing that as
                // this frame's answer would be a fabricated result. The parked
                // thunk sees no matching record and keeps the last real one.
                if (!readiness || !readiness->registryReady)
                    return;
                scene->RecordVisibilityReadback(nodeContext, inputs.args, inputs.visibleIndices, slot);
            });
    }
}
