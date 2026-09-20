#include <NRI.h>

#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>

#undef ERROR

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/GpuScene.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp>

#include <cstring>

namespace Arcane
{
    namespace
    {
        struct CullConstants
        {
            std::uint32_t rowCount = 0;
            std::uint32_t enabled = kMeshCullEnabled ? 1u : 0u;
            std::uint32_t pad[2]{};
            glm::vec4 planes[6]{};
        };
        static_assert(sizeof(CullConstants) == 112);
    }

    std::unique_ptr<MeshCullNode> MeshCullNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<MeshCullNode> node(new MeshCullNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool MeshCullNode::Init(NriGraphContext& context)
    {
        m_device = &context.Device();
        m_pipelines = &context.Pipelines();
        m_shader = context.ShaderBytecode("mesh_cull_cs");
        if (m_shader.empty())
        {
            ARC_ERROR("[nri-graph] MeshCullNode: mesh_cull_cs.bin is missing -- run compile-shaders.bat");
            return false;
        }
        nri::RootConstantDesc root = {};
        root.registerIndex = 0; root.size = sizeof(CullConstants); root.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::DescriptorRangeDesc ranges[4] = {};
        for (nri::DescriptorRangeDesc& range : ranges) range.descriptorNum = 1, range.shaderStages = nri::StageBits::COMPUTE_SHADER, range.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
        ranges[0].baseRegisterIndex = 0; ranges[0].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER; // t0 instances
        ranges[1].baseRegisterIndex = 1; ranges[1].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER; // t1 batches
        ranges[2].baseRegisterIndex = 0; ranges[2].descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER; // u0 visible
        ranges[3].baseRegisterIndex = 1; ranges[3].descriptorType = nri::DescriptorType::STORAGE_STRUCTURED_BUFFER; // u1 args
        nri::DescriptorSetDesc set = {};
        set.registerSpace = 1; set.ranges = ranges; set.rangeNum = 4; set.flags = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
        nri::PipelineLayoutDesc layout = {};
        layout.rootRegisterSpace = 0; layout.rootConstants = &root; layout.rootConstantNum = 1; layout.descriptorSets = &set; layout.descriptorSetNum = 1; layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
        m_layoutId = m_pipelines->RegisterLayout(layout);
        if (m_layoutId == NriPipelineCache::kInvalidLayout) return false;
        nri::DescriptorPoolDesc pool = {};
        pool.descriptorSetMaxNum = kSwapchainFramesInFlight; pool.structuredBufferMaxNum = 2 * kSwapchainFramesInFlight; pool.storageStructuredBufferMaxNum = 2 * kSwapchainFramesInFlight; pool.flags = nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET;
        if (!ARC_NRI_CHECK(m_device->Core().CreateDescriptorPool(m_device->Device(), pool, m_pool)) || !m_pool) return false;
        if (!ARC_NRI_CHECK(m_device->Core().AllocateDescriptorSets(*m_pool, *m_pipelines->Layout(m_layoutId), 0, m_sets, kSwapchainFramesInFlight, 0))) return false;
        m_pipeline = m_pipelines->GetCompute({ 0xF3000004ull, m_layoutId }, [this](nri::ComputePipelineDesc& desc)
        {
            desc.shader.stage = nri::StageBits::COMPUTE_SHADER; desc.shader.bytecode = m_shader.data(); desc.shader.size = m_shader.size(); desc.shader.entryPointName = "cs_main";
        });
        return m_pipeline != nullptr;
    }

    bool MeshCullNode::UpdateSet(std::uint32_t slot, GpuScene& scene)
    {
        const nri::Descriptor* views[4] = { scene.InstancesView(), scene.CullBatchesView(slot), scene.VisibleIndicesStorageView(slot), scene.ArgsStorageView(slot) };
        if (!views[0] || !views[1] || !views[2] || !views[3]) return false;
        if (m_instanceViews[slot] == views[0] && m_batchViews[slot] == views[1] && m_visibleViews[slot] == views[2] && m_argViews[slot] == views[3]) return true;
        nri::UpdateDescriptorRangeDesc updates[4] = {};
        for (std::uint32_t i = 0; i < 4; ++i) { updates[i].descriptorSet = m_sets[slot]; updates[i].rangeIndex = i; updates[i].descriptors = &views[i]; updates[i].descriptorNum = 1; }
        m_device->Core().UpdateDescriptorRanges(updates, 4);
        m_instanceViews[slot] = views[0]; m_batchViews[slot] = views[1]; m_visibleViews[slot] = views[2]; m_argViews[slot] = views[3];
        return true;
    }

    void MeshCullNode::Record(RenderGraphNodeContext& context, GpuScene& scene, const GpuSceneFrame* frame, std::uint32_t slot)
    {
        if (!frame || frame->rowCount == 0 || !m_pipeline || slot >= kSwapchainFramesInFlight) return;
        if (!UpdateSet(slot, scene)) { ARC_ERROR("[nri-graph] MeshCullNode: missing GPU-scene descriptors"); return; }
        CullConstants constants;
        constants.rowCount = frame->rowCount;
        for (std::size_t i = 0; i < frame->frustum.planes.size(); ++i)
        {
            const Plane& p = frame->frustum.planes[i];
            constants.planes[i] = glm::vec4(p.n, p.d);
        }
        const nri::CoreInterface& core = context.core;
        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::COMPUTE, *m_pipelines->Layout(m_layoutId));
        core.CmdSetPipeline(context.cmd, *m_pipeline);
        nri::SetDescriptorSetDesc set = {}; set.setIndex = 0; set.descriptorSet = m_sets[slot]; set.bindPoint = nri::BindPoint::COMPUTE;
        core.CmdSetDescriptorSet(context.cmd, set);
        nri::SetRootConstantsDesc root = {}; root.rootConstantIndex = 0; root.data = &constants; root.size = sizeof(constants);
        core.CmdSetRootConstants(context.cmd, root);
        nri::DispatchDesc dispatch = {}; dispatch.x = MeshCullDispatchGroups(frame->rowCount); dispatch.y = 1; dispatch.z = 1;
        core.CmdDispatch(context.cmd, dispatch);
    }

    void MeshCullNode::Release(Graveyard& graves, std::uint64_t fence)
    {
        if (!m_device || !m_pool) return;
        const nri::CoreInterface* core = &m_device->Core();
        graves.Bury(fence, [core, pool = m_pool] { core->DestroyDescriptorPool(pool); });
        m_pool = nullptr;
    }

    void AddMeshCullNode(RenderGraph& graph, NriGraphContext* context, const GpuSceneNodeInputs& inputs, const GpuSceneFrame* frame)
    {
        graph.AddNode("mesh-cull", RenderGraph::NodeKind::Compute,
            [inputs](RenderGraphBuilder& builder)
            {
                builder.Read(inputs.instances, RgUsage::ShaderRead);
                builder.Read(inputs.cullBatches, RgUsage::ShaderRead);
                builder.Write(inputs.visibleIndices, RgUsage::ShaderWriteCs);
                builder.Write(inputs.args, RgUsage::ShaderWriteCs);
            },
            [context, frame](RenderGraphNodeContext& nodeContext)
            {
                if (context && context->MeshCull() && context->Scene()) context->MeshCull()->Record(nodeContext, *context->Scene(), frame, context->FrameSlot());
            });
    }
}
