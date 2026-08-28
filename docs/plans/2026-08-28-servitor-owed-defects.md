# Servitor Owed Defects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the five owed automation-tooling defects and delete the `-AdvisoryLanes` switch, so the golden gate hard-gates all four lanes again.

**Architecture:** Seven tasks in a fixed order. T1 is a one-line refusal in the function T2 then extends. T2 makes `--settle` bail on a conjunction of an attempt bound and a time bound, paced by a fixed interval. T3 bumps `VerifyReport` to schemaVersion 3 and reports what T2 measured. T4 gives `Project::Open` an options struct so the editor's verify capture stops depending on the machine's crash history. T5 fixes ArcaneHub's exit-3 misreport. T6 is the user's desk checkpoint. T7 deletes the switch.

**Tech Stack:** C++23, Catch2, premake5 + MSBuild (VS 18), PowerShell 5.1, Rust/Tauri (ArcaneHub).

**Spec:** `docs/specs/2026-08-28-servitor-owed-defects-design.md`

## Global Constraints

Every task's requirements implicitly include this section.

- **`msbuild` is NOT on PATH:** `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`. Build `Arcane.slnx`, never a bare `.vcxproj`. Pass `-nr:false`.
- **A 3-second "successful" msbuild is a NO-OP, not a verification.**
- **Run test exes FROM THEIR OWN DIRECTORY** (`cd bin\Debug-windows-x86_64-md\ArcaneTests`). ArcaneTests runs in random order and resolves data relative to cwd.
- **Never run `[gpu]`, `[golden]` or `[mesh]`** in the dev loop — this machine carries a driver-crash hazard and GPU runs happen at the physical desk. Always append `~[gpu]`.
- **Gate baseline to hold: 52207 assertions / 1255 test cases**, 0 warnings across Debug/Release/Dist. **52203/1254 is RETIRED** — anything citing it is stale. Each task adds its own cases on top.
- **New or renamed source files are picked up by a glob** (`premake5.lua:186`), so **re-run `GenerateProjects.bat` after adding or renaming any file**, or MSBuild will not see it. T2 adds a file.
- **`--settle-timeout` default is 5000 ms (INHERITED from Playwright); the 50 ms poll interval is OURS.** Do not present the interval as a citation.
- **DO NOT re-bless `editor-ui` before T4 lands.** The moving input is written by the thing under test, so a re-bless goes green today and red at the next crashed editor run.
- **DO NOT touch `ThirdParty/`** or rewrite historical docs (`docs/plans/*`, `docs/specs/2026-08-2{3,5}*`, `docs/2026-08-27-servitor-rulings-record.md`).
- **The two hosts' settle loops must move in lockstep.** `ArcaneEditor/src/App/EditorAppFrame.cpp` is a line-for-line port of `ArcaneRuntime/src/RuntimeFrame.cpp`'s loop. A change to one that is not made to the other is a defect, not a partial landing.

---

## File Structure

**T1** — Modify: `ArcaneClient/src/Arcane/Host/HostConfig.cpp:179-183`; `ArcaneTests/src/HostConfigTest.cpp`
**T2** — Create: `ArcaneClient/src/Arcane/Host/SettleBound.hpp`, `ArcaneTests/src/SettleBoundTest.cpp`. Modify: `HostConfig.{hpp,cpp}`, `ArcaneRuntime/src/RuntimeFrame.{hpp,cpp}`, `ArcaneRuntime/src/RuntimeApp.{hpp,cpp}`, `ArcaneEditor/src/App/EditorAppFrame.cpp`, `ArcaneEditor/src/App/EditorApp.hpp`, `ArcaneTests/src/HostConfigTest.cpp`
**T3** — Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}`, `ArcaneRuntime/src/RuntimeApp.cpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp`, `ArcaneTests/src/VerifyReportTest.cpp`
**T4** — Create: `ArcaneClient/src/Arcane/Project/ProjectOpenOptions.hpp`. Modify: `Project.{hpp,cpp}`, `Runtime.{hpp,cpp}`, `ArcaneClient/src/Arcane/Host/ProjectBoot.cpp`, `ArcaneEditor/src/App/EditorAppProject.cpp`, `ArcaneTests/src/ProjectTest.cpp`
**T5** — Modify: `ArcaneHub/src-tauri/src/launch.rs`
**T6** — no files; a user desk checkpoint
**T7** — Modify: `scripts/golden-gate.ps1`, `Jenkinsfile`

---

### Task 1: `--fixed-dt nan` is refused

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.cpp:179-183`
- Test: `ArcaneTests/src/HostConfigTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing new. Behaviour change only: `--headless --fixed-dt nan` now exits 2.

**Why first:** the close-out doc calls this *"the first thing to fix on the next touch of that function"*, and Task 2 extends that same validation block.

- [ ] **Step 1: Write the failing test**

Add to `ArcaneTests/src/HostConfigTest.cpp`, beside the other `--fixed-dt` cases:

```cpp
// --fixed-dt nan/inf is the SAME undefined-behaviour class as the
// --max-diff-pixel-ratio bug fixed in 92792847: Cli's NumericOk only asks
// whether from_chars can consume the token, and it accepts "nan" and "inf".
// The value then flows into the frame loop's clock arithmetic, where a NaN dt
// poisons every downstream accumulation silently -- the run exits 0 having
// simulated nothing coherent, which is the worst failure an agent can read.
TEST_CASE("host config: --fixed-dt refuses non-finite values", "[hostconfig]")
{
    for (const char* bad : { "nan", "inf", "-inf" })
    {
        const auto o = Run({ "--headless", "--frames", "60", "--fixed-dt", bad });
        CAPTURE(bad);
        REQUIRE_FALSE(o.config.has_value());
        CHECK(o.exitCode == 2);
    }
    // The existing positive-value contract still holds.
    const auto ok = Run({ "--headless", "--frames", "60", "--fixed-dt", "0.008" });
    REQUIRE(ok.config.has_value());
    CHECK(ok.config->fixedDtSeconds == Catch::Approx(0.008));
}
```

- [ ] **Step 2: Run it and watch it fail for the right reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "host config: --fixed-dt refuses non-finite values"
```
Expected: **FAIL** on the first `REQUIRE_FALSE` — `nan` currently parses and `nan <= 0.0` is **false**, so the existing guard lets it through. That is the correct first failure. If it fails some other way, say so rather than adjusting the test.

