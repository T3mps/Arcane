// Arcane::Diagnostics -- the crash/hang post-mortem capture path. CPU-only ([diag]).
//
// These tests exist because of WHEN this code is supposed to run: during an
// intermittent hang that may take a dozen project switches to provoke. If the
// capture path is silently broken, you find out by reproducing a rare bug and
// getting nothing -- the single most expensive way to learn it. So the walk, the
// minidump, and the watchdog trigger are all exercised here, in the ordinary
// gate, where a regression costs one test run instead of one lost repro.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

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

    ArmedDiagnostics armed(cfg);
    Arcane::Diagnostics::Heartbeat();

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    CHECK(Arcane::Diagnostics::ReportCount() > afterFirstStall);
}
