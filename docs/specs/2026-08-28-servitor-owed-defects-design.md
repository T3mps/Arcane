# The five owed defects, and closing the `-AdvisoryLanes` switch

**Status:** design, approved in brainstorming 2026-08-28. This is **arc 2** of the
automation-tooling close-out. Arc 1 (`docs/specs/2026-08-27-headless-rename-design.md`) is
closed and pushed at `ff10d562`.

**The single input this spec was written from** is
`docs/2026-08-26-servitor-closeout-and-desk-verify.md` — §3 for the defects' mechanisms and §6
for the scope. That document is the record; this one is the design.

---

## Why

Four of these five defects were found **by the golden gate on its first real use**, which is the
gate working. The fifth was found while re-reviewing arc 1's spec. None has been fixed, and two
of them are the reason `scripts/golden-gate.ps1` still ships a `-AdvisoryLanes` switch that
demotes both `ArcaneEditor` lanes to non-gating.

**That switch is the tracking item, and it is the thing this arc exists to delete.**
`golden-gate.ps1`'s own header says so: *"WHEN BOTH LAND, DELETE -AdvisoryLanes AND the
Jenkinsfile argument that passes it. This switch IS the tracking item."* The close-out doc states
the cost of leaving it in place just as plainly: **an advisory lane is one people stop reading.**

This project has now hit the stale-marker failure four times in one checklist, and once more in
arc 1 (a retired gate baseline still cited as current in two places). A switch whose deletion
condition is met but which is still in the tree is the same failure. Deleting it is not
housekeeping; it is the deliverable.

---

## Decisions

### 1. One arc, one spec, one fixed order

**T1** `--fixed-dt nan` → **T2** the settle conjunction → **T3** `VerifyReport` schema 3 →
**T4** `ProjectOpenOptions` → **T5** ArcaneHub `golden_run()` → **T6** USER desk checkpoint →
**T7** delete `-AdvisoryLanes`.

The order is not arbitrary:

- **T1 first** because §3 names it *"the first thing to fix on the next touch of that function"*,
  and T2 adds a flag to that same validation block.
- **T2 before T3** because `settleBailReason`'s value set does not exist until the second bound
  does. Building T3 first means designing a field whose legal values are still open.
- **T4 before T6** because re-blessing `editor-ui` is only legitimate once its input is
  deterministic. §3 is explicit: *"Never re-bless `editor-ui` to make this go away."*
- **T7 last** because deleting the switch requires **evidence** both editor lanes went green, and
  only a GPU desk run can produce that.

T5 is independent of all of it and is placed where it is because it is a different language and
build (Rust/Tauri), so it is a context switch best taken once.

### 2. The settle fix is a conjunction WITH pacing

**The defect** (§3.1): `--settle` counts **attempts** while the condition it waits on
(`ShaderCompiler::IsIdle()`) is denominated in **milliseconds**. At ~3.3 ms/attempt a 30-attempt
budget is spent in ~100 ms, so a fast build bails before background compilation can drain.
Verified at source for this spec: there is **no sleep, no wait and no pump between attempts** in
either host's settle path. The two `sleep_for(1ms)` calls in `ArcaneRuntime/src/RuntimeFrame.cpp`
are unreachable there — `:173` sits behind `IsMinimized()`, whose own comment states it is false
for the whole of a `--headless` run, and `:667` is the `FrameOutcome::Skipped` branch.

**The fix has two halves, and only having both closes the defect.**

**Half one — a conjunction.** Add `--settle-timeout <ms>`. Give up only when **both** bounds are
spent: `attemptsUsed >= attempts AND elapsed >= timeout`. Unreal independently corroborates the
shape ([UE 5.4 `AutomationScreenshotOptions`](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AutomationScreenshotOptions?application_version=5.4)):

> `delay` — *"The delay before we take the screenshot (measured in seconds). **Both this delay and
> the frame delay must be met** before the screenshot is taken."*

Epic denominates its two bounds in different units — seconds and frames — evidently having found
neither sufficient alone. **Scope of that citation, stated because it matters:** Unreal's
conjunction is a **wait before a single capture**, not a poll. It corroborates the *shape* of the
fix. It does **not** decide how to pace a retry loop, because Unreal has no retry loop.

A disjunction (bail when *either* bound is spent) was considered and rejected: it still bails the
moment attempts run out, which is the defect.