- [ ] **Step 3: Extend the guard**

`ArcaneClient/src/Arcane/Host/HostConfig.cpp:179`, replace:

```cpp
        if (cfg.headless && cfg.fixedDtSeconds <= 0.0)
        {
            std::fprintf(stderr, "error: --fixed-dt wants a positive number of seconds\n");
            return { std::nullopt, 2 };
        }
```

with:

```cpp
        // `<= 0.0` alone does NOT reject NaN: every comparison against NaN is
        // false, so `nan <= 0.0` is false and the value flowed straight through
        // into the frame clock. Same UB class as --max-diff-pixel-ratio's range
        // bug (fixed in 92792847), and refused the same way -- at the parse
        // boundary, not clamped.
        if (cfg.headless && (!std::isfinite(cfg.fixedDtSeconds) || cfg.fixedDtSeconds <= 0.0))
        {
            std::fprintf(stderr, "error: --fixed-dt wants a positive, finite number of seconds\n");
            return { std::nullopt, 2 };
        }
```

`<cmath>` is already included at `HostConfig.cpp:5` for exactly this reason — do not add it again.

- [ ] **Step 4: Run the focused test, then the suite**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "host config: --fixed-dt refuses non-finite values"
.\ArcaneTests.exe ~[gpu]
```
Expected: focused PASS; suite **52211 assertions / 1256 cases** (baseline + this case's 1 case and 4 assertions: 3 loop iterations × `REQUIRE_FALSE` + `CHECK`, then `REQUIRE` + `CHECK` — **state the number you actually measure**; if it differs, say so rather than adjusting the expectation).

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneTests/src/HostConfigTest.cpp
git commit -m "fix(cli): --fixed-dt refuses non-finite values

<= 0.0 does not reject NaN -- every comparison against NaN is false, so
nan flowed into the frame clock unchecked. Same UB class as the
--max-diff-pixel-ratio range bug fixed in 92792847, refused the same way."
```

---

### Task 2: `--settle` bails on a conjunction, paced by a fixed interval

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/SettleBound.hpp`, `ArcaneTests/src/SettleBoundTest.cpp`
- Modify: `HostConfig.hpp` (field), `HostConfig.cpp` (registration + read + refusals), `ArcaneRuntime/src/RuntimeFrame.{hpp,cpp}`, `ArcaneRuntime/src/RuntimeApp.{hpp,cpp}`, `ArcaneEditor/src/App/EditorAppFrame.cpp`, `ArcaneEditor/src/App/EditorApp.hpp`
- Test: `ArcaneTests/src/SettleBoundTest.cpp`, `ArcaneTests/src/HostConfigTest.cpp`

**Interfaces:**
- Consumes: Task 1's touched validation block (no API dependency).
- Produces:
  - `Arcane::SettleBail` — `enum class : std::uint8_t { Keep, AttemptsBound, TimeoutBound }`
  - `constexpr Arcane::SettleBail Arcane::SettleBailDecision(std::uint64_t attemptsUsed, std::uint64_t attempts, std::uint64_t elapsedMs, std::uint64_t timeoutMs, std::uint64_t intervalMs) noexcept`
  - `Arcane::kSettleIntervalMs` — `constexpr std::uint64_t`, value `50`
  - `HostConfig::settleTimeoutMs` — `std::uint64_t`, default `5000`
  - Task 3 consumes `SettleBail` to populate `settleBailReason`.

**This task adds a file**, so `GenerateProjects.bat` must be re-run before building.

- [ ] **Step 1: Write the failing test for the pure predicate**

Create `ArcaneTests/src/SettleBoundTest.cpp`:

```cpp
// The settle bail predicate, extracted so it is testable without a GPU, a
// frame loop or a clock. The loop it governs is desk-verified; THIS is the
// part that can be pinned in CI.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/SettleBound.hpp>

using Arcane::SettleBail;
using Arcane::SettleBailDecision;

TEST_CASE("settle bail: keeps going until BOTH bounds are spent", "[settle]")
{
    // Neither spent.
    CHECK(SettleBailDecision(0, 30, 0, 5000, 50)    == SettleBail::Keep);
    // Attempts spent, time is not -- this is the DEFECT case: the old code
    // bailed here, ~100ms in, before compilation could drain.
    CHECK(SettleBailDecision(30, 30, 100, 5000, 50) == SettleBail::Keep);
    // Time spent, attempts are not.
    CHECK(SettleBailDecision(5, 30, 5000, 5000, 50) == SettleBail::Keep);
}

