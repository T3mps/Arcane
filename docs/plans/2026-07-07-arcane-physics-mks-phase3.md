# Arcane MKS Units — Phase 3 (Sleep/Settle/Island Cluster Conversion) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the sleep/settle/island test cluster (7 files, 145 PX-PINs, repo total 635 -> 490) to MKS content, and close the P2 carry-forward by re-deriving the two loosest warm-start drift bounds in PhysicsSolverTest from fresh MKS measurements.

**Architecture:** Spec §4 Phase 3 (`docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md`). THE acceptance risk this phase was chartered to carry — "do piles sleep at 0.05 m/s?" — was RETIRED by the 2026-07-03 probe battery (Appendix A: pile fully asleep by step 338, zero re-wake; 8-box stack by step 57), so this is a mechanical-with-judgment conversion, not an investigation. One branch, seven tasks: the sleep-boundary idiom-setter first (SleepThreshold), then the four heavier files, then the three small island files as one task, then the drift-bound revisit, then controller-run exit. Survey fact base: `.superpowers/sdd/p3-sleep-cluster-survey.md` (full-file reads, all line numbers and literals below are survey-verified).

**Tech Stack:** C++23 (Arcane test suite only — zero engine/Core edits this phase), MSVC via msbuild, Catch2. Parity source: vendored `ThirdParty/box2d-3.1.1` — any behavioral question is checked against it directly, never recalled.

## Global Constraints

- **Conversion protocol:** `.superpowers/sdd/p2-conversion-protocol.md` applies verbatim (PX-PIN deletion rule, MKS WorldDef defaults, Rule-3 impulses as mass x delta-v, empirical re-baseline rule ~1.5x headroom + named driving constant, ASCII-only comments, explicit-path staging, FOREGROUND-only one-command builds/tests, commit trailers). Where this plan and the protocol overlap, this plan's exact values win. Protocol overrides for P3: branch is `feature/arcane-physics-mks-phase3` (off `main` @9043fb9a or later — P2+P6 are merged); `[physics]` reconciliation base = 30636 assertions / 277 cases (re-record at Task 1 Step 1).
- **Spec §6 parity contingency (binding):** behavioral asserts (sleep flags, awake counts, island roots/membership, wake propagation, run-twice identity, validator calls) must pass UNMODIFIED. If a converted pile/stack does NOT settle and sleep at the 0.05 default, that is a parity BUG — STOP, diff the scenario against vendored Box2D v3.1.1, report BLOCKED. Do NOT bump any threshold and do NOT touch engine constants; that was the px-era hack this whole workstream exists to kill.
- **Engine/Core untouched.** This phase edits ONLY `Arcane/Tests/src/*.cpp` (8 files total). The MKS-DEFER burn-down (6 Core sites) belongs to P4/P5 — `grep -rn "MKS-DEFER" Arcane/Core/src` must list exactly the same 6 sites before and after this branch.
- **Cross-cutting conversion rules (survey-derived, apply in every task):**
  1. Gravity is NOT divided: every px-era `gravityY` literal (400 or 900) becomes `Real(10)` — or the line is deleted where the default suffices; zero-g (`gravityY = 0`) stays untouched as a deliberate scene statement.
  2. Lengths/positions divide by the file's chosen family (/10 or /100 per task recipe); direct `SetVelocity` literals divide with them (no mass involved).
  3. `ApplyImpulse` literals NEVER divide — mass = density x area shrinks with length squared, so every impulse rewrites as mass x targetDv per protocol rule 3. Precedent: `PhysicsPersistentIslandTest.cpp:164-169` (`targetDv` -20 / `(2,-25)` m/s magnitudes: well above sleepThreshold 0.05, well under the 400 cap).
  4. Small deliberate "gap" literals (0.1/0.2/0.5 px seatings between stacked bodies) re-author to 0.01-0.02 m (2-4x kLinearSlop, constant named in a comment) — a raw divide lands at or below kLinearSlop (0.005), degenerate territory.
  5. Sleep-onset arithmetic: `kSleepTime = 0.5 s` (Island.hpp:101) = 30 steps at 1/60, and `Island.cpp:95` keeps bodies awake while `timer <= kSleepTime` — so **sleep can occur at step 31 at the earliest**. Loops of exactly 30 steps that need bodies "still awake" are safe by construction; do not panic-extend them.
  6. Density stays 1 everywhere; friction/restitution/angles/angular velocities are unit-free and unchanged.
- **Assertion-count discipline:** four sites have `CHECK`/`REQUIRE` inside fixed-N step loops (SleepThreshold 120, StaticSettle 120+120+200x2, AwakeSet 120, IslandWakeMerge 600). Loop counts stay unchanged unless an empirical settle check forces an extension — any change must be reconciled in the task report arithmetic (`+N = loop X extended from A to B`).
- **Suite green at every commit.** Per-task gate = file tags + `"[physics]"` from the exe dir `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`. Full `~[gpu]` only at Task 7.
- **Build:** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m` — ONE FOREGROUND command. No new .cpp files -> no premake regen.
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466
  ```

## File Structure

