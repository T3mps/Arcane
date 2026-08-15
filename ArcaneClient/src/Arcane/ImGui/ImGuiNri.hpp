#pragma once

// ImGuiNri -- the first-party Dear ImGui renderer backend on RAW NRI.
//
// THE PORT OF ImGuiNvrhi, decided by the capability contract's §7 ruling
// (docs/plans/2026-08-12-nri-capability-contract.md, "PORT OURS"): NRI ships
// an NRIImgui extension, but it hard-requires the NRI Streamer, which the
// adoption spec has already deferred -- and its ImTextureID protocol keys off
// an nri::Descriptor* where ours keys off the raw texture handle (§7.3), a
// convention nine live editor call sites depend on. So this file is the
// NVRHI backend's 365-line surface re-expressed in NRI calls, and NOTHING
// ELSE changes: same shaders (data/shaders/imgui.hlsl), same 1.92
// ImGuiBackendFlags_RendererHasTextures protocol, same blend, same
// display-referred (non-sRGB) atlas, same per-draw scissor, same
// ImTextureID convention.
//
// ================= THE ImTextureID CONVENTION (§7.3) =================
// An ImTextureID IS THE BACKEND'S RAW TEXTURE HANDLE, cast through intptr_t.
// On NVRHI that is `nvrhi::ITexture*`; here it is `nri::Texture*`. The SHAPE
// is what §7.3 pins, not the type -- "a raw resource handle, not a
// descriptor" -- so a host that hands ImGui a texture it owns keeps writing
// `ImGui::Image((ImTextureID)(uintptr_t)tex, ...)` and the backend is what
// maps it onto its own binding. That is why the descriptor SET is looked up
// per texture pointer below rather than being handed in by the caller.
//
// ONE HAZARD THE NVRHI BACKEND DOES NOT HAVE, stated because Phase 3 is what
// makes it reachable: NVRHI ref-counts, so a cached binding set PINNED its
// texture and a live `ITexture*` key was therefore unique. NRI does not
// ref-count. For textures WE own (every ImTextureData) that is closed
// structurally -- DestroyTexture evicts the cache entry before it buries the
// texture. For a USER texture (case b of the convention: a raw handle the
// host passed straight to ImGui::Image) the key is a bare pointer this class
// cannot pin, so a host that destroys such a texture and lets NRI hand its
// address to a replacement would get a cache HIT on a descriptor naming the
// old resource.
//
// THE CLOSURE IS InvalidateUserTexture (NRI Phase 3, Task 8-pre) -- the
// explicit hook this header used to owe, and deliberately not a heuristic:
// there is no observable a heuristic could key on, because the replacement's
// pointer may compare EQUAL to the destroyed one's. THE CALLER CONTRACT IS
// UNCONDITIONAL: whoever destroys a texture it has handed to ImGui::Image must
// call the hook with that pointer, BEFORE the next frame's RenderDrawData and
// regardless of whether the replacement's address changed. The first real
// caller is the editor's viewport resize
// (NriGraphContext::ResizeOffscreen -- see that declaration, which carries the
// exact call).
//
// A SECOND, PHASE-2-ONLY HAZARD FROM THE SAME CONVENTION, stated because it is
// a crash rather than a wrong pixel: while `--nri-graph` holds TWO devices
// (NriGraphContext.hpp, THE TWO-DEVICE WINDOW), an ImTextureID is only
// meaningful for ONE of them, and a raw pointer carries no evidence of which.
// A game module that calls ImGui::Image with an NVRHI handle on that path
// would have it dereferenced here as an nri::Texture*. The backend cannot
// detect it and deliberately does not try -- refusing unknown ids would break
// the legitimate user-texture case at Phase 3, where there is one device and
// the convention is sound again. It is a HOST invariant for the duration of
// the two-device window, and the same root cause as Batch2DNode.hpp's THE
// TEXTURE GAP. Nothing in the tree violates it: ReferenceGame's
// GamePlugin_DrawUI is empty and the vehicle's own HUD draws text only.
//
// ================= WHAT IT OWNS =================
//   * one LINEAR/clamp sampler (ImGui's default filtering);
//   * one pipeline layout registered in the shared NriPipelineCache:
//     ROOT CONSTANTS b0 (16 bytes, VERTEX) + one space-0 descriptor set
//     { t0 texture, s0 sampler } -- see THE ROOT-CONSTANT FINDING below;
//   * one descriptor pool holding kMaxTextures sets, ONE PER TEXTURE
//     (never per frame slot: a set here is written exactly once, at the
//     moment its texture is first seen, and never rewritten while the GPU
//     might read it -- the same discipline Batch2DNode's built-in set has);
//   * every ImTextureData-owned nri::Texture + its SHADER_RESOURCE view.
// The PSO comes from the shared cache, keyed on the target's format.
//
// ================= THE ROOT-CONSTANT FINDING =================
// data/shaders/imgui.hlsl declares its b0 block under
// `#if SPIRV [[vk::push_constant]]`, exactly like sprite.hlsl and
// entity_id.hlsl -- so an NRI ROOT CONSTANT (which lowers to a VK
// push-constant block) is what the shader actually reads on both backends.
// That is the opposite of outline_seed/jfa/composite.hlsl, which declare a
// plain `register(b0)` with no push-constant variant and therefore need a
// descriptor-set CB out of a per-frame-slot arena (PickOutlineNodes.hpp
// states that at length). Read the HLSL before assuming either.
//
// ================= WHERE THE BYTES COME FROM =================
// The per-frame VERTEX/INDEX streams go through the Task 5 upload ring at
// RECORD time (the ring's BeginFrame runs after the frame is declared, so an
// allocation made any earlier lands in the previous frame's slot). The FONT
// ATLAS cannot: it is a texture, and the ring carries CONSTANT|VERTEX|INDEX
// buffers only. It goes through nri::HelperInterface::UploadData at
// DECLARATION time, exactly like Batch2DNode's white texel and a registered
// material's textures -- UploadData submits and waits internally, which is
// why it must never be reached from inside the frame's open command buffer.
//
// NO BARRIER IS RECORDED ANYWHERE IN THIS FILE. The graph's executor derives
// every transition on the render path; the helper owns the ones its own
// one-shot upload needs.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h>, via spdlog, #defines ERROR).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>   // RenderGraphNodeContext, RgTexture

