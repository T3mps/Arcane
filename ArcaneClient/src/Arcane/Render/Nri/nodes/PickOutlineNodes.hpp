#pragma once

// PickOutlineNodes -- the graph's ENTITY-ID pass (PickNode) and its JUMP-FLOOD
// SELECTION OUTLINE (OutlineNode), NRI Phase 2 Task 11.
//
// The frame's tail grows, when and only when the driver asks for it:
//
//   batch2d -> [post..] -> tonemap -> pick -> pickreadback -> outlineseed
//           -> outlinejfa0 .. outlinejfaN-1 -> outlinecomposite -> [capture]
//
// EDITOR WIRING IS PHASE 3. What lands here is the NODES, driven by
// `--nri-graph --pick-probe x,y` and a SCRIPTED selection, so they render real
// ReferenceProject content and are desk-comparable against the NVRHI twins
// (Render/PickBuffer.cpp and Render/SelectionOutline.cpp). With the probe flag
// absent NOTHING below is declared, created or recorded, and the frame is
// byte-for-byte Task 10's -- which is what keeps the batch/post/full stage
// goldens the compare targets they are.
//
// ===================================================================
// ONE EMITTER, TWO RECORDERS -- where the entity ids come from.
// ===================================================================
// Not from the batcher. `CollectPickables` (Render/PickEmit.hpp) is a PURE
// walk of the Astra registry that appends one PickDrawable per pickable
// silhouette in canvas pixels, and the k-th appended drawable IS hit-proxy id
// k+1 (`PickEntityForId` inverts it). The graph path consumes exactly that
// vector -- handed in through NriGraphContext::FrameDesc::pickables by the
// frame driver, which owns the registry -- and turns it into vertices through
// the SHARED `BuildPickIdGeometry`, the same function PickBuffer::RenderIdPass
// calls. So there is one id assignment, not two that agree until one is
// edited. No Batcher2D drain interface changed and no plugin ABI moved.
//
// ===================================================================
// THE READBACK, and why it does not stall.
// ===================================================================
// PickBuffer::Pick copies one texel and then calls waitForIdle -- correct for
// an on-demand editor click, and exactly the anti-pattern for a per-frame
// graph path. Here the copy is a graph COPY node into an IMPORTED
// HOST_READBACK buffer carved into ONE REGION PER FRAME SLOT, and the CPU
// reads a region only at the moment the graph is about to overwrite it:
//
//   * the executor ACQUIRES the backbuffer before it resets this frame's
//     command allocator (RenderGraph::Execute step 1 vs step 3), and the
//     pacing wait inside NriSwapChain::AcquireNextTexture is what makes that
//     slot safe to reuse at all -- i.e. the submission that last recorded into
//     frame slot s has RETIRED by the time any exec fn of this frame runs;
//   * so the readback node's exec fn drains slot s FIRST and records into it
//     SECOND, and the value it drains is the one written kSwapchainFramesInFlight
//     frames ago. No fence query, no idle, no extra API: the same argument that
//     makes Batch2DNode's constant-buffer arena safe.
//
// LATENCY IS THEREFORE kSwapchainFramesInFlight FRAMES, not one -- which is
// why --pick-probe refuses an open-ended run (HostConfig::Parse) and why a
// probe wants a --frames N comfortably above that.
//
// The ReadbackHost pattern (a HOST_READBACK buffer as a copy destination,
// mapped afterwards) is adapted from .example/NRISamples' Source/Readback.cpp
// (MIT -- see that tree's LICENSE.txt), reached here through NriSmoke.cpp and
// NriGraphContext's own capture node.
//
// ===================================================================
// THE PIPELINE SHAPES, source-verified against the shaders.
// ===================================================================
// The plan's input said "JFA per-step push constants are root constants". That
// is true of the ID pass and FALSE of the outline passes, and the difference is
// in the HLSL, not in NRI:
//
//   * data/shaders/entity_id.hlsl declares its b0 block under
//     `#if SPIRV [[vk::push_constant]]`, exactly like sprite.hlsl -- so
//     PickNode's layout is ROOT CONSTANTS ONLY (16 bytes, VERTEX stage) and
//     carries no descriptor set at all;
//   * data/shaders/outline_seed|jfa|composite.hlsl declare plain
//     `cbuffer ... : register(b0)` with NO push-constant variant, and the
//     SPIR-V build shifts b0 to set-0 binding 256 (compile-shaders.bat's
//     `-fvk-b-shift 256 0`). A root constant lowers to a VK push-constant
//     block, which those shaders do not declare -- binding one would leave the
//     uniform block unwritten on Vulkan. Changing the HLSL is not an option
//     either: SelectionOutline.cpp binds real constant buffers there and the
//     NVRHI path is the compatibility floor.
//     => the three outline passes take DESCRIPTOR-SET constant buffers out of
//     a per-frame-slot HOST_UPLOAD arena, the idiom Batch2DNode and
//     PostChainNode already carry (and which the same carry names as the
//     fallback for exactly this case).
//
// All three outline passes declare the IDENTICAL binding shape -- one space-0
// set of { b0 CONSTANT_BUFFER, t0 TEXTURE }, FRAGMENT stage, and no sampler at
// all (every one of them uses `.Load`, an integer texel fetch) -- so ONE
// registered pipeline layout serves the whole chain; NriPipelineCache's dedup
// makes that structural rather than a coincidence.
//
// ===================================================================
// POOL EPOCH DISCIPLINE -- read RenderGraph::PoolEpoch's contract block.
// ===================================================================
// OutlineNode caches SHADER_RESOURCE views over graph TRANSIENTS (the id
// target, the seed target, both ping-pong targets), so it obeys the same two
// mechanisms FullscreenNodes documents in full: SyncPoolEpoch as the FIRST
// statement of every Record, burying stale views at DebugSubmitCount(); and
// one descriptor set per (region, frame slot) so a changed source is ordinary.
// PickNode caches nothing over the pool -- its only view-shaped resource is
// its own readback buffer -- so it needs neither.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/PickEmit.hpp>        // PickDrawable, PickIdVertex, BuildPickIdGeometry
#include <Arcane/Render/Swapchain.hpp>       // kSwapchainFramesInFlight

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

    // The entity-id target's format: single-channel 32-bit UINT, 0 ==
    // background, k == the k-th drawn silhouette. Integer so the PS writes an
    // exact id (no float rounding) -- PickBuffer.cpp's kIdFormat, verbatim.
    inline constexpr nri::Format kGraphPickIdFormat = nri::Format::R32_UINT;

    // The jump-flood field's format. SelectionOutline.cpp's kFieldFormat:
    // .xy is a normalized [-1,1] silhouette-edge position, .z a +-1
    // select/hover tag, .w a 0..1 coverage -- renderable and sample-able on
    // both backends.
    inline constexpr nri::Format kGraphOutlineFieldFormat = nri::Format::RGBA16_SNORM;

    // How many jump-flood steps a `width` x `height` field needs: jumps
    // 2^(N-1) .. 1 sum to 2^N - 1, so N = ceil(log2(maxDim)) reaches every
    // pixel of the field from any seed. Always at least 1.
    //
    // DELIBERATELY DIFFERENT FROM SelectionOutline's `JfaPassCount`, which
    // derives its count from the outline THICKNESS (32 px) instead: that is an
    // optimisation the composite makes legal (it discards any pixel farther
    // than half the outline width from an edge, so the field only has to be
    // exact near a silhouette). Both produce the same composited picture; this
    // one produces a COMPLETE field, which is what the brief pins and what a
    // debug visualisation of the field would want. If a profile ever shows the
    // extra passes matter, switching to the thickness-derived count is a
    // one-function change with no other consequence.
    [[nodiscard]] ARCANE_API std::uint32_t OutlineJfaStepCount(std::uint32_t width,
                                                               std::uint32_t height) noexcept;

    // =====================================================================
    // PickNode -- the entity-id (hit-proxy) pass plus its 1-pixel readback.
    //
    // WHAT IT OWNS, all created once at Create():
    //   * the pipeline layout (root constants b0 only) registered in the
    //     vehicle's NriPipelineCache -- no descriptor pool, no sets, no
    //     sampler, because entity_id.hlsl binds nothing else;
    //   * the vertex-input desc, held as MEMBERS because
    //     GraphicsPipelineDesc::vertexInput is a pointer the cache
    //     dereferences after the fill callback returns;
    //   * the HOST_READBACK buffer (one region per frame slot) and its
    //     persistent mapping.
    // The vertex/index STREAMS are per-frame ring allocations made inside the
    // exec fn, exactly like Batch2DNode's -- never at declaration time, since
    // the vehicle calls ring.BeginFrame AFTER the frame is declared.
    // =====================================================================
    class ARCANE_API PickNode
    {
    public:
        // Loads entity_id_vs/ps through the vehicle, registers the layout and
        // builds the readback buffer. Null (already logged + latched) on
        // failure.
        static std::unique_ptr<PickNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- see Batch2DNode's matching comment.
        ~PickNode();

        PickNode(const PickNode&)            = delete;
        PickNode& operator=(const PickNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Turns this frame's drawables into the id-pass vertex/index arrays.
        // Called at DECLARATION time, for the same reason
        // Batch2DNode::PrepareMaterials is: it is pure CPU work (a quad
        // expansion over the scene's pickables) and the recording window must
        // not carry it. `drawables` is borrowed only for this call.
        void PrepareDrawables(std::span<const PickDrawable> drawables);

        // Records the id pass into an ALREADY-OPEN raster pass whose single
        // colour attachment is the R32_UINT id target. Clears it to 0 (the
        // clear seam -- graph attachments are LOAD/STORE), then draws every
        // prepared silhouette. Emits no barrier: the executor derives them.
        void Record(RenderGraphNodeContext& context, std::uint32_t width, std::uint32_t height,
                    std::uint32_t frameSlot);

        // Records the 1-texel copy into `readback`'s region for `frameSlot`,
        // AFTER draining whatever the same region already held -- see THE
        // READBACK at the top of this file for why that ordering is the whole
        // synchronisation argument. A Copy node, so the executor opens no
        // rendering around it.
        void RecordReadback(RenderGraphNodeContext& context, RgTexture ids, RgBuffer readback,
                            std::int32_t probeX, std::int32_t probeY,
                            std::uint32_t width, std::uint32_t height, std::uint32_t frameSlot);

        // The staging buffer the graph IMPORTS, and its size. Null/0 before a
        // successful Create -- ImportBuffer stores the pointer without
        // dereferencing it, so a headless declaration drive is fine with null.
        [[nodiscard]] nri::Buffer*  ReadbackBuffer() const noexcept { return m_readback; }
        [[nodiscard]] std::uint64_t ReadbackBytes()  const noexcept { return m_readbackBytes; }

        // The most recently DRAINED id, or nullopt until one has landed (the
        // first kSwapchainFramesInFlight frames of a probe run). 0 is a
        // legitimate value: it means the probed pixel was background.
        [[nodiscard]] std::optional<std::uint32_t> LastProbeId() const noexcept
        {
            return m_hasProbe ? std::optional<std::uint32_t>(m_probeId) : std::nullopt;
        }

        // The id pass runs at 1x -- no supersampling on this path. PickBuffer
        // offers ss for the editor's viewport, where the outline's sub-pixel
        // seeding wants it; the vehicle's job is to prove the NODES, and a
        // supersampled id target is an extent multiplier the seed shader
        // already handles through gSuperSample. Stated as a constant rather
        // than left implicit so the seed CB and the sample texel cannot
        // disagree about it.
        static constexpr std::uint32_t kSuperSample = 1;

        // data/shaders/entity_id.hlsl's BatchConstants: float2 invHalfViewport
        // + float2 pad, byte-identical to PickBuffer.cpp's IdPushConstants.
        struct RootConstants
        {
            float invHalfViewportX = 0.0f;
            float invHalfViewportY = 0.0f;
            float padX = 0.0f;
            float padY = 0.0f;
        };

        // PURE and public so the [nri] tests can prove the property no device
        // can show: distinct frame slots own distinct, alignment-legal readback
        // regions. `sliceAlignment` is the device's
        // memoryAlignment.uploadBufferTextureSlice, which
        // TextureDataLayoutDesc::offset must be a multiple of.
        [[nodiscard]] static constexpr std::uint64_t ReadbackRegionStride(
            std::uint64_t rowPitch, std::uint64_t sliceAlignment) noexcept
        {
            return sliceAlignment <= 1
                 ? (rowPitch == 0 ? 4u : rowPitch)
                 : ((rowPitch + sliceAlignment - 1) / sliceAlignment) * sliceAlignment;
        }

    private:
        PickNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateReadback();

        // See Batch2DNode::kShaderPairBase / TonemapNode::kShaderPairId: one
        // shared pipeline cache, so the nodes' opaque shader-pair id spaces
        // must not overlap.
        static constexpr std::uint64_t kShaderPairId = 0x4100;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;

        // Owned by the vehicle's bin cache; outlives this node, which the
        // pipeline cache's fill contract (rule 2) requires.
        std::span<const std::uint8_t> m_vs;
        std::span<const std::uint8_t> m_ps;

        std::uint32_t m_layoutId = NriPipelineCache::kInvalidLayout;

        // MEMBERS, not locals: GraphicsPipelineDesc::vertexInput is a pointer
        // CreateGraphicsPipeline dereferences AFTER the fill callback returns.
        nri::VertexAttributeDesc m_attributes[4]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        // The readback staging buffer: kSwapchainFramesInFlight regions of
        // m_readbackStride bytes, persistently mapped (NRI's D3D12 UnmapBuffer
        // is a no-op anyway). NONE-backend MapBuffer returns null, so this node
        // is a [gpu] path from Create() down -- the same footgun the upload
        // ring and Batch2DNode's arena carry.
        nri::Buffer*  m_readback       = nullptr;
        const void*   m_readbackCpu    = nullptr;
        std::uint64_t m_readbackBytes  = 0;
        std::uint64_t m_readbackStride = 0;
        std::uint32_t m_readbackRow    = 0;   // aligned rowPitch for a 1x1 R32_UINT copy

        // Which frame slots hold a copy that has been recorded but not yet
        // drained. Cleared by the drain, set by the record -- both inside the
        // readback node's exec fn.
        bool m_pending[kSwapchainFramesInFlight]{};

        // The prepared geometry for the frame being declared. Members rather
        // than per-frame vectors so a steady-state probe run allocates nothing.
        std::vector<PickIdVertex> m_vertices;
        std::vector<std::uint32_t> m_indices;

        std::uint32_t m_probeId  = 0;
        bool          m_hasProbe = false;
        bool          m_warnedRing = false;
    };

    // =====================================================================
    // OutlineNode -- ONE object serving the WHOLE outline chain (seed, every
    // jump-flood step, and the composite), because all three passes share one
    // binding shape, one pipeline layout and one constant arena. The per-pass
    // differences are only the PSO (different bytecode, and a different target
    // format + blend for the composite) and which arena region / descriptor set
    // the pass reads -- exactly the split PostChainNode makes.
    //
    // WHAT IT OWNS, all created once at Create():
    //   * the one space-0 pipeline layout ({ b0 CB, t0 texture }, FRAGMENT),
    //     registered in the vehicle's NriPipelineCache;
    //   * one descriptor pool holding kCbRegionsPerFrame x
    //     kSwapchainFramesInFlight sets -- one per (region, frame slot);
    //   * the per-frame-slot HOST_UPLOAD constant arena those sets' b0 views
    //     name, carved into fixed regions: 0 the seed CB, 1 the composite CB,
    //     2 + step the step's JFA CB;
    //   * the cached SHADER_RESOURCE views over the graph transients each pass
    //     samples, under the POOL EPOCH discipline.
    // =====================================================================
    class ARCANE_API OutlineNode
    {
    public:
        // Loads the six outline bins through the vehicle and builds the layout,
        // pool, sets and arena. Null (already logged + latched) on failure.
        static std::unique_ptr<OutlineNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- see Batch2DNode's matching comment.
        ~OutlineNode();

        OutlineNode(const OutlineNode&)            = delete;
        OutlineNode& operator=(const OutlineNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Buries every view over a graph transient and forgets what each set has
        // bound, so the next frame rebuilds both. MUST be called before the graph
        // releases its transient pool -- TEARDOWN and RESIZE only; the in-run
        // case is SyncPoolEpoch inside Record. Idempotent.
        void InvalidateSources(Graveyard& graveyard, std::uint64_t fence);

        // COPIES this frame's selection -- the hit-proxy ids the outline traces
        // as ONE silhouette (their union), so two touching selected entities
        // show a single outline with no seam. Called at DECLARATION time, and
        // that is not a style choice: the driver's span lives only for the span
        // of one BuildFrame (NriGraphContext::CurrentSelectedIds), while
        // RecordSeed runs later, inside Execute. The same reason
        // PostChainNode::PrepareChain copies its GlobalParams by value.
        // Over kMaxSelectedIds the first kMaxSelectedIds are kept, with one WARN.
        void PrepareSelection(std::span<const std::uint32_t> selectedIds);

        // Pass 1: the SUPERSAMPLED R32_UINT id buffer -> a boundary-seeded
        // RGBA16_SNORM field at 1x, over the selection PrepareSelection copied.
        // A cursor of (-1,-1) means no hover.
        void RecordSeed(RenderGraphNodeContext& context, RgTexture ids, RgTexture target,
                        std::int32_t cursorX, std::int32_t cursorY, std::uint32_t superSample,
                        std::uint32_t width, std::uint32_t height, std::uint32_t frameSlot);

        // Pass 2, run once per step with `jump` halving from 2^(N-1) to 1.
        void RecordJfa(RenderGraphNodeContext& context, std::uint32_t step, std::int32_t jump,
                       RgTexture source, RgTexture target,
                       std::uint32_t width, std::uint32_t height, std::uint32_t frameSlot);

        // Pass 3: the field -> an anti-aliased two-colour outline BLENDED over
        // `target` (the tonemapped backbuffer). The PSO's attachment format is
        // read back from the resolved target rather than assumed, for the same
        // reason TonemapNode's is: NRI resolves the swapchain's channel order.
        void RecordComposite(RenderGraphNodeContext& context, RgTexture field, RgTexture target,
                             std::uint32_t width, std::uint32_t height, std::uint32_t frameSlot);

        // The chain's caps, and they are pool/arena SIZING constants rather
        // than opinions about content -- a pool's capacity is fixed at creation
        // and NRI cannot free a single descriptor set, so the alternative to a
        // cap is discovering the limit mid-frame.
        //
        // 16 steps floods a 65536-px field; the declarator clamps to this and
        // says so once.
        static constexpr std::uint32_t kMaxJfaSteps = 16;
        // outline_seed.hlsl's `uint4 gSelectedIds[16]` -- 64 ids. Pinned by a
        // static_assert against kMaxSelectedOutlineIds in the .cpp.
        static constexpr std::uint32_t kMaxSelectedIds = 64;
        // The largest of the three constant blocks: SeedCB is 288 bytes
        // (32 header + 256 ids). Pinned by a static_assert in the .cpp.
        static constexpr std::uint32_t kCbMaxBytes = 288;

        // Region 0 is the seed CB, region 1 the composite CB, region 2 + step
        // the JFA step's. One descriptor set per (region, frame slot).
        static constexpr std::uint32_t kSeedRegion         = 0;
        static constexpr std::uint32_t kCompositeRegion    = 1;
        static constexpr std::uint32_t kJfaRegionBase      = 2;
        static constexpr std::uint32_t kCbRegionsPerFrame  = kJfaRegionBase + kMaxJfaSteps;

        // PURE and public for the same reason Batch2DNode's twins are: they
        // carry invariants whose violation would be silent. The stride must be
        // BOTH a multiple of the device's constant-buffer alignment (or every
        // view past the first is misaligned) AND at least kCbMaxBytes (or the
        // seed CB spills into the next region).
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

    private:
        OutlineNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateBindings();
        bool CreateConstantArena();

        // Records one fullscreen pass: binds the (region, frame slot) set with
        // `source` at t0, copies `cbBytes` into that region, and draws the
        // 3-vertex triangle. False (already reported) when anything refused.
        bool RecordPass(RenderGraphNodeContext& context, std::uint32_t region,
                        std::uint64_t shaderPairId,
                        std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps,
                        RgTexture source, RgTexture target, NriPipelineCache::GraphicsKey::Blend blend,
                        const void* cb, std::size_t cbBytes, std::uint32_t frameSlot);

        // The cached SHADER_RESOURCE view over `texture`, creating it on first
        // sight. Null (already reported) if NRI refused it.
        [[nodiscard]] nri::Descriptor* EnsureView(const nri::CoreInterface& core,
                                                  nri::Texture* texture);
        // Drops every cached view when the graph's POOL EPOCH moved -- the same
        // mechanism, for the same reason, as FullscreenNodes'. Called as the
        // FIRST statement of every Record path.
        void SyncPoolEpoch(const RenderGraphNodeContext& context);

        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot, std::uint32_t region) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot, region);
        }
        [[nodiscard]] std::uint32_t SetIndex(std::uint32_t frameSlot, std::uint32_t region) const noexcept
        {
            return frameSlot * kCbRegionsPerFrame + region;
        }

        // See Batch2DNode::kShaderPairBase: one shared cache, so the nodes'
        // opaque id spaces must not overlap. PickNode is 0x4100.
        static constexpr std::uint64_t kSeedPairId      = 0x4000;
        static constexpr std::uint64_t kJfaPairId       = 0x4001;
        static constexpr std::uint64_t kCompositePairId = 0x4002;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;

        // All six bins, owned by the vehicle's bin cache (fill contract rule 2).
        std::span<const std::uint8_t> m_seedVs, m_seedPs;
        std::span<const std::uint8_t> m_jfaVs, m_jfaPs;
        std::span<const std::uint8_t> m_compositeVs, m_compositePs;

        std::uint32_t m_layoutId = NriPipelineCache::kInvalidLayout;

        nri::DescriptorPool* m_pool = nullptr;
        // One set per (frame slot, region) -- SetIndex() is the flattening.
        nri::DescriptorSet*  m_sets[kSwapchainFramesInFlight * kCbRegionsPerFrame]{};
        // What each set currently has bound at t0; a rebind happens only when
        // it changes.
        nri::Texture*        m_bound[kSwapchainFramesInFlight * kCbRegionsPerFrame]{};

        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;
        nri::Descriptor* m_cbView[kSwapchainFramesInFlight * kCbRegionsPerFrame]{};

        // SHADER_RESOURCE views over graph transients, keyed by texture. Buried
        // wholesale when the pool epoch moves.
        struct SourceView { nri::Texture* texture = nullptr; nri::Descriptor* view = nullptr; };
        // This frame's selection, COPIED by PrepareSelection at declaration time
        // because the driver's span does not survive to record time. Reserved to
        // the cap at Create, so a steady-state probe run allocates nothing.
        std::vector<std::uint32_t> m_selectedIds;

        std::vector<SourceView> m_views;
        // The graph pool epoch m_views was built against. 0 is also
        // RenderGraph's starting value, which is correct: an empty cache has
        // nothing to invalidate.
        std::uint64_t           m_poolEpoch = 0;

        bool m_warnedViewChurn = false;
        bool m_warnedIdOverflow = false;
    };

    // =====================================================================
    // The declarators. Each takes `context` as a POINTER for the same reason
    // AddBatch2DNode does: the headless [nri] frame-shape cases drive the REAL
    // declarations with no device, and a null context makes only the exec fns
    // inert.
    // =====================================================================

    struct RgPickHandles
    {
        RgTexture ids{};        // the R32_UINT entity-id transient
        RgBuffer  readback{};   // the imported HOST_READBACK staging buffer
    };

    // Declares "pick" (Raster, the id pass) and "pickreadback" (Copy, the
    // 1-texel probe copy) into `graph`. The id target is created here and read
    // by both the readback and the outline seed.
    ARCANE_API RgPickHandles AddPickNodes(RenderGraph& graph, NriGraphContext* context,
                                          std::uint32_t width, std::uint32_t height);

    // Declares "outlineseed" plus OutlineJfaStepCount(width, height)
    // "outlinejfaN" nodes, and hands back the LAST step's target -- the field
    // the composite samples. Every target is its OWN RGBA16_SNORM transient at
    // the 1x extent; the two-physical-texture ping-pong is the transient pool
    // allocator's answer to their lifetimes, exactly as it is for the post
    // chain (FullscreenNodes.hpp, THE PING-PONG IS DERIVED).
    ARCANE_API RgTexture AddOutlineNodes(RenderGraph& graph, NriGraphContext* context,
                                         RgTexture ids,
                                         std::uint32_t width, std::uint32_t height);

    // Declares "outlinecomposite": blends the field over `target` -- the
    // imported backbuffer the tonemap has already written. Declared AFTER the
    // tonemap and BEFORE the capture node, which is the editor's own
    // compositing order (EditorApp::RenderSelectionOutline runs after the scene
    // render and before the ImGui pass).
    ARCANE_API void AddOutlineCompositeNode(RenderGraph& graph, NriGraphContext* context,
                                            RgTexture field, RgTexture target,
                                            std::uint32_t width, std::uint32_t height);
}
