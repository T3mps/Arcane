// Witness scenarios: the host-witness harness's OBSERVED half.
//
// Everything here spawns the REAL staged ArcaneRuntime through
// Helpers/HostWitness.hpp and grades what it reported. Nothing in this file
// links a host or reimplements a host's logic -- the whole point is that a
// green ArcaneTests run proves nothing about either host unless a host was
// actually launched (feedback_green_gate_proves_nothing_about_hosts).
//
// FRESH-COPY HYGIENE (spec section 6): every scenario runs against a COPY of
// the staged tree and mutates only the copy. Nothing here may touch
// bin/<Config>-windows-x86_64-md/ArcaneRuntime/ itself.
//
// -----------------------------------------------------------------------
// WHY `--frames 60` AND NOT A SMALLER BUDGET -- load-bearing, measured.
// -----------------------------------------------------------------------
// --settle FREEZES the render clock once the --frames budget is spent
// (RuntimeFrame::AdvanceSim), and ShaderCompiler DISPATCH is gated on that
// same clock by a 200 ms debounce (RuntimeFrame.cpp's own comment: "Collection
// is NOT clock-gated ... only DISPATCH is gated, by ShaderCompiler::Poll's
// readyAt check"). At the headless fixed step of 1/60 s, a budget below 13
// frames freezes the clock BEFORE the debounce elapses in sim time, so the
// queued material compiles never dispatch, IsIdle() never becomes true, and
// the convergence predicate's second conjunct can never be satisfied -- on ANY
// scene, with ANY reference. Measured on this tree: `--frames 10` never
// converges (census spriteBound 0 / postBound false, 38 attempts, bail
// "timeout-bound") while `--frames 60` converges in 3 attempts with everything
// bound. 60 is also exactly what scripts/golden-gate.ps1 passes, so these
// scenarios and the gate exercise the same shape of run.
//
// -----------------------------------------------------------------------
// WHERE W2 IS. NOT FORGOTTEN -- DERIVED UNREACHABLE.
// -----------------------------------------------------------------------
// The planned W2 ("a capture that never lands is recorded as capture-failed")
// has no content-or-CLI lever on this host, so it is deliberately absent
// rather than written against a fake one. `settleCaptureFailed` is
// `!previousCaptureValid` at the bail (RuntimeFrame.cpp:893), and
// previousCaptureValid is set by any settle attempt whose ReadCapture landed.
// ReadCapture only refuses when the capture NODE never ran
// (NriGraphContext.cpp:1915) -- and the predicate that arms that node,
// RenderGraph's `willBeLastFrame` (RuntimeFrame.cpp:619-627), is the SAME
// expression as CaptureTail's `pastBase` (line 701) that decides whether a
// settle attempt happens at all: `frameCount + 1 >= maxFrames` evaluated
// before the increment, and `frameCount >= maxFrames` after it. So on the
// runtime host EVERY settle attempt is, by construction, preceded by a frame
// that armed the readback. (The editor host states the same identity in one
// variable: EditorAppFrame.cpp:2794-2798 assigns `frame.capture = pastBase`
// and then gates its own settle branch on `pastBase`.) What is left --
// EnsureCaptureBuffer failing on a zero-sized surface or a refused
// HOST_READBACK allocation, MapBuffer returning null, a node failing to
// resolve -- is GPU/driver failure, which no scene and no command line can
// express. Reaching capture-failed would take a host code change whose only
// purpose is to be broken, which this harness refuses on principle.

#include "Helpers/HostWitness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/Verdict.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace Arcane::Test;

