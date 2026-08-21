// NriDiagnostics: the crash chain armed by whichever device exists. Headless
// -- [nri], inside the ~[gpu] dev gate.
//
// WHAT IS AND IS NOT REACHABLE HERE, stated up front because the split is the
// whole reason these cases look the way they do:
//
//   * ARMING is fully reachable. Every slot Arm() fills is a process-wide
//     pointer slot in Arcane.dll with a reader, and NriDevice::CreateNoneForTests
//     gives a real nri::Device to arm over. So "the NONE device arms the hook
//     slot", "a double Arm touches nothing", "an incumbent-armed process is
//     left alone" and "Disarm empties exactly what Arm filled" are all EXACT
//     properties here, not proxies.
//   * The HEARTBEAT's end-to-end path is not: its value source is a GPU
//     timeline fence (NONE's GetFenceValue returns 0 unconditionally) and its
//     consumer is a watchdog THREAD against a wall clock -- which is exactly
//     the shape that produced the 2026-08-11 hosted-CI flake. So the case
//     below drives the real, clock-parameterized rule
//     (Diagnostics::ProgressStallRule, the part that can be WRONG) with the
//     value sequence the graph path publishes, and calls the real publisher on
//     the same path so the seam itself is executed rather than merely
//     compiled. The threaded, report-writing end of that chain is already
//     pinned once, generically, in DiagnosticsTest.cpp; a second slow twin of
//     it would buy nothing this cannot state exactly.
//   * FireFault is [gpu] by construction (MapBuffer, a compute pipeline, a
//     real queue) and is desk-proven. In-session it is compile + shape.
//
// Include order: NRI headers first, ALWAYS -- Extensions/NRIDeviceCreation.h
// declares nri::Message::ERROR and <windows.h> #defines ERROR via wingdi.h.
// The route into spdlog is Arcane/Render/RenderErrorLatch.hpp -> Base/Log.hpp.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderDeviceRemovedHookForTest -- reached through Device.hpp until Task 8b
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriDiagnostics.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    // RAII net for the process-wide slots Arm() fills. Same reasoning as
    // RenderGraphTest.cpp's ScopedGpuCrashBackend: a REQUIRE failure between
    // an Arm and its manual Disarm would unwind past the Disarm entirely,
    // leaving the Diagnostics GPU-section provider pointing at a backend this
    // case is about to destroy -- and the next case that writes a report (or
    // the watchdog, on its own thread) would run the provider against freed
    // memory instead of reporting its own, unrelated failure.
    class ScopedNriDiagnostics
    {
    public:
        ScopedNriDiagnostics() = default;
        ~ScopedNriDiagnostics() { Arcane::NriDiagnostics::Disarm(); }

        ScopedNriDiagnostics(const ScopedNriDiagnostics&)            = delete;
        ScopedNriDiagnostics& operator=(const ScopedNriDiagnostics&) = delete;
    };

    // Stands in for whatever incumbent backend armed first. Nothing about it
    // needs to work; Arm()'s test is "the active-backend slot is non-null",
    // and that is the only fact an incumbent backend publishes about itself
    // that this file can see.
    class StubCrashBackend final : public Arcane::IGpuCrashBackend
    {
    public:
        bool WriteMarkerNative(void*, std::uint32_t, bool) override { return false; }
        void CollectFault(Arcane::Diag::Envelope&) override {}
        Arcane::GpuBreadcrumbs& Breadcrumbs() override { return m_breadcrumbs; }
        const char* Name() const override { return "stub-nvrhi"; }
        [[nodiscard]] void* NativeDevice() const override { return nullptr; }

    private:
        Arcane::GpuBreadcrumbs m_breadcrumbs;
    };

    // The same safety net ScopedNriDiagnostics is, for the OTHER writer of the
    // process-wide backend slot -- mirroring RenderGraphTest.cpp's
    // ScopedGpuCrashBackend verbatim in reasoning. A case that installs the
    // stub and then trips a REQUIRE before its manual clear unwinds past the
    // clear, leaving the slot pointing at a StubCrashBackend on this case's
    // dead stack; the next case to write a report (or the watchdog, on its own
    // thread) then use-after-frees instead of reporting its own failure. A
    // case may still clear explicitly on its happy path as an ASSERTION --
    // the dtor is then a harmless no-op CAS miss.
    class ScopedGpuCrashBackend
    {
    public:
        explicit ScopedGpuCrashBackend(Arcane::IGpuCrashBackend* backend) noexcept
            : m_backend(backend)
        {
            Arcane::SetActiveGpuCrashBackend(m_backend);
        }
        ~ScopedGpuCrashBackend() { (void)Arcane::ClearActiveGpuCrashBackendIfCurrent(m_backend); }

        ScopedGpuCrashBackend(const ScopedGpuCrashBackend&)            = delete;
        ScopedGpuCrashBackend& operator=(const ScopedGpuCrashBackend&) = delete;

    private:
        Arcane::IGpuCrashBackend* m_backend;
    };

    // RAII for the process-wide device-lost latch. A case that FIRES the
    // device-removed observer sets it (NoteGpuDeviceLost), and a host polling
    // that latch quits on it -- so a case must never leave it set for the next
    // one, assertion failure or not. Cleared with the same call every arming
    // site makes.
    struct ScopedGpuDeviceLostLatch
    {
        ScopedGpuDeviceLostLatch() = default;
        ~ScopedGpuDeviceLostLatch() { Arcane::ResetGpuDeviceLost(); }

        ScopedGpuDeviceLostLatch(const ScopedGpuDeviceLostLatch&)            = delete;
        ScopedGpuDeviceLostLatch& operator=(const ScopedGpuDeviceLostLatch&) = delete;
    };

    // Diagnostics::Install is a no-op when something is already armed, so a
    // case that installs MUST fully disarm -- and by RAII, since a failing
    // REQUIRE unwinds. Same struct, same reason, as DiagnosticsTest.cpp's.
    struct ArmedDiagnostics
    {
        explicit ArmedDiagnostics(const Arcane::Diagnostics::Config& cfg)
        {
            Arcane::Diagnostics::Install(cfg);
        }
        ~ArmedDiagnostics() { Arcane::Diagnostics::Shutdown(); }

        ArmedDiagnostics(const ArmedDiagnostics&)            = delete;
        ArmedDiagnostics& operator=(const ArmedDiagnostics&) = delete;
    };

    std::filesystem::path FreshReportDir(const char* leaf)
    {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / "arcane-diag-test" / leaf;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::string ReadWholeFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::filesystem::path SiblingWithExt(const std::string& txtPath, const char* ext)
    {
        std::filesystem::path path(txtPath);
        path.replace_extension(ext);
        return path;
    }

    [[nodiscard]] bool HasLayer(const Arcane::Diag::Envelope& envelope, std::string_view layer)
    {
        for (const std::string& active : envelope.activeLayers)
            if (active == layer)
                return true;
        return false;
    }
}

