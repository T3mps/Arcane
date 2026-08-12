#pragma once

// GPU crash diagnostics arc (Task 7): the RENDER-SIDE instrumentation both
// hosts drive. Three pieces, one header because they share exactly one job --
// making a GPU hang legible after the fact:
//
//   1. GpuPassScope    -- an RAII pass marker. On entry it opens a
//      GpuBreadcrumbs scope, writes the backend's GPU begin marker, and emits
//      nvrhi::ICommandList::beginMarker; on exit the mirror image. BOTH marker
//      channels, always, because they answer the same question from two
//      independent sources: the first-party marker buffer (F-1/F-5) is what a
//      crash report replays, and the nvrhi marker is what D3D12 DRED's
//      markers-only tier records. F-2c-bis is BINDING here -- markers-only DRED
//      with no nvrhi markers yields an EMPTY breadcrumb list, strictly worse
//      than no DRED at all, so a scope that emitted only one of the two would
//      silently invalidate the Dist tier.
//
//   2. GpuDrawScope    -- the finer, draw-granular marker. nvrhi markers ONLY,
//      opt-in via the `diagnostics.drawMarkers` EngineConfig key, and compiled
//      out of Dist entirely (ARC_GPU_DRAW_SCOPE). It writes NO breadcrumb ring
//      entry on purpose: the ring holds GpuBreadcrumbs::kRingCapacity scopes
//      total, and per-draw entries would evict the pass scopes the report is
//      actually built from -- trading the answer for the detail.
//
//   3. GpuFrameProgress -- the GPU-side heartbeat source. See its own comment;
//      it is what feeds Diagnostics::GpuHeartbeat.
//
// The active backend is reached through a process-wide slot rather than
// plumbed: F-8e's three command-list owners (GpuContext, OffscreenCanvas,
// PickBuffer) are reached from different layers, and two of them hold only an
// nvrhi::IDevice*. The slot mirrors Diagnostics::SetGpuSectionProvider exactly
// -- same owner (the device layer), same lifetime, same install/clear sites --
// so there is one rule to get right, not two.
//
// THREADING: GpuPassScope/GpuDrawScope are for the thread that records the
// command list (the main/render thread in both hosts). GpuBreadcrumbs is not
// thread-safe and neither are these.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    // -----------------------------------------------------------------
    // The process-wide active-backend slot
    // -----------------------------------------------------------------

    // Install (or replace) the backend pass scopes write into. Called by the
    // device layer beside its one Diagnostics::SetGpuSectionProvider call.
    ARCANE_API void SetActiveGpuCrashBackend(IGpuCrashBackend* backend) noexcept;

    // The installed backend, or null. Null is ORDINARY, not an error: a headless
    // device, a backend that failed to arm, a test.
    [[nodiscard]] ARCANE_API IGpuCrashBackend* ActiveGpuCrashBackend() noexcept;

    // Clear the slot ONLY if it still holds `backend`; returns whether it
    // cleared. Same stale-registration hazard as Diagnostics::ClearSinkIfCurrent
    // -- an unconditional clear from an old device's teardown would disconnect a
    // live, unrelated one. Prefer this in any owner's teardown path.
    [[nodiscard]] ARCANE_API bool ClearActiveGpuCrashBackendIfCurrent(IGpuCrashBackend* backend) noexcept;

    // Draw-level marker toggle (`diagnostics.drawMarkers`, default false). Read
    // per draw, so it is one relaxed atomic load; hosts set it from the layered
    // config. Pass scopes ignore it -- those are always on, in every config.
    ARCANE_API void SetGpuDrawMarkersEnabled(bool enabled) noexcept;
    [[nodiscard]] ARCANE_API bool GpuDrawMarkersEnabled() noexcept;

    // -----------------------------------------------------------------
    // GpuPassScope -- one render pass, both marker channels
    // -----------------------------------------------------------------

    class ARCANE_API GpuPassScope
    {
    public:
        // `commandList` MUST already be open and must stay open for this
        // object's whole lifetime. Not a style preference: on D3D12 a marker
        // written to a not-open list latches the marker layer OFF for the rest
        // of the process (a transient condition read as a capability failure),
        // and on Vulkan nvrhi's getNativeObject on a closed list is an access
        // violation with no null to check. `name` must outlive the constructor
        // call (a literal, in practice) -- it is copied into the breadcrumb
        // ring and forwarded to nvrhi verbatim.
        //
        // Null `commandList`, null backend, or an unarmed backend: every step
        // degrades independently and none of them throws or logs per-call.
        GpuPassScope(nvrhi::ICommandList* commandList, const char* name) noexcept;
        ~GpuPassScope();

        GpuPassScope(const GpuPassScope&)            = delete;
        GpuPassScope& operator=(const GpuPassScope&) = delete;
        GpuPassScope(GpuPassScope&&)                 = delete;
        GpuPassScope& operator=(GpuPassScope&&)      = delete;

    private:
        nvrhi::ICommandList* m_commandList = nullptr;
        IGpuCrashBackend*    m_backend     = nullptr;   // latched at construction: the slot must not change mid-scope
        std::uint32_t        m_token       = 0;
        bool                 m_marked      = false;     // an nvrhi beginMarker went out and owes an endMarker
        bool                 m_scoped      = false;     // a breadcrumb scope is open and owes an EndScope
    };

    // -----------------------------------------------------------------
    // GpuDrawScope -- draw-granular, opt-in, never in Dist
    // -----------------------------------------------------------------

