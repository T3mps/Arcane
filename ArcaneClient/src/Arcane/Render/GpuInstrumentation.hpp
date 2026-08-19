#pragma once

// GPU crash diagnostics arc (Task 7): the RENDER-SIDE instrumentation both
// hosts drive. TWO pieces now (three until NRI Phase 5a, Task 9.5a), one
// header because they share exactly one job -- making a GPU hang legible
// after the fact:
//
//   1. GpuPassScope    -- an RAII pass marker. On entry it opens a
//      GpuBreadcrumbs scope and writes the backend's GPU begin marker; on exit
//      the mirror image.
//
//      F-2c-bis, RESTATED FOR THE ONE-CHANNEL WORLD (NRI Phase 5a, Task 8b).
//      This scope used to write TWO channels: the first-party marker buffer
//      (F-1/F-5), which is what a crash report replays, and
//      nvrhi::ICommandList::beginMarker/endMarker, which is what D3D12 DRED's
//      MARKERS-ONLY auto-breadcrumb tier records. F-2c-bis was the rule tying
//      them together: markers-only DRED with no nvrhi markers yields an EMPTY
//      breadcrumb list -- strictly worse than no DRED at all -- so a scope
//      emitting only one of the two would silently invalidate the Dist tier.
//
//      THE NVRHI CHANNEL IS GONE. Task 8b deleted it here because the layer
//      that consumed it no longer exists: there is no nvrhi device, no nvrhi
//      command list, and nothing left to call beginMarker on. F-2c-bis is not
//      violated by that, because Task 1 had already SUSPENDED the tier that
//      depended on it -- Dist now selects full auto-breadcrumbs rather than
//      markers-only (GpuCrashD3D12.cpp), and DiagnosticsTest pins that no
//      config selects markers-only while there is no marker producer.
//
//      WHAT RESTORES BOTH: the native NRI marker layer. Today
//      IGpuCrashBackend::WriteMarkerNative is a stub on the graph backend
//      (NriDiagnostics.cpp returns false, deliberately, rather than claiming a
//      marker went out). When that lands, this scope's WriteMarkerNative call
//      starts producing real GPU markers with no edit at this line, and the
//      markers-only DRED tier becomes re-earnable -- in that order, and only
//      in that order.
//
//   2. GpuDrawScope    -- the finer, draw-granular marker. nvrhi markers ONLY,
//      opt-in via the `diagnostics.drawMarkers` EngineConfig key, and compiled
//      out of Dist entirely (ARC_GPU_DRAW_SCOPE). It writes NO breadcrumb ring
//      entry on purpose: the ring holds GpuBreadcrumbs::kRingCapacity scopes
//      total, and per-draw entries would evict the pass scopes the report is
//      actually built from -- trading the answer for the detail.
//      DEAD SINCE NRI Phase 5a: it is the LAST nvrhi surface in this header,
//      its one call site is Batcher2D.cpp's NVRHI recorder, and nothing
//      reaches that recorder any more (there is no nvrhi device to build it
//      against). STILL STANDING, and now with an owner: severing it means
//      editing Batcher2D's GPU half, which Task 9.5b owns together with the
//      recorder and the ABI bump that removing Batcher2D's NVRHI surface
//      needs. It is the sole reason <nvrhi/nvrhi.h> is still included below.
//
//   WHAT WENT AT TASK 9.5a, so the numbering above is not read as a gap:
//   GpuFrameProgress (the GPU-side heartbeat source that fed
//   Diagnostics::GpuHeartbeat) and GpuFrameSlot (the stamped event-query
//   pacing slot) are both DELETED. Neither had a production caller: Task 6
//   deleted the GpuContext that built the only GpuFrameProgress, and Task 8b
//   deleted SwapchainD3D12/SwapchainVulkan, GpuFrameSlot's only two users.
//   NriSwapChain's own fence-completed publisher and its own frame slots are
//   the 1:1 replacements for both (NriSwapChain.hpp says so at length). The
//   named loss: DiagnosticsTest's "GpuFrameSlot never claims a stamp that did
//   not go out" case went with them, and it was the only UNIT pin on that
//   invariant -- the graph-side slots enforce the same rule but are covered
//   only by the [gpu] desk battery's resize storm.
//
// The active backend is reached through a process-wide slot rather than
// plumbed: F-8e's command-list owners are reached from different layers and
// do not all hold the same handle to plumb one through. NONE of F-8e's three
// owners survives -- OffscreenCanvas and PickBuffer went at NRI Phase 5a Task
// 4, GpuContext's own command list at Task 6 -- and the graph's recorders
// (RenderGraphExec's NodeScope, NriDiagnostics' fault dispatch) read the slot
// for exactly the original reason: they are reached from layers that cannot be
// handed a backend pointer. The slot mirrors Diagnostics::SetGpuSectionProvider
// exactly -- same owner (NriDiagnostics::Arm/Disarm since Task 8b deleted the
// NVRHI device layer that used to share it), same lifetime, same install/clear
// sites -- so there is one rule to get right, not two.
//
// THREADING: GpuPassScope/GpuDrawScope are for the thread that records the
// command list (the main/render thread in both hosts). GpuBreadcrumbs is not
// thread-safe and neither are these.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>

