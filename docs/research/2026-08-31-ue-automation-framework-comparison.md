# UE 5.6.1 automation framework vs Arcane's — read from source

**Status:** research, 2026-08-31. Complements (does not supersede)
`docs/research/2026-08-26-unreal-screenshot-comparison-research.md`, which was written from
PUBLIC DOCS only. This one is read from the vendored tree at `.example/UnrealEngine-release/`
(UE **5.6.1**, per `Engine/Build/Build.version`). Every claim carries a `file:line`.

---

## 1. The comparator — we are AHEAD on pixel classification, BEHIND on spatial bounds

**Ours** (`ArcaneClient/src/Arcane/Assets/ImageCompare.cpp`, ported from Playwright's
`compare.ts` — hence `ThirdParty/playwright-fixtures/`): a 4-stage perceptual cascade —
exact equality -> **CIE94 dE** (`ColorDeltaE94`, `:66`) -> flood-fill/variance (`:366`) ->
**SSIM over a 31x31 window** (`kSsimWindowRadius = 15`, `:277`), AA classified at
`kSsimAntialiasing = 0.99` (`:279`). Budgets: `maxDiffPixels` + `maxDiffPixelRatio`
(smaller wins; zero if neither set) + `maxColorDeltaE94 = 1.0`.

**Theirs** (`Developer/ScreenShotComparisonTools/`): per-channel BYTE thresholds plus a
resemble.js-style hue heuristic. `FPixelOperations::IsAntialiased`
(`Private/ImageComparer.cpp:151`): 3x3 neighbourhood; AA if >1 sibling differs in hue by >0.3,
OR >1 high-contrast sibling, OR <2 identical siblings. Their SSIM exists but is a **separate**
entry point (`CompareStructuralSimilarity`, `:448`; K1=0.01, K2=0.03, 8x8 windows, Luminance or
Color), NOT part of the main compare path.

**Their tolerance struct** (`Public/ImageComparer.h:28`): `Red/Green/Blue/Alpha`,
`MinBrightness/MaxBrightness`, `IgnoreAntiAliasing`, `IgnoreColors`, **`MaximumLocalError`**,
**`MaximumGlobalError`**. Presets (`Private/ImageComparer.cpp:19-22`):

- `IgnoreNothing`      (0,0,0,0, 0,255, false,false, **0.00, 0.00**)  <- their DEFAULT
- `IgnoreLess`         (16,16,16,16, 16,240, false,false, 0.02, 0.02)
- `IgnoreAntiAliasing` (32,32,32,32, 64,96, **true**,false, 0.02, 0.02)
- `IgnoreColors`       (16,16,16,16, 16,240, false,**true**, 0.02, 0.02)

**CORRECTED 2026-09-01 (was: "Epic's default is zero-tolerance, same as ours") — that was
wrong.** `DefaultIgnoreNothing` is the strictest PRESET, but it is not what the screenshot path
selects. `FAutomationScreenshotOptions`'s default ctor
(`Developer/FunctionalTesting/Public/AutomationScreenshotOptions.h:73-90`) sets
`Tolerance(EComparisonTolerance::Zero)` **but also `MaximumLocalError(0.10f)`,
`MaximumGlobalError(0.02f)` and `bIgnoreAntiAliasing(true)`** — only the PER-CHANNEL colour
tolerance is zero; the spatial budgets are 10% local / 2% global. And when a screenshot's
metadata carries no tolerance rules at all, `FScreenShotManager::CompareScreenshot` falls back to
`FImageTolerance::DefaultIgnoreLess` with `IgnoreAntiAliasing = true`
(`ScreenShotComparisonTools/Private/ScreenShotManager.cpp:274-276`) — looser still.
**Our zero budget is materially stricter than Epic's, in both directions.**

### THE knob we lack — spatial concentration

`Private/ImageComparer.cpp:311-397`. A **10x10 grid of 100 blocks** via spatial hash
`(Y/BlockSizeY)*10 + (X/BlockSizeX)`; mismatches counted per block; then
`MaxLocalDifference = max(LocalMismatches[i]) / (BlockSizeX*BlockSizeY)` and
`GlobalDifference = MismatchCount / (W*H)`.

Our budgets are **purely aggregate**: 500 scattered pixels and 500 pixels forming one broken
widget score IDENTICALLY. Theirs fails the second. One extra accumulator array over a loop we
already run.

**Their size-mismatch policy is WORSE than ours** — forced failure with local=global=1.0
(`:401-413`), discarding the comparison. We pad, still compare, and report the mismatch as its
own named fact. Do not adopt.

---

## 2. Declarable exclusion — Finding Q4-F1, now seen in source

`Runtime/AutomationTest/Public/AutomationTestExcludelist.h:188` —
`FAutomationTestExcludelistEntry { Platforms, Test, Reason, RHIs, Warn }`.

Richer than the doc-derived research could see:

- **`RHIs` scopes by RHI AND FEATURE LEVEL** — `ETEST_RHI_Options` (DX11/DX12/Vulkan/Metal/Null)
  x `ETEST_RHI_FeatureLevel_Options` (SM5/SM6); `NumRHIType()` (`:215`) counts how many
  dimensions must match.
