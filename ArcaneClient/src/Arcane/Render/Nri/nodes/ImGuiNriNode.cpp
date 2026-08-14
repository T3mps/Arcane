// ImGuiNriNode -- see the header for where the node sits in the frame, why
// the work is split across declaration and record time, and why there is no
// pool-epoch discipline in this file.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>

#include "ImGuiNriNode.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#undef ERROR

namespace Arcane
{
    std::unique_ptr<ImGuiNriNode> ImGuiNriNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<ImGuiNriNode> node(new ImGuiNriNode());
        // The bins are cached BY THE VEHICLE and outlive this node, which
        // NriPipelineCache's fill contract (rule 2) requires: the cache
        // dereferences the blob after the fill callback returns.
        if (!node->m_renderer.Init(context.Device(), context.Pipelines(),
                                    context.ShaderBytecode("imgui_vs"),
                                    context.ShaderBytecode("imgui_ps")))
        {
            return nullptr;   // already logged
        }
        return node;
    }

    void ImGuiNriNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        m_renderer.Release(graveyard, fence);
    }

    void ImGuiNriNode::PrepareFrame(ImDrawData* drawData, std::uint64_t fence)
    {
        m_renderer.NewFrameTexUpdates(drawData, fence);
    }

    void ImGuiNriNode::Record(RenderGraphNodeContext& context, ImDrawData* drawData, RgTexture target)
    {
        m_renderer.RenderDrawData(drawData, context, target);
    }

    void AddImGuiNode(RenderGraph& graph, NriGraphContext* context, RgTexture target)
    {
        // The texture protocol runs HERE, at declaration time, exactly like
        // AddBatch2DNode's Drain and AddPickNodes' PrepareDrawables: it is the
        // last point before the frame's command buffer opens, and a texture
        // upload on this path goes through a helper that submits and waits.
        if (context)
        {
            if (ImGuiNriNode* node = context->ImGuiHud())
                node->PrepareFrame(context->CurrentImGuiDrawData(),
                                    context->Graph().DebugSubmitCount());
        }

        graph.AddNode("imgui", RenderGraph::NodeKind::Raster,
            [&graph, target](RenderGraphBuilder& builder)
            {
                // The SAME ColorWrite the tonemap (and the outline composite)
                // declared on this backbuffer -- see the header: consecutive
                // same-state writes derive no transition, and the ROP's
                // destination read is inside AccessBits::COLOR_ATTACHMENT.
                builder.Write(target, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&target, 1));
            },
            [context, target](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // headless declaration-shape drive
                if (ImGuiNriNode* node = context->ImGuiHud())
                    node->Record(nodeContext, context->CurrentImGuiDrawData(), target);
            });
    }
}