// =============================================================================
// Step 1: the heartbeat publisher reaches the stall rule
// =============================================================================

TEST_CASE("nri diagnostics: the graph heartbeat's value stream reaches the stall rule at 8s", "[nri]")
{
    // "Fires at 8s" is the SHIPPED default, not a literal chosen here: the
    // rule the watchdog constructs takes Config::gpuStallSeconds, and a test
    // that hardcoded 8 would keep passing if that default were changed out
    // from under the graph path.
    const Arcane::Diagnostics::Config shipped;
    REQUIRE(shipped.gpuStallSeconds == 8);

    Arcane::Diagnostics::ProgressStallRule rule(static_cast<double>(shipped.gpuStallSeconds));

    // THE PUBLISHER'S OWN VALUE SHAPE. NriGraphContext::RenderFrame publishes
    // NriSwapChain::CompletedFrameValue() -- the pacing timeline fence's
    // completed value -- after every PRESENTED frame. A healthy 60Hz run
    // advances it once per frame.
    double        now   = 0.0;
    std::uint64_t fence = 0;
    int           reports = 0;

    for (int frame = 0; frame < 600; ++frame, now += 1.0 / 60.0)
    {
        ++fence;
        // The real seam, on the real path: exported, callable with no
        // Diagnostics::Install (a headless host never arms the watchdog), and
        // publishing the same value the rule below is fed.
        Arcane::NriDiagnostics::PublishHeartbeat(fence);
        if (rule.Poll(fence, now)) ++reports;
    }
    // Ten seconds of healthy presents -- well past the 8s threshold -- and not
    // one stall verdict, because the counter kept moving.
    CHECK(reports == 0);

    // THE WEDGE. The GPU stops retiring: the render path keeps running (the
    // pacing wait polls and republishes -- NriSwapChain's
    // PollingWaitForTimelineFence), so the value keeps being PUBLISHED while
    // it stops ADVANCING. That distinction is the entire signal.
    const std::uint64_t frozen = fence;
    const double        wedged = now;

    for (int i = 0; i < 60; ++i, now += 1.0 / 60.0)      // 1s frozen
    {
        Arcane::NriDiagnostics::PublishHeartbeat(frozen);
        if (rule.Poll(frozen, now)) ++reports;
    }
    CHECK(reports == 0);

    // ...and at the threshold, exactly one verdict.
    now = wedged + static_cast<double>(shipped.gpuStallSeconds);
    Arcane::NriDiagnostics::PublishHeartbeat(frozen);
    CHECK(rule.Poll(frozen, now));

    // Still wedged, still publishing: one report per stall, not one per poll.
    for (int i = 0; i < 60; ++i, now += 1.0 / 60.0)
    {
        Arcane::NriDiagnostics::PublishHeartbeat(frozen);
        CHECK_FALSE(rule.Poll(frozen, now));
    }
    CHECK(rule.StalledSeconds(now) >= static_cast<double>(shipped.gpuStallSeconds));
}

