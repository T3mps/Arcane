// Arcane::Diagnostics -- the crash/hang post-mortem capture path. CPU-only ([diag]).
//
// These tests exist because of WHEN this code is supposed to run: during an
// intermittent hang that may take a dozen project switches to provoke. If the
// capture path is silently broken, you find out by reproducing a rare bug and
// getting nothing -- the single most expensive way to learn it. So the walk, the
// minidump, and the watchdog trigger are all exercised here, in the ordinary
// gate, where a regression costs one test run instead of one lost repro.
//
// GPU crash diagnostics arc, Task 4: the .arcdiag emission + GPU-section
// provider seam get the same treatment -- CPU-only here too (the provider in
// these tests is a fake, no GPU calls), covering all three provider states
// (installed / never installed / installed-then-cleared) plus the pinned CPU
// path (every .arcdiag is fully renderable on its own).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // GpuFrameSlot -- stamped bookkeeping, testable with no device

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
    // Install() is idempotent by design (a second call is a no-op), so every
    // test here must fully disarm or the next one silently inherits the first
    // one's config. RAII rather than a trailing call: a failing REQUIRE
    // unwinds, and a leaked armed watchdog would outlive the test.
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
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "arcane-diag-test" / leaf;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::string ReadWholeFile(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // RAII for the GPU-section provider slot: same reasoning as
    // ArmedDiagnostics above -- it is process-global state and the suite
    // runs in random order, so a test that installs a provider must clear
    // it before the next test runs, even if an assertion fails first.
    struct ArmedGpuProvider
    {
        ArmedGpuProvider(Arcane::Diagnostics::GpuSectionProvider fn, void* user)
        {
            Arcane::Diagnostics::SetGpuSectionProvider(fn, user);
        }
        ~ArmedGpuProvider() { Arcane::Diagnostics::ClearGpuSectionProvider(); }

        ArmedGpuProvider(const ArmedGpuProvider&)            = delete;
        ArmedGpuProvider& operator=(const ArmedGpuProvider&) = delete;
    };

    std::filesystem::path SiblingWithExt(const std::filesystem::path& txtPath, const char* ext)
    {
        std::filesystem::path p(txtPath);
        p.replace_extension(ext);
        return p;
    }

    // RAII for a live RetargetDumpDir call (GPU crash diagnostics arc, Task
    // 8): Config::dumpDir is process-global (g_cfg) and Shutdown() does NOT
    // reset it (by design -- see Shutdown's own comment), so a test that
    // retargets it live must put it back itself, even if a REQUIRE below
    // fails first -- same reasoning as ArmedGpuProvider above, and doubly so
    // here since the suite runs in random order and the NEXT test's own
    // ArmedDiagnostics assumes whatever dumpDir ITS OWN cfg named.
    struct RetargetedDumpDir
    {
        explicit RetargetedDumpDir(std::filesystem::path restoreTo) : m_restoreTo(std::move(restoreTo)) {}
        ~RetargetedDumpDir() { Arcane::Diagnostics::RetargetDumpDir(m_restoreTo); }

        RetargetedDumpDir(const RetargetedDumpDir&)            = delete;
        RetargetedDumpDir& operator=(const RetargetedDumpDir&) = delete;

        std::filesystem::path m_restoreTo;
    };

    // Fake GPU-section provider: no GPU calls, just proves the seam's data
    // flow -- envelope fields it sets round-trip through WriteFile/ReadFile,
    // humanText lands in the .txt's "=== GPU ===" block, and reportStem is
    // the real sibling-path stem (a real backend would write
    // <reportStem>.gpudump against exactly this path).
    void FakeGpuSectionProvider(Arcane::Diag::Envelope& envelope, std::string& humanText,
                                 const std::filesystem::path& reportStem, void* user)
    {
        if (auto* calls = static_cast<int*>(user)) ++(*calls);

        envelope.queues.push_back({ "direct", "pass:tonemap", { "pass:imgui" } });
        envelope.fault = { "page-fault", "0xDEADBEEF0000", "TestResource" };
        envelope.activeLayers = { "breadcrumbs:pass" };
        envelope.siblingGpuDump = reportStem.string() + ".gpudump";

        humanText = "queue direct: last completed pass:tonemap, in-flight pass:imgui";
    }
}

