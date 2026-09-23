// The crash path (crash window plan 1, task 5): a report runs on a DEDICATED
// crash thread, in UE's order -- backlog frozen, minimal envelope, minidump,
// module+offset text, GPU provider, full envelope, backlog dump, hand-off --
// while the submitting thread only signals, waits and (for a fatal report)
// exits.
//
// Device-free on purpose: everything asserted here is CPU-side and observable
// from the files the report leaves behind, so the freeze this whole arc exists
// to remove is covered in the ordinary [diag] gate rather than on a desk with a
// wedged GPU.

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>

#include "Helpers/HostWitness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Same RAII discipline as DiagnosticsTest's ArmedDiagnostics: Install() is
    // idempotent, so a case that does not disarm leaks its config into the next
    // one under Catch2's random ordering.
    struct Armed
    {
        explicit Armed(const std::filesystem::path& dir)
        {
            // R16: Diagnostics::Install does NOT bootstrap the engine logger
            // (Log::Init is call_once and the FIRST caller's level wins, so
            // bootstrapping there would pin the whole process to the default
            // level). Hosts call Log::Init() before Install -- so the tests
            // do too, which is also what makes the backlog assertion below
            // order-independent: no logger means no file sink, and no file
            // sink means no backlog sink to record into. Init is a once-latch,
            // so this is harmless when an earlier case already ran it.
            Arcane::Log::Init();

            Arcane::Diagnostics::Config cfg;
            cfg.appName = "CrashPathTest";
            cfg.dumpDir = dir.string();
            cfg.unattended = true;
            cfg.spawnReporter = false;
            cfg.startHangWatchdog = false;
            // Never hijack the suite's own fault handling (controller note R2;
            // every existing [diag] case does the same). The crash thread and
            // its events are created regardless -- they are the report engine,
            // not the handler.
            cfg.installCrashHandler = false;
            Arcane::Diagnostics::Install(cfg);
        }
        ~Armed() { Arcane::Diagnostics::Shutdown(); }

        Armed(const Armed&)            = delete;
        Armed& operator=(const Armed&) = delete;
    };

    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // RAII for the process-global GPU-section provider slot -- same reasoning
    // as DiagnosticsTest's ArmedGpuProvider: the suite runs in random order,
    // so a case that installs one must clear it even if an assertion unwinds
    // first, or the NEXT report (or the watchdog, on its own thread) runs a
    // provider pointing at this case's dead stack.
    struct ArmedProvider
    {
        ArmedProvider(Arcane::Diagnostics::GpuSectionProvider fn, void* user)
        {
            Arcane::Diagnostics::SetGpuSectionProvider(fn, user);
        }
        ~ArmedProvider() { Arcane::Diagnostics::ClearGpuSectionProvider(); }

        ArmedProvider(const ArmedProvider&)            = delete;
        ArmedProvider& operator=(const ArmedProvider&) = delete;
    };

    // A provider that returns far more than the envelope's arena reserve can
    // hold: ~400 queues of ~600 bytes each, plus 200 long activeLayers, is
    // several hundred KiB against a 64 KiB reserve. Nothing here is a GPU
    // call -- the point is purely the SIZE the provider hands back, which is
    // the one input to this path a backend controls and Core cannot bound.
    void FloodingGpuSectionProvider(Arcane::Diag::Envelope& envelope, std::string& humanText,
                                    const std::filesystem::path&, void* user)
    {
        if (auto* calls = static_cast<int*>(user)) ++(*calls);

        for (int i = 0; i < 400; ++i)
        {
            envelope.queues.push_back({ std::string(200, 'q'),
                                        std::string(200, 'c'),
                                        { std::string(200, 'f') } });
        }
        envelope.activeLayers.assign(200, std::string(200, 'L'));
        envelope.fault = { "page-fault", "0xDEADBEEF0000", "FloodResource" };

        humanText = "flooded";
    }
}

