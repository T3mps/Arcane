#pragma once

// GPU crash diagnostics: the RENDER-SIDE instrumentation both hosts drive.
// ONE piece:
//
//   1. GpuPassScope    -- an RAII pass marker. On entry it opens a
//      GpuBreadcrumbs scope and writes the backend's GPU begin marker; on exit
//      the mirror image.
//
//      F-2c-bis, IN A ONE-CHANNEL WORLD. This scope writes ONE channel: the
//      first-party marker buffer (F-1/F-5), which is what a crash report
//      replays. F-2c-bis is the rule tying that channel to D3D12 DRED's
//      MARKERS-ONLY auto-breadcrumb tier -- markers-only DRED with no marker
//      producer yields an EMPTY breadcrumb list, strictly worse than no DRED
//      at all. It is not violated today, because the tier that depended on it
//      is SUSPENDED: Dist selects full auto-breadcrumbs (GpuCrashD3D12.cpp),
//      and DiagnosticsTest pins that no config selects markers-only while
//      there is no marker producer.
//
//      WHAT RESTORES THE TIER: the native NRI marker layer. Today
//      IGpuCrashBackend::WriteMarkerNative is a stub on the graph backend
//      (NriDiagnostics.cpp returns false, deliberately, rather than claiming a
//      marker went out). When that lands, this scope's WriteMarkerNative call
//      starts producing real GPU markers with no edit at this line, and the
//      markers-only DRED tier becomes re-earnable -- in that order, and only
//      in that order.
//
//   THE GPU-SIDE HEARTBEAT IS NOT HERE: NriSwapChain owns the
//   fence-completed publisher and the frame slots that pace it
//   (NriSwapChain.hpp says so at length). A named coverage gap goes with
//   that -- "a frame slot never claims a stamp that did not go out" has no
//   UNIT pin; the graph-side slots enforce the rule but are covered only by
//   the [gpu] desk battery's resize storm.
//
// The active backend is reached through a process-wide slot rather than
// plumbed: F-8e's command-list owners are reached from different layers and
// do not all hold the same handle to plumb one through. The graph's recorders
// (RenderGraphExec's NodeScope, NriDiagnostics' fault dispatch) read the slot
// for exactly that reason -- they are reached from layers that cannot be
// handed a backend pointer. The slot mirrors
// Diagnostics::SetGpuSectionProvider exactly -- same owner
// (NriDiagnostics::Arm/Disarm), same lifetime, same install/clear sites -- so
// there is one rule to get right, not two.
//
// THREADING: GpuPassScope is for the thread that records the command list
// (the main/render thread in both hosts). GpuBreadcrumbs is not thread-safe
// and neither is this.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    // -----------------------------------------------------------------
    // The process-wide active-backend slot
    // -----------------------------------------------------------------

    // Install (or replace) the backend pass scopes write into. Called by
    // NriDiagnostics::Arm beside its one Diagnostics::SetGpuSectionProvider
    // call.
    ARCANE_API void SetActiveGpuCrashBackend(IGpuCrashBackend* backend) noexcept;

    // The installed backend, or null. Null is ORDINARY, not an error: a device-less
    // host, a backend that failed to arm, a test.
    [[nodiscard]] ARCANE_API IGpuCrashBackend* ActiveGpuCrashBackend() noexcept;

    // Clear the slot ONLY if it still holds `backend`; returns whether it
    // cleared. Same stale-registration hazard as Diagnostics::ClearSinkIfCurrent
    // -- an unconditional clear from an old device's teardown would disconnect a
    // live, unrelated one. Prefer this in any owner's teardown path.
    [[nodiscard]] ARCANE_API bool ClearActiveGpuCrashBackendIfCurrent(IGpuCrashBackend* backend) noexcept;

    // Draw-level marker toggle (`diagnostics.drawMarkers`, default false).
    // Pass scopes ignore it -- those are always on, in every config.
    //
    // ASYMMETRIC, DELIBERATELY. The SETTER is genuinely live:
    // ProjectBoot.hpp's ApplyDiagnosticsConfig calls it every boot from the
    // `diagnostics.drawMarkers` config key, so the flag is read from JSON and
    // stored. The GETTER has ZERO callers -- there is no draw-granular marker
    // scope to read it. Kept rather than pruned: dropping it means editing
    // that config-loading path too.
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
    // discover. The live fault injector (NriDiagnostics::FireFault) opens its
    // own compact scope, and the graph's own equivalent is
    // RenderGraphExec.cpp's NodeScope -- annotation, CPU ring, and a native
    // marker gated on device identity -- which is the richer of the two,
    // because it holds the device the compact form cannot. This class stays
    // because it is the seam a non-graph recorder would use.

    class ARCANE_API GpuPassScope
    {
    public:
        // `nativeCommandList` is the BACKEND'S OWN native command list --
        // ID3D12GraphicsCommandList* on D3D12, VkCommandBuffer on Vulkan,
        // exactly what IGpuCrashBackend::WriteMarkerNative documents.
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

}