namespace
{
    // The staged runtime lives beside the test exe dir. Tests run FROM the exe
    // dir, so this resolves relative to cwd. HARD requirement, not a skip: an
    // unbuilt host tree on a capable machine is an incomplete build (spec s7).
    std::filesystem::path StagedRuntimeDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneRuntime");
        INFO("staged ArcaneRuntime not found -- build Arcane.slnx first: " << p.string());
        REQUIRE(std::filesystem::exists(p / "ArcaneRuntime.exe"));
        return p;
    }

    // The invocation shape every scenario shares. --report is passed HERE, by
    // the scenario, not by RunWitness -- the helper only watches the path it
    // was told about (HostWitness.hpp's WitnessInvocation::reportPath).
    WitnessInvocation HostInv(const WitnessScratch& scratch, std::vector<std::string> extraArgs)
    {
        WitnessInvocation inv;
        inv.exePath    = scratch.Dir() / "ArcaneRuntime.exe";
        inv.workingDir = scratch.Dir();
        inv.reportPath = scratch.Dir() / "witness-report.json";
        inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan",
                     "--frames", "60", "--report", inv.reportPath.generic_string() };
        inv.args.insert(inv.args.end(), extraArgs.begin(), extraArgs.end());
        inv.hardCapMs = 120000;
        return inv;
    }
}

TEST_CASE("W1: settle spends BOTH bounds when the compare conjunct cannot pass",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "w1-bounds-spent");

    // The lever: a wrong reference at EVERY level (Arc A: one level just falls
    // back to the shared one, and the run passes against the level this
    // scenario did not touch). Overwrite both with a PNG that is guaranteed
    // valid and guaranteed wrong -- an existing unrelated reference from the
    // staged tree, so no image is synthesised here.
    const std::filesystem::path refs  = scratch.Dir() / "ReferenceProject" / "Verify" / "References";
    const std::filesystem::path wrong = refs / "editor-ui.png";
    INFO("staged references: " << refs.string());
    REQUIRE(std::filesystem::exists(wrong));
    REQUIRE(std::filesystem::exists(refs / "runtime-scene.png"));
    REQUIRE(std::filesystem::exists(refs / "vulkan" / "runtime-scene.png"));
    std::filesystem::copy_file(wrong, refs / "runtime-scene.png",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(wrong, refs / "vulkan" / "runtime-scene.png",
                               std::filesystem::copy_options::overwrite_existing);

    // 4000, not the plan's 2000: process wall time INCLUDES boot (~2 s for this
    // host on this tree), so a 2000 ms bound is covered by boot alone and the
    // assertion below would read true even on a run that converged immediately
    // -- an assertion that cannot fail is not a test. At 4000 a converging run
    // measures ~2.1 s and a bound-spending run ~6 s, which separates the two
    // by about 2 s in each direction. See the task report's RED evidence.
    const std::uint32_t timeoutMs = 4000;
    WitnessRun run = RunWitness(HostInv(scratch,
        { "--settle", "4", "--settle-timeout", std::to_string(timeoutMs),
          "--compare", "runtime-scene" }));

    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    INFO("exit " << run.exitCode << ", wall " << run.wallMs << " ms, timedOut " << run.timedOut);

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());   // report exists and parsed

    // THE FACT UNDER TEST, asserted FIRST so a run that converged early fails
    // HERE rather than on some downstream consequence: both bounds were
    // genuinely spent. The wall clock is the only time signal a spawned host
    // exposes -- the report carries settleAttemptsUsed but no elapsed ms.
    REQUIRE(run.wallMs >= timeoutMs);
    REQUIRE(run.exitCode != 0);

    REQUIRE(run.report.contains("settleAttemptsUsed"));
    REQUIRE(run.report.contains("settleBailReason"));
    REQUIRE(run.report.contains("exitReason"));
    REQUIRE(run.report["settleAttemptsUsed"].get<std::uint64_t>() >= 4);
    const std::string bail = run.report["settleBailReason"].get<std::string>();
    REQUIRE((bail == "attempts-bound" || bail == "timeout-bound"));
    // "compare-failed", NOT "settle-not-converged": the latter is the
    // compare-never-evaluated spelling (RuntimeApp.cpp:1158), and this run
    // evaluated the comparison on every stable, idle attempt.
    REQUIRE(run.report["exitReason"].get<std::string>() == "compare-failed");
    // The mirror half: capture WORKED. Stably wrong is a different fact from a
    // readback that never landed, and this pins that the two do not collapse.
    REQUIRE(bail != "capture-failed");
}
