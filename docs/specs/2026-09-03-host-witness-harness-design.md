# Host witness harness — Arc B design

**Status:** **LANDED 2026-09-03.** Six of seven scenarios/tasks implemented, reviewed and
closed on `main` (`aa29454e..67b8c1ed`, plus Gacha `4219c644`); **W2 was removed by ruling —
its behaviour has no content-or-CLI lever, derived in source (see `## Landed` below, the
arc's headline finding).** Plan: `docs/plans/2026-09-03-host-witness-harness.md`.

**Provenance.** This is Arc B of the two automation arcs, inherited from
`docs/specs/2026-09-01-automation-verdict-vocabulary-design.md` §9 (Tier 1 item 4 of
`docs/research/2026-08-31-ue-automation-framework-comparison.md`). Arc A landed 2026-09-03 and
left three things to build on: the verdict vocabulary to assert in, the Gauntlet precedence
cascade to imitate (written out in Arc A §2), and `TriedPaths`. Every UE claim below was
re-verified in the source dump (`.example/UnrealEngine-release/`) on 2026-09-03, per
`feedback_verify_dont_relay`; Playwright claims are from knowledge and are marked so — its
source is not on this machine.

**After this arc, automation is PARKED** (user directive, 2026-09-03). The roadmap is
Arc B → rendering (F2b onward) → cvars.

---

## 1. Why

`ArcaneTests` links neither `ArcaneRuntime` nor `ArcaneEditor`
(`feedback_green_gate_proves_nothing_about_hosts`), so no test today exercises the host
behaviours the vocabulary grades from. Three named facts have never been proven to occur:

1. **`settleCaptureFailed`** — the host records "no readback ever landed" as distinct from
   "captured fine, never agreed" (`RuntimeFrame.cpp:880-893`). The distinction was built
   *because an agent needs to tell them apart* — the comment says so — and nothing has ever
   observed either side of it.
2. **The `compare-missing-reference` absence** — a lane whose reference cannot be resolved at
   any level. Arc A's vocabulary grades it `Indeterminate`; no test forces it.
3. **`--settle-timeout` actually being spent** — that a run which cannot converge consumes
   BOTH bounds (attempts AND milliseconds, the conjunction this repo's own `--settle` fix
   installed) before bailing with a nonzero code, rather than the flag being a declared knob
   nobody has watched apply.

The witness harness forces each condition against the **real staged host executable** and
asserts, with `REQUIRE`, on the facts the host produces. The arc's standing lesson governs its
design: a declared knob is not an applied knob, and the only way to tell is to run it.

## 2. Shape — verified against both prior arts

UE's architecture is two layers, confirmed at source: **in-process** tests compiled into
modules the engine loads, triggered via `-ExecCmds="Automation RunTest <filter>"`
(`Gauntlet/Unreal/Automation/UE.Automation.cs:223,242`), and an **out-of-process** grader —
Gauntlet — that spawns the target and grades it by strict precedence, never linking it
(`Gauntlet/../LowLevelTests/Tests/Gauntlet.LowLevelTestNode.cs:247-320`: killed → exit code →
report-copy failed → parsed report → no-report fallback). Playwright is out-of-process by
construction (runner ↔ browser over a protocol pipe; knowledge, not source-verified).

**Arc B builds the grader layer only** (user ruling, 2026-09-03). The in-process layer — a
witness module riding the existing plugin ABI, UE's answer to reaching live state and the seam
the agent-introspection goal needs — is explicitly parked with the rest of automation. UE's
optional TCP messaging channel into running sessions (`UE.Automation.cs:260-264`) is recorded
here as prior art for whenever that unparks.

**The grader lives in ArcaneTests** as Catch2 cases tagged `[gpu][witness]` (user ruling,
2026-09-03). This buys every Arc A investment for free — `REQUIRE`, JSON/JUnit reporting,
baselines, exclusions, assert routing — and finally gives `Arcane::Verdict` a production
consumer. "ArcaneTests links neither host" stays true: it spawns them, which is Gauntlet's own
relationship to its target.

## 3. The scenarios

Three scenarios, one per unexercised behaviour. Each pins concrete report fields, not verdict
labels alone. All target `ArcaneRuntime` — the settle machinery is runtime-side, and the
reference-resolution change (§5) lives in shared host code, so one runtime scenario proves the
path both hosts use.

| # | Adversarial setup | Facts the host must produce |
|---|---|---|
| **W1 — settle bounds genuinely spent** | Scratch copy with the reference overwritten at **both** levels (`Verify/References/runtime-scene.png` and `vulkan/runtime-scene.png`) by a valid PNG with wrong pixels; run `--settle 4 --settle-timeout 2000 --compare runtime-scene`. The comparison is a **third conjunct in the convergence predicate** (`HostConfig.cpp`'s own comment), so a wrong reference holds the predicate false and the loop must spend BOTH bounds before bailing | Wall clock ≥ the timeout (measured by the helper), `settleAttemptsUsed >= 4`, `settleBailReason` ∈ {`attempts-bound`, `timeout-bound`} — not `converged`, not `capture-failed` — `exitReason: "compare-failed"`, nonzero exit. *Captured fine, never agreed* |
| **W2 — capture path broken** | A scene with **no camera** (candidate lever — pure content; camera is a scene component and no camera ⇒ nullopt, never identity), via `--scene <guid>` | Settle bails, `settleBailReason: "capture-failed"`, no screenshot written, nonzero exit — *readback never landed*. W1's exact counterpart; the pair proves the distinction `RuntimeFrame.cpp:893` records |
| **W3 — reference absent, search space reported** | Delete the reference at **every** level in the scenario's scratch copy — backend *and* shared; Arc A proved deleting one level silently falls back | Report carries `compare.resolvedLevel: "none"` **and the new `compare.triedPaths` array** listing the ordered candidates that failed, in the order tried |

**W1/W2's mirror-image assertions are the point**: each case asserts its own flag true *and*
the sibling flag false, so a regression that conflates the two states fails both cases, loudly,
in opposite directions.

**Two spec corrections made during plan grounding (2026-09-03), both from source:** the first
draft's W1 lever (an animating scene) was impossible — `--settle` freezes the render clock, so
animation cannot prevent convergence; and the report's settle keys are flat
(`settleAttemptsUsed`, `settleBailReason`), not nested. The wrong-reference lever replaced it and
is *stronger*: deterministic, content-only, and it proves the attempts-AND-timeout conjunction —
the exact `--settle` fix this repo's history records — by measuring the time actually spent.

**The W2 lever is verified RED-first in the plan.** If a no-camera scene does not force the
capture failure, the fallback lever is derived then, in source — not guessed here. The spec
records only the constraint: the lever must be content or CLI, never a host code change whose
only purpose is to be broken.

## 4. The spawn helper

`ArcaneTests/src/Helpers/HostWitness.{hpp,cpp}` — a CreateProcess wrapper returning process
facts as one struct:

```
struct WitnessRun {
    int  exitCode;
    bool timedOut;         // hard cap expired; process was killed
    bool progressingAtKill; // stdout grew OR CPU consumed in the final window (Arc A Task 10's rule)
    bool reportFound;
    bool reportParsed;
    nlohmann::json report; // valid only when reportParsed
};
```

- **Precedence is the grading order, imitated from Gauntlet:** scenarios assert on `timedOut` /
  kill facts before exit code, exit code before report presence, presence before contents. A
  killed host's leftover report is never believed.
- **One branch of UE's cascade is deliberately NOT imitated.** Gauntlet's no-report fallback
  grades a clean exit with no report as **Passed** ("Tests passed (no report to parse)",
  `LowLevelTestNode.cs:308-320`) — a pass with zero evidence, the vacuity trap its own
  `ReportParser.HasPassed()` guards against everywhere else (`successes > 0`,
  `LowLevelTests.ReportParser.cs:54`). For witness scenarios a missing or unparseable report is
  a **failure regardless of exit code**: every scenario exists to assert on report contents, so
  absent evidence is absence of the thing under test.
- **Watchdog = duration cap + progress fact**, matching Gauntlet's `MaxDuration` + heartbeat
  conjunction (`Gauntlet.UnrealTestNode.cs:60,74,325`): the hard cap always kills, and
  `progressingAtKill` distinguishes *wedged* from *slow* in the failure message. One wedged
  host cannot hang the suite (Arc A's unkillable-process guard, ported).
- **Verdict mapping**: the helper maps facts → `Arcane::Verdict` via the exported C++
  vocabulary, and scenarios `REQUIRE` the verdict *and* the underlying facts — the vocabulary's
  first production consumer, closing the final review's I8 the sharp way.
- **The helper captures host stdout/stderr to files in the scenario's scratch dir** — required
  by the progress rule (stdout growth is half its conjunction) and by diagnostics: both prior
  arts keep process output with the failure artifact (Gauntlet tails and saves logs;
  Playwright captures stdio into the report — knowledge). The files ride the kept-on-failure
  scratch dir (§6). Arc A Task 10's `Start-Process -RedirectStandardOutput` precedent, in C++.
- The helper never launches concurrently-overlapping hosts; scenarios are ordinary sequential
  Catch2 cases.

## 5. TriedPaths — one shared-code change

`ArcaneClient/src/Arcane/Host/ReferenceImages.cpp` (shared by both hosts) records the ordered
candidate list it actually tried; `VerifyReport` gains `compare.triedPaths`, **schema 4 → 5**.

**This exceeds the UE ancestor rather than copying it**: UE only *logs* the tried paths
(`Developer/ScreenShotComparisonTools/Private/ScreenShotManager.cpp:227-231` — verified
2026-09-03: `UE_LOG` of "tried the following paths", never placed in the comparison result).
Ours goes in the report, machine-readable, because the research doc's own finding was that
UE's log parser lacks the property its XML sibling has — log-scraping is the weaker surface.

Knock-ons, priced by precedent:

- **Plugin ABI 19 → 20.** `VerifyReport` gains a member and an exported setter — the same class
  of move as v18 and v19. Restamp `ReferenceProject/ReferenceProject.arcproj` and Gacha's
  `Game/Aphelyon.arcproj`; extend the `PluginABI.hpp` comment block in the established voice.
- **The schema constants live in three places** — `VerifyReport.hpp`
  (`kOldestSupportedSchemaVersion`/max), the published vocabulary file
  (`automation-vocabulary.txt`'s `reportSchemaMin/Max` lines), and `golden-gate.ps1`
  (`$script:ReportSchemaMin/Max`). This is not drift risk; it is **the first live test of
  Arc A's anti-drift pin**: forget any one of the three and `-SelfTest` fails on the mismatch.
  The plan sequences this change so the pin is watched failing once (wrong constant) before it
  is watched passing.
- `kOldestSupportedSchemaVersion` stays 3 unless the plan finds a reason to move it; widening
  the supported range is the default, narrowing it is a decision.

## 6. Hygiene — fresh copy, never mutate-and-restore

**Both prior arts converge on fresh environment over restore**: Playwright gives every test a
fresh browser context (knowledge); Gauntlet installs builds per-run and saves per-role artifact
directories out (`Gauntlet.UnrealSession.cs:1031`, `SaveRoleArtifacts`). Adopted:

- Each scenario **copies the host's staged tree into the scratch area**, applies its
  adversarial mutation to the copy, and runs the host from there. Live staged trees are never
  touched — no restore logic, no self-heal, no serialization hazard against gate runs.
- **On failure, the scratch dir is kept** — copy, report, logs — as the diagnostic artifact
  (Playwright's artifacts-on-failure; Gauntlet's `SaveRoleArtifacts`). On pass it is deleted.
- `-SelfTest` keeps its own mutate-restore-self-heal pattern: it tests the *gate*, whose
  subject is the real staged tree. The witness tests the *hosts*, which run identically from a
  copied tree.

This finding is recorded for the Servitor corpus: **fresh environment beats
mutate-and-restore wherever a copy is affordable** — convergent across both prior arts,
independently derived by neither of us until the comparison was run.

## 7. Tags, CI, and baselines

- Cases are tagged **`[gpu][witness]`** — they boot a real device, so the existing conventions
  do the wiring with zero new mechanism: the hosted GitHub lane's `~[gpu]` filter excludes them
  untouched (its WARP premise stays unverified, per Arc A Ruling 28), Jenkins' unfiltered run
  includes them (it has the GPU), and the six committed `~[gpu]` baselines **do not move by
  construction**.
- The future Jenkins `"unfiltered"` baseline (Arc A's commented follow-up at the call site)
  will include witness counts when it is read off a real run — no action here.
- Suite cost: three host boots, a few seconds each, plus one staged-tree copy per scenario.
- The witness scenarios require the hosts to be **built and staged**. When
  `ArcaneRuntime.exe` or its staged tree is absent, the cases fail with an actionable message
  naming the build step — a hard failure, not a skip. `SKIP` is for capability the machine
  lacks (no GPU); an unbuilt tree on a GPU machine is an incomplete build, the same ruling
  Arc A made for the exclusions file (Ruling 14) and the vocabulary pin (B2).

## 8. Testing the witness itself

- **The helper gets unit tests against `cmd /c exit N`** as the spawned target — exit-code,
  timeout, kill, and missing-report paths, no new fixture binary.
- The `[witness]` cases are the integration layer; TDD applies throughout, and the W2 lever is
  proven RED-first (§3).
- Every new refusal or failure path added by this arc is **watched firing once** before the
  arc closes — the standard Arc A's final review enforced, applied from the start this time.

## 9. Out of scope, named

- **The in-process witness module** (plugin-ABI layer) — parked with automation.
- **Editor-lane scenarios** — shared code is proven via runtime; an editor boot buys no new
  coverage at real cost.
- **Any scenario-config file format** — three scenarios get three cases, not a framework.
- **Any generic cascade engine** — the precedence lives in assert order, not machinery.
- **A control channel into running hosts** — UE's TcpMessaging analog; recorded as prior art
  only.
- **Retries.** Both targets carry a flake mechanism (Playwright per-test retries — knowledge;
  Gauntlet `-ResumeRunTest`, `UE.Automation.cs:177`). The witness deliberately has none: a
  retry masks exactly the fact a witness exists to observe, and this repo's flake mechanism is
  already the exclusion list with a checked expiry. A flaky witness scenario is a finding, not
  a nuisance.

## 10. Decisions log

| Decision | Choice | Why |
|---|---|---|
| Layers | Grader only | User ruling 2026-09-03; the three behaviours need nothing more; module layer parked with automation |
| Home | Catch2 in ArcaneTests | Inherits every Arc A investment; gives `Verdict` a production consumer; Gauntlet precedent — spawn, never link |
| Hygiene | Fresh scratch copy per scenario, artifacts kept on failure | Convergent prior art (Playwright contexts, Gauntlet installs); deletes restore/self-heal complexity and the gate-collision hazard |
| TriedPaths surface | In the report, not the log | UE logs it and its log parser is its weaker surface; machine-readable beats scraping |
| Tags | `[gpu][witness]` | Existing conventions wire CI and baselines with zero new mechanism |
| Absent host tree | Hard fail, actionable message | An incomplete build on a capable machine is not a skippable condition (Arc A Rulings 14, B2) |
| Watchdog | Hard cap + progressing-at-kill fact | Gauntlet's duration+heartbeat conjunction; Arc A Task 10's progress rule reused |
| W2 lever | No-camera scene, verified RED-first | Content-only forcing; a host code change whose purpose is to be broken is forbidden |

---

## Landed — 2026-09-03

Implemented on `main` as `aa29454e..67b8c1ed` (the plan, then six implementation commits), with
Gacha's ABI restamp at `4219c644`. Everything below is measured on the dev desk (RTX 3070,
Vulkan + D3D12), not projected.

### Against what this spec asked for

| Spec item | Outcome |
|---|---|
| §5 `triedPaths` on `ResolveReference`, report schema **4 → 5** | Landed. The real try order is **backend-keyed first, shared second** — as §3 assumed — and is now pinned by test rather than by assumption |
| §4 the spawn helper | Landed as `ArcaneTests/src/Helpers/HostWitness.{hpp,cpp}` with 7 `[witness-unit]` cases against `cmd /c exit N` |
| §3 W1 — settle bounds genuinely spent | Landed, with **two measured corrections to its invocation** (below) |
| §3 W2 — capture path broken | **REMOVED.** The fact has no content-or-CLI lever on either host (below) |
| §3 W3 — reference absent, search space reported | Landed, but the path is **fail-fast, not a settle bail** (below) |
| §5 ABI 19 → 20, restamps, the three-place schema pin | Landed. The pin was **watched failing once** before it was watched passing |

### Measured figures

`ArcaneTests.exe "~[gpu]"`, re-derived per configuration and committed to
`scripts/automation-baselines.json`:

| Configuration | Assertions | Cases | Previous baseline |
|---|---|---|---|
| Debug | **52462** | **1316** | 52431 / 1304 |
| Release | **52462** | **1316** | 52431 / 1304 |
| Dist | **52394** | **1310** | 52363 / 1298 |

+31 assertions / +12 cases in every configuration, and the Debug↔Dist gap holds at exactly the
68 assertions / 6 cases the baseline file has always documented. `ArcaneTests.exe "[gpu]"` on
this desk: **62042 assertions in 27 test cases**, all passing — the 25 cases that predate the
arc, plus W1 and W3.

**§7's "the six committed `~[gpu]` baselines do not move by construction" was true only of the
scenarios, and is corrected here.** The `[witness][gpu]` cases are indeed outside the filter —
`--list-tests "[witness]"` lists exactly W1 and W3, `--list-tests "~[gpu] [witness]"` lists
zero — but the arc's *unit* cases are inside it, and they are the whole of the rise. The
arithmetic reconciles exactly: 17 `TEST_CASE`s added, minus 3 schema-4 cases superseded by
schema-5 equivalents, = 14 net, of which 2 are the `[witness][gpu]` scenarios — leaving **+12
inside `~[gpu]`**.

Golden gate, Release: `gatePassed: true` read from
`bin/Release-windows-x86_64-md/golden-gate-summary.json`, four lanes, all `diffCount=0` and
`maxLocalDifference=0.0`. `ArcaneRuntime/dx12/runtime-scene` is `PassedOnFallback`
(`resolvedLevel=shared`, expected backend) — pre-existing and unrelated to this arc: only
`vulkan/` carries a backend-level `runtime-scene.png`. `-SelfTest`: **`SELF-TEST PASSED -- all 4
lane(s) launched and caught the broken scene`**, writing its inverted verdict to the *separate*
`golden-gate-selftest-summary.json`, so a self-test can never overwrite a real gate verdict.

### W1 — landed, at an invocation the desk corrected twice

The lever held exactly as §3 designed it: the reference overwritten at **both** levels by a valid
PNG with wrong pixels, and the comparison — the convergence predicate's third conjunct — holding
the predicate false until both bounds are spent. Measured: `NOT CONVERGED after 7 attempt(s) /
4889 ms`, `settleAttemptsUsed: 7`, `settleBailReason: "timeout-bound"`,
`exitReason: "compare-failed"`, exit 3, wall 7943 ms, `diffCount: 903633` (ratio 0.99) at
`resolvedLevel: "backend"`. *Captured fine, never agreed* — witnessed.

Two of the spec's numbers were wrong, and both corrections are load-bearing:

- **`--frames 60`, not the plan's 10.** `--settle` freezes the render clock, and `ShaderCompiler`
  *dispatch* is gated on that same clock by a 200 ms debounce. At the headless fixed step of
  1/60 s, **any frame budget below 13 freezes the clock before the debounce elapses in sim
  time**, so queued compiles never dispatch, `IsIdle()` never becomes true, and the predicate's
  second conjunct can never be satisfied — on *any* scene, with *any* reference. 60 is also what
  `scripts/golden-gate.ps1` passes, so W1 and the gate now exercise the same shape of run.
- **`--settle-timeout 4000`, not 2000.** `wallMs` is spawn→exit and so includes ~2 s of host
  boot. At 2000 the negative control measured 2130 ms ≥ 2000 and the assertion **passed on a
  converging run** — an assertion that could not fail on any input. At 4000 the two cases
  separate by ~2 s in each direction (converging 2.1 s, bound-spending ~7.9 s). This is
  `feedback_default_values_are_not_measurements` in another form: a bound not *separated* from
  the thing it exists to exclude is not a bound.

### W2 — removed, and why that is this arc's headline finding

The spec's candidate lever (a no-camera scene, §3) **does not hold**, and no other content-or-CLI
lever exists. The probe is unambiguous in both directions: at `--frames 10` the run bails
`timeout-bound`; at `--frames 60` it reports `settleBailReason: "converged"` **with a capture
block in the report**. The no-camera scene renders and captures perfectly well — it just renders
nothing.

The derivation, verified in source rather than inferred from the probe:

1. `io.settleCaptureFailed = !io.previousCaptureValid` (`RuntimeFrame.cpp:893`) is the **only**
   writer, and `previousCaptureValid` is set true by every attempt whose `ReadCapture` returned
   true (`:872`). So `capture-failed` requires **every** attempt's readback to fail.
2. **The identity that closes it.** `graphFrame.capture = willBeLastFrame && (screenshot||report)`
   (`RuntimeFrame.cpp:619,627`) is evaluated in `RenderGraph` before the frame counter advances;
   the settle branch runs only when `pastBase` (`:701`, gated at `:735`), evaluated in
   `CaptureTail` after the counter advances. **They are the same frame predicate, by design** —
   the comment at `:606-618` says so in prose. With `--report` present, every settle attempt is
   therefore preceded by a frame that armed the readback node. The editor host states the same
   identity in a single variable (`EditorAppFrame.cpp:2794-2798`, gate at `:2878`), so it is not
   an escape hatch either.
3. What remains is genuine GPU/driver failure only: `read == false` at `:740` requires
   `ReadCapture` to refuse, and it refuses in exactly two places — the capture node never ran
   (`NriGraphContext.cpp:1915`) or `MapBuffer` returned null (`:1933-1936`). **No scene and no
   command line can express either.**

`--crash-gpu N` was considered and rejected: it faults the device, `device-lost` breaks
`MainLoop` ahead of the bail, `SettleVerdictReached` is therefore false and `SetSettle` is never
called — the report would carry no settle keys at all, let alone `capture-failed`.

Per this spec's own standing constraint — *the lever must be content or CLI, never a host code
change whose only purpose is to be broken* (§3) — W2 was not written. Two consequences worth
stating plainly:

- **The `RuntimeFrame.cpp:893` distinction remains only half-witnessed.** W1's mirror assertion
  (`settleBailReason != "capture-failed"`) is the observable half; the positive half stays
  covered where it is actually reachable, at the unit level
  (`ArcaneTests/src/VerifyReportTest.cpp`, which calls `SetSettle(..., captureFailed=true)`
  directly).
- **This is a finding about the host, not a gap in the harness.** §1 asserted the distinction was
  built "because an agent needs to tell them apart". That remains true — but one side of it is
  unreachable from outside the process, which is worth knowing before anyone designs a scenario
  against it again.

### W3 — landed; the absence is fail-fast, not a settle bail

The missing-reference path does not bail out of the settle loop, because it never enters it.
Measured: exit **4**, `exitReason: "compare-missing-reference"`, `framesRendered: 0`, and the
settle keys **absent rather than zeroed** — `SetSettle` is guarded on `SettleVerdictReached`,
whose own doc comment names exactly this case. `RuntimeApp::MainLoop` resolves `--compare` before
boot completes and fail-fasts on `ReferenceLevel::None`; the report-writer's `exitReason` chain
checks `m_compareMissingFatal` **before** `settleFailed`, specifically so this case cannot
misreport as `settle-not-converged`. `compare.triedPaths` carries both candidates in order,
backend-keyed first, shared second — §5's whole point, machine-readable, delivered.

A naive expectation (`settle-not-converged` at some non-zero attempt count) would have been
wrong. The test pins the *absence* of the settle keys precisely because that is what separates W3
from "W1 with a different reference".

### The watched failure

§5 predicted the three-place schema constant would be its own anti-drift test. It was, and it
fired verbatim with the header at 5 and `golden-gate.ps1` still at 4:

```
SELF-TEST FAILED -- reportSchemaMax has DRIFTED: the engine publishes 5, this script has 4.
Change BOTH in the same commit: VerifyReport::kSchemaVersion and this script's $script:ReportSchemaMax.
```

— accompanied by all four lanes going `Indeterminate` ("schemaVersion 5, outside this gate's
supported range 3..4") and by the vacuity guard ("A gate that cannot fail is not a gate"). Arc A's
pin works, and has now been observed working.

### Parked — a host defect this arc found and deliberately did not fix

**`--frames <13` combined with `--settle` can never converge, on any scene** (the clock freezes
before the 200 ms shader-compile debounce, above), yet the host's bail message reads *"timeout
governed -- raise --settle-timeout"* — a knob that cannot help. This is exactly the
send-the-caller-to-a-useless-knob failure the settle vocabulary was built to prevent, and it is a
second instance the vocabulary does not currently name. It is out of scope here (Arc B is the
grader layer only, §2), and is recorded for whoever unparks automation.
