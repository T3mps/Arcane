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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>

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