TEST_CASE("settle bail: names the BINDING bound, not the tripped one", "[settle]")
{
    // 30 attempts x 50ms = 1500ms < 5000ms timeout -> time governs.
    CHECK(SettleBailDecision(94, 30, 5000, 5000, 50) == SettleBail::TimeoutBound);
    // 200 attempts x 50ms = 10000ms >= 5000ms timeout -> attempts govern.
    CHECK(SettleBailDecision(200, 200, 10000, 5000, 50) == SettleBail::AttemptsBound);
    // Exactly equal: attempts reach the timeout at the same instant. Attempts
    // is reported, because raising the timeout alone would not change it.
    CHECK(SettleBailDecision(100, 100, 5000, 5000, 50) == SettleBail::AttemptsBound);
}

TEST_CASE("settle bail: a zero timeout degrades to the attempt bound alone", "[settle]")
{
    // --settle-timeout 0 means "no time bound": the conjunction reduces to the
    // old attempts-only behaviour rather than becoming unsatisfiable.
    CHECK(SettleBailDecision(29, 30, 0, 0, 50) == SettleBail::Keep);
    CHECK(SettleBailDecision(30, 30, 0, 0, 50) == SettleBail::AttemptsBound);
}
```

- [ ] **Step 2: Run it and watch it fail for the right reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[settle]"
```
Expected: **compile error** — `Arcane/Host/SettleBound.hpp` does not exist. That is the correct first failure.

- [ ] **Step 3: Create the header**

Create `ArcaneClient/src/Arcane/Host/SettleBound.hpp`:

```cpp
#pragma once

// The --settle bail decision, as a pure function.
//
// THE DEFECT THIS CLOSES: --settle counted ATTEMPTS while the condition it
// waits on (ShaderCompiler::IsIdle()) is denominated in MILLISECONDS. With no
// sleep, wait or pump between attempts, ~3.3ms/attempt spends a 30-attempt
// budget in ~100ms -- so a fast build bailed "not converged" long before
// background compilation could drain.
//
// THE FIX IS A CONJUNCTION, not a bigger attempt count. Unreal independently
// corroborates the shape: AutomationScreenshotOptions requires `delay` AND
// `frame_delay` -- "both ... must be met" -- denominated in different units,
// because Epic found neither sufficient alone. Playwright's toHaveScreenshot
// bounds by time alone. Ours is a superset of both.
//
// Extracted into its own header so it can be tested without a GPU, a frame
// loop or a clock -- the loop it governs is desk-verified, this part is not.

#include <cstdint>

namespace Arcane
{
    // How long the loop waits between attempts. OURS, not inherited: Playwright
    // documents a cadence only for its EXPLICIT polling helpers (expect.poll's
    // `intervals`, default [100, 250, 500, 1000] -- an escalating backoff), and
    // not for the auto-retrying assertions toHaveScreenshot belongs to. That
    // backoff is safe there because those helpers carry NO attempt bound; under
    // our --settle 30 floor it would cost ~26.9s per lane. A fixed interval
    // keeps 30 attempts inside ~1.6s so the timeout stays the governing bound,
    // which is the entire point of the conjunction.
    inline constexpr std::uint64_t kSettleIntervalMs = 50;

    enum class SettleBail : std::uint8_t
    {
        Keep,           // at least one bound is unspent -- keep attempting
        AttemptsBound,  // both spent; the ATTEMPT budget is what governed
        TimeoutBound,   // both spent; the TIME budget is what governed
    };

    // Returns Keep until BOTH bounds are spent. Once both are, names the bound
    // that GOVERNED -- the one whose knob would actually change the outcome --
    // rather than the one that happened to be checked first.
    //
    // With a fixed interval the governing bound is a property of the arguments,
    // not of the run: the attempt budget is reached at ~attempts * intervalMs,
    // so whichever of that and timeoutMs is larger is what the loop waited for.
    // A caller told "attempts-bound" should raise --settle; one told
    // "timeout-bound" should raise --settle-timeout. That is the whole point of
    // reporting it.
    [[nodiscard]] constexpr SettleBail SettleBailDecision(std::uint64_t attemptsUsed,
                                                           std::uint64_t attempts,
                                                           std::uint64_t elapsedMs,
                                                           std::uint64_t timeoutMs,
                                                           std::uint64_t intervalMs) noexcept
    {
        if (attemptsUsed < attempts || elapsedMs < timeoutMs)
            return SettleBail::Keep;
        return (attempts * intervalMs >= timeoutMs) ? SettleBail::AttemptsBound
                                                     : SettleBail::TimeoutBound;
    }
}
```

- [ ] **Step 4: Regenerate projects, build, run the focused test**

```bash
GenerateProjects.bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug -nr:false
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[settle]"
```
Expected: PASS, 3 cases. `GenerateProjects.bat` is **required** — two new files were added and premake globs them.

- [ ] **Step 5: Commit the predicate**

```bash
git add ArcaneClient/src/Arcane/Host/SettleBound.hpp ArcaneTests/src/SettleBoundTest.cpp
git commit -m "feat(settle): the bail decision as a pure, testable predicate

Keep until BOTH bounds are spent, then name the bound that GOVERNED so a
caller knows which knob to turn. Extracted so it is testable without a GPU,
a frame loop or a clock."
```

- [ ] **Step 6: Write the failing CLI tests**

Add to `ArcaneTests/src/HostConfigTest.cpp`:

