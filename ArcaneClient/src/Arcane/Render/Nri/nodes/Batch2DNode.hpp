#pragma once

// Batch2DNode -- the 2D batch as a RENDER GRAPH NODE (NRI Phase 2, Task 8).
//
// The CPU half of the 2D path is NOT rewritten here. `Batcher2D` still does
// all of it -- Quad/Rect/Line/Circle/Glyph accumulation, the (layer, order,
// material, texture) sort key, the index stream and the draw spans -- and
// this node consumes the result through `Batcher2D::Drain()`
// (Batch2DDrained, Batcher2D.hpp). One batcher, one batching algorithm, two
// recorders: NVRHI's `End()` and this. That is the homogenized-submission
// mandate applied to the second backend, and it is why the phase's NVRHI
// golden floor can stay bit-green while this lands.
//
// WHAT THIS NODE OWNS (all persistent, all created once at Create()):
//   * the 1x1 white texel + its SHADER_RESOURCE view -- the untextured path,
//     mirroring Batcher2D::Init;
//   * one linear/clamp sampler, matching the NVRHI batcher's;
//   * the BUILT-IN pipeline layout, registered in the vehicle's
//     NriPipelineCache: root constants b0 (the 16-byte BatchConstants from
//     data/shaders/sprite.hlsl) plus descriptor set space0 = { t0 texture,
//     s0 sampler }, and the ONE descriptor set that goes with it -- written
//     once at Create (it binds the white texel for every built-in draw, see
//     THE TEXTURE GAP below), never per frame;
//   * ONE descriptor pool serving that set AND every registered material's
//     (see below), plus the per-frame-slot constant-buffer arena those
//     materials read their b1/b2 from.
// The PIPELINES are not owned here: they come from the vehicle's shared
// NriPipelineCache, keyed by (shader pair, layout, canvas format, blend), so
// a canvas format change is a cache miss rather than a stale PSO.
//
// WHAT IT DOES NOT OWN: the vertex/index data. Both are per-frame ring
// allocations (NriUploadRing) made INSIDE the exec fn -- never at declaration
// time, because the vehicle calls `ring.BeginFrame(slot)` AFTER BuildFrame,
// so a setup-time allocation would land in the PREVIOUS frame's slot. Ring
// buffers are deliberately not graph resources (NriUploadRing.hpp).
//
// REGISTERED SPRITE MATERIALS (Task 9) are drawn too, not only the built-ins.
// A drained span whose `material` is >= 3 names a material the host registered
// with the batcher (SpriteMaterialCache -> Batcher2D::RegisterMaterial), and
// this node builds its NRI half from the SAME registration the NVRHI path
// uses -- reached through `Batcher2D::MaterialDesc(id)`:
//   * the PSO from the retained stitched blobs (Material2DDesc::vsBytes/
//     psBytes), keyed in the shared cache by their CONTENT hash, so a material
//     recompile is a cache miss rather than a stale pipeline;
//   * a pipeline layout matching sprite_material.hlsl's register map --
//     root constants b0, and ONE descriptor set at space0 carrying material CB
//     b1 (when the template has numeric params), globals CB b2, sprite texture
//     t0, declared textures t1.., sampler s0. See THE REGISTER-SPACE RULE
//     below for why b1/b2 are set entries and not root descriptors;
//   * both constant buffers out of this node's OWN per-frame-slot arena (see
//     THE CONSTANT-BUFFER ARENA below), packed with MaterialInstance::PackCB
//     once per material per frame -- the same dedup Batcher2D::End does;
//   * the material's declared TEXTURES, made resident on this device.
//
// THE REGISTER-SPACE RULE (verified against NRI's own validation sources, not
// assumed). NRI refuses a pipeline layout whose `rootRegisterSpace` equals any
// `DescriptorSetDesc::registerSpace` -- but ONLY when the layout also carries
// root DESCRIPTORS or root SAMPLERS (Source/Validation/DeviceVal.hpp, the
// `if (rootDescriptorNum || rootSamplerNum)` guard; root CONSTANTS are exempt
// because VK lowers them to push constants, which live outside the set-space
// entirely -- Source/VK/PipelineLayoutVK.hpp). Every register in
// sprite_material.hlsl is in the implicit space0, and the NVRHI path is the
// format-compatibility floor so the shaders cannot move, which means the
// texture/sampler set MUST be space0 and therefore rootRegisterSpace must be 0
// too. Adding root descriptors for b1/b2 would collide -- so the constant
// buffers are ordinary CONSTANT_BUFFER ranges in that one space-0 set. Root
// CONSTANTS (b0) stay, exactly as Task 8 left them, for exactly the reason the
// guard exempts them.
//
// THE CONSTANT-BUFFER ARENA, and why the Task 5 upload ring does not back it.
// An NRI descriptor-set constant buffer is bound through an `nri::Descriptor`
// whose (buffer, offset) is BAKED IN at creation, and `SetDescriptorSetDesc`
// carries no dynamic offset (NRI v180). The ring is a bump allocator whose
// offsets are not stable frame to frame, so a pre-created descriptor cannot
// name a ring allocation, and creating one per frame would mean per-frame
// descriptor churn plus a descriptor-set rewrite while the GPU may still be
// reading it. (The one NRI binding that DOES take a dynamic offset is
// CmdSetRootDescriptor -- refused here by the space rule above.) So this node
// owns one small HOST_UPLOAD buffer, carved into fixed, alignment-correct
// regions indexed by (frame slot, material slot); the descriptors over them are
// created once and the CPU only memcpys. The ring still carries the vertex and
// index streams, which have no descriptor and therefore no such constraint.
//
// THE TEXTURE GAP IS CLOSED (Phase 3, Task 2 -- read before debugging a
// golden). It used to be that a Batch2DDrawSpan named only an
// `nvrhi::ITexture*` -- an object on the ENGINE's device, which this node's NRI
// device cannot sample -- so the SPRITE's own texture (t0) was the white texel
// on every span and a textured sprite rendered as its vertex tint alone.
// A span now ALSO carries the image's asset Guid (Batch2DDrawSpan::textureId),
// which is device-independent, and this node resolves it through the vehicle's
// shared NriTextureCache -- the same cache the post chain uses, so an image
// named by both is uploaded once. Task 9's machinery for a material's DECLARED
// params (t1..) is unchanged in shape; it now reads that shared cache instead
// of a private one.
//
// A span whose id is NIL keeps the white texel, and that is the ordinary
// untextured case, not a degradation: Rect/Line/Circle/Triangle and every
// colored quad record nil. So does a GLYPH -- a font atlas is a runtime
// texture with no asset Guid, which is the one texture gap this task leaves
// open and names.
//
// THE PER-TEXTURE DESCRIPTOR SETS this needs are the same shape Task 9 built
// for materials: a set is written ONCE, at declaration time, and never
// rewritten while a frame is in flight. A built-in span's set is
// per (texture), because its contents (t0 + s0) carry nothing per-frame; a
// registered material's is per (material, texture, frame slot), because its
// constant-buffer views are per-frame-slot. Both are capped
// (kMaxSpriteTextures) for the reason every cap in this file exists: a
// descriptor pool's capacity is fixed at creation and NRI cannot free one set,
// so the alternative to a cap is discovering the limit mid-frame.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h> #defines ERROR via wingdi.h).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>   // kSwapchainFramesInFlight

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Arcane
{
    class Batcher2D;
    class Graveyard;
    class NriDevice;
    class NriGraphContext;
    class NriTextureCache;
    struct Batch2DDrained;
    struct Batch2DDrawSpan;
    struct Material2DDesc;

    // =====================================================================
    // The pipeline-layout SHAPE a registered sprite material needs, as a
    // standalone buildable object rather than a block of locals inside
    // Batch2DNode -- so the [nri] tests can pin it on a headless device.
    //
    // What they pin is THE REGISTER-SPACE RULE stated at the top of this file:
    // the layout carries root CONSTANTS only, so `rootRegisterSpace` and the
    // texture/sampler set may both be 0 (which sprite_material.hlsl requires,
    // since every register in it is in the implicit space0). The moment anyone
    // adds a root DESCRIPTOR or a root SAMPLER here, NRI's validation refuses
    // the layout outright -- and the test that asserts those counts are zero is
    // the cheap way to find that out, rather than a desk run on a device.
    // =====================================================================
    struct ARCANE_API SpriteMaterialLayout
    {
        // Fills everything for a material whose template has `cbSize` bytes of
        // numeric params (0 == none, and then there is no b1 range at all) and
        // `textureCount` declared textures. `desc` is left pointing at THIS
        // object's arrays, so it is only valid while this object lives.
        void Build(std::uint32_t cbSize, std::uint32_t textureCount);

        // Range indices into `ranges`, for UpdateDescriptorRanges.
        // kNoRange when the material declares no numeric params.
        static constexpr std::uint32_t kNoRange = 0xFFFFFFFFu;
        std::uint32_t materialCb = kNoRange;   // b1
        std::uint32_t globalsCb  = kNoRange;   // b2
        std::uint32_t textures   = kNoRange;   // t0 .. tN
        std::uint32_t sampler    = kNoRange;   // s0

        nri::RootConstantDesc    rootConstant{};
        nri::DescriptorRangeDesc ranges[4]{};
        nri::DescriptorSetDesc   set{};
        nri::PipelineLayoutDesc  desc{};

        SpriteMaterialLayout() = default;
        // `desc` holds interior pointers -- a copy would name the source's
        // arrays. Build it where it is used.
        SpriteMaterialLayout(const SpriteMaterialLayout&)            = delete;
        SpriteMaterialLayout& operator=(const SpriteMaterialLayout&) = delete;
    };

    class ARCANE_API Batch2DNode
    {
    public:
        // Loads the six built-in shader bins through the vehicle, creates the
        // white texel + sampler + descriptor set, and registers the pipeline
        // layout. Null (already logged + latched) on any failure -- a vehicle
        // that cannot build this node must not render a frame that silently
        // draws nothing.
        static std::unique_ptr<Batch2DNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- same shape as ~NriPipelineCache. The
        // sanctioned release is Release() at a fence the owner knows; if this
        // still holds objects it destroys them directly behind a
        // DeviceWaitIdle and says so at WARN, because there is no fence value
        // to bury against here and burying at 0 would violate Graveyard's
        // nondecreasing rule on a device the graph has been burying against
        // all run.
        ~Batch2DNode();

        Batch2DNode(const Batch2DNode&)            = delete;
        Batch2DNode& operator=(const Batch2DNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent. The caller picks the fence for the same reason
        // NriPipelineCache::Clear does: only it knows which timeline the
        // node's users submitted on.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Resolves everything one drained batch needs before a command buffer
        // exists: every span's SPRITE TEXTURE (through the shared
        // NriTextureCache) and its per-texture descriptor set, and every
        // REGISTERED material's pipeline, per-frame-slot sets and declared
        // param textures. Called at DECLARATION time -- deliberately, and it
        // is the reason this is a separate entry point rather than the top of
        // Record():
        //   * making a texture resident goes through NRI's HelperInterface,
        //     which SUBMITS AND WAITS internally, and that must not happen with
        //     the frame's command buffer open;
        //   * so does ALLOCATING a descriptor set and writing it: a set the GPU
        //     may still be reading must never be rewritten, and a set allocated
        //     mid-recording could exhaust the pool inside a frame that is
        //     otherwise fine;
        //   * MaterialInstance::PackCB is pure CPU work over the values, so it
        //     belongs outside the recording window too. Record() only memcpys
        //     the packed bytes into this frame's arena region.
        // Safe to call with an empty batch (does nothing) and safe to skip
        // entirely -- Record() then binds the white texel and the plain sprite
        // pipeline, and says so once.
        void Prepare(Batcher2D& batcher, const Batch2DDrained& batch,
                     nri::Format canvasFormat);

        // Records one frame's 2D content into an ALREADY-OPEN raster pass
        // whose single colour attachment is the canvas. In order: clear the
        // canvas (the clear seam -- graph attachments are LOAD/STORE and a
        // node that wants a cleared target clears it itself, see
        // NriGraphContext::BuildFrame), then, if the batch drew anything,
        // upload its vertex/index streams through the ring and issue one
        // CmdDrawIndexed per drained span.
        //
        // Emits NO barrier: the executor derives and batches every one of them
        // from the declarations (RgCompiled's contract block).
        //
        // `frameSlot` is the vehicle's own per-frame slot
        // (NriGraphContext::FrameSlot) -- the SAME number it gave the upload
        // ring, so this node's constant-buffer arena is double-buffered against
        // exactly the fence the swapchain already waits on.
        void Record(RenderGraphNodeContext& context, const Batch2DDrained& batch,
                    nri::Format canvasFormat, std::uint32_t frameSlot);

        // How many distinct REGISTERED materials one frame may draw through
        // this node, and how many declared textures one of them may carry.
        // Both are descriptor-pool and arena sizing constants, not opinions
        // about content: a pool's capacity is fixed at creation and NRI cannot
        // free one descriptor set, so the alternative to a cap is discovering
        // the limit mid-frame. Over either cap the material degrades to the
        // plain sprite pipeline with one ERROR naming the constant to raise.
        static constexpr std::uint32_t kMaxMaterialSlots    = 8;
        static constexpr std::uint32_t kMaxMaterialTextures = 8;
        // How many DISTINCT sprite textures one run may bind at t0 -- the
        // third pool-sizing cap, and the one Task 2 added. Past it a span
        // degrades to the white texel with one ERROR naming this constant,
        // exactly as the other two caps degrade.
        //
        // RAISED 8 -> 64 (whole-branch review, M6). 8 was chosen while the only
        // content on this path was ReferenceProject's handful of markers, with
        // the deferral stated as "revisit before the editor tasks land content".
        // Those tasks have landed and the editor opens ARBITRARY projects, where
        // a tileset plus a few characters plus UI art passes 8 without anything
        // unusual happening -- and the failure is quiet in the way that matters:
        // the sprites draw as flat tint, the ERROR is one log line, and
        // RenderErrorCount (the --nri-graph exit-code gate) does not move, so a
        // scripted run still exits 0.
        //
        // 64 rather than higher because the cost is NOT linear: PoolSizes()
        // spends (1 + kMaxSpriteTextures) descriptor sets per material slot per
        // frame slot, so this multiplies the whole pool. At 64 that is 65
        // built-in sets and 8 * 2 * 65 = 1040 material sets -- ~1105 sets and
        // ~9425 texture descriptors, tens of KiB of descriptor heap, created
        // once at node creation. Comfortably inside both backends' limits while
        // covering an ordinary 2D scene by a wide margin; 256 would quadruple it
        // for content this renderer has no other reason to expect.
        static constexpr std::uint32_t kMaxSpriteTextures   = 64;
        // Arena region size BEFORE alignment. A sprite material's cbuffer is a
        // handful of 16-byte registers (the reference material's is 32 bytes);
        // 256 is also D3D12's constant-buffer placement alignment, so on that
        // backend this is exactly one region and nothing is wasted.
        static constexpr std::uint32_t kMaterialCbMaxBytes  = 256;
        // Region 0 of every frame slot is the globals CB; 1 + n is material
        // slot n's.
        static constexpr std::uint32_t kCbRegionsPerFrame   = kMaxMaterialSlots + 1;

        // The arena's region stride on a device whose
        // deviceDesc.memoryAlignment.constantBufferOffset is
        // `constantBufferAlignment`. PURE and public for the same reason
        // CbRegionOffset is: it carries an invariant whose violation would be
        // silent. The result must be BOTH a multiple of the device's alignment
        // (or every CB view past the first is misaligned) AND at least
        // kMaterialCbMaxBytes (or a material's packed bytes spill into the next
        // region). Rounding UP satisfies both for any power-of-two alignment,
        // including one larger than kMaterialCbMaxBytes.
        [[nodiscard]] static constexpr std::uint64_t CbRegionStride(
            std::uint64_t constantBufferAlignment) noexcept
        {
            return constantBufferAlignment <= 1
                 ? kMaterialCbMaxBytes
                 : ((kMaterialCbMaxBytes + constantBufferAlignment - 1) / constantBufferAlignment)
                       * constantBufferAlignment;
        }

        // Byte offset of one constant-buffer region in the arena. PURE, static
        // and public so the [nri] tests can prove the property that matters and
        // cannot be observed from outside on a device: distinct (frame slot,
        // region) pairs never alias, and every region starts on a multiple of
        // `regionStride` -- which is what makes ONE alignment computed at
        // creation correct for every region.
        [[nodiscard]] static constexpr std::uint64_t CbRegionOffset(
            std::uint64_t regionStride, std::uint32_t frameSlot, std::uint32_t region) noexcept
        {
            return ((std::uint64_t)frameSlot * kCbRegionsPerFrame + region) * regionStride;
        }

        // How many DISTINCT non-nil texture ids `spans` names -- i.e. how many
        // per-texture descriptor sets this node must have written before it can
        // record them, and therefore how much of kMaxSpriteTextures the frame
        // spends. PURE and public for the same reason the two above are: it
        // carries an invariant no device can show and no golden can fail on.
        // Spans repeat ids freely (the sort splits a run whenever the layer or
        // the material changes), so this is a COUNT OF DISTINCT ids, not of
        // spans. Prepare() CALLS THIS (it is the frame's up-front budget
        // check), so a case that asserts it asserts the node's own arithmetic
        // rather than a lookalike.
        [[nodiscard]] static std::uint32_t DistinctTextureCount(
            std::span<const Batch2DDrawSpan> spans);

        // THE DESCRIPTOR POOL'S CAPACITY, as pure arithmetic over the three
        // caps above. PUBLIC and separated from CreateBindings for the same
        // reason CbRegionOffset is separated from the arena: a pool's sizes are
        // fixed at creation and NRI cannot free one descriptor set, so a
        // capacity that does not cover what the caps ALLOW is not a compile
        // error or a wrong pixel -- it is an allocation that fails part-way
        // through a frame at the desk, after which that material or texture
        // silently draws with the white texel. No device can show that the
        // numbers agree; a headless case can, and does ([nri], RenderGraphTest).
        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes() noexcept;

    private:
        Batch2DNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateWhiteTexel();
        bool CreateBindings();
        bool CreateConstantArena();

        // The pipeline for one drained span's material, from the shared cache.
        // Null (already logged) if the cache refused it.
        [[nodiscard]] nri::Pipeline* PipelineFor(std::uint16_t material, nri::Format canvasFormat);

        // ---- the registered-material half (Task 9) ----------------------
        // One material slot's whole NRI half. `Prepare` builds it; `Record`
        // only reads it.
        struct MaterialSlot
        {
            std::uint64_t stamp        = 0;   // identity of the registration this was built from
            std::uint64_t shaderPairId = 0;   // CONTENT hash of the blob pair -- the PSO cache key
            std::uint32_t layoutId     = NriPipelineCache::kInvalidLayout;
            std::uint32_t cbSize       = 0;   // 0 == the template declares no numeric params
            std::uint32_t cbRegion     = 0;   // 1 + slot index; region 0 is the globals CB
            // Held so the bytecode the pipeline cache's fill callback points at
            // outlives the GetGraphics call (its fill contract, rule 2) AND the
            // whole PSO's life in the cache.
            std::shared_ptr<const std::vector<std::uint8_t>> vs, ps;
            std::vector<std::uint8_t> packed;                    // PackCB output, refreshed every frame
            // Resolved by Prepare for the canvas format of the frame
            // being declared, so Record never reaches the pipeline cache from
            // inside an open command buffer -- a MISS there would compile a PSO
            // mid-recording, and a FAILED miss would latch an error on a frame
            // that is otherwise fine. Owned by the cache; borrowed here.
            nri::Pipeline*      pipeline = nullptr;
            nri::Descriptor*    cbView[kSwapchainFramesInFlight]{};
            bool ready = false;
            // Transient Prepare flag -- Batcher2D::End's `packedThisBatch`,
            // verbatim in purpose.
            bool packedThisBatch = false;

            // ---- the per-SPRITE-TEXTURE variants (Task 2) ---------------
            // A material's set carries the SPRITE's own texture at t0, and one
            // material can be drawn with several of them in one frame (the
            // sort splits a run per texture). t0 is the only thing that
            // differs, so a variant is one set per frame slot, allocated
            // lazily on first use of that (material, texture) pair and written
            // ONCE -- never rewritten, which is what keeps this node free of
            // ResetDescriptorPool and its fence discipline.
            //
            // `variants[0]` is always the NIL-texture variant (the white
            // texel), which is what an untextured span through a registered
            // material binds, and what Task 9's single set was.
            struct TextureVariant
            {
                Guid                id{};
                nri::DescriptorSet* set[kSwapchainFramesInFlight]{};
            };
            std::vector<TextureVariant> variants;

            // BuildMaterial's resolved shape, kept so a later variant can be
            // written without rebuilding the layout or re-resolving the
            // declared params.
            std::uint32_t textureCount = 0;
            // The declared params' views (t1..), in ordinal order. Null means
            // "the white texel", the same substitution the NVRHI path makes
            // for an unbound handle.
            nri::Descriptor* paramViews[kMaxMaterialTextures]{};
        };

        // The slot for `material`, building it if needed. Null (already
        // reported once) when the material cannot be honoured -- the caller
        // then draws it through the plain sprite pipeline.
        [[nodiscard]] MaterialSlot* EnsureMaterial(Batcher2D& batcher, std::uint16_t material);
        [[nodiscard]] bool BuildMaterial(MaterialSlot& slot, const Material2DDesc& desc);
        // The SHADER_RESOURCE view for asset `id`, through the vehicle's
        // SHARED cache (which uploads it on first sight). Null -- nil Guid,
        // unresolvable, undecodable, or NRI refused it -- means "use the white
        // texel", which is what the NVRHI path does for a null handle.
        [[nodiscard]] nri::Descriptor* TextureView(const Guid& id);

        // The BUILT-IN descriptor set that binds `id` at t0, allocating and
        // writing it on first use. Falls back to m_set (the white texel) for a
        // nil id, an image that is not resident, or a run that has spent
        // kMaxSpriteTextures. Declaration-time only.
        [[nodiscard]] nri::DescriptorSet* EnsureSpriteSet(const Guid& id);
        // The same for a REGISTERED material: the (material, texture) variant's
        // set for every frame slot, allocated and written on first use. Null
        // when the variant could not be created, which sends the span to the
        // nil variant.
        [[nodiscard]] MaterialSlot::TextureVariant* EnsureMaterialVariant(MaterialSlot& slot,
                                                                          const Guid& id);
        // Writes one material descriptor set: CBs, the texture range (t0 =
        // `spriteView` or the white texel, then the declared params) and the
        // sampler. Shared by BuildMaterial and the variant path so the two can
        // never disagree about a set's contents.
        void WriteMaterialSet(const MaterialSlot& slot, nri::DescriptorSet& set,
                              std::uint32_t frameSlot, nri::Descriptor* spriteView);
        // The BUILT-IN set for `id` as a pure LOOKUP -- record-time only, and
        // deliberately not the Ensure above: nothing inside the recording
        // window may allocate a set or upload a texture.
        [[nodiscard]] nri::DescriptorSet* SpriteSetFor(const Guid& id) const;
        // CbRegionOffset against this node's own stride.
        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot, std::uint32_t region) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot, region);
        }

        // sprite / circle / msdf -- Batcher2D::kMaterialSprite/Circle/Text, in
        // that order, so a drained span's `material` indexes this directly.
        static constexpr std::uint32_t kBuiltInCount = 3;

        // NriPipelineCache::GraphicsKey::shaderPairId is opaque to the cache
        // and is the CALLER's discriminator for everything the key does not
        // carry (that class's fill-contract rule 3). This node and
        // TonemapNode share one cache, so their id spaces must not overlap --
        // see FullscreenNodes.hpp's matching base.
        static constexpr std::uint64_t kShaderPairBase = 0x2000;

        struct BuiltIn
        {
            std::span<const std::uint8_t> vs;
            std::span<const std::uint8_t> ps;
        };

        NriDevice* m_device = nullptr;
        NriPipelineCache* m_pipelines = nullptr;
        // The owning vehicle. It outlives this node by construction (it holds
        // the unique_ptr).
        NriGraphContext* m_owner = nullptr;
        // The vehicle's SHARED image residency cache -- borrowed, never owned,
        // and released by the vehicle AFTER this node (its sets name the
        // cache's views). See NriTextureCache.hpp.
        NriTextureCache* m_textureCache = nullptr;

        // Bytecode is OWNED BY THE VEHICLE (NriGraphContext's bin cache) and
        // outlives this node -- which the pipeline cache's fill contract
        // (rule 2) requires, since CreateGraphicsPipeline runs after the fill
        // callback returns.
        BuiltIn m_builtIns[kBuiltInCount]{};

        nri::Texture*    m_white     = nullptr;
        nri::Descriptor* m_whiteView = nullptr;
        nri::Descriptor* m_sampler   = nullptr;

        nri::DescriptorPool* m_pool = nullptr;
        nri::DescriptorSet*  m_set  = nullptr;

        std::uint32_t m_layoutId = NriPipelineCache::kInvalidLayout;

        // MEMBERS, not locals, and that is load-bearing:
        // nri::GraphicsPipelineDesc::vertexInput is a POINTER into caller
        // memory that CreateGraphicsPipeline dereferences AFTER the fill
        // callback has returned (NriPipelineCache.hpp, fill-contract rule 2).
        nri::VertexAttributeDesc m_attributes[3]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        // ---- registered materials ---------------------------------------
        // The constant-buffer arena: ONE HOST_UPLOAD buffer, persistently
        // mapped, carved into (kMaxMaterialSlots + 1) regions per frame slot.
        // See THE CONSTANT-BUFFER ARENA in the file header for why this is not
        // the Task 5 upload ring.
        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;   // AlignUp(kMaterialCbMaxBytes, constantBufferOffset)
        nri::Descriptor* m_globalsView[kSwapchainFramesInFlight]{};

        // Slot per REGISTERED material id, assigned in first-use order (ids are
        // dense from 3 today, but nothing in Batcher2D promises that).
        std::vector<MaterialSlot>                        m_materials;
        std::unordered_map<std::uint16_t, std::uint32_t> m_materialSlotOf;
        // Materials this node has already refused, so the refusal is reported
        // once rather than every frame.
        std::unordered_set<std::uint16_t>                m_materialRefused;
        // The BUILT-IN per-texture sets, keyed by asset Guid. `m_set` (above)
        // is the nil-Guid one; these bind a real image at t0 instead. Written
        // once each, never rewritten -- see the file header.
        std::unordered_map<Guid, nri::DescriptorSet*>    m_spriteSets;

        // One WARN, not one per span per frame, for the degradations a reader
        // must be able to see: a material this node could not build (drawn
        // through the plain sprite pipeline) and a run that spent its
        // per-texture set budget (the rest draw with the white texel). The
        // "image not resident" warn moved to NriTextureCache, which is where
        // the miss now happens.
        bool m_warnedRegisteredMaterial = false;
        bool m_warnedTextureBudget      = false;
    };

    // Declares the 2D batch node into `graph` and hands back the RGBA16F canvas
    // transient it renders into, for the nodes downstream (tonemap today; the
    // post chain at Task 10).
    //
    // Signature note: the plan sketches this as
    // `AddBatch2DNode(RenderGraph&, NriGraphContext&, RgTexture canvas)`. The
    // canvas cannot be an INPUT: a transient can only be minted from a
    // RenderGraphBuilder, and a builder exists only inside a node's setup fn --
    // so the first node to touch the canvas is necessarily the one that creates
    // it. Hence extent in, handle out. `context` is a POINTER because the
    // headless [nri] frame-shape test drives the real declarations with no
    // device: with a null context every declaration is identical and the exec
    // fn does nothing, which is exactly what makes that test able to fail when
    // this function's DECLARATIONS change.
    ARCANE_API RgTexture AddBatch2DNode(RenderGraph& graph, NriGraphContext* context,
                                        std::uint32_t width, std::uint32_t height);
}