| File | Change | Task |
|---|---|---|
| `Arcane/Tests/src/PhysicsSleepThresholdTest.cpp` | MKS conversion, 25 pins deleted; threshold cases re-authored around the 0.05 default | 1 |
| `Arcane/Tests/src/PhysicsStaticSettleTest.cpp` | MKS conversion, 25 pins deleted; stale DragStep caps synced to shipped Interaction.hpp | 2 |
| `Arcane/Tests/src/PhysicsAwakeSetTest.cpp` | MKS conversion, 45 pins deleted; 2 Rule-3 impulses | 3 |
| `Arcane/Tests/src/PhysicsIslandTest.cpp` | MKS conversion, 30 pins deleted; 2 Rule-3 impulses; 2 non-ASCII arrows fixed | 4 |
| `Arcane/Tests/src/PhysicsIslandWakeMergeTest.cpp` | MKS conversion, 5 pins deleted (tile-span scene) | 5 |
| `Arcane/Tests/src/PhysicsJointSleepTest.cpp` | MKS conversion, 5 pins deleted (zero-g joint scenes) | 5 |
| `Arcane/Tests/src/PhysicsSensorIslandTest.cpp` | MKS conversion, 10 pins deleted | 5 |
| `Arcane/Tests/src/PhysicsSolverTest.cpp` | two 0.05 warm-start drift bounds re-derived (P2 carry-forward; :596, :726) | 6 |

---

### Task 1: Branch, baseline, and PhysicsSleepThresholdTest (the sleep-boundary idiom-setter)

**Files:**
- Modify: `Arcane/Tests/src/PhysicsSleepThresholdTest.cpp` (106 lines, 5 cases `[physics][sleep]`, 25 pins inline per case)
- Add (commit only): `docs/superpowers/plans/2026-07-07-arcane-physics-mks-phase3.md` (this plan)

**Interfaces:**
- Consumes: MKS WorldDef defaults (gravity (0,+10), sleepThreshold 0.05, restitutionThreshold 1.0, contactPushMaxVelocity 3.0, hashCellSize 1.0) — pin deletion falls back to these. Combined sleep gate `|v| + |w|*maxExtent < threshold` (PhysicsWorld.cpp:2349).
- Produces: the cluster's sleep-boundary idiom — margins against the REAL 0.05 default, stated in comments. Later tasks imitate it.

- [ ] **Step 1: Branch + baseline.** `git checkout -b feature/arcane-physics-mks-phase3` from main. Commit this plan doc first (`git add docs/superpowers/plans/2026-07-07-arcane-physics-mks-phase3.md`, subject `docs(arcane/physics): MKS Phase 3 plan (sleep/settle/island cluster)`). Build; from the exe dir run `.\ArcaneTests.exe "[physics]"` and record the exact counts (expect 30636/277 — this is the reconciliation base for every later task). Record `grep -rc "PX-PIN" Arcane/Tests/src Arcane/Sandbox/src` total (expect 635).

- [ ] **Step 2: Convert the two maxExtent accessor cases (lines 19-51).**
  - Case 1 "maxExtent equals circle radius": delete its 6 PX-PIN lines (:22-27); circle radius `Real(10)` -> `Real(0.1)` (:30); assert `WithinAbs(10.0, 1e-4)` -> `WithinAbs(0.1, 1e-4)` (:33) — analytic, recomputes from the authored radius.
  - Case 2 "maxExtent equals box half-diagonal": delete its 6 PX-PIN lines (:39-44); AABB half-extents `Real(3), Real(4)` -> **`Real(0.3), Real(0.4)`** (:47) — re-authored /10, NOT /100 (0.03/0.04 m would undershoot the 0.1 m body floor); half-diagonal assert `WithinAbs(5.0, 1e-4)` -> `WithinAbs(0.5, 1e-4)` (:51). Comment: `// 3:4:5 kept at /10 scale; /100 would undershoot the 0.1 m body-size floor (MKS P3)`.

- [ ] **Step 3: Re-author the override-plumbing case (lines 54-75).** Delete its 5 PX-PIN lines (:57-61 minus the custom-threshold line). Replace the arbitrary px-era distinguishers with values that are BOTH distinct from the engine default 0.05 (stronger test — it can now tell "world default applied" from "engine default leaked through"):
  ```cpp
  wd.sleepThreshold = Real(0.02); // custom world default, deliberately != engine default 0.05 (MKS P3)
  ...
  d.sleepThreshold = Real(0.08);  // per-body override, != world default and != engine default
  ```
  The `SleepThresholdSlot` equality asserts (:68-69) recompute from these authored constants. No `Step()` is ever called in this case — values are plumbing distinguishers, not physics.

