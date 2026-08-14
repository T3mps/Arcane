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

#include <Arcane/Assets/ImageIo.hpp>          // LoadPngRgba -- the engine's ONE image decode
#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>   // kSpriteMaterialCbSlot / kSpriteGlobalCbSlot / kSpriteMaterialTextureBase
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/Batcher2D.hpp>        // Batch2DDrained / Batch2DVertex / Batch2DDrawSpan
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/ShaderConventions.hpp>  // kVsEntry / kPsEntry

#undef ERROR

#include <cstring>
#include <filesystem>
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

        // The canvas is RGBA16F. Since Task 10 the constant lives beside the
        // frame's shape (NriGraphContext.hpp's kGraphCanvasFormat), because
        // the post-chain targets must be the SAME format as the canvas and two
        // files independently mirroring Canvas.cpp is exactly how they would
        // drift apart.
        constexpr nri::Format kCanvasFormat = kGraphCanvasFormat;

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

        // FNV-1a. Used for two different jobs below and the difference matters:
        //   * over the BYTECODE it produces NriPipelineCache::GraphicsKey::
        //     shaderPairId, which that cache's fill contract (rule 3) requires
        //     to change whenever the bytecode would -- a content hash satisfies
        //     that by construction, where a material id would not;
        //   * over the registration's identity fields it produces the stamp
        //     that decides whether a built material slot is still current.
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
        constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

        std::uint64_t HashBytes(const void* data, std::size_t size, std::uint64_t seed = kFnvOffset) noexcept
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            std::uint64_t hash = seed;
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= kFnvPrime;
            }
            return hash;
        }

        std::uint64_t HashValue(std::uint64_t value, std::uint64_t seed) noexcept
        {
            return HashBytes(&value, sizeof(value), seed);
        }
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
        m_owner     = &context;

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

        return CreateWhiteTexel() && CreateBindings() && CreateConstantArena();
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

        // ONE pool for the built-in set AND every registered material's sets.
        //
        // NOTHING IN IT IS EVER REWRITTEN WHILE THE GPU MIGHT READ IT, which is
        // what keeps ResetDescriptorPool (and its frame-slot fence discipline)
        // out of this file entirely. The built-in set binds the white texel and
        // is written once. A material's sets are per FRAME SLOT, so the set a
        // frame binds is the one whose constant-buffer region that same frame
        // owns -- and the only path that rewrites an existing set is a material
        // REBUILD (an asset re-save at the desk), which idles the device first
        // and says so. NRI's own guidance: "DescriptorSet is a tiny struct, so
        // lots of descriptor sets can be created in advance and reused without
        // calling ResetDescriptorPool" (NRI.h).
        //
        // Capacity is the hard cap kMaxMaterialSlots/kMaxMaterialTextures name:
        // a pool's sizes are fixed at creation and a single set cannot be freed,
        // so the numbers are decided here rather than discovered mid-frame.
        constexpr std::uint32_t kMaterialSets = kMaxMaterialSlots * kSwapchainFramesInFlight;
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = 1 + kMaterialSets;
        // Per material set: the sprite's t0 plus its declared t1..N.
        poolDesc.textureMaxNum        = 1 + (1 + kMaxMaterialTextures) * kMaterialSets;
        poolDesc.samplerMaxNum        = 1 + kMaterialSets;
        // Per material set: material CB b1 (when the template has numeric
        // params) and globals CB b2.
        poolDesc.constantBufferMaxNum = 2 * kMaterialSets;
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

    void SpriteMaterialLayout::Build(std::uint32_t cbSize, std::uint32_t textureCount)
    {
        // sprite_material.hlsl's register map: root constants b0, and ONE
        // space-0 descriptor set carrying material CB b1 (only when the template
        // has numeric params -- the stitcher omits the cbuffer otherwise),
        // globals CB b2, the sprite's own texture t0, the declared textures
        // t1.., and the sampler s0.
        //
        // RANGE ORDER IS DELIBERATE. NRI's D3D12 backend merges CONSECUTIVE
        // ranges of the same D3D12 range type into one root table
        // (Source/D3D12/PipelineLayoutD3D12.hpp), so grouping the two CBVs, then
        // the SRVs, then the sampler costs three root parameters instead of
        // five -- and t0 with t1..N is ONE contiguous SRV range for the same
        // reason.
        rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = 16;   // BatchConstants: float2 + float2
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER;

        // Both stages: a material template's VERTEX_BODY may sample its declared
        // textures and read its params (sprite_material.hlsl's displace()), so
        // narrowing these to FRAGMENT would break a displacing material on
        // D3D12, where visibility is a hard root-signature property. Mirrors the
        // NVRHI layout's setVisibility(ShaderType::All).
        constexpr nri::StageBits kBothStages =
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        for (nri::DescriptorRangeDesc& range : ranges)
            range = {};
        materialCb = globalsCb = textures = sampler = kNoRange;

        std::uint32_t rangeCount = 0;
        if (cbSize > 0)
        {
            materialCb = rangeCount;
            ranges[rangeCount].baseRegisterIndex = kSpriteMaterialCbSlot;   // b1
            ranges[rangeCount].descriptorNum     = 1;
            ranges[rangeCount].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
            ranges[rangeCount].shaderStages      = kBothStages;
            ++rangeCount;
        }
        globalsCb = rangeCount;
        ranges[rangeCount].baseRegisterIndex = kSpriteGlobalCbSlot;         // b2
        ranges[rangeCount].descriptorNum     = 1;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        textures = rangeCount;
        // t0 is the sprite's own texture; the declared params follow at
        // kSpriteMaterialTextureBase (t1), which is why ONE range starting at 0
        // covers both.
        static_assert(kSpriteMaterialTextureBase == 1,
                      "the sprite's own texture is t0 and declared params follow it contiguously");
        ranges[rangeCount].baseRegisterIndex = 0;
        ranges[rangeCount].descriptorNum     = 1 + textureCount;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        sampler = rangeCount;
        ranges[rangeCount].baseRegisterIndex = 0;                           // s0
        ranges[rangeCount].descriptorNum     = 1;
        ranges[rangeCount].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[rangeCount].shaderStages      = kBothStages;
        ++rangeCount;

        set = {};
        set.registerSpace = 0;
        set.ranges        = ranges;
        set.rangeNum      = rangeCount;

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed). Two materials with the same (cbSize>0, textureCount) shape
        // therefore SHARE one layout id, which is the point.
        //
        // rootDescriptorNum and rootSamplerNum stay ZERO, and that is THE
        // REGISTER-SPACE RULE in force: NRI refuses rootRegisterSpace == a set's
        // registerSpace only when one of those two is nonzero
        // (Source/Validation/DeviceVal.hpp), and the shaders leave us no choice
        // but to put both in space 0.
        desc = {};
        desc.rootRegisterSpace = 0;
        desc.rootConstants     = &rootConstant;
        desc.rootConstantNum   = 1;
        desc.descriptorSets    = &set;
        desc.descriptorSetNum  = 1;
        desc.shaderStages      = kBothStages;
    }

    bool Batch2DNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // The CALLER supplies constant-buffer alignment everywhere on this path
        // (NriUploadRing::Allocate has no default either), and this is where it
        // comes from. It also fixes the SIZE every CB view is created with:
        // NRI passes BufferViewDesc::size straight into
        // D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes, which D3D12 requires to
        // be a multiple of 256 -- so the views name a whole region, not the
        // template's byte count, and the shader simply reads less than it.
        m_arenaStride = CbRegionStride(deviceDesc.memoryAlignment.constantBufferOffset);

        // RESERVED, not merely sized: EnsureMaterial hands PrepareMaterials a
        // MaterialSlot* out of this vector and then keeps building, so a
        // reallocation mid-frame would dangle it. The cap is the reservation, so
        // the vector never grows past it.
        m_materials.reserve(kMaxMaterialSlots);

        const std::uint64_t regionsPerFrame = kMaxMaterialSlots + 1;
        const std::uint64_t arenaBytes = regionsPerFrame * kSwapchainFramesInFlight * m_arenaStride;

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the material constant-buffer arena ({} bytes) could "
                      "not be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph batch material CBs");

        // Persistent map, unmapped once in Release()/~Batch2DNode -- the same
        // shape NriUploadRing uses, and the same NONE-backend footgun: MapBuffer
        // returns null unconditionally there, so this node is [gpu]-only from
        // here down.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: the material constant-buffer arena could not be "
                      "mapped (the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        // The globals CB view per frame slot, created ONCE. Its contents change
        // every frame; its (buffer, offset) never does, which is exactly what
        // lets a descriptor set naming it be written once too.
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.offset = ArenaOffset(slot, 0);
            viewDesc.size   = m_arenaStride;
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_globalsView[slot]))
                || !m_globalsView[slot])
            {
                ARC_ERROR("[nri-graph] Batch2DNode: the globals constant-buffer view for frame slot "
                          "{} could not be created", slot);
                return false;
            }
        }
        return true;
    }

    Batch2DNode::~Batch2DNode()
    {
        if (!m_device || (!m_white && !m_whiteView && !m_sampler && !m_pool && !m_arena))
            return;

        ARC_WARN("[nri-graph] Batch2DNode destroyed with live NRI objects -- either Create() failed "
                 "part way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        if (m_pool)      core.DestroyDescriptorPool(m_pool);
        for (MaterialSlot& slot : m_materials)
            for (nri::Descriptor*& view : slot.cbView)
                if (view) { core.DestroyDescriptor(view); view = nullptr; }
        for (nri::Descriptor*& view : m_globalsView)
            if (view) { core.DestroyDescriptor(view); view = nullptr; }
        if (m_arena)
        {
            if (m_arenaCpu) core.UnmapBuffer(*m_arena);
            core.DestroyBuffer(m_arena);
        }
        for (auto& [id, resident] : m_textures)
        {
            if (resident.view)    core.DestroyDescriptor(resident.view);
            if (resident.texture) core.DestroyTexture(resident.texture);
        }
        m_textures.clear();
        m_materials.clear();
        m_materialSlotOf.clear();
        if (m_sampler)   core.DestroyDescriptor(m_sampler);
        if (m_whiteView) core.DestroyDescriptor(m_whiteView);
        if (m_white)     core.DestroyTexture(m_white);
        m_pool = nullptr; m_set = nullptr;
        m_arena = nullptr; m_arenaCpu = nullptr;
        m_sampler = nullptr; m_whiteView = nullptr; m_white = nullptr;
    }

    void Batch2DNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture (or, for
        // the constant buffers below, its arena).
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            m_set  = nullptr;   // owned by the pool; not separately destroyable
        }

        // The material half: CB views, then the arena they name; texture views,
        // then the textures they view.
        for (MaterialSlot& slot : m_materials)
        {
            for (nri::Descriptor*& view : slot.cbView)
            {
                if (!view)
                    continue;
                graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
                view = nullptr;
            }
            for (nri::DescriptorSet*& set : slot.set)
                set = nullptr;   // owned by the pool, buried above
        }
        m_materials.clear();
        m_materialSlotOf.clear();
        m_materialRefused.clear();

        for (nri::Descriptor*& view : m_globalsView)
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

        for (auto& [id, resident] : m_textures)
        {
            if (resident.view)
                graveyard.Bury(fence, [core, d = resident.view] { core->DestroyDescriptor(d); });
            if (resident.texture)
                graveyard.Bury(fence, [core, t = resident.texture] { core->DestroyTexture(t); });
        }
        m_textures.clear();

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
            // A REGISTERED material that PrepareMaterials could not build (or
            // was never offered -- a caller that skipped Prepare). Falling back
            // to the plain sprite pipeline draws the right geometry with the
            // wrong shader rather than dropping the content, which is the same
            // degradation Batcher2D::QuadMaterial already applies to an unknown
            // id. The reason it could not be built was reported there.
            if (!m_warnedRegisteredMaterial)
            {
                m_warnedRegisteredMaterial = true;
                ARC_WARN("[nri-graph] Batch2DNode: material id {} has no NRI pipeline -- the graph "
                         "path draws it through the plain sprite pipeline instead", material);
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

    // =====================================================================
    // REGISTERED MATERIALS (Task 9)
    // =====================================================================

    nri::Descriptor* Batch2DNode::EnsureTexture(const Guid& id)
    {
        if (!id.IsValid())
            return nullptr;   // unbound param -- the white texel, same as the NVRHI path

        const auto cached = m_textures.find(id);
        if (cached != m_textures.end())
            return cached->second.view;   // null for a load this node already failed

        // Remembered BEFORE any early return, so a failure is attempted once
        // rather than re-resolved every frame.
        ResidentTexture& resident = m_textures[id];

        const auto reportFallback = [&](const std::string& why)
        {
            if (!m_warnedTextureFallback)
            {
                m_warnedTextureFallback = true;
                ARC_WARN("[nri-graph] Batch2DNode: material texture {} is not resident on the graph "
                         "device -- {}; the white texel is bound in its place (further occurrences "
                         "are silent)", id.ToString(), why);
            }
        };

        const std::optional<std::filesystem::path> path =
            m_owner ? m_owner->ResolveAsset(id) : std::nullopt;
        if (!path)
        {
            reportFallback("no asset resolver, or the Guid is not in the project registry");
            return nullptr;
        }

        // The ENGINE's decode, not a second one: LoadPngRgba is the device-free
        // half Assets::GetTexture uses (Assets/ImageIo.hpp), so both devices
        // read the same file through the same stb call and get the same bytes.
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        if (!LoadPngRgba(*path, width, height, rgba) || width == 0 || height == 0 || rgba.empty())
        {
            reportFallback("the image at " + path->string() + " could not be decoded");
            return nullptr;
        }
        // nri::Dim_t is 16-bit, and the cast below would silently wrap -- the
        // same refusal RenderGraph::RealizePool makes for a transient's extent.
        if (width > 0xFFFFu || height > 0xFFFFu)
        {
            reportFallback("the image at " + path->string() + " is "
                           + std::to_string(width) + "x" + std::to_string(height)
                           + ", which nri::Dim_t (16-bit) cannot express");
            return nullptr;
        }

        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        // SRGB, matching Assets::GetTexture's nvrhi::Format::SRGBA8_UNORM: the
        // canvas is linear and the hardware does the decode, so a UNORM here
        // would render the same asset visibly brighter than the NVRHI path.
        textureDesc.format    = nri::Format::RGBA8_SRGB;
        textureDesc.width     = (nri::Dim_t)width;
        textureDesc.height    = (nri::Dim_t)height;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;   // the NVRHI path uploads mip 0 only too
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, resident.texture))
            || !resident.texture)
        {
            reportFallback("CreateCommittedTexture failed");
            resident.texture = nullptr;
            return nullptr;
        }
        core.SetDebugName(resident.texture, path->filename().string().c_str());

        // Through NRI's OWN helper, exactly as the white texel is: UploadData
        // submits and waits internally, which is why this whole function runs at
        // DECLARATION time and never from inside Record's open command buffer.
        nri::HelperInterface helper = {};
        if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface), &helper)))
        {
            reportFallback("HelperInterface unavailable");
            return nullptr;
        }

        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = rgba.data();
        subresource.sliceNum   = 1;
        subresource.rowPitch   = width * 4;
        subresource.slicePitch = width * height * 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = resident.texture;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            reportFallback("the texture upload failed");
            return nullptr;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = resident.texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, resident.view)) || !resident.view)
        {
            reportFallback("the shader-resource view could not be created");
            resident.view = nullptr;
            return nullptr;
        }

        ARC_INFO("[nri-graph] Batch2DNode: material texture {} ({}x{}) is resident on the graph device",
                 path->filename().string(), width, height);
        return resident.view;
    }

    bool Batch2DNode::BuildMaterial(MaterialSlot& slot, const Material2DDesc& desc)
    {
        const nri::CoreInterface& core = m_device->Core();
        const std::uint32_t textureCount = desc.templ->TextureCount();
        const std::uint32_t cbSize       = desc.templ->CbSize();

        if (textureCount > kMaxMaterialTextures)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: material '{}' declares {} textures, over this node's "
                      "cap of {} -- raise kMaxMaterialTextures (it sizes the descriptor pool)",
                      desc.templ->Name(), textureCount, kMaxMaterialTextures);
            return false;
        }
        if (cbSize > kMaterialCbMaxBytes)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: material '{}' has a {}-byte constant buffer, over "
                      "this node's arena region of {} -- raise kMaterialCbMaxBytes",
                      desc.templ->Name(), cbSize, kMaterialCbMaxBytes);
            return false;
        }

        // THE LAYOUT -- see SpriteMaterialLayout (and THE REGISTER-SPACE RULE in
        // this file's header) for the whole of its shape and its reasoning. It
        // is a separate object so the headless [nri] cases can assert that shape
        // without a device.
        SpriteMaterialLayout layout2D;
        layout2D.Build(cbSize, textureCount);
        static_assert(sizeof(BatchRootConstants) == 16,
                      "SpriteMaterialLayout::Build hardcodes the b0 block size");

        const std::uint32_t previousLayout = slot.layoutId;
        slot.layoutId = m_pipelines->RegisterLayout(layout2D.desc);
        nri::PipelineLayout* layout = m_pipelines->Layout(slot.layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] Batch2DNode: pipeline-layout registration failed for material '{}'",
                      desc.templ->Name());
            return false;
        }
        // A REBUILD whose SHAPE changed (the re-saved snippet gained or lost a
        // param or a texture decl). A descriptor set is allocated against one
        // layout's set-0 shape, so the existing sets no longer fit and must be
        // replaced -- and NRI cannot free the old ones, so they are abandoned
        // inside the pool until Release(). Rare (a structural edit at the desk)
        // and self-limiting: a run that does it often enough exhausts the pool
        // and gets the loud allocation failure below, which names the constant.
        if (previousLayout != NriPipelineCache::kInvalidLayout && previousLayout != slot.layoutId)
        {
            ARC_INFO("[nri-graph] Batch2DNode: material '{}' changed binding SHAPE -- its descriptor "
                     "sets are re-allocated (the previous ones are stranded in the pool until "
                     "shutdown)", desc.templ->Name());
            for (nri::DescriptorSet*& set : slot.set)
                set = nullptr;
        }

        // The PSO key. shaderPairId is the CONTENT hash of the blob pair, which
        // is what the cache's fill contract (rule 3) needs: a recompile of the
        // same material id produces different bytes, therefore a different key,
        // therefore a new pipeline -- where an id-derived value would have
        // served the stale one.
        slot.vs = desc.vsBytes;
        slot.ps = desc.psBytes;
        slot.shaderPairId = HashBytes(slot.ps->data(), slot.ps->size(),
                                      HashBytes(slot.vs->data(), slot.vs->size()));
        // Keeps registered materials clear of the small literal bases the
        // built-ins and TonemapNode use in this shared cache.
        slot.shaderPairId |= 0x8000000000000000ull;

        slot.cbSize = cbSize;
        slot.packed.assign(cbSize, 0);

        // ---------------------------------------------------------------
        // The per-frame-slot constant-buffer views and descriptor sets.
        // Allocated ONCE per material slot and only rewritten on a rebuild,
        // which idles first (EnsureMaterial) -- so nothing here is ever written
        // while a frame in flight reads it.
        // ---------------------------------------------------------------
        const std::vector<Guid> textureIds = desc.instance->ResolveTextures();
        nri::Descriptor* textures[1 + kMaxMaterialTextures] = {};
        textures[0] = m_whiteView;   // t0: the sprite's own texture -- THE TEXTURE GAP
        for (std::uint32_t t = 0; t < textureCount; ++t)
        {
            nri::Descriptor* view = t < textureIds.size() ? EnsureTexture(textureIds[t]) : nullptr;
            textures[1 + t] = view ? view : m_whiteView;
        }

        for (std::uint32_t frameSlot = 0; frameSlot < kSwapchainFramesInFlight; ++frameSlot)
        {
            if (cbSize > 0 && !slot.cbView[frameSlot])
            {
                nri::BufferViewDesc viewDesc = {};
                viewDesc.buffer = m_arena;
                viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
                viewDesc.offset = ArenaOffset(frameSlot, slot.cbRegion);
                viewDesc.size   = m_arenaStride;
                if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, slot.cbView[frameSlot]))
                    || !slot.cbView[frameSlot])
                {
                    ARC_ERROR("[nri-graph] Batch2DNode: the material constant-buffer view for '{}' "
                              "(frame slot {}) could not be created", desc.templ->Name(), frameSlot);
                    return false;
                }
            }

            if (!slot.set[frameSlot]
                && (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                                &slot.set[frameSlot], 1, 0))
                    || !slot.set[frameSlot]))
            {
                ARC_ERROR("[nri-graph] Batch2DNode: descriptor-set allocation failed for material "
                          "'{}' -- the pool holds {} sets, sized by kMaxMaterialSlots",
                          desc.templ->Name(), 1 + kMaxMaterialSlots * kSwapchainFramesInFlight);
                return false;
            }

            // EVERY `descriptors` SOURCE BELOW MUST OUTLIVE THE
            // UpdateDescriptorRanges CALL AT THE BOTTOM OF THIS BLOCK.
            // UpdateDescriptorRangeDesc::descriptors is a POINTER TO AN ARRAY of
            // descriptors (NRIDescs.h:1101), dereferenced inside the call -- so
            // a single descriptor is passed as the address of a variable, and
            // that variable has to still be alive when the call runs. Hence
            // `cb` is declared HERE, in the same scope as its siblings, rather
            // than inside the `cbSize > 0` block that fills it: taking &cb from
            // a narrower scope leaves updates[] holding a pointer into dead
            // storage, and the compiler is free to give that slot to `globals`
            // -- which would silently write the GLOBALS view into range b1 and
            // produce a well-formed descriptor NRI's validation cannot fault.
            nri::UpdateDescriptorRangeDesc updates[4] = {};
            std::uint32_t updateCount = 0;
            const nri::Descriptor* cb = nullptr;
            if (cbSize > 0)
            {
                cb = slot.cbView[frameSlot];
                updates[updateCount].descriptorSet = slot.set[frameSlot];
                updates[updateCount].rangeIndex    = layout2D.materialCb;
                updates[updateCount].descriptors   = &cb;
                updates[updateCount].descriptorNum = 1;
                ++updateCount;
            }
            const nri::Descriptor* globals = m_globalsView[frameSlot];
            updates[updateCount].descriptorSet = slot.set[frameSlot];
            updates[updateCount].rangeIndex    = layout2D.globalsCb;
            updates[updateCount].descriptors   = &globals;
            updates[updateCount].descriptorNum = 1;
            ++updateCount;

            const nri::Descriptor* const* textureArray = textures;
            updates[updateCount].descriptorSet = slot.set[frameSlot];
            updates[updateCount].rangeIndex    = layout2D.textures;
            updates[updateCount].descriptors   = textureArray;
            updates[updateCount].descriptorNum = 1 + textureCount;
            ++updateCount;

            const nri::Descriptor* sampler = m_sampler;
            updates[updateCount].descriptorSet = slot.set[frameSlot];
            updates[updateCount].rangeIndex    = layout2D.sampler;
            updates[updateCount].descriptors   = &sampler;
            updates[updateCount].descriptorNum = 1;
            ++updateCount;

            core.UpdateDescriptorRanges(updates, updateCount);
        }

        slot.ready = true;
        return true;
    }

    Batch2DNode::MaterialSlot* Batch2DNode::EnsureMaterial(Batcher2D& batcher, std::uint16_t material)
    {
        if (m_materialRefused.contains(material))
            return nullptr;   // already reported, once

        const Material2DDesc* desc = batcher.MaterialDesc(material);
        const auto refuse = [&](const char* why) -> MaterialSlot*
        {
            m_materialRefused.insert(material);
            ARC_WARN("[nri-graph] Batch2DNode: material id {} cannot be drawn through its own "
                     "pipeline -- {}. It falls back to the plain sprite pipeline for this run.",
                     material, why);
            return nullptr;
        };
        if (!desc || !desc->templ || !desc->instance)
            return refuse("the batcher has no registration for it");
        if (!desc->vsBytes || !desc->psBytes || desc->vsBytes->empty() || desc->psBytes->empty())
            return refuse("its registration carries no stitched shader bytecode "
                          "(Material2DDesc::vsBytes)");

        // The identity of THIS registration. A material re-save replaces the
        // whole Material2DDesc (SpriteMaterialCache -> Batcher2D::
        // UpdateMaterial), so every pointer below moves with it; the sizes and
        // texture Guids are folded in so a same-address reallocation of a
        // different blob is caught too. Values are NOT in it -- PackCB runs
        // every frame, exactly as Batcher2D::End does, so a live param edit
        // needs no rebuild.
        std::uint64_t stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->vsBytes.get(), kFnvOffset);
        stamp = HashValue(desc->vsBytes->size(), stamp);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->psBytes.get(), stamp);
        stamp = HashValue(desc->psBytes->size(), stamp);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->templ.get(), stamp);
        stamp = HashValue((std::uint64_t)(std::uintptr_t)desc->instance.get(), stamp);
        for (const Guid& id : desc->instance->ResolveTextures())
        {
            stamp = HashValue(id.hi, stamp);
            stamp = HashValue(id.lo, stamp);
        }

        const auto known = m_materialSlotOf.find(material);
        if (known != m_materialSlotOf.end())
        {
            MaterialSlot& slot = m_materials[known->second];
            if (slot.ready && slot.stamp == stamp)
                return &slot;

            // A REBUILD: the registration behind this id changed (an asset
            // re-save at the desk). Its descriptor sets are about to be
            // rewritten and NRI cannot free one, so they are REUSED rather than
            // re-allocated -- which means the GPU must not be reading them.
            // Idling here is honest: the edit already cost a shader compile, and
            // the alternative (a fresh set per rebuild) drains a fixed pool.
            ARC_INFO("[nri-graph] Batch2DNode: material id {} was re-registered -- rebuilding its "
                     "pipeline and bindings behind a device idle", material);
            (void)ARC_NRI_CHECK(m_device->Core().DeviceWaitIdle(&m_device->Device()));
            const nri::CoreInterface& core = m_device->Core();
            for (nri::Descriptor*& view : slot.cbView)
            {
                if (!view)
                    continue;
                core.DestroyDescriptor(view);   // safe behind the idle above
                view = nullptr;
            }
            slot.ready = false;
            slot.stamp = stamp;
            if (!BuildMaterial(slot, *desc))
                return refuse("its pipeline or bindings could not be rebuilt");
            return &slot;
        }

        if (m_materials.size() >= kMaxMaterialSlots)
            return refuse("this node's material-slot table is full -- raise kMaxMaterialSlots "
                          "(it sizes the descriptor pool and the constant-buffer arena)");

        MaterialSlot& slot = m_materials.emplace_back();
        slot.cbRegion = (std::uint32_t)m_materials.size();   // region 0 is the globals CB
        slot.stamp    = stamp;
        if (!BuildMaterial(slot, *desc))
        {
            // Left in the table (its sets/views are already allocated out of a
            // pool that cannot free them) but never marked ready, so Record
            // draws it through the plain sprite pipeline.
            return refuse("its pipeline or bindings could not be built");
        }
        m_materialSlotOf[material] = (std::uint32_t)(m_materials.size() - 1);
        return &slot;
    }

    void Batch2DNode::PrepareMaterials(Batcher2D& batcher, const Batch2DDrained& batch,
                                       nri::Format canvasFormat)
    {
        // PACK-ONCE-PER-BATCH, the same dedup Batcher2D::End applies with its
        // `packedThisBatch` flag: the sort key groups by material but a material
        // can still own several spans (a different texture between them splits
        // the run), and re-resolving a whole MaterialInstance parent chain per
        // span would be per-draw work for a per-material answer.
        for (MaterialSlot& slot : m_materials)
            slot.packedThisBatch = false;

        for (const Batch2DDrawSpan& span : batch.spans)
        {
            if (span.material < kBuiltInCount)
                continue;

            MaterialSlot* slot = EnsureMaterial(batcher, span.material);
            if (!slot || !slot->ready || slot->packedThisBatch)
                continue;   // already reported once, or already done this frame
            slot->packedThisBatch = true;

            // The PSO, built HERE so a first-frame pipeline compile does not
            // land inside the recording window either.
            NriPipelineCache::GraphicsKey key = {};
            key.shaderPairId    = slot->shaderPairId;
            key.layoutId        = slot->layoutId;
            key.colorFormats[0] = canvasFormat;
            key.colorCount      = 1;
            key.depthFormat     = nri::Format::UNKNOWN;
            key.topology        = nri::Topology::TRIANGLE_LIST;
            key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;

            nri::ShaderDesc stages[2] = {};
            stages[0].stage          = nri::StageBits::VERTEX_SHADER;
            stages[0].bytecode       = slot->vs->data();
            stages[0].size           = slot->vs->size();
            stages[0].entryPointName = kVsEntry;
            stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
            stages[1].bytecode       = slot->ps->data();
            stages[1].size           = slot->ps->size();
            stages[1].entryPointName = kPsEntry;

            slot->pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
                {
                    desc.vertexInput = &m_vertexInput;
                    desc.shaders     = stages;
                    desc.shaderNum   = 2;
                    desc.rasterization.fillMode = nri::FillMode::SOLID;
                    desc.rasterization.cullMode = nri::CullMode::NONE;
                });
            if (!slot->pipeline)
            {
                // The cache already logged + latched. Refuse the material for
                // the rest of the run rather than clearing `ready`, which would
                // make EnsureMaterial take its REBUILD branch -- and therefore
                // idle the device -- on every subsequent frame.
                m_materialRefused.insert(span.material);
                ARC_WARN("[nri-graph] Batch2DNode: material id {} has no pipeline for the canvas "
                         "format -- it falls back to the plain sprite pipeline for this run",
                         span.material);
                continue;
            }

            // The values, packed on the CPU while there is no command buffer
            // open. Every frame, unconditionally: MaterialInstance resolves its
            // parent chain here and a live param edit must show up without a
            // rebuild -- which is exactly what Batcher2D::End does too.
            if (slot->cbSize > 0)
            {
                const Material2DDesc* desc = batcher.MaterialDesc(span.material);
                if (desc && desc->instance)
                    desc->instance->PackCB(slot->packed.data(), slot->packed.size());
            }
        }
    }

    void Batch2DNode::Record(RenderGraphNodeContext& context, const Batch2DDrained& batch,
                             nri::Format canvasFormat, std::uint32_t frameSlot)
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

        nri::PipelineLayout* builtInLayout = m_pipelines->Layout(m_layoutId);
        if (!builtInLayout)
        {
            GraphError("Batch2DNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // The constant buffers, into THIS frame slot's arena regions. Written
        // here rather than at declaration time for the same reason the ring's
        // allocations are: the frame-pacing fence this slot is safe behind is
        // waited inside Execute (the swapchain acquire), which is upstream of
        // every exec fn and downstream of every declaration. HOST_UPLOAD memory
        // is host-coherent, so a memcpy is the whole of the upload -- no
        // barrier, no flush, exactly as the ring's streams need none.
        //
        // The globals block is 16 bytes (GlobalParams) and the material blocks
        // were PACKED on the CPU in PrepareMaterials; all that is left is the
        // copy. Nothing here is written when the batch drew no registered
        // material, and the arena is untouched in that case.
        // ---------------------------------------------------------------
        auto* arena = static_cast<std::uint8_t*>(m_arenaCpu);
        if (arena && batch.globals)
            std::memcpy(arena + ArenaOffset(frameSlot, 0), batch.globals, sizeof(GlobalParams));
        if (arena)
        {
            for (MaterialSlot& slot : m_materials)
            {
                if (!slot.ready || !slot.packedThisBatch || slot.packed.empty())
                    continue;
                std::memcpy(arena + ArenaOffset(frameSlot, slot.cbRegion),
                            slot.packed.data(), slot.packed.size());
            }
        }

        // Projection: canvas pixels (y down) -> clip space, exactly the
        // PushConstants the NVRHI End() sets. The SAME block for every pipeline
        // here -- sprite.hlsl and sprite_material.hlsl declare an identical b0.
        BatchRootConstants push;
        push.invHalfViewportX = batch.viewport.x > 0.0f ? 2.0f / batch.viewport.x : 0.0f;
        push.invHalfViewportY = batch.viewport.y > 0.0f ? 2.0f / batch.viewport.y : 0.0f;

        core.CmdSetDescriptorPool(context.cmd, *m_pool);

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
        //
        // The LAYOUT and the SET are now per-span, not per-frame: a registered
        // material has its own of each (it carries two constant buffers and its
        // declared textures). CmdSetPipelineLayout invalidates the bound sets
        // and root constants on both backends, so a layout change re-binds all
        // three -- while a SET change alone (two materials sharing one layout,
        // which the dedup makes the common case for materials of the same
        // shape) re-binds only the set. Tracking BOTH is load-bearing: keying
        // the rebind on the layout alone would silently leave the previous
        // material's textures and constants bound.
        nri::PipelineLayout* lastLayout = nullptr;
        nri::DescriptorSet*  lastSet    = nullptr;
        for (const Batch2DDrawSpan& span : batch.spans)
        {
            nri::Pipeline*       pipeline = nullptr;
            nri::PipelineLayout* layout   = nullptr;
            nri::DescriptorSet*  set      = nullptr;

            // Everything a registered material needs was resolved at
            // declaration time (PrepareMaterials) -- this is three reads, not a
            // cache lookup: the recording window must not contain a PSO compile.
            const auto known = span.material >= kBuiltInCount
                             ? m_materialSlotOf.find(span.material)
                             : m_materialSlotOf.end();
            if (known != m_materialSlotOf.end() && m_materials[known->second].ready)
            {
                const MaterialSlot& slot = m_materials[known->second];
                pipeline = slot.pipeline;
                layout   = m_pipelines->Layout(slot.layoutId);
                set      = slot.set[frameSlot];
            }

            if (!pipeline || !layout || !set)
            {
                // Built-in, or a registered material that could not be honoured
                // -- PipelineFor states which and warns once.
                pipeline = PipelineFor(span.material, canvasFormat);
                layout   = builtInLayout;
                set      = m_set;
            }
            if (!pipeline)
                continue;   // already logged + latched by the cache

            if (layout != lastLayout)
            {
                lastLayout = layout;
                lastSet    = nullptr;   // a layout change invalidates the bound set
                core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

                nri::SetRootConstantsDesc rootConstants = {};
                rootConstants.rootConstantIndex = 0;
                rootConstants.data              = &push;
                rootConstants.size              = sizeof(push);
                core.CmdSetRootConstants(context.cmd, rootConstants);
            }
            if (set != lastSet)
            {
                lastSet = set;
                nri::SetDescriptorSetDesc setDesc = {};
                setDesc.setIndex      = 0;
                setDesc.descriptorSet = set;
                core.CmdSetDescriptorSet(context.cmd, setDesc);
            }

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
            {
                drained = batcher->Drain();
                // ...and the REGISTERED materials it names are resolved here
                // too, for the same reason: this is the last point before the
                // frame's command buffer opens, and making a material's texture
                // resident goes through a helper that submits and waits. See
                // PrepareMaterials.
                if (Batch2DNode* node = context->Batch2D())
                    node->PrepareMaterials(*batcher, drained, kCanvasFormat);
            }
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
                    node->Record(nodeContext, drained, kCanvasFormat, context->FrameSlot());
            });
        return canvas;
    }
}