TEST_CASE("Diagnostics writes a symbolized all-thread report on demand", "[diag]")
{
    const std::filesystem::path dir = FreshReportDir("ondemand");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;   // never hijack the suite's own fault handling
    cfg.startHangWatchdog   = false;   // this case drives WriteReport directly

    const std::uint32_t before = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);
    Arcane::Diagnostics::SetPhase("unit-test-phase");

    const std::string txtPath = Arcane::Diagnostics::WriteReport("unit-test-reason");
    REQUIRE_FALSE(txtPath.empty());
    REQUIRE(std::filesystem::exists(txtPath));

    CHECK(Arcane::Diagnostics::ReportCount() == before + 1);

    const std::string body = ReadWholeFile(txtPath);
    CHECK(body.find("=== Arcane diagnostic report ===") != std::string::npos);
    CHECK(body.find("unit-test-reason") != std::string::npos);
    // The phase label is the field that turns "the main thread is stuck" into
    // "stuck in <stage>" -- the whole reason SetPhase exists.
    CHECK(body.find("unit-test-phase") != std::string::npos);

    // The walk actually produced thread sections, and tagged the registered
    // main thread. Without this the report could be a well-formed header over
    // no stacks at all, which is the failure mode that costs a repro.
    CHECK(body.find("--- thread") != std::string::npos);
    CHECK(body.find("(MAIN)") != std::string::npos);

    // At least one resolved frame. "module!..." is emitted whenever the module
    // base resolved, so this holds even where PDBs are absent and symbol names
    // degrade -- it proves StackWalk64 walked, rather than asserting on symbol
    // quality that legitimately varies by configuration.
    CHECK(body.find('!') != std::string::npos);
    CHECK(body.find("<no frames recovered>") == std::string::npos);

    // The minidump is the artifact a debugger opens; the .txt is the one a
    // human reads. Losing either one quietly is a real regression.
    std::filesystem::path dmp(txtPath);
    dmp.replace_extension(".dmp");
    REQUIRE(std::filesystem::exists(dmp));
    std::error_code ec;
    CHECK(std::filesystem::file_size(dmp, ec) > 0u);
}

TEST_CASE("Diagnostics RetargetDumpDir switches the live write location", "[diag]")
{
    // Two DISTINCT dirs -- the whole point is proving the report set follows
    // the retarget rather than the dir Install() was originally armed with.
    const std::filesystem::path originalDir = FreshReportDir("retarget-original");
    const std::filesystem::path newDir      = FreshReportDir("retarget-new");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = originalDir.string();
    cfg.installCrashHandler = false;   // never hijack the suite's own fault handling
    cfg.startHangWatchdog   = false;   // this case drives WriteReport directly

    ArmedDiagnostics armed(cfg);
    // Constructed AFTER armed, so it destructs BEFORE armed's own Shutdown()
    // -- the restore runs first, then Shutdown() disarms on top of it.
    RetargetedDumpDir restore(originalDir);

    Arcane::Diagnostics::RetargetDumpDir(newDir);

    const std::string txtPath = Arcane::Diagnostics::WriteReport("retarget-test-reason");
    REQUIRE_FALSE(txtPath.empty());
    REQUIRE(std::filesystem::exists(txtPath));
    CHECK(std::filesystem::path(txtPath).parent_path() == newDir);

    // The .arcdiag sibling follows the retarget too -- both files WriteReport
    // mints share ReportDir()'s one call site (F-6b).
    const std::filesystem::path diagPath = SiblingWithExt(txtPath, ".arcdiag");
    REQUIRE(std::filesystem::exists(diagPath));
    CHECK(diagPath.parent_path() == newDir);

    // And nothing landed in the dir Install() originally named -- the switch
    // is a real retarget, not an additional write location.
    std::error_code ec;
    const bool originalHasEntries =
        std::filesystem::directory_iterator(originalDir, ec) != std::filesystem::directory_iterator{};
    CHECK_FALSE(originalHasEntries);
}

