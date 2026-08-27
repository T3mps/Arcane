# Servitor close-out — desk-verify checklist and owed work

**Status:** Plan B is agent-complete and merge-ready; branch pushed to `origin/main` at
`015531b7` (fast-forward `d1804591..015531b7`, 67 commits). Gate **52203 assertions /
1254 cases**, Debug and Release green, 0 warnings across Debug/Release/Dist.

**The only thing between this arc and done is the desk pass in section 2, which needs a
display and a physical GPU and therefore cannot be run by an agent.**

This document exists because the arc's working record — the SDD ledger at
`.superpowers/sdd/2026-08-25-plan-b-servitor-comparator/progress.md` — is gitignored scratch
that gets deleted once the plan closes. Everything below would die with it. The plans and
specs it refers to are in git; the ledger is not.

Plans: `docs/plans/2026-08-23-agent-verification-offscreen-hosts.md` (Plan A),
`docs/plans/2026-08-25-plan-b-servitor-comparator.md` (Plan B).
Specs: `docs/specs/2026-08-23-agent-verification-offscreen-design.md`,
`docs/specs/2026-08-25-package-tiering-design.md`.

---

## 1. Three corrections to Plan B's own Task 14 checklist

**Read these before running the checklist, or a correct result will read as a failure.**

- **Checklist A, bullet 1** says `golden-gate.ps1 -Configuration Debug` should give
  "exit 0, four passing comparisons (two hosts × two backends)". **No longer the expected
  result.** The two editor lanes now run *advisory* (see §3), so the expected outcome is
  **exit 0, two passing runtime comparisons, and two advisory editor lanes reporting
  KNOWN-RED and printing the two owed defects**. A hard-red editor lane would be the
  surprise, not the advisory one.
- **Checklist B, bullet 2** says to confirm "`editor-ui` ended up backend-split while
  `runtime-scene` stayed shared". **The actual outcome is the exact inverse.** The tree
  holds `Verify/References/runtime-scene.png` *and* `Verify/References/vulkan/runtime-scene.png`
  (split), with `editor-ui.png` shared. The plan predicted wrong; check against the
  inversion.
- **Checklist A, bullet 2** says to break "the mesh entity in `test.arcscene`". **That file
  is not in this repository** — `test.arcscene` belongs to the Gacha/Aphelyon game project.
  The reference scene here is `ReferenceProject/Content/scenes/main.arcscene`, and the mesh
  entity is **`MeshCube`** (it carries the `Arcane::MeshRenderer`).

The reason for the inversion is worth keeping: the 121 "cross-backend pixels" recorded in
Plan A were **never** glyph-rasterisation variance. 120 of them are a debug HUD label whose
*content* differs (`Backend: D3D12` vs `Backend: Vulkan`, ungated at `RuntimeFrame.cpp`).
Genuine cross-backend scene difference is **~1 pixel** — this renderer is far more
backend-consistent than Plan A recorded.

## 2. The desk pass

**Run `scripts\desk-verify-servitor.ps1`.** It automates the mechanical half of Task 14 —
the gate runs, the deliberate break, the restore, and the timed bless round-trip — and stops
at each point that genuinely needs your eyes, marked `>> YOUR CALL:`. It applies §1's three
corrections itself, so you are not checking against stale expectations.

```
cd D:\dev\starworks\Arcane
scripts\desk-verify-servitor.ps1                      # phases A, B, C
scripts\desk-verify-servitor.ps1 -Phase A             # one phase at a time
scripts\desk-verify-servitor.ps1 -Configuration Release
```

Two safety properties it holds, deliberately: **it never blesses `editor-ui`** (owed defect
2 — its input is written by the thing under test, so a bless manufactures a pass and destroys
the evidence), and **it never leaves the tree dirty** — every mutation is inside `try/finally`,
restored with `git checkout --`, and it asserts `ReferenceProject/` is clean before it exits.
Ctrl-C is safe.

Budget 20–40 minutes: each gate invocation does an unconditional `/t:Rebuild` of
ReferenceProject (the single-slot `Binaries/` precondition) plus four host launches, and the
run makes several. Do not walk away during the phase B timing step — observing it *is* the
test.

The underlying checklist is sections A–F of Plan B's Task 14. Its shape:

