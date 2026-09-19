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

#include <Arcane/Render/Nri/GpuScene.hpp>                  // the instance / args / visible-index buffers Record draws from (F3 plan 1 T7)
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp>   // AddGpuSceneSyncNode -- declared ahead of the mesh node (F3 plan 1 T6)

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

        // THE ROOT BLOCK is MeshRootConstants (MeshNode.hpp, 8 bytes) since
        // F3 plan 1 T7. The 128-byte per-instance MeshConstants block that
        // lived here -- model + tint + NormalMatrixFor's three columns packed
        // as float4s, with the material slot bit-cast into col0.w because the
        // block sat at EXACTLY Vulkan's guaranteed-minimum push-constant
        // budget with zero headroom -- is gone: every one of those values is
        // now a field of the GPU scene's 240-byte row (GpuInstance), staged
        // by GpuSceneSync (registry rows) or by Prepare below (ad-hoc rows),
        // and read by the vertex shader through t0/space1. The root block
        // carries only WHICH row(s): a visible-index start for an indirect
        // batch, or the row itself for a direct draw. See `git log` for the
        // old struct if the packing account is ever needed again.

        // mesh.hlsl's MeshFrameCB (b1). std140/HLSL cbuffer packing rules put
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

        // glm::normalize divides by the length WITHOUT checking it, so a
        // zero-length light direction yields NaN in all three components --
        // which then propagates through N.L into every lit pixel, on a value
        // MeshSceneDesc explicitly allows a caller to leave at its default and
        // then zero out. Returning the zero vector instead makes that case mean
        // "no directional light": saturate(dot(n, 0)) is 0, so the surface
        // falls back to the ambient term, which is the only sensible reading.
        //
        // ON THE CPU, ONCE PER FRAME, rather than in the pixel shader: mesh.hlsl
        // consumes this value directly, so the guard cannot be bypassed by a
        // caller and costs one normalize per frame instead of one per pixel.
        // The epsilon is on the SQUARED length, so it is 1e-12 in length terms
        // -- far below any direction anyone would author and far above the
        // denormal range where the division itself misbehaves.
        [[nodiscard]] glm::vec3 SafeNormalize(const glm::vec3& v) noexcept
        {
            const float lengthSquared = glm::dot(v, v);
            if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
                return glm::vec3(0.0f);
            return v / std::sqrt(lengthSquared);
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
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        // THE GATE (Task 8/10 Step 1), FIRST -- before shader loads, before
        // any NRI object exists. NriDeviceCaps::SupportsBindless()'s FIRST
        // production call site: this node's whole material model is a
        // descriptor-indexed table, which means nothing on hardware that
        // cannot dynamically index a descriptor array (bindless tier 0).
        // The house refuse-loudly posture: producing wrong pixels (or
        // silently degrading to some other path) on tier-0 hardware is
        // worse than refusing to build the node at all, so this returns
        // false -- Create() turns that into a logged, latched null -- rather
        // than limping on with an unbuilt or half-built table.
        if (!m_device->Caps().SupportsBindless())
        {
            ARC_ERROR("[nri-graph] MeshNode: refused -- this device reports bindless tier 0 "
                      "(NriDeviceCaps::SupportsBindless() is false). The opaque mesh pass's "
                      "material table is a descriptor-indexed bindless array; there is no "
                      "correct way to build it on hardware that cannot dynamically index a "
                      "descriptor array, so the node is refused here rather than rendering "
                      "wrong pixels or silently degrading.");
            return false;
        }

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

        // Reserved once so Prepare()'s per-frame residency table keeps the STEADY
        // STATE free of heap traffic inside the declaration window. It is not an
        // absolute: a frame carrying more than kInitialResidentSlots distinct meshes
        // grows the vector once, on the high-water mark.
        m_residents.reserve(kInitialResidentSlots);

        return CreateBindless() && CreateBindings() && CreateConstantArena() && CreateSets();
    }

    bool MeshNode::CreateBindless()
    {
        // Gated already, in Init(), before this ever runs -- a device that
        // reached here has bindless tier > 0.
        m_bindless = BindlessTable::Create(*m_device, kBindlessCapacity);
        if (!m_bindless)
        {
            ARC_ERROR("[nri-graph] MeshNode: BindlessTable::Create failed -- the material table "
                      "could not be built (already logged: capacity {})", kBindlessCapacity);
            return false;
        }
        return true;
    }

    nri::DescriptorPoolDesc MeshNode::PoolSizes() noexcept
    {
        // TWO dimensions now (Task 8/10): kSwapchainFramesInFlight per-frame
        // sets, each carrying exactly ONE CONSTANT_BUFFER descriptor (b1);
        // and ONE bindless set carrying up to kBindlessCapacity TEXTURE
        // descriptors. The root sampler (CreateBindings) consumes NO pool
        // budget at all -- static/immutable samplers are not allocated from
        // a descriptor pool on either backend (NRIDescs.h:1077's own words).
        constexpr std::uint32_t kFrameSets = kSwapchainFramesInFlight;

        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum      = kFrameSets + 1;      // +1: the one bindless set
        poolDesc.constantBufferMaxNum     = kFrameSets;          // b1, one per frame slot
        poolDesc.structuredBufferMaxNum   = 2 * kFrameSets;      // t0 instances + t1 visible indices, per frame slot (F3 plan 1 T7)
        poolDesc.textureMaxNum            = kBindlessCapacity;   // the bindless array's own budget
        // ALLOW_UPDATE_AFTER_SET (Task 11): a POOL-level permission bit only --
        // it lets a set ALLOCATED from this pool opt into update-after-bind
        // (the bindless set since Task 11; the per-frame sets too since F3
        // plan 1 T7, for the t0/t1 rewrite Record does when the GPU scene's
        // buffers move). See AddMaterial's own synchronization comment and
        // the header's WHAT THIS NODE OWNS block.
        poolDesc.flags                    = nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET;
        return poolDesc;
    }

    bool MeshNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // THE LAYOUT (Task 8/10 rewrote this in full; F3 plan 1 T7 shrank
        // the root block and grew the frame set). mesh.hlsl's register map:
        // b0 root constants (the 8-byte MeshRootConstants {firstOutput,
        // flags} -- MeshNode.hpp) plus ONE immutable ROOT SAMPLER at s0,
        // both at the implicit rootRegisterSpace; and TWO ordinary
        // descriptor sets -- space1 = { b1 frame CB, t0 the GPU scene's
        // instance rows, t1 this slot's visible indices }, space2 = { t0
        // bindless material array }.
        //
        // THE REGISTER-SPACE RULE (Batch2DNode.hpp's header states it in
        // full, verified against Source/Validation/DeviceVal.hpp's
        // `rootDescriptorNum || rootSamplerNum` guard): NRI refuses
        // rootRegisterSpace == a set's registerSpace ONLY when the layout
        // carries a root DESCRIPTOR or root SAMPLER -- root CONSTANTS alone
        // are exempt, because VK lowers them to push constants, outside the
        // set-space numbering entirely. Before this task the layout carried
        // only root constants, so keeping the one descriptor set AND
        // rootRegisterSpace both at space0 was legal and is what
        // mesh.hlsl's then-implicit space0 registers required. Adding the
        // root sampler below trips that guard, so root items stay at
        // space0 (rootRegisterSpace unchanged -- b0 and s0 keep their
        // existing implicit-space0 HLSL registers, UNCHANGED text) and the
        // two ordinary sets move to space1/space2 instead (mesh.hlsl's b1/
        // t0 gain explicit `spaceN` annotations to match). This also has to
        // agree with the SPIR-V register-shift table dxc is invoked with
        // (compile-shaders.bat's SPIRV_FLAGS / ShaderConventions.hpp::
        // kSpirvArgs): NRI adds the SAME per-resource-type binding offset
        // to a range regardless of which space it is in
        // (Source/VK/PipelineLayoutVK.hpp's `bindingOffsets` array), so
        // dxc must shift b-registers in space1 and t-registers in space1/
        // space2 by the same amounts it already shifts space0's -- both
        // files carry `-fvk-b-shift 256 1` / `-fvk-t-shift 0 2` (Task 8/10)
        // and `-fvk-t-shift 0 1` (F3 plan 1 T7) for exactly that reason:
        // in SPIR-V, set 1 = { b1 at binding 257, t0 at 0, t1 at 1 }.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;                       // b0
        rootConstant.size          = sizeof(MeshRootConstants);   // 8 (the header static_asserts it)
        // BOTH stages, unchanged from Task 8/10 -- though only vs_main reads
        // the root block now (the tint and the material slot travel per row
        // and reach ps_main as varyings; spirv-dis of mesh_ps.bin shows no
        // push-constant block at all). A stage set that is a SUPERSET of the
        // readers is legal on both backends (a VK push-constant range may
        // name a stage that declares none; a D3D12 root parameter visible
        // to more stages than read it is merely unoptimised), and two
        // dwords of root budget is not worth narrowing something that has
        // been right on both backends since F2a.
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        // THE ONE IMMUTABLE SAMPLER (Task 10's slice-one sampler strategy,
        // spec section 6): trilinear -- LINEAR min/mag/mip, unchanged from
        // the sampler this replaces -- and REPEAT (a mesh's UVs tile),
        // anisotropy as the ONE tunable knob. A ROOT/static sampler
        // (NRIDescs.h:1022's own words), baked into the pipeline layout
        // itself rather than a descriptor-set entry: it consumes no pool
        // budget and needs no per-frame write -- CmdSetPipelineLayout
        // pushes it automatically on both backends (VK:
        // CommandBufferVK::SetPipelineLayout's "Push immutable samplers"
        // block; D3D12: a D3D12_STATIC_SAMPLER_DESC baked into the root
        // signature at creation) -- and Record()'s existing
        // CmdSetPipelineLayout call is already what does that; nothing new
        // to call here.
        nri::RootSamplerDesc rootSampler = {};
        rootSampler.registerIndex     = 0;                    // s0
        rootSampler.desc.filters.min  = nri::Filter::LINEAR;
        rootSampler.desc.filters.mag  = nri::Filter::LINEAR;
        rootSampler.desc.filters.mip  = nri::Filter::LINEAR;
        rootSampler.desc.addressModes = { nri::AddressMode::REPEAT, nri::AddressMode::REPEAT,
                                          nri::AddressMode::REPEAT };
        rootSampler.desc.anisotropy   = 16;    // THE ONE KNOB -- see the comment above
        rootSampler.desc.mipMax       = 16.0f;
        rootSampler.shaderStages      = nri::StageBits::FRAGMENT_SHADER;   // only ps_main samples

        // set (array index 0, space1), THREE ranges: b1, the per-frame-slot
        // frame CB -- read by both stages (viewProjection in the VS, the
        // light in the PS); then (F3 plan 1 T7) t0, the GPU scene's ONE
        // instance buffer (a STRUCTURED_BUFFER view, stride 240 --
        // GpuScene::InstancesView), and t1, THIS slot's visible-index buffer
        // (stride 4 -- GpuScene::VisibleIndicesView(slot)), both vertex-
        // only. Range indices 0/1/2 are what Record's UpdateDescriptorRanges
        // names. ALL THREE carry ALLOW_UPDATE_AFTER_SET (with the set-level
        // flag below): ranges 1-2 are rewritten by Record for a slot whose
        // buffers moved, and range 0 asks for it too so the set's ranges are
        // uniform (NRI maps the range flag to VK's per-binding UPDATE_AFTER_
        // BIND bit and D3D12's DATA_VOLATILE/DESCRIPTORS_VOLATILE -- either
        // is legal on a range that is never rewritten).
        nri::DescriptorRangeDesc frameRanges[3] = {};
        frameRanges[0].baseRegisterIndex = 1;                 // b1
        frameRanges[0].descriptorNum     = 1;
        frameRanges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        frameRanges[0].shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        frameRanges[1].baseRegisterIndex = 0;                 // t0 -- the instance rows
        frameRanges[1].descriptorNum     = 1;
        frameRanges[1].descriptorType    = nri::DescriptorType::STRUCTURED_BUFFER;
        frameRanges[1].shaderStages      = nri::StageBits::VERTEX_SHADER;
        frameRanges[2].baseRegisterIndex = 1;                 // t1 -- this slot's visible indices
        frameRanges[2].descriptorNum     = 1;
        frameRanges[2].descriptorType    = nri::DescriptorType::STRUCTURED_BUFFER;
        frameRanges[2].shaderStages      = nri::StageBits::VERTEX_SHADER;
        for (nri::DescriptorRangeDesc& r : frameRanges)
            r.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

        nri::DescriptorSetDesc frameSetDesc = {};
        frameSetDesc.registerSpace = 1;
        frameSetDesc.ranges        = frameRanges;
        frameSetDesc.rangeNum      = 3;
        frameSetDesc.flags         = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

        // set (array index 1, space2): the bindless material array at t0 --
        // a FIXED size (kBindlessCapacity, matched by mesh.hlsl's own
        // literal), not VARIABLE_SIZED_ARRAY: BindlessTable's capacity is
        // decided once at MeshNode creation and never resized
        // (BindlessTable.hpp's SLOT POLICY), so there is nothing for
        // AllocateDescriptorSets' variableDescriptorNum argument to do
        // here (CreateSets passes 0). PARTIALLY_BOUND: a table that has
        // not yet Added every slot leaves the unused tail genuinely
        // unwritten, which is legal by construction -- mesh.hlsl only ever
        // indexes a slot AddMaterial actually wrote -- rather than a
        // validation violation.
        // ALLOW_UPDATE_AFTER_SET (Task 11, on top of Task 10's ARRAY |
        // PARTIALLY_BOUND): lets AddMaterial's UpdateDescriptorRanges write a
        // NEW slot into this range AFTER the set has already been bound by an
        // earlier, possibly still-in-flight frame's command buffer -- exactly
        // what a live feed resolving textures mid-run needs (a mesh whose
        // albedo finishes cooking, or is referenced for the first time, on
        // frame N must not have to wait for every frame before N to retire
        // first). Both backends implement it fully (VK:
        // VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, gated on the device's
        // VK_EXT_descriptor_indexing feature bundle, which this device
        // already has -- SupportsBindless() above is gated on the SAME
        // Vulkan 1.2 `descriptorIndexing` feature, and NRI's device creation
        // enables every sub-feature the physical device reports, granular
        // update-after-bind bits included; D3D12: PipelineLayoutD3D12.hpp
        // marks the range DATA_VOLATILE/DESCRIPTORS_VOLATILE, which is
        // exactly this backend's "safe to rewrite a live table" idiom). See
        // AddMaterial's own doc comment for what this makes safe and what it
        // still does not (a slot, once written, is never rewritten or freed
        // -- BindlessTable's own SLOT POLICY -- so nothing here has to reason
        // about a draw that is CURRENTLY reading the slot being touched, only
        // about writing slots nothing has read yet).
        nri::DescriptorRangeDesc bindlessRange = {};
        bindlessRange.baseRegisterIndex = 0;                  // t0
        bindlessRange.descriptorNum     = kBindlessCapacity;
        bindlessRange.descriptorType    = nri::DescriptorType::TEXTURE;
        bindlessRange.shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        bindlessRange.flags             = nri::DescriptorRangeBits::ARRAY
                                         | nri::DescriptorRangeBits::PARTIALLY_BOUND
                                         | nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;

        nri::DescriptorSetDesc bindlessSetDesc = {};
        bindlessSetDesc.registerSpace = 2;
        bindlessSetDesc.ranges        = &bindlessRange;
        bindlessSetDesc.rangeNum      = 1;
        // The SET-level counterpart the range's flag requires (NRIDescs.h:
        // "ALLOW_UPDATE_AFTER_SET ... allows DescriptorRangeBits::
        // ALLOW_UPDATE_AFTER_SET") -- the per-frame frameSetDesc above
        // carries it too since F3 plan 1 T7 (its t0/t1 ranges).
        bindlessSetDesc.flags         = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;

        nri::DescriptorSetDesc setDescs[2] = { frameSetDesc, bindlessSetDesc };

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has to
        // be zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.rootConstants     = &rootConstant;
        layoutDesc.rootConstantNum   = 1;
        layoutDesc.rootSamplers      = &rootSampler;
        layoutDesc.rootSamplerNum    = 1;
        layoutDesc.descriptorSets    = setDescs;
        layoutDesc.descriptorSetNum  = 2;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] MeshNode: pipeline layout registration failed");
            return false;
        }

        // The pool. NOTHING IN IT IS EVER REWRITTEN WHILE THE GPU MIGHT READ
        // IT: a per-frame set's b1 is written once, at Create; its t0/t1 are
        // rewritten only for the CURRENT slot, whose previous frame retired
        // at BeginFrame (the header's WHAT THIS NODE OWNS block); and the
        // bindless set only ever gains slots nothing has read yet (see
        // AddMaterial's own synchronization comment). Nothing is ever FREED,
        // which is what keeps ResetDescriptorPool and its fence discipline
        // out of this file, exactly as in Batch2DNode.
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

        return true;
    }

    bool MeshNode::CreateSets()
    {
        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] MeshNode: no layout to allocate descriptor sets from");
            return false;
        }

        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            // setIndex 0: the ARRAY position of frameSetDesc in
            // CreateBindings' setDescs[2] -- an ARRAY INDEX, not a register
            // space (Source/VK/PipelineLayoutVK.hpp's `m_BindingInfo.sets`
            // is a 1:1 copy of `descriptorSets[]` in array order, and
            // `SetDescriptorSet` indexes it by that same position).
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                           &m_sets[slot], 1, 0))
                || !m_sets[slot])
            {
                ARC_ERROR("[nri-graph] MeshNode: descriptor-set allocation failed for frame slot "
                          "{} -- the pool holds {} sets (PoolSizes)", slot,
                          PoolSizes().descriptorSetMaxNum);
                return false;
            }

            // `cb`'s ADDRESS must outlive the UpdateDescriptorRanges call
            // below: UpdateDescriptorRangeDesc::descriptors is a POINTER TO
            // AN ARRAY dereferenced inside the call, so a single descriptor
            // is passed as the address of a variable that has to still be
            // alive when the call runs.
            const nri::Descriptor* cb = m_frameCbView[slot];

            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = m_sets[slot];
            update.rangeIndex    = 0;   // b1 -- ranges 1-2 (t0/t1, the GPU scene) are Record's, per slot
            update.descriptors   = &cb;
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }

        // THE BINDLESS SET -- setIndex 1 (bindlessSetDesc's array position),
        // allocated ONCE for the node's whole lifetime, not per frame slot:
        // unlike b1 there is nothing in it that differs frame to frame.
        // Its range is left UNWRITTEN here -- AddMaterial writes each slot
        // as it is Added, and CreateBindings' PARTIALLY_BOUND flag is what
        // makes allocating it with a still-empty range legal.
        if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 1, &m_bindlessSet, 1, 0))
            || !m_bindlessSet)
        {
            ARC_ERROR("[nri-graph] MeshNode: the bindless material descriptor set could not be "
                      "allocated -- the pool holds {} sets (PoolSizes)",
                      PoolSizes().descriptorSetMaxNum);
            return false;
        }
        return true;
    }

    std::uint32_t MeshNode::AddMaterial(nri::Descriptor* srv)
    {
        // See this method's own doc comment in MeshNode.hpp for the full
        // ownership and synchronization contract.
        if (!m_bindless || !m_bindlessSet)
            return BindlessTable::kInvalidSlot;

        const std::uint32_t slot = m_bindless->Add(srv);
        if (slot == BindlessTable::kInvalidSlot)
            return BindlessTable::kInvalidSlot;   // null srv, or the table is full -- BindlessTable
                                                   // already warned (once) or refused silently, per
                                                   // its own Add() contract

        const nri::CoreInterface& core = m_device->Core();
        // `view`'s ADDRESS must outlive UpdateDescriptorRanges -- same rule
        // CreateSets' `cb` local follows, and for the same reason.
        const nri::Descriptor* view = srv;

        nri::UpdateDescriptorRangeDesc update = {};
        update.descriptorSet  = m_bindlessSet;
        update.rangeIndex     = 0;      // the bindless array is the set's only range
        update.baseDescriptor = slot;   // WHERE in the array -- not an append
        update.descriptors    = &view;
        update.descriptorNum  = 1;
        core.UpdateDescriptorRanges(&update, 1);
        return slot;
    }

    MeshNode::~MeshNode()
    {
        // m_bindless resets UNCONDITIONALLY below, regardless of this guard
        // (and regardless of whether m_device is even non-null): BindlessTable's
        // own destructor is a no-op once Release() has emptied it (its guard is
        // `!m_device || m_slots.empty()`), and a genuine leak -- Release() never
        // called -- is exactly the case that destructor's own WARN + DeviceWaitIdle
        // + destroy exists to catch, so this function does not need to duplicate
        // that check, and a default-constructed MeshNode (m_bindless still null)
        // resets a null unique_ptr harmlessly.
        if (m_device && (m_pool || m_arena))
        {
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
            // The sets (including the bindless one) are owned by the pool
            // destroyed above -- NRI has no per-set destroy, so forgetting
            // the pointers is the whole of it.
            for (nri::DescriptorSet*& set : m_sets)
                set = nullptr;
            m_bindlessSet = nullptr;
            m_pool = nullptr;
            m_arena = nullptr; m_arenaCpu = nullptr;
        }
        m_bindless.reset();
    }

    void MeshNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // THE BINDLESS TABLE FIRST: BindlessTable::Release buries every
        // occupied slot's descriptor at `fence` (its own comment) --
        // discharging views this node's table owns per AddMaterial's
        // OWNERSHIP contract, NOT the textures they view (a caller's own,
        // per that same contract). The unique_ptr itself is NOT reset here
        // -- see ~MeshNode's comment for why an idempotent Release() leaving
        // an empty-but-alive BindlessTable behind is the right shape.
        if (m_bindless)
            m_bindless->Release(graveyard, fence);
        m_bindlessSet = nullptr;   // owned by the pool buried below

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture (or, for
        // the constant buffer below, its arena).
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
        }
        // Sets are owned by the pool buried above; NRI has no per-set destroy,
        // so forgetting the pointers is the whole of it.
        for (nri::DescriptorSet*& set : m_sets)
            set = nullptr;

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

    void MeshNode::Prepare(nri::Format canvasFormat, const MeshSceneDesc& scene,
                           NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter)
    {
        // The PSO, built HERE so a first-frame pipeline compile does not land
        // inside the recording window. Re-resolved every frame because the
        // canvas format is the CALLER's (AddMeshNode takes it as a parameter,
        // and a differently-formatted canvas must be a cache MISS rather than
        // a silent attachment mismatch) and a cache HIT is a linear scan over
        // a handful of entries.
        m_pipeline = PipelineFor(canvasFormat);

        // Residency at DECLARATION time -- UploadData submits and waits
        // internally. Record only looks this table up.
        //
        // GATED SEPARATELY FROM THE PIPELINE ABOVE, deliberately: a cache-less
        // context still gets its pipeline and still reports the real reason it drew
        // nothing, instead of a "missing pipeline" warning naming the wrong cause.
        m_residents.clear();
        m_adHocRows.clear();
        m_adHocDraws.clear();
        const auto resolveOnce = [&](const Guid& id)
        {
            if (id.IsNil() || !meshBuffers)
                return;
            for (const auto& e : m_residents)
                if (e.first == id)
                    return;
            m_residents.push_back({ id, meshBuffers->Resolve(id, frameCounter) });
        };

        // 1. The registry-backed batches: one resolve per distinct mesh.
        if (scene.scene)
            for (const GpuBatchDraw& batch : scene.scene->batches)
                resolveOnce(batch.mesh);

        // 2. The ad-hoc instances (F3 plan 1 T7): resolved the same way, AND
        //    staged as GpuInstance rows for the sync node's scratch region --
        //    built even with a null cache, since they are the sync node's
        //    input regardless of whether anything can be drawn. Capped at the
        //    scratch capacity: GpuScene::Reserve warns once for the overflow
        //    and Apply clamps to the same count, so row i here IS scratch row
        //    i on the device (Record's pushRoot depends on that).
        for (const MeshInstance& instance : scene.instances)
        {
            if (instance.mesh.IsNil())
                continue;   // an empty slot is not an error -- see MeshInstance::mesh
            if (m_adHocRows.size() >= GpuScene::kScratchRows)
                break;
            resolveOnce(instance.mesh);

            GpuInstance row;
            row.model     = instance.model;
            row.prevModel = instance.model;   // an ad-hoc row has no history: prev == model (the G2 contract's "new row")
            // NormalMatrixFor's singular guard means a degenerate instance
            // gets identity here rather than NaN reaching the GPU -- the same
            // product the old root block carried, now three row columns.
            const glm::mat3 n = NormalMatrixFor(instance.model);
            row.normal0      = glm::vec4(n[0], 0.0f);
            row.normal1      = glm::vec4(n[1], 0.0f);
            row.normal2      = glm::vec4(n[2], 0.0f);
            row.baseColor    = instance.baseColor;
            row.materialSlot = instance.materialSlot;
            m_adHocRows.push_back(row);
            m_adHocDraws.push_back(AdHocDraw{ instance.mesh, instance.indexOffset, instance.indexCount });
        }
    }

    void MeshNode::Record(RenderGraphNodeContext& context, const MeshSceneDesc& scene,
                          std::uint32_t frameSlot, GpuScene* gpuScene)
    {
        const nri::CoreInterface& core = context.core;

        // m_residents holds BORROWED pointers into NriMeshBufferCache's map and this
        // function is their last reader, so the table is dropped on EVERY exit path
        // -- not just the tail. Between Record(N) and Prepare(N+1) an
        // InvalidateMeshGeometry erases map nodes; leaving stale pointers in a live
        // member for that window is a trap even though nothing walks it today. See
        // the member's own declaration comment.
        struct ResidentScope
        {
            std::vector<std::pair<Guid, const NriMeshBufferCache::Resident*>>* table;
            ~ResidentScope() { table->clear(); }
        } residentScope{ &m_residents };

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

        // NOTHING TO DRAW is a cleared depth buffer, not an error: no emitted
        // batch and no ad-hoc row. That includes ruling R-D's frame -- rows
        // STAGED but everything culled (MeshSceneDesc::Empty explains why
        // such a frame is declared at all): the sync node ahead of this one
        // uploaded them, and this pass has no draw to record.
        const bool hasBatches = scene.scene && scene.scene->HasDraws();
        if (!hasBatches && m_adHocDraws.empty())
            return;

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
        if (!gpuScene)   // AddMeshNode passes context->Scene(); null means the vehicle has no GPU scene
        {
            GraphError("MeshNode: no GpuScene on the context -- nothing recorded");
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
        frameConstants.lightDirection = glm::vec4(SafeNormalize(scene.lightDirection), 0.0f);
        frameConstants.lightColor     = glm::vec4(scene.lightColor, 0.0f);
        frameConstants.ambient        = glm::vec4(scene.ambient, 0.0f);
        if (auto* arena = static_cast<std::uint8_t*>(m_arenaCpu))
            std::memcpy(arena + ArenaOffset(frameSlot), &frameConstants, sizeof(frameConstants));

        // ---------------------------------------------------------------
        // Resident vertex + index buffers, looked up from Prepare's table.
        // NEVER resolved here -- UploadData submits and waits internally.
        // DEDUPED BY GUID: a scene of twenty cubes is one bind and twenty
        // draws. Offset is always 0: these are dedicated buffers, not a ring.
        // ---------------------------------------------------------------
        const auto residentFor = [&](const Guid& id) -> const NriMeshBufferCache::Resident*
        {
            for (const auto& e : m_residents)
                if (e.first == id)
                    return e.second;
            return nullptr;
        };

        // THE TWO SETS this pass binds. set0 (frame slot) carries only b1
        // now (Task 8/10 moved the white texel/sampler out -- s0 is a root/
        // immutable sampler CmdSetPipelineLayout pushes automatically,
        // needing no CmdSetDescriptorSet of its own); every instance reads
        // the same b1, so there is nothing per-instance in it to rebind.
        // set1 is the bindless material array -- ONE set for the node's
        // whole lifetime, not per frame slot, bound here once per pass
        // exactly like set0 (its CONTENTS may still be growing via
        // AddMaterial between passes; what does not change mid-pass is
        // WHICH set is bound). A null here means Create() failed part way
        // and already said so.
        nri::DescriptorSet* set = frameSlot < kSwapchainFramesInFlight ? m_sets[frameSlot] : nullptr;
        if (!set || !m_bindlessSet)
        {
            GraphError("MeshNode: no descriptor set for this frame slot -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // THE FRAME SET's t0/t1 VIEWS (F3 plan 1 T7): rewritten for THIS slot
        // when the GPU scene's buffers changed -- EITHER the instance buffer
        // grew (its generation moved; the view object was replaced with it)
        // OR this slot's visible-index buffer was re-created (its view
        // pointer moved; GpuScene keeps no generation for those). Both
        // halves of the compare are load-bearing. Safe: this slot's previous
        // frame retired at BeginFrame, so nothing in flight reads the set;
        // the ranges carry ALLOW_UPDATE_AFTER_SET regardless. The first
        // Record for a slot always writes (the members start at 0 / null).
        // ---------------------------------------------------------------
        if (m_setInstanceGen[frameSlot] != gpuScene->InstanceBufferGeneration()
            || m_setVisibleView[frameSlot] != gpuScene->VisibleIndicesView(frameSlot))
        {
            // `views` must outlive the UpdateDescriptorRanges call -- the same
            // rule CreateSets' `cb` follows: `descriptors` is a pointer to an
            // array dereferenced inside the call.
            const nri::Descriptor* views[2] = { gpuScene->InstancesView(), gpuScene->VisibleIndicesView(frameSlot) };
            if (!views[0] || !views[1])
            {
                GraphError("MeshNode: the GPU scene has no instance / visible-index view for this frame slot -- nothing recorded");
                return;
            }
            nri::UpdateDescriptorRangeDesc updates[2] = {};
            for (std::uint32_t i = 0; i < 2; ++i)
            {
                updates[i].descriptorSet = set;
                updates[i].rangeIndex    = 1 + i;   // CreateBindings' frameRanges[1] (t0) / [2] (t1)
                updates[i].descriptors   = &views[i];
                updates[i].descriptorNum = 1;
            }
            core.UpdateDescriptorRanges(updates, 2);
            m_setInstanceGen[frameSlot] = gpuScene->InstanceBufferGeneration();
            m_setVisibleView[frameSlot] = gpuScene->VisibleIndicesView(frameSlot);
        }

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        // ONE layout for the whole pass, so this is bound once. CmdSetPipeline
        // Layout invalidates the bound sets and root constants on both
        // backends, which is exactly why it must not be re-issued per draw --
        // and why the sets below are bound AFTER it. It is also what pushes
        // the root/immutable s0 sampler automatically on both backends
        // (CommandBufferVK::SetPipelineLayout's "Push immutable samplers"
        // block; a D3D12 static sampler needs no runtime bind call at all)
        // -- nothing else in this function touches s0.
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc frameSetDesc = {};
        frameSetDesc.setIndex      = 0;   // frameSetDesc's array position, CreateBindings
        frameSetDesc.descriptorSet = set;
        core.CmdSetDescriptorSet(context.cmd, frameSetDesc);

        nri::SetDescriptorSetDesc bindlessSetDesc = {};
        bindlessSetDesc.setIndex      = 1;   // bindlessSetDesc's array position, CreateBindings
        bindlessSetDesc.descriptorSet = m_bindlessSet;
        core.CmdSetDescriptorSet(context.cmd, bindlessSetDesc);

        core.CmdSetPipeline(context.cmd, *m_pipeline);

        // Binds a mesh's resident vertex + index buffers if it is resident and
        // not already bound; null means SKIP THE DRAW -- never a stale bind
        // from the previous one. Offset is always 0: dedicated buffers, not a
        // ring.
        Guid lastMesh{};
        bool lastBound = false;
        const auto bindMesh = [&](const Guid& id) -> const NriMeshBufferCache::Resident*
        {
            const NriMeshBufferCache::Resident* resident = residentFor(id);
            if (!resident || !resident->ready || !resident->vertexBuffer || !resident->indexBuffer)
                return nullptr;
            if (!lastBound || id != lastMesh)
            {
                lastMesh  = id;
                lastBound = true;
                nri::VertexBufferDesc vertexBuffer = {};
                vertexBuffer.buffer = resident->vertexBuffer;
                vertexBuffer.offset = 0;
                vertexBuffer.stride = sizeof(MeshVertex);
                core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
                core.CmdSetIndexBuffer(context.cmd, *resident->indexBuffer, 0,
                                        nri::IndexType::UINT32);
            }
            return resident;
        };
        // THE ROOT BLOCK, once per draw: {firstOutput, flags} -- the whole of
        // what a draw carries now (MeshRootConstants, MeshNode.hpp).
        const auto pushRoot = [&](std::uint32_t firstOutput, std::uint32_t flags)
        {
            const MeshRootConstants push{ firstOutput, flags };
            nri::SetRootConstantsDesc rootConstants = {};
            rootConstants.rootConstantIndex = 0;
            rootConstants.data              = &push;
            rootConstants.size              = sizeof(push);
            core.CmdSetRootConstants(context.cmd, rootConstants);
        };

        // ---------------------------------------------------------------
        // 1. THE REGISTRY-BACKED BATCHES, INDIRECT, in the frame's order
        //    (BuildGpuSceneFrame: opaque before masked, nearest first). One
        //    CmdDrawIndexedIndirect per batch, its nri::DrawIndexedDesc at
        //    argIndex in this slot's args buffer (GpuSceneSyncNode copied
        //    the frame's DrawIndexedArgs there -- GpuScene.cpp static_asserts
        //    the two layouts agree). instanceNum is the CPU coarse count
        //    (plan 2: the cull dispatch writes it), and the vertex shader
        //    maps SV_InstanceID through the visible-index list from
        //    batch.firstOutput. An indirect draw with instanceNum 0 is legal
        //    and draws nothing; a batch is only EMITTED with >= 1 visible row
        //    anyway.
        // ---------------------------------------------------------------
        if (hasBatches)
        {
            nri::Buffer* args = gpuScene->Args(frameSlot);
            if (!args)
            {
                GraphError("MeshNode: the GPU scene has no indirect-args buffer for this frame slot -- the batches are dropped");
            }
            else
            {
                for (const GpuBatchDraw& batch : scene.scene->batches)
                {
                    if (!bindMesh(batch.mesh))
                        continue;   // not resident: skip, never a stale bind
                    pushRoot(batch.firstOutput, 0);
                    core.CmdDrawIndexedIndirect(context.cmd, *args,
                                                std::uint64_t(batch.argIndex) * sizeof(nri::DrawIndexedDesc),
                                                1, sizeof(nri::DrawIndexedDesc), nullptr, 0);
                }
            }
        }

        // ---------------------------------------------------------------
        // 2. THE AD-HOC ROWS, DIRECT, in submission order, from this slot's
        //    scratch region: Prepare staged row i as m_adHocRows[i], the sync
        //    node copied it to scratch row ScratchFirstRow(slot) + i, and the
        //    root block names that row outright (kMeshRootDirect). One
        //    CmdDrawIndexed each -- the F2a shape, minus the 128-byte push.
        // ---------------------------------------------------------------
        const std::uint32_t scratchFirst = gpuScene->ScratchFirstRow(frameSlot);
        for (std::size_t i = 0; i < m_adHocDraws.size(); ++i)
        {
            const AdHocDraw& d = m_adHocDraws[i];
            const NriMeshBufferCache::Resident* resident = bindMesh(d.mesh);
            if (!resident)
                continue;
            pushRoot(scratchFirst + static_cast<std::uint32_t>(i), kMeshRootDirect);

            nri::DrawIndexedDesc draw = {};
            draw.baseIndex   = d.indexOffset;
            draw.indexNum    = d.indexCount ? d.indexCount : resident->indexCount;
            draw.instanceNum = 1;
            core.CmdDrawIndexed(context.cmd, draw);
        }
    }

    RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                          RgTexture canvas, nri::Format canvasFormat,
                          const MeshSceneDesc& scene,
                          std::uint32_t width, std::uint32_t height)
    {
        // Resolved at DECLARATION time on purpose: a PSO compile must not land
        // inside the recording window. See MeshNode::Prepare.
        //
        // `canvasFormat` is the CALLER's, not kGraphCanvasFormat assumed --
        // see AddMeshNode's header comment for why this function cannot derive
        // it and what a wrong one costs.
        std::span<const GpuInstance> adHoc;
        if (context)
        {
            // UNCONDITIONAL once a node exists: MeshBuffers() may be null, and
            // Prepare's own residency half is what skips then -- the PIPELINE half
            // must still run (see Prepare's declaration comment). Prepare ALSO
            // stages the ad-hoc rows (F3 plan 1 T7), which the sync node below
            // needs BEFORE the mesh node is declared -- so it runs first.
            if (MeshNode* node = context->Mesh())
            {
                node->Prepare(canvasFormat, scene, context->MeshBuffers(),
                              context->PresentedFrames());
                adHoc = node->AdHocRows();
            }
        }

        // THE GPU SCENE'S WRITER, FIRST (F3 plan 1 T6, ruling R-A): the sync
        // node imports the persistent instance / args / visible-index buffers
        // as CopyDst and copies this frame's rows into them -- the registry
        // scene's staged rows and (T7) the ad-hoc rows into this slot's
        // scratch region; the mesh node below Reads the same handles, so the
        // graph derives the copy -> read barriers.
        const GpuSceneNodeInputs gpuScene = AddGpuSceneSyncNode(graph, context, scene.scene, adHoc);

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

                // The GPU scene's buffers, as this pass consumes them: the
                // instance rows and the visible indices from the vertex
                // shader, the args by CmdDrawIndexedIndirect.
                builder.Read(gpuScene.instances, RgUsage::ShaderRead);
                builder.Read(gpuScene.visibleIndices, RgUsage::ShaderRead);
                builder.Read(gpuScene.args, RgUsage::IndirectArgs);
            },
            [context, scene](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive: no device, nothing to record
                if (MeshNode* node = context->Mesh())
                    node->Record(nodeContext, scene, context->FrameSlot(), context->Scene());
            });

        // TEST-ONLY, and declared only when a test armed it (GpuScene::
        // EnableDebugReadback): the instance buffer read back after this pass
        // consumed it. A no-op on every production frame.
        AddGpuSceneDebugReadbackNode(graph, context, gpuScene);
        return depth;
    }
}