TEST_CASE("Diagnostics hang watchdog fires when the main thread stops beating", "[diag]")
{
#if defined(_WIN32)
    // A debugger attached suppresses the watchdog on purpose (a breakpoint
    // stops the main thread by design, and crying wolf trains people to ignore
    // the one signal that matters). Under one, there is nothing to assert.
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    const std::filesystem::path dir = FreshReportDir("watchdog");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.hangSeconds         = 1;      // the shipped default is 12; 1 keeps the gate quick
    cfg.startHangWatchdog   = true;

    const std::uint32_t before = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);

    // One beat arms the trigger. Until a first Heartbeat() the watchdog stays
    // deliberately silent, so that a host which never beats gets silence rather
    // than a spurious report -- assert that contract holds before relying on it.
    CHECK(Arcane::Diagnostics::ReportCount() == before);

    Arcane::Diagnostics::SetPhase("watchdog-test-phase");
    Arcane::Diagnostics::Heartbeat();

    // Then stop beating: this thread sleeping IS the simulated hang. The
    // watchdog polls at 250ms, so ~1s threshold + slack.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    REQUIRE(Arcane::Diagnostics::ReportCount() > before);

    // And it wrote a real report, not just a counter bump.
    bool foundReport = false;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (e.path().extension() != ".txt") continue;
        const std::string body = ReadWholeFile(e.path());
        if (body.find("hang (main thread has not ticked") != std::string::npos &&
            body.find("watchdog-test-phase") != std::string::npos)
        {
            foundReport = true;
            break;
        }
    }
    CHECK(foundReport);
}

TEST_CASE("Diagnostics watchdog re-arms and does not spam a single stall", "[diag]")
{
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    const std::filesystem::path dir = FreshReportDir("rearm");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.hangSeconds         = 1;
    cfg.startHangWatchdog   = true;

    // Elastic wait, exact assertion: a loaded shared runner (hosted CI) can
    // take several seconds to symbolize + write the first report, so a fixed
    // post-stall sleep captures afterFirstStall BEFORE the report lands and
    // the no-spam check below trips on the report's late arrival (2-for-2 on
    // GitHub-hosted runners, 2026-08-11). Poll for the count instead; only
    // the WAIT is generous, one-report-per-stall stays exact.
    const auto waitForCount = [](std::uint32_t target, std::chrono::seconds cap)
    {
        const auto deadline = std::chrono::steady_clock::now() + cap;
        while (Arcane::Diagnostics::ReportCount() < target)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    };

    const std::uint32_t base = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);
    Arcane::Diagnostics::Heartbeat();

    // No heartbeats from here on -- this thread IS the stall. The poll sleeps
    // are on the test thread, not the watchdog's, so they do not feed it.
    REQUIRE(waitForCount(base + 1, std::chrono::seconds(15)));
    const std::uint32_t afterFirstStall = Arcane::Diagnostics::ReportCount();

    // Still stalled, well past another threshold: ONE stall must yield ONE
    // report. A watchdog that re-reports every poll buries the first (and most
    // informative) stack under hundreds of duplicates.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(Arcane::Diagnostics::ReportCount() == afterFirstStall);

    // Main thread recovers, then stalls again: that is a NEW stall and must be
    // reported. Otherwise the first hang of a session would be the only one
    // ever captured.
    Arcane::Diagnostics::Heartbeat();
    REQUIRE(waitForCount(afterFirstStall + 1, std::chrono::seconds(15)));
    CHECK(Arcane::Diagnostics::ReportCount() > afterFirstStall);
}

