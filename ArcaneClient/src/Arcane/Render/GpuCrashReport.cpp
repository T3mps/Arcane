// GPU crash diagnostics arc: the backend-agnostic half of a crash report.
// See GpuCrashReport.hpp for why this is its own TU rather than more of
// IGpuCrashBackend.hpp. Nothing here touches a GPU API; every function is a
// pure transformation of (breadcrumb ring, dump container) into (envelope,
// human text, sibling file).

#include <Arcane/Render/GpuCrashReport.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <utility>

namespace Arcane::Diag
{
    void ReplayMarkerBuffer(GpuBreadcrumbs& breadcrumbs,
                            GpuDumpWriter&  raw,
                            Envelope&       envelope,
                            const void*     markerMemory,
                            bool            armed)
    {
        if (!markerMemory)
        {
            envelope.activeLayers.emplace_back("breadcrumbs:off");
            return;
        }

        const auto* slots = static_cast<const volatile std::uint32_t*>(markerMemory);
        for (std::uint32_t slot = 0; slot < kGpuMarkerSlots; ++slot)
        {
            const std::uint32_t beginValue = slots[slot * kGpuMarkerValuesPerSlot + 0];
            const std::uint32_t endValue   = slots[slot * kGpuMarkerValuesPerSlot + 1];
            if (beginValue != kGpuMarkerUnwritten) breadcrumbs.OnMarkerWritten(beginValue - 1, true);
            if (endValue   != kGpuMarkerUnwritten) breadcrumbs.OnMarkerWritten(endValue   - 1, false);
        }
        raw.Add("markers", markerMemory, kGpuMarkerBytes);

        // activeLayers is the report's declared truth channel for "what
        // engaged", so the claim tracks the runtime kill switch -- see the
        // header for why a frozen region must not read as `pass`.
        envelope.activeLayers.emplace_back(armed ? "breadcrumbs:pass" : "breadcrumbs:disarmed");
    }

    void EmitQueueSnapshot(const GpuBreadcrumbs& breadcrumbs,
                           std::string_view      queueName,
                           Envelope&             envelope,
                           std::string&          humanText)
    {
        const GpuBreadcrumbs::Snapshot snapshot = breadcrumbs.Capture();

        Envelope::Queue queue;
        queue.name          = std::string(queueName);
        queue.lastCompleted = snapshot.lastCompleted;
        queue.inFlight      = snapshot.inFlight;
        envelope.queues.push_back(std::move(queue));

        humanText += "queue " + std::string(queueName) + "\n";
        humanText += "  last completed : " +
                     (snapshot.lastCompleted.empty() ? std::string("<none>") : snapshot.lastCompleted) + "\n";
        if (snapshot.inFlight.empty())
        {
            humanText += "  in flight      : <none>\n";
        }
        else
        {
            for (const std::string& scope : snapshot.inFlight)
                humanText += "  in flight      : " + scope + "\n";
        }
    }

    void FreezeBreadcrumbsOnDeviceLoss(GpuBreadcrumbs& breadcrumbs, const Envelope& envelope)
    {
        // "device-alive" is the one healthy verdict a crash backend can
        // report, and this guard is LOAD-BEARING, not vestigial:
        // NriGraphCrashBackend::CollectFault (Nri/NriDiagnostics.cpp) sets
        // fault.type to "device-alive" for every gpu-stall report -- the
        // watchdog's verdict on a device that is merely slow, not lost. So
        // this branch is reached through the non-empty `!= "device-alive"`
        // path on every gpu-stall report, not only through the EMPTY case
        // below. Treating it as dead and deleting it would freeze the ring on
        // every gpu-stall, destroying the crash-time breadcrumb ring on a
        // device that never actually died. The string is also the envelope's
        // published contract (DiagEnvelopeTest pins fault.type's round-trip
        // shape).
        // An EMPTY type means no backend classified anything -- also not a
        // loss. Everything else (device-removed/-hung/-reset, page-fault
        // kinds, driver-internal-error) means the device is gone and the
        // frames the host keeps pumping must not recycle the crash-time ring.
        if (!envelope.fault.type.empty() && envelope.fault.type != "device-alive")
            breadcrumbs.Freeze();
    }

    void EmitGpuDumpSibling(const GpuDumpWriter&         raw,
                            Envelope&                    envelope,
                            std::string&                 humanText,
                            const std::filesystem::path& reportStem)
    {
        if (!envelope.kind.starts_with("gpu"))
            return;

        std::filesystem::path dumpPath = reportStem;
        dumpPath += ".gpudump";
        if (raw.Write(dumpPath))
        {
            // Truthful sibling: recorded only because the file landed.
            envelope.siblingGpuDump = dumpPath.string();
            humanText += "gpu dump     : " + dumpPath.string() +
                         " (" + std::to_string(raw.SectionCount()) + " sections: " +
                         raw.Inventory() + ")\n";
        }
        else
        {
            ARC_WARN("GPU crash backend: failed to write {}", dumpPath.string());
            humanText += "gpu dump     : <failed to write>\n";
        }
    }
}