// =============================================================================
// Step 2: the arming seam
// =============================================================================

TEST_CASE("nri diagnostics: a NONE device arms the whole chain, and a second Arm is a no-op", "[nri]")
{
    // Nothing may already hold the slots. In the ~[gpu] gate no real device
    // exists, and every case that installs a stub cleans up after itself --
    // so a failure here is a leak from an earlier case, not this one's bug.
    REQUIRE_FALSE(Arcane::NriDiagnostics::IsArmed());
    REQUIRE(Arcane::ActiveGpuCrashBackend() == nullptr);
    REQUIRE(Arcane::RenderDeviceRemovedHookForTest() == nullptr);

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    ScopedNriDiagnostics guard;

    // THIS call armed.
    CHECK(Arcane::NriDiagnostics::Arm(*device));
    CHECK(Arcane::NriDiagnostics::IsArmed());

    // All three installs, each read back from the slot it actually filled.
    void (*const hook)() = Arcane::RenderDeviceRemovedHookForTest();
    CHECK(hook != nullptr);

    Arcane::IGpuCrashBackend* const backend = Arcane::ActiveGpuCrashBackend();
    REQUIRE(backend != nullptr);
    CHECK(backend == Arcane::NriDiagnostics::ArmedBackend());

    // The graph's breadcrumbs land in THIS backend's ring from here on --
    // RenderGraphExec's NodeScope reads the same process-wide slot. Tokens are
    // monotonic from 0 and never reused, so a fresh backend's first token is 0.
    CHECK(backend->Breadcrumbs().BeginScope("probe") == 0);

    // A SECOND Arm touches nothing: same hook, same backend, and it says so.
    CHECK_FALSE(Arcane::NriDiagnostics::Arm(*device));
    CHECK(Arcane::NriDiagnostics::IsArmed());
    CHECK(Arcane::RenderDeviceRemovedHookForTest() == hook);
    CHECK(Arcane::ActiveGpuCrashBackend() == backend);
    // Not merely "a backend is installed" -- the SAME object, i.e. no second
    // one was built and quietly swapped in (which would strand the ring the
    // graph had been writing into).
    CHECK(Arcane::NriDiagnostics::ArmedBackend() == backend);

    // Disarm empties exactly what Arm filled, and is idempotent.
    Arcane::NriDiagnostics::Disarm();
    CHECK_FALSE(Arcane::NriDiagnostics::IsArmed());
    CHECK(Arcane::NriDiagnostics::ArmedBackend() == nullptr);
    CHECK(Arcane::ActiveGpuCrashBackend() == nullptr);
    CHECK(Arcane::RenderDeviceRemovedHookForTest() == nullptr);

    Arcane::NriDiagnostics::Disarm();
    CHECK_FALSE(Arcane::NriDiagnostics::IsArmed());
}

