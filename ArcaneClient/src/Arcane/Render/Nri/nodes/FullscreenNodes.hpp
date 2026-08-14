#pragma once

// FullscreenNodes -- the graph's fullscreen-triangle passes.
//
// TONEMAP TODAY (NRI Phase 2, Task 8). Task 10 extends this file with the
// POST CHAIN; the tonemap lives here from the start rather than being moved
// later, because the plan's locked file structure assigns "post-chain passes
// + tonemap" to this file and a stage golden is display-referred backbuffer
// pixels -- so the batch stage cannot be compared to anything without a
// tonemap. That is why Task 8 lands it: the real tonemap_vs/ps bins, no post
// chain, nothing speculative.
//
// TonemapNode is the exact NRI counterpart of Render/TonemapPass.cpp: the
// same two offline shader bins, the same POINT/clamp sampler (1:1 canvas ->
// backbuffer), the same single 3-vertex draw with no vertex buffer
// (data/shaders/tonemap.hlsl builds the triangle from SV_VertexID).
//
// WHAT IT OWNS: one sampler, one pipeline layout registered in the vehicle's
// NriPipelineCache (descriptor set space0 = { t0 texture, s0 sampler }; no
// root constants -- the shader reads none), one descriptor pool holding ONE
// descriptor set, and the SHADER_RESOURCE view over its source texture.
//
// THE SOURCE VIEW is the one piece with a lifetime worth stating. The source
// is a graph TRANSIENT (the canvas), whose physical texture the graph owns and
// destroys on exactly one path: RenderGraph::ReleaseGpuResources, which
// NriGraphContext::Resize and ~NriGraphContext call. Both call
// InvalidateSource() in the same breath, BEFORE that release, so this node's
// descriptor can never name a freed texture. The pointer check inside
// EnsureSource is the backstop, not the mechanism: if the resolved texture
// ever changes without an invalidation, that is a bug in the owner and it says
// so through the error latch rather than quietly binding a stale view.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

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

        // Buries JUST the source view (and any view a missed invalidation left
        // behind), so the next frame builds a fresh one. MUST be called before
        // the graph releases the transient pool -- see THE SOURCE VIEW above.
        // Idempotent; a no-op when no view exists.
        void InvalidateSource(Graveyard& graveyard, std::uint64_t fence);

        // Records the tonemap into an ALREADY-OPEN raster pass whose single
        // colour attachment is `target`. `source` is the linear HDR texture to
        // sample -- the canvas today, the post chain's last target at Task 10.
        //
        // The PSO's attachment format is read back from `target`'s resolved
        // texture rather than assumed: NRI resolves a swapchain's channel order
        // instead of letting anyone pin it (NriSwapChain::Format), and a
        // pipeline bakes its attachment formats at creation. Emits no barrier:
        // the executor derives them.
        void Record(RenderGraphNodeContext& context, RgTexture source, RgTexture target);

    private:
        TonemapNode() = default;

        bool Init(NriGraphContext& context);
        // Creates (or reuses) the SHADER_RESOURCE view over `texture`.
        [[nodiscard]] bool EnsureSource(const nri::CoreInterface& core, nri::Texture* texture);

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
        nri::DescriptorSet*  m_set  = nullptr;

        std::uint32_t m_layoutId = NriPipelineCache::kInvalidLayout;

        nri::Texture*    m_sourceTexture = nullptr;
        nri::Descriptor* m_sourceView    = nullptr;
        // Views a MISSED invalidation orphaned. Normally empty; kept so the
        // backstop path in EnsureSource leaks nothing, and swept by
        // InvalidateSource/Release.
        std::vector<nri::Descriptor*> m_orphanedViews;
    };

    // Declares the tonemap node into `graph`: imports the swapchain backbuffer
    // as its colour attachment, reads `source` as a shader resource, and hands
    // the backbuffer handle back for the nodes downstream (the capture node).
    // `context` may be null -- see AddBatch2DNode's signature note.
    ARCANE_API RgTexture AddTonemapNode(RenderGraph& graph, NriGraphContext* context,
                                        RgTexture source);
}