TEST_CASE("crash path: a manual report runs on the crash thread, writes envelope before dump, "
          "a module+offset stack, and the log backlog", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Armed armed(dir);

    ARC_WARN("a line the backlog must carry");
    // exitCode 0: survivable. A fatal report would TerminateProcess here.
    Arcane::Diagnostics::SubmitReport({ "hang (test)", nullptr, false, 0 });

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    REQUIRE(std::filesystem::exists(stem + ".arcdiag"));
    REQUIRE(std::filesystem::exists(stem + ".dmp"));
    REQUIRE(std::filesystem::exists(stem + ".txt"));
    REQUIRE(std::filesystem::exists(stem + ".log.txt"));

    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    // Plan 2 (D2): the reason rides in the envelope so the reporter reads one file.
    CHECK(env->reason == "hang (test)");
    CHECK(env->siblingDmp.empty() == false);

    const std::string txt = Slurp(stem + ".txt");
    // Portable frames name modules, and nothing on this path symbolizes any
    // more -- no DbgHelp, so no "module!function" line can appear.
    CHECK(txt.find("ArcaneCore.dll + 0x") != std::string::npos);
    CHECK(txt.find("!") == std::string::npos);
    CHECK(Slurp(stem + ".log.txt").find("a line the backlog must carry") != std::string::npos);
}

TEST_CASE("crash path: a lightweight (ensure) report writes envelope and text only and returns", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test-ensure";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Armed armed(dir);

    Arcane::Diagnostics::SubmitReport({ "ensure: index < count", nullptr, /*lightweight*/true, 0 });

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    CHECK(std::filesystem::exists(stem + ".arcdiag"));
    CHECK_FALSE(std::filesystem::exists(stem + ".dmp"));

    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "ensure");
}

TEST_CASE("crash path: an envelope that cannot fit the arena is elided or withheld, never left "
          "truncated on disk", "[diag]")
{
    // THE FAILURE THIS PINS. The envelope is hand-written into a fixed arena
    // span, and CrashArena::Builder::Append copies min(remaining, size) and
    // only flags the arena -- so an overrun is SILENTLY TRUNCATED,
    // syntactically invalid JSON. The full envelope is then moved over the
    // minimal one with MOVEFILE_REPLACE_EXISTING, which would destroy the
    // only parseable report on disk. The GPU-section provider is the one
    // input to that path Core cannot bound (a real backend returns however
    // many queues, in-flight passes and layers the device has), so that is
    // where the overrun is driven from here.
    const auto dir = std::filesystem::temp_directory_path() / "arcane-crash-path-test-flood";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    Armed armed(dir);

    int providerCalls = 0;
    ArmedProvider provider(&FloodingGpuSectionProvider, &providerCalls);

    Arcane::Diagnostics::SubmitReport({ "hang (envelope flood)", nullptr, false, 0 });
    CHECK(providerCalls == 1);

    const std::string stem = Arcane::Diagnostics::LastReportStem();
    REQUIRE_FALSE(stem.empty());
    REQUIRE(std::filesystem::exists(stem + ".arcdiag"));

    // THE ASSERTION: whatever landed, it PARSES. Either the full envelope was
    // withheld and the minimal one stands, or the rebuild with the unbounded
    // fields elided replaced it -- both are valid JSON, and a truncated file
    // is neither.
    const auto env = Arcane::Diag::ReadFile(stem + ".arcdiag");
    REQUIRE(env.has_value());

    // ...and it is still the RIGHT report: kind and the sibling paths survive
    // elision, because they are what a reader acts on.
    CHECK(env->kind == "hang");
    CHECK(env->guid.IsValid());
    CHECK(env->siblingDmp.empty() == false);

    // The stack is never lost by eliding -- it lives in the .txt sibling the
    // envelope names, which is written before any of this.
    const std::string txt = Slurp(stem + ".txt");
    CHECK(txt.find(" + 0x") != std::string::npos);

    // No unparseable temp is left lying around for a reporter to pick up.
    CHECK_FALSE(std::filesystem::exists(stem + ".arcdiag.tmp"));
}