TEST_CASE("Diagnostics emits an .arcdiag with a GPU section when a provider is installed", "[diag]")
{
    const std::filesystem::path dir = FreshReportDir("arcdiag-with-provider");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;

    ArmedDiagnostics armed(cfg);

    int providerCalls = 0;
    ArmedGpuProvider provider(&FakeGpuSectionProvider, &providerCalls);

    const std::string txtPath = Arcane::Diagnostics::WriteReport("gpu-crash: test");
    REQUIRE_FALSE(txtPath.empty());
    CHECK(providerCalls == 1);

    const std::string body = ReadWholeFile(txtPath);
    CHECK(body.find("=== GPU ===") != std::string::npos);

    const std::filesystem::path diagPath = SiblingWithExt(txtPath, ".arcdiag");
    REQUIRE(std::filesystem::exists(diagPath));

    const auto env = Arcane::Diag::ReadFile(diagPath);
    REQUIRE(env.has_value());
    CHECK(env->guid.IsValid());
    CHECK(env->kind == "gpu-crash");
    CHECK(env->appName == "DiagTest");

    REQUIRE(env->queues.size() == 1);
    CHECK(env->queues[0].name == "direct");
    CHECK(env->queues[0].lastCompleted == "pass:tonemap");
    CHECK(env->fault.resource == "TestResource");

    CHECK(env->siblingTxt == txtPath);
    CHECK(env->siblingDmp == SiblingWithExt(txtPath, ".dmp").string());
    CHECK(env->siblingGpuDump.find(".gpudump") != std::string::npos);

    // CPU path pinned: every emitted .arcdiag -- with or without a provider
    // -- carries the same all-thread walk that feeds the .txt, and names the
    // registered main thread (mirrors the existing "(MAIN)" .txt assertion
    // in the first TEST_CASE above).
    CHECK_FALSE(env->cpuThreadSummary.empty());
    CHECK(env->cpuThreadSummary.find("(MAIN)") != std::string::npos);
    // The GPU block is a .txt-only convenience appended AFTER the walk;
    // cpuThreadSummary is snapshotted before that and stays CPU-only.
    CHECK(env->cpuThreadSummary.find("=== GPU ===") == std::string::npos);
}

TEST_CASE("Diagnostics emits an .arcdiag with an empty GPU section when no provider is installed", "[diag]")
{
    const std::filesystem::path dir = FreshReportDir("arcdiag-no-provider");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;

    // Defensive: the provider slot is process-global and the suite runs in
    // random order (project_arcanetests_random_order_typecontext) -- do not
    // rely solely on another test's RAII having already unwound.
    Arcane::Diagnostics::ClearGpuSectionProvider();

    ArmedDiagnostics armed(cfg);

    const std::string txtPath = Arcane::Diagnostics::WriteReport("unit-test-reason-no-provider");
    REQUIRE_FALSE(txtPath.empty());

    const std::string body = ReadWholeFile(txtPath);
    CHECK(body.find("=== GPU ===") == std::string::npos);

    const std::filesystem::path diagPath = SiblingWithExt(txtPath, ".arcdiag");
    REQUIRE(std::filesystem::exists(diagPath));

    const auto env = Arcane::Diag::ReadFile(diagPath);
    REQUIRE(env.has_value());
    CHECK(env->guid.IsValid());
    CHECK(env->kind == "crash");
    CHECK(env->queues.empty());
    CHECK(env->fault.type.empty());
    CHECK(env->siblingGpuDump.empty());

    CHECK_FALSE(env->cpuThreadSummary.empty());
    CHECK(env->cpuThreadSummary.find("(MAIN)") != std::string::npos);
}