```cpp
TEST_CASE("host config: --settle-timeout parses and defaults to 5000", "[hostconfig]")
{
    const auto def = Run({ "--headless", "--frames", "60", "--settle", "30",
                           "--screenshot", "s.png" });
    REQUIRE(def.config.has_value());
    // 5000ms is INHERITED from Playwright's expect timeout, not invented here.
    CHECK(def.config->settleTimeoutMs == 5000u);

    const auto set = Run({ "--headless", "--frames", "60", "--settle", "30",
                           "--settle-timeout", "250", "--screenshot", "s.png" });
    REQUIRE(set.config.has_value());
    CHECK(set.config->settleTimeoutMs == 250u);
}

TEST_CASE("host config: --settle-timeout requires --settle", "[hostconfig]")
{
    // A timeout on a loop that never runs is a caller error, and this file
    // refuses those loudly rather than ignoring them.
    const auto o = Run({ "--headless", "--frames", "60", "--settle-timeout", "250",
                         "--screenshot", "s.png" });
    REQUIRE_FALSE(o.config.has_value());
    CHECK(o.exitCode == 2);
}

TEST_CASE("host config: --settle-timeout requires --headless", "[hostconfig]")
{
    // Joins --fixed-dt/--probe/--report/--settle/--compare in the
    // wantsOffscreenOnly set: a settle timeout in a windowed run is
    // meaningless for the same reason --settle already is.
    const auto o = Run({ "--frames", "60", "--settle", "30", "--settle-timeout", "250",
                         "--screenshot", "s.png" });
    REQUIRE_FALSE(o.config.has_value());
    CHECK(o.exitCode == 2);
}
```

- [ ] **Step 7: Run them and watch them fail for the right reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[hostconfig]"
```
Expected: **compile error** — `HostConfig` has no member `settleTimeoutMs`. That is the correct first failure.

- [ ] **Step 8: Add the field, the flag, the read and the refusals**

`ArcaneClient/src/Arcane/Host/HostConfig.hpp`, beside `settleAttempts` (`:135`):

```cpp
        // --settle-timeout MS. The TIME half of settle's bail conjunction; the
        // loop gives up only when BOTH this and settleAttempts are spent (see
        // Arcane/Host/SettleBound.hpp). 0 means "no time bound", degrading to
        // the old attempts-only behaviour rather than becoming unsatisfiable.
        //
        // 5000 is INHERITED: Playwright's toHaveScreenshot timeout "Defaults to
        // timeout in TestConfig.expect", and that default is 5 seconds. The
        // 50ms poll interval beside it (kSettleIntervalMs) is OURS -- do not
        // present the two as having the same provenance.
        std::uint64_t   settleTimeoutMs = 5000;
```

`HostConfig.cpp`, registration beside `--settle` (`:31`):

```cpp
        cli.Option("settle-timeout", "5000", "milliseconds the settle loop must ALSO spend before "
                                         "giving up; it bails only when the attempt budget AND "
                                         "this timeout are both spent (0 = no time bound; "
                                         "--settle only)").Type(CliType::Uint);
```

`HostConfig.cpp`, read beside `cfg.settleAttempts` (`:123`):

```cpp
        cfg.settleTimeoutMs = r.GetAs<std::uint64_t>("settle-timeout");
```

`HostConfig.cpp`, add `|| r.Supplied("settle-timeout")` to the `wantsOffscreenOnly` expression at `:165-167`, and extend its error text at `:170` to name `--settle-timeout`.

`HostConfig.cpp`, after the `settleAttempts == 1` refusal (`:201-207`):

```cpp
        // A timeout bounds a loop that --settle turns on. Supplying one without
        // the other is a caller error, refused rather than ignored -- the same
        // treatment --max-diff-pixels gets without --compare.
        if (r.Supplied("settle-timeout") && cfg.settleAttempts == 0)
        {
            std::fprintf(stderr, "error: --settle-timeout requires --settle (it bounds the settle "
                                 "loop, which --settle is what turns on)\n");
            return { std::nullopt, 2 };
        }
```

Also **reword `--settle`'s own help text** at `:30-33`: it currently says *"up to N attempts"*, which the conjunction makes false. It is now **at least** N attempts.

- [ ] **Step 9: Run the CLI tests**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[hostconfig]"
```
Expected: PASS.

- [ ] **Step 10: Wire the conjunction into BOTH host loops**

`ArcaneRuntime/src/RuntimeFrame.hpp`, add to `FrameIo` beside `settleAttemptsUsed` (`:161`):

```cpp
        // Monotonic ms since the settle phase began -- the frame on which
        // pastBase first went true. NOT since process start: that would charge
        // boot, project open and the --frames budget against the convergence
        // budget and make the timeout depend on how long the scene took to load.
        std::uint64_t&              settleElapsedMs;
        Arcane::SettleBail&         settleBail;
```

`ArcaneRuntime/src/RuntimeApp.hpp`, beside `m_settleAttemptsUsed` (`:214`):

```cpp
    std::chrono::steady_clock::time_point m_settleStartedAt{};
    bool                                  m_settleStartedAtValid = false;
    std::uint64_t                         m_settleElapsedMs      = 0;
    Arcane::SettleBail                    m_settleBail           = Arcane::SettleBail::Keep;
```

`ArcaneRuntime/src/RuntimeFrame.cpp`, in the settle branch, immediately after the `++io.settleAttemptsUsed;` at `:730`:

```cpp
        // Stamp the settle phase's own start on its FIRST attempt, and measure
        // from there. NOT from process start: that would charge boot, project
        // open and the whole --frames budget against the convergence budget,
        // making the timeout depend on how long the scene took to load.
        if (io.settleAttemptsUsed == 1)
            io.settleStartedAt = std::chrono::steady_clock::now();
        io.settleElapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - io.settleStartedAt).count());
```

