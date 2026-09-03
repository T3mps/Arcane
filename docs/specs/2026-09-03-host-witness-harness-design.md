# Host witness harness — Arc B design

**Status:** design, 2026-09-03. Approved in brainstorming; implementation plan to follow.

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
3. **`--settle-timeout` actually being spent** — that a genuinely unstable scene consumes the
   bound and exits `settle-not-converged` with a nonzero code, rather than the flag being a
   declared knob nobody has watched apply.

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
| **W1 — settle bound genuinely spent** | ReferenceProject scene animating on `Time` (PulseSprite computes `sin(Time * PulseSpeed)`), `--fixed-time` deliberately absent, small `--settle-timeout` | `exitReason: "settle-not-converged"`, nonzero exit code, `settle.attemptsUsed > 0`, capture-failed **false** — *captured fine, never agreed* |
| **W2 — capture path broken** | A scene with **no camera** (candidate lever — pure content; camera is a scene component and no camera ⇒ nullopt, never identity) | Settle bails, `settle.result: "capture-failed"`, no screenshot written, nonzero exit — *readback never landed*. W1's exact counterpart; the pair proves the distinction `RuntimeFrame.cpp:893` records |
| **W3 — reference absent, search space reported** | Delete the reference at **every** level in the scenario's scratch copy — backend *and* shared; Arc A proved deleting one level silently falls back | Report carries `compare.resolvedLevel: "none"` **and the new `compare.triedPaths` array** listing the ordered candidates that failed, in the order tried |

**W1/W2's mirror-image assertions are the point**: each case asserts its own flag true *and*
the sibling flag false, so a regression that conflates the two states fails both cases, loudly,
in opposite directions.

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
- **Watchdog = duration cap + progress fact**, matching Gauntlet's `MaxDuration` + heartbeat
  conjunction (`Gauntlet.UnrealTestNode.cs:60,74,325`): the hard cap always kills, and
  `progressingAtKill` distinguishes *wedged* from *slow* in the failure message. One wedged
  host cannot hang the suite (Arc A's unkillable-process guard, ported).
- **Verdict mapping**: the helper maps facts → `Arcane::Verdict` via the exported C++
  vocabulary, and scenarios `REQUIRE` the verdict *and* the underlying facts — the vocabulary's
  first production consumer, closing the final review's I8 the sharp way.
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
