# Arcane MKS Units — Phase 4 (Broadphase/Spatial/World/Debug Cluster + Engine Flips) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the remaining non-P5 test surface to MKS — 19 files (421 PX-PINs deleted, repo 490 -> 69, plus the pin-invisible PhysicsTileGridTest) — and land the three deferred engine flips: CharacterController (kMaxSubstep 8 -> 0.1, kDepenetrationSkin 0.05 -> 0.02), Gjk kShapeCastTol re-coupled to kLinearSlop, and the PhysicsDebugDraw px-era defaults (5.0/18.0/3.0 -> 0.05/0.18/0.03).

**Architecture:** Spec §4 P4 (`docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md`) + the P3 plan's Appendix B allocation (all 9 spec-unassigned files land HERE, per survey C's firm verdict) + the accumulated carry-forwards. Unlike P2/P3/P6, **this phase touches engine code** (3 flip clusters), each flipped in the SAME task as its test cluster (the spec's Stage-iii deferral logic: flipping under px content breaks, not shifts). Fact base: three survey docs (`.superpowers/sdd/p4-survey-A-spatial-query.md`, `p4-survey-B-mt-character.md`, `p4-survey-C-world-debug.md`) — full-file reads, all counts grep-verified, all line numbers exact as of main @3691f024. Task briefs point implementers at their survey sections for the complete literal inventories; this plan carries the binding decisions and every value that is NOT a mechanical divide.

**Tech Stack:** C++23 (Arcane Core/Physics + Render/PhysicsDebugDraw + tests), MSVC via msbuild, Catch2. Parity source: vendored `ThirdParty/box2d-3.1.1` — verify invariants against it directly, never recall.

## Global Constraints

- **Conversion protocol:** `.superpowers/sdd/p2-conversion-protocol.md` applies verbatim (PX-PIN deletion, Rule-3 impulses, empirical re-baseline = measure -> ~1.5x headroom or named-constant floor -> justify, ASCII-only, explicit-path staging, FOREGROUND-one-command builds/tests, trailers). Overrides: branch `feature/arcane-physics-mks-phase4` off main @3691f024+; `[physics]` base = 30636 assertions / 277 cases; full `~[gpu]` reference = 112058 / 494 @~5 min.
- **Spec §6 contingency (binding):** behavioral asserts (hit-set membership, byte-identity ST==MT, island/contact counts, no-tunnel gates, `RenderErrorCount()==0`) pass UNMODIFIED. A behavioral failure at MKS content = STOP, diff vs vendored Box2D, report BLOCKED. Never weaken, never bump engine constants beyond this plan's three mandated flips.
- **Engine flips are task-scoped:** exactly THREE engine edits are authorized, each inside its named task — Gjk.hpp (Task 3), CharacterController.hpp (Task 8), PhysicsDebugDraw.hpp/.cpp (Task 9). Any other engine/Core/Sandbox diff is a defect. MKS-DEFER burn-down: 6 -> 3 (the P5 MouseJoint trio must remain).
- **Survey-derived cross-cutting rules** (in addition to the protocol's):
  1. Gravity literals (400/900/200) -> `Real(10)` or delete-for-default; never divided. Zero-g scenes keep explicit zero.
  2. Default scale family is **/10** (bodies land 0.1-10 m). Exceptions, all mandated per-task below: NarrowphaseMt's span scene (/100, WakeMerge mirror), AwakeSet-style sandbox mirrors (none in P4), and do-not-touch zones (SpatialGrid budget tests, FixtureBroadphase nudge).
  3. `hashCellSize = 64` pins are DEAD CODE in every P4 file (no file selects `BroadphaseKind::Hash`; `MakeBroadphase()` reads it only for Hash — survey A §top). Pure deletions.
  4. Solver-residual/energy bounds (`~0.21` penetration budgets, `keBound` formulas, `MaxPenetration < 0.5`) are NEVER divided — re-derive per protocol rule 6 against g=10/kLinearSlop/kSkin, cross-checked against P2's converted PhysicsSolverTest/PhysicsSolverBudgetTest values.
  5. Per-iteration-assert loop counts stay fixed unless empirically forced; any change reconciles the `[physics]` delta arithmetic explicitly.
  6. Plan-authoring rule (P3 lesson): re-measure steps state the procedure, not the expected value.
- **Suite green at every commit.** Per-task gates = file tags + `"[physics]"`. Full `~[gpu]`, SandboxSmoke pair, and Loom only at Task 10 (engine WAS touched this phase — the smoke pair is mandatory again, unlike P3).
- **Build:** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m` — ONE FOREGROUND command. No new .cpp files -> no premake regen.
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466
  ```

## File Structure

| File | Change | Task |
|---|---|---|
| `Arcane/Tests/src/PhysicsWorldTest.cpp` | /10, 60 pins deleted | 1 |
| `Arcane/Tests/src/PhysicsPauseTest.cpp` | /10, 6 pins | 1 |
| `Arcane/Tests/src/PhysicsCollisionFilterTest.cpp` | /10, 17 pins | 1 |
| `Arcane/Tests/src/PhysicsSpatialGridTest.cpp` | /10 uniform-K incl subject cell sizes; budget tests UNTOUCHED; 48 pins | 2 |
| `Arcane/Tests/src/PhysicsTileGridTest.cpp` | /10 (ZERO pins — the survey-A discovery) | 2 |
| `Arcane/Tests/src/PhysicsMksDefaultsTest.cpp` | blind-spot closure (comment + assert where accessible) | 2 |
| `Arcane/Tests/src/PhysicsQueriesTest.cpp` | /10 + margin tightening, 72 pins | 3 |
| `Arcane/Tests/src/PhysicsQueryRotationTest.cpp` | /10, 30 pins | 3 |
| `Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp` | **ENGINE FLIP:** kShapeCastTol -> 1.25*kLinearSlop | 3 |
| `Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp` | /10 EXCEPT nudge (already MKS from P1.iii); 30 pins | 4 |
| `Arcane/Tests/src/PhysicsFixtureTest.cpp` | /10 + full hand-derivation comment recompute; 24 pins | 4 |
| `Arcane/Tests/src/PhysicsDebugAccessorsTest.cpp` | /10 (broadphase-internals file, not debug-draw); 12 pins | 4 |
| `Arcane/Tests/src/PhysicsSystemTest.cpp` | /10 + ECS-coupled recomputes + intra-file scale cleanup; 15 pins | 5 |
| `Arcane/Tests/src/PhysicsInvariantsTest.cpp` | /10 + keBound/pen-budget re-derives; 41 pins | 6 |
| `Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp` | span /100 (WakeMerge mirror) + rest /10; 9 pins | 7 |
| `Arcane/Tests/src/SolverMtInvarianceTest.cpp` | pins only (content already meter-scale); 17 pins | 7 |
| `Arcane/Tests/src/BroadphaseMtInvarianceTest.cpp` | pins only; 5 pins | 7 |
| `Arcane/Tests/src/PhysicsIslandWakeMergeTest.cpp` | comment ride-alongs ONLY (mirror header + slack driver) | 7 |
| `Arcane/Tests/src/PhysicsCharacterTest.cpp` | /10 + MaxPenetration re-derive; 6 pins | 8 |
| `Arcane/Core/src/Arcane/Physics/CharacterController.hpp` | **ENGINE FLIP:** kMaxSubstep 0.1, kDepenetrationSkin 0.02 | 8 |
| `Arcane/Tests/src/PhysicsDebugRichTest.cpp` | /10, 18 pins | 9 |
| `Arcane/Tests/src/PhysicsDebugCapsuleTest.cpp` | /10 (NOT /100 — tilt-threshold coupling), 6 pins | 9 |
| `Arcane/Tests/src/PhysicsDebugDrawTest.cpp` | /10 + explicit zoom fold; 5 pins; [gpu] re-verify | 9 |
| `Arcane/Core/src/Arcane/Render/PhysicsDebugDraw.hpp` (+ .cpp comments) | **ENGINE FLIP:** defaults 0.05/0.18/0.03 + comMarkerSize doc fix | 9 |

---

### Task 1: Branch, baseline, and the mechanical trio (WorldTest + PauseTest + CollisionFilter)

**Files:** Modify the three test files above (83 pins). Add (commit only): this plan doc.

**Survey sections:** C §1 (WorldTest), §5 (Pause), §3 (CollisionFilter) — complete literal inventories with line numbers.

- [ ] **Step 1: Branch + baseline.** `git checkout -b feature/arcane-physics-mks-phase4`; commit this plan doc. Build; record `.\ArcaneTests.exe "[physics]"` counts (expect 30636/277) and the PX-PIN total (expect 490).
- [ ] **Step 2: Convert all three files (/10).** Per survey tables: delete all 83 pins (WorldTest's are bare `WorldDef wd;`-adjacent; every pinned field is dead or coincides with the MKS default); rescale content /10; CollisionFilter case (a)'s live `gravityY = 200` -> `Real(10)` default (assertion is count-based, insensitive); WorldTest's determinism-case velocity formula converts /10. No analytic rework exists in these files (survey-verified).
- [ ] **Step 3: Gate.** File tags green + `"[physics]"` reconciled (expected 0/0); PX-PIN grep == 0 for all three.
- [ ] **Step 4: Commit** (explicit paths). `test(arcane/physics): world/pause/collision-filter tests -> MKS, pins deleted (MKS P4)`.

---

### Task 2: SpatialGrid + TileGridTest + the [mks] blind-spot closure

**Files:** `PhysicsSpatialGridTest.cpp` (48 pins), `PhysicsTileGridTest.cpp` (ZERO pins — px content invisible to the burn-down; survey A §TileGrid), `PhysicsMksDefaultsTest.cpp`.

**Survey sections:** A §File-1 (the subject-vs-pin cell-size analysis is THE task hazard), A §TileGrid, A §MksDefaults.

- [ ] **Step 1: SpatialGridTest.** Delete all 48 pins (all dead-or-default). Apply **uniform-K /10 to whole scopes together** — each raw `SpatialGrid(tileSize)` construction is SUBJECT-UNDER-TEST: tileSize 32 -> 3.2 WITH its scope's pos [-500,500] -> [-50,50], ext [2,60] -> [0.2,6.0], and every moved/queried box (survey A lists each; grid bucketing floor(coord/tileSize) is scale-covariant only under uniform division). **The 3 budget/robustness tests (:339-387) are DO-NOT-TOUCH** — they exercise the dimensionless kMaxCellsPerAxis/total-cell engine budgets ("under per-axis, over total" arithmetic at :367-371 must keep exercising the TOTAL-budget path); zero pins live there. G4's velocity 600 -> 60 m/s gets a comment (legal, fast-for-scale).
- [ ] **Step 2: TileGridTest.** /10 throughout: cellSize `Real(32)` -> `Real(3.2)` (:87, :129, :166, :182, :226), capsule halfLen/r 6/4 -> 0.6/0.4, gap 1.0 -> 0.1 (:259-266). Grid col/row counts unchanged. This file has no WorldDef and no pins — cite survey A §TileGrid in the commit body so the burn-down bookkeeping explains a converted file with no pin delta.
- [ ] **Step 3: MksDefaultsTest blind-spot.** Add a documenting comment block + assertions where an accessor exists: (a) `hashCellSize` is read ONLY under `BroadphaseKind::Hash` (`MakeBroadphase`, PhysicsWorld.cpp:93-107) — the default Tree broadphase ignores it; (b) `m_residencyGrid` is hardcoded `SpatialGrid{Real(1)}` (PhysicsWorld.hpp:1402, TODO map-integration) independent of hashCellSize; (c) assert `SpatialHash` default cellSize == 1 IF a getter exists (triage: if none, comment-only — do NOT add an engine accessor for a test). Report which branch was taken.
- [ ] **Step 4: Gate.** `"[grid]"` + `"[physics]"` green (file tags per survey); PX-PIN grep == 0 for SpatialGridTest; budget tests byte-untouched (`git diff` shows no hunk in :339-387).
- [ ] **Step 5: Commit.** `test(arcane/physics): spatial-grid + tile-grid tests -> MKS; [mks] blind spot closed (MKS P4)`.

---

### Task 3: Queries + QueryRotation + the kShapeCastTol re-couple (ENGINE)

**Files:** `PhysicsQueriesTest.cpp` (72 pins), `PhysicsQueryRotationTest.cpp` (30 pins), `Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp` (ONE constant).

**Survey sections:** A §File-2 (incl §g, the tol blast-radius analysis), A §File-3, A §Gjk-audit.

- [ ] **Step 1 (ENGINE): re-couple kShapeCastTol.** Gjk.hpp ~:931:
  ```cpp
  // Coupled to kLinearSlop per Box2D v3.1.1 (distance.c:610-614,641: target =
  // max(linearSlop, totalRadius - linearSlop), tolerance = 0.25*linearSlop --
  // effective near-zero-radius hit band = 1.25*linearSlop). Arcane compares a
  // single flat tolerance against the POST-RADII surface distance (Gjk.cpp
  // Advance), so the flat equivalent of Box2D's band is used; the radius-aware
  // target split is a recorded parity note, not adopted (MKS P4).
  inline constexpr Real kShapeCastTol = Real(1.25) * kLinearSlop; // = 0.00625
  ```
  Delete the MKS-DEFER(P4) marker lines. Both consumers move automatically (Gjk.cpp:888 hit criterion; Queries.cpp:339-358 swept-pad, coupled by identity — verify no other consumer appeared: `grep -rn kShapeCastTol Arcane` must show exactly definition + 2). Verify kLinearSlop is visible in Gjk.hpp's includes (it lives in PhysicsTypes.hpp); add the include if missing.
- [ ] **Step 2: QueriesTest (/10).** Convert `MakeWorldDef`'s centralized pin block (deletes propagate to all 9 call sites) + the 11 inline blocks; `kCellSize 10 -> 1` (16x16 m world), body content /10 per survey. **Margins:** the 4 cast-tolerance margins (:278, :391, :394, :720/:727 — 0.2/0.1/0.5-family) convert /10 AND absorb the tol re-couple in one derivation: new margins ~0.02-0.05 are 3-8x the new 0.00625 tol — state per site `// margin ~Nx kShapeCastTol (1.25*kLinearSlop) (MKS P4)`. Dimensionless `Approx(t)` fractions unchanged; `point.x/y` Approx values /10. The tile-span ShapeCast case (:693) re-derives its span-AABB comment with the new kCellSize.
- [ ] **Step 3: QueryRotationTest (/10).** Per survey mapping: capsule (0.6,0.2), circles r 0.2, boxes (1,0.2), compound offset (1.2,0), wall (5,0.05), bullet box (0.8,0.1), velocities 200/kStep -> 20/kStep (Kinematic — the 400 cap does not apply; KEEP the inline comment saying so). Inequality bounds 91.5/88/95 -> 9.15/8.8/9.5 (t-fraction-preserving, survey-verified safe). Tight Approx margins 1e-3/1e-2 -> /10 (1e-4/1e-3, still >> f32 epsilon — survey's judgment call, adopted for file consistency).
- [ ] **Step 4: Gate.** `"[queries]"`-family tags + `"[physics]"` green; both PX-PIN greps == 0; NO count delta expected. The Invariants case-6 and TileGridTest ShapeCastPoly consumers ride the tol change — `"[physics]"` green covers them; a failure there = triage against the new tol before touching anything (rule 6).
- [ ] **Step 5: Commit.** `feat(arcane/physics): kShapeCastTol -> 1.25*kLinearSlop + queries cluster -> MKS (MKS P4)`. Body: the Box2D cite, the structural parity note, margin table.

---

### Task 4: FixtureBroadphase + FixtureTest + DebugAccessors (fixture/broadphase-internals cluster)

**Files:** `PhysicsFixtureBroadphaseTest.cpp` (30), `PhysicsFixtureTest.cpp` (24), `PhysicsDebugAccessorsTest.cpp` (12 — re-filed here: it tests broadphase/narrowphase debug ACCESSORS, not PhysicsDebugDraw; survey C §7).

**Survey sections:** A §File-4 (THE nudge do-not-touch), C §2 (hand-derivation recompute), C §7.

- [ ] **Step 1: FixtureBroadphaseTest.** Delete 5 pin blocks; /10 the 4 PhysicsWorld tests + SetAngle regression ((4,0.4)/(0.8,0.8)/(0,3)). **DynamicTree fuzzer (:245-294): convert ONLY `pos(-300,300) -> (-30,30)` and `ext(4,40) -> (0.4,4.0)` at :248. Do NOT touch `:252 nudge(-0.02,0.02)`** — already MKS-correct from P1.iii against the fixed kMargin 0.05 (survey A §c: re-dividing would shrink it to slop-scale and defeat the declared-riskiest-case purpose). Add the one-line comment noting the nudge:ext ratio shift is intentional.
- [ ] **Step 2: FixtureTest.** Delete 24 pins; /10 geometry; **fully recompute the load-bearing hand-derivation comments** (header :8-33 + in-test :116-150 — mass/COM/inertia arithmetic at the new dims, not a literal search-replace). The reviewer will recompute them; show the work.
- [ ] **Step 3: DebugAccessorsTest.** Delete 12 pins; /10 content. Zero coupling to the Task-9 default flips (no PhysicsDebugDrawOptions reference — survey-verified).
- [ ] **Step 4: Gate.** File tags + `"[physics]"` green; three PX-PIN greps == 0; fuzzer still green over its 600 iterations.
- [ ] **Step 5: Commit.** `test(arcane/physics): fixture/broadphase-internals cluster -> MKS, pins deleted (MKS P4)`.

---

### Task 5: SystemTest (Astra ECS integration)

**Files:** `PhysicsSystemTest.cpp` (15 pins). **Survey section:** C §4 — read it whole; this is the highest-effort core file.

- [ ] **Step 1: Convert.** Delete 15 pins — **NOTE the bare-float pin form** (`400.0f // PX-PIN`, not `Real(400)`; a Real()-shaped regex misses them). Re-author `kGravityY -> 10.0f` (engine default; nothing requires non-default), `kKinSpeed 100 -> 10.0f`, `startX 50 -> 5`; recompute TEST1's bound from `0.5*g*(kSteps*dt)^2` at g=10 (kSteps=10 unchanged) and TEST2's `expectedX = 5 + 10*dt*30` (kSteps=30 unchanged); widen TEST2's 0.01 margin proportionally if the recompute demands (rule 6, justify). `LocalTransform.position` IS the body position (no scale layer) — every Transform assert recomputes with the content.
- [ ] **Step 2: Resolve the pre-existing intra-file scale inconsistency** (survey C §d gotcha): BuildScene's r=0.5 (already metric) vs TEST5/6's r=5.0/1.0 (px) — pick ONE convention file-wide: keep 0.5, convert TEST5 -> r 0.5, aabb (0.3,0.3)@(2,0), entity (10,5); TEST6 -> r 0.1, localPos (0.3,0). Note the cleanup in the commit body.
- [ ] **Step 3: Gate.** File tag + `"[physics]"` green; PX-PIN grep == 0. TEST4 bit-exact cross-run identity unmodified.
- [ ] **Step 4: Commit.** `test(arcane/physics): PhysicsSystemTest -> MKS + intra-file scale unification (MKS P4)`.

---

### Task 6: InvariantsTest

**Files:** `PhysicsInvariantsTest.cpp` (41 pins). **Survey section:** B §1 — the keBound/pen-budget analysis in §d is the task hazard.

- [ ] **Step 1: Convert (/10).** Delete 41 pins across 8 sites; file-local `kGravity = Real(400)` -> `Real(10)` (live content, feeds 5 cases); /10 all geometry per survey table (floors 30, halves 0.8-1.0, wall 10/0.1/6, speed 200/kStep -> 20/kStep with the tunnel case's zero-g kept); case 6's ShapeCastPoly scene /10 (cs 32 -> 3.2, capsule 0.6/0.4, gap 0.1, margins 0.02/2.2) — re-verify its gap/margins against the NEW kShapeCastTol from Task 3 (direct consumer). Case 5's broadphase-axis enumeration (Tree/Hash/Sap) untouched — that IS the test.
- [ ] **Step 2: Re-derive the four non-length bounds (rule 6, never divided):** the two `~0.21` penetration budgets (cases 1, 8) re-derive empirically and must land in the same family as P2's converted equivalents (PhysicsSolverTest ball-rest measured ~0.0003 -> bound 0.001; cross-check and cite); the two `keBound` energy bounds (cases 3, 8) recompute from the FIXED g=10 and converted drops (`4*g*maxDrop` form recomputes, e.g. 4*10*20 = 800 — verify empirically, do not /10 the old product; gravity scaled /40, lengths /10).
- [ ] **Step 3: Gate.** File tags + `"[physics]"` green; PX-PIN grep == 0; count delta 0/0 expected.
- [ ] **Step 4: Commit** with the re-derivation table.

---

### Task 7: The MT trio + WakeMerge ride-alongs

**Files:** `PhysicsNarrowphaseMtTest.cpp` (9), `SolverMtInvarianceTest.cpp` (17), `BroadphaseMtInvarianceTest.cpp` (5), `PhysicsIslandWakeMergeTest.cpp` (comments ONLY).

**Survey sections:** B §2 (incl §h mirror verdict), B §3, B §4.

- [ ] **Step 1: NarrowphaseMt.** TWO divisors in one file, deliberately (precedent: PersistentIsland /10 vs WakeMerge /100): `BuildSpanRain`/`RunCaptureSpans` -> **/100, copying WakeMerge's exact converted constants** (kSpanCellSize 0.2, spawns x[1.2,11.6] y[5.2,7.2], circle r[0.06,0.11], AABB 0.08, capsule 0.10/0.05, containment slack +60 -> +0.6) **including the below-floor accepted-exception comment verbatim** — this restores the numeric mirror. `BuildChurn`/`BuildCreateHeavy` -> /10 (circle 0.6-1.2, box 0.8, capsule 1.0/0.5, statics /10). **`wd.sleepThreshold = Real(0)` (:294) STAYS** (deliberate MT-coverage knob, scale-invariant); re-word its comment to drop the stale "avoid the invariant" framing (that bug is fixed — WakeMerge proves it), keeping the keep-all-bodies-awake MT rationale.
- [ ] **Step 2: SolverMt + BroadphaseMt.** Delete 17+5 pins. Content is ALREADY meter-scale (piles: box half 0.4/0.45, spacing 1.0/1.2 — survey-verified twin templates); the live `gravityY = Real(400)` in both -> `Real(10)` (delete for default). Verify the piles still generate enough moving proxies for the MT grain thresholds (the files' own WARN-on-1-worker pattern reports degradation; byte-identity asserts are self-relative and scale-independent).
- [ ] **Step 3: WakeMerge ride-alongs (comment-only, 2 sites):** (a) the ":43 mirrors RunCaptureSpans" header — restore its accuracy now the mirror is numeric again (both files /100, same constants); (b) the ":222 slack comment** — rename the driver: the 0.8 bound equals the floor-span thickness (4 rows x 0.2 m), the same structural relation as the px-era 80 = 4x20 (P3 final-review Minor #3).
- [ ] **Step 4: Gate.** `"[mt]"` + `"[determinism]"` + `"[island]"` + `"[physics]"` green; three PX-PIN greps == 0. MT cases WARN (not fail) on single-core — pristine-output rule still applies to real noise.
- [ ] **Step 5: Commit.** `test(arcane/physics): MT trio -> MKS, span mirror restored (MKS P4)`.

---

### Task 8: CharacterController flip + CharacterTest (ENGINE — same commit, mandatory)

**Files:** `Arcane/Core/src/Arcane/Physics/CharacterController.hpp` (2 constants), `PhysicsCharacterTest.cpp` (6 pins, centralized in `MakeWorldDef`).

**Survey section:** B §5 — read it whole (the §g audit is the fact base: exactly 2 length literals in CC, both DEFER-tagged; the .cpp's 1e-9 epsilon is scale-invariant).

- [ ] **Step 1 (ENGINE): flip the two constants.**
  ```cpp
  static constexpr Real kMaxSubstep = Real(0.1); // m -- spec sec 3 (MKS P4); kMaxPasses stays the iteration count
  ...
  static constexpr Real kDepenetrationSkin = Real(0.02); // = kSkin value (spec sec 3, MKS P4); still distinct from Arcane::Physics::kSkin in purpose
  ```
  Delete both MKS-DEFER(P4) marker lines; keep the purpose-distinction sentence.
- [ ] **Step 2: Convert the test (/10, SAME COMMIT).** MakeWorldDef's 6-pin block (zero-g suite — both gravity zeros stay explicit); `kCellSize 16 -> 1.6`; capsules (0.5-0.7); slide-per-tick 20/64/14/6/18 -> 2.0/6.4/1.4/0.6/1.8; wall-face/spawn asserts recompute from the converted grid constants (`23*1.6` etc. — keep them expression-derived where the file already does). **Ratio decision, recorded:** substep:cell goes 1:2 -> 1:16 (spec commits to the flat 0.1 regardless of content scale) — the march gets finer relative to cells, strictly more conservative; document in the commit body, do not re-tune cellSize to chase the old ratio.
- [ ] **Step 3: Re-derive `MaxPenetration < 0.5` (5 sites, per-tick).** Rule 6: measure the per-tick max across the 4 driving cases (WARN instrumentation, removed before commit), bound with headroom against kDepenetrationSkin (0.02)/kLinearSlop — candidate scale ~0.05, but MEASURE first. Loop counts (200/60/120/200) stay — the per-tick asserts dominate this file's count contribution.
- [ ] **Step 4: Empirical focus: case 4 (keystone seam-slide, (0.6,1.8) per tick).** The x-component now spans 6 substeps/tick (was <1). If the seam-catching behavior changes (mid-loop MaxPenetration failures or movedY stall), STOP — that is a CC behavior question vs the Lua-port contract, report BLOCKED with the tick trace; do not loosen the bound.
- [ ] **Step 5: Gate.** `"[character]"` + `"[physics]"` green; PX-PIN grep == 0; `grep -rn "MKS-DEFER" Arcane/Core/src` shows the CC markers GONE (Gjk's went in Task 3; 3 MouseJoint remain).
- [ ] **Step 6: Commit.** `feat(arcane/physics): CharacterController -> MKS (kMaxSubstep 0.1, kDepenetrationSkin 0.02) + character tests (MKS P4)`. Body: measured MaxPenetration table, the ratio note.

---

### Task 9: PhysicsDebugDraw default flips + the debug trio (ENGINE)

**Files:** `Arcane/Core/src/Arcane/Render/PhysicsDebugDraw.hpp` (+ one .cpp comment if stale), `PhysicsDebugRichTest.cpp` (18), `PhysicsDebugCapsuleTest.cpp` (6), `PhysicsDebugDrawTest.cpp` (5, `[gpu]` both cases).

**Survey sections:** C §6, §8 (THE risk analysis — read whole), §9, and §6-9 shared context.

- [ ] **Step 1 (ENGINE): flip the three defaults + fix the doc bug.** `comMarkerSize 5.0f -> 0.05f` AND its :88 doc comment corrected ("canvas px, pre-zoom" is WRONG — .cpp:423 multiplies by zoom; re-word to world-units like contactMarkerSize's); `orientationTickLen 18.0f -> 0.18f`; `contactMarkerSize 3.0f -> 0.03f` (delete its MKS-DEFER(P4/P5) tag). `velocityScale` (seconds) and `lineThickness` (true screen-px, never zoomed) UNCHANGED — survey-verified dimensionally correct. Sandbox is immune (always forwards explicit MKS values — survey C §8 verified the forwarding site copies every field).
- [ ] **Step 2: DebugRichTest (/10).** 18 pins + content; flip-immune by construction (every case sets all four flags and never reads magnitudes) — it rides as the flag-gating regression guard.
- [ ] **Step 3: DebugCapsuleTest (/10 — NOT /100).** 6 pins + capsule dims/position /10; the `1.0f` tilt threshold STAYS (valid at /10 per survey §9 math; at /100 it would fail spuriously — this is why the family choice is mandated). Note in the commit body that this file's default-options usage silently shrinks the emitted COM/tick geometry ~100x (no assertion watches it; CPU mock).
- [ ] **Step 4: DebugDrawTest (/10 + zoom fold) — THE risk file.** Content /10 with the 60-step budget re-verified at g=10; **set `opts.zoom` (and offset if needed) explicitly so the converted scene fills the 128x128 canvas and the flipped marker magnitudes (0.05/0.18/0.03 world) rasterize multi-pixel** — mirror the sandbox's pixelsPerMeter=100 convention (e.g. a ~1.2 m scene at zoom 100). The existing behavioral gates (`stats.quads > 0`, `RenderErrorCount()==0`) stay; ADD one cheap discriminating assert if the Batcher stats expose it (e.g. quad count >= the body-outline floor + marker contribution) — triage against what `Batcher2D::Stats()` actually offers; if nothing discriminates markers, note the pixel-readback gap as a recorded residual (pre-existing class, "no-render-error != pixels visible").
- [ ] **Step 5: Gate.** Trio tags + `"[physics]"` green; 3 PX-PIN greps == 0; **`[gpu]` pair run isolated** (d3d12 + vulkan cases — serialize with any Loom usage per the DLL copy-lock rule) — green with `RenderErrorCount()==0`.
- [ ] **Step 6: Commit.** `feat(arcane/render): physics debug-draw defaults -> MKS (0.05/0.18/0.03) + debug tests (MKS P4)`. Body: the doc-bug fix, the zoom-fold rationale, the flip-visibility matrix (who sees the defaults).

---

### Task 10: Phase 4 exit verification (controller-run)

- [ ] **Step 1: Burn-down.** PX-PIN total == **69** (490 - 421), zero in all 18 pin files; TileGridTest scale-literal spot-check (no 32-px cellSize remains). `grep -rn "MKS-DEFER" Arcane/Core/src` == exactly the 3 MouseJoint sites (P5's). Committed diff = the 24 files in File Structure + this plan, nothing else.
- [ ] **Step 2: Full suite.** Build; `.\ArcaneTests.exe ~[gpu]` — ALL PASS; reconcile vs 112058/494 with per-task delta arithmetic.
- [ ] **Step 3: Engine-touched gates (mandatory this phase, unlike P3):** SandboxSmoke pair ISOLATED (d3d12 + vulkan, ~170 s each reference; names need `\,` escapes; serialize) — green; `Loom.exe --frames 180` both backends exit 0 (CC/Gjk/debug-draw changes ride the sandbox path). Run serially (DLL copy lock).
- [ ] **Step 4: Spec sweep.** Whole-branch changed-assert audit: every changed bound is an enumerated conversion/re-derive; behavioral asserts (byte-identity, hit sets, no-tunnel, RenderErrorCount) untouched; the three engine flips are exactly the mandated values.
- [ ] **Step 5: Exit report + push.** Justification tables consolidated; carry-forwards -> **P5:** MouseJoint maxForce (last 3 MKS-DEFERs), CCD re-arm (~150-300 m/s probes), VelocityClamp + Joints conversion (69 pins -> ZERO), the FULL-SUITE wall-time restoration assert (P1-era 1087 s reference; expect ~5 min already), framing-tripwire candidate (P6 residual), DebugDrawTest pixel-readback gap (recorded residual). `git push -u origin feature/arcane-physics-mks-phase4`; STOP — merge = USER.

---

## Self-Review (author checklist — completed)

- **Spec coverage:** §4 P4 cluster -> Tasks 2-8 (Broadphase=FixtureBroadphase+DebugAccessors T4, SpatialGrid+TileGrid T2, Queries+QueryRotation T3, Character T8, Invariants T6, 3 MT suites T7); §3 CC + kShapeCastTol rows -> T8/T3 with exact spec values; unassigned allocation (P3 Appendix B + survey C verdict) -> T1/T4/T5/T9; debug-default carry-forward (P6) -> T9; [mks] blind spot (P1) -> T2; NarrowphaseMt mirror + WakeMerge comments (P3 final review) -> T7; TileGridTest (survey-A discovery) -> T2.
- **Placeholder scan:** every non-mechanical value is either stated (flip targets, mirror constants, margin families) or procedurally defined per rule 6 with the P3 lesson applied (no primed expected values in measure steps — Task 8's MaxPenetration and Task 6's budgets say "measure first", candidates labeled as candidates).
- **Type consistency:** kShapeCastTol expression uses kLinearSlop (PhysicsTypes.hpp:145); CC values match spec §3 lines 64-65; debug defaults match survey C's .cpp-verified world-unit semantics; WakeMerge mirror constants match B §h field-for-field listing.
- **Ordering:** T3's tol flip precedes T6 (Invariants case 6 consumes it — its gate re-verifies); T8/T9 engine flips ride with their clusters (spec Stage-iii logic); T1 proves the /10 convention on the cheapest files first; T10 restores the engine-touched gate set (smoke pair) that P3 legitimately skipped.