Add `std::chrono::steady_clock::time_point& settleStartedAt;` to `FrameIo` alongside the two fields above, owned by `RuntimeApp` as `m_settleStartedAt` (already declared in this step). `RuntimeFrame.cpp` already includes `<thread>` for its existing `sleep_for` calls; add `<chrono>` if it is not already present.

Then **sleep `kSettleIntervalMs` before returning `false`** at `:867`, and replace the bail at `:853`:

```cpp
        io.settleBail = Arcane::SettleBailDecision(io.settleAttemptsUsed, io.config.settleAttempts,
                                                    io.settleElapsedMs, io.config.settleTimeoutMs,
                                                    Arcane::kSettleIntervalMs);
        if (io.settleBail != Arcane::SettleBail::Keep)
        {
            ARC_ERROR("--settle: NOT CONVERGED after {} attempt(s) / {} ms -- two consecutive "
                      "captures never compared byte-equal with the compiler idle ({})",
                      io.settleAttemptsUsed, io.settleElapsedMs,
                      io.settleBail == Arcane::SettleBail::AttemptsBound
                          ? "attempt budget governed -- raise --settle"
                          : "timeout governed -- raise --settle-timeout");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::kSettleIntervalMs));
        return false;
```

**Then make the identical change in `ArcaneEditor/src/App/EditorAppFrame.cpp`'s port** (its settle branch begins at `:2854`; its members live on `EditorApp`, declared in `ArcaneEditor/src/App/EditorApp.hpp`). The two loops must stay line-for-line equivalent.

- [ ] **Step 11: Build all three configurations and run the suite**

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug -nr:false
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Release -nr:false
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Dist -nr:false
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```
Expected: 0 warnings in all three. **State the actual assertion/case count.**

- [ ] **Step 12: Commit**

```bash
git add -A
git commit -m "fix(settle)!: bail on a conjunction of attempts AND time, paced at 50ms

--settle counted ATTEMPTS while the condition it waits on is denominated in
MILLISECONDS: ~3.3ms/attempt spent a 30-attempt budget in ~100ms, so a fast
build bailed before compilation could drain. Verified: no sleep, wait or
pump existed between attempts in either host.

Adds --settle-timeout (default 5000, INHERITED from Playwright's expect
timeout) and a 50ms poll interval (OURS -- Playwright documents a cadence
only for expect.poll, whose escalating backoff would cost ~26.9s per lane
under our attempt floor). Bails only when both bounds are spent, and reports
which one GOVERNED so a caller knows which knob to turn.

--settle N now means AT LEAST N attempts, not up to N. Help text reworded.
Both hosts moved in lockstep."
```

---

### Task 3: `VerifyReport` schemaVersion 3

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}`, `ArcaneRuntime/src/RuntimeApp.cpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp`
- Test: `ArcaneTests/src/VerifyReportTest.cpp`

**Interfaces:**
- Consumes: Task 2's `Arcane::SettleBail`.
- Produces: `VerifyReport::SetSettle(std::uint64_t attemptsUsed, bool converged, Arcane::SettleBail bail, bool captureFailed)`; JSON keys `settleAttemptsUsed` (number) and `settleBailReason` (string, one of `converged` / `attempts-bound` / `timeout-bound` / `capture-failed`); `schemaVersion` is now `3`; `mode` is now `"headless"`.

**ONE commit.** The two hosts disagree about the counter today and shipping the field without reconciling them makes the report **lie on the editor**.

- [ ] **Step 1: Write the failing tests**

Add to `ArcaneTests/src/VerifyReportTest.cpp`:

```cpp
TEST_CASE("verify report: schemaVersion 3 carries settle facts and the headless mode", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    r.SetSettle(94, /*converged=*/false, Arcane::SettleBail::TimeoutBound,
                /*captureFailed=*/false);
    const auto doc = nlohmann::json::parse(r.ToJson());

    CHECK(doc["schemaVersion"] == 3);
    // The MODE's machine-readable name, in the mode's own word. Changed on this
    // bump because a schemaVersion bump is exactly when a wire value may change.
    CHECK(doc["mode"] == "headless");
    CHECK(doc["settleAttemptsUsed"] == 94);
    CHECK(doc["settleBailReason"] == "timeout-bound");
}

TEST_CASE("verify report: a converged run reports its real count, not its budget", "[verify]")
{
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    r.SetSettle(7, /*converged=*/true, Arcane::SettleBail::Keep, /*captureFailed=*/false);
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK(doc["settleAttemptsUsed"] == 7);
    CHECK(doc["settleBailReason"] == "converged");
}

TEST_CASE("verify report: settle keys are ABSENT when settle was never asked for", "[verify]")
{
    // Absence-is-absence, the same contract SetCapture and SetCompare uphold:
    // an agent must be able to tell "not asked" from "asked and converged".
    Arcane::VerifyReport r;
    r.SetRun("D3D12", 60, "frames-complete");
    const auto doc = nlohmann::json::parse(r.ToJson());
    CHECK_FALSE(doc.contains("settleAttemptsUsed"));
    CHECK_FALSE(doc.contains("settleBailReason"));
}
```

Update the existing assertions: `schemaVersion` at `:71`, `:652`, `:676` become `3`; `"mode"` at `:73` and `:159` become `"headless"`.

