#pragma once

// GPU crash diagnostics arc: the report-composition helpers BOTH backends
// share (Task 5 = D3D12, Task 6 = Vulkan).
//
// Three concerns, three files, deliberately:
//   - IGpuCrashBackend.hpp -- the seam a backend implements, plus the
//     `.gpudump` container (pure byte shuffling, header-only).
//   - GpuBreadcrumbs.hpp   -- the CPU-side scope ring.
//   - THIS file            -- turning those two into the parts of a crash
//     report that are IDENTICAL on every backend: the marker-buffer replay,
//     the queue-timeline block, and the `.gpudump` sibling write.
//
// Everything declared here is BACKEND-AGNOSTIC by construction -- no D3D12 or
// Vulkan type appears in any signature -- which is the whole point. A fix to
// the sibling contract, to the marker-replay rule, or to the human-readable
// queue block must land ONCE, not once per backend and never half-landed.
//
// Split out of IGpuCrashBackend.hpp rather than added to it: that header's
// stated identity is "the seam plus the container, deliberately header-only so
// a test links it without pulling in a GPU backend", and report composition
// depends on GpuBreadcrumbs, which is a third thing. These are ordinary
// exported functions in Arcane.dll, so a `[diag]` test links them with no GPU
// backend either (GpuCrashReportTest.cpp).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace Arcane::Diag
{
    struct Envelope;      // <Arcane/Base/DiagEnvelope.hpp>
    class GpuDumpWriter;  // <Arcane/Render/IGpuCrashBackend.hpp>

    // ---------------------------------------------------------------------
    // Marker-buffer geometry -- ONE definition, shared by both backends
    // ---------------------------------------------------------------------
    // One 2-value entry (begin, end) per scope, indexed `id % kGpuMarkerSlots`
    // and sized to GpuBreadcrumbs' ring so a marker slot recycles exactly when
    // the ring entry that names it does. This is F-8e's "one buffer with
    // per-scope slots addressed by a stable scope id" -- never one buffer per
    // command list, never sized to a submit count.
    //
    // Shared rather than per-backend because ReplayMarkerBuffer below reads
    // ANY backend's region with these constants: if a backend's writer and
    // this reader could disagree, the replay would silently produce garbage.
    inline constexpr std::uint32_t kGpuMarkerSlots =
        static_cast<std::uint32_t>(GpuBreadcrumbs::kRingCapacity);
    inline constexpr std::uint32_t kGpuMarkerValuesPerSlot = 2;
    inline constexpr std::size_t   kGpuMarkerBytes =
        std::size_t{ kGpuMarkerSlots } * kGpuMarkerValuesPerSlot * sizeof(std::uint32_t);

    // A backend writes `id + 1` so that 0 -- what a region is zeroed to at arm
    // time -- unambiguously means "the GPU never reached this marker".
    inline constexpr std::uint32_t kGpuMarkerUnwritten = 0;

    // ---------------------------------------------------------------------
    // Formatting (no fmt in a post-mortem path by choice -- these run inside
    // a process that is already misbehaving)
    // ---------------------------------------------------------------------

    [[nodiscard]] inline std::string HexU32(std::uint32_t v)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "0x%08X", v);
        return buffer;
    }

    [[nodiscard]] inline std::string HexU64(std::uint64_t v)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(v));
        return buffer;
    }

    // ---------------------------------------------------------------------
    // Shared report composition
    // ---------------------------------------------------------------------

    // Replays a GPU-written marker region into `breadcrumbs`, appends the raw
    // bytes to `raw` as the "markers" section, and pushes the matching
    // breadcrumbs key onto `envelope.activeLayers`.
    //
    // `markerMemory` is read VOLATILE: the GPU may still be writing into it,
    // so it is an observation of foreign memory, not a value the compiler may
    // cache or reorder. It must be at least kGpuMarkerBytes long.
    //
    // `armed` is the backend's RUNTIME kill switch, not merely "the region
    // exists". A backend latches markers off when its write path breaks
    // mid-run, which leaves the region FROZEN at whatever it last held;
    // claiming `breadcrumbs:pass` there would sell a stale timeline as a live
    // one. The raw section ships either way (partial data beats none) --
    // `breadcrumbs:disarmed` is what tells a reader not to trust it as a pass
    // timeline.
    //
    // A null `markerMemory` pushes `breadcrumbs:off` and adds no section, so a
    // backend with no marker layer at all can call this unconditionally. A
    // backend that has a DIFFERENT fallback (Vulkan's fence correlation) must
    // instead guard the call and emit its own key.
    ARCANE_API void ReplayMarkerBuffer(GpuBreadcrumbs& breadcrumbs,
                                       GpuDumpWriter&  raw,
                                       Envelope&       envelope,
                                       const void*     markerMemory,
                                       bool            armed);

    // Captures `breadcrumbs` and emits the queue timeline BOTH ways: as an
    // Envelope::Queue named `queueName` (the machine-readable .arcdiag field)
    // and as the report's human-readable "queue <name>" block appended to
    // `humanText`. Never throws; an empty ring yields "<none>" for both lines.
    ARCANE_API void EmitQueueSnapshot(const GpuBreadcrumbs& breadcrumbs,
                                      std::string_view      queueName,
                                      Envelope&             envelope,
                                      std::string&          humanText);

    // Freezes `breadcrumbs` iff `envelope`'s fault classification says the
    // device was actually LOST -- any non-empty fault.type other than the
    // backends' shared healthy verdict "device-alive". Called by both
    // backends' FillReport after CollectFault, so the rule lands ONCE (this
    // header's charter). A gpu-stall on a live device must NOT freeze: the
    // device is still executing and the ring should keep recording.
    ARCANE_API void FreezeBreadcrumbsOnDeviceLoss(GpuBreadcrumbs& breadcrumbs,
                                                  const Envelope& envelope);

    // Writes `raw` to `<reportStem>.gpudump` and records the sibling.
    //
    // The container is written for gpu kinds ALWAYS, partial collection
    // included -- the section table doubles as the capture inventory, and
    // "markers present, fault data absent" is itself the answer. A CPU-only
    // crash/hang report gets no `.gpudump` at all, which is why the kind check
    // lives here rather than at each call site.
    //
    // `envelope.siblingGpuDump` is set ONLY when the file actually landed: a
    // report must never name a sibling it did not write.
    ARCANE_API void EmitGpuDumpSibling(const GpuDumpWriter&         raw,
                                       Envelope&                    envelope,
                                       std::string&                 humanText,
                                       const std::filesystem::path& reportStem);
}