TEST_CASE("nri diagnostics: Arm no-ops when an NVRHI crash backend already holds the chain", "[nri]")
{
    // THE PROPERTY IS GENERAL: displacing ANY incumbent crash backend would
    // point the Diagnostics GPU-section provider at a ring with no DRED, no
    // device-fault query and no marker buffer, while the device that HAS all
    // three is still rendering. So Arm() must no-op rather than take the chain
    // from whoever already holds it.
    REQUIRE_FALSE(Arcane::NriDiagnostics::IsArmed());
    REQUIRE(Arcane::ActiveGpuCrashBackend() == nullptr);

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    StubCrashBackend      nvrhiBackend;
    ScopedGpuCrashBackend incumbent(&nvrhiBackend);

    ScopedNriDiagnostics guard;

    CHECK_FALSE(Arcane::NriDiagnostics::Arm(*device));
    CHECK_FALSE(Arcane::NriDiagnostics::IsArmed());
    CHECK(Arcane::NriDiagnostics::ArmedBackend() == nullptr);

    // Untouched, all of it: the incumbent keeps the backend slot, and no hook
    // was installed on its behalf either (the incumbent owns that too).
    CHECK(Arcane::ActiveGpuCrashBackend() == &nvrhiBackend);
    CHECK(Arcane::RenderDeviceRemovedHookForTest() == nullptr);

    // ...and a Disarm from the un-armed installer must not unslot the
    // incumbent. This is the stale-owner hazard
    // ClearActiveGpuCrashBackendIfCurrent exists for, stated as a property.
    Arcane::NriDiagnostics::Disarm();
    CHECK(Arcane::ActiveGpuCrashBackend() == &nvrhiBackend);

    // Explicit on the happy path as an ASSERTION (the incumbent is still the
    // slot's owner, so the clear takes); `incumbent`'s dtor is then a no-op.
    REQUIRE(Arcane::ClearActiveGpuCrashBackendIfCurrent(&nvrhiBackend));
}

// =============================================================================
// Step 2, continued: what the armed chain actually PUTS IN A REPORT
// =============================================================================
// The slot-identity cases above prove Arm() filled the three slots. They say
// nothing about the one thing the phase GATE is: that a report written while
// this installer holds the chain carries a GPU section built by the graph
// backend rather than nothing at all. Diagnostics::WriteReport drives exactly
// the path a device-removed observation takes (DiagnosticsTest.cpp's GPU cases
// use it the same way), so the whole provider -> CollectFault -> queue block ->
// `.gpudump` chain is reachable here with no GPU in the process.