TEST_CASE("Diagnostics omits the GPU section once a provider is cleared", "[diag]")
{
    const std::filesystem::path dir = FreshReportDir("arcdiag-cleared-provider");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;

    ArmedDiagnostics armed(cfg);

    int providerCalls = 0;
    {
        ArmedGpuProvider provider(&FakeGpuSectionProvider, &providerCalls);
        // provider clears itself at the end of this scope
    }
    CHECK(providerCalls == 0);   // never invoked -- no WriteReport happened yet

    // Kind derivation reads the REASON string, not provider presence -- keep
    // a "gpu-crash" reason here to prove that distinction: the kind survives
    // the provider going away, the GPU fields do not.
    const std::string txtPath = Arcane::Diagnostics::WriteReport("gpu-crash: cleared-provider");
    REQUIRE_FALSE(txtPath.empty());
    CHECK(providerCalls == 0);

    const std::string body = ReadWholeFile(txtPath);
    CHECK(body.find("=== GPU ===") == std::string::npos);

    const std::filesystem::path diagPath = SiblingWithExt(txtPath, ".arcdiag");
    REQUIRE(std::filesystem::exists(diagPath));

    const auto env = Arcane::Diag::ReadFile(diagPath);
    REQUIRE(env.has_value());
    CHECK(env->kind == "gpu-crash");
    CHECK(env->queues.empty());
    CHECK(env->siblingGpuDump.empty());
}

TEST_CASE("Diagnostics kind derivation covers gpu-stall, not just gpu-crash", "[diag]")
{
    // Task 7 (not yet wired) reports a GPU-progress stall via exactly
    // WriteReport("gpu-stall") -- see docs/specs/2026-08-11-gpu-crash-
    // diagnostics-design.md. "gpu" alone must not collapse both gpu kinds
    // into "gpu-crash".
    const std::filesystem::path dir = FreshReportDir("arcdiag-gpu-stall-kind");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;

    ArmedDiagnostics armed(cfg);

    const std::string txtPath = Arcane::Diagnostics::WriteReport("gpu-stall");
    REQUIRE_FALSE(txtPath.empty());

    const auto env = Arcane::Diag::ReadFile(SiblingWithExt(txtPath, ".arcdiag"));
    REQUIRE(env.has_value());
    CHECK(env->kind == "gpu-stall");
}

// =============================================================================
// GPU-progress watchdog (GPU crash diagnostics arc, Task 7)
// =============================================================================
// The staleness DECISION is extracted into Arcane::Diagnostics::ProgressStallRule
// so it can be driven by hand -- no threads, no wall clock, no GPU. That matters
// twice over: the rule is the part that can be wrong (one report per stall,
// re-armed on progress), and the 2026-08-11 hosted-CI flake showed that timing
// a watchdog against a deadline is how a correct rule ships a red build. Here
// the clock is a parameter, so these cases are exact rather than generous.

TEST_CASE("Diagnostics GPU stall rule fires once per stall and re-arms on progress", "[diag]")
{
    Arcane::Diagnostics::ProgressStallRule rule(8.0);

    // First poll only seeds: a rule that fired on its first observation would
    // report a "stall" the instant the watchdog started.
    CHECK_FALSE(rule.Poll(100, 0.0));

    // Same value, but not yet long enough.
    CHECK_FALSE(rule.Poll(100, 4.0));
    CHECK_FALSE(rule.Poll(100, 7.9));

    // Threshold reached: exactly one true.
    CHECK(rule.Poll(100, 8.0));

    // Still stalled, arbitrarily far past the threshold: a watchdog that
    // re-reports every poll buries the first (and most informative) capture.
    CHECK_FALSE(rule.Poll(100, 8.25));
    CHECK_FALSE(rule.Poll(100, 60.0));

    // The GPU moved again: re-armed, and the stall clock restarts from HERE
    // (not from the original value's first sighting).
    CHECK_FALSE(rule.Poll(101, 60.5));
    CHECK_FALSE(rule.Poll(101, 68.4));

    // A NEW stall is a new report. Otherwise the first hang of a session would
    // be the only one ever captured.
    CHECK(rule.Poll(101, 68.5));
    CHECK_FALSE(rule.Poll(101, 69.0));
}