- **A — the gate does what it claims.** Including: deliberately break something visible,
  re-run, and confirm it fails with a legible diff. *A gate never observed failing is not
  a gate.*
- **B — blessing is cheap enough that nobody disables the gate.** Time it. The spec's own
  warning is that a gate nobody can cheaply bless gets switched off in the first week, and
  the Unreal research (`docs/research/2026-08-26-unreal-screenshot-comparison-research.md`)
  found the same failure mode independently.
- **C — the layout seed** shows the seeded layout, and `git status ReferenceProject/` is
  clean after an editor session.
- **D — driver reproduction** (carried from Plan A, still owed). A negative result closes
  it validly.
- **E — cross-format parity**, optional; deliberately not automated, because automating it
  would put windowed runs back on the box carrying the driver hazard to re-assert something
  already answered structurally.

Section F of that plan names what the checklist **cannot** close: `FastStats` memory at 4K
is arithmetic rather than a measured run; the Linux `IsIdle()` stub still returns `true`
unconditionally; mesh picking is still unimplemented.

## 3. Four owed engine defects — none fixed, all with named mechanisms

These were found *by the gate on its first real use*, which is the gate working. They are
owed to a future plan.

1. **`--settle` counts ATTEMPTS while the condition it waits on is denominated in
   MILLISECONDS.** It waits on shader-compiler idle; at ~3.3 ms/attempt the whole 30-attempt
   budget elapses in ~100 ms, so a fast enough build bails before compilation drains.
   Confirmed at code level: no sleep, no wait, no pump between attempts, and no vsync under
   `--offscreen` (`ArcaneRuntime/src/RuntimeFrame.cpp` settle loop; `EditorAppFrame.cpp` is
   a line-for-line port). **This is a mode-wide design flaw, not an editor bug** — the
   runtime simply has not crossed the line yet. It makes the gate quietly timing-dependent
   on build configuration, which is why the Release editor lane fails.
   **Raising the attempt count treats the symptom.** Unreal independently corroborates the
   fix shape: Epic requires `delay` **and** `frame_delay` — a time bound *and* a frame
   bound. The fix is a **conjunction**, not a bigger number.
2. **The editor's verify capture depends on the machine's failure history.** Its Assets
   panel enumerates `Saved/Diagnostics/` — a directory the editor itself writes crash and
   hang captures into — so the golden image moves when the machine has crashed. Manifests
   as exactly 24 anti-aliased pixels: the bottom cap of the Assets scrollbar thumb, at
   byte-identical coordinates on both backends. Fix: keep the verify capture off
   `Saved/Diagnostics/`.
   **Never re-bless `editor-ui` to make this go away.** The moving input is a directory the
   editor writes into, so re-blessing goes green today and red at the next crashed, hung or
   killed editor run. It is a fixture-determinism defect, not a stale image.
3. **`settleAttemptsUsed` is absent from `VerifyReport` entirely** — convergence counts
   exist only in logs. Wants its own task with a `schemaVersion` bump to 3, and it **must**
   reconcile the two hosts' counter semantics in the same commit (the runtime leaves the
   counter at 0 in its defensive branch; the editor forces it to the budget) or the new
   field will lie on the editor.
4. **`--fixed-dt nan`** (`ArcaneClient/src/Arcane/Host/HostConfig.cpp`, the `<= 0.0` guard)
   — the same undefined-behaviour class as the `--max-diff-pixel-ratio` bug fixed in
   `92792847`. One line. First thing to fix on the next touch of that function.

### The CI decision these produced

The two editor lanes ship **advisory**, via `-AdvisoryLanes 'ArcaneEditor'` in
`scripts/golden-gate.ps1`: the lanes still run, their diffs are still archived, both owed
defects print on every run, and only those lanes are excluded from the failure tally.
Runtime lanes stay hard-gating.

Decided this way because the Golden stage runs *before* two pre-existing Jenkins stages, so
a hard-red gate aborts `stage('Windows')` and `ReferenceProject (SDK build)` and
`Scripted GPU-verify` never run — red would delete two working stages on every build. It
also defeats that stage's own "Release then Debug is load-bearing" comment, since Release
is the failing lane and the Debug pass that restores the single-slot `Binaries/` never
executes.

