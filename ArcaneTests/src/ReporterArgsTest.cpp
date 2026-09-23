// ArcaneCrashReporter's argument parser (crash window plan 2, task 4; spec §6).
//
// This is the reporter's ONLY input surface, and it is handed to it by a host
// that is already dying -- so the refusals matter as much as the successes: an
// unknown flag, a missing value or a non-numeric number must be named and
// refused (ExitCode::kBadArgs), never silently absorbed into a default that
// then drives a TerminateProcess or a file write.
//
// ReporterArgs.cpp source-compiles into this exe (premake5.lua, ArcaneTests'
// `files` list), the same way ConsoleBuffer.cpp and arcbuild's pure core do --
// ReporterMain.cpp (wWinMain) is NOT compiled here.

#include "ReporterArgs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace Arcane::Reporter;

// R48: the `--host-created` pair below is what THIS PLAN's spawn line carries
// once task 8 lands the hang protocol -- plan 1's SpawnReporter
// (Diagnostics.cpp) emits only the envelope path, --pid, --kind, --product and
// --unattended. The parser has to accept the finished line from the start,
// because the hosts and the reporter ship as one build.
TEST_CASE("reporter args: the crash hand-off line this plan emits parses into Report mode", "[reporter]")
{
    const std::vector<std::string> argv = {
        "D:/p/diagnostics/ArcaneEditor-20260923-101112-pid4242.arcdiag",
        "--pid", "4242", "--kind", "crash", "--product", "Arcane Editor", "--unattended",
        "--host-created", "133700000000000000",
    };
    const ParseResult r = ParseArgs(argv);
    REQUIRE(r.error.empty());
    REQUIRE(r.args.has_value());
    CHECK(r.args->mode == Args::Mode::Report);
    CHECK(r.args->envelopePath == argv[0]);
    CHECK(r.args->pid == 4242u);
    CHECK(r.args->kind == "crash");
    CHECK(r.args->product == "Arcane Editor");
    CHECK(r.args->unattended);
    CHECK(r.args->hostCreated == 133700000000000000ull);
    CHECK(r.args->deadlineSeconds == 60u);
    CHECK(r.args->recoveredEvent.empty());
}

TEST_CASE("reporter args: the hang line carries the recovered event; the monitor line selects Monitor mode", "[reporter]")
{
    const ParseResult hang = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "7", "--kind", "hang", "--product", "P",
        "--recovered-event", "Local\\Arcane-Recovered-7", "--relaunch", "P.exe --project X" });
    REQUIRE(hang.args.has_value());
    CHECK(hang.args->recoveredEvent == "Local\\Arcane-Recovered-7");
    CHECK(hang.args->relaunch == "P.exe --project X");

    const ParseResult mon = ParseArgs(std::vector<std::string>{
        "--monitor", "99", "--session", "D:/x/diagnostics/ArcaneEditor-pid99.session", "--unattended" });
    REQUIRE(mon.error.empty());
    REQUIRE(mon.args.has_value());
    CHECK(mon.args->mode == Args::Mode::Monitor);
    CHECK(mon.args->pid == 99u);
    CHECK(mon.args->sessionPath == "D:/x/diagnostics/ArcaneEditor-pid99.session");
    CHECK(mon.args->unattended);
}

TEST_CASE("reporter args: test seams and refusals", "[reporter]")
{
    const ParseResult seams = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "0", "--symbol-path", "D:/a;D:/b", "--deadline", "10" });
    REQUIRE(seams.args.has_value());
    CHECK(seams.args->symbolPath == "D:/a;D:/b");
    CHECK(seams.args->deadlineSeconds == 10u);

    CHECK_FALSE(ParseArgs(std::vector<std::string>{}).args.has_value());                              // no envelope, no --monitor
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid" }).args.has_value());        // missing value
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "x" }).args.has_value());   // not a number
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--bogus" }).args.has_value());      // unknown flag
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "--monitor", "5" }).args.has_value());            // monitor without --session
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "a.arcdiag", "b.arcdiag" }).args.has_value());    // two positionals
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "x" }).error.empty());
    CHECK(Usage().find("--monitor") != std::string::npos);
}

// R33 (plan 2 pre-flight). The monitor task 9 builds respawns ITSELF (D16), so
// by the time the second instance wants the host it can no longer trust the
// pid: the host may already be gone and the number reused. The host therefore
// passes an INHERITABLE handle to itself and the monitor re-opens nothing; 0
// means "absent -- fall back to OpenProcess by pid". Task 4 owns the PARSING
// only; no handle is inherited anywhere yet.
TEST_CASE("reporter args: --host-handle carries the host's inherited process handle", "[reporter]")
{
    const ParseResult mon = ParseArgs(std::vector<std::string>{
        "--monitor", "4242", "--session", "s.session",
        "--host-handle", "18446744073709551615", "--respawned" });
    REQUIRE(mon.error.empty());
    REQUIRE(mon.args.has_value());
    CHECK(mon.args->hostHandle == 18446744073709551615ull);   // a full u64 round-trips
    CHECK(mon.args->respawned);

    // Absent is 0, which is what "fall back to the pid" reads off.
    const ParseResult plain = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "7" });
    REQUIRE(plain.args.has_value());
    CHECK(plain.args->hostHandle == 0u);

    // Same refusal shape as every other numeric option.
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--host-handle" }).args.has_value());
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--host-handle", "0x1f4" }).args.has_value());
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--host-handle", "0x1f4" }).error.empty());
}
