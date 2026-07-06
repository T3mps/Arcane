# Physics Arc — Closeout Inventory (2026-07-06)

Everything known-open across the Arcane physics arc, gathered from: the MKS spec/plans
(`docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md`), the SDD ledger,
the Box2D-parity program notes, the SpatialGrid benchmark findings, the 2026-07-04 engine
gap survey (§E), and session memory. Purpose: a single list to drive the conscious
"close out physics and start the game" decision the roadmap analysis recommended.

Branch state (verified 2026-07-06): `feature/arcane-physics-mks-phase2` @50d8d0f5 (parked,
NOT merged) and `feature/arcane-spatial-grid-benchmark` @b4c5fd0c (review-approved,
NOT merged) are both open against `main` @15d824cc.

---

## A. Must-land to close (the arc cannot end mid-stream)

**A1. Finish MKS Phase 2 (parked at Task 2/10, branch `feature/arcane-physics-mks-phase2`).**
Resume = Task 2 whole-diff review (base `6a4da466`), then Tasks 3-9 = convert the remaining
solver/dynamics test files to meters (Dynamics, Solver, Rotation, CompoundCom+Slide,
Simd+Compacted+BodyContacts, PersistentContact+Island, Determinism+2 harnesses; ~265 pins),
then Task 10 exit (PX-PIN burn-down 946->670, full `~[gpu]`, isolated SandboxSmoke pair,
Loom --frames 180, push). Plan: `docs/superpowers/plans/2026-07-03-arcane-physics-mks-phase2.md`.
Effort: M-L (mechanical with a per-file recipe already written).

**A2. MKS Phase 3 — sleep/settle/island cluster.** Own plan doc when reached (SleepThreshold,
StaticSettle, AwakeSet, Island, IslandWakeMerge, JointSleep, SensorIsland). THE acceptance risk
was RETIRED by the 2026-07-03 probe battery (pile sleeps at 0.05 by step 338, zero re-wake;
8-box stack step 57; numbers archived in `.superpowers/sdd/mks-probe-report.md` — copy into
the P3 plan; gitignored, so copy early). Effort: M.

**A3. MKS Phase 4 — broadphase/spatial cluster + the deferred CC/query constants.** Converts
Broadphase, SpatialGrid, FixtureBroadphase, TileGrid, Queries, QueryRotation, Character,
Invariants, the 3 MT-invariance suites. MUST pick up the `MKS-DEFER(P4)` markers:
`CharacterController::kMaxSubstep` 8px -> 0.1 m, `kDepenetrationSkin` 0.05 -> kSkin (0.02),
`Gjk.hpp kShapeCastTol` re-couple to kLinearSlop (Box2D distance.c:611-614). Plus the `[mks]`
blind-spot note (residency tile + SpatialHash default have no regression tripwire). Effort: M-L.

**A4. MKS Phase 5 — CCD + clamp + joints cluster.** Re-arm the Ccd tests (dynamic probes
~150-300 m/s, genuinely fast under the 400 cap, thin meter walls), VelocityClamp, Joints
(meters + mass-scaled impulses). MUST pick up `MKS-DEFER(P5)`: MouseJoint `maxForce = 1e9`
rescale (3 sites: Joint.hpp/Joints.hpp/Joints.cpp — at MKS, mass*g is ~1-1e3 N). Effort: M.

**A5. MKS Phase 6 — sandbox + camera (the visible payoff).** All 8 scenes re-authored in round
meters, `pixelsPerMeter = 100` camera transform, drag/throw retuned (40 m/s / 400 m/s^2),
bullet scene ~70 m/s, HUD slider ranges + debug-viz sizes in meters, the two interim-adjusted
sandbox tests properly rewritten, last PX-PINs deleted, CLAUDE.md gains the MKS line.
EXIT ASSERT (carry-forward): `~[gpu]` wall-time RESTORES from ~1087 s (the px-content-on-
0.05-margin churn) — converts an asserted attribution into a measured one. Effort: M-L.

**A6. Merge/close the SpatialGrid benchmark branch** (`feature/arcane-spatial-grid-benchmark`,
17 commits, final review READY TO MERGE @b4c5fd0c). Merging also kills the recurring `Bench`
slnx-ghost build noise every implementer must be warned about. USER decision. Effort: S (merge)
— or explicitly close it unmerged and delete (the findings doc is the durable artifact either way).