**Half two — pacing.** A fixed **50 ms** interval between attempts. Without it the conjunction is
brute force: the loop burns its attempt budget in ~100 ms and then spins ~1500 further offscreen
frames waiting on CPU-side compilation. Pacing makes each attempt mean something.

Playwright is the closer analogue here, and it is the system our comparator is already a port of
([`toHaveScreenshot`](https://playwright.dev/docs/api/class-locatorassertions#locator-assertions-to-have-screenshot-1)):

> *"This function will **wait until two consecutive locator screenshots yield the same result**,
> and then compare the last screenshot with the expectation."*

That is our settle loop nearly word for word. Playwright bounds by **time alone** and has no
attempt count; our conjunction is a superset, keeping the attempt bound on Unreal's evidence.

**Consequence, stated rather than discovered later:** under a conjunction, `--settle N` means
**"at least N attempts"**, not "up to N". With a 50 ms interval and a 5000 ms timeout a
non-converging run consumes ~94 attempts. **The flag's help text must be reworded to match.**
`settleAttemptsUsed` (decision 5) reports the real number, not the budget.

**Four mechanics that must not be left to the implementer's judgment:**

1. **`elapsed` is measured from the moment the settle phase begins** — the first frame on which
   `pastBase` is true — using a monotonic clock (`std::chrono::steady_clock`). **Not** from
   process start, which would charge boot, project open and the `--frames` budget against the
   convergence budget and make the timeout depend on how long the scene took to load.
2. **The 50 ms interval is a named constant, not a flag.** The CLI gains exactly one new option
   in this arc. If the interval ever needs tuning that is evidence for a later decision, not a
   knob to ship speculatively.
3. **`--settle-timeout` requires `--settle`** and is refused at parse time otherwise, with a
   message naming both flags. A timeout on a loop that never runs is a caller error, and this
   codebase refuses those loudly rather than ignoring them.
4. **`--settle-timeout` joins the `--headless`-only set** (`wantsOffscreenOnly`,
   `HostConfig.cpp:165`), alongside `--fixed-dt`, `--probe`, `--report`, `--settle` and
   `--compare`. A settle timeout in a windowed run is meaningless for the same reason `--settle`
   already is.

### 3. The two constants have DIFFERENT provenance, and the spec says which is which

| constant | value | provenance |
|---|---|---|
| `--settle-timeout` default | **5000 ms** | **Inherited.** Playwright's `toHaveScreenshot` timeout *"Defaults to `timeout` in `TestConfig.expect`"*, and [`TestConfig.expect`](https://playwright.dev/docs/api/class-testconfig) defaults to 5000 ms. |
| poll interval | **50 ms** | **Ours.** Playwright documents no retry cadence anywhere in its API reference, and Unreal has no poll to pace. This is a judgment call, not a citation. |

Recording both as if they were inherited would be false provenance. The standing rule this arc
works under is: **defer to the reference implementations where they have made a decision; where
they are silent, choose and say that we chose.**

### 4. `settleBailReason` names the BINDING bound, not the tripped one

Under a conjunction nothing "trips first" — the loop ends when the *later* of the two bounds is
satisfied. So the useful fact is **which bound was binding**, because that is the one whose knob
would change the outcome:

- `converged`
- `attempts-bound` — attempts were satisfied last; raising the timeout will not help
- `timeout-bound` — time was satisfied last; raising attempts will not help
- `capture-failed`

This is strictly more useful than "which expired first" would have been, and it is why the fixed
interval matters: with a derived interval (`timeout / attempts`) both bounds would be satisfied at
the same instant by construction and the distinction would collapse.

### 5. `schemaVersion` 2 → 3 carries three changes, in ONE commit

1. **`settleAttemptsUsed`** — the honest count.
2. **`settleBailReason`** — decision 4's vocabulary.
3. **`"mode"` becomes `"headless"`** — see decision 6.

**The two hosts disagree today and must be reconciled in the same commit.** The runtime leaves the
counter at 0 in its defensive branch; the editor forces it to the budget —
`ArcaneEditor/src/App/EditorAppFrame.cpp:2860` is literally
`m_settleAttemptsUsed = m_config.settleAttempts;`. Ship the field without fixing that and **the
new field lies on the editor**, which is worse than its absence: an absent field is an honest
gap, a present wrong one is a false report an agent will trust.

`ArcaneTests/src/VerifyReportTest.cpp` asserts the report parses **without linking the engine** —
that property is the Servitor boundary and must survive. Its `schemaVersion` assertions are at
`:71`, `:652`, `:676`; its `"mode"` assertions at `:73` and `:159`.

### 6. `"mode": "offscreen"` becomes `"headless"` on this bump

`ArcaneClient/src/Arcane/Host/VerifyReport.cpp:516` emits the **mode's** machine-readable name in
the **technique's** word. It is the last inversion of arc 1's Decision 3 left in the tree, and the
only one a machine reads.

Arc 1 deliberately did **not** change it, because changing a wire value without a version bump is
the silent contract drift this project keeps hitting. **A `schemaVersion` bump is precisely the
versioned moment at which a wire value may change — that is what the version is for.** The
close-out doc's tracking note recorded a dated measurement that no other consumer keys on the
string, and the gate reads `exitReason`, not `mode`.

A one-version grace period emitting both was considered and rejected: this repo took a hard break
with no alias on exactly this vocabulary one arc ago, and a grace period here would contradict
that precedent immediately.

### 7. `Project::Open` gets a DEFAULTED options struct

**The defect** (§3.2): the editor's verify capture includes the Assets panel, which enumerates
`Saved/Diagnostics/` — a directory the editor itself writes crash and hang captures into. The
golden image therefore depends on the machine's failure history. It manifests as exactly **24
anti-aliased pixels** of the Assets scrollbar thumb, at byte-identical coordinates on both
backends.

`Project::Open(pathOrFile, onProgress)` has **no awareness of `HostConfig`**, so this is a real
engine-API change, not a local conditional. Fixing the symptom one layer up in the editor would
not hold: the `diag://` mount is created in the engine, at
`ArcaneClient/src/Arcane/Project/Project.cpp:444`, and registered **unconditionally** — `:432`'s
own comment explains why, and that reasoning still stands.

```cpp
struct ProjectOpenOptions
{
    bool mountDiagnostics = true;
};
```

Passed as a **defaulted** third parameter and threaded `HostConfig` → `Runtime::OpenProject` →
`Project::Open`. Defaulting is what makes this tractable: there are **41 call sites**, of which
**39 are in `ArcaneTests`** and continue to compile untouched.

Per user direction the struct is **intended to absorb future per-open engine settings**; it is not
a single bool wearing a struct's clothes.

### 8. `golden_run()` keys on `--compare`

**The defect** (§6.2): `ArcaneHub/src-tauri/src/launch.rs:104` is
`extra.iter().any(|a| a.starts_with("--golden-"))`, and **no `--golden-*` flag exists anywhere**
in ArcaneClient, ArcaneRuntime, ArcaneEditor or ArcaneTests. It is retired vocabulary from a
pre-Plan-A design. So the guard at `:335` (`Some(3) if !golden`) **always** matches, and a genuine
golden compare failure is reported to the user as *"that project is already open in another
editor"*; the correct arm at `:338` is **unreachable**.

The replacement is **`--compare`** — the only flag that can produce a post-boot exit 3. The
function's own doc comment already states the principle it violates: *"`--frames` is likewise NOT
evidence and is deliberately absent: on its own it cannot produce a post-boot 2 or 3."*

Its existing test, `golden_run_sees_golden_vocabulary_only` (`:585`), passes by asserting the same
retired vocabulary — **a test that cannot fail.** The replacement test must assert against flags
the engine actually registers, and must be capable of failing.

**This is sequenced after arc 1 deliberately**, so it is not written against a flag name that was
about to change.

### 9. `-AdvisoryLanes` dies in THIS arc, not a later one

Once T2 and T4 land and the desk pass confirms both editor lanes at 0-diff, T7 deletes
`-AdvisoryLanes`, its `Jenkinsfile` argument, and `Write-OwedDefects` — which hardcodes
editor-specific text regardless of the lane named (§6.3 item 8) and dies with the switch.
`advisoryFailures` in `golden-gate-summary.json` should then read **0**.

Splitting the close-out into a later arc was considered and rejected: it would leave the tracking
item outliving the arc that fixes its condition, which is the exact failure mode named in "Why".

---

## Scope

Measured 2026-08-28 at `ff10d562`.

| Surface | Size | Action |
|---|---|---|
| `Project::Open` call sites | **41** (39 in `ArcaneTests`, 2 in engine/editor) | defaulted param — all 41 keep compiling |
| `VerifyReport` `schemaVersion` assertions | 3 (`VerifyReportTest.cpp:71, 652, 676`) | 2 → 3 |
| `VerifyReport` `"mode"` assertions | 2 (`VerifyReportTest.cpp:73, 159`) | `"offscreen"` → `"headless"` |
| settle loop hosts | 2 (`RuntimeFrame.cpp`, `EditorAppFrame.cpp`) | must move in lockstep |
| ArcaneHub `golden_run` sites | 3 (`launch.rs:104`, `:335`, `:585`) | key on `--compare`; test must be able to fail |
| gate lanes demoted to advisory | 2 (`ArcaneEditor` × dx12/vulkan) | back to hard-gating at T7 |

---

## Verification

- **Gate baseline to hold: 52207 assertions / 1255 cases**, 0 warnings across Debug/Release/Dist.
  This is arc 1's new baseline; **52203/1254 is retired.** Each task adds its own cases.
- **T1** — a `HostConfigTest` case asserting `--fixed-dt nan` is refused with exit 2. It must fail
  before the fix.
- **T2** — the bail predicate is **extracted as a pure function** and unit-tested without a clock,
  a GPU or a frame loop: given (attemptsUsed, attempts, elapsed, timeout) it must return the
  conjunction and name the binding bound. Pacing itself is desk-verified, not unit-tested.
  Plus `HostConfigTest` cases for `--settle-timeout`'s parse, its `--headless` gate, and its
  refusals.
- **T3** — `VerifyReportTest` cases for the three new/changed fields at `schemaVersion` 3, and the
  **counter-semantics reconciliation asserted for both hosts**, not just one.
- **T5** — a Rust test that `golden_run` is true for `--compare` and **false for `--frames`
  alone**, i.e. one that fails if the function reverts to retired vocabulary.
- **T6 (desk, GPU)** — `golden-gate.ps1 -Configuration Debug` with **no `-AdvisoryLanes`**: all
  four lanes hard-gating, `gatePassed: true`, `advisoryFailures: 0`. Assert on those two fields in
  `golden-gate-summary.json`, **never the exit code**.
- **`--bless` writes to the STAGED tree** — point `--project` at the source or you bless a build
  artifact.

---

## Risks and open items

- **The Linux `IsIdle()` stub returns `true` unconditionally.** After T2 the timeout becomes the
  **only** real convergence signal on that platform, since the idle conjunct is vacuous there.
  This is currently recorded only as outstanding evidence (§6.5). It does not block this arc —
  the Linux agent is lane-check only — but T2's design must not assume `IsIdle()` is meaningful.
- **The arc's real acceptance criterion — both editor lanes green — can only be proven at the
  desk.** Same shape as arc 1's Task 3. An agent cannot close this arc alone.
- **`--settle N`'s meaning changes** from "up to N" to "at least N". Every current caller passes
  30. The change is deliberate and the help text moves with it, but it is a semantic change to an
  existing flag and belongs in the commit message, not only here.
- **The Jenkins pipeline has never run `-AdvisoryLanes`** (§6.5). If T2/T4/T7 land first the
  question dissolves before it is ever answered. Either outcome is fine; assuming it works is not.

---

## Non-goals

- Re-blessing `editor-ui` **before** T4 lands. §3 forbids it explicitly: the moving input is
  written by the thing under test, so a re-bless goes green today and red at the next crashed run.
- Renaming the `Offscreen*` render-technique layer. Arc 1 decision 3 stands.
- Unreal's **direction 2** — a first-class `ExcludeTest` list with a mandatory reason, reported as
  Skipped (research doc Finding Q4-F1). Real, and not this arc.
- The deferred minors from arc 1's ledger, and the `HostConfigTest.cpp` case named *"still reports
  the `--compare` mistake"* that asserts only refusal and exit 2 so it passes under either
  ordering (§6.4). Worth fixing; not here.
- `FastStats` 4K memory (arithmetic, not a measured run) and mesh picking (`CollectPickables` has
  no `MeshRenderer` view) — both named in §6.5 so they are not assumed closed.

---

## What this closes

With T7 landed, the automation tooling has no owed defects and no advisory lanes: four
hard-gating golden lanes on two hosts and two backends, a settle loop bounded in the unit it
actually waits on, a report that carries an honest convergence count and a stated bail reason at a
versioned schema, a Hub that reports a compare failure as a compare failure, and one word per
sense throughout.
