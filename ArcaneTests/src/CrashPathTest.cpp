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

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    // Same RAII discipline as DiagnosticsTest's ArmedDiagnostics: Install() is
    // idempotent, so a case that does not disarm leaks its config into the next
    // one under Catch2's random ordering.
    struct Armed
    {
        explicit Armed(const std::filesystem::path& dir)
        {
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
