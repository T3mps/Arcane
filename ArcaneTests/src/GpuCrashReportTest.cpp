// Shared crash-report composition (GPU crash diagnostics arc, Tasks 5+6):
// the parts of a GPU crash report that are IDENTICAL on every backend, hoisted
// out of the two backend TUs into Arcane/Render/GpuCrashReport.{hpp,cpp}.
//
// Worth its own suite precisely BECAUSE it used to be unreachable: while this
// logic lived inside D3D12CrashBackend and VulkanCrashBackend it could only be
// exercised by a real device removal on a real GPU. As free functions over
// (GpuBreadcrumbs, GpuDumpWriter, Envelope, string, path) it has no GPU
// dependency at all, so the marker-replay rule, the queue-timeline block, and
// the `.gpudump` sibling contract are now machine-checked -- once, for both
// backends, which is the point of the hoist.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuCrashReport.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // the device-lost latch
#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using Arcane::GpuBreadcrumbs;
using Arcane::Diag::EmitGpuDumpSibling;
using Arcane::Diag::EmitQueueSnapshot;
using Arcane::Diag::Envelope;
using Arcane::Diag::GpuDumpWriter;
using Arcane::Diag::ReadGpuDump;
using Arcane::Diag::ReplayMarkerBuffer;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "arcane_gpucrashreport_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A marker region shaped exactly like the one a backend hands to the
    // replay: kGpuMarkerSlots x { begin, end }, zero meaning "never reached".
    struct MarkerRegion
    {
        MarkerRegion() : words(Arcane::Diag::kGpuMarkerSlots * Arcane::Diag::kGpuMarkerValuesPerSlot, 0u) {}

        // A backend writes `id + 1`, never `id`, so that 0 stays reserved.
        void Write(std::uint32_t id, bool begin)
        {
            const std::uint32_t slot = id % Arcane::Diag::kGpuMarkerSlots;
            words[slot * Arcane::Diag::kGpuMarkerValuesPerSlot + (begin ? 0u : 1u)] = id + 1;
        }

        [[nodiscard]] const void* Data() const { return words.data(); }

        std::vector<std::uint32_t> words;
    };

    [[nodiscard]] bool HasLayer(const Envelope& envelope, std::string_view layer)
    {
        return std::find(envelope.activeLayers.begin(), envelope.activeLayers.end(), layer) !=
               envelope.activeLayers.end();
    }

    [[nodiscard]] bool HasSection(const GpuDumpWriter& raw, std::string_view tag)
    {
        return raw.Inventory().find(tag) != std::string::npos;
    }
}

// ---------------------------------------------------------------------------
// ReplayMarkerBuffer
// ---------------------------------------------------------------------------

TEST_CASE("marker replay: a null region is breadcrumbs:off and adds no section", "[diag]")
{
    GpuBreadcrumbs breadcrumbs;
    GpuDumpWriter  raw;
    Envelope       envelope;

    // The D3D12 backend calls this unconditionally, so the null case must be
    // the honest "this backend has no marker layer" answer rather than a crash
    // or a silently-missing key.
    ReplayMarkerBuffer(breadcrumbs, raw, envelope, nullptr, /*armed=*/true);

    CHECK(HasLayer(envelope, "breadcrumbs:off"));
    CHECK(envelope.activeLayers.size() == 1);
    CHECK(raw.SectionCount() == 0);
}

TEST_CASE("marker replay: begin-without-end is the in-flight scope", "[diag]")
{
    GpuBreadcrumbs breadcrumbs;
    const std::uint32_t outer = breadcrumbs.BeginScope("frame");
    const std::uint32_t inner = breadcrumbs.BeginScope("shadow pass");
    breadcrumbs.EndScope(inner);
    breadcrumbs.EndScope(outer);   // CPU intent only -- must not affect the replay

    MarkerRegion region;
    region.Write(outer, true);
    region.Write(inner, true);     // entered the pass and never left it

    GpuDumpWriter raw;
    Envelope      envelope;
    ReplayMarkerBuffer(breadcrumbs, raw, envelope, region.Data(), /*armed=*/true);

    CHECK(HasLayer(envelope, "breadcrumbs:pass"));
    REQUIRE(HasSection(raw, "markers"));

    const GpuBreadcrumbs::Snapshot snapshot = breadcrumbs.Capture();
    CHECK(snapshot.lastCompleted.empty());
    // The enclosing scope comes along: GPU order guarantees an ancestor's end
    // marker lands after its descendants'.
    REQUIRE(snapshot.inFlight.size() == 2);
    CHECK(snapshot.inFlight[0] == "frame");
    CHECK(snapshot.inFlight[1] == "shadow pass");
}

