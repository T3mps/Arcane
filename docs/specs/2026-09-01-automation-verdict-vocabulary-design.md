# Automation verdict vocabulary — Arc A design

**Status:** design, 2026-09-01. Approved in brainstorming; implementation plan to follow.

**Provenance.** Every item here traces to
`docs/research/2026-08-31-ue-automation-framework-comparison.md` — its Tier 1 + Tier 2 lists and
the 2026-09-01 addendum that closed its three unread areas (`Programs/LowLevelTests/`, the CVar
list behind `bDisableNoisyRenderingFeatures`, `ScreenShotManager`'s fallback resolution). Read
that doc before this one; it carries the `file:line` evidence this design argues from and is not
restated here.

**Every UE claim this design leans on was re-verified at the source on 2026-09-01, before
planning.** Four did not survive and are retracted in that doc's section E; the three that
affected this design are corrected in place below (exclusion rationale, capability probe,
block-size arithmetic). Where this spec now differs from Epic, it says so.

**This is Arc A of two.** Arc B is the host-level witness harness (Tier 1 item 4), split out
because it is the only item needing a new buildable thing and because it would assert against
the vocabulary Arc A defines. See "Arc B handoff" at the end.

---

## 1. What this arc is for

Two findings from the addendum set the whole shape:

- `exitReason` already carries seven distinct values — `frames-complete`, `device-lost`,
  `render-failed`, `validation-errors`, `compare-missing-reference`, `compare-failed`,
  `settle-not-converged` (`ArcaneRuntime/src/RuntimeApp.cpp:1119-1158`), plus `gpu-stall`
  (`ArcaneClient/src/Arcane/Base/Diagnostics.cpp:206`). `golden-gate.ps1` collapses all seven
  non-green ones into a single `FAIL`.
- `compare.resolvedLevel` already carries `"none" | "shared" | "backend"`
  (`ArcaneClient/src/Arcane/Host/VerifyReport.hpp:255-258`). The gate never reads it
  (`scripts/golden-gate.ps1:579-581` reads only `exitReason` and `compare.passed`), so a lane
  that passed against an inherited reference is reported as an unqualified `PASS`.

**The facts exist; nothing names them.** That is what this arc supplies, and it is why the arc's
centre is a vocabulary rather than new plumbing.

### In scope (10 items)

| # | Item | Source |
|---|---|---|
| 1 | Verdict vocabulary replacing umbrella `FAIL` | Tier 1.1 |
| 2 | Spatial-concentration knob (`maxLocalDifference`) | Tier 1.2 |
| 3 | Declarable exclusion, config-driven, expiry-enforced | Tier 1.3 |
| 4 | Repro command on every non-green lane | Tier 2.5 |
| 5 | Telemetry with unit + baseline | Tier 2.6 |
| 6 | Untangle `[gpu]`: capability vs comparability | Tier 2.7 |
| 7 | Absolute-time pinning (`--fixed-time`) | Tier 2.8 |
| 8 | Preflight readiness gate | Tier 2.10 |
| 9 | Engine-assertion routing into Catch2 | addendum D.4 |
| 10 | Progress bound distinct from duration bound | addendum D.4 |

**Removed from scope during planning, 2026-09-01:** a `--fixed-dt nan` validation item. It is
**already implemented** at `ArcaneClient/src/Arcane/Host/HostConfig.cpp:191-195`
(`!std::isfinite(cfg.fixedDtSeconds) || cfg.fixedDtSeconds <= 0.0`), with a comment spelling out
the NaN reasoning. Two memory notes disagreed about whether it was owed and the spec took the
stale one. What survives is only the requirement that **`--fixed-time` be given the same
validation** — see 5.2.

### Out of scope

- **Arc B**: the host-level witness harness (Tier 1.4).
- **Named tolerance presets** (Tier 2.9) — **deliberately cut**. UE has four presets because it
  has four callers; we have one policy (zero) and no second consumer. A one-entry table is a
  table waiting to be misused. Instead the default is made explicit and named
  (`ImageCompareOptions::Strict()`), and presets arrive when a second one has a real caller.
- Everything in the comparison doc's Tier 3, and everything in its section 10 ("Do NOT inherit").
- The withdrawn candidates named in addendum D: a custom Catch2 reporter, a Catch2 fork for
  active-test info, the log-substring timeout contract, `DISABLED_TEST_CASE`, and the
  console-report parser shape.

---

## 2. The vocabulary

Seven values, derived from distinctions the system can already make.

| Value | Meaning | Produced by |
|---|---|---|
| `Passed` | Ran, met its contract, against its own reference | `frames-complete` + `compare.passed` + `resolvedLevel` == expected |
| `PassedOnFallback` | Ran and matched — against an **inherited** reference | same, with `resolvedLevel` == `"shared"` where backend-specific was expected |
| `Failed` | Ran, did not meet its contract | `compare-failed`, `settle-not-converged` |
| `Errored` | Ran, died before it could answer | `device-lost`, `render-failed`, `validation-errors`, `gpu-stall` |
| `NotRun` | Could not be started | exe missing; preflight refusal |
| `Skipped` | Deliberately not run, **with a stated reason** | active exclusion; absent capability |
| `Indeterminate` | Ran, and we cannot tell what happened | no report; unparseable report; `compare-missing-reference` |

### Three rulings inside that table

**`Errored` is not `Failed`.** UE's `TestResult` separates them
(`Gauntlet/Framework/Base/Gauntlet.TestNode.cs:19`) and our `exitReason` already does. A
validation-error lane and a mismatched-pixels lane call for different next actions, and
flattening them costs the reader that information at exactly the moment they need it.

**`compare-missing-reference` is `Indeterminate`, not `Failed`.** The subject rendered
correctly; the harness had nothing to check it against. It stays red — it must not read as "the
render is wrong." This is the same separation UE draws with `IsNew()` versus `AreSimilar()`
travelling as two booleans (`AutomationControllerManager.cpp:511-534`).

**`PassedOnFallback` is green but qualified.** It counts toward `gatePassed`. It exists so that
"this lane has no reference of its own" is visible without being a failure, which is the state
`editor-ui` occupies today, unnamed.

### `gatePassed`

True iff **at least one lane is `Passed` or `PassedOnFallback`** and **no lane is in
{`Failed`, `Errored`, `NotRun`, `Indeterminate`}**.

`PassedOnFallback` counts toward the "at least one" because such a lane genuinely ran and
genuinely compared — against a real reference that merely was not its own. Excluding it would
turn today's legitimate shared-`editor-ui` arrangement red for no defect.

`Skipped` lanes do not fail the gate, and do **not** count toward the "at least one" — so an
all-skipped run is red. This is the vacuity guard lifted from
`Utility/LowLevelTests.ReportParser.cs:54` (`... && NrOverallResultsSuccesses > 0 &&
NrOverallResultsCasesSuccesses > 0`), and it is the property that stops a fully-excluded gate
from reporting green.

### Gate mapping precedence

Evaluated in order; first match wins. Exclusion is first so that an excluded lane needs no exe
on disk.

1. exclusion matches and has not expired → `Skipped`
2. a required capability is absent → `Skipped`
3. exe missing, or a preflight precondition failed → `NotRun`
4. no report on disk, or report present but unparseable → `Indeterminate`
5. `exitReason` in {`device-lost`, `render-failed`, `validation-errors`, `gpu-stall`} → `Errored`
6. `exitReason` == `compare-missing-reference` → `Indeterminate`
7. `exitReason` == `frames-complete` && `compare.passed` → `Passed`, or `PassedOnFallback` when
   `resolvedLevel` is not the expected level for that lane
8. otherwise → `Failed`

**"Expected level" is declared, not inferred.** Whether a lane is supposed to have its own
backend-specific reference or to share one is a fact about the lane, and nothing in the report
can tell the gate which was intended — a `"shared"` result looks identical whether that was the
design or an oversight. So each entry in the gate's `$combos` table carries an
`expectedLevel` of `"backend"` or `"shared"`, and step 7 compares against it. A lane declaring
`"shared"` and resolving `"shared"` is a plain `Passed`.

This mirrors the precedence cascade in `Gauntlet.LowLevelTestNode.cs:247-320`, adapted to facts
we emit. Note step 4 preserves the existing "COULD NOT DETERMINE" reasoning already written at
`scripts/golden-gate.ps1:589-606` — that comment was right, and this only gives its conclusion a
name.

### Where the vocabulary lives

PowerShell cannot include a C++ enum, so there is no shared symbol available. Rather than ask
two files to stay in step by convention:

- The definition of record is `Arcane::Verdict` plus a `ToString`/`FromString` pair in a small
  header.
- A Catch2 case pins the exact string set.
- `golden-gate.ps1` carries the same literal set, and its `-SelfTest` asserts its set matches
  the one the header produces.

Two independent pins on one list. Divergence is a test failure, not a latent inconsistency.

---

## 3. The three surfaces

### 3.1 Golden-gate lanes

`scripts/golden-gate.ps1` and `golden-gate-summary.json`:

- `schemaVersion` **1 → 2**. `verdict` widens from `'PASS'|'FAIL'` to the seven values. This is
  a breaking change to the field consumers are documented to assert on
  (`golden-gate.ps1:684-685`), and the version bump is the signal that makes it legitimate — the
  same reasoning that governed `VerifyReport`'s `"offscreen"` → `"headless"` rename at
  schemaVersion 3 (`VerifyReport.hpp:156-162`).
- `gatePassed` keeps its name and its type, and gains the rule in section 2. It remains the safe
  single thing for a consumer to assert on.
- New field `skipReason`, emitted **only** when `verdict` is `Skipped`. This follows the
  absence-must-be-absence contract `VerifyReport` already upholds for `compare` and `settle`
  (`VerifyReport.hpp:240-249, 287-294`), so "not skipped" and "skipped with no stated reason"
  cannot collapse into the same value.
- `detail` is unchanged.
- **`$exeMissingCount` is deleted** (`golden-gate.ps1:468, 491, 758`). It was the bespoke patch
  for exactly the hole the enum closes. `-SelfTest`'s assertion becomes "every lane is `Failed`"
  — a lane that never launched is now `NotRun`, which is not `Failed`, so the check needs no
  second counter to stay honest.

### 3.2 ArcaneTests

Catch2 already models three of the seven natively (passed / failed / skipped). This arc wires
the producers:

- The GPU capability probe calls `SKIP("no <backend> adapter on this machine")`, so a
  GPU-less runner reports skips instead of hiding tests behind a filter.
- The exclusion list is consulted at case start; a match calls `SKIP` with the entry's reason.
- `-r json::out=` is added to **both** CI lanes. GitHub Actions currently runs
  `ArcaneTests.exe "~[gpu]"` with no reporter at all (`.github/workflows/ci.yml:59-68`), so its
  only signal is the process exit code — the precise thing our own gate's rule forbids relying
  on. Jenkins already uses `-r junit::out=` (`Jenkinsfile:54-55`) but nothing asserts on the
  counts it produces.

**Arc A deliberately does not wrap ArcaneTests in a verdict script.** The per-case half of the
vocabulary is Catch2's already. The suite-level half — `Errored`, `NotRun`, `Indeterminate` for
the process itself — is what Arc B's witness harness produces. Building a wrapper now means
building it twice.

One thing we get free and should record: zero tests is already a failure for us.
`Config::zeroTestsCountAsSuccess()` (`ThirdParty/Catch2/extras/catch_amalgamated.cpp:911`) is
driven by `--allow-running-no-tests` (`:3586-3587`), off by default. UE has to enforce the same
property by hand. What we still lack is **count regression** detection — see 6.3.

### 3.3 The compare result

- `ImageCompareResult` gains `maxLocalDifference` (section 5.1).
- `VerifyReport`'s `compare` object carries it through; `schemaVersion` **3 → 4**.
- At the same bump, adopt the supported-range shape from `ImageComparer.h:352-358`: a
  `kSchemaVersion` / `kOldestSupportedSchemaVersion` pair with a validity predicate, so a
  consumer across the Servitor boundary can declare "I understand 3..4" rather than "I
  understand 4", and a result can be deliberately marked unreadable.

**`VerifyReport` gets no verdict field.** Its header states the boundary — *"The engine's job
stops at emitting facts; there is no assertion DSL here, and none belongs here"*
(`VerifyReport.hpp:11-12`). A verdict is a judgement over facts, so it lives on the consuming
side. The report's job is to keep emitting the distinctions; this arc's finding was that nothing
downstream read them.

---

## 4. The two producers of `Skipped`

### 4.1 Declarable exclusion

One file, `scripts/automation-exclusions.json`, read by both surfaces — PowerShell via
`ConvertFrom-Json`, C++ via the already-vendored nlohmann. Entry shape, modelled on
`FAutomationTestExcludelistEntry` (`Runtime/AutomationTest/Public/AutomationTestExcludelist.h:188`)
but scoped to axes we actually have:

| Field | Required | Meaning |
|---|---|---|
| `target` | yes | a gate lane label (`"runtime/d3d12"`) or a Catch2 test-name / tag pattern |
| `backends` | no | `["dx12","vulkan"]`; absent means all |
| `hosts` | no | `["runtime","editor"]`; absent means all |
| `configurations` | no | `["Debug","Release","Dist"]`; absent means all |
| `reason` | yes | free text — there is no tracker to link to |
| `expires` | yes | ISO `YYYY-MM-DD` |

**Backend spelling is `dx12` / `vulkan`** — the CLI's own vocabulary
(`HostConfig.cpp:12`, `.Choices({ "dx12", "vulkan" })`) and the gate's lane labels
(`golden-gate.ps1:167-170`). Note three spellings exist in the tree: the CLI's `dx12`, the
report's `D3D12` (`ToString(GraphicsBackend)`, `GraphicsBackend.cpp:9`), and the enumerator
`GraphicsBackend::D3D12`. The exclusion file uses the CLI spelling because that is what a person
types; matching is case-insensitive and accepts `d3d12` as an alias for `dx12` so the other
spelling is not a silent miss.

**The expiry is enforced by the same run that honours the entry.** Past its date, the
*exclusion* is reported `Failed` — not the thing it excludes. It surfaces in both places:

- a dedicated Catch2 case (`"automation exclusions: none has expired"`), so it runs everywhere
  including GitHub Actions;
- a synthetic lane in the gate.

This is what makes the mechanism a mechanism, and
`feedback_time_boxed_stances_need_checked_expiry` is explicit that a stated expiry without a check
is not one.

**Do not cite UE as precedent for this property — it does not have it.** The 2026-09-01
verification pass (research doc §E.2) retracted the earlier claim that Epic ties exclusions to
tracker entries "to stop them becoming permanent". In fact the ticket is optional
(`AutomationTestExcludelist.cpp:51-54`), the composing function is `#if WITH_EDITOR` only so a
hand-edited config never passes through it, `Reason` is a bare `FName` never validated as
non-empty, and **there is no expiry, deadline or review concept anywhere in their mechanism**.
Their entry is a good SCOPING model and nothing more; the expiry is ours and is the part that
makes it safe to have at all.

**A malformed file is a refusal, not "no exclusions."** A parse error that silently disables the
whole mechanism is the failure class `ParseProbe` already refuses by precedent
(`VerifyReport.hpp:81-92`: *"a probe an agent asked for and silently never got is a worse
failure than one that never ran, because it is invisible in the report"*). An **absent** file is
fine — that is the default state and means no exclusions.

The expiry predicate takes an **injectable** "today" so it is unit-testable at fixed dates; the
real clock is passed only at the call site. A wall-clock-dependent test is normally a smell, but
here the clock is the subject.

### 4.2 Capability probe

`Arcane::NativeDeviceOwner::Create` per backend, attempted **lazily on first use** and cached —
probing both backends at startup would pay for device creation on runs that touch neither. A
`RequireBackend(GraphicsBackend)` helper calls `SKIP(...)` when the backend is unavailable.

Once it exists, GitHub Actions drops `~[gpu]` and reports honest skips rather than a filter that
silently omits 53 tagged sites.

**This borrows UE's IDEA and rejects its mechanism.** `NonNullRHI`
(`Runtime/Core/Public/Misc/AutomationTest.h:102`) is a declared capability requirement, but the
verification pass (research doc §E.3) found it is decided from the `-nullrhi` switch,
`IsRunningCommandlet()` or `IsRunningDedicatedServer()` — never from the device, as its own
`@todo` admits (`Runtime/Core/Private/Misc/AutomationTest.cpp:772-782`) — and that a test failing
the check is dropped from the **enumerated list**, so the run silently omits it
(`:805`). A real probe plus a reported `SKIP` is better on both counts, and avoids reproducing
the vacuity their own `successes > 0` guard exists to catch.

**Included in the same sweep — retired prose still in-tree.**
`ArcaneTests/src/NriGraphPixelTest.cpp:47-48` still reads *"`[gpu]` MEANS DESK, AND THESE HAVE
NOT BEEN RUN"*, and `ArcaneTests/src/NriDeviceCapsTest.cpp:4` still calls `~[gpu]` a "dev gate".
The ban was retired 2026-08-31 and `[gpu]` has since passed 62002 assertions across 25 cases.
Leaving a retired stance written where the next reader will believe it is the same defect class
the expiry mechanism above exists to prevent.

---

## 5. Comparator and capture determinism

### 5.1 Spatial concentration

One `std::array<std::uint64_t, 100>` alongside the existing `diffCount`, incremented at the same
two sites (`ArcaneClient/src/Arcane/Assets/ImageCompare.cpp:383, 405`), then
`maxLocalDifference = max(block) / blockArea`. Mirrors
`Developer/ScreenShotComparisonTools/Private/ImageComparer.cpp:311-397`.

Two correctness notes that are not obvious from the UE source:

- The loop runs in **padded** coordinates (`y` from `pad` to `r1.height - pad`,
  `ImageCompare.cpp:317-319`) and already computes `dx`/`dy` for the diff image. The block hash
  must use `dx`/`dy` against the **unpadded** extent, or every block is offset by the pad.
- **Block size is `ceil(extent / 10)`, not `extent / 10`.** UE gets this right and the earlier
  draft of this spec got it wrong (research doc §E.4): `ImageComparer.cpp:311-312` uses
  `FMath::RoundFromZero(CompareWidth / 10.0)`, which for positive input is `ceil`
  (`UnrealMathUtility.h:2269-2277`). Ceil sizing keeps `(extent-1) / blockSize` at 9 for **every**
  extent, so indices stay in `[0,99]` by construction — no clamp needed, and no clamp wanted,
  since clamping would pile the remainder into the last block and inflate its score. Add a
  `max(1, ...)` guard against a zero extent, which UE lacks.

**Budget ruling.** `ImageCompareOptions::maxLocalDiffRatio` is `std::optional<double>`, default
unset, and **when unset it is not evaluated**. The existing rule — "if neither budget is set the
budget is ZERO" (`ImageCompare.hpp:202-204`) — already fails on a single differing pixel, so a
strict local gate would be pure redundancy. Local concentration only bites once someone relaxes
the aggregate budget. But `maxLocalDifference` is **reported as a fact on every comparison**
regardless, so it is measurable long before anything gates on it.

**Not adopted:** UE's size-mismatch policy, which compares the overlap, marks out-of-bounds pixels
as errors, and then *overwrites* both scores with `1.0` (`ImageComparer.cpp:401-410`) — the
verdict is thrown away, though the delta artifact survives. We pad, still compare, and report the mismatch as its
own named fact. The comparison doc's section 1 already ruled on this.

### 5.2 Absolute-time pinning, and the owed `--fixed-dt` defect

`--fixed-time <seconds>` pins the clock the scene sees at capture, alongside `--fixed-dt` which
pins the step. UE's equivalent is `OverrideTimeTo`, applied at
`AutomationBlueprintFunctionLibrary.cpp:198-202` under the comment *"Turn off time the ultimate
source of noise."*

**Why it is worth having when `--fixed-dt` already makes a run deterministic:** it decouples scene
time from the FRAME COUNT. Today `Time` is `frames x fixedDt`, so changing `--frames 60` to
`--frames 90` re-shades anything animated on `Time` — `ReferenceProject`'s `PulseSprite` computes
`sin(Time * PulseSpeed)` — and silently invalidates a blessed reference.

**The seam, found during planning.** Do NOT pin the accumulators: `io.hostClock`
(`RuntimeFrame.cpp:235`) and `m_editorClock` (`EditorAppFrame.cpp:1291`) each serve TWO purposes —
the scene's `Time` and the shader-compile / material-watch debounce clocks
(`RuntimeFrame.cpp:770`, `EditorApp.hpp:1145`, `EditorAppProject.cpp:204-206`) — and a frozen
clock would break the latter. Override **only** where the value is handed to the scene, which is
a single assignment in each host: `frame.now` at `RuntimeFrame.cpp:321` and
`EditorAppFrame.cpp:1296`. The accumulators keep advancing, untouched.

Since the task touches that parser anyway, **fold in the shelved `--fixed-dt nan` defect**: both
flags reject non-finite and negative values rather than accepting them and producing a run whose
timing is quietly undefined. This closes a servitor-arc item that has been owed since 2026-08-27.

### 5.3 Not in this arc — the capture-time CVar equivalents

The addendum's section B catalogued UE's noise-suppression list. Most has no counterpart: we
render offscreen with a fixed timestep and no post stack to speak of. Two properties will matter
when F4 gives the editor its own camera and viewport — DPI independence
(`r.SecondaryScreenPercentage.GameViewport = 100`) and resolution independence
(`r.ScreenPercentage = 100`). They belong to F4, not here.

The cautionary half of that section does apply now, though, and informs 5.1's "report the fact
before gating on it": UE declares thirteen CVar swappers and applies twelve, with
`TonemapperSharpen` constructed at `:246`, never `Set`, and its `Restore()` commented out at
`:331`; and their per-view show-flag path (`:204-227`) is entirely commented out while the
declared boolean still reads as implemented. **A declared knob is not an applied knob**, and
nothing in their type system notices.

---

## 6. Assertion routing, bounds, telemetry, ergonomics

### 6.1 Engine-assertion routing

UE routes engine assertions **into** Catch2 rather than around it, by two mechanisms: a `GError`
swap whose `Serialize` raises a Catch2 `ExplicitFailure`
(`Private/TestRunnerOutputDeviceError.cpp:12-18`) and an `OnHandleSystemEnsure` hook
(`Private/TestRunner.cpp:229-235`). Today an `ARC_ASSERT` firing inside an ArcaneTests case is
not attributed to the running test case at all.

**A constraint to design around, not discover.** `Mosaic::SetAssertHandler` writes an `inline`
atomic (`ThirdParty/Mosaic/include/Mosaic/Assert.hpp:81`), so the slot is **per-module**.
Installing from `test_main.cpp` routes the test exe's own asserts and *not* those fired inside
`Arcane.dll` — which is exactly why `Arcane::Assert::InstallMosaicHandler()` is an inline
installer over an exported handler (`ArcaneClient/src/Arcane/Base/Assert.hpp:3-7, 15-17`). The
test harness needs the same two-part shape: an exported "report into Catch2" handler plus an
inline installer each module calls.

The handler returns `AssertAction::Continue`, never `Break` — an unattended process must not
execute an `int3`, which is the same reasoning `Assert.hpp:55-62` already gives for its
`IsDebuggerPresent` guard.

**The inverse direction:** `REQUIRE_ARC_ASSERT` / `CHECK_ARC_ENSURE` as RAII scopes that swap in
a counting handler and fail the case when the count is zero, mirroring UE's
`FCheckScope`/`FEnsureScope` usage (`Public/TestMacros/Assertions.h:32-82`). Two caveats:

- `MOSAIC_ASSERT` compiles out under `NDEBUG` (`Assert.hpp:193-198`). ArcaneTests should define
  `MOSAIC_ENABLE_ASSERTS` in **all three** configs, so these cases exist identically everywhere
  rather than moving the per-config baselines apart.
- `MOSAIC_ENSURE` fires **once per call site** via a static atomic (`:233-243`). A second test
  touching the same production site observes nothing. The scope must report "already fired" as
  its own outcome rather than passing silently; `MOSAIC_ENSURE_ALWAYS` (`:245`) is the
  alternative where a site is deliberately test-targeted.

### 6.2 Progress bound

At the gate, not per-case. UE's per-case version spawns a `while(true)` thread that is never
joined or deleted (`Private/TestListeners/EventListener.cpp:29-50, 75-77`) and only *logs*, with
termination delegated to a C# substring match on `"Timeout detected"`
(`Gauntlet.LowLevelTestNode.cs:370-384`). Do not copy the design; take the distinction.

The gate polls the child's stdout for **total duration** and **time since last write** as two
separate bounds. An exceeded bound reports **which** one, following the rule `SettleBail` already
follows (`VerifyReport.hpp:324-330`: *"the caller is told which knob would actually change the
outcome"*). Verdict on either: `Errored`.

The C# side of UE's design has this shape correctly — `MaxDuration` at
`Gauntlet.LowLevelTestNode.cs:36` and the inactivity watchdog at `:145-163` are independent.

### 6.3 Telemetry with unit and baseline

`{name, value, unit, baseline}` records, modelled on
`Gauntlet/Framework/Base/Gauntlet.TestReport.cs:97-114`, emitted into `golden-gate-summary.json`
and a companion for the test suite. **`baseline` is optional-absent, not zero-defaulted** — UE's
`TelemetryData` defaults it to `0` (`:105`), which makes "no baseline" and "baseline of zero" the
same value; that is `feedback_default_values_are_not_measurements` inside their own telemetry
type, and the same absence-must-be-absence rule this design applies everywhere else. **The baseline lives in a committed file**, so a count regression
is a diff rather than a memory note. This retires "52298 assertions / 1277 cases, tracked by hand"
into something the repo checks.

Our Catch2 3.15's built-in `JsonReporter` (`extras/catch_amalgamated.hpp:14113`) already emits
the counts. UE had to hand-write `FUnrealReporter` because v3.4.0 predates it — their vendored
tree has automake, compact, console, junit, multi, sonarqube, tap, teamcity and xml, and no JSON.
**We do not need to write a reporter.**

Relatedly: if a future task wants the running test's name, `IResultCapture::getCurrentTestName()`
(`catch_amalgamated.hpp:1129`) is available unforked. UE forked Catch2 to add
`catch_active_test.{hpp,cpp}` and the `GROUP_*` lifecycle. **Do not fork Catch2.**

### 6.4 Repro command and preflight

- **Repro command** on every non-green lane — the exact invocation that reproduces it locally.
  UE's `GetRunLocalCommand` (`Gauntlet.TestNode.cs:227`). Small, high ergonomic payoff.
- **Preflight**: all lanes' preconditions are checked *before* any launches — exes present,
  `ReferenceProject` staged, exclusions file parses. A precondition failure reports `NotRun` for
  every affected lane up front rather than being discovered lane by lane. This is
  `IsReadyToStart()` (`Gauntlet.TestNode.cs:145-150`) formalised for our shape — **and only half
  of it**: their contract is two-level, returning false for "not ready yet" and *throwing* for
  "will never be ready". We implement the second half only. The retry half is device-farm
  machinery, and a lane that might become ready later is not a state a single-machine gate has.

---

## 7. Testing

The arc's own property is that **every verdict value is reachable and observed**. A value no
synthetic condition can produce does not belong in the enum.

`-SelfTest` currently proves one thing: a broken scene drives all four lanes red
(`golden-gate.ps1:44-137`). It grows into a matrix, one synthetic condition per value:

| Value | Synthetic condition |
|---|---|
| `Failed` | the existing MeshCube 0.4 → 0.6 mutation |
| `Errored` | a lane forced to a crash-set `exitReason` |
| `NotRun` | a lane's exe made absent |
| `Indeterminate` | the reference deleted (`compare-missing-reference`), and separately the report suppressed |
| `Skipped` | a temporary exclusion entry, and separately an unavailable backend |
| `PassedOnFallback` | a backend-specific reference removed with the shared one left in place |
| `Passed` | the ordinary green run |

Existing `-SelfTest` invariants are preserved: refuse on a dirty tree, self-heal a
previously-killed run's residue, restore source **and both staged copies** in a `finally`, and
write to `golden-gate-selftest-summary.json` so a green build's artifact is never overwritten
(`golden-gate.ps1:240-336, 633-667, 700-711`).

Unit-level coverage:

- the verdict string set, pinned from both sides (section 2);
- `maxLocalDifference` against hand-built images: scattered vs concentrated diffs with identical
  aggregate counts must score differently — that is the entire point of the knob;
- the exclusion matcher and its expiry predicate at fixed injected dates, including the malformed-file
  refusal;
- `--fixed-dt` / `--fixed-time` rejecting `nan`, `inf`, and negatives;
- `REQUIRE_ARC_ASSERT` / `CHECK_ARC_ENSURE` proving both directions — a guard that fires, and the
  failure reported when one does not.

**Baseline note.** Adding cases moves the tracked assertion/case counts. The new baseline is
recorded by re-running the declared command and pasting from its output, never recalled
(`feedback_derive_counts_never_recall_them`), and it lands in the committed baseline file from
6.3 rather than in a memory note.

---

## 8. Risks and constraints

| Risk | Mitigation |
|---|---|
| Widening `verdict` breaks an existing consumer | `schemaVersion` 1→2 is the signal; `gatePassed` is unchanged in name, type, and meaning, and remains the documented safe assertion |
| Two spellings of the vocabulary drift | Pinned from both sides, so divergence is a test failure (section 2) |
| Per-module assert handler routes less than expected | Called out in 6.1 as a design constraint, with the existing two-part installer as the pattern to copy |
| `MOSAIC_ENSURE`'s once-per-site latch makes a test silently vacuous | The scope reports "already fired" as a distinct outcome rather than passing |
| Exclusion mechanism becomes the way work is avoided | Mandatory expiry, checked by the run that honours it; a stale entry is itself `Failed` |
| A fallback-chain config cycle hangs the resolver | Not adopting a chain in this arc. If one is added later it needs a visited set or depth bound — UE's `while` at `ScreenShotManager.cpp:178` exits only on a `Find` miss |
| Enabling `MOSAIC_ENABLE_ASSERTS` in Release/Dist changes behaviour under test | Scoped to the ArcaneTests target only; hosts are untouched |

**Prerequisite:** `Game/` needs a rebuild against the SDK after any ABI-affecting change. This
arc changes `VerifyReport`'s schema and adds an exported assert handler, so plan for an ABI bump
(cheap during engine dev, per `feedback_abi_bumps_are_cheap`) and a matching `Aphelyon.arcproj`
restamp in the Gacha repo.

---

## 9. Arc B handoff

Arc B is the **host-level witness harness**, Tier 1 item 4. `ArcaneTests` links neither
`ArcaneRuntime` nor `ArcaneEditor` (`feedback_green_gate_proves_nothing_about_hosts`), so no
test today exercises `settleCaptureFailed`, the `compare-missing-reference` absence, or
`--settle-timeout` actually being spent. Newly buildable now that an agent can drive hosts
unattended.

Arc A leaves it three things to build on:

1. **The vocabulary to assert in** — including the suite-level half (`Errored`, `NotRun`,
   `Indeterminate`) that Arc A deliberately did not wrap ArcaneTests in.
2. **The precedence cascade** to imitate (`Gauntlet.LowLevelTestNode.cs:247-320`), already
   written out in section 2.
3. **`TriedPaths`** (`ScreenShotManager.cpp:227-231`) — report an absent reference *with the
   ordered search space that failed to produce it*, not merely as an absence.

---

## 10. Decisions log

| Decision | Choice | Why |
|---|---|---|
| Arc scoping | Two arcs: vocabulary + cheap items, then the witness harness | The witness is the only item needing a new buildable thing, and it would assert against Arc A's vocabulary — a real dependency, not just a split |
| `verdict` compatibility | Widen the field, bump `schemaVersion` 1→2 | One field, one meaning. A parallel `outcome` field creates two truths that can disagree, with the interesting states visible in only one of them |
| Exclusion expiry | Mandatory date, checked by the honouring run; stale entry is itself `Failed` | No tracker to link to, and a stated expiry without a check is not a mechanism |
| `[gpu]` untangle | Runner-side capability probe; tests `SKIP` with a reason | Makes the third state real inside ArcaneTests, and lets GitHub Actions drop a filter that hides tests rather than reporting them omitted |
| Approach | One vocabulary, three surfaces | The two sharpest findings are both "the fact exists, nothing names it"; a vocabulary is the missing thing |
| Named tolerance presets | **Cut** | One policy, no second caller. A one-entry table is a table waiting to be misused |
