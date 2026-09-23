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
    // R65: a REAL pid here -- Report mode now refuses pid 0, and this case is
    // about the seams, not the pid rule (which has its own case below).
    const ParseResult seams = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "4242", "--symbol-path", "D:/a;D:/b", "--deadline", "10" });
    REQUIRE(seams.args.has_value());
    CHECK(seams.args->symbolPath == "D:/a;D:/b");
    CHECK(seams.args->deadlineSeconds == 10u);

    CHECK_FALSE(ParseArgs(std::vector<std::string>{}).args.has_value());                              // no envelope, no --monitor
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid" }).args.has_value());        // missing value
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "x" }).args.has_value());   // not a number
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "--monitor", "5" }).args.has_value());            // monitor without --session
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "a.arcdiag", "b.arcdiag" }).args.has_value());    // two positionals
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "x" }).error.empty());
    CHECK(Usage().find("--monitor") != std::string::npos);
    // The unknown-flag refusal moved to its own case (R68): asserting only
    // "did not parse" here let it pass through the MISSING-VALUE branch
    // instead, so the branch this line was believed to cover was never run.
}

// R65. Usage() declares --pid mandatory in Report mode and nothing enforced
// it, so a line missing it parsed clean with a silent pid == 0 -- the field
// tasks 8 and 9 use to find and TERMINATE the host. It fails closed now, and
// the rule is UNIFORM across both modes: 0 names the System Idle Process, so
// it can never be a host, and refusing it at the boundary is cheaper than
// making task 9 reason about a value the parser already knows is impossible.
TEST_CASE("reporter args: neither mode accepts a pid of zero", "[reporter]")
{
    // Report mode, pid absent entirely -- the silent-zero shape.
    const ParseResult none = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--kind", "crash" });
    CHECK_FALSE(none.args.has_value());
    CHECK(none.error.find("--pid") != std::string::npos);

    // Report mode, pid present and explicitly zero.
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "0" }).args.has_value());

    // Monitor mode: a pid cannot be ABSENT here (the flag that supplies it is
    // the flag that selects the mode), but an explicit zero is refused by the
    // same rule rather than reaching task 9's OpenProcess.
    const ParseResult monZero = ParseArgs(std::vector<std::string>{
        "--monitor", "0", "--session", "s.session" });
    CHECK_FALSE(monZero.args.has_value());
    CHECK(monZero.error.find("--monitor") != std::string::npos);

    // A real pid still parses in BOTH modes -- the rule refuses zero, not pids.
    const ParseResult mon = ParseArgs(std::vector<std::string>{ "--monitor", "99", "--session", "s.session" });
    REQUIRE(mon.args.has_value());
    CHECK(mon.args->pid == 99u);
    const ParseResult rep = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "99" });
    REQUIRE(rep.args.has_value());
    CHECK(rep.args->pid == 99u);
}

// R66. A value-taking option must not swallow the NEXT FLAG as its value.
// "--relaunch --unattended" used to produce a garbage relaunch line that task
// 7's restart button would hand to CreateProcess AND silently drop the host's
// unattended intent -- putting a window on a machine that asked for none.
TEST_CASE("reporter args: a flag is never absorbed as another option's value", "[reporter]")
{
    const ParseResult dropped = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "7", "--relaunch", "--unattended" });
    CHECK_FALSE(dropped.args.has_value());
    CHECK(dropped.error.find("--relaunch") != std::string::npos);
    CHECK(dropped.error.find("needs a value") != std::string::npos);

    // Same rule for a numeric option, and mid-line rather than last.
    CHECK_FALSE(ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "--unattended", "--kind", "crash" }).args.has_value());

    // ...and the legitimate shape still parses: a relaunch line starts with
    // the executable, never with "--", so nothing real is refused here.
    const ParseResult ok = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "7", "--relaunch", "P.exe --project X", "--unattended" });
    REQUIRE(ok.args.has_value());
    CHECK(ok.args->relaunch == "P.exe --project X");
    CHECK(ok.args->unattended);
}

// R68. An unknown flag is named as UNKNOWN wherever it sits. Before this the
// value was demanded first, so "--bogus" in final position came back as
// "--bogus needs a value" -- the wrong defect named, and the unknown-argument
// branch was unreachable from the end of a line even though a case commented
// "// unknown flag" was believed to cover it.
TEST_CASE("reporter args: an unknown flag is refused as unknown in any position", "[reporter]")
{
    // Final position -- the shape that used to report the wrong error.
    const ParseResult last = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "7", "--bogus" });
    CHECK_FALSE(last.args.has_value());
    CHECK(last.error.find("unknown argument: --bogus") != std::string::npos);

    // Mid-line, with something that LOOKS like its value after it.
    const ParseResult mid = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--bogus", "value", "--pid", "7" });
    CHECK_FALSE(mid.args.has_value());
    CHECK(mid.error.find("unknown argument: --bogus") != std::string::npos);

    // A KNOWN option genuinely missing its value still says exactly that, so
    // the two refusals stay distinguishable to a reader of the log.
    const ParseResult missing = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "7", "--kind" });
    CHECK_FALSE(missing.args.has_value());
    CHECK(missing.error.find("--kind needs a value") != std::string::npos);
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

// R60 (controller ruling, task 5). The deadline had no floor: `--deadline 0`
// parsed and was accepted, and zero seconds is not a deadline -- it is a
// guarantee that the symbolizing worker is killed before it can resolve a
// single frame, so every unattended run carrying it would produce a partial
// report and exit 5 while looking like it had tried.
//
// The three candidate meanings were "no deadline", "expire immediately" and
// "refused". It is REFUSED, on the same fail-closed rule R65 applied to the
// pid: a value that can never express a legitimate intent is named at the
// boundary rather than absorbed into behaviour a later reader has to
// reverse-engineer. "No deadline" is the worst of the three -- it would make
// the one flag whose entire job is to BOUND an unattended child silently
// unbound, which is precisely the headless hazard spec §6 wrote it for -- and
// "expire immediately" is not even a reliable test lever, because
// wait_for(0s, pred) evaluates the predicate once and a worker that had
// already finished would return success.
//
// The floor is therefore 1 second: the smallest value the flag can express
// that still gives the worker a real window, and the lever the deadline
// branch is exercised with.
TEST_CASE("reporter args: a deadline of zero is refused -- the floor is one second", "[reporter]")
{
    const ParseResult zero = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "7", "--deadline", "0" });
    CHECK_FALSE(zero.args.has_value());
    CHECK(zero.error.find("--deadline") != std::string::npos);

    // Monitor mode carries the same flag to the report it synthesizes (task
    // 9), so the rule is uniform across both modes -- as the pid rule is.
    const ParseResult monZero = ParseArgs(std::vector<std::string>{
        "--monitor", "7", "--session", "s.session", "--deadline", "0" });
    CHECK_FALSE(monZero.args.has_value());

    // One second is the floor, not a refusal: it is the lever the deadline
    // branch itself is driven with.
    const ParseResult one = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "7", "--deadline", "1" });
    REQUIRE(one.args.has_value());
    CHECK(one.args->deadlineSeconds == 1u);

    // The default is untouched: 60 s is spec §6's number, not the only legal one.
    const ParseResult plain = ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "7" });
    REQUIRE(plain.args.has_value());
    CHECK(plain.args->deadlineSeconds == 60u);
}
