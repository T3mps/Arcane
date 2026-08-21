// MeshNode -- see the header for what this node owns, what it deliberately is
// NOT, and where it sits in the frame.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "MeshNode.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry

#undef ERROR

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // The offline artifacts this node loads. Their stems' _vs/_ps suffixes
        // agree with mesh.hlsl's vs_main/ps_main entry points, which is the
        // INVARIANT compile-shaders.bat states at the top of itself.
        constexpr const char* kMeshVs = "mesh_vs";
        constexpr const char* kMeshPs = "mesh_ps";

        // THE DEPTH CLEAR. 1.0 is "far" under the engine's FORWARD-Z, [0,1]
        // convention (SceneCamera.hpp's DEPTH CONVENTION block -- reverse-Z is
        // a separately-decided-against choice), which is the value that pairs
        // with CompareOp::LESS below. Flipping one without the other is
        // exactly the mistake that renders an empty frame.
        constexpr float kDepthClear = 1.0f;

        // data/shaders/mesh.hlsl's MeshConstants (b0 / the VK push-constant
        // block): a 4x4 model matrix followed by a float4 tint.
        struct MeshRootConstants
        {
            glm::mat4 model{1.0f};
            glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
        };
        static_assert(sizeof(MeshRootConstants) == 80, "must match mesh.hlsl's MeshConstants");

        // ...and its MeshFrameCB (b1). std140/HLSL cbuffer packing rules put
        // each float4 on its own 16-byte boundary, which is what the three
        // glm::vec4s below are for -- a glm::vec3 member would pack to 12
        // bytes here and misalign everything after it.
        struct MeshFrameConstants
        {
            glm::mat4 viewProjection{1.0f};
            glm::vec4 lightDirection{0.0f, 0.0f, 1.0f, 0.0f};
            glm::vec4 lightColor{1.0f, 1.0f, 1.0f, 0.0f};
            glm::vec4 ambient{0.0f, 0.0f, 0.0f, 0.0f};
        };
        static_assert(sizeof(MeshFrameConstants) == 112, "must match mesh.hlsl's MeshFrameCB");
        static_assert(sizeof(MeshFrameConstants) <= MeshNode::kFrameCbMaxBytes,
                      "the frame constants must fit one arena region");

        // Every element finite. A NaN or Inf clip position is UNDEFINED on the
        // GPU rather than a wrong picture, and SceneCamera::PerspectiveProjection
        // is an unvalidated pure function (a zero fov, aspectRatio <= 0 or
        // nearZ >= farZ all produce them silently), so the pass checks what it
        // is handed rather than trusting the caller to have used the guarded
        // path. See MeshSceneDesc's camera comment.
        [[nodiscard]] bool IsFinite(const glm::mat4& m) noexcept
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(m[c][r]))
                        return false;
            return true;
        }
    }

    std::unique_ptr<MeshNode> MeshNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<MeshNode> node(new MeshNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool MeshNode::Init(NriGraphContext& context)
    {
        m_device       = &context.Device();
        m_pipelines    = &context.Pipelines();
        m_textureCache = context.Textures();

        m_vs = context.ShaderBytecode(kMeshVs);
        m_ps = context.ShaderBytecode(kMeshPs);
        if (m_vs.empty() || m_ps.empty())
        {
            ARC_ERROR("[nri-graph] MeshNode: shader bin '{}'/'{}' is missing -- the opaque mesh "
                      "pass cannot be built", kMeshVs, kMeshPs);
            return false;
        }

        // The vertex input, in members because GraphicsPipelineDesc::vertexInput
        // is a pointer the cache dereferences after `fill` returns. Layout is
        // MeshVertex (MeshBuilder.hpp) and the semantic names are mesh.hlsl's
        // VSInput.
        m_attributes[0].d3d.semanticName = "POSITION";
        m_attributes[0].vk.location      = 0;
        m_attributes[0].offset           = offsetof(MeshVertex, position);
        m_attributes[0].format           = nri::Format::RGB32_SFLOAT;
        m_attributes[1].d3d.semanticName = "NORMAL";
        m_attributes[1].vk.location      = 1;
        m_attributes[1].offset           = offsetof(MeshVertex, normal);
        m_attributes[1].format           = nri::Format::RGB32_SFLOAT;
        m_attributes[2].d3d.semanticName = "TEXCOORD";
        m_attributes[2].vk.location      = 2;
        m_attributes[2].offset           = offsetof(MeshVertex, uv);
        m_attributes[2].format           = nri::Format::RG32_SFLOAT;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(MeshVertex);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        // Reserved once so Record()'s per-frame upload table never allocates
        // inside the recording window -- the same rule every other node on this
        // path keeps.
        m_uploads.reserve(kInitialUploadSlots);

        return CreateWhiteTexel() && CreateBindings() && CreateConstantArena();
    }

    bool MeshNode::CreateWhiteTexel()
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
            ARC_ERROR("[nri-graph] MeshNode: the 1x1 white texel could not be created");
            return false;
        }
        core.SetDebugName(m_white, "nri-graph mesh white");

        // Through NRI's OWN helper rather than a hand-rolled staging buffer +
        // transition pair, for the reason Batch2DNode gives at the identical
        // call: the phase's rule is that graph code records no CmdBarrier of
        // its own, and the helper means this file contains none at all.
        nri::HelperInterface helper = {};
        if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface), &helper)))
        {
            ARC_ERROR("[nri-graph] MeshNode: HelperInterface unavailable -- cannot upload the white texel");
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
            ARC_ERROR("[nri-graph] MeshNode: the white texel upload failed");
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
            ARC_ERROR("[nri-graph] MeshNode: the white texel's shader-resource view could not be created");
            return false;
        }
        return true;
    }

    nri::DescriptorPoolDesc MeshNode::PoolSizes() noexcept
    {
        // ONE set family per (albedo, frame slot): the nil/white one plus up to
        // kMaxAlbedoTextures real ones. Each set carries exactly three
        // descriptors -- b1, t0, s0 -- so the three per-type maxima are all the
        // same number as the set count.
        constexpr std::uint32_t kSets =
            (1 + kMaxAlbedoTextures) * kSwapchainFramesInFlight;

        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = kSets;
        poolDesc.textureMaxNum        = kSets;
        poolDesc.samplerMaxNum        = kSets;
        poolDesc.constantBufferMaxNum = kSets;
        return poolDesc;
    }

    bool MeshNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // Linear + REPEAT, unlike Batch2DNode's clamped sampler: a mesh's UVs
        // are a surface parametrization that legitimately tiles, where a sprite
        // atlas's must not bleed across cell edges.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min  = nri::Filter::LINEAR;
        samplerDesc.filters.mag  = nri::Filter::LINEAR;
        samplerDesc.filters.mip  = nri::Filter::LINEAR;
        samplerDesc.addressModes = { nri::AddressMode::REPEAT, nri::AddressMode::REPEAT,
                                     nri::AddressMode::REPEAT };
        samplerDesc.mipMax       = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] MeshNode: sampler creation failed");
            return false;
        }

        // THE LAYOUT. mesh.hlsl's register map: b0 root constants (model +
        // tint), and ONE space-0 descriptor set carrying the frame CB b1, the
        // albedo t0 and the sampler s0.
        //
        // THE REGISTER-SPACE RULE (Batch2DNode.hpp's header states it in full):
        // NRI refuses rootRegisterSpace == a set's registerSpace only when the
        // layout carries root DESCRIPTORS or root SAMPLERS. This layout carries
        // neither -- root CONSTANTS are exempt, because VK lowers them to push
        // constants which live outside the set-space entirely -- so both may be
        // space 0, which mesh.hlsl's implicit space0 leaves no choice about.
        //
        // RANGE ORDER: CBV, then SRV, then sampler. NRI's D3D12 backend merges
        // consecutive ranges of the same D3D12 range type into one root table,
        // so this shape costs three root parameters rather than a scattered
        // more.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = sizeof(MeshRootConstants);
        // BOTH stages: the vertex shader reads `model`, the pixel shader reads
        // `baseColor`. Narrowing this to VERTEX would break the tint on D3D12,
        // where root-parameter visibility is a hard root-signature property.
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        // b1 is read by both stages too (viewProjection in the VS, the light in
        // the PS); t0/s0 are fragment-only.
        nri::DescriptorRangeDesc ranges[3] = {};
        ranges[0].baseRegisterIndex = 1;                       // b1
        ranges[0].descriptorNum     = 1;
        ranges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        ranges[0].shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        ranges[1].baseRegisterIndex = 0;                       // t0
        ranges[1].descriptorNum     = 1;
        ranges[1].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[1].shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        ranges[2].baseRegisterIndex = 0;                       // s0
        ranges[2].descriptorNum     = 1;
        ranges[2].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[2].shaderStages      = nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc setDesc = {};
        setDesc.registerSpace = 0;
        setDesc.ranges        = ranges;
        setDesc.rangeNum      = 3;

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
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] MeshNode: pipeline layout registration failed");
            return false;
        }

        // The pool. NOTHING IN IT IS EVER REWRITTEN WHILE THE GPU MIGHT READ
        // IT -- every set is written once, on first use of its albedo, and
        // never again -- which is what keeps ResetDescriptorPool and its fence
        // discipline out of this file, exactly as in Batch2DNode.
        const nri::DescriptorPoolDesc poolDesc = PoolSizes();
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] MeshNode: descriptor pool creation failed");
            return false;
        }
        return true;
    }

    bool MeshNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // The CALLER supplies constant-buffer alignment everywhere on this
        // path, and this is where it comes from. It also fixes the SIZE every
        // CB view is created with: NRI passes BufferViewDesc::size straight
        // into D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes, which D3D12
        // requires to be a multiple of 256 -- so the views name a whole region
        // and the shader simply reads less than it.
        m_arenaStride = CbRegionStride(deviceDesc.memoryAlignment.constantBufferOffset);
        const std::uint64_t arenaBytes = m_arenaStride * kSwapchainFramesInFlight;

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] MeshNode: the frame constant-buffer arena ({} bytes) could not "
                      "be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph mesh frame CBs");

        // Persistent map, unmapped once in Release()/~MeshNode -- the same
        // shape NriUploadRing and Batch2DNode use, and the same NONE-backend
        // footgun: MapBuffer returns null unconditionally there, so this node
        // is [gpu]-only from here down.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] MeshNode: the frame constant-buffer arena could not be mapped "
                      "(the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // The b1 view per frame slot, created ONCE. Its contents change every
        // frame; its (buffer, offset) never does, which is exactly what lets a
        // descriptor set naming it be written once too.
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.offset = ArenaOffset(slot);
            viewDesc.size   = m_arenaStride;
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_frameCbView[slot]))
                || !m_frameCbView[slot])
            {
                ARC_ERROR("[nri-graph] MeshNode: the frame constant-buffer view for frame slot {} "
                          "could not be created", slot);
                return false;
            }
        }

        // THE NIL-ALBEDO FAMILY -- the white texel, allocated and written once,
        // here. It is what an untextured instance binds and the fallback for
        // every refusal below.
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            if (!layout
                || !ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                               &m_whiteSets.set[slot], 1, 0))
                || !m_whiteSets.set[slot])
            {
                ARC_ERROR("[nri-graph] MeshNode: descriptor-set allocation failed for the white "
                          "texel (frame slot {})", slot);
                return false;
            }
        }
        WriteSets(m_whiteSets, m_whiteView);
        return true;
    }

    MeshNode::~MeshNode()
    {
        if (!m_device || (!m_white && !m_whiteView && !m_sampler && !m_pool && !m_arena))
            return;

        ARC_WARN("[nri-graph] MeshNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        if (m_pool) core.DestroyDescriptorPool(m_pool);
        for (nri::Descriptor*& view : m_frameCbView)
            if (view) { core.DestroyDescriptor(view); view = nullptr; }
        if (m_arena)
        {
            if (m_arenaCpu) core.UnmapBuffer(*m_arena);
            core.DestroyBuffer(m_arena);
        }
        // The sets are owned by the pool destroyed above, and the TEXTURES they
        // view belong to the vehicle's shared NriTextureCache, which releases
        // its own -- this node owns neither.
        m_albedoSets.clear();
        m_whiteSets = AlbedoSets{};
        if (m_sampler)   core.DestroyDescriptor(m_sampler);
        if (m_whiteView) core.DestroyDescriptor(m_whiteView);
        if (m_white)     core.DestroyTexture(m_white);
        m_pool = nullptr;
        m_arena = nullptr; m_arenaCpu = nullptr;
        m_sampler = nullptr; m_whiteView = nullptr; m_white = nullptr;
    }

    void MeshNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture (or, for
        // the constant buffer below, its arena).
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
        }
        // Sets are owned by the pool buried above; the images behind them
        // belong to the vehicle's shared NriTextureCache, which the vehicle
        // releases itself.
        m_albedoSets.clear();
        m_whiteSets = AlbedoSets{};

        for (nri::Descriptor*& view : m_frameCbView)
        {
            if (!view)
                continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
        }
        if (m_arena)
        {
            // Unmapped HERE rather than in the burial: the map is a CPU-side
            // fact with no GPU lifetime, and leaving it live inside a deferred
            // thunk would mean a mapped pointer this object still advertises
            // outlives the object.
            if (m_arenaCpu)
            {
                core->UnmapBuffer(*m_arena);
                m_arenaCpu = nullptr;
            }
            graveyard.Bury(fence, [core, b = m_arena] { core->DestroyBuffer(b); });
            m_arena = nullptr;
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
        m_pipeline = nullptr;   // owned by the shared cache; the vehicle clears it
    }

    nri::Pipeline* MeshNode::PipelineFor(nri::Format canvasFormat)
    {
        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = canvasFormat;
        key.colorCount      = 1;
        // THE DEPTH FORMAT IS PART OF THE PSO's IDENTITY -- NRI bakes the
        // depth-stencil format into a graphics pipeline exactly as it does the
        // colour formats, and binding one inside a CmdBeginRendering whose
        // depth attachment carries a different format is undefined on both
        // backends.
        key.depthFormat     = kGraphDepthFormat;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        // OPAQUE. The pass writes depth, so blending would be order-dependent
        // against a depth test that is order-INdependent -- the two do not
        // combine. Transparency is a separate pass, and not this slice's.
        key.blend           = NriPipelineCache::GraphicsKey::Blend::Opaque;

        // `stages` lives in THIS frame, which encloses the GetGraphics call --
        // the fill contract's rule 2. The bytecode it points at is owned by the
        // vehicle's bin cache and outlives this node, which the same rule needs
        // (CreateGraphicsPipeline runs after the callback returns).
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;   // SPIR-V matches by name; DXIL ignores it
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_ps.data();
        stages[1].size           = m_ps.size();
        stages[1].entryPointName = kPsEntry;

        return m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;

            // ============ THE WINDING, and why it is THIS pair ============
            // MeshBuilder emits triangles whose (v0,v1,v2) order is
            // COUNTER-CLOCKWISE as seen from OUTSIDE the surface
            // (MeshBuilder.hpp's WINDING block, algebraically verified there
            // for all six cube faces and for the sphere).
            //
            // The camera is right-handed and looks down -Z with +Y up
            // (SceneCamera.hpp, Task 5), and the executor sets a viewport with
            // originBottomLeft = false -- which on Vulkan is NRI's negative
            // height flip, so BOTH backends put NDC +Y at the TOP of the
            // target. An outward-facing triangle therefore lands on screen in
            // the order a viewer reads as counter-clockwise.
            //
            // `frontCounterClockwise = true` is what names that orientation
            // front-facing, and BACK culling then drops the far side of a
            // closed mesh. NRI maps this flag 1:1 to
            // D3D12_RASTERIZER_DESC::FrontCounterClockwise and to
            // VK_FRONT_FACE_COUNTER_CLOCKWISE (Source/D3D12/PipelineD3D12.hpp,
            // Source/VK/PipelineVK.hpp), so one value covers both -- and NRI's
            // own samples, which draw glTF content (CCW front faces, by that
            // spec), set the same `true`.
            //
            // THE DESK CHECK THAT FALSIFIES THIS IN ONE LOOK: get the pair
            // backwards and a closed CONVEX mesh (the cube, the sphere)
            // disappears entirely rather than rendering subtly wrong. There is
            // no plausible-looking failure mode here.
            desc.rasterization.fillMode              = nri::FillMode::SOLID;
            desc.rasterization.cullMode              = nri::CullMode::BACK;
            desc.rasterization.frontCounterClockwise = true;

            // ============ THE DEPTH TEST ============
            // FORWARD-Z, [0,1]: near maps to 0 and far to 1
            // (SceneCamera.hpp's DEPTH CONVENTION -- reverse-Z is a
            // separately-decided-against choice), so LESS is "nearer wins" and
            // the clear value is 1.0 (kDepthClear above). The pass is opaque,
            // so it writes depth as well as testing it.
            desc.outputMerger.depth.compareOp = nri::CompareOp::LESS;
            desc.outputMerger.depth.write     = true;
        });
    }

    std::uint32_t MeshNode::DistinctAlbedoCount(std::span<const MeshInstance> instances)
    {
        // Small by construction (a scene's distinct albedo images), so a flat
        // scan beats a set allocation -- and this runs once per frame, never
        // per instance.
        std::vector<Guid> seen;
        for (const MeshInstance& instance : instances)
        {
            if (!instance.albedo.IsValid())
                continue;
            bool known = false;
            for (const Guid& id : seen)
                known = known || id == instance.albedo;
            if (!known)
                seen.push_back(instance.albedo);
        }
        return (std::uint32_t)seen.size();
    }

    nri::Descriptor* MeshNode::TextureView(const Guid& id)
    {
        // ONE shared cache on the vehicle, so an image named by this node and
        // by the 2D batch is uploaded once. The miss warn lives there too -- it
        // is where the miss happens.
        if (!m_textureCache || !m_textureCache->Resolve(id))
            return nullptr;
        return m_textureCache->View(id);
    }

    void MeshNode::WriteSets(AlbedoSets& sets, nri::Descriptor* albedoView)
    {
        const nri::CoreInterface& core = m_device->Core();
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            if (!sets.set[slot])
                continue;

            // EVERY `descriptors` SOURCE BELOW MUST OUTLIVE THE
            // UpdateDescriptorRanges CALL: UpdateDescriptorRangeDesc::
            // descriptors is a POINTER TO AN ARRAY dereferenced inside the
            // call, so a single descriptor is passed as the address of a
            // variable that has to still be alive when the call runs. Hence
            // all three locals are declared in THIS scope.
            const nri::Descriptor* cb      = m_frameCbView[slot];
            const nri::Descriptor* texture = albedoView ? albedoView : m_whiteView;
            const nri::Descriptor* sampler = m_sampler;

            nri::UpdateDescriptorRangeDesc updates[3] = {};
            updates[0].descriptorSet = sets.set[slot];
            updates[0].rangeIndex    = 0;   // b1
            updates[0].descriptors   = &cb;
            updates[0].descriptorNum = 1;
            updates[1].descriptorSet = sets.set[slot];
            updates[1].rangeIndex    = 1;   // t0
            updates[1].descriptors   = &texture;
            updates[1].descriptorNum = 1;
            updates[2].descriptorSet = sets.set[slot];
            updates[2].rangeIndex    = 2;   // s0
            updates[2].descriptors   = &sampler;
            updates[2].descriptorNum = 1;
            core.UpdateDescriptorRanges(updates, 3);
        }
    }

    const MeshNode::AlbedoSets* MeshNode::EnsureAlbedoSets(const Guid& id)
    {
        // A nil id is the untextured case -- the white-texel family, written
        // once at Create.
        if (!id.IsValid())
            return &m_whiteSets;

        const auto known = m_albedoSets.find(id);
        if (known != m_albedoSets.end())
            return known->second.set[0] ? &known->second : &m_whiteSets;

        nri::Descriptor* view = TextureView(id);
        if (!view)
        {
            // Not resident (NriTextureCache said why, once). Memoized as an
            // EMPTY family so this does not re-ask the cache every frame.
            m_albedoSets[id] = AlbedoSets{};
            return &m_whiteSets;
        }

        // Counts LIVE families, not map entries: a memoized miss holds no
        // descriptor sets, and letting unresolvable images spend the budget
        // would refuse one that actually resolved.
        std::size_t live = 0;
        for (const auto& [key, existing] : m_albedoSets)
            if (existing.set[0])
                ++live;
        if (live >= kMaxAlbedoTextures)
        {
            if (!m_warnedTextureBudget)
            {
                m_warnedTextureBudget = true;
                ARC_ERROR("[nri-graph] MeshNode: more than {} distinct albedo images -- the rest "
                          "draw with the white texel. Raise kMaxAlbedoTextures (it sizes the "
                          "descriptor pool), or wait for the bindless material table.",
                          kMaxAlbedoTextures);
            }
            m_albedoSets[id] = AlbedoSets{};
            return &m_whiteSets;
        }

        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        AlbedoSets fresh;
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            if (!layout
                || !ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                               &fresh.set[slot], 1, 0))
                || !fresh.set[slot])
            {
                ARC_ERROR("[nri-graph] MeshNode: descriptor-set allocation failed for albedo {} -- "
                          "it draws with the white texel. The pool holds {} sets, sized by "
                          "kMaxAlbedoTextures.", id.ToString(),
                          (1 + kMaxAlbedoTextures) * kSwapchainFramesInFlight);
                // Memoized as a miss for the same reason as above: a pool that
                // is out of sets will not have more next frame.
                m_albedoSets[id] = AlbedoSets{};
                return &m_whiteSets;
            }
        }

        // Written ONCE, here, and never again -- the discipline every set in
        // this node obeys (see the pool comment in CreateBindings), which is
        // what keeps ResetDescriptorPool and its fence rules out of this file.
        WriteSets(fresh, view);
        const auto inserted = m_albedoSets.emplace(id, fresh);
        return &inserted.first->second;
    }

    const MeshNode::AlbedoSets* MeshNode::AlbedoSetsFor(const Guid& id) const
    {
        // RECORD-TIME ONLY: a pure lookup over what Prepare already built. It
        // must never allocate or upload -- both would happen inside the frame's
        // open command buffer. A miss is the white texel, which is also what a
        // caller that skipped Prepare entirely gets.
        if (!id.IsValid())
            return &m_whiteSets;
        const auto known = m_albedoSets.find(id);
        return (known != m_albedoSets.end() && known->second.set[0]) ? &known->second : &m_whiteSets;
    }

    void MeshNode::Prepare(const MeshSceneDesc& scene, nri::Format canvasFormat)
    {
        // THE BUDGET, checked ONCE up front from the same pure count the [nri]
        // cases pin -- so the number in the message is the scene's real
        // distinct-albedo count rather than "the seventeenth one I happened to
        // reach". EnsureAlbedoSets still enforces per-image (it is the
        // allocation point); both share m_warnedTextureBudget, so a scene over
        // budget says this exactly once.
        const std::uint32_t distinct = DistinctAlbedoCount(scene.instances);
        if (distinct > kMaxAlbedoTextures && !m_warnedTextureBudget)
        {
            m_warnedTextureBudget = true;
            ARC_ERROR("[nri-graph] MeshNode: this scene names {} distinct albedo images, over this "
                      "node's cap of {} -- the ones past the cap draw with the white texel. Raise "
                      "kMaxAlbedoTextures (it sizes the descriptor pool).", distinct,
                      kMaxAlbedoTextures);
        }
        for (const MeshInstance& instance : scene.instances)
            (void)EnsureAlbedoSets(instance.albedo);

        // The PSO, built HERE so a first-frame pipeline compile does not land
        // inside the recording window. Re-resolved every frame because the
        // canvas format is the caller's and a cache HIT is a linear scan over a
        // handful of entries.
        m_pipeline = PipelineFor(canvasFormat);
    }

    void MeshNode::Record(RenderGraphNodeContext& context, const MeshSceneDesc& scene,
                          std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        // ============ THE DEPTH CLEAR, and ONLY the depth clear ============
        // Graph attachments are LOAD/STORE and the declaration API carries no
        // clear op, so a node that needs a cleared target clears it from its
        // own exec fn (NriGraphContext::DeclareGraphFrame's THE CLEAR SEAM).
        // The depth target is a fresh transient pool slot whose contents are
        // undefined, so this is mandatory rather than tidy.
        //
        // THE COLOUR PLANE IS NOT CLEARED: batch2d already cleared the canvas
        // and drew this frame's 2D content into it, and clearing here would
        // erase that. That asymmetry is the whole reason this node clears by
        // PLANE rather than by attachment.
        nri::ClearAttachmentDesc clear = {};
        clear.planes = nri::PlaneBits::DEPTH;
        clear.value.depthStencil.depth   = kDepthClear;
        clear.value.depthStencil.stencil = 0;
        core.CmdClearAttachments(context.cmd, &clear, 1, nullptr, 0);

        if (scene.instances.empty())
            return;   // a scene with no geometry is a cleared depth buffer, not an error

        if (!m_pipeline)
        {
            if (!m_warnedNoPipeline)
            {
                m_warnedNoPipeline = true;
                ARC_WARN("[nri-graph] MeshNode: no pipeline for the canvas format -- the opaque "
                         "pass draws nothing this run (Prepare was skipped, or the cache refused "
                         "it and said why)");
            }
            return;
        }

        // THE CAMERA. Checked rather than trusted -- see IsFinite's comment.
        if (!IsFinite(scene.view) || !IsFinite(scene.projection))
        {
            if (!m_warnedBadCamera)
            {
                m_warnedBadCamera = true;
                GraphError("MeshNode: the scene's view or projection matrix is not finite (a zero "
                           "fov, a non-positive aspect ratio or nearZ >= farZ all produce NaN "
                           "silently) -- the opaque pass is dropped rather than recording a draw "
                           "with an undefined clip position");
            }
            return;
        }

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("MeshNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // THE FRAME CONSTANTS, into THIS frame slot's arena region. Written
        // here rather than at declaration time because the frame-pacing fence
        // this slot is safe behind is waited inside Execute (the swapchain
        // acquire), which is upstream of every exec fn and downstream of every
        // declaration. HOST_UPLOAD memory is host-coherent, so a memcpy is the
        // whole of the upload -- no barrier, no flush.
        // ---------------------------------------------------------------
        MeshFrameConstants frameConstants;
        frameConstants.viewProjection = scene.projection * scene.view;
        frameConstants.lightDirection = glm::vec4(scene.lightDirection, 0.0f);
        frameConstants.lightColor     = glm::vec4(scene.lightColor, 0.0f);
        frameConstants.ambient        = glm::vec4(scene.ambient, 0.0f);
        if (auto* arena = static_cast<std::uint8_t*>(m_arenaCpu))
            std::memcpy(arena + ArenaOffset(frameSlot), &frameConstants, sizeof(frameConstants));

        // ---------------------------------------------------------------
        // Vertex + index streams, straight into this frame's ring slot.
        // ALLOCATED HERE, at RECORD time, and that is load-bearing: the
        // vehicle calls ring.BeginFrame(slot) AFTER BuildFrame, so anything
        // allocated during graph SETUP would land in the previous frame's slot
        // and be overwritten while the GPU reads it.
        //
        // DEDUPED BY MeshData POINTER: a scene of twenty cubes is one upload
        // and twenty draws, not twenty uploads. The table is a reserved member
        // so this allocates nothing inside the recording window.
        // ---------------------------------------------------------------
        m_uploads.clear();
        // Returns BY VALUE, deliberately: the table is a growable vector and a
        // borrowed pointer into it would dangle the moment the next distinct
        // mesh pushes past the reservation. An Upload is six words.
        const auto uploadFor = [&](const MeshData* mesh) -> Upload
        {
            for (const Upload& existing : m_uploads)
                if (existing.mesh == mesh)
                    return existing;

            Upload fresh;
            fresh.mesh = mesh;

            const std::uint64_t vertexBytes = mesh->vertices.size() * sizeof(MeshVertex);
            const std::uint64_t indexBytes  = mesh->indices.size() * sizeof(std::uint32_t);
            const NriUploadRing::Alloc vertexAlloc = context.ring.Allocate(vertexBytes, sizeof(MeshVertex));
            const NriUploadRing::Alloc indexAlloc  = context.ring.Allocate(indexBytes, sizeof(std::uint32_t));
            if (!vertexAlloc.cpu || !indexAlloc.cpu)
            {
                if (!m_warnedRingOverflow)
                {
                    m_warnedRingOverflow = true;
                    GraphError("MeshNode: the upload ring could not fit this frame's mesh streams ("
                               + std::to_string(vertexBytes + indexBytes)
                               + " bytes) -- those instances are dropped this frame. Raise "
                                 "kUploadRingBytesPerFrame in NriGraphContext.cpp.");
                }
                // Memoized as a FAILED upload (ok stays false) so a scene with
                // twenty instances of one oversized mesh retries the ring once,
                // not twenty times.
                m_uploads.push_back(fresh);
                return fresh;
            }
            std::memcpy(vertexAlloc.cpu, mesh->vertices.data(), (std::size_t)vertexBytes);
            std::memcpy(indexAlloc.cpu, mesh->indices.data(), (std::size_t)indexBytes);

            fresh.vertexBuffer = vertexAlloc.buffer;
            fresh.vertexOffset = vertexAlloc.offset;
            fresh.indexBuffer  = indexAlloc.buffer;
            fresh.indexOffset  = indexAlloc.offset;
            fresh.indexCount   = (std::uint32_t)mesh->indices.size();
            fresh.ok           = true;
            m_uploads.push_back(fresh);
            return fresh;
        };

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        // ONE layout for the whole pass, so this is bound once. CmdSetPipeline
        // Layout invalidates the bound sets and root constants on both
        // backends, which is exactly why it must not be re-issued per draw.
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);
        core.CmdSetPipeline(context.cmd, *m_pipeline);

        const nri::DescriptorSet* lastSet = nullptr;
        const MeshData*           lastMesh = nullptr;
        for (const MeshInstance& instance : scene.instances)
        {
            if (!instance.mesh || instance.mesh->vertices.empty() || instance.mesh->indices.empty())
                continue;   // an empty slot is not an error -- see MeshInstance::mesh

            const Upload upload = uploadFor(instance.mesh);
            if (!upload.ok)
                continue;   // the ring said why, once

            // The set family Prepare built for this instance's albedo, or the
            // white-texel one. A pure lookup: nothing inside the recording
            // window may allocate a set.
            const AlbedoSets* sets = AlbedoSetsFor(instance.albedo);
            nri::DescriptorSet* set = sets ? sets->set[frameSlot] : nullptr;
            if (!set)
                continue;   // Create() failed part way; it already said so
            if (set != lastSet)
            {
                lastSet = set;
                nri::SetDescriptorSetDesc setDesc = {};
                setDesc.setIndex      = 0;
                setDesc.descriptorSet = set;
                core.CmdSetDescriptorSet(context.cmd, setDesc);
            }

            if (instance.mesh != lastMesh)
            {
                lastMesh = instance.mesh;
                nri::VertexBufferDesc vertexBuffer = {};
                vertexBuffer.buffer = upload.vertexBuffer;
                vertexBuffer.offset = upload.vertexOffset;
                vertexBuffer.stride = sizeof(MeshVertex);
                core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
                core.CmdSetIndexBuffer(context.cmd, *upload.indexBuffer, upload.indexOffset,
                                        nri::IndexType::UINT32);
            }

            MeshRootConstants push;
            push.model     = instance.model;
            push.baseColor = instance.baseColor;
            nri::SetRootConstantsDesc rootConstants = {};
            rootConstants.rootConstantIndex = 0;
            rootConstants.data              = &push;
            rootConstants.size              = sizeof(push);
            core.CmdSetRootConstants(context.cmd, rootConstants);

            nri::DrawIndexedDesc draw = {};
            draw.indexNum    = upload.indexCount;
            draw.instanceNum = 1;
            core.CmdDrawIndexed(context.cmd, draw);
        }
    }

    RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                          RgTexture canvas, const MeshSceneDesc& scene,
                          std::uint32_t width, std::uint32_t height)
    {
        // Resolved at DECLARATION time on purpose: making an albedo resident
        // goes through a helper that submits and waits, allocating a descriptor
        // set must not happen while a frame's command buffer is open, and a PSO
        // compile must not land inside the recording window. See
        // MeshNode::Prepare.
        if (context)
        {
            if (MeshNode* node = context->Mesh())
                node->Prepare(scene, kGraphCanvasFormat);
        }

        // `depth` is captured by reference ([&]) below, not shared_ptr -- safe
        // here only because the SETUP lambda is the one that mutates it and
        // AddNode runs setup synchronously (before this function returns), so
        // `depth` is still this frame's live stack local. The EXEC lambda below
        // captures nothing of it.
        RgTexture depth{};
        graph.AddNode("mesh", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                RgTextureDesc desc;
                desc.format       = kGraphDepthFormat;
                desc.width        = width;
                desc.height       = height;
                desc.depthStencil = true;
                depth = builder.CreateTexture("depth", desc);

                // BOTH targets, both written, both attached. The colour one
                // was minted by batch2d and already carries this frame's 2D
                // content -- this pass draws on top of it, which is why it is
                // a Write and not a Read.
                builder.Write(canvas, RgUsage::ColorWrite);
                builder.Write(depth, RgUsage::DepthWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&canvas, 1));
                graph.SetDepthAttachment(depth);
            },
            [context, scene](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // headless declaration-shape drive: no device, nothing to record
                if (MeshNode* node = context->Mesh())
                    node->Record(nodeContext, scene, context->FrameSlot());
            });
        return depth;
    }
}
