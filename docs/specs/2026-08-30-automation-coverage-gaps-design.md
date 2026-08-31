# Closing the automation coverage gaps

**Status:** design, approved in brainstorming 2026-08-30. Follows arc 2
(`docs/specs/2026-08-28-servitor-owed-defects-design.md`), which closed five owed defects and
deleted `-AdvisoryLanes`, leaving all four golden lanes hard-gating.

**The single input this spec was written from** is the coverage audit run at the close of arc 2:
what each CI lane actually executes, checked against the pipelines rather than recalled.

---

## Why

Arc 2 ended with four hard-gating lanes and a green desk run. That is not the same as a full
automation suite, and the audit found four gaps. Three are closed here. The fourth is named and
deliberately deferred.

Two of the three have a concrete failure mode already waiting:

- **The `editor-ui` reference was blessed on one machine, and asset order is not machine-stable.**
  `AssetRegistry::All()` (`AssetRegistry.hpp:96`) materialises its vector from
  `std::unordered_map<Guid, std::string> m_byGuid` (`:102`). Unordered-map iteration order depends
  on hash values and insertion history, so two trees holding identical files can enumerate them
  differently. Nothing downstream sorts — not `All()`, not the Assets panel. The next Jenkins run
  is **the first four-lane hard-gated run in the pipeline's history**, and if the agent is not the
  machine the reference was blessed on, `editor-ui` fails for a reason that has nothing to do with
  rendering.
- **Dist is built by no CI lane.** The standing rule is *0 warnings across Debug/Release/Dist*;
  GitHub Actions builds Debug and Release, Jenkins builds Debug and Release. Dist — the shipping
  configuration, and the one with `ARCANE_DIST` compiling code **out** — is verified only by
  whoever last built it by hand.

The third gap is that **no automated job has ever proven the gate is capable of failing.** A gate
never observed failing is not a gate; that property currently lives only in a desk script a human
must remember to run.

---

## Conventions this design was checked against

**This repo's own nondeterminism policy, and it is the binding one.**
`docs/research/2026-08-26-unreal-screenshot-comparison-research.md` records the philosophical
split with Unreal: Epic makes flakiness *declarable*, while

> the settle loop exists so that a test is either deterministic or it is a bug, and there is no
> third state.

Under that policy, machine-dependent asset ordering is **a bug, not a tolerable flake**, and the
correct response is to remove the nondeterminism rather than budget for it. That decides change 1
on established grounds rather than taste. The same passage records the cost of the position — it
is "unfalsifiable in the direction that matters" — which is the argument for change 3.

**Document layout.** Specs live at `docs/specs/YYYY-MM-DD-<topic>-design.md` and plans at
`docs/plans/YYYY-MM-DD-<topic>.md`. This is the repo's established practice (arc 1, arc 2, the
package-tiering design); it overrides the `docs/superpowers/specs/` default the brainstorming
skill suggests.

**Ordinal, not locale-aware, comparison.** `std::string::operator<` compares through
`char_traits::compare`, which is byte-wise and locale-independent. This matters: a locale-aware
sort would reintroduce exactly the class of machine-dependence this change removes. The C++
default is correct here; the equivalent PowerShell or .NET sort would not be.

---

## Decisions

### 1. Sort inside `AssetRegistry::All()`, by mount path, Guid as tiebreak

One function, one existing consumer (`EditorAppProject.cpp:302`). Sorting at the accessor rather
than at the panel means every future consumer inherits determinism instead of having to remember
it.

Sorting by **mount path** rather than Guid gives the Assets panel a stable alphabetical order,
which is also what a reader wants; Guid breaks ties so the order is total even if two entries ever
share a path.

**Consequence, stated rather than discovered later: this changes the `editor-ui` golden image.**
Row order is part of the picture. The reference must be re-blessed and restaged, and that is a
desk step requiring a GPU. Sort, re-bless and restage therefore move **together** — landing the
sort on `main` without the re-bless turns the gate red for everyone.

### 2. Dist joins the GitHub Actions lane, built and tested

`ci.yml` gains `Build Dist` and `Test Dist (~[gpu])`, mirroring the Debug and Release pairs it
already carries. GitHub Actions rather than Jenkins because it runs on **every push**, so a
warning or a Dist-only compile break is caught on the branch instead of after merge — and because
the job needs no GPU, so it has no business consuming the single self-hosted GPU agent.

Running the **suite** and not merely the build is deliberate: `ARCANE_DIST` compiles code out, so
the Dist binary is genuinely different. Its expected figure is **52228 assertions / 1270 cases**
against Debug's 52296 / 1276 — the gap is the measure of how different.

### 3. `golden-gate.ps1` gains `-SelfTest`, and Jenkins runs it on `main`