// GpuDrawScope ONLY -- see piece 2 above. Task 9.5b takes this line out with
// Batcher2D's recorder; nothing else in this header names an nvrhi type.
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    // -----------------------------------------------------------------
    // The process-wide active-backend slot
    // -----------------------------------------------------------------

    // Install (or replace) the backend pass scopes write into. Called by
    // NriDiagnostics::Arm beside its one Diagnostics::SetGpuSectionProvider
    // call (the NVRHI device layer was the other caller until Task 8b).
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
    // The process-wide device-lost latch
    // -----------------------------------------------------------------
    //
    // Set by ObserveDeviceRemoved (Render/DeviceCreation{D3D12,Vulkan}.cpp,
    // where Task 8b moved it with the deletion of DeviceD3D12/DeviceVulkan)
    // AFTER the gpu-crash report is written -- so "observed" always means "the
    // report exists". Hosts poll it once per frame (top of MainLoop, both
    // hosts) and shut down cleanly: there is no device-recovery path today,
    // and a host that keeps pumping frames at a dead device either spins in a
    // present-fail loop or walks into an access violation on the next
    // resource-creating path (the editor's PickBuffer was the desk repro).
    // Same slot idiom as the backend slot above: one process-wide atomic in
    // Arcane.dll, the render layer writes, hosts read.
    ARCANE_API void NoteGpuDeviceLost() noexcept;
    [[nodiscard]] ARCANE_API bool GpuDeviceLostObserved() noexcept;

    // Cleared where the once-per-removal report guard is re-armed: when a
    // NEW device comes up (project switch recreates the device). A latch
    // that outlived the dead device it described would instantly quit the
    // host the moment a healthy replacement started presenting.
    ARCANE_API void ResetGpuDeviceLost() noexcept;

    // -----------------------------------------------------------------
    // GpuPassScope -- one render pass, one marker channel
    // -----------------------------------------------------------------
    //
    // NO CALLERS TODAY, stated plainly rather than left for a reader to
    // discover: its last one was GpuFaultInjector::Fire, retired in the same
    // task that severed the nvrhi channel (the injector's live arm is
    // NriDiagnostics::FireFault, which opens its own compact scope). The
    // graph's own equivalent is RenderGraphExec.cpp's NodeScope -- annotation,
    // CPU ring, and a native marker gated on device identity -- which is the
    // richer of the two because it holds the device the compact form cannot.
    // This class stays because it is the seam a non-graph recorder would use,
    // and because deleting a public instrumentation type is Task 11's sweep,
    // not this task's.

    class ARCANE_API GpuPassScope
    {
    public:
        // `nativeCommandList` is the BACKEND'S OWN native command list --
        // ID3D12GraphicsCommandList* on D3D12, VkCommandBuffer on Vulkan,
        // exactly what IGpuCrashBackend::WriteMarkerNative documents. It was
        // an nvrhi::ICommandList* until NRI Phase 5a Task 8b; the nvrhi
        // marker channel and the type that carried it went together.
        //
        // It MUST already be open and must stay open for this object's whole
        // lifetime. Not a style preference: on D3D12 a marker written to a
        // not-open list latches the marker layer OFF for the rest of the
        // process (a transient condition read as a capability failure).
        // `name` must outlive the constructor call (a literal, in practice) --
        // it is copied into the breadcrumb ring.
        //
        // CALLERS MUST GATE ON DEVICE IDENTITY before passing a pointer here,
        // for the reason IGpuCrashBackend::NativeDevice() exists: a marker is
        // a write from a command buffer into the backend's marker buffer, and
        // both APIs require the two to belong to ONE device. This class cannot
        // check -- it holds no device -- so it does not pretend to.
        //
        // Null `nativeCommandList`, null backend, or an unarmed backend: every
        // step degrades independently and none of them throws or logs
        // per-call.
        GpuPassScope(void* nativeCommandList, const char* name) noexcept;
        ~GpuPassScope();

        GpuPassScope(const GpuPassScope&)            = delete;
        GpuPassScope& operator=(const GpuPassScope&) = delete;
        GpuPassScope(GpuPassScope&&)                 = delete;
        GpuPassScope& operator=(GpuPassScope&&)      = delete;

    private:
        void*             m_commandList = nullptr;
        IGpuCrashBackend* m_backend     = nullptr;   // latched at construction: the slot must not change mid-scope
        std::uint32_t     m_token       = 0;
        bool              m_scoped      = false;     // a breadcrumb scope is open and owes an EndScope
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
}
