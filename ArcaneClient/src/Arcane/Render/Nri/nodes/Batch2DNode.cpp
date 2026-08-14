// Batch2DNode -- see the header for what this node owns, why the CPU batching
// is reused rather than reimplemented, and THE TEXTURE GAP that bounds Task 8.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "Batch2DNode.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Batcher2D.hpp>        // Batch2DDrained / Batch2DVertex / Batch2DDrawSpan
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/ShaderConventions.hpp>  // kVsEntry / kPsEntry

#undef ERROR

#include <cstring>
#include <iterator>
#include <string>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            NvrhiMessageCallback::Instance().NoteError("nri-graph", text.c_str());
        }

        // The engine's canvas clear, verbatim from RuntimeApp::MainLoop's
        // clearTextureFloat on the NVRHI path. It lives HERE, in the only node
        // that clears the canvas, so the two paths cannot drift into different
        // backgrounds -- which is what makes a stage golden's background pixels
        // match by construction rather than by coincidence.
        constexpr float kCanvasClear[4] = { 0.02f, 0.02f, 0.04f, 1.0f };

        // The canvas is RGBA16F, matching Canvas.cpp:9 (kCanvasFormat =
        // nvrhi::Format::RGBA16_FLOAT). Colours are LINEAR and may exceed 1.0
        // -- the tonemap node is what turns them display-referred.
        constexpr nri::Format kCanvasFormat = nri::Format::RGBA16_SFLOAT;

        // data/shaders/sprite.hlsl's BatchConstants: float2 invHalfViewport +
        // float2 pad. Mirrors Batcher2D.cpp's PushConstants exactly (b0 on
        // D3D12, [[vk::push_constant]] on SPIR-V, which is what NRI's
        // RootConstantDesc lowers to on each backend).
        struct BatchRootConstants
        {
            float invHalfViewportX = 0.0f;
            float invHalfViewportY = 0.0f;
            float padX = 0.0f;
            float padY = 0.0f;
        };
        static_assert(sizeof(BatchRootConstants) == 16, "must match sprite.hlsl's BatchConstants");

        // The built-in shader pairs, in Batcher2D::kMaterial* order -- the SAME
        // offline bins the NVRHI batcher loads through ShaderLibrary
        // (Batcher2D.cpp's material table).
        constexpr const char* kBuiltInVs[] = { "sprite_vs", "circle_vs", "msdf_vs" };
        constexpr const char* kBuiltInPs[] = { "sprite_ps", "circle_ps", "msdf_ps" };
    }

    std::unique_ptr<Batch2DNode> Batch2DNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<Batch2DNode> node(new Batch2DNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool Batch2DNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        for (std::uint32_t i = 0; i < kBuiltInCount; ++i)
        {
            m_builtIns[i].vs = context.ShaderBytecode(kBuiltInVs[i]);
            m_builtIns[i].ps = context.ShaderBytecode(kBuiltInPs[i]);
            if (m_builtIns[i].vs.empty() || m_builtIns[i].ps.empty())
            {
                ARC_ERROR("[nri-graph] Batch2DNode: shader bin '{}'/'{}' is missing -- the batch node "
                          "cannot be built", kBuiltInVs[i], kBuiltInPs[i]);
                return false;
            }
        }

        // The vertex input, in members because GraphicsPipelineDesc::vertexInput
        // is a pointer the cache dereferences after `fill` returns. Layout is
        // Batch2DVertex, and the semantic names are sprite.hlsl's VSInput.
        m_attributes[0].d3d.semanticName = "POSITION";
        m_attributes[0].vk.location      = 0;
        m_attributes[0].offset           = offsetof(Batch2DVertex, pos);
        m_attributes[0].format           = nri::Format::RG32_SFLOAT;
        m_attributes[1].d3d.semanticName = "TEXCOORD";
        m_attributes[1].vk.location      = 1;
        m_attributes[1].offset           = offsetof(Batch2DVertex, uv);
        m_attributes[1].format           = nri::Format::RG32_SFLOAT;
        m_attributes[2].d3d.semanticName = "COLOR";
        m_attributes[2].vk.location      = 2;
        m_attributes[2].offset           = offsetof(Batch2DVertex, color);
        m_attributes[2].format           = nri::Format::RGBA32_SFLOAT;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(Batch2DVertex);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        return CreateWhiteTexel() && CreateBindings();
    }

    bool Batch2DNode::CreateWhiteTexel()
    {
        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        textureDesc.format    = nri::Format::RGBA8_UNORM;
        textureDesc.width     = 1;
        textureDesc.height    = 1;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, m_white))
            || !m_white)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the 1x1 white texel could not be created");
            return false;
        }
        core.SetDebugName(m_white, "nri-graph batch white");

        // Populated through NRI's OWN helper rather than a hand-rolled staging
        // buffer + transition pair. Deliberate: the phase's rule is that graph
        // code records no CmdBarrier of its own, and while a one-shot
        // initialisation upload is not a graph node (there is no derived
        // barrier chain for it to fight), the helper means this file contains
        // no barrier at all -- so the rule reads the same from the outside as
        // it does from the inside. UploadData submits and waits internally,
        // which is what "once, at Create()" wants anyway.
        nri::HelperInterface helper = {};
        if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface), &helper)))
        {
            ARC_ERROR("[nri-graph] Batch2DNode: HelperInterface unavailable -- cannot upload the white texel");
            return false;
        }

        const std::uint32_t whitePixel = 0xFFFFFFFFu;
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = &whitePixel;
        subresource.sliceNum   = 1;
        subresource.rowPitch   = 4;
        subresource.slicePitch = 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = m_white;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the white texel upload failed");
            return false;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = m_white;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, m_whiteView)) || !m_whiteView)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the white texel's shader-resource view could not be created");
            return false;
        }
        return true;
    }

    bool Batch2DNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // Linear + clamp, exactly the NVRHI batcher's sampler
        // (Batcher2D::Init's setAllFilters(true) / Clamp): sprites scale
        // smoothly, and a clamped atlas edge does not bleed.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min   = nri::Filter::LINEAR;
        samplerDesc.filters.mag   = nri::Filter::LINEAR;
        samplerDesc.filters.mip   = nri::Filter::LINEAR;
        samplerDesc.addressModes  = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                      nri::AddressMode::CLAMP_TO_EDGE };
        samplerDesc.mipMax        = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: sampler creation failed");
            return false;
        }

        // THE LAYOUT. sprite/circle/msdf all declare the same register map:
        // b0 constants, t0 texture, s0 sampler (data/shaders/sprite.hlsl).
        //
        // Root constants live in `rootRegisterSpace` (0 -> b0, space0 on
        // D3D12; a VK push-constant block, which has no space at all), and the
        // texture/sampler set is registerSpace 0 to match the shaders' implicit
        // space0. Those two CAN share space 0: NRI only refuses a collision
        // between rootRegisterSpace and a descriptor set when the layout also
        // carries root DESCRIPTORS or root SAMPLERS (Validation/DeviceVal.hpp),
        // which is exactly why the sampler here is a descriptor-set entry and
        // not a root (static) sampler -- a root sampler would claim space 0 and
        // push the texture set into a space the shaders do not name.
        //
        // On Vulkan the t0/s0 register indices are shifted by NRI itself using
        // the device's vkBindingOffsets (t=0, s=128), which NriDevice pins to
        // ShaderConventions.hpp's dxc -fvk-*-shift values with static_asserts.
        // So this desc is written in HLSL register numbers on both backends.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = sizeof(BatchRootConstants);
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER;

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

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.rootConstants     = &rootConstant;
        layoutDesc.rootConstantNum   = 1;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: pipeline layout registration failed");
            return false;
        }

        // ONE set, allocated once and written once. Task 8 binds the white
        // texel for every draw (THE TEXTURE GAP in the header), so nothing here
        // is per-frame -- which also means nothing here can be written while a
        // previous frame still reads it (the hazard ResetDescriptorPool exists
        // for). NRI's own guidance: "DescriptorSet is a tiny struct, so lots of
        // descriptor sets can be created in advance and reused without calling
        // ResetDescriptorPool" (NRI.h).
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = 1;
        poolDesc.textureMaxNum       = 1;
        poolDesc.samplerMaxNum       = 1;
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: descriptor pool creation failed");
            return false;
        }
        if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &m_set, 1, 0)) || !m_set)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: descriptor set allocation failed");
            return false;
        }

        const nri::Descriptor* texture = m_whiteView;
        const nri::Descriptor* sampler = m_sampler;
        nri::UpdateDescriptorRangeDesc updates[2] = {};
        updates[0].descriptorSet = m_set;
        updates[0].rangeIndex    = 0;
        updates[0].descriptors   = &texture;
        updates[0].descriptorNum = 1;
        updates[1].descriptorSet = m_set;
        updates[1].rangeIndex    = 1;
        updates[1].descriptors   = &sampler;
        updates[1].descriptorNum = 1;
        core.UpdateDescriptorRanges(updates, 2);
        return true;
    }

    Batch2DNode::~Batch2DNode()
    {
        if (!m_device || (!m_white && !m_whiteView && !m_sampler && !m_pool))
            return;

        ARC_WARN("[nri-graph] Batch2DNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        if (m_pool)      core.DestroyDescriptorPool(m_pool);
        if (m_sampler)   core.DestroyDescriptor(m_sampler);
        if (m_whiteView) core.DestroyDescriptor(m_whiteView);
        if (m_white)     core.DestroyTexture(m_white);
        m_pool = nullptr; m_set = nullptr;
        m_sampler = nullptr; m_whiteView = nullptr; m_white = nullptr;
    }

    void Batch2DNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture.
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            m_set  = nullptr;   // owned by the pool; not separately destroyable
        }
        if (m_sampler)
        {
            graveyard.Bury(fence, [core, d = m_sampler] { core->DestroyDescriptor(d); });
            m_sampler = nullptr;
        }
        if (m_whiteView)
        {
            graveyard.Bury(fence, [core, d = m_whiteView] { core->DestroyDescriptor(d); });
            m_whiteView = nullptr;
        }
        if (m_white)
        {
            graveyard.Bury(fence, [core, t = m_white] { core->DestroyTexture(t); });
            m_white = nullptr;
        }
    }

    nri::Pipeline* Batch2DNode::PipelineFor(std::uint16_t material, nri::Format canvasFormat)
    {
        std::uint32_t builtIn = material;
        if (builtIn >= kBuiltInCount)
        {
            // A REGISTERED sprite material (Task 9 builds those pipelines).
            // Falling back to the plain sprite pipeline draws the right
            // geometry with the wrong shader rather than dropping the content,
            // which is the same degradation Batcher2D::QuadMaterial already
            // applies to an unknown id.
            if (!m_warnedRegisteredMaterial)
            {
                m_warnedRegisteredMaterial = true;
                ARC_WARN("[nri-graph] Batch2DNode: material id {} is a REGISTERED material -- the graph "
                         "path draws it through the plain sprite pipeline until Task 9 lands", material);
            }
            builtIn = 0;
        }

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairBase + builtIn;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = canvasFormat;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        // Straight (non-premultiplied) alpha -- byte-for-byte the NVRHI
        // batcher's blend state (Batcher2D::GetPipeline: SrcAlpha/InvSrcAlpha
        // colour, One/InvSrcAlpha alpha).
        key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;

        // `stages` lives in THIS frame, which encloses the GetGraphics call --
        // the fill contract's rule 2. The bytecode it points at is owned by the
        // vehicle's bin cache and outlives this node.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_builtIns[builtIn].vs.data();
        stages[0].size           = m_builtIns[builtIn].vs.size();
        stages[0].entryPointName = kVsEntry;   // SPIR-V matches by name; DXIL ignores it
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_builtIns[builtIn].ps.data();
        stages[1].size           = m_builtIns[builtIn].ps.size();
        stages[1].entryPointName = kPsEntry;

        return m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            // Cull NONE + no depth test: the 2D batch is painter-ordered by its
            // own sort key, and Triangle()/Line() deliberately emit whatever
            // winding the caller's points imply (Batcher2D.hpp).
            desc.rasterization.fillMode = nri::FillMode::SOLID;
            desc.rasterization.cullMode = nri::CullMode::NONE;
        });
    }

    void Batch2DNode::Record(RenderGraphNodeContext& context, const Batch2DDrained& batch,
                             nri::Format canvasFormat)
    {
        const nri::CoreInterface& core = context.core;

        // THE CLEAR. Graph attachments are LOAD/STORE and the declaration API
        // carries no clear op, so a node that needs a cleared target clears it
        // from its own exec fn -- the seam Task 7 chose and documented in
        // NriGraphContext::BuildFrame. The executor has already opened
        // rendering with the canvas attached and set the full-target viewport
        // and scissor; this must not do either itself.
        nri::ClearAttachmentDesc clear = {};
        clear.planes               = nri::PlaneBits::COLOR;
        clear.colorAttachmentIndex = 0;
        clear.value.color.f = { kCanvasClear[0], kCanvasClear[1], kCanvasClear[2], kCanvasClear[3] };
        core.CmdClearAttachments(context.cmd, &clear, 1, nullptr, 0);

        if (batch.spans.empty() || batch.vertices.empty() || batch.indices.empty())
            return;   // an empty frame is a cleared canvas, not an error

        // ---------------------------------------------------------------
        // Vertex + index streams, straight into this frame's ring slot.
        // ALLOCATED HERE, at RECORD time, and that is load-bearing: the
        // vehicle calls ring.BeginFrame(slot) AFTER BuildFrame, so anything
        // allocated during graph SETUP would land in the previous frame's
        // slot and be overwritten while the GPU reads it.
        //
        // The ring's buffers are CONSTANT|VERTEX|INDEX_BUFFER upload-heap
        // allocations owned by NriUploadRing, deliberately NOT graph
        // resources: HOST_UPLOAD memory is host-coherent and the frame-pacing
        // fence the swapchain already waits on is what orders the CPU write
        // against the GPU read, so they need no barrier and no declaration.
        //
        // Alignment is the caller's to supply (NriUploadRing::Allocate has no
        // default): the vertex stride for the VB, the index size for the IB.
        // ---------------------------------------------------------------
        const std::uint64_t vertexBytes = batch.vertices.size() * sizeof(Batch2DVertex);
        const std::uint64_t indexBytes  = batch.indices.size() * sizeof(std::uint32_t);

        const NriUploadRing::Alloc vertexAlloc = context.ring.Allocate(vertexBytes, sizeof(Batch2DVertex));
        const NriUploadRing::Alloc indexAlloc  = context.ring.Allocate(indexBytes, sizeof(std::uint32_t));
        if (!vertexAlloc.cpu || !indexAlloc.cpu)
        {
            GraphError("Batch2DNode: the upload ring could not fit this frame's vertex/index streams ("
                       + std::to_string(vertexBytes + indexBytes)
                       + " bytes) -- the batch is dropped this frame. Raise "
                         "kUploadRingBytesPerFrame in NriGraphContext.cpp.");
            return;
        }
        std::memcpy(vertexAlloc.cpu, batch.vertices.data(), (std::size_t)vertexBytes);
        std::memcpy(indexAlloc.cpu, batch.indices.data(), (std::size_t)indexBytes);

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("Batch2DNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        // Projection: canvas pixels (y down) -> clip space, exactly the
        // PushConstants the NVRHI End() sets.
        BatchRootConstants push;
        push.invHalfViewportX = batch.viewport.x > 0.0f ? 2.0f / batch.viewport.x : 0.0f;
        push.invHalfViewportY = batch.viewport.y > 0.0f ? 2.0f / batch.viewport.y : 0.0f;

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc setDesc = {};
        setDesc.setIndex      = 0;
        setDesc.descriptorSet = m_set;
        core.CmdSetDescriptorSet(context.cmd, setDesc);

        nri::SetRootConstantsDesc rootConstants = {};
        rootConstants.rootConstantIndex = 0;
        rootConstants.data              = &push;
        rootConstants.size              = sizeof(push);
        core.CmdSetRootConstants(context.cmd, rootConstants);

        nri::VertexBufferDesc vertexBuffer = {};
        vertexBuffer.buffer = vertexAlloc.buffer;
        vertexBuffer.offset = vertexAlloc.offset;
        vertexBuffer.stride = sizeof(Batch2DVertex);
        core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
        core.CmdSetIndexBuffer(context.cmd, *indexAlloc.buffer, indexAlloc.offset,
                                nri::IndexType::UINT32);

        // One draw per drained span, in the batcher's sorted order -- the same
        // loop End() runs, with the pipeline coming from the shared cache
        // instead of the batcher's own table.
        for (const Batch2DDrawSpan& span : batch.spans)
        {
            nri::Pipeline* pipeline = PipelineFor(span.material, canvasFormat);
            if (!pipeline)
                continue;   // already logged + latched by the cache
            core.CmdSetPipeline(context.cmd, *pipeline);

            nri::DrawIndexedDesc draw = {};
            draw.indexNum    = span.indexCount;
            draw.instanceNum = 1;
            draw.baseIndex   = span.firstIndex;
            core.CmdDrawIndexed(context.cmd, draw);
        }
    }

    RgTexture AddBatch2DNode(RenderGraph& graph, NriGraphContext* context,
                             std::uint32_t width, std::uint32_t height)
    {
        // Drained at DECLARATION time on purpose: it is pure CPU work (sort +
        // index/span build) with no ring allocation in it, and doing it here
        // means the exec fn only touches the GPU. The returned spans view the
        // batcher's own storage and stay valid until its next Begin(), which is
        // a whole frame away.
        Batch2DDrained drained;
        if (context)
        {
            if (Batcher2D* batcher = context->CurrentBatch())
                drained = batcher->Drain();
        }

        RgTexture canvas{};
        graph.AddNode("batch2d", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                RgTextureDesc desc;
                desc.format = kCanvasFormat;
                desc.width  = width;
                desc.height = height;
                canvas = builder.CreateTexture("canvas", desc);
                builder.Write(canvas, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&canvas, 1));
            },
            [context, drained](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // headless declaration-shape drive: no device, nothing to record
                if (Batch2DNode* node = context->Batch2D())
                    node->Record(nodeContext, drained, kCanvasFormat);
            });
        return canvas;
    }
}
