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

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
    // Runs the fixture with `--die <mode>` into a fresh dir; returns the run and the newest .arcdiag stem.
    struct FixtureRun { Arcane::Test::WitnessRun run; std::filesystem::path stem; };
    FixtureRun RunFixture(const char* mode, std::vector<std::string> extra = {})
    {
        const auto exe = std::filesystem::absolute("../death-fixture/death-fixture.exe");
        REQUIRE(std::filesystem::exists(exe));
        const auto dir = std::filesystem::temp_directory_path() / (std::string("arcane-death-") + mode);
        std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
        std::vector<std::string> args = { "--dir", dir.string(), "--die", mode };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 30000;
        FixtureRun out{ Arcane::Test::RunWitness(inv), {} };
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".arcdiag") out.stem = e.path().parent_path() / e.path().stem();
        return out;
    }
}

TEST_CASE("death fixture: an access violation yields a crash report and exit code 10 within the cap, with no dialog", "[diag]")
{
    const FixtureRun r = RunFixture("av");
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(std::filesystem::exists(r.stem.string() + ".dmp"));
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "crash");
    CHECK(r.run.wallMs < 15000);
}

TEST_CASE("death fixture: a clean run exits 0 and writes nothing", "[diag]")
{
    const FixtureRun r = RunFixture("none");
    CHECK(r.run.exitCode == 0);
    CHECK(r.stem.empty());
}
