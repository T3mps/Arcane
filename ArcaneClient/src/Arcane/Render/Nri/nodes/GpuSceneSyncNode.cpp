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
        GpuScene* scene = context ? context->Scene() : nullptr;
        const std::uint32_t slot = context ? context->FrameSlot() : 0;

        // Growth BEFORE the imports (ruling R-B): the handles minted below must
        // name the buffer this frame writes and reads. A refusal is logged and
        // the previous buffers stay -- the record then refuses too, loudly.
        if (scene)
            (void)scene->Reserve(frame, adHoc.size(), slot, context->CurrentFence());

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
                builder.Write(in.instances, RgUsage::CopyDst);
                builder.Write(in.args, RgUsage::CopyDst);
                builder.Write(in.visibleIndices, RgUsage::CopyDst);
            },
            [context, frame, adHoc](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                GpuScene* s = context->Scene();
                if (!s)
                    return;
                if (!s->Apply(frame, adHoc, context->FrameSlot(), nodeContext))
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
}