#if !defined(ARCANE_DIST)
    class ARCANE_API GpuDrawScope
    {
    public:
        GpuDrawScope(nvrhi::ICommandList* commandList, const char* name) noexcept;
        ~GpuDrawScope();

        GpuDrawScope(const GpuDrawScope&)            = delete;
        GpuDrawScope& operator=(const GpuDrawScope&) = delete;

    private:
        nvrhi::ICommandList* m_commandList = nullptr;   // null when the toggle is off
    };
#endif

    // Use this rather than naming GpuDrawScope directly: in Dist the class does
    // not exist at all, so the site must vanish with it.
#if defined(ARCANE_DIST)
    #define ARC_GPU_DRAW_SCOPE(commandList, name) ((void)0)
#else
    #define ARC_GPU_DRAW_SCOPE_CAT2(a, b) a##b
    #define ARC_GPU_DRAW_SCOPE_CAT(a, b)  ARC_GPU_DRAW_SCOPE_CAT2(a, b)
    #define ARC_GPU_DRAW_SCOPE(commandList, name) \
        ::Arcane::GpuDrawScope ARC_GPU_DRAW_SCOPE_CAT(arcGpuDrawScope_, __LINE__)((commandList), (name))
#endif

    // -----------------------------------------------------------------
    // GpuFrameProgress -- the value Diagnostics::GpuHeartbeat watches
    // -----------------------------------------------------------------
    //
    // WHY THIS EXISTS AT ALL: there is no cross-backend "completed instance" to
    // read. nvrhi::IDevice::executeCommandList returns the SUBMITTED instance,
    // which advances happily while the GPU is wedged; queueGetCompletedInstance
    // is declared only on nvrhi::vulkan::IDevice (vulkan.h:45) and has no D3D12
    // sibling in nvrhi. What IS cross-backend is nvrhi's event query, which both
    // swapchains already use for frame pacing -- so this stamps its own query
    // chain and polls it NON-BLOCKING (pollEventQuery, nvrhi.h:3712). The
    // resulting count is honest by construction: it advances only when the
    // device actually signalled a fence past a point we submitted.
    //
    // Its own chain rather than the swapchain's: the swapchain's queries are
    // consumed by a BLOCKING wait for slot reuse, and a diagnostics counter must
    // never be the thing that blocks.
    class ARCANE_API GpuFrameProgress
    {
    public:
        // `device` must outlive this object. A null device makes every call a
        // no-op (headless hosts, tests).
        explicit GpuFrameProgress(nvrhi::IDevice* device);

        GpuFrameProgress(const GpuFrameProgress&)            = delete;
        GpuFrameProgress& operator=(const GpuFrameProgress&) = delete;

        // Call ONCE per host frame, AFTER the frame's last executeCommandList
        // (a stamp placed before the frame's work would retire early and report
        // progress the GPU had not made). Retires whatever finished, stamps a
        // new sync point if a slot is free, and publishes the completed count to
        // Diagnostics::GpuHeartbeat. Never blocks.
        //
        // When every slot is outstanding -- i.e. the GPU is far enough behind
        // that it has not passed ANY of the last kSlots stamps -- no new stamp
        // goes out and the counter stays put, which is exactly the state the
        // watchdog is looking for.
        void EndFrame() noexcept;

        [[nodiscard]] std::uint64_t CompletedFrames() const noexcept { return m_completed; }

    private:
        // Four: deeper than the two frames the swapchains keep in flight, so an
        // ordinary frame never finds the chain saturated, and shallow enough
        // that saturation means something is genuinely wrong.
        static constexpr std::size_t kSlots = 4;

        nvrhi::IDevice*         m_device = nullptr;
        nvrhi::EventQueryHandle m_queries[kSlots];
        std::size_t             m_next      = 0;   // next slot to stamp
        std::size_t             m_oldest    = 0;   // oldest un-retired stamp
        std::size_t             m_live      = 0;   // stamps outstanding
        std::uint64_t           m_completed = 0;   // monotone: sync points the GPU has passed
    };

    // -----------------------------------------------------------------
    // GpuFrameSlot -- one swapchain frame slot's completion fence
    // -----------------------------------------------------------------
    //
    // An nvrhi event query plus the ONE bit of state nvrhi does not expose and
    // that the pacing wait cannot be correct without: whether the query is
    // currently STAMPED.
    //
    // That bit is load-bearing because nvrhi's poll and wait DISAGREE about an
    // unstamped query, in opposite directions:
    //
    //   - Vulkan: pollEventQuery -> Queue::pollCommandList returns FALSE for
    //     commandListID == 0 (vulkan-queue.cpp:429-431) -- forever -- while
    //     waitEventQuery returns IMMEDIATELY for that same state
    //     (vulkan-queries.cpp:57-58).
    //   - D3D12: pollEventQuery returns false while !started
    //     (d3d12-queries.cpp:53-54); waitEventQuery returns immediately
    //     (d3d12-queries.cpp:75-76).
    //
    // So a poll loop over an unstamped query spins out its ENTIRE window and
    // then reports a GPU stall for a device that was never asked to do
    // anything -- where the plain blocking wait returned instantly. Not
    // hypothetical and not exotic: on Vulkan, BeginFrame resets the slot, then
    // acquireNextImageKHR throws OutOfDateKHRError -- an ordinary window resize
    // -- and the frame bails without advancing the frame counter, so Present,
    // the only stamp site, never runs. The next BeginFrame meets that same
    // slot, unstamped. D3D12 has no such path today, which makes it a latent
    // version of the identical trap rather than a safe one.
    //
    // Kept as a TYPE rather than a bool sitting beside each query so the flag
    // cannot desync from the query it describes: every transition goes through
    // this object, in both backends. A future present-skipping path then gets
    // the right behaviour for free instead of resurrecting the trap.
    class ARCANE_API GpuFrameSlot
    {
    public:
        // Creates the event query. Returns false -- and leaves a permanently
        // unstamped slot -- if the device could not make one, which degrades
        // WaitAndReset to nvrhi's immediate return rather than to a poll loop
        // with nothing to poll.
        bool Init(nvrhi::IDevice* device);

        // Present side: record this frame's completion point. Marks the slot
        // stamped ONLY if the stamp actually went out, so a slot can never
        // claim a fence it does not have.
        void Stamp(nvrhi::IDevice* device, nvrhi::CommandQueue queue);

        // BeginFrame side: wait until this slot's frame has retired, then clear
        // the query for reuse. An UNSTAMPED slot skips the polling wait
        // entirely and falls straight through to nvrhi's instant return.
        void WaitAndReset(nvrhi::IDevice* device);

        [[nodiscard]] bool IsStamped() const noexcept { return m_stamped; }

    private:
        nvrhi::EventQueryHandle m_query;
        bool                    m_stamped = false;
    };

    // -----------------------------------------------------------------
    // The pacing wait WaitAndReset performs, and what it costs
    // -----------------------------------------------------------------
    //
    // Both swapchains gate slot reuse on "frame N - kSwapchainFramesInFlight has
    // retired". That gate used to be a bare `waitEventQuery`, and it is where
    // the GPU-progress rule went to die: a wedged GPU parks the main thread
    // INSIDE the wait, so the render path stops publishing, the freshness gate
    // disarms the GPU rule, and the only report that could ever land was a plain
    // `hang`. The rule was unreachable in BOTH hosts -- two frames of headroom is
    // far under even the 2s freshness window, let alone 8s.
    //
    // Polling with the beats republished each iteration makes "the counter is
    // frozen while the render path is demonstrably alive" the observable it was
    // always supposed to be, and keeps the hang rule quiet through an ordinary
    // long wait (the main thread is not wedged -- it is waiting, on purpose).
    //
    // COST, honestly. The first poll happens before any sleep, so the common
    // case -- the slot's frame retired long ago, which is every frame that is
    // not GPU-bound -- costs one `pollEventQuery` and nothing else. Only a frame
    // that ACTUALLY had to wait pays, and it pays at most one sleep quantum
    // (~1ms) of extra latency, on a frame where the CPU was already idle waiting
    // for the GPU. The sleep is SDL_DelayNS and NOT std::this_thread::sleep_for,
    // deliberately: MSVC's sleep_for lowers to Sleep(), whose granularity is the
    // process timer resolution -- ~15.6ms by default on Windows, which would
    // have turned a 1ms intent into most of a 60Hz frame. SDL3's delay uses a
    // high-resolution waitable timer, so the quantum stays ~1ms without raising
    // the global timer resolution for the whole process.
    //
    // After a bounded window it falls back to the blocking wait: by then any
    // report has long been written, there is no point waking every millisecond
    // forever, and a permanently wedged GPU then stops beating again -- so the
    // hang rule still eventually captures the parked stack. Completion semantics
    // are identical either way: this NEVER returns before `query` has completed.
    //
    // Shared by both backends rather than copied into each: a pacing wait that
    // drifted between D3D12 and Vulkan is exactly the kind of host divergence
    // this codebase has been bitten by before. It is NOT exposed as a free
    // function -- calling it requires knowing the stamped state above, and a
    // second entry point taking that as a parameter would just be a second way
    // to get it wrong. GpuFrameSlot is the only door.
}