TEST_CASE("nri diagnostics: an armed NRI chain fills a real report's GPU section", "[nri]")
{
    REQUIRE_FALSE(Arcane::NriDiagnostics::IsArmed());
    REQUIRE(Arcane::ActiveGpuCrashBackend() == nullptr);

    const std::filesystem::path dir = FreshReportDir("nri-armed-chain");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "NriDiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;   // this case drives WriteReport directly

    ArmedDiagnostics armed(cfg);

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // Declared AFTER `armed` so it unwinds FIRST: the provider slot must be
    // emptied while Diagnostics is still up, exactly as NriGraphContext's dtor
    // orders its Disarm ahead of the host's diagnostics shutdown.
    ScopedNriDiagnostics guard;
    REQUIRE(Arcane::NriDiagnostics::Arm(*device));

    Arcane::IGpuCrashBackend* const backend = Arcane::ActiveGpuCrashBackend();
    REQUIRE(backend != nullptr);

    // The shape the graph records: one open node scope, named exactly as
    // --crash-gpu's is, since reading `pass:gpu-fault` back out of a report is
    // the desk battery's item. RenderGraphExec's NodeScope does precisely this
    // against this same slot. Spelled as a literal rather than through
    // GpuFaultInjector::kPassName because that constant is itself
    // `#if !defined(ARCANE_DIST)` and this exe builds in Dist too.
    const std::uint32_t token = backend->Breadcrumbs().BeginScope("pass:gpu-fault");

    const std::string txtPath = Arcane::Diagnostics::WriteReport("gpu-crash: device removed (test)");
    REQUIRE_FALSE(txtPath.empty());

    const std::string body = ReadWholeFile(txtPath);
    CHECK(body.find("=== GPU ===") != std::string::npos);
    CHECK(body.find("queue graphics") != std::string::npos);

    const std::filesystem::path diagPath = SiblingWithExt(txtPath, ".arcdiag");
    REQUIRE(std::filesystem::exists(diagPath));

    const auto envelope = Arcane::Diag::ReadFile(diagPath);
    REQUIRE(envelope.has_value());
    CHECK(envelope->kind == "gpu-crash");

    // THE VERDICT, from the only evidence this backend has: a `gpu-crash`
    // report is written by the device-removed observer and by nothing else, so
    // the device is lost by construction -- which is also what makes
    // FreezeBreadcrumbsOnDeviceLoss fire below, exactly as it does on both
    // native backends. (A `gpu-stall` reason would instead yield the shared
    // healthy verdict "device-alive" and leave the ring live.)
    CHECK(envelope->fault.type == "device-removed");

    // The honest inventory, not a failure: no GPU-written marker layer here,
    // so ReplayMarkerBuffer pushes `breadcrumbs:off` and adds no raw section --
    // and the `.gpudump` still lands, because the section table IS the
    // inventory.
    CHECK(HasLayer(*envelope, "breadcrumbs:off"));
    REQUIRE_FALSE(envelope->siblingGpuDump.empty());
    REQUIRE(std::filesystem::exists(envelope->siblingGpuDump));
    const auto dump = Arcane::Diag::ReadGpuDump(envelope->siblingGpuDump);
    REQUIRE(dump.has_value());
    CHECK(dump->sections.empty());

    // The queue block is EMITTED, and -- until a marker layer feeds
    // OnMarkerWritten -- it is empty, because GpuBreadcrumbs::Capture() trusts
    // marker evidence only (CPU-side EndScope deliberately feeds it nothing).
    // Pinned so the report's real content is not mistaken for the ring's.
    REQUIRE(envelope->queues.size() == 1);
    CHECK(envelope->queues[0].name == "graphics");
    CHECK(envelope->queues[0].lastCompleted.empty());
    CHECK(envelope->queues[0].inFlight.empty());

    // ...and the OTHER half of the same property: the ring already holds the
    // graph's scope names, so the day a native NRI marker layer lands (its own
    // arc -- NriDiagnostics.hpp) and WriteMarkerNative stops returning false,
    // THIS backend's queue block names them with no edit to the provider. The
    // marker evidence is simulated here exactly as a marker-buffer replay
    // delivers it.
    backend->Breadcrumbs().OnMarkerWritten(token, /*begin=*/true);

    const std::string secondTxt = Arcane::Diagnostics::WriteReport("gpu-crash: device removed (test 2)");
    REQUIRE_FALSE(secondTxt.empty());
    const auto second = Arcane::Diag::ReadFile(SiblingWithExt(secondTxt, ".arcdiag"));
    REQUIRE(second.has_value());
    REQUIRE(second->queues.size() == 1);
    REQUIRE(second->queues[0].inFlight.size() == 1);
    CHECK(second->queues[0].inFlight[0] == "pass:gpu-fault");
}