With `-SelfTest`, the gate mutates `ReferenceProject/Content/scenes/main.arcscene` (MeshCube
position x, `0.4 -> 0.6` — the same mutation `desk-verify-golden-gate.ps1` uses, so there is one
mutation vocabulary and not two), runs the four lanes, and asserts **all four go FAIL**. The assertion is on the
per-lane `verdict`, not on any particular `exitReason` — a lane that fails by erroring instead of
by comparing is still a lane that noticed. It exits non-zero if any lane stayed green.

**All four, not the runtime pair alone.** The desk script asserts only the runtime lanes because
the editor lanes were advisory when it was written. They hard-gate now, and the desk run of
2026-08-30 measured the editor lanes at 23824 differing pixels under this same mutation, so they
demonstrably respond to it.

**The discipline this must not break.** `golden-gate.ps1` carries an explicit *"this script only
ever CHECKS"* rule at its lane invocation. `-SelfTest` mutates, so the header must say out loud
what bounds the mutation: it touches `Content/` only, never `Verify/`, never blesses, and always
restores. Restoration goes in `try/finally` so an error or a Ctrl-C still restores, and the mode
asserts `ReferenceProject/` is clean before it exits — the same contract the desk script already
upholds.

**Cadence: `main` and `milestone/*` only.** The property under test — *this gate is capable of
failing* — belongs to the **gate**, which changes rarely. It does not need per-branch re-proof,
and the self-test runs the full four-lane gate a second time, roughly doubling its wall clock on
the one machine that runs everything else.

---

## Sequencing

The order is load-bearing, and there is a **user desk checkpoint in the middle**, as in arc 2:

1. Sort + its unit test.
2. **USER, at the desk:** re-bless `editor-ui`, restage to both hosts, confirm four lanes green.
3. Dist in GitHub Actions.
4. `-SelfTest`.
5. The Jenkins stage that calls it.

Step 2 gates steps 4 and 5 absolutely. Enabling the self-test stage before the reference is
re-blessed means its first CI run compares against a stale reference and reports a failure that
proves nothing about the gate.

---

## Verification

- **Change 1** — a unit test asserting `All()` returns entries in sorted order. Sortedness over a
  **total** key (path, then Guid) *is* the determinism property: it makes the sequence a function
  of the content alone, independent of hash or insertion order, so no second "different insertion
  order" test is needed. What the test does need is a fixture large enough that an **unsorted**
  implementation would not pass by luck — a handful of entries whose natural map order differs
  from alphabetical. Confirm it fails with the sort removed.
- **Change 2** — the Dist steps appear in the GH Actions run and report `52228 assertions in 1270
  test cases`, with 0 warnings.
- **Change 3** — the mode must be **observed failing correctly**: run it against an unmutated tree
  and confirm it reports the lanes did NOT fail as expected. A self-test that cannot itself fail
  is the defect it exists to detect.
- Gate baseline entering this work: **Debug/Release 52296 assertions / 1276 cases; Dist 52228 /
  1270**, 0 warnings in all three.
- `--bless` writes to the level the reference resolved from, and **golden-gate.ps1 deliberately
  does not restage `Verify/`** — a bless must be copied to both hosts by hand or by
  `desk-verify-golden-gate.ps1`.

---

## Non-goals

- **Host-level witnesses.** `settleCaptureFailed` ever being true, the
  `compare-missing-reference` key absence, and a non-converging run actually spending
  `--settle-timeout` still have no automated proof. `ArcaneTests` links neither host, so closing
  these needs a harness that launches a real host and asserts on its JSON report — materially
  larger than this spec, and deliberately separate.
- **Finding Q4-F1**, declarable exclusion with a mandatory reason, reported as Skipped. Arc 2
  deleted `-AdvisoryLanes` and replaced it with nothing; this spec does not replace it either. The
  gap stays open and named.
- **`Binaries/` staging.** It is additive like `Content/` was, but it mirrors a postbuild, is
  rebuilt every run, and already asserts on the DLL it needs. Left alone.
- **The `/ZI` Debug line-number defect.** Catch2 assertion lines are wrong in Debug builds. Real,
  reported, and out of scope — the fix removes Edit-and-Continue for every project.
- **A Linux lane** for the engine repo.

---

## Risks

- **The re-bless is a second desk trip**, and the plan stalls at step 2 until it happens. Accepted
  deliberately: the alternative is leaving a known machine-dependency in a gate that is about to
  hard-gate four lanes in CI.
- **The first CI self-test run has never executed.** Like arc 2's four-lane run, its first real
  execution is in the pipeline. It is gated to `main`, so a failure there is loud and immediate.
- **Sorting is a behaviour change to an engine accessor**, not only to the editor. One consumer
  today; any future consumer inherits an order it may not expect to be alphabetical.