- **The reason MAY compose beautified text + a ticket string** — `UpdateReason`
  (`Private/AutomationTestExcludelist.cpp:49-83`) concatenates `BeautifiedReason + FullTicketString`.
  **CORRECTED 2026-09-01 (was: "Exclusions are tied to tracker entries — that is what stops them
  becoming permanent"). That claim was mine, not Epic's, and the source does not support it.**
  The ticket is OPTIONAL (`if (TaskTrackerTicketId.IsEmpty()) { Reason = BeautifiedReason; }`,
  `:51-54`), the whole function is `#if WITH_EDITOR` so a hand-edited config entry never passes
  through it, `Reason` is a bare `FName` never validated as non-empty (`Public/...h:170-172`), and
  **there is no expiry, deadline or review concept anywhere in the mechanism.** Nothing stops a UE
  exclusion becoming permanent. Treat their entry as a SCOPING model only.
- Config-driven (`UCLASS(config = Engine, defaultconfig)`), enforced at
  `FAutomationWorkerModule::IsTestExcluded`
  (`Runtime/AutomationWorker/Private/AutomationWorkerModule.cpp:578`, called `:657`), yielding a
  `SkipReason`.

Gauntlet has a SECOND, weaker one: `DenylistEntry { TestName, Platforms[], BranchName }`
(`Programs/AutomationTool/Gauntlet/Framework/Base/Gauntlet.Denylist.cs:16`) — adds **branch**
scoping but has **no Reason**. The engine-level one is the better base.

**We have nothing.** Arc 2 deleted `-AdvisoryLanes` and replaced it with nothing.

---

## 3. Verdict vocabulary — fixes a defect we patched bespoke on 2026-08-31

`Gauntlet/Framework/Base/Gauntlet.TestNode.cs:19` —
`TestResult { Invalid, Passed, Failed, WantRetry, Cancelled, TimedOut, InsufficientDevices }`

This distinguishes **"the subject failed"** from **"we could not run the subject"**. That is
EXACTLY the hole the whole-branch review found in `-SelfTest`, where `FAIL` was an umbrella
covering the script's own synthetic `exe not found` verdict. We patched it with a bespoke
`$exeMissingCount`. **A verdict enum is the root fix; `InsufficientDevices` is literally our
case.** Do NOT adopt `WantRetry` — it contradicts the zero-flake policy.

Also `TestStatus`, `StopReason {Completed, MaxDuration}`, `TestPriority`,
`EventSeverity {Info, Warning, Error, Fatal}` (`:33-70`), and
`EFunctionalTestResult { Default, Invalid, Error, Running, Failed, Succeeded }`
(`Developer/FunctionalTesting/Classes/FunctionalTest.h:195`), where `Default` means "let the
assertions decide".

---

## 4. `ITestNode` lifecycle — two cheap ergonomics worth stealing

`Gauntlet.TestNode.cs:111`: `IsReadyToStart()` -> `StartTest(Pass, NumPasses)` -> `TickTest()`
-> `StopTest(StopReason)` -> `CleanupTest()`, plus `SetCancellationReason(string)`,
`AddTestEvent`, `RestartTest()`.

- **`GetRunLocalCommand(LaunchingBuildCommand)` (`:227`)** — every test prints the exact command
  to reproduce it locally. Our gate reports lane names and diff paths but never "run this to
  reproduce". Tiny; large ergonomic payoff.
- **`IsReadyToStart()` (`:150`)** — a readiness gate. This is the `-Preflight` idea, formalised.

---

## 5. Per-test capture contract — `FAutomationScreenshotOptions`

`Developer/FunctionalTesting/Public/AutomationScreenshotOptions.h`. Authored **per test, in the
editor**; ours is per-invocation CLI flags.

- **`Delay(0.2f)` + `FrameDelay(5)`** (`:75-76`) — the time-and-frame conjunction. **We
  INDEPENDENTLY arrived at the same shape**: `--settle` conjoins frame-stability with shader
  compiler `IsIdle()` and bounds by BOTH attempts and time (`HostConfig.hpp:113,126`). Ours is
  arguably stronger — compiler-idle is a real quiescence signal, not a fixed delay.
- **`OverrideTimeTo` + `bOverride_OverrideTimeTo` (`:132-139`)** — pins ABSOLUTE world time.
  **A determinism lever we lack.** `--fixed-dt` pins the timestep; this pins the clock.
- **`bDisableNoisyRenderingFeatures` (`:149`)** and **`bDisableTonemapping` (`:156`, shown as
  "Disable Eye Adaptation")** — capture-time noise suppression as a DECLARED per-test option.
- `EComparisonTolerance::Zero` is the default (`:83`); `FComparisonToleranceAmount
  {R,G,B,A,MinBrightness,MaxBrightness}`; `MaximumLocalError` clamped 0..1 (`:196`).
- Lifecycle `PrepareTest -> IsReady -> StartTest -> RequestScreenshot -> OnScreenShotCaptured ->
  OnScreenshotTakenAndCompared -> FinishTest`, plus `OnTimeout` and explicit view/viewport
  save-restore (`Classes/ScreenshotFunctionalTestBase.h:31-91`).

---

## 6. Reference variant axes — Q2, in source

`ScreenShotComparisonTools/Public/ScreenShotComparisonSettings.h`:
`FScreenshotFallbackEntry { Parent, Child }` — a config-driven fallback CHAIN between platforms,
plus `bUseConfidentialPlatformPathsForSavedResults`.

Ours is a hardcoded `vulkan/` subdirectory with `editor-ui` as a shared slot. Theirs is the
shape to copy WHEN a configuration axis actually exists — not before.

---

## 7. Test flag taxonomy — four ORTHOGONAL axes

`Runtime/Core/Public/Misc/AutomationTest.h:85`:

1. **Application context** — Editor/Client/Server/Commandlet/Program (where it may run)
2. **Feature requirement** — **`NonNullRHI`**, `RequiresUser`
3. **Priority** — Critical/High/Medium/Low (+ `_HighPriorityAndAbove` masks, `:139-142`)
4. **Speed/kind** — Smoke/Engine/Product/Perf/Stress/**Negative**

Plus **`Disabled`** (`:108`) — "fast disabling of tests without commenting code out".

Two matter to us:

- **`NonNullRHI` is a declared capability requirement — but CORRECTED 2026-09-01, it is NOT a
  device probe and it does NOT report a skip.** `FAutomationTestFramework::GetValidTestNames`
  (`Runtime/Core/Private/Misc/AutomationTest.cpp:772-805`) decides `bUsingNullRHI` from
  `-nullrhi` on the command line, `IsRunningCommandlet()` or `IsRunningDedicatedServer()` — its own
  comment says so: *"@todo: Handle this correctly... For now, assume Null RHI is only used for
  commandlets, servers, and when the command line specifies to use it."* It never asks the machine
  whether an adapter exists. Failing `bPassesFeatureRequirements` (`:805`) removes the test from the
  ENUMERATED LIST, so the run silently omits it rather than reporting it omitted — the same vacuity
  their own `successes > 0` guard exists to catch. **Take the declared-requirement IDEA; the
  mechanism to build is a real probe plus a reported skip, which is strictly better than this.**
  Our `[gpu]` conflated a (now-retired) driver hazard with a baseline-comparability convention —
  the exact confusion that cost 2026-08-31.
- **`NegativeFilter` — "tests whose correct expected outcome is failure"** — a first-class slot
  for what `-SelfTest` is.

---

## 8. Telemetry — mechanises what we track BY HAND

`Gauntlet/Framework/Base/Gauntlet.TestReport.cs:97` —
`TelemetryData { TestName, DataPoint, Measurement, Context, Unit, Baseline }` +
`ITelemetryReport`; `ITestReport` carries a `Version` and a `Metadata` dictionary.

A measurement WITH A UNIT AND A BASELINE is first-class. We track "52298 assertions / 1277
cases" and gate baselines by hand in memory notes. This is the mechanism for that.

---

## 9. Where WE are ahead

- **`-SelfTest` as an END-TO-END NEGATIVE CONTROL.** `Gauntlet/SelfTest/` exists but unit-tests
  Gauntlet's OWN components (log parser, timeouts, param parsing, order-of-ops, device
  targeting). It does not break the subject and assert the whole gate goes red. Ours tests the
  property; theirs tests the parts. (This CORRECTS an over-strong "no equivalent" claim made
  earlier in the session.)
- **Perceptual pixel classification** — CIE94 dE + SSIM cascade vs byte thresholds + hue heuristic.
- **Machine-readable output designed for an AGENT consumer** — `VerifyReport` schemaVersion 3 +
  per-lane `golden-gate-summary.json`. Theirs targets a human in the Screenshot Browser.
- **Size-mismatch handling** (see section 1).

---

## 10. Do NOT inherit

- `WantRetry` / `RestartTest()` / multi-pass — contradicts "deterministic or it is a bug".
- Their 2% relaxed presets — contradicts the zero-budget policy.
- Controller/Worker message-bus split (`AutomationController` <-> `AutomationWorker` over
  `AutomationMessages`) — exists for device farms; we have one machine.
- `AutomationDriver` synthetic input — F4 owns editor UX.
- **BuildGraph** (`Programs/AutomationTool/BuildGraph/`: `BgCondition`, `BgTask`,
  `BgNodeExecutor`, `BgSchema`, `BgScriptReader/Writer`) — a declarative node-graph DSL for
  multi-agent, multi-platform build farms. Judged OUT OF SCOPE **from its structure, not a
  line-by-line read** — stated plainly so nobody mistakes this for a deep review.

---

## Proposed scope — the LAST automation arc

**Tier 1 (defect-class, all four):**

1. Verdict vocabulary replacing umbrella `FAIL` (section 3)
2. Spatial-concentration knob, `MaxLocalDifference` (section 1)
3. Declarable exclusion — config-driven, ticket-linked reason, backend + feature-level scoped (section 2)
4. Host-level witness harness — `settleCaptureFailed`, the `compare-missing-reference` absence,
   `--settle-timeout` actually spent. `ArcaneTests` links neither host. Newly buildable now that
   an agent can drive hosts unattended.

**Tier 2 (cheap, high leverage):**

5. `GetRunLocalCommand()` — repro command in every failure report (section 4)
6. Telemetry with unit + baseline (section 8)
7. Untangle `[gpu]`: capability requirement vs comparability convention (section 7)
8. Absolute-time pinning, `OverrideTimeTo`-style (section 5)
9. Named tolerance presets, default stays zero (section 1)
10. `IsReadyToStart()` / `-Preflight` readiness gate (section 4)

**Tier 3 (needs a reason to exist first):** SSIM as a reported score; config-driven reference
fallback chain (needs a second axis); brightness-window / IgnoreColors; Smoke/priority tiers
(needs a bigger corpus); per-test authored tolerance (F4 territory).

---

## What was NOT read

`FunctionalTesting`'s latent-action internals; the CVar list behind
`bDisableNoisyRenderingFeatures`; `AutomationController`/`Worker` reporting pipeline internals;
`ScreenShotManager` fallback-resolution mechanics; Horde integration; and —
**the highest-value unexplored area — `Programs/LowLevelTests/`, which is UE's CATCH2 harness.**
`ArcaneTests` is Catch2, so their Catch2 integration is the most directly comparable thing in the
whole tree and was never opened.

---

# ADDENDUM — 2026-09-01: the three unread areas, read from source

Closes the three items this doc's own "What was NOT read" section named as
highest-value: `Programs/LowLevelTests/` (UE's Catch2 harness), the CVar list behind
`bDisableNoisyRenderingFeatures`, and `ScreenShotManager`'s fallback resolution. Same tree
(`.example/UnrealEngine-release/`, UE 5.6.1); paths below are relative to `Engine/Source/`
unless noted. **A 2026-09-01 verification pass against UE source subsequently retracted four
claims made above; each is marked CORRECTED in place, and they are listed together in section E.**

---

## A. UE's Catch2 harness — the most directly comparable code in their tree

`Programs/LowLevelTests/` is only the *test targets* (`FoundationTests`, `MathCoreTests`,
`StateStreamTests`). The harness is `Developer/LowLevelTestsRunner/` — **2,847 lines of C++**
(`wc -l` over its `.cpp`/`.h`) plus an 801-line `README.md` that is the best-written artifact
in the whole comparison.

### A.1 The runner is thin — and ours is thinner still

`Private/TestRunner.cpp:290-338` (`RunTests`) is the whole entry point:

```
ReadAndAppendAdditionalArgs(exe path)   :307   <- command line may come from a FILE
ParseCommandLine                        :316
SleepOnInit                             :318
GlobalSetup                             :320
Catch::Session().run(...)               :336   (via RunCatchSession, :283-286)
ON_SCOPE_EXIT { GlobalTeardown; Terminate; UnloadModulesAtShutdown; RequestEngineExit; GLog->TearDown }  :322-334
```

Ours is `ArcaneTests/src/test_main.cpp:18-34`: pin the shared Astra TypeContext, pin
`Arcane.dll`'s module slot via a throwaway `Runtime`, `return Catch::Session().run(argc, argv)`.
Same shape. **We have no equivalent of their steps 1, 2, 3, or the teardown scope** — and three
of those four are worth having (below).

### A.2 THE FINDING: engine assertions are routed INTO Catch2, not around it

Two mechanisms, both absent from ours:

- **`GError` swap** — `Private/TestRunner.cpp:218-225` replaces the platform error device with
  `FTestRunnerOutputDeviceError`, whose `Serialize` (`Private/TestRunnerOutputDeviceError.cpp:12-18`)
  builds a `Catch::AssertionHandler` and calls `handleMessage(ResultWas::ExplicitFailure, ...)`.
  Its own comment: *"report error back to catch otherwise failed `check`'s don't fail the tests"*.
- **`ensure` forwarding** — `Private/TestRunner.cpp:229-235` hooks
  `FCoreDelegates::OnHandleSystemEnsure` and pushes `GErrorHist` into
  `Catch::getResultCapture().handleMessage(...)` as an `ExplicitFailure`. Comment: *"forward
  unhandled `ensure` to catch to force tests to fail. test will continue to execute"*.

**We have `ARC_ASSERT` / `ARC_VERIFY` / `ARC_ENSURE` (`ArcaneClient/src/Arcane/Base/Assert.hpp:21-23`,
forwarding to Mosaic) and no equivalent routing.** Today an engine assertion firing inside a test
is whatever Mosaic does with it — not a Catch2 assertion failure attributed to the running test
case. That is a genuine attribution gap, not a stylistic difference.

### A.3 And the inverse: asserting that an assertion FIRES

`Public/TestMacros/Assertions.h:9-30` gives eight macros:

```
REQUIRE_ENSURE / CHECK_ENSURE / REQUIRE_ENSURE_MSG / CHECK_ENSURE_MSG
REQUIRE_CHECK  / REQUIRE_CHECK_MSG / REQUIRE_CHECK_SLOW / REQUIRE_CHECK_SLOW_MSG
```

Each wraps the expression in an `FEnsureScope` / `FCheckScope` (`Runtime/Core/Public/Tests/`),
runs it, and fails Catch2 if `scope.GetCount() == 0` — *"Expected failure of `check` not
received"* (`:66`). The `_MSG` variants also match the message text (`:79`).

This is **`NegativeFilter` at the assertion level** — the first-class slot section 7 identified,
implemented. Our `-SelfTest` proves the *gate* can fail; this proves an individual *guard* fires.
Complementary, not substitutes.

### A.4 Their timeout is a LOG LINE parsed by a string match in C#

`Private/TestListeners/EventListener.cpp:29-50` spawns a detached thread on the first test case
(`:75-77`) that sleeps `GetTimeoutMinutes()` and then, if the test name has not changed,
`UE_CLOG(..., Error, TEXT("Timeout detected: Test case \"%hs\" has been running for more than
%d minute(s)"), ...)` (`:40-41`).

It does **not** abort anything. Termination is Gauntlet's, via
`Programs/AutomationTool/LowLevelTests/Tests/Gauntlet.LowLevelTestNode.cs:370-384`:
`Line.Contains("Timeout detected")`.

Two things to take from this and one not to:

- **Do not take** the design: a cross-language contract carried by an English log substring,
  where the C++ side cannot enforce anything. Also `TimeoutThread = new std::thread(...)`
  (`:77`) is never joined or deleted and its body is `while (true)` — a leaked thread per run.
- **Do take** the *distinction* it implies, which appears properly on the C# side (A.6): a
  **progress/inactivity** bound is not a **total-duration** bound.
- **Do take** the per-test granularity. Our timeouts are per-phase (`--settle-timeout`); we have
  no "this one test case has hung" signal at all.

### A.5 Their reporter is hand-written because their Catch2 is old — ours is not

`Private/UnrealReporter.cpp:25-163` is a custom `StreamingReporterBase` registered as
`CATCH_REGISTER_REPORTER("unreal", FUnrealReporter)` (`:163`), emitting a minimal
`<testrun>/<testcase>/<failure>/<result success= duration=>` XML. It sets
`shouldRedirectStdOut = true` and `shouldReportAllAssertions = true` (`:34-35`) and emits
`skipped="true"` for skipped cases (`:76-81`).

**Why they wrote it:** their vendored Catch2 is `ThirdParty/Catch2/v3.4.0` (also `v3.0.1` and a
legacy `2.13.6`). Ours is **3.15.0** (`ThirdParty/Catch2/extras/catch_amalgamated.hpp:7571-7573`)
— eleven minors ahead — and ships a built-in `JsonReporter`
(`extras/catch_amalgamated.hpp:14113`). We do not need to write one.

Two more places our newer Catch2 makes a UE-style fork unnecessary:

- UE **forked Catch2** to add `catch_active_test.{hpp,cpp}` and the `GROUP_*` lifecycle
  (`ThirdParty/Catch2/v3.4.0/src/catch2/catch_active_test.hpp`, `catch_test_group_info.hpp`,
  `interfaces/catch_interfaces_group.hpp`,
  `internal/catch_test_group_event_registry_impl.hpp` — none exist upstream). Their
  `getActiveTestName()` (`catch_active_test.cpp:18-23`) is available to us unforked as
  `IResultCapture::getCurrentTestName()` (`catch_amalgamated.hpp:1129`).
  **If a future task wants the running test's name, do not fork Catch2.**
- **Zero tests already fails for us.** `Config::zeroTestsCountAsSuccess()`
  (`extras/catch_amalgamated.cpp:911`) is driven by `--allow-running-no-tests`
  (`:3586-3587`), off by default. UE has to enforce the same property by hand (A.6).

### A.6 The verdict machine — `Gauntlet.LowLevelTestNode.cs`, and it corroborates our rule

`Programs/AutomationTool/LowLevelTests/Tests/Gauntlet.LowLevelTestNode.cs:170-330` (`StopTest`)
is the closest analogue to our `golden-gate.ps1` verdict block. Its precedence:

| Order | Condition | Verdict | Reason string |
|---|---|---|---|
| 1 | `WasKilled` && (`MaxDuration` \|\| already TimedOut) | `TimedOut` | "Timed Out" (`:250-254`) |
| 2 | `WasKilled` otherwise | `Failed` | "Process was killed by Gauntlet with reason {InReason}" (`:257-258`) |
| 3 | `ExitCode != 0` | `Failed` | "Process exited with exit code {N}" (`:261-265`) |
| 4 | report copy threw | `Failed` | **"Unable to read test report"** (`:266-270`) |
| 5 | report present | parse it (`:271-307`) | "Tests passed/failed according to xml report" |
| 6 | no report | exit code decides | **"(no report to parse)"** (`:308-320`) |

**The absence of a report is its own named outcome at two different levels** — a copy that
failed (4) and a report that was never requested (6) get different strings. This is an
independent arrival at the rule our gate already holds
(`scripts/golden-gate.ps1:589-606`: *"COULD NOT DETERMINE pass/fail... treat this as
unresolved"*). Two codebases, same conclusion, reached separately.

**And the vacuity guard, mechanised** — `Utility/LowLevelTests.ReportParser.cs:41-54`:

```csharp
return NrOverallResultsFailures == 0 && NrOverallResultsCasesFailures == 0
    && NrOverallResultsSuccesses > 0 && NrOverallResultsCasesSuccesses > 0;
```

Initialised to `-1` (`:36-39`), so a missing `<OverallResults>` element yields `false`; an
unparseable document sets `IsValid = false` and short-circuits (`:22-33`). **A run that passed
nothing is not a pass.** This is `feedback_default_values_are_not_measurements` written as code.

**Their console path is much weaker — do not copy it.** `Utility/LowLevelTests.LogParser.cs:135`:
`TestResults.Passed = string.IsNullOrEmpty(TestCaseResult.Failed) || Convert.ToInt32(...) == 0;`
— if the regex finds a total-cases line but no `N failed` line, that reads as PASS. The XML path
has the property; the regex path is a fallback that quietly lacks it.

**The inactivity watchdog** (`:145-163`) is separate from `MaxDuration` (`:36`, 30 min): if no
new log lines arrive for `Timeout + 0.5` minutes, `TestResult.TimedOut`. Progress and duration
are two different bounds. We have neither at the process level.

### A.7 Command-line composition — two cheap ideas

`RunLowLevelTests.cs:1019-1049` assembles the child command line once, then sets
`CanAlterCommandArgs = false` (`:1077`) so nothing downstream can mutate it. Notable flags:

- `--filenames-as-tags` (`:1034`) — Catch2 auto-tags every case with its source filename, so
  `-# [#ImageCompareCascadeTest]` selects a file. Free in our Catch2, unused by us.
- `--durations=no` (`:1019`), `--out=<report path>` (`:1032`), `--debug --log` (`:1043-1044`)
- `--buildmachine` gated on `Environment.GetEnvironmentVariable("IsBuildMachine") == "1"` (`:1049-1051`)

`Private/TestRunner.cpp:94-184` is the child's side. The parse rule is worth stating exactly:
**every unrecognised argument falls through to Catch2** (`:179`), everything after `--extra-args`
goes to the application (`:116`), and `--break` is appended unconditionally (`:183`). And
`ReadAndAppendAdditionalArgs` (`:307`) lets the whole command line come from a file next to the
exe — a device-farm affordance we do not need.

### A.8 Test-target knobs (README, "Test Target Rules Reference")

`TestTargetRules` exposes `bUsePlatformFileStub` (swap `FPlatformFile` for an IO-disabling mock),
`bMockEngineDefaults`, `bNeverCompileAgainstEngine` / `CoreUObject` / `ApplicationCore` /
`Editor`, and `bWithLowLevelTestsOverride`. `TestModuleRules.TestMetadata` carries `TestName`,
`TestShortName`, `ReportType`, **`Disabled`**, `InitialExtraArgs`, `HasAfterSteps`, `UsesCatch2`,
`PlatformTags`, `PlatformCompilationExtraArgs`, `PlatformsRunUnsupported`.

`Public/TestHarness.h:98-102` also gives `DISABLED_TEST_CASE` / `DISABLED_SCENARIO` /
`DISABLED_SECTION`, which **compile the body out entirely** rather than skipping at runtime — a
third disabling mechanism alongside the `Disabled` metadata flag and the excludelist of section 2.
Three mechanisms for one concept is not a model to copy; the excludelist (ticket-linked,
config-driven) is still the right one.

`Public/TestCommon/Expectations.h` (435 lines) is a compatibility shim mapping the old
`TestEqual(What, Actual, Expected)` AutomationTest vocabulary onto `FAIL_CHECK`, with
`CHECK_EQUALS`/`CHECK_NOT_EQUALS` macros at `:430-433`. It lives in an **anonymous namespace in a
header** (`:18`) — one copy per TU. Migration scaffolding, not a design.

### A.9 Their runner self-test is two cases

`Tests/SelfTests.cpp:10-25`: `SKIP` works; `getActiveTestName`/`Tags` return the right strings.
This **confirms** section 9's corrected claim — Gauntlet's `SelfTest/` unit-tests the parts, and
this file tests two runner behaviours. Neither breaks the subject and asserts the gate goes red.
`-SelfTest` remains ours alone.

---

## B. The CVar list behind `bDisableNoisyRenderingFeatures`

`Developer/FunctionalTesting/Private/AutomationBlueprintFunctionLibrary.cpp`.

`FAutomationTestScreenshotEnvSetup` declares thirteen `FConsoleVariableSwapperTempl` members by
CVar name at `:238-250`. `Setup()` (`:258-317`) applies them:

**When `bDisableNoisyRenderingFeatures` (`:264-273`):**

| CVar | Value |
|---|---|
| `r.AntiAliasingMethod` | 0 |
| `r.DefaultFeature.AutoExposure` | 0 |
| `r.DefaultFeature.MotionBlur` | 0 |
| `r.MotionBlurQuality` | 0 |
| `r.SSR.Quality` | 0 |
| `r.ContactShadows` | 0 |
| `r.EyeAdaptationQuality` | 0 |
| `r.TonemapperGamma` | 2.2 |

**Else when `bDisableTonemapping` (`:275-279`):** `r.EyeAdaptationQuality` 0, `r.TonemapperGamma`
2.2. Note the `else if` — the tonemapping flag is only reachable when the noisy-features flag is
off, which is exactly what its `EditCondition = "!bDisableNoisyRenderingFeatures"` metadata says
(`Public/AutomationScreenshotOptions.h:155`).

**Unconditionally, on every screenshot (`:281-296`):**

| CVar | Value | Purpose (their comment) |
|---|---|---|
| `r.DynamicRes.TestScreenPercentage` | 0 | "Completely disable dynamic resolution" |
| `r.DynamicRes.OperationMode` | 0 | idem, plus an `EmitDynamicResolutionEvent` End/Begin frame pair (`:288-289`) to force the change to take |
| `r.ScreenPercentage` | 100 | "Forces ScreenPercentage=100" |
| `r.SecondaryScreenPercentage.GameViewport` | 100 | **"Ignore High-DPI settings"** |

The mechanism is `FConsoleVariableSwapperTempl::Set` (`:106-137`), which records `OriginalValue`
on first touch and writes with **`SetWithCurrentPriority`** — *"I need these overrides to
supersede anything the user does while taking the shot"* (`:135`). `Restore()` (`:139-152`,
called from `FAutomationTestScreenshotEnvSetup::Restore` at `:319-352`) writes the original back
at the same priority.

### Two cautions that matter more than the list

1. **A declared knob is not an applied knob.** `TonemapperSharpen` (`r.Tonemapper.Sharpen`) is
   constructed at `:246` and declared at `Public/AutomationBlueprintFunctionLibrary.h:301`. It is
   **never `Set`**, and its `Restore()` is commented out at `:331`. Thirteen declared, twelve
   applied — and nothing in the type system or a test notices.
2. **The per-view path is dead code.** `FAutomationViewExtension::SetupViewFamily` (`:181-228`)
   has an `if (Options.bDisableNoisyRenderingFeatures)` block at `:204-221` whose body is
   **entirely commented out**, and the same for `bDisableTonemapping` at `:223-227`. The live
   show-flag path reads a separate `UAutomationViewSettings` asset (`:183-196`) — a different
   input from the boolean. The declared option and the mechanism that once implemented it drifted
   apart, and the abandoned half was left in place looking implemented.

   This is `feedback_instrument_blind_spots` in someone else's tree: the declared instrument
   became the definition of done, and nobody re-derived what it actually reached.

**`OverrideTimeTo` is live**, and it is in the view extension, not the CVars:
`InViewFamily.Time = FGameTime::CreateUndilated(Options.OverrideTimeTo, 0.0f)` (`:198-202`),
under the comment *"Turn off time the ultimate source of noise."* Section 5's determinism-lever
claim stands, and now has its implementation site.

**What this means for us.** We render offscreen with a fixed timestep and no post stack to speak
of, so most of the list has no counterpart yet. Two do:
`r.SecondaryScreenPercentage.GameViewport = 100` is a **DPI-independence guarantee at capture
time**, and `r.ScreenPercentage = 100` a **resolution-independence** one. When F4 gives the
editor its own camera and viewport, the capture path needs both properties stated somewhere — and
the lesson above says: state them where something can *check* they were applied, not as a list of
names in a constructor.

---

## C. `ScreenShotManager`'s fallback resolution

`Developer/ScreenShotComparisonTools/Private/ScreenShotManager.cpp`.

### C.1 The path shape

`GetApprovedFolderForImageWithOptions` (`:98-134`):

```
non-confidential (:125):  <Project>/Test/Screenshots/<Context>/<ShotName>/<Platform>/<RHI_FeatureLevel>/
confidential     (:121):  <Project>/Platforms/<Platform>/Test/Screenshots/<Context>/<ShotName>/<RHI_FeatureLevel>/
```

`GetIdealApprovedFolderForImage` (`:136-140`) is that call with the project's default
confidentiality option. "Ideal" is a named concept: *the folder this image would live in if a
reference existed for exactly this platform and RHI*.

### C.2 The four-step resolution — and what it logs

`FindApprovedFiles` (`:142-235`):

1. Search the ideal path (`:157`).
2. If empty **and** `bUseConfidentialPlatformPaths`, retry the non-confidential path (`:162-167`).
3. Walk a **chain**, not a single hop (`:171-223`): `FallbackPlatforms.Find(CurrentPlatformRHI)`
   (`:180`) maps child to parent; on a hit, `CurrentPlatformRHI` becomes the parent (`:186`) and
   the loop repeats. The key is split on `/` into `Platform` and `RHI_FeatureLevel`, then on `_`
   into RHI and feature level (`:188-207`) — a one-component value means feature-level-only, no
   RHI (`:200-206`). Success logs `"Using fallback images from %s"` (`:215`).
4. If still empty, log **every path tried**: `"Couldn't Find any fallback images, tried the
   following paths:"` then one line per entry of the `TriedPaths` array (`:227-231`), which
   `FindImages` appends to on every attempt (`:147`).

The chain is built at startup by `BuildFallbackPlatformsListFromConfig` (`:796-...`, called from
`:51`), merging the project's `ScreenshotFallbackPlatforms` with
`UScreenShotComparisonSettings::GetAllPlatformSettings()` (`:801-803`).

**Step 4 is the finding.** An absent reference is reported *with the search space that failed to
produce it*. That is the mechanised form of Tier 1 item 4's `compare-missing-reference` absence:
not "no reference", but "no reference, and here are the N paths I looked in, in order".

**One hazard, do not inherit:** the `while (ApprovedImages.Num() == 0)` loop (`:178`) exits only
when `Find` misses. A config cycle (A to B to A) never terminates. If we adopt a chain, the walk
needs a visited set or a depth bound.

### C.3 The result knows which reference it used

`Public/ImageComparer.h`:

- `IsNew()` (`:400-403`) — `ApprovedFilePath.IsEmpty()`: there was no reference at all.
- `IsIdeal()` (`:409-412`) — `FPaths::GetPath(ApprovedFilePath) == IdealApprovedFolderPath`:
  the reference used was this platform's own, not an inherited one.
- `AreSimilar()` (`:417-425`) — **returns `false` when `IsNew()`**, before any tolerance check.
  A missing reference is never a pass. Ours holds the same rule from the other side
  (`VerifyReport.hpp:255-264`: `resolvedLevel "none"` must "NEVER [be conflated] with a
  zero-difference pass"), but ours states it in a comment for the caller to honour, while theirs
  is a method that cannot be got around.

`IsNew()` and `AreSimilar()` travel as **separate booleans** to the controller
(`Developer/AutomationController/Private/AutomationControllerManager.cpp:511-534`, including the
log line `"(IsNew=%d, AreSimilar=%d)"` at `:530-533`) — two facts, never collapsed into one
verdict. And a non-ideal comparison renames the report's approved artifact to
`Approved_<Platform>_<RHI>.png` (`ScreenShotManager.cpp:419-425`) so the provenance of the
reference is visible in the output directory itself.

**Passing against a fallback reference is a third state, and it is neither PASS nor FAIL.** We
have exactly this today, unnamed: `resolvedLevel` is `"none" | "shared" | "backend"`
(`VerifyReport.hpp:255-258`), so `"shared"` *is* our fallback level — a lane can pass against the
shared `editor-ui` reference while its backend-specific reference does not exist, and
`golden-gate.ps1` reports that as an unqualified `PASS` (`scripts/golden-gate.ps1:579-581` reads
`exitReason` and `compare.passed`, and never reads `resolvedLevel`). **The field exists; the gate
does not look at it.**

### C.4 A versioned result with a supported RANGE

`Public/ImageComparer.h:352-358`: `int32 Version`, `CurrentVersion = 3`,
`OldestSupportedVersion = 2`, with `IsValid()` (`:385`) checking
`Version >= Oldest && Version <= Current` and `SetInvalid()` (`:392-395`) stamping 0.

Ours has `schemaVersion` as a single number. A **range** plus a validity predicate is the better
shape for the Servitor boundary `VerifyReport.hpp` describes: it lets a consumer say "I understand
2..3" rather than "I understand 3", and gives a deliberate way to mark a result unreadable. Cheap,
and it costs nothing to adopt at the next bump.

---

## D. What the addendum changes about the proposed scope

Nothing above is removed. Four changes:

1. **Tier 1 item 1 (verdict vocabulary) gains a fifth value and a second consumer.** Beyond
   `Failed` vs "could not run", section C.3 shows **passed-against-a-fallback** is its own state,
   and we already carry the datum (`resolvedLevel`) without grading it. And it is not only the
   gate: GitHub Actions runs `ArcaneTests.exe "~[gpu]"` with **no reporter at all**
   (`.github/workflows/ci.yml:59-68`), so its only signal is the process exit code — the exact
   thing our own gate's rule forbids relying on. Jenkins does better (`Jenkinsfile:54-55`,
   `-r junit::out=...`) but nothing asserts on the counts.
2. **Tier 1 item 4 (host witness) gets a shape.** The pieces to imitate are
   `Gauntlet.LowLevelTestNode.cs`'s verdict precedence (A.6), the `successes > 0` vacuity guard
   (`ReportParser.cs:54`), and `TriedPaths` (`ScreenShotManager.cpp:227-231`). All three are small
   and all three are properties our current surface lacks.
3. **Tier 2 item 6 (telemetry) has a first customer and a free mechanism.** The baseline we track
   by hand — 52298 assertions / 1277 cases — is exactly
   `TelemetryData{Measurement, Unit, Baseline}`. Our Catch2 3.15's `JsonReporter`
   (`catch_amalgamated.hpp:14113`) emits the counts already; UE had to hand-write a reporter
   because 3.4.0 could not.
4. **Two NEW candidates, both Tier 2, neither derivable from the earlier read:**
   - **Engine-assertion routing (A.2/A.3).** `ARC_ASSERT`/`ARC_VERIFY`/`ARC_ENSURE` firing inside
     a test case should fail *that case*, and there should be a `REQUIRE_ARC_ASSERT`-style macro
     to assert a guard fires. This is the single largest capability gap the addendum found.
   - **A progress bound distinct from a duration bound (A.4/A.6).** Per-test-case, and at the
     process level for host lanes.

**Withdrawn from consideration:** UE's custom Catch2 reporter (we have `JsonReporter`), their
Catch2 fork for active-test info (we have `getCurrentTestName()`), their log-substring timeout
contract, their third disabling mechanism (`DISABLED_TEST_CASE`), and their console-report parser
(`LogParser.cs:135` lacks the property its XML sibling has).

---

## E. Verification pass, 2026-09-01 — four claims retracted

Before planning Arc A, every UE claim this design leans on was re-checked **at the source**
rather than trusted from this doc. Four did not survive. Three were mine, one was a misreading of
which default the code selects. All are corrected in place above; they are collected here so a
reader who already absorbed the earlier text knows exactly what moved.

**The pattern in all four: I read the artifact that was easiest to find — a preset table, a
function that composes a ticket string, a flag's declaration — and reported it as the thing the
system actually does, without following through to the call site that selects it.** That is
`feedback_verify_dont_relay` and `feedback_name_the_artifact_before_arguing_from_it` in one.

### E.1 "Epic's default is zero-tolerance, same as ours" — FALSE (was §1)

`DefaultIgnoreNothing` is the strictest entry in the preset table, and I reported the table's
strictest row as the system's default. The screenshot path does not select it.
`FAutomationScreenshotOptions`'s default ctor (`AutomationScreenshotOptions.h:73-90`) pairs
`EComparisonTolerance::Zero` with **`MaximumLocalError(0.10f)`, `MaximumGlobalError(0.02f)` and
`bIgnoreAntiAliasing(true)`**, and the metadata-absent fallback is `DefaultIgnoreLess` +
`IgnoreAntiAliasing` (`ScreenShotManager.cpp:274-276`).

Only the per-channel colour tolerance is zero. **Our zero budget is materially stricter than
Epic's** — the comparison under-claimed us in one direction while over-claiming them in another.

### E.2 "Exclusions are tied to tracker entries — that is what stops them becoming permanent" — UNSUPPORTED (was §2)

That sentence was editorialising, not something the source says or enforces. `UpdateReason`
(`AutomationTestExcludelist.cpp:49-83`) makes the ticket **optional**, lives entirely under
`#if WITH_EDITOR` so a hand-edited config never passes through it, writes into a bare `FName`
`Reason` that is never validated as non-empty, and **there is no expiry, deadline or review
concept anywhere in the mechanism**.

Consequence for Arc A: its mandatory, checked expiry is **not a substitute for something UE has**
— it is an addition UE lacks. The spec must not cite Epic as precedent for the property.

### E.3 "`NonNullRHI` is a declared capability requirement the runner checks" — OVERSTATED (was §7)

Two ways. `GetValidTestNames` (`AutomationTest.cpp:772-805`) derives `bUsingNullRHI` from the
`-nullrhi` switch, `IsRunningCommandlet()` or `IsRunningDedicatedServer()` — never from the
device — and the code says so itself (*"@todo: Handle this correctly"*). And a test failing
`bPassesFeatureRequirements` is dropped from the enumerated list, so the run **silently omits it**
rather than reporting a skip.

Consequence for Arc A: the runner-side probe plus reported `SKIP` is not "UE's `NonNullRHI`
shape" — it is strictly better than it on both counts.

### E.4 "UE's block-size arithmetic must not be inherited uncritically" — WRONG, they got it right

This one would have produced a worse implementation. I assumed `CompareWidth / 10` truncated and
warned that the far edge would index past 99. It does not: `ImageComparer.cpp:311-312` uses
**`FMath::RoundFromZero(CompareWidth / 10.0)`**, which for positive input is `ceil`
(`Runtime/Core/Public/Math/UnrealMathUtility.h:2269-2277`). Ceil sizing keeps
`(extent-1) / blockSize` at 9 for every extent, so indices stay in `[0,99]` by construction.

**Ceil is the correct fix, and it is better than the truncate-and-clamp I was about to specify** —
clamping would pile the remainder into the last block and inflate its score. A `max(1, ...)` guard
against a zero extent is still ours to add; UE has no such guard.

### E.5 Three smaller refinements (not retractions)

- **`IsReadyToStart()` has a two-level contract** (`Gauntlet.TestNode.cs:145-150`): return false
  for "not ready yet", *throw* for "will never be ready". Arc A's preflight implements the second
  half only; the retry half is device-farm machinery we do not want.
- **`TelemetryData.Baseline` defaults to `0`** (`Gauntlet.TestReport.cs:105`), so "no baseline"
  and "baseline of zero" are the same value — `feedback_default_values_are_not_measurements` in
  their own telemetry type. Ours must be optional-absent.
- **The size-mismatch path does not quite "discard" the comparison** (`ImageComparer.cpp:401-410`):
  it compares the overlap, marks out-of-bounds pixels as errors and counts them, then *overwrites*
  `MaxLocalDifference` and `GlobalDifference` with `1.0`. The delta image and its path survive. The
  verdict is thrown away; the artifact is not.
- Their compare loop is a `ParallelFor` over columns with `InterlockedIncrement` on both
  accumulators (`:316-381`). Ours is serial, so a plain array suffices — but the block accumulator
  becomes shared state the day that changes.