- [ ] **Step 4: Re-author the two boundary cases around the REAL default (lines 76-106).**
  - Case 4 "slowly-rolling body sleeps": delete its 4 PX-PIN lines (:78-82) AND delete the custom `wd.sleepThreshold = Real(5)` — the case now guards the true engine default. Keep `wd.gravityY = 0` (deliberate zero-g). Circle radius `Real(10)` -> `Real(0.1)` (:85); angular velocity `Real(0.08)` (:87) UNCHANGED (rad/s, unit-free). Margin arithmetic, stated in a comment: combined = |w|*maxExtent = 0.08 * 0.1 = 0.008 m/s = 6.25x BELOW the 0.05 default — exactly the px-era margin (0.8 vs 5).
  - Case 5 "fast-spinning body never sleeps": same treatment (:95-99, delete pins + the `Real(5)` line, keep zero-g); radius -> `Real(0.1)` (:102); `Real(2.0)` spin (:104) unchanged. Margin: 2.0 * 0.1 = 0.2 m/s = 4x ABOVE 0.05 — exactly the px-era margin (20 vs 5).
  - Both loops stay 120 steps (4x kSleepTime margin; case 5's per-iteration `REQUIRE` keeps its 120-assert contribution — do not change the loop count).

- [ ] **Step 5: Build + run + triage.** Build (foreground). `.\ArcaneTests.exe "[sleep]"` green; `grep -c "PX-PIN" Arcane/Tests/src/PhysicsSleepThresholdTest.cpp` == 0; `.\ArcaneTests.exe "[physics]"` green with counts reconciled vs Step 1 (expected delta: 0/0 — no loop counts changed). A sleep-flag failure here = spec §6 contingency (STOP, parity investigation).

- [ ] **Step 6: Commit.**
  ```bash
  git add Arcane/Tests/src/PhysicsSleepThresholdTest.cpp
  git commit -m "test(arcane/physics): PhysicsSleepThresholdTest -> MKS, boundary cases guard the real 0.05 default (MKS P3)"
  ```
  Body: pin count, the margin-preservation table (6.25x under / 4x over), the case-3 re-author rationale (both values distinct from engine default), trailers.

---

### Task 2: Convert PhysicsStaticSettleTest (+ sync the stale DragStep caps)

**Files:**
- Modify: `Arcane/Tests/src/PhysicsStaticSettleTest.cpp` (380 lines, 5 cases, tag `[physics]` only — no finer tag; 25 pins inline, 5 per case)

**Interfaces:**
- Consumes: shipped P6 interaction constants — `Interaction.hpp:109 kDragMaxSpeed = 40.0f`, `:121 kDragMaxAccel = 400.0f`, `:129 kDragMaxAngVel = 8.0f` (verify by reading the header, copy verbatim).
- Produces: capsule/box settle-and-sleep coverage at MKS; the file's local `DragStep` test double back in sync with production.

- [ ] **Step 1: Convert scene constants + WorldDefs (/100 family — this file's content came from the sandbox Mixed-shapes scene).**
  - Delete all 25 PX-PIN lines (5 per case).
  - `kGravityY = Real(900)` (:111) -> `Real(10)` (gravity rule, NOT /100).
  - `kCapRadius 14 -> 0.14`, `kCapHalf 24 -> 0.24` (:112-113); floor `kFloorHalfW 560 -> 5.6`, `kFloorHalfH 20 -> 0.2` (:117-119, `kFloorTopY = 0` unchanged).
  - Spawns: `Vec2(0,-60)` -> `Vec2(0,-0.6)` (:174, :227); `Vec2(0,-kCapRadius)` auto-converts (:269, :315). Box case: `hw=hh=Real(20)` -> `Real(0.2)`, spawn `Vec2(0,-hh)` auto (:360-361).
  - Tilt angle `Real(0.25)` rad (:228) unchanged.
- [ ] **Step 2: Sync `DragStep` (lines 40-104) to production.** Replace the three stale caps (:43-45): `kDragMaxSpeed 4000 -> 40.0`, `kDragMaxAccel 40000 -> 400.0`, `kDragMaxAngVel 8 -> 8.0` (unchanged value, rad/s) — **copied verbatim from `Interaction.hpp:109,121,129`, cite the header in the comment; do not derive.** Fix the stale ":39 The caps match Interaction.hpp" comment to name the P6 retune. Grab anchor `Vec2(Real(20), Real(0))` -> `Vec2(Real(0.2), Real(0))` (:321); cursor target `Vec2(Real(0), Real(200))` -> `Vec2(Real(0), Real(2.0))` (:322).
- [ ] **Step 3: Convert the forced-push velocity.** `SetVelocity(..., Real(600))` -> `Real(6.0)` (:283, :371) — direct velocity, /100 divides cleanly (old push-to-gravity-dv ratio 40x, new 36x — same regime; note it in the commit body).
- [ ] **Step 4: Re-derive the numeric budgets empirically (protocol rule 6 — run, record, 1.5x headroom, name the driving constant).** Sites:
  - `spd < Real(1.0)` (:192, :247) — expect sleepThreshold-scale; the body has SLEPT by assert time (CHECK_FALSE IsAwake at :204/:250), so measured is likely ~0 -> bound at `Real(0.05)` justified "must have crossed below the sleepThreshold default to sleep".
  - `avel < Real(0.05)` (:193, :248) — rad/s (unit-free) but the settled residual changes with scale; re-measure. Flag from survey: the old literal coincidentally equals the new threshold — the new comment must name what actually drives the chosen bound.
  - `penetration < Real(0.25)` (:198, :249) — solver residual; expect kLinearSlop-scale (P2 precedent: single-body resting contact measures ~0.0003, bounded at 0.001).
  - `penetration > Real(-2.0)` (:201) — "not floating above the floor"; re-measure, expect kSkin-scale negative bound.
  - Forced-push `penetration < Real(2.0)` (:291, :377) and drag `penetration < Real(6.0)` (:337) — re-measure both; these guard non-tunneling under sustained push/drag, the bound is a fraction of the (now 0.2 m) floor half-height, name it.
  - `p.y < kFloorTopY + kFloorHalfH * Real(2)` (:292, :378) and `pEnd.y < kFloorTopY` (:342) — analytic, recompute from the converted constants; asserts unmodified in form.
- [ ] **Step 5: Build + run + triage.** `.\ArcaneTests.exe "[physics]"` green (this file has no finer tag); PX-PIN grep == 0; counts reconciled (expected 0/0 — the 120/120/200-iteration assert loops keep their counts; settle windows re-verified per protocol rule 5: drop is 0.46 m -> ~18 steps vs 180/240-step budgets, not tight). `CHECK_FALSE(IsAwake)` failures = spec §6 contingency.
- [ ] **Step 6: Commit.**
  ```bash
  git add Arcane/Tests/src/PhysicsStaticSettleTest.cpp
  git commit -m "test(arcane/physics): PhysicsStaticSettleTest -> MKS + DragStep caps synced to shipped Interaction.hpp (MKS P3)"
  ```
  Body: justification table for every re-derived budget (measured -> bound -> driving constant), the DragStep sync note with header cites.

---

### Task 3: Convert PhysicsAwakeSetTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsAwakeSetTest.cpp` (266 lines, 9 cases `[physics][awakeset]`, 45 pins inline — 5 per case)

**Interfaces:**
- Consumes: Rule-3 impulse idiom; Task 1's margin-comment idiom.
- Produces: awake-set membership/determinism coverage at MKS. Case 9 lands on the SAME /100 values as the P6-converted sandbox Playground scene (it is that scene's regression twin).

- [ ] **Step 1: Convert cases 1-8 (/10 family — matches the converted sibling PhysicsPersistentIslandTest).**
  - Delete all 45 pins. Gravity: `gravityY = 400` (cases 1,2,3,4,5,7,8) -> `Real(10)`; case 6's `gravityY = 0` stays (zero-g deliberate).
  - Floors: `AddFloor((0,5),200,5)` -> `((0,0.5),20,0.5)` (floor top stays y=0); case 5's wider `400` -> `40`.
  - Boxes: half-extents `5 -> 0.5`, `4 -> 0.4`; columns `y = -10 - 9*i` -> `y = -1.0 - 0.9*i` (cases 2, 8).
  - Case 3: boxes `(0,-20)/(0,-40)/(0,-60)` -> `(0,-2)/(0,-4)/(0,-6)`; **`ApplyImpulse(b1, Vec2(0,-8000))` (:94) rewrites Rule 3:**
    ```cpp
    // Authored as mass x delta-v (rule 3): box mass = density * 4*hw*hh
    const Real boxMass = Real(1) * Real(4) * Real(0.5) * Real(0.5); // = 1.0 kg
    const Real targetDv = Real(-20); // m/s: well above sleepThreshold 0.05, well under the 400 cap
    w.ApplyImpulse(b1, Vec2(Real(0), boxMass * targetDv));
    ```
  - Case 5 (45-box pile): `hw=hh=4 -> 0.4`; spacing gaps `0.2` (:145-146) re-author to **`Real(0.02)`** (= kSkin, 4x kLinearSlop — comment names it; a raw /10 would coincide, state that it is chosen, not divided). 300-step budget: re-verify empirically that the pile fully drains the awake-set (probe precedent: an 8-body pile sleeps by ~step 338 from ~2x these heights — if 300 is short, extend with justification and reconcile the count delta; no per-iteration asserts in this loop).
  - Case 6: box `(0,0),0.5,0.5`; `SetVelocity(b,(50,0))` -> `(5.0, 0)` m/s (direct velocity, /10). FIX the stale comment ":157 far above the sleep threshold (|v| < 2.0)" — the 2.0 never matched any set constant; rewrite naming the real gate (5.0 m/s = 100x the 0.05 default).
  - Case 7: kinematic `(150,-50)` -> `(15,-5)`; dynamic `(0,-20)` -> `(0,-2)`, halves 0.5.
  - Case 8: recycle box `(30,-10),4,4` -> `(3,-1),0.4,0.4`; **`ApplyImpulse(boxes[5], Vec2(120,-3000))` (:211) rewrites Rule 3 on BOTH components:** `boxMass = 1*4*0.4*0.4 = 0.64 kg`, `targetDv = Vec2(2,-25)` (precedent magnitudes) -> `ApplyImpulse(boxes[5], boxMass * Vec2(Real(2), Real(-25)))`.
- [ ] **Step 2: Convert case 9 (/100 — the sandbox scene-0 twin; these values MUST mirror the P6-converted Playground).** `gravityY 900 -> 10`. Statics: `(640,820,760,36)` -> `(6.4,8.2,7.6,0.36)`, `(-80,560,36,300)` -> `(-0.8,5.6,0.36,3.0)`, `(1360,560,36,300)` -> `(13.6,5.6,0.36,3.0)`. Dynamic boxes: `(440,120,54,54)` -> `(4.4,1.2,0.54,0.54)`, `(640,40,66,42)` -> `(6.4,0.4,0.66,0.42)`, `(860,90,46,46)` -> `(8.6,0.9,0.46,0.46)`. Circles: `(540,260,r=50)` -> `(5.4,2.6,r=0.50)`, `(760,200,r=58)` -> `(7.6,2.0,r=0.58)`. Comment: values mirror `Scenes.cpp` Playground (P6). 600-step budget generous.
- [ ] **Step 3: Build + run + triage.** `.\ArcaneTests.exe "[awakeset]"` green; PX-PIN grep == 0; `"[physics]"` reconciled (expected 0/0 unless case 5's loop extended — case 6's 120-loop count unchanged). Behavioral gates (run-twice identity :58-59/:219-220, `checkInvariant`, `AwakeBodies().empty()` :152/:265, frozen-lerp exact-equality :121/:124) unmodified — failures = spec §6 contingency.
- [ ] **Step 4: Commit.**
  ```bash
  git add Arcane/Tests/src/PhysicsAwakeSetTest.cpp
  git commit -m "test(arcane/physics): PhysicsAwakeSetTest -> MKS content, pins deleted (MKS P3)"
  ```
  Body: /10 + /100(case 9) mapping table, both Rule-3 rewrites, the gap re-author, the case-6 comment fix, any step-count delta arithmetic.

---

### Task 4: Convert PhysicsIslandTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsIslandTest.cpp` (407 lines, 6 cases `[physics][island]`, 30 pins inline — 5 per case; 2 non-ASCII arrows at :309, :340)

**Interfaces:**
- Consumes: Rule-3 idiom; gap re-author rule; the step-31 sleep-onset floor (Global Constraints rule 5).
- Produces: island sleep/wake/root coverage at MKS.

- [ ] **Step 1: Convert content (/10 family).**
  - Delete all 30 pins; `gravityY = 400` -> `Real(10)` in all 6 cases (:88, :128, :162, :212, :295, :366 — no zero-g case here).
  - Floors: `((0,5),200,5)` -> `((0,0.5),20,0.5)` (cases 1,2,4,6); case 3's `400` -> `40`.
  - Circles (helper default `r=Real(10)` :53-63): `-> Real(1.0)`; balls at `(0,-50)` -> `(0,-5)` (cases 1,2,3).
  - Boxes: `hw=hh=4 -> 0.4` (cases 4,6), `5 -> 0.5` (case 5).
  - Case 2 **`ApplyImpulse(ball,(0,-6000))` (:148) rewrites Rule 3:** `const Real ballMass = Real(1) * kPi * Real(1.0) * Real(1.0);` (density 1, r=1.0 — if the file has no kPi local, compute from the shape helper's constants; do NOT collide with `Arcane::Physics::kPi`, follow the P2 T3 precedent of a file-local alias check), `targetDv = Real(-20)` -> `ApplyImpulse(ball, Vec2(Real(0), ballMass * targetDv))`.
  - Case 3: pusher kinematic `r 10 -> 1.0` at `(-80,-10)` -> `(-8,-1)`; `SetVelocity(pusher,(120,0))` -> `(12, 0)` m/s — time-to-contact preserved exactly (old 60 px gap at 120 px/s = new 6 m at 12 m/s = 0.5 s = 30 steps, inside the 120-step wake window).
  - Case 4: stack `y = -(2*hh + 0.1)*(i+1)` — gap `0.1` re-authors to **`Real(0.01)`** (2x kLinearSlop, comment names it). **`ApplyImpulse(boxes[N-1],(0,-8000))` (:267) rewrites Rule 3:** `boxMass = 1*4*0.4*0.4 = 0.64`, `targetDv = Real(-20)`.
  - Case 5 (4-box chain + far body): gap `Real(0.5)` (:317) re-authors to **`Real(0.02)`** (= kSkin, comment); far box `(500,-hh)` -> `(50,-0.5)`. The 30-step loop stays 30: sleep is impossible before step 31 (rule 5), and contact lands by ~step 4 (fall 0.02 m under g=10 ~ 0.063 s) — empirically confirm all four boxes are awake and in mutual contact at loop end (the case's own asserts prove it).
  - Case 6: same stack recipe as case 4 (gap `0.01`).
- [ ] **Step 2: ASCII drive-by.** Replace the two non-ASCII arrows in comments: `:309` (`<->` for the U+2194s) and `:340` (`->` for the U+2192s). Verify zero non-ASCII bytes on the touched lines afterward.
- [ ] **Step 3: Build + run + triage.** `.\ArcaneTests.exe "[island]"` green (shared tag — PersistentIsland/BodyContacts/WakeMerge/SensorIsland ride along, all must stay green); PX-PIN grep == 0; `"[physics]"` reconciled (expected 0/0 — no per-iteration assert loops in this file). Behavioral gates (IsAwake/IsAwake-false families, frozen exact-equality :116-117/:255-256, IslandRootOf equalities :348-354, run-twice case 6) unmodified.
- [ ] **Step 4: Commit.**
  ```bash
  git add Arcane/Tests/src/PhysicsIslandTest.cpp
  git commit -m "test(arcane/physics): PhysicsIslandTest -> MKS content, pins deleted (MKS P3)"
  ```
  Body: mapping table, both Rule-3 rewrites, both gap re-authors with constants named, the ASCII fix note.

---

### Task 5: Convert the three small island files (one commit)

**Files:**
- Modify: `Arcane/Tests/src/PhysicsIslandWakeMergeTest.cpp` (215 lines, 1 case `[physics][island]`, 5 pins in one block :174-178)
- Modify: `Arcane/Tests/src/PhysicsJointSleepTest.cpp` (186 lines, 3 cases `[physics][joint]`, 5 pins centralized in `MakeQuietWorld()` :50-54)
- Modify: `Arcane/Tests/src/PhysicsSensorIslandTest.cpp` (134 lines, 2 cases `[physics][island]`, 10 pins inline)

**Interfaces:**
- Consumes: gap re-author rule; Rule-3 idiom; the tileCellSize-vs-hashCellSize distinction (below).
- Produces: wake-merge/joint-island/sensor-island coverage at MKS; cluster PX-PIN count reaches 0.

- [ ] **Step 1: Convert PhysicsIslandWakeMergeTest (/100 — tile-span rain scene).**
  - Delete the 5-pin block (:174-178); `gravityY = 400` (:166) -> `Real(10)`. `sleepThreshold` stays UNSET — the ":173 Do NOT set sleepThreshold to 0" precondition is now satisfied by the inherited 0.05 default (note it in the comment).
  - **Do not conflate the two cell sizes:** the pinned `hashCellSize = 64` is a plain deletion (inherits 1.0); `wd.tileCellSize = kSpanCellSize` is SCENE content — `kSpanCellSize = Real(20)` (:51) -> `Real(0.2)` (/100). Grid row/col indices (`kSpanGridW/H = 64`, `kSpanFloorRow 40/43`, `kSpanWallRowTop 24`) are counts — unchanged; `kSpanFloorTop = kSpanFloorRow * kSpanCellSize` recomputes to 8.0 m analytically. `tileOrigin (0,0)` unchanged. (Deliberate divergence from P2-T8's tile-span precedent, which re-authored its cell to the 1-m residency scale: THIS scene is a 64x64-cell bowl whose whole geometry hangs off the cell size — /100 preserves the proven 140-body scene shape exactly; document in the commit body.)
  - `BuildRain` spawn bounds (:96-97): x `rnd(120,1160)` -> `rnd(1.2,11.6)`, y `rnd(520,720)` -> `rnd(5.2,7.2)`. Shape dims (:101,:105,:110): circle `rnd(6,11)` -> `rnd(0.06,0.11)`, AABB `(8,8)` -> `(0.08,0.08)`, capsule `MakeCapsule(10,5)` -> `(0.10,0.05)` (halfLen, r — Shapes.hpp:155 order). These land below the 0.1 m body-size floor — ACCEPTED EXCEPTION, documented in a comment: all dims are still 2.5-5.5x kSkin (not degenerate), and preserving the body:tile ratio is what the wake-merge scenario needs. LCG seed/algorithm untouched.
  - Containment slack `Position(h).y < kSpanFloorTop + Real(80)` (:211): slack -> re-measure; expect `Real(0.8)` (/100) — verify empirically per rule 6 (it guards tunneling).
  - 600-step loop with per-iteration `REQUIRE(IslandsUniformlyAwake(...))` (:191) stays 600 (= 600 asserts, the cluster's largest single count contributor — do not change without reconciling). Empirically confirm `sawAnySleep` (:202) still fires and the pile settles within the window (drops are 0.8-2.8 m ~ 24-45 steps; the uncertainty is 140-body settle dynamics — if the window must grow, reconcile the assert-count delta explicitly).
- [ ] **Step 2: Convert PhysicsJointSleepTest (zero-g joint scenes — the cluster's lowest-risk file).**
  - Delete the 5-pin block inside `MakeQuietWorld()` (:50-54); `gravityY = Real(0)` (:49) stays (file-header-documented load-bearing zero-g).
  - `AddDynamicCircle` default radius `Real(5)` (:58-67) -> **`Real(0.1)`** (re-authored to the body floor; /100's 0.05 would sit below it, and nothing in the file depends on the exact radius).
  - Positions: `A(0,0), B(30,0), C(60,0)` -> `(0,0), (0.3,0), (0.6,0)` (:93-95, :132-134); case 3 `B(40,0)` -> `(0.4,0)` (:161-162). `jd.length = Real(-1)` sentinel (current-separation) — untouched, no joint-length literal exists.
  - **`ApplyImpulse(c, Vec2(0,-4000))` (:145) rewrites Rule 3:** `const Real circleMass = Real(1) * kPi * Real(0.1) * Real(0.1);` (~0.0314 kg), `targetDv = Real(-20)` -> impulse ~ -0.628 (expression, not the literal).
  - Step counts untouched (v=0 at spawn in zero-g; 120-180-step budgets are 4-6x kSleepTime — survey-verified robust).
- [ ] **Step 3: Convert PhysicsSensorIslandTest (/10 family).**
  - Delete both 5-pin blocks (:75-79, :110-114). Case 1 `gravityY = 0` stays; case 2 `gravityY = 400` (:109) -> `Real(10)`.
  - Case 1 (the cluster's most slop-marginal geometry — re-author /10, NOT /100): sensor box `(0,0), hw=hh=5` -> `hw=hh=0.5`; solid box `(3,0)` -> `(0.3, 0)`. Update the ":83 7-unit overlap" comment: half-width 0.5 each, centres 0.3 apart -> 0.7 m overlap — the "large overlap relative to body size" shape is preserved and every quantity stays 15x+ above kSkin (a /100 centre offset of 0.03 m would sit 1.5x above kSkin, inside speculative-margin noise).
  - Case 2: floor `((0,5),200,5)` -> `((0,0.5),20,0.5)`; both solid boxes `hw=hh=5 -> 0.5`; stack gap `Real(0.5)` (:121) re-authors to **`Real(0.02)`** (= kSkin, comment — mirrors Task 4 case 5). The 30-step loop stays (sleep impossible before step 31; contact by ~step 4).
- [ ] **Step 4: Build + run + triage.** `.\ArcaneTests.exe "[island]"` and `"[joint]"` green; all three PX-PIN greps == 0; `"[physics]"` reconciled (expected 0/0 unless the WakeMerge window grew — reconcile explicitly). Behavioral gates: `IslandsUniformlyAwake` per-step, `sawAnySleep`, island-root (in)equalities, `DebugHasContact`, `JointCount()==0`, IsAwake families — all unmodified; failures = spec §6 contingency.
- [ ] **Step 5: Commit.**
  ```bash
  git add Arcane/Tests/src/PhysicsIslandWakeMergeTest.cpp Arcane/Tests/src/PhysicsJointSleepTest.cpp Arcane/Tests/src/PhysicsSensorIslandTest.cpp
  git commit -m "test(arcane/physics): island wake-merge + joint-sleep + sensor-island -> MKS, pins deleted (MKS P3)"
  ```
  Body: per-file mapping tables, the tile-precedent divergence note, the below-floor exception note, the Rule-3 rewrite, gap re-authors.

---

### Task 6: Re-derive the two warm-start drift bounds in PhysicsSolverTest (P2 carry-forward)

**Files:**
- Modify: `Arcane/Tests/src/PhysicsSolverTest.cpp` — ONLY the two flagged bounds and their comments: `REQUIRE(maxDrift < Real(0.05))` (:596, case "warm-start continuity keeps a stack settled" :539) and `REQUIRE(penPeakTail <= penAtN + Real(0.05))` (:726, the warm-start tail-continuity check — locate by the "CONTINUITY: penetration at N+50" comment if line numbers drifted)

**Interfaces:**
- Consumes: the P2 final-review flag — both 0.05 bounds were kept at px-era values with unit-confused justifications ("2.5x kSkin", "absolute length tripwire... measured tail growth at MKS: 0.0"); the :723-725 comment explicitly says "P3 sleep/settle may revisit with fresh measurements". This task is that revisit.
- Produces: the two loosest P2 re-baselines tightened to measured-and-constant-derived bounds; the P2 flag closed.

- [ ] **Step 1: Measure.** Temporarily add `WARN("maxDrift = " << maxDrift);` before :596 and `WARN("penAtN = " << penAtN << " penPeakTail = " << penPeakTail);` before :726 (Catch2 WARN reports without failing and without adding an assertion). Build; run `.\ArcaneTests.exe "[solver]"` (foreground); record both measured values.
- [ ] **Step 2: Re-derive.** Apply protocol rule 6 with a floor: bound = max(measured x 1.5, `2 * kLinearSlop` = 0.01) — if measured is ~0 (the P2 observation), the bound lands at `Real(0.01)` with the comment naming kLinearSlop as the driving constant and stating the measured value + date:
  ```cpp
  // Re-derived (MKS P3, P2 carry-forward): measured maxDrift ~ <measured> over
  // 1600 steps; bound = 2*kLinearSlop (0.01) -- a settled stack's total creep
  // stays inside the solver's own slop scale, not a px-era absolute.
  REQUIRE(maxDrift < Real(0.01));
  ```
  (same pattern for the :726 growth cushion: `penPeakTail <= penAtN + Real(0.01)`). If a measured value is NOT ~0, use measured x 1.5 and name whichever constant sits nearest (kLinearSlop or kSkin); do not exceed the old 0.05 — that would need a BLOCKED report, not a bound bump.
- [ ] **Step 3: Remove the WARN instrumentation.** Verify the diff touches only the two REQUIRE lines + their comment blocks.
- [ ] **Step 4: Build + run + gate.** `.\ArcaneTests.exe "[solver]"` green; `"[physics]"` green with counts IDENTICAL to the Task 1 baseline arithmetic (WARNs removed, no assertion added or removed).
- [ ] **Step 5: Commit.**
  ```bash
  git add Arcane/Tests/src/PhysicsSolverTest.cpp
  git commit -m "test(arcane/physics): warm-start drift bounds re-derived from fresh MKS measurements (MKS P3, P2 carry-forward)"
  ```
  Body: measured values, old -> new bounds, driving constant.

---

### Task 7: Phase 3 exit verification (controller-run)

**Files:** none (verification + report; fixes loop back into the owning task).

**Interfaces:** Consumes everything above. Produces the P3 exit evidence for the whole-branch review.

- [ ] **Step 1: Burn-down.** `grep -rc "PX-PIN" Arcane/Tests/src Arcane/Sandbox/src | grep -v ":0"` — total **490** (635 - 145), ZERO in all 7 cluster files. `grep -rn "MKS-DEFER" Arcane/Core/src` — still exactly 6 sites (P3 burns none). `git diff main --stat` touches ONLY the 8 test files + this plan doc.
- [ ] **Step 2: Full suite.** Build; `.\ArcaneTests.exe ~[gpu]` (reference: 112058 asserts / 494 cases, ~5.5 min at P6 exit) — ALL PASS; reconcile the count delta against the per-task arithmetic (expected 0/0 unless a settle window grew — every delta must be named).
- [ ] **Step 3: Headless GPU sanity.** `Arcane\bin\Debug-windows-x86_64-md\Loom\Loom.exe --frames 180` and `--backend vulkan --frames 180` — exit 0 (cheap ~3 s each post-P6). SandboxSmoke isolation NOT re-run: P3 touches zero sandbox/engine code (justified skip — the P6 restoration numbers stand).
- [ ] **Step 4: Spec §6 sweep.** Converted piles/stacks settle AND sleep at the 0.05 default (SleepThreshold case 4, StaticSettle both settle cases, AwakeSet case 5 pile + case 9 scene-twin, Island case 1/4/6, JointSleep all three, WakeMerge sawAnySleep) — all green above; zero thresholds bumped, zero behavioral asserts weakened (audit the diff for REQUIRE/CHECK line changes outside the enumerated numeric re-derives).
- [ ] **Step 5: Exit report + push.** Consolidated justification table (every empirical re-derive across T1-T6: measured -> bound -> constant), burn-down number, suite counts + delta arithmetic, the two closed P2 flags (drift bounds), and carry-forwards: **P4** = CC retunes (kMaxSubstep 8->0.1, kDepenetrationSkin 0.05->kSkin) + Gjk kShapeCastTol re-couple + debug-draw px defaults flip (contactMarkerSize 3.0 / comMarkerSize 5.0 / orientationTickLen 18.0 in PhysicsDebugDraw.hpp) + `[mks]` blind-spot note (residency tile / SpatialHash default) + **the unassigned-file allocation (Appendix B — P4/P5 plans must absorb the 9 files / 163 pins the spec never clustered, or burn-down cannot reach zero)**; **P5** = MouseJoint maxForce + CCD re-arm + full-suite wall-time restoration assert + headless framing-tripwire candidate (P6 final-review residual). `git push -u origin feature/arcane-physics-mks-phase3`, then STOP — whole-branch review + merge = USER's call.

---

## Appendix A: MKS probe battery (2026-07-03) — the retired P3 acceptance risk

Copied from the committed P6 plan Appendix A (source `.superpowers/sdd/mks-probe-report.md`, gitignored). ALL 5 PROBES PASS — the spec §6 contingency exists but is NOT expected to trigger:
- **A pile-sleep@0.05:** 5 boxes + 3 circles ALL ASLEEP by step 338, zero re-wake, maxV/maxW == 0 by step 600.
- **B 8-box stack:** slept step 57, drift 0, top-box position error 0.010 m.
- **C CCD@150 m/s:** bullet clamps x=9.93 (wall face 9.98); kinematic baseline tunnels; 400-cap untouched.
- **D restitution 0.6:** apex 0.677 m (analytic e^2*h = 0.72), sleeps step 153.
- **E free-fall:** 50.0 m/s after 5 s (8x under the 400 cap).
- Authoring gotcha: dynamic `MakeAabb` hard-asserts `fixedRotation` (PhysicsWorld.cpp:972-974) — all 7 cluster files already comply (survey-verified).

## Appendix B: PX-PIN files the spec assigned to NO phase (P4/P5 planning input)

After P3, 490 pins remain. Spec §4's P4 cluster holds 258 (FixtureBroadphase 30, SpatialGrid 48, Queries 72, QueryRotation 30, Character 6, Invariants 41, SolverMtInvariance 17, BroadphaseMtInvariance 5, NarrowphaseMt 9) and P5 holds 69 (Ccd 29, VelocityClamp 18, Joints 22). The remaining **163 pins across 9 files were never clustered** and must be explicitly allocated when the P4/P5 plans are written (geometry-pure files rescale "opportunistically" per spec, but these have no owner at all):

| File | Pins | Suggested home |
|---|---|---|
| PhysicsWorldTest.cpp | 60 | P4 (world/lifecycle, broadphase-adjacent) |
| PhysicsFixtureTest.cpp | 24 | P4 (fixture cluster rides FixtureBroadphase) |
| PhysicsDebugRichTest.cpp | 18 | P4/P5 (debug-draw cluster, flips px defaults with it) |
| PhysicsCollisionFilterTest.cpp | 17 | P4 (broadphase filter semantics) |
| PhysicsSystemTest.cpp | 15 | P4 (Astra integration harness) |
| PhysicsDebugAccessorsTest.cpp | 12 | P4/P5 (debug cluster) |
| PhysicsDebugCapsuleTest.cpp | 6 | P4/P5 (debug cluster) |
| PhysicsPauseTest.cpp | 6 | P4 (no-step pause harness) |
| PhysicsDebugDrawTest.cpp | 5 | P4/P5 (debug cluster, with contactMarkerSize/comMarkerSize/orientationTickLen default flips) |

## Self-Review (author checklist — completed)

- **Spec coverage:** §4 P3 cluster list (SleepThreshold, StaticSettle, AwakeSet, Island, IslandWakeMerge, JointSleep, SensorIsland) -> Tasks 1-5 one-for-one; §2 authoring rules -> Global Constraints cross-cutting rules; §6 acceptance + contingency -> the binding Global Constraint + per-task triage steps + Task 7 Step 4; P2 carry-forwards -> Task 6 (drift bounds) + Appendix A (probe numbers, audit item A2's "copy early" directive satisfied by committing them in this plan).
- **Placeholder scan:** every conversion carries exact old -> new values with survey line numbers; the only execution-derived numbers are empirical re-baselines, procedurally defined (measure -> 1.5x headroom or constant floor -> justify), the designed mechanism with two phases of precedent.
- **Type consistency:** `kLinearSlop`/`kSkin`/`kSleepTime` names match PhysicsTypes.hpp:145/153 + Island.hpp:101 (survey-verified); WorldDef field names match the pins being deleted; `Interaction.hpp:109/121/129` cap values read from the shipped header, not recalled; Rule-3 mass formulas match Shapes.cpp (density x area; circle pi*r^2, AABB 4*hw*hh).
- **Ordering:** Task 1 sets the sleep-boundary idiom before heavier files consume it; Task 5's tile-span file documents its deliberate divergence from the P2-T8 tile precedent; Task 6 is independent of Tasks 1-5 (different file) but runs after so its `[physics]` gate reconciles against a stable base; Task 7 skips SandboxSmoke with stated justification (zero sandbox/engine edits).