- [ ] **Step 2: Run and watch it fail for the right reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[verify]"
```
Expected: **compile error** — `VerifyReport` has no member `SetSettle`.

- [ ] **Step 3: Add the setter, the state and the emission**

`VerifyReport.hpp` — declare `SetSettle` with a doc comment stating the absence contract, and add `m_settleSet`, `m_settleAttemptsUsed`, `m_settleConverged`, `m_settleBail`, `m_settleCaptureFailed`. Include `<Arcane/Host/SettleBound.hpp>`.

`VerifyReport.cpp` — `j["schemaVersion"] = 3;` at `:508`, `j["mode"] = "headless";` at `:516` (replacing its now-false comment about "offscreen"), and after the `capture` block:

```cpp
        if (m_settleSet)
        {
            j["settleAttemptsUsed"] = m_settleAttemptsUsed;
            j["settleBailReason"]   =
                m_settleCaptureFailed              ? "capture-failed"
              : m_settleConverged                  ? "converged"
              : m_settleBail == SettleBail::AttemptsBound ? "attempts-bound"
                                                          : "timeout-bound";
        }
```

- [ ] **Step 4: Reconcile BOTH hosts' counter semantics — the part with no other witness**

**First, define where `captureFailed` comes from** — it has no source yet. Add `bool m_settleCaptureFailed = false;` to `RuntimeApp` (and its editor twin), exposed through `FrameIo` as `bool& settleCaptureFailed;`, and set it at the bail site added in Task 2:

```cpp
        if (io.settleBail != Arcane::SettleBail::Keep)
        {
            // No readback EVER landed, so the loop never had two frames to
            // compare. That is a different fact from "captured fine, never
            // agreed", and an agent needs to tell them apart: one is a broken
            // capture path, the other is a genuinely unstable scene.
            io.settleCaptureFailed = !io.previousCaptureValid;
```

`captureFailed` outranks the bound in `ToJson`'s ternary chain (Step 3) precisely because it means the bounds never got a fair test.

`ArcaneRuntime/src/RuntimeApp.cpp`: call `SetSettle` from `ShutdownGraphPath` only when `m_config.settleAttempts != 0`, passing `m_settleAttemptsUsed` as measured. Its defensive branch currently leaves the counter at 0 — it must pass the real count.

`ArcaneEditor/src/App/EditorAppFrame.cpp:2860` currently reads:

```cpp
            m_settleAttemptsUsed = m_config.settleAttempts;
```

**Delete that assignment.** It forces the counter to the budget so the loop's own `>=` check terminates; replace that mechanism with a separate `m_settleAborted = true` flag that the loop checks, leaving the counter honest. Shipping the field without this makes the report lie on the editor, which is worse than the field's absence: an absent field is an honest gap, a present wrong one is something an agent will trust.

- [ ] **Step 5: Run the suite**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```
Expected: PASS. `VerifyReportTest.cpp` must still parse the report **without linking the engine** — that property is the Servitor boundary. **State the actual count.**

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(verify)!: schemaVersion 3 -- honest settle count, bail reason, headless mode

settleAttemptsUsed existed only in logs. Adds it plus settleBailReason
(converged / attempts-bound / timeout-bound / capture-failed) and changes
mode to \"headless\" -- a schemaVersion bump is exactly the versioned moment
a wire value may change.

Reconciles both hosts' counter semantics IN THIS COMMIT: the editor forced
the counter to its budget (EditorAppFrame.cpp:2860) while the runtime left
it at 0. Shipping the field without this would make the report lie on the
editor, which is worse than the field being absent."
```

---

### Task 4: `ProjectOpenOptions` — keep the verify capture off `Saved/Diagnostics/`

**Files:**
- Create: `ArcaneClient/src/Arcane/Project/ProjectOpenOptions.hpp`
- Modify: `Project.{hpp,cpp}`, `Runtime.{hpp,cpp}`, `ArcaneClient/src/Arcane/Host/ProjectBoot.cpp`, `ArcaneEditor/src/App/EditorAppProject.cpp`
- Test: `ArcaneTests/src/ProjectTest.cpp`

**Interfaces:**
- Consumes: nothing from T1–T3.
- Produces: `struct Arcane::ProjectOpenOptions { bool mountDiagnostics = true; };`; `Project::Open(path, onProgress = {}, ProjectOpenOptions opts = {})`; `Runtime::OpenProject(path, onProgress = {}, ProjectOpenOptions opts = {})`.

**The parameter is DEFAULTED.** There are 41 `Project::Open` call sites, 39 of them in `ArcaneTests`; defaulting is what keeps them compiling untouched.

- [ ] **Step 1: Write the failing test**

Add to `ArcaneTests/src/ProjectTest.cpp`:

```cpp
TEST_CASE("Project::Open honours mountDiagnostics", "[project]")
{
    // TempDir + WriteFile are this file's own helpers (ProjectTest.cpp:18, :29);
    // this is the same setup shape every other case here uses.
    const auto dir = TempDir("open_diag_opt_out");
    WriteFile(dir / "Aphelyon.arcproj",
              R"({ "formatVersion": 1, "name": "Aphelyon", "engine": { "abi": 4 } })");
    std::filesystem::create_directories(dir / "Content");
    std::filesystem::create_directories(dir / "Saved" / "Diagnostics");

    // Default: the mount IS registered when the directory exists.
    auto withDiag = Arcane::Project::Open(dir);
    REQUIRE(withDiag.has_value());
    CHECK(withDiag->Mounts().HasMount("diag"));

    // Opted out: not registered, even though the directory is right there.
    // This is what makes the editor's verify capture independent of the
    // machine's crash history.
    Arcane::ProjectOpenOptions opts;
    opts.mountDiagnostics = false;
    auto without = Arcane::Project::Open(dir, {}, opts);
    REQUIRE(without.has_value());
    CHECK_FALSE(without->Mounts().HasMount("diag"));

    // The opt-out is SCOPED to diag:// -- game:// must be unaffected, or the
    // fix has broken asset resolution rather than narrowed it.
    CHECK(without->Mounts().HasMount("game"));
}
```

The accessor is `MountTable::HasMount(std::string_view)` (`MountTable.hpp:23`) — verified, not assumed.

- [ ] **Step 2: Run and watch it fail for the right reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[project]"
```
Expected: **compile error** — no `ProjectOpenOptions`.

- [ ] **Step 3: Create the options header**

Create `ArcaneClient/src/Arcane/Project/ProjectOpenOptions.hpp`:

```cpp
#pragma once

// Per-open settings for Project::Open, threaded from HostConfig through
// Runtime::OpenProject.
//
// WHY THIS EXISTS: the editor's verify capture included the Assets panel,
// which enumerates <root>/Saved/Diagnostics -- a directory the editor itself
// writes crash and hang captures into. The golden image therefore moved with
// the machine's failure history (measured: 24 anti-aliased pixels of the
// Assets scrollbar thumb, byte-identical coordinates on both backends).
//
// Fixed HERE and not in the editor because the mount is created in the engine
// (Project.cpp's diag:// branch); a symptom fixed one layer up comes back.
//
// Intended to absorb future per-open engine settings -- this is a struct on
// purpose, not a bool wearing a struct's clothes.

namespace Arcane
{
    struct ProjectOpenOptions
    {
        // Register diag:// -> <root>/Saved/Diagnostics when that directory
        // exists. Default true: every ordinary open wants it, and every one of
        // the 41 existing Project::Open call sites keeps its old behaviour
        // without being touched.
        bool mountDiagnostics = true;
    };
}
```

- [ ] **Step 4: Thread it through**

`Project.hpp:39` — add the defaulted third parameter:

```cpp
        static std::optional<Project> Open(const std::filesystem::path& pathOrFile,
                                           AssetRegistry::ScanProgressFn onProgress = {},
                                           ProjectOpenOptions opts = {});
```

`Project.cpp:247` — gate the mount:

```cpp
        const std::filesystem::path diagDir = root / "Saved" / "Diagnostics";
        if (opts.mountDiagnostics && std::filesystem::is_directory(diagDir, ec))
```

`Runtime.hpp:102` / `Runtime.cpp:392` — same defaulted parameter, forwarded to `Project::Open` at `:394`.

`ProjectBoot.cpp:200` — pass `ctx`'s options through (add a `ProjectOpenOptions` field to the boot context and populate it from `HostConfig`: `opts.mountDiagnostics = !(cfg.headless && (!cfg.compareReference.empty() || !cfg.reportPath.empty()));`).

**Do not modify `EditorAppProject.cpp:545`'s call** — it is a manifest probe, wants the default, and compiles unchanged.

- [ ] **Step 5: Run the suite**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```
Expected: PASS, all 41 call sites still compiling. **State the actual count.**

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "fix(project): ProjectOpenOptions -- keep the verify capture off Saved/Diagnostics

The editor's verify capture enumerated a directory the editor itself writes
crash captures into, so the golden image moved with the machine's failure
history -- 24 anti-aliased pixels of the Assets scrollbar thumb.

Fixed in the engine, where the mount is created, rather than in the editor:
a symptom fixed one layer up comes back. The parameter is DEFAULTED, so all
41 Project::Open call sites keep compiling untouched."
```

---

### Task 5: ArcaneHub reports a compare failure as a compare failure

**Files:**
- Modify: `ArcaneHub/src-tauri/src/launch.rs:104`, `:585`

**Interfaces:**
- Consumes: nothing. Independent of T1–T4.
- Produces: nothing consumed later.

- [ ] **Step 1: Replace the test that cannot fail**

`launch.rs:585`'s `golden_run_sees_golden_vocabulary_only` passes by asserting the same retired vocabulary the function keys on, so it proves nothing. Replace it:

```rust
    #[test]
    fn golden_run_keys_on_flags_the_engine_actually_registers() {
        // --compare is the ONLY flag that can produce a post-boot exit 3.
        assert!(golden_run(&args(&["--compare", "runtime-scene"])));
        assert!(golden_run(&args(&["--headless", "--frames", "60", "--compare", "editor-ui"])));

        // THE BUG THIS PINS: --golden-* is retired vocabulary that no engine
        // binary registers. Keying on it made the exit-3 guard always match,
        // so a real compare failure was reported as "already open in another
        // editor" and the correct arm was unreachable.
        assert!(!golden_run(&args(&["--golden-capture", "D:/out"])));

        // Neither is --frames evidence on its own -- it cannot produce a
        // post-boot 2 or 3, which is why it was deliberately absent before.
        assert!(!golden_run(&args(&["--frames", "5"])));
        assert!(!golden_run(&args(&[])));
    }
```

- [ ] **Step 2: Run it and watch it fail for the right reason**

```bash
cd ArcaneHub\src-tauri
cargo test golden_run
```
Expected: **FAIL** on the first assertion — `--compare` does not start with `--golden-`.

- [ ] **Step 3: Key on the real vocabulary**

`launch.rs:104`:

```rust
/// `--compare` is the only flag that can produce a post-boot exit 3: it is
/// what asks for a golden comparison at all. `--headless`, `--settle` and
/// `--report` are all reachable without one, and `--frames` on its own cannot
/// produce a post-boot 2 or 3, which is why none of them is evidence here.
fn golden_run(extra: &[String]) -> bool {
    extra.iter().any(|a| a == "--compare" || a.starts_with("--compare="))
}
```

Update the doc comment above it that still names `--golden-`.

- [ ] **Step 4: Run the Hub's tests**

```bash
cd ArcaneHub\src-tauri
cargo test
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ArcaneHub/src-tauri/src/launch.rs
git commit -m "fix(hub): golden_run keys on --compare, not retired --golden-* vocabulary

No --golden-* flag exists in any engine binary, so the exit-3 guard at
launch.rs:335 ALWAYS matched: a real golden compare failure was reported to
the user as \"that project is already open in another editor\", and the
correct arm was unreachable.

Its old test passed by asserting the same retired vocabulary -- a test that
could not fail. The replacement asserts against flags the engine actually
registers and pins the bug directly."
```

---

### Task 6: Desk checkpoint — USER

**This task is the user's and cannot be marked complete by an agent.** It needs a GPU and a display.

- [ ] **A. Re-bless `editor-ui` — legitimate ONLY now that T4 has landed**

```
cd D:\dev\starworks\Arcane
scripts\desk-verify-servitor.ps1 -Phase B
```

Then bless the editor reference from the SOURCE tree, not a build artifact:

```
bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --compare editor-ui --bless
```

**`--bless` writes to the STAGED tree** — point `--project` at the source or you bless a build artifact.

- [ ] **B. Both editor lanes must go GREEN with no advisory switch**

```
scripts\golden-gate.ps1 -Configuration Debug
type bin\Debug-windows-x86_64-md\golden-gate-summary.json
```

Expect **all four lanes PASS with `diffCount=0`**, `"gatePassed": true` and `"advisoryFailures": 0`. Assert on those two fields, **never the exit code**.

**If either editor lane is still red, STOP — do not proceed to T7.** A red lane means T2 or T4 did not fully close its defect, and deleting the switch would hide that.

- [ ] **C. Confirm the settle change did not slow the gate**

Note the wall-clock time of the run in B against the pre-arc baseline. The 50 ms interval should cost ~5 s per lane at worst. If it costs materially more, say so — escalating backoff with a low cap is the recorded fallback in the spec.

---

### Task 7: Delete `-AdvisoryLanes` — the tracking item closes itself

**Files:**
- Modify: `scripts/golden-gate.ps1`, `Jenkinsfile:120-121`

**Interfaces:**
- Consumes: Task 6's evidence that both editor lanes are green. **Do not start this task without it.**

- [ ] **Step 1: Delete the switch and everything that served it**

From `scripts/golden-gate.ps1`: the `$AdvisoryLanes` parameter (`:95`), its validation and announcement block (`:116-169`), `Write-OwedDefects` entirely, the `$isAdvisory` logic (`:295`), the KNOWN-RED branches (`:446-460`), the `advisoryLanes`/`advisoryFailures` summary fields (`:505-507`), and the known-red epilogue (`:532`).

**Keep `gatePassed` and the per-lane `verdict` fields** — consumers assert on those, and `advisoryFailures` should simply disappear rather than be pinned at 0. The header's whole "THE ADVISORY LANES" section goes with it.

- [ ] **Step 2: Delete the Jenkinsfile arguments**

`Jenkinsfile:120-121` — remove ` -AdvisoryLanes ArcaneEditor` from both invocations. Delete the explanatory comments at `:69` and `:132` that exist only to describe the switch.

- [ ] **Step 3: Verify the gate script still parses and its help is coherent**

```bash
powershell -NoProfile -ExecutionPolicy Bypass -Command "$null = [ScriptBlock]::Create((Get-Content -Raw scripts\golden-gate.ps1)); 'parses OK'"
git grep -n "AdvisoryLanes\|advisoryFailures\|Write-OwedDefects" -- scripts Jenkinsfile
```
Expected: `parses OK`, and the grep returns **no matches**. A surviving mention is a partial deletion.

- [ ] **Step 4: Commit**

```bash
git add scripts/golden-gate.ps1 Jenkinsfile
git commit -m "chore(gate)!: delete -AdvisoryLanes -- both editor lanes are green

The switch was its own tracking item, by its own header's instruction:
\"WHEN BOTH LAND, DELETE -AdvisoryLanes AND the Jenkinsfile argument that
passes it.\" Defects 1 and 2 have landed and the desk pass confirmed all
four lanes at diffCount=0, so the condition is met.

advisoryFailures disappears from golden-gate-summary.json rather than being
pinned at 0: a field that can only ever read 0 is a stale marker, which is
the failure this deletion exists to avoid. Write-OwedDefects goes with it."
```

---

## Notes for whoever executes this

- **Plan-supplied code is unrun code.** Every snippet was written against `ff10d562` with citations re-derived rather than recalled — but if one does not compile, fix it and say so in the report. Do not contort the design to match a plan that was wrong.
- **Ask of every check: would this FAIL if the thing it names were wrong?** T1 Step 2, T2 Step 7, T3 Step 2, T5 Step 2 and T7 Step 3 are all written to be capable of failing. Keep them that way.
- **The two settle loops are a line-for-line port of each other.** Every arc that has touched one and not the other has produced a defect. T2 Step 10 and T3 Step 4 both span both hosts.
- **Do not assume `ShaderCompiler::IsIdle()` is meaningful.** Its non-`_WIN32` stub returns `true` unconditionally, so on Linux the quiescence conjunct is vacuous and the time bound becomes the only real convergence signal. Nothing in this plan depends on `IsIdle()` being truthful — the bail predicate is independent of it, and a longer timeout is strictly safer there — but do not "simplify" the conjunction by leaning on it.
- **T6 gates T7 absolutely.** An agent cannot close this arc alone, and deleting the switch on anything less than a green four-lane run would hide the very defect it was tracking.
