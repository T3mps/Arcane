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
}