TEST_CASE("marker replay: a disarmed region still ships its bytes but is not a pass timeline", "[diag]")
{
    GpuBreadcrumbs breadcrumbs;
    const std::uint32_t scope = breadcrumbs.BeginScope("frame");
    MarkerRegion region;
    region.Write(scope, true);
    region.Write(scope, false);

    GpuDumpWriter raw;
    Envelope      envelope;
    ReplayMarkerBuffer(breadcrumbs, raw, envelope, region.Data(), /*armed=*/false);

    // Partial data beats none -- the section ships; the LABEL is what tells a
    // reader the region is frozen and must not be read as a live timeline.
    CHECK(HasLayer(envelope, "breadcrumbs:disarmed"));
    CHECK_FALSE(HasLayer(envelope, "breadcrumbs:pass"));
    CHECK(HasSection(raw, "markers"));
    CHECK(breadcrumbs.Capture().lastCompleted == "frame");
}

TEST_CASE("marker replay: the section carries the whole region, byte for byte", "[diag]")
{
    GpuBreadcrumbs breadcrumbs;
    const std::uint32_t scope = breadcrumbs.BeginScope("frame");
    MarkerRegion region;
    region.Write(scope, true);

    GpuDumpWriter raw;
    Envelope      envelope;
    ReplayMarkerBuffer(breadcrumbs, raw, envelope, region.Data(), /*armed=*/true);

    const std::filesystem::path stem = TempDir("region") / "report";
    envelope.kind = "gpu-crash";
    std::string humanText;
    EmitGpuDumpSibling(raw, envelope, humanText, stem);

    const auto dump = ReadGpuDump(stem.string() + ".gpudump");
    REQUIRE(dump.has_value());
    REQUIRE(dump->sections.size() == 1);
    CHECK(dump->sections[0].tag == "markers");
    REQUIRE(dump->sections[0].bytes.size() == Arcane::Diag::kGpuMarkerBytes);
    // Slot 0's begin word is the scope id + 1, little-endian.
    CHECK(dump->sections[0].bytes[0] == static_cast<std::uint8_t>(scope + 1));
}

// ---------------------------------------------------------------------------
// EmitQueueSnapshot
// ---------------------------------------------------------------------------

TEST_CASE("queue snapshot: an empty ring still emits a named queue and <none> lines", "[diag]")
{
    const GpuBreadcrumbs breadcrumbs;
    Envelope             envelope;
    std::string          humanText;

    EmitQueueSnapshot(breadcrumbs, "graphics", envelope, humanText);

    REQUIRE(envelope.queues.size() == 1);
    CHECK(envelope.queues[0].name == "graphics");
    CHECK(envelope.queues[0].lastCompleted.empty());
    CHECK(envelope.queues[0].inFlight.empty());

    CHECK(humanText.find("queue graphics\n") != std::string::npos);
    CHECK(humanText.find("  last completed : <none>\n") != std::string::npos);
    CHECK(humanText.find("  in flight      : <none>\n") != std::string::npos);
}

TEST_CASE("queue snapshot: envelope and human text agree on the same timeline", "[diag]")
{
    GpuBreadcrumbs breadcrumbs;
    const std::uint32_t done   = breadcrumbs.BeginScope("upload");
    breadcrumbs.EndScope(done);
    const std::uint32_t flying = breadcrumbs.BeginScope("shadow pass");
    breadcrumbs.EndScope(flying);

    breadcrumbs.OnMarkerWritten(done, true);
    breadcrumbs.OnMarkerWritten(done, false);
    breadcrumbs.OnMarkerWritten(flying, true);   // entered, never left

    Envelope    envelope;
    std::string humanText;
    EmitQueueSnapshot(breadcrumbs, "graphics", envelope, humanText);

    REQUIRE(envelope.queues.size() == 1);
    CHECK(envelope.queues[0].lastCompleted == "upload");
    REQUIRE(envelope.queues[0].inFlight.size() == 1);
    CHECK(envelope.queues[0].inFlight[0] == "shadow pass");

    CHECK(humanText.find("  last completed : upload\n") != std::string::npos);
    CHECK(humanText.find("  in flight      : shadow pass\n") != std::string::npos);
    CHECK(humanText.find("<none>") == std::string::npos);
}

TEST_CASE("queue snapshot: the queue name is not hardcoded", "[diag]")
{
    const GpuBreadcrumbs breadcrumbs;
    Envelope             envelope;
    std::string          humanText;

    EmitQueueSnapshot(breadcrumbs, "compute", envelope, humanText);

    REQUIRE(envelope.queues.size() == 1);
    CHECK(envelope.queues[0].name == "compute");
    CHECK(humanText.find("queue compute\n") != std::string::npos);
}

