#pragma once

// FullscreenNodes -- the graph's fullscreen-triangle passes: the scene POST
// CHAIN (PostChainNode, Task 10) and the TONEMAP (TonemapNode, Task 8).
//
// The frame's tail is `canvas -> post0 -> .. -> postN-1 -> tonemap`, one graph
// node per chain pass, each pass a fullscreen triangle rendering into its OWN
// declared RGBA16F transient. Nothing here records a barrier: pass k declares
// a Read of pass k-1's target and a Write of its own, and the executor derives
// every transition from exactly that (RgCompiled's contract block).
//
// THE PING-PONG IS DERIVED, NOT HAND-ROLLED. Each pass declares its own
// transient, and RenderGraph::Compile's lifetime-interval pool allocator is
// what collapses them onto two physical textures: target k overlaps target
// k-1 (both live at node k) so they never share, while target k and target
// k-2 are disjoint and do. That is why the declarations are the honest DAG
// (a pass reads whatever chain index it was authored to read, at any
// distance) and the two-slot alternation still falls out for the linear case
// -- including the CANVAS, whose lifetime ends at pass 0 and which therefore
// hosts the even-numbered targets. Declaring two ping-pong textures by hand
// would have produced the same picture with THREE pool slots and a chain
// restriction the material system does not have.
//
// TonemapNode is the exact NRI counterpart of the deleted Render/TonemapPass.cpp: the
// same two offline shader bins, the same POINT/clamp sampler (1:1 canvas ->
// backbuffer), the same single 3-vertex draw with no vertex buffer
// (data/shaders/tonemap.hlsl builds the triangle from SV_VertexID).
//
// WHAT IT OWNS: one sampler, one pipeline layout registered in the vehicle's
// NriPipelineCache (descriptor set space0 = { t0 texture, s0 sampler }; no
// root constants -- the shader reads none), one descriptor pool holding ONE
// descriptor set PER FRAME SLOT, and its cached SHADER_RESOURCE views.
//
// ===================================================================
// SOURCE VIEWS AND THE POOL -- the lifetime rule BOTH nodes here obey.
// ===================================================================
// Every texture either node samples is a graph TRANSIENT the graph owns: the
// canvas, and each post pass's target. Two separate facts make caching a
// descriptor over one dangerous, and they need two separate mechanisms:
//
//  1. THE GRAPH MAY DESTROY A POOL TEXTURE MID-EXECUTE. RealizePool buries a
//     slot past the compiled slot count (a shrink) or one whose desc changed,
//     and both run inside Execute() -- after every declaration, before every
//     exec fn -- so the owner gets no ordering hook. Since Task 10 the frame's
//     slot count genuinely varies at runtime (a post chain appears when its
//     compile lands, goes away, or re-wires to a different slot count), which
//     is what made this reachable. NRI may then hand the recreated texture the
//     address the destroyed one vacated, so a pointer-keyed cache reports a
//     HIT and binds freed memory.
//     => Both nodes compare RenderGraph::PoolEpoch() at record time and bury
//     their whole view cache when it moved. That is the MECHANISM; read that
//     accessor's contract block.
//
//  2. THE SOURCE MAY LEGITIMATELY CHANGE FROM FRAME TO FRAME. The tonemap
//     samples the canvas with no chain and the last pass's target with one,
//     and a scene's chain binds a few frames into an ordinary run.
//     => Both nodes hold ONE DESCRIPTOR SET PER FRAME SLOT and rewrite only
//     the slot the current frame owns, whose previous submission has already
//     retired (the pacing wait inside NriSwapChain::AcquireNextTexture). No
//     idle, no error: a changed source is ordinary.
//
// InvalidateSource()/InvalidateSources() remain for TEARDOWN and RESIZE, where
// the owner releases the pool and then drains the graveyard -- there is no
// later Record() to notice the epoch on those paths, and the views must be
// buried before the device goes away.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/GlobalParams.hpp>   // GlobalParams (16 bytes, held by value)
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>        // kSwapchainFramesInFlight

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;
    class NriTextureCache;
    struct PostChainDesc;

    // A SHADER_RESOURCE view over one graph transient, keyed by the texture it
    // views. Shared by both nodes here because they cache them for the same
    // reason and drop them on the same two triggers -- see SOURCE VIEWS AND
    // THE POOL above.
    struct FullscreenSourceView
    {
        nri::Texture*    texture = nullptr;
        nri::Descriptor* view    = nullptr;
    };

    // =====================================================================
    // The pipeline-layout SHAPE one fullscreen material pass needs, as a
    // standalone buildable object rather than a block of locals inside
    // PostChainNode -- so the [nri] tests can pin it on a headless device.
    // The sprite twin is SpriteMaterialLayout (Batch2DNode.hpp); the two
    // differ because the two TEMPLATES differ, and the differences are the
    // whole reason this is its own type:
    //
    //   * NO ROOT CONSTANTS AT ALL. fullscreen_material.hlsl has no push
    //     constants -- b0 is the MATERIAL cbuffer here, where the sprite
    //     template's b0 is the batcher's projection block. So the material CB
    //     is b0 (kMaterialCbSlot) and globals is b1 (kGlobalCbSlot), one slot
    //     below their sprite positions.
    //   * THE TEXTURE RANGE CARRIES THE CHAIN INPUTS. GenerateMaterialBindings
    //     emits the material's declared textures at t0.. and then the
    //     reserved InputTexture(N) slots immediately after, so ONE contiguous
    //     SRV range of (textureCount + chainInputs) covers both -- which is
    //     also what lets D3D12 merge them into one root table.
    //
    // Unchanged from the sprite layout, and for the same reasons: root
    // register space 0 alongside a space-0 descriptor set (legal because
    // rootDescriptorNum and rootSamplerNum are both ZERO -- NRI's
    // Source/Validation/DeviceVal.hpp guard), CBVs before SRVs before the
    // sampler (the D3D12 table merge), and every range visible to BOTH stages
    // (a template's %{VERTEX_BODY} may read params and sample textures).
    // =====================================================================
    struct ARCANE_API FullscreenMaterialLayout
    {
        // Fills everything for a material whose merged template has `cbSize`
        // bytes of numeric params (0 == none, and then there is no b0 range at
        // all), `textureCount` declared texture params and `chainInputs`
        // reserved upstream-input slots. `desc` is left pointing at THIS
        // object's arrays, so it is only valid while this object lives.
        void Build(std::uint32_t cbSize, std::uint32_t textureCount, std::uint32_t chainInputs);

        // Range indices into `ranges`, for UpdateDescriptorRanges. kNoRange
        // when the material's shape declares no such range.
        static constexpr std::uint32_t kNoRange = 0xFFFFFFFFu;
        std::uint32_t materialCb = kNoRange;   // b0
        std::uint32_t globalsCb  = kNoRange;   // b1
        std::uint32_t textures   = kNoRange;   // t0 .. t(textureCount + chainInputs - 1)
        std::uint32_t sampler    = kNoRange;   // s0

        nri::DescriptorRangeDesc ranges[4]{};
        nri::DescriptorSetDesc   set{};
        nri::PipelineLayoutDesc  desc{};

        FullscreenMaterialLayout() = default;
        // `desc` holds interior pointers -- a copy would name the source's
        // arrays. Build it where it is used.
        FullscreenMaterialLayout(const FullscreenMaterialLayout&)            = delete;
        FullscreenMaterialLayout& operator=(const FullscreenMaterialLayout&) = delete;
    };

    // =====================================================================
    // PostChainNode -- ONE object serving EVERY pass of the scene's post
    // chain, because a chain shares one merged template, one packed constant
    // buffer and one binding shape (BuildMaterialChainSource's contract).
    // Per pass it owns only what genuinely differs: the PSO (different
    // bytecode) and the descriptor sets (different input textures).
    //
    // WHAT IT OWNS, all created once at Create():
    //   * a 1x1 WHITE texel (an unbound/unresolvable declared texture param --
    //     what FullscreenMaterialPass binds) and a 1x1 TRANSPARENT BLACK one
    //     (a chain input slot with nothing wired into it -- likewise), plus
    //     their views;
    //   * one LINEAR/wrap sampler, matching FullscreenMaterialPass::Init's;
    //   * one descriptor pool sized for kMaxPasses x kSwapchainFramesInFlight
    //     sets;
    //   * the per-frame-slot constant-buffer arena the b0/b1 views name.
    // The pipeline LAYOUT and the PSOs come from the vehicle's shared
    // NriPipelineCache.
    //
    // THE CONSTANT-BUFFER ARENA is the Batch2DNode pattern, and it is here for
    // the identical NRI reason (that file's header states it in full): a
    // descriptor-set constant buffer is bound through an nri::Descriptor whose
    // (buffer, offset) is BAKED IN at creation, SetDescriptorSetDesc carries
    // no dynamic offset in NRI v180, and the one binding that does take a
    // dynamic offset -- CmdSetRootDescriptor -- is refused by the register-
    // space rule. So the upload ring cannot back these, and this node owns one
    // small HOST_UPLOAD buffer carved into TWO fixed regions per frame slot:
    // region 0 the globals CB, region 1 the ONE material CB the whole chain
    // shares. It is deliberately a second, smaller arena rather than a shared
    // helper -- see the task report; the shapes differ (2 fixed regions here,
    // 1 + kMaxMaterialSlots there) and the extraction is a later cleanup.
    //
    // SOURCE VIEWS AND THE POOL. Every texture a pass samples is a graph
    // TRANSIENT (the canvas, or another pass's target) whose physical texture
    // the graph can destroy on TWO paths, not one -- see SOURCE VIEWS AND THE
    // POOL above for the full mechanism: mid-execute via RealizePool (a
    // shrink or a desc change, caught by comparing PoolEpoch() at record
    // time), and at teardown/resize via RenderGraph::ReleaseGpuResources,
    // which NriGraphContext::Resize and ~NriGraphContext call. Both of those
    // call InvalidateSources() in the same breath, BEFORE that release, which
    // is what keeps this node's descriptors from ever naming a freed texture
    // on THAT path. The per-(pass, frame slot) rebind below is safe for the
    // same reason Batch2DNode's arena is: a frame slot's previous submission
    // has retired before this frame records into it (the pacing wait inside
    // NriSwapChain::AcquireNextTexture).
    // =====================================================================
    class ARCANE_API PostChainNode
    {
    public:
        // Builds the fallback texels, the sampler, the pool and the arena.
        // Null (already logged + latched) on failure. The chain's own
        // pipelines are built later, per bound chain, by PrepareChain.
        static std::unique_ptr<PostChainNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- see Batch2DNode's matching comment.
        ~PostChainNode();

        PostChainNode(const PostChainNode&)            = delete;
        PostChainNode& operator=(const PostChainNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Buries every view over a graph transient and forgets what each
        // descriptor set has bound, so the next frame rebuilds both. MUST be
        // called before the graph releases its transient pool -- see SOURCE
        // VIEWS AND THE POOL above. Idempotent.
        void InvalidateSources(Graveyard& graveyard, std::uint64_t fence);

        // Resolves `desc` into pipelines, descriptor sets and packed constants
        // for THIS frame, and returns how many passes may be recorded --
        // 0 meaning "bypass the chain entirely", which the declarator honours
        // by adding no node at all. Called at DECLARATION time, deliberately,
        // and for the same two reasons Prepare is: a PSO compile and
        // MaterialInstance::PackCB must not happen inside the frame's open
        // command buffer.
        //
        // `globals` is copied (16 bytes) rather than borrowed, because Record
        // runs long after the frame driver's GlobalParams reference is gone.
        [[nodiscard]] std::uint32_t PrepareChain(const PostChainDesc& desc,
                                                 const GlobalParams& globals,
                                                 nri::Format targetFormat);

        // Records pass `pass` into an ALREADY-OPEN raster pass whose single
        // colour attachment is `target`. `sources` is chainInputSlots long, in
        // InputTexture slot order; an invalid handle (a slot this pass wired
        // nothing into) binds the black texel, exactly as the NVRHI chain
        // does. Emits no barrier: the executor derives them.
        void Record(RenderGraphNodeContext& context, std::uint32_t pass,
                    std::span<const RgTexture> sources, RgTexture target,
                    std::uint32_t frameSlot);

        // How many chain passes one frame may record, how many declared
        // texture params one chain may carry, and the arena's region size
        // before alignment. Pool/arena sizing constants, not opinions about
        // content -- the same reasoning as Batch2DNode's caps: a pool's
        // capacity is fixed at creation and NRI cannot free one descriptor
        // set, so the alternative to a cap is discovering the limit mid-frame.
        // Over either cap the chain is refused wholesale (the frame renders
        // canvas -> tonemap) with one ERROR naming the constant to raise.
        static constexpr std::uint32_t kMaxPasses   = 8;
        static constexpr std::uint32_t kMaxTextures = 8;
        // kMaxPassInputs (Material/MaterialSource.hpp) is 4; pinned by a
        // static_assert in the .cpp so this cannot silently fall behind.
        static constexpr std::uint32_t kMaxInputs   = 4;
        static constexpr std::uint32_t kCbMaxBytes  = 256;
        // Region 0 of every frame slot is the globals CB, region 1 the ONE
        // material CB the whole chain shares.
        static constexpr std::uint32_t kCbRegionsPerFrame = 2;
        static constexpr std::uint32_t kGlobalsRegion     = 0;
        static constexpr std::uint32_t kMaterialRegion    = 1;

        // PURE and public for the same reason Batch2DNode's twins are: they
        // carry invariants whose violation would be silent, and no device can
        // show them. The stride must be BOTH a multiple of the device's
        // constant-buffer alignment (or every view past the first is
        // misaligned) AND at least kCbMaxBytes (or the packed bytes spill into
        // the next region).
        [[nodiscard]] static constexpr std::uint64_t CbRegionStride(
            std::uint64_t constantBufferAlignment) noexcept
        {
            return constantBufferAlignment <= 1
                 ? kCbMaxBytes
                 : ((kCbMaxBytes + constantBufferAlignment - 1) / constantBufferAlignment)
                       * constantBufferAlignment;
        }

        [[nodiscard]] static constexpr std::uint64_t CbRegionOffset(
            std::uint64_t regionStride, std::uint32_t frameSlot, std::uint32_t region) noexcept
        {
            return ((std::uint64_t)frameSlot * kCbRegionsPerFrame + region) * regionStride;
        }

        // ===== THE OWNER'S DECLARATION-TIME POOL SYNC (whole-branch review, I1)
        // Same operation as the private, exec-fn-driven SyncPoolEpoch below --
        // "if the graph's pool epoch moved, bury every cached view and start
        // over" -- reachable by the object that OWNS this node, from OUTSIDE a
        // frame. It exists because the exec-fn call alone cannot cover a node
        // that is not in the frame at all, and that is the reachable case:
        // a post chain that has been re-wired away, an outline chain with the
        // selection toggled off. Such a node keeps descriptors over pool
        // textures the graph retired frames ago, and buries them only whenever
        // it next happens to record -- long after the texture's own burial.
        //
        // The OWNER therefore calls this on EVERY node it owns, in frame,
        // skipped or idle, at DECLARATION time -- which is before Execute()
        // flushes the graph's retirement staging area (RenderGraph::
        // m_retiredPool), so the views are always buried first. The two halves
        // are a pair; see NriGraphContext::BuildFrame.
        //
        // Idempotent, cheap (one uint64 comparison) and a no-op before the
        // graph's first Execute() (no lane latched yet, and nothing cached).
        void SyncPoolEpoch(const RenderGraph& graph);

    private:
        PostChainNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateFallbackTexels();
        bool CreateSampler();
        bool CreatePool();
        bool CreateConstantArena();

        // Rebuilds the layout, the per-pass PSOs and the descriptor sets for a
        // chain whose identity stamp changed. False (already logged) refuses
        // the chain for this frame.
        [[nodiscard]] bool BuildChain(const PostChainDesc& desc, nri::Format targetFormat);
        // The cached SHADER_RESOURCE view over `texture`, creating it on first
        // sight. Null (already reported) if NRI refused it.
        [[nodiscard]] nri::Descriptor* EnsureView(const nri::CoreInterface& core,
                                                  nri::Texture* texture);
        // Drops every cached view when the graph's POOL EPOCH moved, i.e. when
        // RealizePool buried a pool texture this node may hold a descriptor
        // over. Called at the top of every Record -- see SOURCE VIEWS AND THE
        // POOL, mechanism 1.
        void SyncPoolEpoch(const RenderGraphNodeContext& context);
        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot, std::uint32_t region) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot, region);
        }

        struct Pass
        {
            std::uint64_t  shaderPairId = 0;   // CONTENT hash of the blob pair
            // Held so the bytecode the pipeline cache's fill callback points at
            // outlives the GetGraphics call (its fill contract, rule 2).
            std::shared_ptr<const std::vector<std::uint8_t>> vs, ps;
            nri::Pipeline*      pipeline = nullptr;
            nri::DescriptorSet* set[kSwapchainFramesInFlight]{};
            // What this pass's set for that frame slot currently has bound in
            // its texture range -- the declared params first, then the chain
            // inputs. A rebind happens only when one of them changes.
            nri::Texture* bound[kSwapchainFramesInFlight][kMaxTextures + kMaxInputs]{};
            bool          written[kSwapchainFramesInFlight]{};
        };

        // See Batch2DNode::kShaderPairBase and TonemapNode::kShaderPairId: one
        // shared cache, so the nodes' opaque shader-pair id spaces must not
        // overlap. A post pass's id is the CONTENT hash of its blob pair with
        // the high bit set (like a registered sprite material's) -- and the
        // layout id is part of GraphicsKey anyway, so the two cannot collide
        // even on identical bytecode.
        static constexpr std::uint64_t kShaderPairMark = 0x8000000000000000ull;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;
        // The vehicle's SHARED image residency cache (NRI Phase 3, Task 2) --
        // borrowed, never owned. This is what closed THE POST TEXTURE GAP: a
        // declared texture param is now made resident through the same cache
        // Batch2DNode uses, so a post pass and a sprite naming one image share
        // one upload.
        NriTextureCache*  m_textures  = nullptr;

        nri::Texture*    m_white     = nullptr;
        nri::Descriptor* m_whiteView = nullptr;
        nri::Texture*    m_black     = nullptr;
        nri::Descriptor* m_blackView = nullptr;
        nri::Descriptor* m_sampler   = nullptr;

        nri::DescriptorPool* m_pool = nullptr;

        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;
        nri::Descriptor* m_globalsView[kSwapchainFramesInFlight]{};
        nri::Descriptor* m_materialView[kSwapchainFramesInFlight]{};

        // The bound chain's shape. Rebuilt only when `m_stamp` changes.
        std::uint64_t m_stamp        = 0;
        bool          m_ready        = false;
        bool          m_refused      = false;   // this stamp was already reported unbuildable
        std::uint32_t m_layoutId     = NriPipelineCache::kInvalidLayout;
        std::uint32_t m_cbSize       = 0;
        std::uint32_t m_textureCount = 0;
        std::uint32_t m_chainInputs  = 0;
        // FullscreenMaterialLayout's index for the SRV range, stored rather
        // than re-derived at record time so the two cannot drift.
        std::uint32_t m_textureRange = FullscreenMaterialLayout::kNoRange;
        nri::Format   m_targetFormat = nri::Format::UNKNOWN;
        // The DECLARED param textures + views, resolved at PrepareChain time
        // through m_textures. Null means "the white texel", the same
        // substitution FullscreenMaterialPass makes for an unbound handle.
        // Held as BOTH because Record compares TEXTURES for its rebind
        // decision and binds VIEWS -- and unlike a chain input's view, one of
        // these is NOT over a graph pool texture, so it must never reach
        // EnsureView (whose cache is buried on every pool-epoch move).
        nri::Texture*    m_paramTextures[kMaxTextures]{};
        nri::Descriptor* m_paramViews[kMaxTextures]{};

        std::vector<Pass>                 m_passes;
        std::vector<FullscreenSourceView> m_views;
        // The graph pool epoch m_views was built against -- see
        // SyncPoolEpoch. 0 is also RenderGraph's starting value, which is
        // correct: an empty cache has nothing to invalidate.
        std::uint64_t                     m_poolEpoch = 0;

        std::vector<std::uint8_t> m_packed;    // PackCB output, refreshed every frame
        GlobalParams              m_globals{}; // this frame's engine-global constants

        bool m_warnedViewChurn = false;
    };

    class ARCANE_API TonemapNode
    {
    public:
        // Loads tonemap_vs/ps through the vehicle and builds the sampler,
        // layout, pool and set. Null (already logged + latched) on failure.
        static std::unique_ptr<TonemapNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- see Batch2DNode's matching comment.
        ~TonemapNode();

        TonemapNode(const TonemapNode&)            = delete;
        TonemapNode& operator=(const TonemapNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Buries every cached source view and forgets what each set has bound,
        // so the next frame rebuilds both. For TEARDOWN and RESIZE, where the
        // owner releases the pool and drains -- the in-run case is the epoch
        // check inside Record (see SOURCE VIEWS AND THE POOL). Idempotent.
        void InvalidateSource(Graveyard& graveyard, std::uint64_t fence);

        // Records the tonemap into an ALREADY-OPEN raster pass whose single
        // colour attachment is `target`. `source` is the linear HDR texture to
        // sample -- the canvas on a frame with no post chain, the chain's last
        // target on one with it, and IT MAY DIFFER BETWEEN CONSECUTIVE FRAMES:
        // a scene's chain binds a few frames into an ordinary run. That is
        // ordinary here, which is why `frameSlot` is a parameter -- only that
        // slot's descriptor set is rewritten, and its previous submission has
        // already retired.
        //
        // The PSO's attachment format is read back from `target`'s resolved
        // texture rather than assumed: NRI resolves a swapchain's channel order
        // instead of letting anyone pin it (NriSwapChain::Format), and a
        // pipeline bakes its attachment formats at creation. Emits no barrier:
        // the executor derives them.
        void Record(RenderGraphNodeContext& context, RgTexture source, RgTexture target,
                    std::uint32_t frameSlot);

        // The owner's declaration-time pool sync -- see PostChainNode's
        // identically-named overload for the full reasoning; this node caches
        // views over pool textures for exactly the same reason and owes the
        // same discipline.
        void SyncPoolEpoch(const RenderGraph& graph);

    private:
        TonemapNode() = default;

        bool Init(NriGraphContext& context);
        // Binds `texture` into frame slot `frameSlot`'s descriptor set,
        // creating (or reusing) the SHADER_RESOURCE view over it.
        [[nodiscard]] bool EnsureSource(const nri::CoreInterface& core, nri::Texture* texture,
                                        std::uint32_t frameSlot);
        // Drops every cached view when the graph's POOL EPOCH moved -- the same
        // mechanism, for the same reason, as PostChainNode's.
        void SyncPoolEpoch(const RenderGraphNodeContext& context);

        // See Batch2DNode::kShaderPairBase: one shared cache, so the two nodes'
        // opaque shader-pair id spaces must not overlap.
        static constexpr std::uint64_t kShaderPairId = 0x3000;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;

        // Owned by the vehicle's bin cache; outlives this node, which the
        // pipeline cache's fill contract (rule 2) requires.
        std::span<const std::uint8_t> m_vs;
        std::span<const std::uint8_t> m_ps;

        nri::Descriptor* m_sampler = nullptr;

        nri::DescriptorPool* m_pool = nullptr;
        // ONE SET PER FRAME SLOT (mechanism 2 above). Task 8 held a single
        // shared set, which was correct while nothing in it was per-frame --
        // Task 10's chain makes the source change mid-run, and rewriting a
        // shared set with frames in flight is a real hazard.
        nri::DescriptorSet*  m_set[kSwapchainFramesInFlight]{};
        // The texture frame slot i's set currently binds at t0.
        nri::Texture*        m_bound[kSwapchainFramesInFlight]{};

        std::uint32_t m_layoutId = NriPipelineCache::kInvalidLayout;

        std::vector<FullscreenSourceView> m_views;
        std::uint64_t                     m_poolEpoch = 0;
        bool                              m_warnedViewChurn = false;
    };

    // Declares `passCount` post-chain nodes into `graph` -- one per chain
    // pass, named "post0".."postN-1" -- and hands back the LAST pass's target,
    // which is what the tonemap then samples instead of the canvas.
    //
    // Each pass CREATES its own RGBA16F transient at the canvas extent and
    // declares a Read of every upstream target its wiring names (`scene` for a
    // kSceneInput entry). The two-slot ping-pong is the pool allocator's
    // answer to those lifetimes, not something declared here -- see THE
    // PING-PONG IS DERIVED at the top of this file.
    //
    // `desc` is read for its per-pass WIRING and slot count only; the bytecode
    // and values were already consumed by PostChainNode::PrepareChain, which
    // is also what decided `passCount`. `context` may be null -- see
    // AddBatch2DNode's signature note.
    ARCANE_API RgTexture AddPostChainNodes(RenderGraph& graph, NriGraphContext* context,
                                           RgTexture scene, const PostChainDesc& desc,
                                           std::uint32_t passCount,
                                           std::uint32_t width, std::uint32_t height);

    // Declares the tonemap node into `graph`: imports the frame's FINAL TARGET
    // as its colour attachment, reads `source` as a shader resource, and hands
    // that target's handle back for the nodes downstream (outline composite,
    // HUD, capture). `context` may be null -- see AddBatch2DNode's signature
    // note.
    //
    // `offscreenOutput` chooses which target (NRI Phase 3, Task 7):
    //   * NULL -- the swapchain backbuffer, via ImportSwapChainTexture: no
    //     nri::Texture* exists at declaration time because the graph owns
    //     acquire/present, and the fixed PRESENT exit state is what makes "the
    //     graph presents" structural.
    //   * NON-NULL -- an ordinary ImportTexture of the vehicle's PERSISTENT
    //     output, with a contents-discarding entry ({NONE, UNDEFINED, ALL},
    //     the same triple a freshly acquired backbuffer carries -- correct
    //     because the tonemap's opaque fullscreen triangle writes every pixel,
    //     and the one entry that is D3D12-enhanced-barrier legal on frame 1 as
    //     well as frame N) and a SHADER_RESOURCE exit, so the frame ends with
    //     the texture in a state a sampler can read. NOTHING is presented.
    // The pointer is recorded, never dereferenced here.
    ARCANE_API RgTexture AddTonemapNode(RenderGraph& graph, NriGraphContext* context,
                                        RgTexture source,
                                        nri::Texture* offscreenOutput = nullptr);
}
