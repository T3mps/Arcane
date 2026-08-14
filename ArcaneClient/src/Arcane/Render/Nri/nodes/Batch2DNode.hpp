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
//   * ONE pipeline layout, registered in the vehicle's NriPipelineCache:
//     root constants b0 (the 16-byte BatchConstants from
//     data/shaders/sprite.hlsl) plus descriptor set space0 = { t0 texture,
//     s0 sampler };
//   * one descriptor pool holding ONE descriptor set. The set is written once
//     and never rewritten -- Task 8 binds the white texel for every draw (see
//     THE TEXTURE GAP below), so there is nothing per-frame about it.
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
// THE TEXTURE GAP (Task 8 scope, stated so it is a known gap and not a
// discovery at the desk): a Batch2DDrawSpan names an `nvrhi::ITexture*` --
// a texture on the ENGINE's NVRHI device, which this node's NRI device cannot
// sample. Task 8 therefore binds the white texel for EVERY span, so textured
// sprites render as their vertex tint alone. Geometry, ordering, blending,
// the three built-in pipelines and the whole graph plumbing are real; sprite
// TEXTURES are not, until a task ports them onto the NRI device. A batch-stage
// golden compare will differ on exactly those pixels until then.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h> #defines ERROR via wingdi.h).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;
    struct Batch2DDrained;

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
        void Record(RenderGraphNodeContext& context, const Batch2DDrained& batch,
                    nri::Format canvasFormat);

    private:
        Batch2DNode() = default;

        bool Init(NriGraphContext& context);
        bool CreateWhiteTexel();
        bool CreateBindings();

        // The pipeline for one drained span's material, from the shared cache.
        // Null (already logged) if the cache refused it.
        [[nodiscard]] nri::Pipeline* PipelineFor(std::uint16_t material, nri::Format canvasFormat);

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

        // One WARN, not one per span per frame, when a registered material id
        // arrives before Task 9 can build its pipeline.
        bool m_warnedRegisteredMaterial = false;
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
