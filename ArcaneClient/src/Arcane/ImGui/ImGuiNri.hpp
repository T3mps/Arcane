#pragma once

// ImGuiNri -- the first-party Dear ImGui renderer backend on RAW NRI.
//
// FIRST-PARTY RATHER THAN NRI'S OWN, per the capability contract's §7
// ruling (docs/plans/2026-08-12-nri-capability-contract.md, "PORT OURS"): NRI
// ships an NRIImgui extension, but it hard-requires the NRI Streamer, which
// the adoption spec has already deferred -- and its ImTextureID protocol keys
// off an nri::Descriptor* where ours keys off the raw texture handle
// (§7.3), a convention nine live editor call sites depend on. Shaders are
// data/shaders/imgui.hlsl; the protocol is 1.92's
// ImGuiBackendFlags_RendererHasTextures; the atlas is display-referred
// (non-sRGB); scissor is per-draw.
//
// ================= THE ImTextureID CONVENTION (§7.3) =================
// An ImTextureID IS THE BACKEND'S RAW TEXTURE HANDLE, cast through intptr_t.
// Here that handle is `nri::Texture*`. The SHAPE is what §7.3 pins,
// not the type -- "a raw resource handle, not a
// descriptor" -- so a host that hands ImGui a texture it owns keeps writing
// `ImGui::Image((ImTextureID)(uintptr_t)tex, ...)` and the backend is what
// maps it onto its own binding. That is why the descriptor SET is looked up
// per texture pointer below rather than being handed in by the caller.
//
// ONE HAZARD, stated because it is reachable: NRI DOES NOT REF-COUNT, so a
// cached binding set does not pin its texture and a live `nri::Texture*` key
// is not guaranteed unique over time. For textures WE own (every
// ImTextureData) that is closed
// structurally -- DestroyTexture evicts the cache entry before it buries the
// texture. For a USER texture (case b of the convention: a raw handle the
// host passed straight to ImGui::Image) the key is a bare pointer this class
// cannot pin, so a host that destroys such a texture and lets NRI hand its
// address to a replacement would get a cache HIT on a descriptor naming the
// old resource.
//
// THE CLOSURE IS THE InvalidateUserTexture PAIR below -- an explicit hook,
// and deliberately not a heuristic: there is no observable a heuristic could
// key on, because the
// replacement's pointer may compare EQUAL to the destroyed one's.
//
// THE CALLER CONTRACT, IN FULL, AND IT IS THE PAIR'S DECLARATIONS THAT GOVERN
// (read them before writing a call -- the two variants are NOT
// interchangeable):
//   * WHO owes it: whoever destroys a texture it has handed to ImGui::Image.
//   * WHEN: **BEFORE the texture is destroyed**, not merely before the next
//     RenderDrawData. Those are the same instant only when the caller owns the
//     texture AND buries it in the very lane it passes to the deferred variant;
//     any other arrangement lets DestroyDescriptor run after DestroyTexture,
//     because nothing orders two graveyards.
//   * WHICH VARIANT: InvalidateUserTextureNow whenever the texture belongs to
//     ANOTHER context -- it destroys the view inside the call, so running it
//     first makes the ordering unconditional. The deferred
//     InvalidateUserTexture only for a texture dying in the same lane after it.
//   * UNCONDITIONALLY, regardless of whether the replacement's address changed
//     -- an unchanged address is precisely the case a recycled one fakes.
// THE CALLERS, and note there are TWO KINDS -- "destroys" above means every
// way a texture dies, not only the one that gets talked about:
//   * A RESIZE. The editor's viewport resize is the first real caller and the
//     cross-context case, so it owes the `Now` variant BEFORE its resize:
//     NriGraphContext::ResizeOffscreen carries the exact sequence, the reason
//     the order is load-bearing, and the adjacency rule that goes with it.
//   * A DESTRUCTION -- the owner going away, which for a host means PROCESS
//     TEARDOWN and a project switch. Easy to miss because it looks
//     like something member ordering should handle, and it is NOT: when the two
//     contexts share a device, the borrower must be destroyed FIRST (for the
//     device) while its texture's VIEW, held by the other context's backend,
//     must be destroyed first too (for this rule) -- opposite orders, so no
//     declaration order satisfies both. The owner's last living moment is the
//     only place it can be closed, explicitly. EditorApp::Shutdown is that
//     call for the editor's viewport output, and it is
//     unconditional and idempotent: a miss is routine and a null is an
//     early-out, so a host that is not sure whether anything ever drew the
//     texture should simply make the call.
//     A PROJECT SWITCH IS THE SAME CALLER KIND, and the one that
//     shows why this is a KIND rather than a synonym for "shutdown":
//     EditorApp::TeardownGraphForSwitch destroys the viewport context AND
//     every closed document's preview context mid-session, while the backend
//     caching their outputs -- the chrome context's -- goes on rendering
//     afterwards. So the eviction is not tidiness before exit; skipping it
//     leaves a live cache entry that the very next frame can hit on a
//     recycled address.
//
// ================= WHAT IT OWNS =================
//   * one LINEAR/clamp sampler (ImGui's default filtering);
//   * one pipeline layout registered in the shared NriPipelineCache:
//     ROOT CONSTANTS b0 (16 bytes, VERTEX) + one space-0 descriptor set
//     { t0 texture, s0 sampler } -- see THE ROOT-CONSTANT FINDING below;
//   * a GROWING CHAIN of descriptor pools (PoolCapacityFor sizes each link),
//     handing out ONE SET PER TEXTURE (never per frame slot: a set here is
//     written exactly once, at the moment its texture is first seen, and
//     never rewritten while the GPU might read it -- the same discipline
//     Batch2DNode's built-in set has);
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

    class ARC_API ImGuiNri
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

        // Install this backend's identity + capability flags on a NAMED ImGui
        // context (an `ImGuiContext*`, spelled void* like every other
        // context-passing seam in this tree -- ClientRuntime::SetImGui,
        // OffscreenImGuiLayer::Context). Pins it for the call and restores
        // whatever was current. Idempotent and null-safe.
        //
        // WHY IT EXISTS. Init above sets those flags on
        // whatever context is CURRENT, which is exactly right for a host whose
        // ONE ImGui context is the one it just created and left pinned -- both
        // hosts' primary context. It is WRONG for a SECOND context: the
        // editor's game-UI context is created during boot, while the vehicle
        // that renders it is built after boot with the EDITOR context current,
        // so Init would flag the editor's and leave the game's without
        // ImGuiBackendFlags_RendererHasTextures. A draw list built without that
        // flag carries no `Textures` array for NewFrameTexUpdates to walk, so
        // the font atlas never reaches the GPU and every draw samples nothing --
        // a silently blank HUD rather than an error. Call this once, with the
        // context this backend will actually be handed draw data from.
        void AdoptContext(void* imguiContext);

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
        // THE LIFT OUT OF RenderDrawData IS FORCED, not stylistic: the helper
        // submits and waits, so a texture upload cannot share the open command
        // list the draw is about to use.
        void NewFrameTexUpdates(ImDrawData* drawData, Graveyard& graveyard, std::uint64_t fence);

        // ============ THE USER-TEXTURE INVALIDATION HOOK ============
        // (The closure named under ONE HAZARD above.)
        //
        // TWO VARIANTS, AND PICKING THE WRONG ONE INVERTS AN ORDERING. Both
        // drop this backend's cached binding for `texture` and RETIRE its
        // descriptor set for age-gated recycling; they differ only in WHEN the
        // SHADER_RESOURCE view is destroyed, and therefore in what they can
        // promise about the view-before-texture rule.
        //
        // Common to both: the TEXTURE is never touched -- it is the caller's,
        // which is what makes it a USER texture. An ImTextureData-owned entry
        // is REFUSED (the 1.92 protocol owns those; DestroyTexture is their
        // path, and it must also bury the texture and stamp the status) rather
        // than half-disposed. True means an entry was evicted; FALSE IS ROUTINE
        // AND NOT AN ERROR -- a texture that was never drawn has no entry, and
        // a caller invalidating unconditionally (which it should) hits that
        // before the first frame. Neither is teardown: the next ImGui::Image
        // over the same pointer re-creates the view and takes a fresh set.
        //
        // ---- DEFERRED. Buries the view in `graveyard` at `fence`; does not
        // stall. CORRECT ONLY WHEN THE TEXTURE DIES IN THAT SAME LANE, AT THE
        // SAME OR A LATER FENCE -- i.e. when the caller owns the texture and
        // buries it itself, so one lane's burial ORDER is what keeps the view
        // ahead of the resource it views.
        //
        // DO NOT USE IT FOR A TEXTURE ANOTHER CONTEXT OWNS: there is no
        // ordering between two graveyards, and in the editor's resize case the
        // two are ordered the WRONG way -- NriGraphContext::ResizeOffscreen
        // destroys its output synchronously (it drains its own lane inside the
        // call) while a burial here waits one to two frames for the caller's
        // fence, so DestroyDescriptor would run after DestroyTexture on every
        // resize. Use InvalidateUserTextureNow for that.
        bool InvalidateUserTexture(nri::Texture* texture, Graveyard& graveyard, std::uint64_t fence);

        // ---- IMMEDIATE. Destroys the view inside this call, behind a
        // DeviceWaitIdle this function issues ITSELF -- there is no
        // precondition on the caller, because an unenforceable "you have
        // already idled" is precisely the kind of contract this seam has been
        // burned by.
        //
        // THE CROSS-CONTEXT VARIANT, and the one the editor's viewport resize
        // owes. CALL IT BEFORE THE OWNER DESTROYS THE TEXTURE and the view
        // provably dies first -- no lane, no fence, and no ordering between two
        // graveyards left to reason about. NriGraphContext::ResizeOffscreen
        // carries the exact three-line sequence.
        //
        // IT STALLS, deliberately. A resize is a rare event that already idles
        // (ResizeOffscreen does its own), so the cost is one extra
        // DeviceWaitIdle on a frame that was going to spend one anyway. Do not
        // reach for it from inside a frame -- that is what the deferred variant
        // is for.
        bool InvalidateUserTextureNow(nri::Texture* texture);

        // Creates the view + descriptor set for a USER texture up front, if it
        // has none yet -- the SAME call RenderDrawData makes on first sight of
        // an ImTextureID, hoisted out of record time. False (already reported)
        // when the pool chain could not grow or NRI refused the view.
        //
        // TWO CALLERS, both legitimate: a host that knows it is about to draw a
        // texture and would rather learn about a full pool where it can still
        // report it than inside an open command buffer; and the [nri] cases,
        // for which this is the ONLY way to reach the user-texture half of the
        // cache at all -- RenderDrawData allocates from the upload ring first,
        // and NONE's MapBuffer refuses, so an Init'd ring does not exist on the
        // one backend the device-less gate can drive.
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
        // m_pools/m_sampler are cleared here so their teardown only runs once,
        // and the platform-texture walk is harmless to repeat (a second pass
        // finds nothing left matching RefCount == 1). m_device itself is
        // NEVER cleared, so the `!m_device` early-out only guards an object
        // that was never Release()'d at all, not a repeat call. It walks the
        // PLATFORM texture list rather than only our own map -- that is what
        // catches an ImTextureData whose WantCreate arrived after the last
        // frame, so a later re-Init is clean.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // ============ THE POOL CHAIN (no concurrent-texture cap) ============
        // A pool's capacity is fixed at creation and NRI cannot free a single
        // descriptor set, so the backend used to size ONE pool up front at 32
        // sets and refuse the 33rd concurrent texture. The editor blows
        // through that on an ordinary boot -- every Content Browser and
        // Inspector thumbnail is its own user texture, on top of the font
        // atlas and the material previews -- and the refusal was not benign:
        // once the pool was full, a font atlas ImGui re-created (1.92 grows
        // its dynamic atlas by re-creating it) could not get a set, kept an
        // invalid TexID, and ImDrawCmd::GetTexID ASSERTED on the very next
        // draw that named it.
        //
        // SO THE CAPACITY GROWS INSTEAD. When the newest pool is full,
        // AcquireSet creates another, sized by PoolCapacityFor(its index):
        // kFirstPoolSets, doubling per link, clamped at kMaxPoolSets. Earlier
        // pools are never destroyed before Release -- their sets are live or
        // retired, and a retired set is RECYCLED (m_retired) before any pool
        // grows, so the chain tracks the PEAK concurrent texture count, not
        // the lifetime one.
        //
        // WHY kMaxPoolSets CLAMPS A LINK (not the total): on D3D12 every pool
        // owns its own shader-visible heaps, and a shader-visible SAMPLER heap
        // may hold at most 2048 descriptors; each set here takes one sampler
        // slot. 1024 keeps every link comfortably inside that limit while
        // still meaning a pathological count needs only a handful of links.
        //
        // WHAT A SECOND LINK COSTS AT RECORD TIME: RenderDrawData re-binds the
        // pool (CmdSetDescriptorPool -> ID3D12GraphicsCommandList::
        // SetDescriptorHeaps on D3D12; a no-op on Vulkan) whenever two
        // consecutive draws' sets live in different links. Geometric growth is
        // what keeps the link count -- and so that switching -- small.
        static constexpr std::uint32_t kFirstPoolSets = 64;
        static constexpr std::uint32_t kMaxPoolSets   = 1024;

        // The set capacity of the `poolIndex`-th link of the chain. PURE (no
        // device), which is what lets a non-[nri] case pin the growth policy.
        [[nodiscard]] static constexpr std::uint32_t PoolCapacityFor(std::size_t poolIndex) noexcept
        {
            std::uint32_t capacity = kFirstPoolSets;
            for (std::size_t i = 0; i < poolIndex && capacity < kMaxPoolSets; ++i)
                capacity *= 2u;
            return capacity < kMaxPoolSets ? capacity : kMaxPoolSets;
        }

        // Introspection for the [nri] tests and the node's logging. Not part
        // of any cross-task contract.
        [[nodiscard]] std::size_t LiveTextureCount() const noexcept { return m_textures.size(); }
        // Links in the descriptor-pool chain -- 1 after Init, more once the
        // peak concurrent texture count outgrew the earlier links.
        [[nodiscard]] std::size_t PoolCount() const noexcept { return m_pools.size(); }
        // True while a cached view + descriptor set exists for `texture` --
        // i.e. while an ImTextureID naming it would report a cache HIT. The
        // observable InvalidateUserTexture flips.
        [[nodiscard]] bool HasEntryFor(nri::Texture* texture) const noexcept;
        // Descriptor sets evicted but not yet recycled. NRI cannot free a
        // single set, so a disposed entry's set lands here for
        // FramesInFlight() recorded frames rather than being destroyed
        // -- this is what proves a disposal took the RETIREMENT path.
        [[nodiscard]] std::size_t RetiredSetCount() const noexcept { return m_retired.size(); }

    private:
        // The three lines Init and AdoptContext share, on an ALREADY-PINNED
        // context. Never call it without pinning first. Also RECORDS that
        // context as m_imguiContext.
        void InstallBackendIdentity();

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
            // The chain link `set` was allocated from -- what RenderDrawData
            // must have bound (CmdSetDescriptorPool) before binding `set`.
            nri::DescriptorPool* pool   = nullptr;
            // True when this backend created `texture` and must bury it. A
            // user texture is the host's; we only ever own the view + set.
            bool                owned   = false;
        };

        // A set handed out of the chain, with the link it came from.
        struct AcquiredSet
        {
            nri::DescriptorSet*  set  = nullptr;
            nri::DescriptorPool* pool = nullptr;
        };

        // One link of the pool chain. `allocated` is what AcquireSet
        // range-checks against `capacity` -- a set is never returned to its
        // pool (NRI has no per-set free), so it only ever counts up;
        // m_retired is the recycling half.
        struct PoolLink
        {
            nri::DescriptorPool* pool      = nullptr;
            std::uint32_t        capacity  = 0;
            std::uint32_t        allocated = 0;
        };

        // A descriptor set whose texture is gone. It cannot be freed (NRI has
        // no per-set free), so it is REWRITTEN for the next texture instead --
        // but only once the submission that last bound it has retired, which
        // is the same pacing argument every per-frame-slot resource on this
        // path rests on: frame N's record happens after frame
        // N - FramesInFlight() completed (the wait inside
        // NriSwapChain::AcquireNextTexture).
        struct RetiredSet
        {
            nri::DescriptorSet*  set       = nullptr;
            nri::DescriptorPool* pool      = nullptr;   // the link it belongs to
            std::uint64_t        retiredAt = 0;         // m_recordCount at retirement
        };

        bool CreateSampler();
        bool CreateLayout();
        // Appends one link, sized PoolCapacityFor(m_pools.size()). Init calls
        // it once (so a device that cannot make even one pool fails at boot);
        // AcquireSet calls it whenever the newest link is full.
        bool CreatePool();

        // The entry for `texture`, creating the view + descriptor set on
        // first sight. Null (already reported) when the pool chain could not
        // grow or NRI refused a view.
        [[nodiscard]] Entry* EnsureEntry(const nri::CoreInterface& core, nri::Texture* texture,
                                          ImTextureData* owner);
        // A set to write into: a recycled one when a retirement has aged out,
        // otherwise a fresh allocation from the newest link -- growing the
        // chain first when that link is full. A null set only when NRI refused
        // a new pool or the allocation itself (already reported).
        [[nodiscard]] AcquiredSet AcquireSet(const nri::CoreInterface& core);

        void UpdateTexture(ImTextureData* tex, Graveyard& graveyard, std::uint64_t fence);
        void DestroyTexture(ImTextureData* tex, Graveyard& graveyard, std::uint64_t fence);
        // Disposes `entry`'s view (and its texture when we own it) and retires
        // its descriptor set; clears the entry in place, the caller erases it
        // from m_textures. `graveyard` non-null BURIES at `fence`; NULL
        // DESTROYS IMMEDIATELY, which only InvalidateUserTextureNow asks for
        // and only behind the idle it issues. See the .cpp.
        void ReleaseEntry(Entry& entry, Graveyard* graveyard, std::uint64_t fence);
        // The shared body of both invalidation variants -- one eviction, two
        // disposal disciplines. `who` names the caller in the refusal message.
        bool EvictUserEntry(nri::Texture* texture, Graveyard* graveyard, std::uint64_t fence,
                            const char* who);

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
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;
        // The pool chain, oldest first. Only the LAST link can have room: a
        // new one is appended only once the previous is full.
        std::vector<PoolLink> m_pools;

        std::vector<Entry>      m_textures;
        std::vector<RetiredSet> m_retired;
        // RenderDrawData calls, i.e. recorded frames. The clock the
        // retirement gate above is measured in.
        std::uint64_t           m_recordCount = 0;

        // WHICH ImGui context this backend serves -- an `ImGuiContext*` held as
        // void* so this header stays imgui-free (only Release compares it, and
        // only after a cast in the .cpp). Recorded by InstallBackendIdentity,
        // i.e. at Init or at AdoptContext, whichever named the context.
        //
        // WHY IT IS WORTH A MEMBER. Release walks
        // ImGui::GetPlatformIO().Textures to catch ImTextureData this backend
        // never serviced, and that walk read WHATEVER CONTEXT WAS CURRENT. With
        // one backend per process that was right by luck. With TWO -- the
        // editor's chrome backend over the editor context and its game backend
        // over the plugin context, released back to back from teardowns that
        // pin neither -- one of them necessarily walks the OTHER's LIVE atlas.
        //
        // AND THE DAMAGE IS NOT "it reads as Destroyed", which is worth
        // spelling out because it decides what an observer must look at:
        // ImTextureData::SetStatus (imgui.h) BOUNCES a Destroyed request back
        // to WantCreate whenever the CPU-side Pixels are still live, which for
        // a live atlas they are. SetTexID carries no such guard. So the victim
        // is left WANTCREATE WITH AN INVALID TexID -- ImGui asking the OWNING
        // backend to create a SECOND texture for an ImTextureData it still
        // holds a live cache Entry for, with the id its draw commands carried
        // gone. At process exit that is invisible; across a PROJECT SWITCH,
        // where the ImGui contexts OUTLIVE the graph contexts, it is live.
        // Pinned by the "Release stamps the ADOPTED context's atlas"
        // [nri] case, which asserts on TexID for exactly that reason.
        //
        // THE INVARIANT THIS RESTS ON is ONE ImGuiNri PER ImGui CONTEXT, and it
        // is enforced structurally rather than assumed -- see
        // NriGraphContext::NodeSet, whose per-node gating is what stops a
        // context that will never draw a HUD from building a second backend
        // over somebody else's context.
        //
        // THE CALLER'S OBLIGATION, because a pin is a dereference: the ImGui
        // context a backend adopted must OUTLIVE that backend's Release. In the
        // editor that is declaration order (m_gameImgui is declared before
        // m_viewportTargets, so it destructs after it -- stated at that member
        // too); a PROJECT SWITCH states the same ordering explicitly instead,
        // because it destroys a backend while every ImGui context in the
        // process survives -- EditorApp::TeardownGraphForSwitch, which keeps
        // m_gameImgui untouched across the release and re-adopts
        // it on the rebuilt node.
        void* m_imguiContext = nullptr;

        bool m_warnedPoolFull     = false;   // the chain could not grow
        bool m_warnedUnregistered = false;   // a draw named an ImTextureData with no TexID
        bool m_warnedFormat       = false;
        bool m_warnedRing         = false;
    };
}