namespace
{
    // Spec §6: no reporter spawn on a build machine (Install forces
    // spawnReporter=false there), so every case that needs a SPAWNED reporter
    // skips -- the desk gate is where these run.
    void SkipIfBuildMachine()
    {
        if ((std::getenv("CI") || std::getenv("ARCANE_BUILD_MACHINE")) && !std::getenv("ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
            SKIP("CI/ARCANE_BUILD_MACHINE set -- the reporter is never spawned on a build machine (spec §6; "
                 "ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE overrides)");
    }

    // The newest .arcdiag stem in `dir`, polling up to `timeout` -- a
    // detached reporter or monitor writes AFTER the fixture has exited.
    std::filesystem::path WaitForStem(const std::filesystem::path& dir, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::filesystem::path found;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".arcdiag") found = e.path().parent_path() / e.path().stem();
            if (!found.empty() || std::chrono::steady_clock::now() >= deadline) return found;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool WaitForFile(const std::filesystem::path& p, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!std::filesystem::exists(p) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return std::filesystem::exists(p);
    }

    // Runs the fixture with `--die <mode>` into a fresh dir; returns the run,
    // the newest .arcdiag stem and the directory itself. The `tag` derivation
    // gives each FLAG VARIANT its own directory ("arcane-death-av-reporter"),
    // so a `--reporter` run can never read a plain run's stem -- before this,
    // the three `--die none` cases all shared "arcane-death-none".
    struct FixtureRun { Arcane::Test::WitnessRun run; std::filesystem::path stem; std::filesystem::path dir; };
    FixtureRun RunFixture(const char* mode, std::vector<std::string> extra = {})
    {
        const auto exe = std::filesystem::absolute("../death-fixture/death-fixture.exe");
        REQUIRE(std::filesystem::exists(exe));
        std::string tag = mode;
        for (const auto& e : extra) if (e.rfind("--", 0) == 0) tag += "-" + e.substr(2);
        const auto dir = std::filesystem::temp_directory_path() / ("arcane-death-" + tag);
        std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
        std::vector<std::string> args = { "--dir", dir.string(), "--die", mode };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 30000;
        FixtureRun out{ Arcane::Test::RunWitness(inv), {}, dir };
        out.stem = WaitForStem(dir, std::chrono::milliseconds(0));
        return out;
    }
}

// The hand-off, end to end (spec §6): the fixture crashes, the crash thread
// spawns the STAGED reporter beside the fixture, the reporter reads the
// envelope and writes <stem>.symbolized.txt after the host is already dead.
TEST_CASE("death fixture --reporter: a crash report gains a .symbolized.txt from the detached reporter", "[diag]")
{
    SkipIfBuildMachine();
    REQUIRE(std::filesystem::exists(std::filesystem::absolute("../death-fixture/ArcaneCrashReporter.exe")));
    const FixtureRun r = RunFixture("av", { "--reporter" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    const auto sibling = r.stem.string() + ".symbolized.txt";
    REQUIRE(WaitForFile(sibling, std::chrono::seconds(20)));
    const std::string text = Slurp(sibling);
    CHECK(text.find("symbolized by ArcaneCrashReporter") != std::string::npos);
    CHECK(text.find("thread") != std::string::npos);
}

TEST_CASE("death fixture: an access violation yields a crash report and exit code 10 within the cap, with no dialog", "[diag]")
{
    const FixtureRun r = RunFixture("av");
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(std::filesystem::exists(r.stem.string() + ".dmp"));
    // Plan 2 (D9): a FATAL report's echo reaches the log file WITHOUT spdlog.
    // The fixture's logDir is derived: <dumpDir>/../Logs/<appName>.log.
    const auto log = std::filesystem::temp_directory_path() / "Logs" / "DeathFixture.log";
    REQUIRE(std::filesystem::exists(log));
    const std::string logText = Slurp(log);
    // The direct append lands in the same file spdlog is writing. This much
    // was green before D9 too -- ARC_ERROR used to carry the line...
    CHECK(logText.find("-- report written") != std::string::npos);
    // ...so the assertion that actually PINS D9 is that the echo is
    // UNPREFIXED. spdlog stamps every line it emits with
    // "[<ts>] [Arcane] [<level>] "; FatalEcho's WriteFile does not. An echo
    // that regressed to ARC_ERROR fails both of these -- the line would no
    // longer start at a newline, and an [error]-prefixed copy would appear.
    CHECK(logText.find("\nDiagnostics: crash (unhandled exception) -- report written")
          != std::string::npos);
    CHECK(logText.find("[error] Diagnostics: crash") == std::string::npos);
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "crash");
    CHECK(r.run.wallMs < 15000);
}

TEST_CASE("death fixture: a clean run exits 0 and writes nothing", "[diag]")
{
    const FixtureRun r = RunFixture("none");
    CHECK(r.run.exitCode == 0);
    CHECK(r.stem.empty());
}

// THE FAILURE THIS PINS (task 7). Every death that is NOT an SEH fault is
// invisible today: a failing ARC_ASSERT goes to Mosaic's abort(), an uncaught
// exception to std::terminate, a CRT contract violation to __fastfail -- none
// of them reach the crash thread, so none of them leave a report, and under a
// Debug CRT some of them stop on a modal box no unattended run can dismiss.
// The wall-time bound is part of the assertion, not decoration: a dialog is
// observable here ONLY as a run that burns the 30s cap.
TEST_CASE("death fixture: assert, terminate, abort, invalid parameter, pure call, stack overflow "
          "and OOM all yield a report with the right kind and exit 10, bounded", "[diag]")
{
    struct Row { const char* mode; const char* kind; };
    const Row rows[] = { {"assert","assert"}, {"terminate","terminate"}, {"abort","terminate"},
                         {"invalid-parameter","crash"}, {"purecall","crash"},
                         {"stack-overflow","crash"}, {"oom","out-of-memory"} };
    for (const Row& row : rows)
    {
        INFO("mode " << row.mode);
        const FixtureRun r = RunFixture(row.mode);
        CHECK_FALSE(r.run.timedOut);
        CHECK(r.run.exitCode == 10);
        REQUIRE_FALSE(r.stem.empty());
        CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == row.kind);
        CHECK(r.run.wallMs < 20000);
    }
}

// The one member of the family that must NOT kill the process: an ensure is a
// report you walk away from (spec S5.3, UE's continuable report). Envelope
// only -- no minidump -- and the fixture goes on to return 0.
TEST_CASE("death fixture: an ensure writes a lightweight report and the process continues to exit 0", "[diag]")
{
    const FixtureRun r = RunFixture("ensure");
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "ensure");
    CHECK_FALSE(std::filesystem::exists(r.stem.string() + ".dmp"));
}

// Spec S5.4, as a whole process: a hang report is SURVIVABLE. The watchdog
// writes it and hands the host straight back -- the fixture then finishes its
// sleep and leaves by its own `return 0`, WITHOUT calling Shutdown(), which is
// also what proves the raw watchdog thread + atexit hook (task 8) let a main()
// return cleanly where a joinable std::thread used to call std::terminate.
TEST_CASE("death fixture: a hang writes a hang report and the process stays alive until it exits on its own with 0", "[diag]")
{
    const FixtureRun r = RunFixture("none", { "--hang", "5", "--hang-seconds", "1" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "hang");
}

// Spec S5.7, the other half: from RequestCleanExit() onward the watchdog's beat
// rule is replaced by an exit deadline, and a host that never gets out is NAMED
// ("hang at exit") rather than left wedged on somebody's desk. The fixture asks
// for a clean exit and then never exits, so only the sentinel can end it.
TEST_CASE("death fixture: a hang at exit is named by the sentinel and ends with exit code 12", "[diag]")
{
    const FixtureRun r = RunFixture("none", { "--hang-at-exit", "--exit-seconds", "2" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 12);
    REQUIRE_FALSE(r.stem.empty());
    const auto env = Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    CHECK(env->exitCode == 12);
}

// The console family (spec S5.7): Ctrl-C is two-step as in UE -- the first
// press requests the host's clean exit, the second terminates -- and a console
// close requests the SAME clean exit rather than a second one. Driven through
// the SimulateConsoleCtrl seam so the rule is testable without a console, a
// signal, or a process to kill; note that `Armed` runs with
// startHangWatchdog = false, so none of this may depend on the watchdog.
TEST_CASE("diagnostics: the console handler is two-step for Ctrl-C and requests a clean exit on close", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-console-ctrl-test";
    std::filesystem::create_directories(dir);
    Armed armed(dir);
    static int hookCalls = 0; hookCalls = 0;

    // WITH NO HOOK INSTALLED the press is DECLINED, not swallowed. A console
    // tool that never opted in has nothing for a first Ctrl-C to start, so
    // claiming the event would cost the user a press and then kill the process
    // with 0xC000013A instead of the exit they asked for. Returning false
    // hands it to Windows' default handler, which terminates exactly as it did
    // before this module existed.
    Arcane::Diagnostics::SetCleanExitHook(nullptr, nullptr);
    CHECK_FALSE(Arcane::Diagnostics::SimulateConsoleCtrl(0 /*CTRL_C_EVENT*/));
    CHECK(hookCalls == 0);

    Arcane::Diagnostics::SetCleanExitHook([](void*) { ++hookCalls; }, nullptr);
    CHECK(Arcane::Diagnostics::SimulateConsoleCtrl(0 /*CTRL_C_EVENT*/));
    CHECK(hookCalls == 1);
    // A second Ctrl-C would terminate: not simulated. Close requests the same clean exit once.
    CHECK(Arcane::Diagnostics::SimulateConsoleCtrl(2 /*CTRL_CLOSE_EVENT*/));
    CHECK(hookCalls == 1);   // idempotent

    // The slot is PROCESS-wide and outlives this case's Armed: leave it empty
    // rather than pointing every later RequestCleanExit at this case's counter.
    Arcane::Diagnostics::SetCleanExitHook(nullptr, nullptr);
}
