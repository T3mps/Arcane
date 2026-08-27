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
//   * NOT the material system. A per-instance linear tint (MeshInstance::
//     baseColor) is the whole of it, and t0 is this node's own white texel for
//     every draw. A per-instance albedo Guid resolved into per-image descriptor
//     sets was written and then REMOVED at Task 7's first fix round -- nothing
//     exercised it and Task 8's BindlessTable replaces t0 outright. That
//     table's per-instance MATERIAL INDEX is what belongs here next; see
//     MeshInstance's own NO PER-INSTANCE ALBEDO block.
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
//   * the 1x1 white texel + its SHADER_RESOURCE view -- what t0 binds for
//     EVERY draw, mirroring Batch2DNode's untextured path;
//   * one linear/repeat sampler (REPEAT, not clamp: a mesh's UVs tile);
//   * the pipeline layout -- root constants b0 (the 128-byte MeshConstants
//     from mesh.hlsl: model + tint + the per-instance normal matrix, Task 8/
//     F2a) plus descriptor set space0 = { b1 frame CB, t0 albedo texture, s0
//     sampler };
//   * one descriptor pool, and ONE descriptor set PER FRAME SLOT out of it
//     (CreateSets) -- written ONCE each, at Create, and never rewritten. That
//     is the same discipline Batch2DNode keeps, and stronger: the sets exist
//     before the first frame does, so there is no window in which the GPU
//     could be reading one (and NRI cannot free a single set anyway);
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
#include <Arcane/Render/MeshBuilder.hpp>      // MeshData / MeshVertex -- the CPU geometry
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kSwapchainFramesInFlight

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

    // NormalMatrixFor -- the inverse transpose of `model`'s upper 3x3, which is
    // the transform that keeps a surface normal PERPENDICULAR TO ITS SURFACE
    // under a NON-UNIFORM scale. (Why the upper 3x3 alone is wrong: a surface
    // tangent scales WITH the object, so for dot(normal, tangent) to stay zero
    // after a non-uniform scale, the normal has to scale by the INVERSE along
    // each axis, not the same factor. A uniform scale s*I is the one case
    // where this doesn't matter -- its inverse transpose is (1/s)*I, a
    // positive multiple of I that this file's vs_main normalize() divides
    // straight back out -- which is exactly why the upper-3x3 shortcut looked
    // correct until F1 gave Transform a per-axis glm::vec3 scale the Inspector
    // authors freely: the first non-uniform scale-handle drag on a mesh hits
    // this.)
    //
    // SINGULAR GUARD -- computed, not predicted. `glm::inverse` runs
    // UNCONDITIONALLY below; the guard checks its OUTPUT (all nine elements
    // finite) rather than pre-screening `upper`'s determinant against a fixed
    // threshold. A pre-screen is a CONDITIONING HEURISTIC, not a test for the
    // true singular set, and the difference is not academic: a 3x3
    // determinant scales as s^3 under a uniform scale s, so no fixed
    // threshold is scale-invariant. A too-tight one -- the bug this shape
    // actually had -- lets a small NON-UNIFORM scale (whose determinant can
    // sit anywhere, independent of how ill-conditioned any one axis is) sail
    // past the check and fall through to a normal transform that is silently
    // wrong, reinstating exactly the defect this function exists to fix, with
    // no diagnostic. Checking the RESULT instead is exact by construction:
    // `glm::inverse` divides by the determinant unconditionally, so a genuine
    // singularity (or a non-finite `model`, e.g. a degenerate ancestor in a
    // WorldTransform product) is precisely the input for which the computed
    // inverse transpose comes back non-finite, and nothing else does.
    //
    // WHY GUARD AT ALL: an Inf/NaN normal is undefined behaviour on the GPU
    // rather than a wrong picture -- the same class of guard SceneCamera.hpp's
    // degenerate-basis fallback (ActivePerspectiveSceneCamera) takes, and the
    // same class MeshNode.cpp's own IsFinite/SafeNormalize take for the
    // camera and the light. Identity is the least-wrong answer here: an
    // instance whose model has collapsed a dimension has already lost its
    // geometry to the same degeneracy (its vertices are degenerate too), so
    // there is no "correct" normal direction left to recover -- identity just
    // keeps the pixel shader's arithmetic finite.
    //
    // PURE and header-only, matching SceneCamera.hpp's PerspectiveProjection:
    // no NRI device, no Registry, no MeshNode instance, so ArcaneTests can pin
    // the analytic property directly (MeshNodeTest.cpp).
    //
    // Computed PER INSTANCE inside MeshNode::Record from `model` alone --
    // MeshInstance gains no field for this; see its own comment below.
    [[nodiscard]] inline glm::mat3 NormalMatrixFor(const glm::mat4& model) noexcept
    {
        const glm::mat3 upper = glm::mat3(model);
        const glm::mat3 inverseTranspose = glm::transpose(glm::inverse(upper));
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (!std::isfinite(inverseTranspose[c][r]))
                    return glm::mat3(1.0f);
        return inverseTranspose;
    }

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

        // Model -> world, METERS (MKS). Rotation, translation AND a
        // NON-UNIFORM scale are all safe here (Task 8/F2a): MeshNode::Record
        // derives NormalMatrixFor(model) fresh for every instance and pushes
        // it alongside `model`, so mesh.hlsl's vs_main transforms normals by
        // the inverse transpose rather than the upper 3x3. (Before Task 8 this
        // comment stated a UNIFORM-scale-only restriction -- F1 gave Transform
        // a glm::vec3 scale the Inspector authors freely, so the first
        // non-uniform scale-handle drag on a mesh hit the old shortcut.
        // NormalMatrixFor, above, is the fix; nothing here still depends on
        // that restriction.)
        glm::mat4 model{1.0f};

        // LINEAR, and may exceed 1.0 (the canvas is RGBA16F). It multiplies
        // the albedo sample, and since t0 is the node's white texel for every
        // instance today (see NO PER-INSTANCE ALBEDO below), it IS the
        // instance's colour.
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};

        // ===== NO PER-INSTANCE ALBEDO FIELD, DELIBERATELY =====
        // mesh.hlsl samples t0, and this node binds its 1x1 white texel there
        // for every draw. A `Guid albedo` resolved through the shared
        // NriTextureCache into per-(image, frame slot) descriptor sets was
        // written and then REMOVED at Task 7's first fix round: Task 8's
        // BindlessTable replaces t0, the descriptor ranges and the pipeline
        // layout outright, so that binding could not survive one more commit,
        // and nothing in the suite or either host exercised it in the
        // meantime. Task 8's per-instance MATERIAL INDEX is the field that
        // belongs here, and it arrives with the table that gives it meaning.
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

        // THE ONE DIRECTIONAL LIGHT. `lightDirection` points TOWARD the light.
        // It does NOT have to be unit length: MeshNode::Record normalizes it
        // once per frame on the CPU and mesh.hlsl consumes the result directly.
        //
        // A ZERO-LENGTH VECTOR IS LEGAL and means "no directional light" --
        // the pass falls back to the ambient term alone. That is a GUARD, not
        // a convention: `normalize()` on a zero vector is a division by zero
        // and yields NaN, which would propagate through N.L into every lit
        // pixel. Normalizing on the CPU is what lets the zero case be handled
        // at all (and costs one normalize per frame instead of one per pixel).
        //
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

        // Resolves the PIPELINE for the colour format the frame being declared
        // will attach. Called at DECLARATION time for the reason
        // Batch2DNode::Prepare is: a PSO compile must not land inside the
        // recording window, where a first-frame miss would stall it and a
        // FAILED miss would latch an error on a frame that is otherwise fine.
        //
        // NO SCENE PARAMETER. It took one while instances carried a per-image
        // albedo Guid that had to be made resident and given descriptor sets
        // here; with t0 fixed at the node's white texel there is nothing about
        // the scene left to resolve ahead of recording, and an unread parameter
        // is only something for a reader to reason about. Task 8's bindless
        // table is what gives this a scene-dependent job again.
        //
        // Safe to skip entirely -- Record() then reports the missing pipeline
        // once and draws nothing.
        void Prepare(nri::Format canvasFormat);

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

        // THE DESCRIPTOR POOL'S CAPACITY, as pure arithmetic over the ONE
        // dimension this node's sets have (the frame slot). PUBLIC and
        // separated from the creation call for the reason Batch2DNode::
        // PoolSizes is: a pool's sizes are fixed at creation and NRI cannot
        // free a single set, so a capacity that does not cover what the node
        // allocates is not a compile error and not a wrong pixel -- it is an
        // AllocateDescriptorSets failure part-way through Create at the desk.
        // Each set carries exactly three descriptors (b1, t0, s0), so all
        // three per-type maxima equal the set count.
        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes() noexcept;

    private:
        MeshNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateWhiteTexel();
        bool CreateBindings();
        bool CreateConstantArena();
        // Allocates the per-frame-slot descriptor sets and writes each one's
        // three ranges: that slot's b1 view, the white texel at t0, the
        // sampler at s0. Written ONCE, at Create, and never again -- which is
        // what keeps ResetDescriptorPool and its fence discipline out of this
        // node entirely.
        bool CreateSets();

        // The opaque pipeline for this frame's attachment formats, from the
        // shared cache. Null (already logged) if the cache refused it.
        [[nodiscard]] nri::Pipeline* PipelineFor(nri::Format canvasFormat);

        // ONE distinct MeshData's ring allocation for ONE frame. Record()
        // builds this table so a scene of twenty cubes is one upload and twenty
        // draws rather than twenty uploads.
        //
        // A RESERVED MEMBER, not a local, so the STEADY STATE allocates nothing
        // inside the recording window -- which is the rule every node on this
        // path keeps for descriptor sets, pipelines and GPU resources, and
        // which Record() keeps ABSOLUTELY for all three. This table is the one
        // qualified case: a frame carrying more than kInitialUploadSlots
        // distinct meshes grows the vector and therefore does hit the heap
        // mid-recording. That is a plain allocation with no fence or pool
        // implications, it happens once per high-water mark rather than once
        // per frame, and raising kInitialUploadSlots is the whole fix -- but it
        // is not "never", and this comment does not say so.
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
        // from the vehicle (device, pipeline cache, shader bytecode) is taken
        // at Create, and the two per-frame values it reads -- the canvas format
        // and the frame slot -- arrive as Record/Prepare parameters.
        // Batch2DNode carries one that nothing reads; this does not copy it.
        //
        // NO NriTextureCache POINTER EITHER, since the fix round that removed
        // per-instance albedo: this node's only image is its OWN white texel,
        // so it borrows nothing from the vehicle's shared residency cache.
        // Task 8's bindless table is what makes it a consumer of that cache.

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

        // THE descriptor sets, one per frame slot. Each binds that slot's b1
        // region, the white texel at t0 and the sampler at s0, and is written
        // ONCE at Create and never again. One dimension only (the frame slot),
        // because b1 is the only thing in a set that differs frame to frame.
        nri::DescriptorSet* m_sets[kSwapchainFramesInFlight]{};

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
    // `canvasFormat` IS A PARAMETER AND NOT AN ASSUMPTION. NRI bakes attachment
    // formats into a graphics pipeline, so binding one inside a
    // CmdBeginRendering whose colour attachment carries a different format is
    // undefined on both backends -- and RenderGraph exposes no way to read a
    // handle's format back, so this function cannot derive it. It must be the
    // format `canvas` was CREATED with; the caller that minted the handle is
    // the one that knows. (It was hardcoded to kGraphCanvasFormat until Task
    // 7's first fix round, which made a differently-formatted canvas a silent
    // mismatch with no diagnostic.)
    //
    // `context` is a POINTER because the device-less [nri] frame-shape cases drive
    // the real declarations with no device: with a null context every
    // declaration is identical and the exec fn does nothing, which is exactly
    // what makes those cases able to fail when this function's DECLARATIONS
    // change.
    //
    // `scene` is BORROWED at declaration time and its SPAN is copied into the
    // exec fn -- see MeshSceneDesc::instances for the lifetime rule.
    ARCANE_API RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                                      RgTexture canvas, nri::Format canvasFormat,
                                      const MeshSceneDesc& scene,
                                      std::uint32_t width, std::uint32_t height);
}
