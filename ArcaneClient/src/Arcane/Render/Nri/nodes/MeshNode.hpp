#pragma once

// MeshNode -- THE OPAQUE 3D PASS as a render graph node (NRI Phase 4, Task 7).
//
// This is the node that puts a lit, textured, DEPTH-TESTED mesh through the
// frame graph. It renders into the frame's canvas (the same RGBA16F transient
// the 2D batch draws into) with a D32_SFLOAT depth target it creates itself.
//
// WHAT IT IS DELIBERATELY NOT:
//   * NOT PBR. data/shaders/mesh.hlsl is one directional light, Lambert
//     diffuse plus a constant ambient term, one albedo texture. The GGX
//     material model belongs to the Deadlock-class renderer arc; a half
//     version here would be a second material model to reconcile.
//   * NOT a commitment to forward OR deferred. One opaque pass writing a
//     colour target and a depth target is the shared prefix of both.
//   * NOT a mesh IMPORT path. Geometry comes from MeshBuilder (procedural
//     cube/sphere); cgltf/.arcmesh is a later arc, and no library is vendored
//     for it here.
//   * NOT the material system. A per-instance albedo Guid and a linear tint
//     are the whole of it; Task 8's BindlessTable is what turns that into a
//     material index.
//
// WHERE IT SITS IN THE FRAME: after `batch2d`, before the post chain and the
// tonemap. The canvas is MINTED AND CLEARED by AddBatch2DNode, so the mesh
// pass is necessarily the canvas's second writer -- it clears the DEPTH plane
// (which is a fresh pool slot with undefined contents) and never the colour
// one, or it would wipe the 2D content that ran before it. Rendering 3D UNDER
// the 2D layer instead is a clear-seam question (NriGraphContext::
// DeclareGraphFrame's THE CLEAR SEAM block), not this node's.
//
// WHAT THIS NODE OWNS (all persistent, all created once at Create()):
//   * the 1x1 white texel + its SHADER_RESOURCE view -- what t0 binds for an
//     instance that names no albedo, mirroring Batch2DNode's;
//   * one linear/repeat sampler (REPEAT, not clamp: a mesh's UVs tile);
//   * the pipeline layout -- root constants b0 (the 80-byte MeshConstants from
//     mesh.hlsl) plus descriptor set space0 = { b1 frame CB, t0 albedo,
//     s0 sampler };
//   * one descriptor pool, and the per-(albedo, frame slot) descriptor sets
//     out of it -- written ONCE each and never rewritten, the same discipline
//     Batch2DNode keeps and for the same reason (a set the GPU may still be
//     reading must not be rewritten, and NRI cannot free a single set);
//   * the per-frame-slot constant-buffer arena the b1 views name.
// The PIPELINE is not owned here: it comes from the vehicle's shared
// NriPipelineCache, keyed by (shader pair, layout, canvas format, DEPTH
// format, blend), so a format change is a cache miss rather than a stale PSO.
//
// WHY THE SETS ARE PER FRAME SLOT and Batch2DNode's built-in ones are not: a
// set here carries the per-frame constant buffer b1, whose (buffer, offset) is
// baked into its nri::Descriptor at creation. Double-buffering the CB means
// double-buffering the set that names it. See Batch2DNode.hpp's THE
// CONSTANT-BUFFER ARENA for the whole account of why the upload ring cannot
// back a constant buffer that a descriptor set names.
//
// WINDING AND CULLING -- read MeshBuilder.hpp's WINDING block first. That
// module emits triangles that are COUNTER-CLOCKWISE as seen from OUTSIDE the
// surface, and this node's rasterizer state is set to match:
// `frontCounterClockwise = true` + `cullMode = BACK`. See MeshNode.cpp's
// PipelineFor() for the full derivation (and the desk check that falsifies it
// in one look: get it backwards and a closed mesh renders INSIDE-OUT or, for a
// convex one, vanishes entirely).
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h> #defines ERROR via wingdi.h).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/MeshBuilder.hpp>      // MeshData / MeshVertex -- the CPU geometry
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kSwapchainFramesInFlight

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;
    class NriTextureCache;

    // =====================================================================
    // THE CPU-SIDE SCENE, and the whole of what a host has to build.
    //
    // Deliberately NOT an ECS view, a Registry walk or a Scene pointer: this
    // is a RENDER vehicle's input, the same shape and the same borrowing rules
    // FrameDesc::pickables carries (Render/PickEmit.hpp is its sibling). The
    // thing that owns a registry and a camera is the FRAME DRIVER, and Task 9
    // is what teaches both hosts to fill this in.
    // =====================================================================
    struct MeshInstance
    {
        // BORROWED. Must outlive the RenderFrame call that carries it -- the
        // vertex/index streams are copied into the upload ring at RECORD time,
        // which is inside that call. Null means "draw nothing", which is not an
        // error (a scene may legitimately carry a slot with no geometry yet).
        const MeshData* mesh = nullptr;

        // Model -> world, METERS (MKS). Rotation + translation + UNIFORM scale
        // only: mesh.hlsl transforms normals by the upper 3x3 rather than the
        // inverse transpose, which is exact for those and wrong for a
        // non-uniform scale. See that file's vs_main.
        glm::mat4 model{1.0f};

        // LINEAR, and may exceed 1.0 (the canvas is RGBA16F). Multiplies the
        // albedo sample, so it is a tint on a textured instance and the whole
        // colour on an untextured one.
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};

        // The albedo image's ASSET Guid, resolved through the vehicle's shared
        // NriTextureCache -- the same cache the 2D batch and the post chain
        // use, so an image named by several of them is uploaded once. A NIL
        // Guid (the default) binds this node's 1x1 white texel, which is the
        // ordinary untextured case and not a degradation.
        Guid albedo{};
    };

    struct MeshSceneDesc
    {
        // BORROWED for the duration of the RenderFrame call, exactly like
        // FrameDesc::pickables: the declaration copies the SPAN into the
        // node's exec fn, never the elements, and the exec fn runs inside the
        // same call. EMPTY IS LEGAL and means "no mesh pass this frame" --
        // DeclareGraphFrame declares no node for it, so an empty scene costs
        // the same as no scene at all.
        std::span<const MeshInstance> instances;

        // The camera, already resolved by the frame driver. TWO matrices
        // rather than one product because that is what SceneCamera hands back
        // (PerspectiveCameraView, Task 5) and because a later pass that needs
        // the view alone should not have to un-multiply it.
        //
        // THE CALLER OWES VALID MATRICES. SceneCamera::PerspectiveProjection
        // is an unvalidated pure function -- a zero fov, a non-positive aspect
        // ratio or nearZ >= farZ silently produces NaN/Inf -- so build these
        // through the guarded path (ActivePerspectiveSceneCamera) or check
        // them. MeshNode::Record checks them too and drops the pass with one
        // ERROR rather than recording a draw with a NaN transform, because a
        // NaN clip position is undefined behaviour on the GPU rather than a
        // wrong picture.
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};

        // THE ONE DIRECTIONAL LIGHT. `lightDirection` points TOWARD the light
        // (the convention the shader's N.L uses directly); it is normalized in
        // the shader, so an unnormalized value is a scale, not a bug.
        // `ambient` is a flat term added to every lit surface -- the whole of
        // the indirect lighting model here, deliberately.
        glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
        glm::vec3 lightColor{1.0f, 1.0f, 1.0f};
        glm::vec3 ambient{0.05f, 0.05f, 0.05f};
    };

    class ARCANE_API MeshNode
    {
    public:
        // Loads mesh_vs/mesh_ps through the vehicle, creates the white texel +
        // sampler + descriptor pool + constant arena, and registers the
        // pipeline layout. Null (already logged + latched) on any failure --
        // a vehicle that cannot build this node must not render a frame that
        // silently draws nothing.
        static std::unique_ptr<MeshNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- same shape as ~Batch2DNode. The
        // sanctioned release is Release() at a fence the owner knows.
        ~MeshNode();

        MeshNode(const MeshNode&)            = delete;
        MeshNode& operator=(const MeshNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent. The caller picks the fence for the same reason
        // NriPipelineCache::Clear does.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Resolves everything one scene needs before a command buffer exists:
        // every instance's ALBEDO (through the shared NriTextureCache) and its
        // per-frame-slot descriptor sets, and the PIPELINE for this frame's
        // attachment formats. Called at DECLARATION time and for the same
        // three reasons Batch2DNode::Prepare is: a texture upload submits and
        // waits internally, a descriptor set must never be allocated or
        // written while the GPU may be reading the pool, and a PSO compile must
        // not land inside the recording window.
        //
        // Safe to call with an empty scene, and safe to skip entirely --
        // Record() then binds the white texel and reports the missing pipeline
        // once.
        void Prepare(const MeshSceneDesc& scene, nri::Format canvasFormat);

        // Records one scene's opaque geometry into an ALREADY-OPEN raster pass
        // whose colour attachment is the canvas and whose depth attachment is
        // this node's depth target. In order: clear the DEPTH plane (the clear
        // seam -- graph attachments are LOAD/STORE, see
        // NriGraphContext::DeclareGraphFrame), upload each distinct mesh's
        // vertex/index streams through the ring, then one CmdDrawIndexed per
        // instance.
        //
        // IT DOES NOT CLEAR THE COLOUR PLANE. batch2d already cleared and drew
        // into the canvas; clearing it here would erase that.
        //
        // Emits NO barrier: the executor derives and batches every one of them
        // from the declarations.
        //
        // `frameSlot` is the vehicle's own per-frame slot -- the SAME number it
        // gave the upload ring, so this node's constant-buffer arena is
        // double-buffered against exactly the fence the swapchain already waits
        // on.
        //
        // NO canvasFormat PARAMETER, unlike Batch2DNode::Record: that node
        // resolves a pipeline PER SPAN at record time and needs the format
        // there, while this one has exactly one pipeline and Prepare already
        // keyed it. A parameter this function did not read would just be
        // something for a reader to reason about.
        void Record(RenderGraphNodeContext& context, const MeshSceneDesc& scene,
                    std::uint32_t frameSlot);

        // How many DISTINCT albedo images one run may bind. A descriptor
        // pool's capacity is fixed at creation and NRI cannot free one set, so
        // this is decided up front rather than discovered mid-frame; past it an
        // instance draws with the white texel and one ERROR names this
        // constant. 16 rather than Batch2DNode's 64 because a mesh scene binds
        // materials, not sprite atlases -- and because Task 8's BindlessTable
        // is the answer to a scene that genuinely needs hundreds.
        static constexpr std::uint32_t kMaxAlbedoTextures = 16;

        // The b1 block's region size BEFORE alignment. mesh.hlsl's MeshFrameCB
        // is 112 bytes; 256 is also D3D12's constant-buffer placement
        // alignment, so on that backend this is exactly one region.
        static constexpr std::uint32_t kFrameCbMaxBytes = 256;

        // The arena's region stride on a device whose
        // deviceDesc.memoryAlignment.constantBufferOffset is
        // `constantBufferAlignment`. PURE and public for the same reason
        // Batch2DNode::CbRegionStride is: it carries an invariant whose
        // violation would be silent. The result must be BOTH a multiple of the
        // device's alignment AND at least kFrameCbMaxBytes; rounding UP
        // satisfies both for any power-of-two alignment.
        [[nodiscard]] static constexpr std::uint64_t CbRegionStride(
            std::uint64_t constantBufferAlignment) noexcept
        {
            return constantBufferAlignment <= 1
                 ? kFrameCbMaxBytes
                 : ((kFrameCbMaxBytes + constantBufferAlignment - 1) / constantBufferAlignment)
                       * constantBufferAlignment;
        }

        // Byte offset of one frame slot's constant-buffer region. PURE, static
        // and public so the [nri] cases can prove the property no device can
        // show: distinct frame slots never alias, and every region starts on a
        // multiple of `regionStride`.
        [[nodiscard]] static constexpr std::uint64_t CbRegionOffset(
            std::uint64_t regionStride, std::uint32_t frameSlot) noexcept
        {
            return (std::uint64_t)frameSlot * regionStride;
        }

        // THE DESCRIPTOR POOL'S CAPACITY, as pure arithmetic over the cap
        // above. PUBLIC and separated from the creation call for the reason
        // Batch2DNode::PoolSizes is: a capacity that does not cover what the
        // cap ALLOWS is not a compile error or a wrong pixel -- it is an
        // allocation that fails part-way through a frame at the desk.
        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes() noexcept;

        // How many DISTINCT non-nil albedo Guids `instances` names -- how many
        // per-albedo set families this scene spends out of kMaxAlbedoTextures.
        // PURE and public for the same reason Batch2DNode::DistinctTextureCount
        // is, and Prepare() CALLS THIS, so a case that asserts it asserts the
        // node's own arithmetic rather than a lookalike.
        [[nodiscard]] static std::uint32_t DistinctAlbedoCount(
            std::span<const MeshInstance> instances);

    private:
        MeshNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateWhiteTexel();
        bool CreateBindings();
        bool CreateConstantArena();

        // The opaque pipeline for this frame's attachment formats, from the
        // shared cache. Null (already logged) if the cache refused it.
        [[nodiscard]] nri::Pipeline* PipelineFor(nri::Format canvasFormat);

        // The SHADER_RESOURCE view for asset `id` through the vehicle's SHARED
        // cache. Null -- nil Guid, unresolvable, undecodable, or NRI refused it
        // -- means "use the white texel".
        [[nodiscard]] nri::Descriptor* TextureView(const Guid& id);

        // One albedo's descriptor sets, one per frame slot. `set[i]` binds that
        // frame slot's b1 region, the albedo at t0 and the sampler at s0.
        struct AlbedoSets
        {
            nri::DescriptorSet* set[kSwapchainFramesInFlight]{};
        };

        // ONE distinct MeshData's ring allocation for ONE frame. Record()
        // builds this table so a scene of twenty cubes is one upload and twenty
        // draws rather than twenty uploads -- and keeps it as a RESERVED member
        // so the per-frame table never allocates inside the recording window,
        // the rule every node on this path keeps.
        struct Upload
        {
            const MeshData* mesh         = nullptr;
            nri::Buffer*    vertexBuffer = nullptr;
            std::uint64_t   vertexOffset = 0;
            nri::Buffer*    indexBuffer  = nullptr;
            std::uint64_t   indexOffset  = 0;
            std::uint32_t   indexCount   = 0;
            // False memoizes a FAILED upload (the ring ran dry), so a scene
            // with twenty instances of one oversized mesh retries the ring once
            // rather than twenty times.
            bool            ok           = false;
        };

        // How many distinct meshes one frame is expected to carry before the
        // table grows. Not a cap -- growth is legal and merely allocates once,
        // unlike the descriptor-pool caps above, which are hard.
        static constexpr std::size_t kInitialUploadSlots = 16;

        // The sets that bind `id` at t0, allocating and writing them on first
        // use. Falls back to the white-texel family for a nil id, an image that
        // is not resident, or a run that has spent kMaxAlbedoTextures.
        // DECLARATION-TIME ONLY.
        [[nodiscard]] const AlbedoSets* EnsureAlbedoSets(const Guid& id);
        // The same as a pure LOOKUP -- record-time only, and deliberately not
        // the Ensure above: nothing inside the recording window may allocate a
        // set or upload a texture.
        [[nodiscard]] const AlbedoSets* AlbedoSetsFor(const Guid& id) const;
        // Writes one set family's three ranges. Shared by the white-texel path
        // and the per-albedo path so the two can never disagree about a set's
        // contents.
        void WriteSets(AlbedoSets& sets, nri::Descriptor* albedoView);

        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot);
        }

        // NriPipelineCache::GraphicsKey::shaderPairId is opaque to the cache
        // and is the CALLER's discriminator for everything the key does not
        // carry (that class's fill-contract rule 3) -- here the vertex input,
        // the rasterizer state and the depth TEST state, none of which are
        // keyed. One shared cache, so the node id spaces must not overlap:
        // Batch2DNode is 0x2000..0x2002, TonemapNode 0x3000, the outline chain
        // 0x4000..0x4002, PickNode 0x4100.
        static constexpr std::uint64_t kShaderPairId = 0x5000;

        NriDevice*        m_device       = nullptr;
        NriPipelineCache* m_pipelines    = nullptr;
        // NO m_owner BACK-POINTER, deliberately: everything this node needs
        // from the vehicle (device, pipeline cache, texture cache, shader
        // bytecode) is taken at Create, and the two per-frame values it reads
        // -- the canvas format and the frame slot -- arrive as Record/Prepare
        // parameters. Batch2DNode carries one that nothing reads; this does not
        // copy it.
        //
        // The vehicle's SHARED image residency cache -- borrowed, never owned,
        // and released by the vehicle AFTER this node (its sets name the
        // cache's views). See NriTextureCache.hpp.
        NriTextureCache*  m_textureCache = nullptr;

        // Bytecode is OWNED BY THE VEHICLE (NriGraphContext's bin cache) and
        // outlives this node -- which the pipeline cache's fill contract
        // (rule 2) requires, since CreateGraphicsPipeline runs after the fill
        // callback returns.
        std::span<const std::uint8_t> m_vs, m_ps;

        nri::Texture*    m_white     = nullptr;
        nri::Descriptor* m_whiteView = nullptr;
        nri::Descriptor* m_sampler   = nullptr;

        nri::DescriptorPool* m_pool = nullptr;
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;

        // The per-frame-slot b1 arena: ONE HOST_UPLOAD buffer, persistently
        // mapped, carved into kSwapchainFramesInFlight regions.
        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;
        nri::Descriptor* m_frameCbView[kSwapchainFramesInFlight]{};

        // The NIL-albedo family (the white texel), written once at Create.
        AlbedoSets m_whiteSets{};
        // ...and one family per distinct albedo Guid, written once each on
        // first use. A memoized MISS is stored as an empty family so the cache
        // is not re-asked every frame.
        std::unordered_map<Guid, AlbedoSets> m_albedoSets;

        // MEMBERS, not locals, and that is load-bearing:
        // nri::GraphicsPipelineDesc::vertexInput is a POINTER into caller
        // memory that CreateGraphicsPipeline dereferences AFTER the fill
        // callback has returned (NriPipelineCache.hpp, fill-contract rule 2).
        nri::VertexAttributeDesc m_attributes[3]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        // Resolved by Prepare for the canvas format of the frame being
        // declared, so Record never reaches the pipeline cache from inside an
        // open command buffer. Owned by the cache; borrowed here.
        nri::Pipeline* m_pipeline = nullptr;

        // This frame's distinct-mesh upload table -- see Upload. Cleared at the
        // top of every Record(), never shrunk.
        std::vector<Upload> m_uploads;

        // One WARN/ERROR each, not one per instance per frame, for the
        // degradations a reader must be able to see.
        bool m_warnedTextureBudget = false;
        bool m_warnedNoPipeline    = false;
        bool m_warnedRingOverflow  = false;
        bool m_warnedBadCamera     = false;
    };

    // Declares the opaque mesh node into `graph` and hands back the
    // D32_SFLOAT depth transient it created and attached.
    //
    // THE DEPTH TARGET IS MINTED HERE, in this node's own Setup -- the same
    // create-then-write-then-attach shape AddBatch2DNode uses for the canvas,
    // and the reason Task 4's placeholder "depth" node is not declared on a
    // frame that carries a mesh scene. A transient can only be minted from a
    // RenderGraphBuilder, and a builder exists only inside a node's Setup, so
    // the node that consumes it is the natural one to create it.
    //
    // `canvas` is an INPUT (unlike AddBatch2DNode's, which is an output):
    // batch2d already minted and cleared it, and this pass draws on top.
    //
    // `context` is a POINTER because the headless [nri] frame-shape cases drive
    // the real declarations with no device: with a null context every
    // declaration is identical and the exec fn does nothing, which is exactly
    // what makes those cases able to fail when this function's DECLARATIONS
    // change.
    //
    // `scene` is BORROWED at declaration time and its SPAN is copied into the
    // exec fn -- see MeshSceneDesc::instances for the lifetime rule.
    ARCANE_API RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                                      RgTexture canvas, const MeshSceneDesc& scene,
                                      std::uint32_t width, std::uint32_t height);
}