#include <cstdint>
#include <span>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace Arcane
{
    class Graveyard;
    class NriDevice;

    class ARCANE_API ImGuiNri
    {
    public:
        ImGuiNri() = default;

        // SAFETY NET, NOT THE PATH -- the same contract every Nri/ owner
        // carries. Release(graveyard, fence) is how this is torn down; a
        // destructor that still finds live objects WARNs and destroys them
        // directly behind a DeviceWaitIdle.
        ~ImGuiNri();

        ImGuiNri(const ImGuiNri&)            = delete;
        ImGuiNri& operator=(const ImGuiNri&) = delete;

        // Sets the renderer backend flags/name on the CURRENT ImGui context
        // and creates the sampler, the pipeline layout and the descriptor
        // pool. `vs`/`ps` are the imgui_vs/imgui_ps offline artifacts and
        // MUST outlive this object (NriPipelineCache's fill contract, rule 2
        // -- CreateGraphicsPipeline dereferences the blob after `fill`
        // returns). False, already logged + latched, on any failure.
        bool Init(NriDevice& device, NriPipelineCache& pipelines,
                  std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps);

        // The 1.92 texture protocol -- creates, updates and destroys the
        // ImTextureData-owned textures `drawData` names. MUST be called at
        // DECLARATION time, never from an exec fn: a create/update goes
        // through nri::HelperInterface::UploadData, which submits and waits
        // on the graphics queue. `graveyard`/`fence` are where and at what
        // value destroyed objects are buried -- THE OWNING CONTEXT'S LANE and
        // its graph's own last submitted value (NriGraphContext::Graves(),
        // Graph().DebugSubmitCount()).
        //
        // THE LANE IS A PARAMETER, not `device.Graves()`, for the reason
        // NriGraphContext.hpp's TWO CONTEXTS, TWO LANES block states in full:
        // one device may carry several contexts, each with its own submission
        // timeline, and a fence value means nothing outside the one it came
        // from. Every disposal this class makes takes it the same way, so a
        // backend's objects can never be split across two graveyards -- and
        // ordering across two graveyards is undefined, while ordering WITHIN
        // one is the contract (a view must die before the texture it views).
        //
        // This is ImGuiNvrhiRenderer's inline texture loop lifted out of
        // RenderDrawData, and the lift is forced rather than stylistic: the
        // NVRHI version could write a texture into the SAME open command list
        // it was about to draw with, and NRI's helper cannot.
        void NewFrameTexUpdates(ImDrawData* drawData, Graveyard& graveyard, std::uint64_t fence);

        // ============ THE USER-TEXTURE INVALIDATION HOOK ============
        // (NRI Phase 3, Task 8-pre -- the closure named under ONE HAZARD above.)
        //
        // Drops this backend's cached binding for `texture`: buries its
        // SHADER_RESOURCE view in `graveyard` at `fence` and RETIRES its
        // descriptor set for age-gated recycling. Byte for byte the disposal
        // DestroyTexture applies to an ImTextureData-owned entry, and it must
        // be -- an in-flight frame may still be reading that set, so neither
        // may be destroyed here and now.
        //
        // The TEXTURE itself is never touched: it is the caller's, which is
        // what makes it a USER texture. Calling this on an ImTextureData-owned
        // entry is refused (the 1.92 protocol owns those; DestroyTexture is
        // their path) and reported, rather than half-disposing a texture this
        // backend must also bury.
        //
        // Returns true when an entry was evicted. FALSE IS ROUTINE AND NOT AN
        // ERROR: a texture that was never drawn has no entry, and a caller that
        // invalidates unconditionally (which it should -- see the contract
        // above) will hit that on the first resize before the first frame.
        //
        // The next ImGui::Image over the same pointer re-creates the view and
        // takes a fresh set, so this is invalidation, not teardown.
        bool InvalidateUserTexture(nri::Texture* texture, Graveyard& graveyard, std::uint64_t fence);

        // Creates the view + descriptor set for a USER texture up front, if it
        // has none yet -- the SAME call RenderDrawData makes on first sight of
        // an ImTextureID, hoisted out of record time. False (already reported)
        // when the pool is exhausted or NRI refused the view.
        //
        // TWO CALLERS, both legitimate: a host that knows it is about to draw a
        // texture and would rather learn about a full pool where it can still
        // report it than inside an open command buffer; and the [nri] cases,
        // for which this is the ONLY way to reach the user-texture half of the
        // cache at all -- RenderDrawData allocates from the upload ring first,
        // and NONE's MapBuffer refuses, so an Init'd ring does not exist on the
        // one backend the headless gate can drive.
        bool EnsureUserTexture(nri::Texture* texture);

        // Records `drawData` into an ALREADY-OPEN raster pass whose single
        // colour attachment is `target`. Emits no barrier and begins no
        // rendering: both are the executor's. Re-sets the SCISSOR per draw
        // command (CmdSetScissors is not a barrier, and every pass
        // re-establishes its own state, so nothing is restored afterwards);
        // the viewport the executor set to the full attachment is kept.
        void RenderDrawData(ImDrawData* drawData, RenderGraphNodeContext& context,
                            RgTexture target);

        // Buries every NRI object this backend owns at `fence` and empties
        // it. Safe to call more than once, though not free the second time:
        // m_pool/m_sampler are cleared here so their teardown only runs once,
        // and the platform-texture walk is harmless to repeat (a second pass
        // finds nothing left matching RefCount == 1). m_device itself is
        // NEVER cleared, so the `!m_device` early-out only guards an object
        // that was never Release()'d at all, not a repeat call. Mirrors
        // ImGuiNvrhiRenderer::Shutdown, including its
        // walk of the PLATFORM texture list rather than only our own map --
        // that is what catches an ImTextureData whose WantCreate arrived
        // after the last frame, so a later re-Init is clean.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // How many distinct textures one context may have in flight here. A
        // pool's capacity is fixed at creation and NRI cannot free a single
        // descriptor set, so this is decided up front rather than discovered
        // mid-frame -- the same reasoning as Batch2DNode::kMaxMaterialSlots.
        // Sets from destroyed textures ARE recycled (see m_retired), so this
        // caps CONCURRENT textures, not lifetime ones.
        static constexpr std::uint32_t kMaxTextures = 32;

        // Introspection for the [nri] tests and the node's logging. Not part
        // of any cross-task contract.
        [[nodiscard]] std::size_t LiveTextureCount() const noexcept { return m_textures.size(); }
        // True while a cached view + descriptor set exists for `texture` --
        // i.e. while an ImTextureID naming it would report a cache HIT. The
        // observable InvalidateUserTexture flips.
        [[nodiscard]] bool HasEntryFor(nri::Texture* texture) const noexcept;
        // Descriptor sets evicted but not yet recycled. NRI cannot free a
        // single set, so a disposed entry's set lands here for
        // kSwapchainFramesInFlight recorded frames rather than being destroyed
        // -- this is what proves a disposal took the RETIREMENT path.
        [[nodiscard]] std::size_t RetiredSetCount() const noexcept { return m_retired.size(); }

    private:
        // One texture this backend can draw from: the resource, its
        // SHADER_RESOURCE view, and the descriptor set that binds the pair
        // alongside the shared sampler. `owner` is the ImTextureData that
        // asked for it, or null for a USER texture (a raw handle the host
        // passed to ImGui::Image -- see the ImTextureID convention above).
        struct Entry
        {
            ImTextureData*      owner   = nullptr;
            nri::Texture*       texture = nullptr;
            nri::Descriptor*    view    = nullptr;
            nri::DescriptorSet* set     = nullptr;
            // True when this backend created `texture` and must bury it. A
            // user texture is the host's; we only ever own the view + set.
            bool                owned   = false;
        };

        // A descriptor set whose texture is gone. It cannot be freed (NRI has
        // no per-set free), so it is REWRITTEN for the next texture instead --
        // but only once the submission that last bound it has retired, which
        // is the same pacing argument every per-frame-slot resource on this
        // path rests on: frame N's record happens after frame
        // N - kSwapchainFramesInFlight completed (the wait inside
        // NriSwapChain::AcquireNextTexture).
        struct RetiredSet
        {
            nri::DescriptorSet* set       = nullptr;
            std::uint64_t       retiredAt = 0;   // m_recordCount at retirement
        };

        bool CreateSampler();
        bool CreateLayout();
        bool CreatePool();

        // The entry for `texture`, creating the view + descriptor set on
        // first sight. Null (already reported) when the pool is exhausted or
        // NRI refused a view.
        [[nodiscard]] Entry* EnsureEntry(const nri::CoreInterface& core, nri::Texture* texture,
                                          ImTextureData* owner);
        // A set to write into: a recycled one when a retirement has aged out,
        // otherwise a fresh allocation. Null when the pool is exhausted.
        [[nodiscard]] nri::DescriptorSet* AcquireSet(const nri::CoreInterface& core);

        void UpdateTexture(ImTextureData* tex, Graveyard& graveyard, std::uint64_t fence);
        void DestroyTexture(ImTextureData* tex, Graveyard& graveyard, std::uint64_t fence);
        // Buries `entry`'s view (and its texture when we own it) in
        // `graveyard` and retires its descriptor set. Clears the entry in
        // place; the caller erases it from m_textures.
        void ReleaseEntry(Entry& entry, Graveyard& graveyard, std::uint64_t fence);

        // One shared cache, so every node's opaque shader-pair id space must
        // stay disjoint: Batch2DNode 0x2000+, TonemapNode 0x3000, the outline
        // passes 0x4000-0x4002, PickNode 0x4100. ImGui takes 0x5000.
        static constexpr std::uint64_t kShaderPairId = 0x5000;

        NriDevice*        m_device    = nullptr;
        NriPipelineCache* m_pipelines = nullptr;

        // Owned by the vehicle's bin cache; outlives this object, which the
        // pipeline cache's fill contract (rule 2) requires.
        std::span<const std::uint8_t> m_vs;
        std::span<const std::uint8_t> m_ps;

        // MEMBERS, not locals: GraphicsPipelineDesc::vertexInput is a pointer
        // the cache dereferences after the fill callback returns (fill
        // contract, rule 2).
        nri::VertexAttributeDesc m_attributes[3]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        nri::Descriptor*     m_sampler = nullptr;
        nri::DescriptorPool* m_pool    = nullptr;
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;
        // Sets ever handed out of the pool. The pool's capacity is
        // kMaxTextures and a set is never returned to it, so this is what
        // AcquireSet range-checks -- m_retired is the recycling half.
        std::uint32_t        m_setsAllocated = 0;

        std::vector<Entry>      m_textures;
        std::vector<RetiredSet> m_retired;
        // RenderDrawData calls, i.e. recorded frames. The clock the
        // retirement gate above is measured in.
        std::uint64_t           m_recordCount = 0;

        bool m_warnedPoolFull = false;
        bool m_warnedFormat   = false;
        bool m_warnedRing     = false;
    };
}
