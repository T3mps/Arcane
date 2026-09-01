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

**Epic's default is zero-tolerance, same as ours.** Relaxation is opt-in and NAMED.

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
- **The reason composes beautified text + a TICKET string**
  (`Private/AutomationTestExcludelist.cpp:53-79`, `BeautifiedReason + FullTicketString`).
  Exclusions are tied to tracker entries — that is what stops them becoming permanent.
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

- **`NonNullRHI` is a declared CAPABILITY REQUIREMENT the runner checks.** Our `[gpu]` conflated
  a (now-retired) driver hazard with a baseline-comparability convention — the exact confusion
  that cost 2026-08-31.
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