**The cost of this decision, stated plainly: an advisory lane is one people stop reading.**
The switch itself is the tracking artifact; when defects 1 and 2 are fixed, delete it. It is
reversible in one flag.

## 3a. The bless workflow trap — measured 2026-08-26, document it wherever blessing is described

**`--bless` writes to the level the reference RESOLVED FROM, and that is not the repo unless
you make it be.**

The hosts run from their own output directory, and `--project ReferenceProject` there resolves
to the **staged** tree that premake's host postbuild copies beside each exe
(`premake5.lua:355`, `:430`). So the natural move — copy `golden-gate.ps1`'s invocation, add
`--bless` — blesses a build artifact:

- measured: the staged PNG's mtime moved, the committed reference's did not, and `git status`
  stayed clean. Nothing to commit, and no sign anything went wrong.
- worse, it is silently lossy: the next host build's `{COPYDIR}` overwrites the staged file
  from the repo, discarding the bless.

**Point `--project` at the source tree instead** — verified to write the file you actually
commit:

```
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneRuntime
.\ArcaneRuntime.exe --project D:\dev\starworks\Arcane\ReferenceProject `
    --offscreen --backend dx12 --frames 60 --settle 30 `
    --report ReferenceProject\Saved\Verify\bless.json `
    --compare runtime-scene --bless
```

`--settle` also **requires** `--screenshot` or `--report`; omit both and the host refuses with
exit 2 and says so. That refusal is correct behaviour, not a bug.

This is a workflow trap rather than an engine defect, but it lands squarely on the question
Task 14 section B exists to answer — *is blessing cheap enough that nobody disables the gate?*
A bless that appears to succeed, cannot be committed, and evaporates on the next build is not
cheap.

**An open design question, deliberately not decided here:** `golden-gate.ps1` restages
`Content/` but not `Verify/`, so that it cannot trample a bless. If blessing is always
supposed to target the source tree, then restaging `Verify/` too would be the more coherent
model — the repo becomes the single source of truth and the staged tree a pure build
artifact — and it would make the trap above fail loudly and immediately instead of silently
and later. That is a judgment call for the engine's owner.

## 4. Parked findings

Five minors were introduced by the final fix wave and parked rather than fixed, because the
method allows one fix wave and no more. In priority order:

1. **`ArcaneTests/src/HostConfigTest.cpp` (~`:625`)** — a case *named* "still reports the
   `--compare` mistake" that asserts only refusal and exit 2, so it passes under either
   ordering. Its body discloses the limit; its title does not. This is the arc's own
   signature defect class — *would this test fail if the thing it names were wrong?* —
   appearing inside the fix for it. **Fix this one first.**
2. Nothing wipes stale diff PNGs before a gate run (golden-gate deletes the stale *report*
   but not the stale *diff*), so under `always`-archiving a green build can archive an older
   build's diff image.
3. `ImageCompare.cpp` documents a precondition its `ARCANE_API` caller owns, but
   `ImageCompare.hpp` never states it — the owner cannot read it where they look.
4. A retraction block in the Plan A spec cites a line number its own insertion moved.
5. `Write-OwedDefects` hardcodes editor-specific text regardless of the lane named —
   cosmetic while `ArcaneEditor` is the only advisory lane, wrong the moment a second
   is added.

Twenty further minors were triaged "ship" during the final review. Two batching notes worth
keeping: fold the two `CompareBackendName` copies into one exported helper with an
exhaustive `switch` on the next host-tier touch, and batch the doc-consistency minors into
a single pass.

## 5. What the arc proved, in one paragraph

The comparator is a port of Playwright's image comparison, validated against **Playwright's
own fixture corpus with zero divergence** across 21 pairs, both SSIM traps caught. Bit-parity
is a requirement, not an aspiration, and the corpus is the oracle that enforces it: partial
sums are `double` not `uint64_t`, the variance window radius is 1, `BlendWithWhite` truncates
rather than rounds, dE94 is asymmetric. **Exactly one knowing deviation from upstream exists
— the wait loop** (Playwright checks its goal only on iteration 1 then exits on stability; we
re-check every attempt). The final whole-branch review confirmed no second deviation exists
anywhere in the branch. If the conformance corpus ever fails, the port diverged: bisect
against the porting-traps table in Plan B's Global Constraints. **Do not loosen a constant.**