TEST_CASE("Diagnostics GPU stall rule counts one report per stall across many polls", "[diag]")
{
    // The shape the watchdog thread actually runs: a fixed poll cadence over a
    // long stall, then recovery, then another stall. Poll-driven rather than
    // deadline-driven on purpose (2026-08-11 flake fix) -- the cadence here is
    // simulated, so a loaded runner cannot change the answer.
    constexpr double kPollSeconds  = 0.25;
    constexpr double kStallSeconds = 2.0;

    Arcane::Diagnostics::ProgressStallRule rule(kStallSeconds);

    std::uint64_t fence   = 7;
    int           reports = 0;
    double        now     = 0.0;

    // 40 polls (10s) with the fence frozen: one report, not forty.
    for (int i = 0; i < 40; ++i, now += kPollSeconds)
        if (rule.Poll(fence, now)) ++reports;
    CHECK(reports == 1);
    CHECK(rule.StalledSeconds(now) >= kStallSeconds);

    // The fence advances every poll for a while: nothing reported, and the
    // rule reports the stall age as ~0 while progress continues.
    for (int i = 0; i < 20; ++i, now += kPollSeconds)
        if (rule.Poll(++fence, now)) ++reports;
    CHECK(reports == 1);
    CHECK(rule.StalledSeconds(now) < kStallSeconds);

    // Frozen again: exactly one more.
    for (int i = 0; i < 40; ++i, now += kPollSeconds)
        if (rule.Poll(fence, now)) ++reports;
    CHECK(reports == 2);
}

TEST_CASE("Diagnostics GPU stall rule discards the stall clock on Reset", "[diag]")
{
    // Reset is what the watchdog runs when the render path stops publishing at
    // all. The value is unchanged across it -- so a rule that only tracked the
    // VALUE would fire the instant publishing resumed, blaming the GPU for a
    // gap in which nothing was expected to move.
    Arcane::Diagnostics::ProgressStallRule rule(2.0);

    CHECK_FALSE(rule.Poll(9, 0.0));
    CHECK_FALSE(rule.Poll(9, 1.9));

    rule.Reset();
    CHECK(rule.StalledSeconds(100.0) == 0.0);

    // 100s later, same value: the first poll after a Reset only re-seeds.
    CHECK_FALSE(rule.Poll(9, 100.0));
    CHECK_FALSE(rule.Poll(9, 101.9));

    // ...and the clock runs from the RESUME, not from the original sighting.
    CHECK(rule.Poll(9, 102.0));
}

TEST_CASE("Diagnostics GPU watchdog writes a gpu-stall report when the fence stops advancing", "[diag]")
{
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    const std::filesystem::path dir = FreshReportDir("gpu-stall");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.gpuStallSeconds     = 1;      // the shipped default is 8; 1 keeps the gate quick
    cfg.startHangWatchdog   = true;

    // Elastic wait, exact assertion -- same poll-not-deadline discipline as the
    // CPU re-arm case above (a loaded shared runner can take seconds to
    // symbolize + write a report). Publishing `fence` on every poll IS the
    // simulated render path: the rule's signal is "still being published, still
    // not moving", so a test that published once and slept would be simulating a
    // host that stopped rendering instead -- a case the rule deliberately
    // ignores.
    const auto pumpUntilCount = [](std::uint64_t fence, std::uint32_t target,
                                   std::chrono::seconds cap)
    {
        const auto deadline = std::chrono::steady_clock::now() + cap;
        while (Arcane::Diagnostics::ReportCount() < target)
        {
            Arcane::Diagnostics::GpuHeartbeat(fence);
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    };

    const auto pumpFor = [](std::uint64_t fence, std::chrono::milliseconds span)
    {
        const auto until = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < until)
        {
            Arcane::Diagnostics::GpuHeartbeat(fence);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    const std::uint32_t base = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);

    // No GPU heartbeat yet: like the CPU trigger, the GPU rule stays silent
    // until the render path has published at least one value, so a headless
    // host that never renders gets silence rather than a spurious report.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(Arcane::Diagnostics::ReportCount() == base);

    // A frame loop that keeps running while the GPU stops retiring: the value
    // republished every poll, never advancing. This thread deliberately never
    // calls Heartbeat(), so the CPU trigger stays disarmed and the only report
    // that can appear is the GPU one.
    REQUIRE(pumpUntilCount(42, base + 1, std::chrono::seconds(20)));
    const std::uint32_t afterFirstStall = Arcane::Diagnostics::ReportCount();

    // One stall, one report -- still stalled, still publishing.
    pumpFor(42, std::chrono::milliseconds(1500));
    CHECK(Arcane::Diagnostics::ReportCount() == afterFirstStall);

    // The report says gpu-stall, which is what Diagnostics::DeriveKind turns
    // into the .arcdiag's "gpu-stall" kind -- the whole reason the GPU section
    // provider engages for this report instead of it reading as a CPU hang.
    bool foundReport = false;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (e.path().extension() != ".arcdiag") continue;
        const auto env = Arcane::Diag::ReadFile(e.path());
        if (env && env->kind == "gpu-stall") { foundReport = true; break; }
    }
    CHECK(foundReport);

    // The GPU moved again, then stalled again: a NEW stall is a NEW report.
    REQUIRE(pumpUntilCount(43, afterFirstStall + 1, std::chrono::seconds(20)));
}