// ---------------------------------------------------------------------------
// EmitGpuDumpSibling
// ---------------------------------------------------------------------------

TEST_CASE("gpudump sibling: a CPU-only report gets no container at all", "[diag]")
{
    GpuDumpWriter raw;
    raw.Add("markers", std::string_view{ "xx" });

    Envelope envelope;
    envelope.kind = "hang";        // not a gpu kind
    std::string humanText;

    const std::filesystem::path stem = TempDir("cpuonly") / "report";
    EmitGpuDumpSibling(raw, envelope, humanText, stem);

    CHECK(envelope.siblingGpuDump.empty());
    CHECK(humanText.empty());
    CHECK_FALSE(std::filesystem::exists(stem.string() + ".gpudump"));
}

TEST_CASE("gpudump sibling: a gpu kind writes the file and records the sibling truthfully", "[diag]")
{
    GpuDumpWriter raw;
    raw.Add("markers", std::string_view{ "ab" });
    raw.Add("dred.breadcrumb", std::string_view{ "" });   // armed but empty is a real answer

    Envelope envelope;
    envelope.kind = "gpu-crash";
    std::string humanText;

    const std::filesystem::path stem = TempDir("gpu") / "report";
    EmitGpuDumpSibling(raw, envelope, humanText, stem);

    const std::string expected = stem.string() + ".gpudump";
    CHECK(envelope.siblingGpuDump == expected);
    REQUIRE(std::filesystem::exists(expected));

    // The human block quotes the inventory so a reader knows what was captured
    // without opening the container.
    CHECK(humanText.find("gpu dump     : " + expected) != std::string::npos);
    CHECK(humanText.find("2 sections: markers, dred.breadcrumb") != std::string::npos);

    const auto dump = ReadGpuDump(expected);
    REQUIRE(dump.has_value());
    REQUIRE(dump->sections.size() == 2);
    CHECK(dump->sections[1].bytes.empty());
}

TEST_CASE("gpudump sibling: a failed write never names a sibling that does not exist", "[diag]")
{
    GpuDumpWriter raw;
    raw.Add("markers", std::string_view{ "ab" });

    Envelope envelope;
    envelope.kind = "gpu-stall";
    std::string humanText;

    // A stem inside a directory that does not exist: fopen fails, so the
    // report must say so rather than record a path nothing was written to.
    const std::filesystem::path stem = TempDir("badpath") / "no_such_dir" / "report";
    EmitGpuDumpSibling(raw, envelope, humanText, stem);

    CHECK(envelope.siblingGpuDump.empty());
    CHECK(humanText.find("gpu dump     : <failed to write>") != std::string::npos);
}

TEST_CASE("device-loss freeze: only a real loss verdict freezes the ring", "[diag]")
{
    // The rule lands ONCE for both backends (this suite's charter): any
    // non-empty fault.type other than the shared healthy verdict
    // "device-alive" is a loss. A stall on a live device keeps recording.
    using Arcane::Diag::FreezeBreadcrumbsOnDeviceLoss;

    GpuBreadcrumbs bc;
    Envelope envelope;

    SECTION("an unclassified envelope does not freeze")
    {
        FreezeBreadcrumbsOnDeviceLoss(bc, envelope);   // fault.type empty
        CHECK_FALSE(bc.IsFrozen());
    }

    SECTION("device-alive (the gpu-stall case) does not freeze")
    {
        envelope.fault.type = "device-alive";
        FreezeBreadcrumbsOnDeviceLoss(bc, envelope);
        CHECK_FALSE(bc.IsFrozen());
    }

    SECTION("every loss verdict freezes")
    {
        for (const char* verdict : { "device-removed", "device-hung", "device-reset",
                                     "driver-internal-error", "page-fault" })
        {
            GpuBreadcrumbs fresh;
            Envelope e;
            e.fault.type = verdict;
            FreezeBreadcrumbsOnDeviceLoss(fresh, e);
            CHECK(fresh.IsFrozen());
        }
    }
}

TEST_CASE("device-lost latch: set-after-report semantics, host-visible, re-armable", "[diag]")
{
    // The latch itself is trivial by design -- the contract that matters is
    // lifecycle: hosts poll it every frame, the device layer sets it after
    // the gpu-crash report lands, and a NEW device (project switch) clears
    // it so a healthy replacement does not inherit the dead one's verdict.
    Arcane::ResetGpuDeviceLost();   // other [diag] cases must not leak into this one
    CHECK_FALSE(Arcane::GpuDeviceLostObserved());

    Arcane::NoteGpuDeviceLost();
    CHECK(Arcane::GpuDeviceLostObserved());

    Arcane::ResetGpuDeviceLost();
    CHECK_FALSE(Arcane::GpuDeviceLostObserved());
}
