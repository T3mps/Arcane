// FullscreenNodes -- see the header for the file's scope (tonemap at Task 8,
// the post chain at Task 10) and THE SOURCE VIEW lifetime rule.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp).
#include <NRI.h>

#include "FullscreenNodes.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry

#undef ERROR

#include <string>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            NvrhiMessageCallback::Instance().NoteError("nri-graph", text.c_str());
        }
    }

    std::unique_ptr<TonemapNode> TonemapNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<TonemapNode> node(new TonemapNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool TonemapNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        m_vs = context.ShaderBytecode("tonemap_vs");
        m_ps = context.ShaderBytecode("tonemap_ps");
        if (m_vs.empty() || m_ps.empty())
        {
            ARC_ERROR("[nri-graph] TonemapNode: the tonemap_vs/tonemap_ps bins are missing -- the graph "
                      "cannot produce display-referred pixels");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        // POINT + clamp, exactly TonemapPass::Init's sampler: the canvas maps
        // 1:1 onto the target, so any filtering would only soften it.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min  = nri::Filter::NEAREST;
        samplerDesc.filters.mag  = nri::Filter::NEAREST;
        samplerDesc.filters.mip  = nri::Filter::NEAREST;
        samplerDesc.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                     nri::AddressMode::CLAMP_TO_EDGE };
        samplerDesc.mipMax       = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] TonemapNode: sampler creation failed");
            return false;
        }

        // data/shaders/tonemap.hlsl declares t0 + s0 and NO constant buffer, so
        // this layout carries no root constants at all. Register space 0 to
        // match the shaders' implicit space; on Vulkan NRI applies the device's
        // vkBindingOffsets (t=0, s=128) to these indices itself.
        nri::DescriptorRangeDesc ranges[2] = {};
        ranges[0].baseRegisterIndex = 0;
        ranges[0].descriptorNum     = 1;
        ranges[0].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[0].shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        ranges[1].baseRegisterIndex = 0;
        ranges[1].descriptorNum     = 1;
        ranges[1].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[1].shaderStages      = nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc setDesc = {};
        setDesc.registerSpace = 0;
        setDesc.ranges        = ranges;
        setDesc.rangeNum      = 2;

        // Value-initialized then assigned field by field -- NriPipelineCache's
        // DEDUP CONTRACT.
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] TonemapNode: pipeline layout registration failed");
            return false;
        }

        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = 1;
        poolDesc.textureMaxNum       = 1;
        poolDesc.samplerMaxNum       = 1;
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] TonemapNode: descriptor pool creation failed");
            return false;
        }
        if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &m_set, 1, 0)) || !m_set)
        {
            ARC_ERROR("[nri-graph] TonemapNode: descriptor set allocation failed");
            return false;
        }

        // The sampler half of the set never changes; the texture half is
        // written by EnsureSource on the first frame and after every
        // invalidation.
        const nri::Descriptor* sampler = m_sampler;
        nri::UpdateDescriptorRangeDesc update = {};
        update.descriptorSet = m_set;
        update.rangeIndex    = 1;
        update.descriptors   = &sampler;
        update.descriptorNum = 1;
        core.UpdateDescriptorRanges(&update, 1);
        return true;
    }

    TonemapNode::~TonemapNode()
    {
        if (!m_device || (!m_sampler && !m_pool && !m_sourceView && m_orphanedViews.empty()))
            return;

        ARC_WARN("[nri-graph] TonemapNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (nri::Descriptor* view : m_orphanedViews)
            core.DestroyDescriptor(view);
        m_orphanedViews.clear();
        if (m_sourceView) core.DestroyDescriptor(m_sourceView);
        if (m_pool)       core.DestroyDescriptorPool(m_pool);
        if (m_sampler)    core.DestroyDescriptor(m_sampler);
        m_sourceView = nullptr; m_sourceTexture = nullptr;
        m_pool = nullptr; m_set = nullptr; m_sampler = nullptr;
    }

    void TonemapNode::InvalidateSource(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        for (nri::Descriptor* view : m_orphanedViews)
            graveyard.Bury(fence, [core, view] { core->DestroyDescriptor(view); });
        m_orphanedViews.clear();
        if (m_sourceView)
        {
            graveyard.Bury(fence, [core, view = m_sourceView] { core->DestroyDescriptor(view); });
            m_sourceView = nullptr;
        }
        m_sourceTexture = nullptr;
    }

    void TonemapNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        InvalidateSource(graveyard, fence);

        const nri::CoreInterface* core = &m_device->Core();
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            m_set  = nullptr;   // owned by the pool
        }
        if (m_sampler)
        {
            graveyard.Bury(fence, [core, d = m_sampler] { core->DestroyDescriptor(d); });
            m_sampler = nullptr;
        }
    }

    bool TonemapNode::EnsureSource(const nri::CoreInterface& core, nri::Texture* texture)
    {
        if (m_sourceView && m_sourceTexture == texture)
            return true;

        if (m_sourceView)
        {
            // BACKSTOP, not the mechanism. The owner is supposed to call
            // InvalidateSource before anything can destroy the transient pool
            // (header: THE SOURCE VIEW), so reaching here means an owner path
            // skipped it -- which is exactly the class of bug that otherwise
            // shows up as a use-after-free on a recycled texture address. Park
            // the old view rather than destroy it (the GPU may still be reading
            // it) and say so through the latch.
            GraphError("TonemapNode: the source texture changed without an InvalidateSource -- an owner "
                       "path is releasing the transient pool without telling the node");
            m_orphanedViews.push_back(m_sourceView);
            m_sourceView    = nullptr;
            m_sourceTexture = nullptr;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        // The transient's ACTUAL format, read back from NRI -- never assumed.
        viewDesc.format   = core.GetTextureDesc(*texture).format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, m_sourceView)) || !m_sourceView)
        {
            m_sourceView = nullptr;
            GraphError("TonemapNode: the source shader-resource view could not be created");
            return false;
        }
        m_sourceTexture = texture;

        const nri::Descriptor* view = m_sourceView;
        nri::UpdateDescriptorRangeDesc update = {};
        update.descriptorSet = m_set;
        update.rangeIndex    = 0;
        update.descriptors   = &view;
        update.descriptorNum = 1;
        core.UpdateDescriptorRanges(&update, 1);
        return true;
    }

    void TonemapNode::Record(RenderGraphNodeContext& context, RgTexture source, RgTexture target)
    {
        const nri::CoreInterface& core = context.core;

        nri::Texture* sourceTexture = context.Resolve(source);
        nri::Texture* targetTexture = context.Resolve(target);
        if (!sourceTexture || !targetTexture)
        {
            GraphError("TonemapNode: the node could not resolve its source or its target");
            return;
        }
        if (!EnsureSource(core, sourceTexture))
            return;   // already reported

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("TonemapNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = core.GetTextureDesc(*targetTexture).format;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        key.blend           = NriPipelineCache::GraphicsKey::Blend::Opaque;

        // `stages` lives in THIS frame, which encloses GetGraphics -- the fill
        // contract's rule 2.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_ps.data();
        stages[1].size           = m_ps.size();
        stages[1].entryPointName = kPsEntry;

        nri::Pipeline* pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            // No vertex input at all: tonemap.hlsl's vs_main builds the
            // fullscreen triangle from SV_VertexID.
            desc.vertexInput = nullptr;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
            desc.rasterization.cullMode = nri::CullMode::NONE;
        });
        if (!pipeline)
            return;   // already logged + latched by the cache

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc setDesc = {};
        setDesc.setIndex      = 0;
        setDesc.descriptorSet = m_set;
        core.CmdSetDescriptorSet(context.cmd, setDesc);

        core.CmdSetPipeline(context.cmd, *pipeline);

        nri::DrawDesc draw = {};
        draw.vertexNum   = 3;
        draw.instanceNum = 1;
        core.CmdDraw(context.cmd, draw);
    }

    RgTexture AddTonemapNode(RenderGraph& graph, NriGraphContext* context, RgTexture source)
    {
        // Shared rather than captured by value: the backbuffer handle is minted
        // INSIDE this node's own setup fn, which AddNode runs after both
        // lambdas have already been constructed. A by-value capture would
        // freeze the default (invalid) handle.
        auto backbuffer = std::make_shared<RgTexture>();
        graph.AddNode("tonemap", RenderGraph::NodeKind::Raster,
            [&graph, source, backbuffer](RenderGraphBuilder& builder)
            {
                // NOT ImportTexture: the graph owns acquire/present sequencing
                // and there is no nri::Texture* to hand over at declaration
                // time -- Execute() resolves this to the texture it acquires.
                *backbuffer = builder.ImportSwapChainTexture("backbuffer");
                builder.Read(source, RgUsage::ShaderRead);
                builder.Write(*backbuffer, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(backbuffer.get(), 1));
            },
            [context, source, backbuffer](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // headless declaration-shape drive
                if (TonemapNode* node = context->Tonemap())
                    node->Record(nodeContext, source, *backbuffer);
            });
        return *backbuffer;
    }
}