TEST_CASE("GpuFrameSlot never claims a stamp that did not go out", "[diag]")
{
    // The invariant the whole poll/wait split rests on. nvrhi's pollEventQuery
    // reports an UNSTAMPED query as incomplete FOREVER (Vulkan:
    // Queue::pollCommandList returns false for commandListID == 0; D3D12:
    // !started), while waitEventQuery returns from that same state instantly.
    // So a slot that wrongly believes it is stamped sends the wait into a poll
    // loop nvrhi will never satisfy -- a multi-second host freeze plus a
    // gpu-stall report blaming the GPU for a frame that never submitted
    // anything. Reachable on Vulkan via an ordinary window resize
    // (OutOfDateKHRError bails BeginFrame before Present ever stamps).
    //
    // No device here on purpose: a slot whose query could not be created must
    // behave like an unstamped slot, and that is exactly what a null device
    // produces. The GPU-side half of this is desk-covered ([gpu] PacingTest /
    // SwapchainTest, plus the resize storm in the desk battery).
    Arcane::GpuFrameSlot slot;

    // Fresh: nothing stamped.
    CHECK_FALSE(slot.IsStamped());

    // A failed Init leaves it unstamped rather than half-armed.
    CHECK_FALSE(slot.Init(nullptr));
    CHECK_FALSE(slot.IsStamped());

    // Stamp with no device: the stamp did NOT go out, so the flag must not
    // claim it did. This is the assertion that keeps the poll loop honest.
    slot.Stamp(nullptr, nvrhi::CommandQueue::Graphics);
    CHECK_FALSE(slot.IsStamped());

    // And the wait over an unstamped/queryless slot returns rather than
    // polling. If this ever hangs, the regression is back.
    slot.WaitAndReset(nullptr);
    CHECK_FALSE(slot.IsStamped());
}

TEST_CASE("Diagnostics GPU watchdog fires while the render path is parked in a frame-slot wait", "[diag]")
{
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    // THE case the rule exists for, and the one it could not see before
    // GpuFrameSlot's polling wait: a wedged GPU parks the main thread in the
    // swapchain's slot wait, so no new counter value can ever be published --
    // the host is blocked producing one. A blocking wait made that
    // indistinguishable from "not rendering", the freshness gate disarmed on
    // it, and gpu-stall was unreachable in both hosts. The polling wait
    // republishes GpuHeartbeatRefresh() (freshness, same value) and
    // Heartbeat() (this thread is alive and waiting on purpose) each
    // iteration; this test IS that loop, minus the GPU.
    const std::filesystem::path dir = FreshReportDir("gpu-stall-slot-wait");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.gpuStallSeconds     = 1;
    // The shipped ordering is gpuStallSeconds < hangSeconds so the GPU rule
    // names the cause first. Keep that relationship rather than letting a hang
    // report race in and pass this test for the wrong reason.
    cfg.hangSeconds         = 3600;
    cfg.startHangWatchdog   = true;

    const std::uint32_t base = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);

    // One value published (the last frame that completed), then the "wait
    // loop": refresh-only, never a new value.
    Arcane::Diagnostics::GpuHeartbeat(4242);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (Arcane::Diagnostics::ReportCount() < base + 1 &&
           std::chrono::steady_clock::now() < deadline)
    {
        Arcane::Diagnostics::Heartbeat();
        Arcane::Diagnostics::GpuHeartbeatRefresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(Arcane::Diagnostics::ReportCount() == base + 1);

    // And it is a gpu-stall, not a hang: that distinction is the entire claim
    // the report makes about the cause.
    bool foundGpuStall = false;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (e.path().extension() != ".arcdiag") continue;
        const auto env = Arcane::Diag::ReadFile(e.path());
        if (env && env->kind == "gpu-stall") { foundGpuStall = true; break; }
    }
    CHECK(foundGpuStall);
}