**A7. Fix the restitution-rebound test tautology** (found during P2: `apexY`/`reboundHeight`
in PhysicsSolverBudgetTest never updates under y-down, so the assertion proves nothing about
real rebound; pre-existing, deliberately left by the conversion). Natural home: the A1 resume
(same file/branch). Effort: S.

## B. Recorded Box2D-parity divergences (fix OR formally accept — decide each)

Deliberate, ledgered gaps vs the vendored Box2D v3.1.1 oracle. None block gameplay; closing
the arc means marking each FIX or ACCEPTED-AS-DIVERGENCE in the parity ledger.

- **B1. Kinematic/pinned bodies not speed-clamped** — awake-set integrate clamps dynamics only;
  Box2D clamps its whole awake set (solver.c:79-122). Effort: S.
- **B2. Gravity/damping ordering** — Arcane damps gravity; Box2D adds the undamped gravity delta
  after damping (solver.c:102-106). Behavior-shifting to fix; re-baselines. Effort: S-M.
- **B3. GAP A: joints in island split/merge linkage** — island machinery follow-up from the
  split-linkage work. Effort: M.
- **B4. GAP B: sensors in island linkage** — sibling follow-up. Effort: M.
- **B5. G3: CCD multithreading** — Box2D parallelizes CCD; Arcane's is serial. Effort: M.
- **B6. G4: 24-color constraint coloring parity** — Arcane's persistent coloring differs from
  Box2D's 24-color scheme. Effort: M-L, value unproven.
- **B7. Dynamics resting on tile-spans trip the EmitContactConstraints no-sleeping-dynamic
  invariant** (pre-existing, serial path). Diagnose + fix or relax with justification. Effort: S-M.

## C. Optional perf/MT tails (recommend: DECLARE DONE, revisit only when a game consumer demands)

- **C1. Create-phase narrowphase MT** (~3.0 ms serial tail; machinery from update-phase MT reusable).
- **C2. D3 sleep-pass MT.**
- **C3. StaticCandidates MT.**
- **C4. Broadphase set-churn algorithmic floor.**
- **C5. SpatialGrid Cycle 2 feature** — opt-in tile-occupancy index + opt-in mover-broadphase for
  large tile-dense scenes; statics stay on the tree. DATA-GATED by the Cycle-1 findings doc; the
  gate should be "a real game map shows the profile," not curiosity.
- **C6. `m_staticList` consolidation** — deferred benchmark decision.
- **C7. MT solve breaks even at 10k (L3-bandwidth-bound)** — known ceiling, inherent, no action.

## D. Robustness/verification follow-ups

- **D1. Escape-robustness stress re-confirm** — the isfinite guard + SaneBox magnitude/total-cell
  budgets landed; re-verify under finite-but-huge coordinates once P6's meter-scale scenes exist
  (the original crash repro was px-scale unwalled density). Effort: S.
- **D2. Post-arc memory/doc sweep** — update the physics memories (P1 carry-forward notes,
  parity-program NEXT pointers), CLAUDE.md Arcane section, and record the B-ledger dispositions.
  Effort: S.

---

## Recommended closeout package (minimum honest "done")

1. **A1 + A7** — finish Phase 2 (it is unacceptable to leave the tree half-converted with 670
   pins and markers pointing at phases that never come).
2. **A2 -> A5** — run Phases 3-6 to completion (each is a plan doc + a mechanical-with-judgment
   conversion; the risk retired with the probe battery). Exit: zero PX-PIN, zero MKS-DEFER,
   wall-time restored, Loom visual gate.
3. **A6** — merge or explicitly close the benchmark branch.
4. **B1-B7** — a single half-day adjudication pass: fix the cheap ones (B1, likely B7), ACCEPT
   the rest with one-line rationales in the parity ledger.
5. **C1-C7** — mark all "closed: revisit on game-consumer demand" — no code.
6. **D1-D2** — fold D1 into P6 exit; D2 is the final act.

After that, physics is DONE — by decision, not exhaustion — until combat/overworld actually
consume it.