// =============================================================================
// Step 2, continued: re-arming re-arms the OBSERVER, not just the slots
// =============================================================================
// `ObserveDeviceRemoved` reports the first removal it sees and then latches --
// one loss cascades into many callbacks and one report is the truth. Both
// device TUs clear that latch at their own arming site, one line above
// ResetGpuDeviceLost(); NriDiagnostics::Arm is the SECOND arming site (and
// after Task 6 the only one), so it owes the same pair.
//
// The property is reachable headlessly because the observer touches no device
// at all: latch -> Diagnostics::WriteReport -> NoteGpuDeviceLost. So the hook
// can be FIRED here, through the same read-back the arming cases use, and "a
// second loss after a re-arm still writes a report" is an exact statement
// rather than a proxy for one.

TEST_CASE("nri diagnostics: re-arming clears the removal latch, so a second device loss still reports", "[nri]")
{
    REQUIRE_FALSE(Arcane::NriDiagnostics::IsArmed());
    REQUIRE(Arcane::ActiveGpuCrashBackend() == nullptr);

    const std::filesystem::path dir = FreshReportDir("nri-relatch");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "NriDiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;

    ArmedDiagnostics armed(cfg);

    // The observer sets the process-wide device-lost latch (hosts poll it and
    // quit on it), so this case must put it back even if an assertion below
    // unwinds first -- same reasoning as every other guard in this file.
    ScopedGpuDeviceLostLatch lostLatch;

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // ---- the first device's life: armed, then lost ----
    {
        ScopedNriDiagnostics guard;
        REQUIRE(Arcane::NriDiagnostics::Arm(*device));

        void (*const hook)() = Arcane::RenderDeviceRemovedHookForTest();
        REQUIRE(hook != nullptr);

        const std::uint32_t before = Arcane::Diagnostics::ReportCount();
        hook();   // exactly what the latch does on a removal
        CHECK(Arcane::Diagnostics::ReportCount() == before + 1);
        CHECK(Arcane::GpuDeviceLostObserved());
    }

    // ---- the host survives it and rebuilds the graph context ----
    {
        ScopedNriDiagnostics guard;
        REQUIRE(Arcane::NriDiagnostics::Arm(*device));

        // The pair, both halves: the loss the old latches described is over.
        CHECK_FALSE(Arcane::GpuDeviceLostObserved());

        void (*const hook)() = Arcane::RenderDeviceRemovedHookForTest();
        REQUIRE(hook != nullptr);

        // THE REGRESSION THIS CASE EXISTS FOR. With the removal latch left set
        // by the previous device's loss, this call returns at its first line
        // and the second loss is SILENT -- no report, no `.gpudump`, and no
        // NoteGpuDeviceLost for the host to quit on. The count is the whole
        // assertion.
        const std::uint32_t before = Arcane::Diagnostics::ReportCount();
        hook();
        CHECK(Arcane::Diagnostics::ReportCount() == before + 1);
        CHECK(Arcane::GpuDeviceLostObserved());
    }

    // The removal latch is left SET here, deliberately: clearing it is Arm()'s
    // job and nobody else's, which is the property this case just stated. Any
    // later case that arms therefore gets a cleared latch by construction.
}