TEST_CASE("Diagnostics GpuHeartbeatRefresh alone never arms the GPU rule", "[diag]")
{
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    // Refresh carries no value -- it only says "same counter, still watching".
    // If it armed the rule, a host that reached a frame-slot wait before ever
    // completing a frame (a device that never retired anything) would be
    // reported as stalled on a counter nobody ever published.
    const std::filesystem::path dir = FreshReportDir("gpu-refresh-only");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.gpuStallSeconds     = 1;
    cfg.hangSeconds         = 3600;
    cfg.startHangWatchdog   = true;

    const std::uint32_t base = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);

    // Refresh only -- GpuHeartbeat is never called, so no value ever exists.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < until)
    {
        Arcane::Diagnostics::GpuHeartbeatRefresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(Arcane::Diagnostics::ReportCount() == base);
}

TEST_CASE("Diagnostics arming discards beats published before Install", "[diag]")
{
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    // Heartbeat()/GpuHeartbeat() are unconditional stores -- they do not check
    // whether diagnostics are installed, on purpose (they sit on the hot frame
    // path). So a beat can be sitting in the globals, arbitrarily old, when a
    // host arms. If arming inherited it, the watchdog would file a hang report
    // seconds later for something that happened before it existed. This is not
    // hypothetical: it is what a suite running in random order does to itself,
    // and what a host that beats during boot before Install() would do in
    // production.
    const std::filesystem::path dir = FreshReportDir("stale-beat");

    Arcane::Diagnostics::Heartbeat();
    Arcane::Diagnostics::GpuHeartbeat(1234);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.hangSeconds         = 1;
    cfg.gpuStallSeconds     = 1;
    cfg.startHangWatchdog   = true;

    const std::uint32_t base = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);

    // Both thresholds are 1s and the pre-Install beats are already older than
    // that, so an inherited beat reports on the very first poll.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    CHECK(Arcane::Diagnostics::ReportCount() == base);
}

TEST_CASE("Diagnostics GPU watchdog stays silent when the host stops rendering", "[diag]")
{
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        SUCCEED("watchdog is suppressed under a debugger by design");
        return;
    }
#endif

    // A minimized host renders no frames: the counter freezes because nothing
    // is being submitted, not because the GPU stopped retiring. Publishing
    // stops with it, and that -- not the frozen value -- is what tells the rule
    // there is no evidence here. Without this gate, minimizing a window for
    // gpuStallSeconds would file a GPU crash report.
    const std::filesystem::path dir = FreshReportDir("gpu-stall-not-rendering");

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.gpuStallSeconds     = 1;
    // Takes the CPU rule out of the picture for the whole run: this case
    // asserts that NO report appears, and the two rules share one watchdog
    // thread, so a hang report would read as a GPU-rule failure it isn't.
    cfg.hangSeconds         = 3600;
    cfg.startHangWatchdog   = true;

    const std::uint32_t base = Arcane::Diagnostics::ReportCount();

    ArmedDiagnostics armed(cfg);

    // One frame's worth of publishing, then the host "minimizes": no further
    // GpuHeartbeat calls, for many multiples of the stall window.
    Arcane::Diagnostics::GpuHeartbeat(7);
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    CHECK(Arcane::Diagnostics::ReportCount() == base);
}
