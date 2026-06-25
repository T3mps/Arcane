# Arcane Physics — Data-Model Rearchitecture to the Box2D-v3 Two-Layer Model (Design)

- **Date:** 2026-06-25
- **Status:** Design approved (decomposition + sequence). Phase A plan pending.
- **Scope:** Rearchitect the Arcane 2D physics engine's data substrate from a single stable-slot SoA to the Box2D-v3 **two-layer model** — a stable *identity* layer (id + generation) over a compacted, migratable *hot-data* layer (per-set / per-color contiguous arrays). This is a **milestone-level strangler migration in four phases**, each independently landable, tested, and value-delivering. This doc is the milestone roadmap + target architecture; each phase gets its own spec → plan → execute cycle.
- **Relates to:** `project_arcane_simd_solver` (Part 1, the colored-SoA SIMD solver — re-homed in Phase C, not discarded), `project_arcane_physics_perf_stress` (the structural perf roadmap this supersedes/absorbs), `feedback_engine_evolves_not_frozen` (determinism is the only hard contract; numerics re-baseline on purpose).
- **Supersedes the premise of:** `docs/superpowers/specs/2026-06-25-arcane-simd-solver-design.md` §1, which asserted the 10k stress scene is "solver-bound." Measurement (below) disproves it: the solve is ~3% of the Step. The SIMD-solver work is correct and foundational, but the 10k perf win requires this data-model restructure, not more solver-side optimization.

---

## 1. Context & Motivation

After Part 1 (the colored-SoA SIMD contact solver) landed correct and gate-green, a Dist `--perf` smoke of the 10k stress scene (Sandbox scene 8) showed **no material change** vs the pre-rewrite baseline (~100–336 ms per `Step`). Direct per-phase instrumentation of `PhysicsWorld::Step` (stabilized, ~11.5k contacts, 10 000 bodies) found:

| Phase | ms/Step | % | Optimized by Part 1? |
|---|---|---|---|
| narrowphase (`UpdateContacts`) | 26.0 | 47% | no |
| broadphase proxy refresh (`FinalizePositionsSoA` → `UpdateMoverProxies`) | 22.7 | 41% | no |
| emit + events | 2.9 | 5% | no |
| **the SIMD solve math** | **1.6** | **3%** | ✅ Part 1 |
| coloring + SoA build + sync | 0.7 | 1% | (Part 1 overhead) |

**The Step was never solver-bound.** It is collision-maintenance-bound, over a pile that never sleeps. Three root causes, all confirmed in our code:

1. **9 992 / 10 000 bodies never sleep.** `Island::UpdateSleep` rebuilds a single global union-find every step, groups the whole connected pile into **one island**, and sleeps an island only as a unit — so one body kept above threshold by the soft solver's residual jitter vetoes sleep for all 10 000. (Our own code flags this scan is O(n²) for one big island.)
2. **Broadphase proxies + a second residency `SpatialGrid` are refreshed per-body-per-step**, unconditionally, with no "moved beyond margin" gate at the call site (`CommitSlotPosition` → `UpdateMoverProxies`). The `DynamicTree` has a fat-AABB containment skip, but it still marks every body "moved," so the pair-set rebuild + residency teardown pay full price.
3. **The SIMD solve is gather-bound.** Because body state is indexed by sparse world slot, Part 1 had to add `gather`/`scatter`, a serialized scatter, a dummy slot, and an `iload` op — overhead that exists *only* because the hot data isn't compacted. This is a throughput ceiling, not a bug.

Box2D v3 (the engine's direct model) eliminates all three with one structural idea.

## 2. Box2D v3 findings (sourced)

Verified against the live `erincatto/box2d` `main` source and Erin Catto's posts (box2d.org/posts):

- **Two-layer data model.** *Identity* records (`b2Body`, `b2Contact`, `b2Shape`, proxies) live in id-indexed arrays with generation counters — stable for the object's lifetime; all cross-references point here. *Hot data* (`b2BodySim`, `b2BodyState`, `b2ContactSim`) lives in **per-set / per-color contiguous arrays**, swap-remove-compacted, and **migrates between sets** on sleep/wake. The bridge is one indirection: `b2Body.setIndex + localIndex` (`src/body.h`, `src/world.h`).
- **Solver sets** (`b2SolverSet`): `b2_staticSet`, `b2_disabledSet`, `b2_awakeSet`, then one set **per sleeping island**. The solver iterates only the awake set's contiguous arrays. **`b2BodyState` (the hottest velocity/delta data) exists only for awake bodies** — dropped on sleep, recreated on wake. Migration is swap-remove from source + emplace into target + patch the swapped element's back-reference; sleep/wake is O(island), not O(world) (`src/solver_set.c`).
- **Sleep = set migration, not a flag check.** A sleeping island is physically moved out of the awake set, so the solver / broadphase-move / narrowphase loops *never iterate it* — ~zero cost. Per-island as a unit (one un-quiet body keeps its island awake — by design). Threshold: farthest-point speed < `sleepThreshold` (0.05 m/s default) for `B2_TIME_TO_SLEEP = 0.5 s`. The TGS-soft relax pass settles velocities below threshold cleanly so bodies actually sleep (`src/solver.c`, box2d.org/posts/2023/10/simulation-islands/).
- **Persistent incremental islands.** New body = 1-body island; new touching contact **merges** islands (immediate, *serial* union-find — kept serial on purpose for deterministic constraint order / stable warm-starting); destroyed contact makes the island a **split candidate** (deferred, quota-limited). ~10× faster than v2's per-step DFS rebuild.
- **Contact graph = the SIMD substrate.** Awake+touching `b2ContactSim` records live in **per-color contiguous arrays** (`b2GraphColor.contactSims`), with a per-color body bitset. Within a color no body appears twice ⇒ independent ⇒ both lock-free MT and gather-friendly 8-wide AVX2. Coloring is **incremental** (assigned at contact create, mutated on destroy — never recomputed per step). Can't-color constraints go to a single **overflow** color solved scalar/serial. (Color count: older v3 / our Part 1 use 12; v3.1 source shows 24 — a Phase-C detail.)
- **Broadphase.** Fat-AABB **containment skip**: a moving body only touches the tree when its tight AABB *exits* its fat AABB (50 mm margin); otherwise no-op. A **move buffer** (per-type bitset + array) lists only escaped proxies; pair-find iterates `moveCount`, not body count. **Sleeping bodies keep their proxies in the tree but leave the move set.**
- **Warm-start travels with the record.** Impulses live inside `b2ContactSim.manifold`; they survive the per-step narrowphase rebuild by feature-id match *and* set migration by `memcpy`. **No separate slot-keyed warm-start table.** (Part 1's T3 already adopted this — our impulses live on the persistent Contact's manifold points.)

## 3. Decision

**Adopt the full Box2D-v3 two-layer model** (stable identity + compacted migratable hot data in solver sets / graph colors), rather than the lighter "awake-index-list over the stable slot SoA."

Rationale: the compaction is the single substrate that delivers **sleep** (resting islands migrate out → ~zero cost across residency + broadphase + narrowphase + solve at once), **gather-free SIMD** (contiguous per-color lanes — makes Part 1 actually pay off), **awake-only state allocation**, **O(island) sleep/wake**, and a clean **MT** parallel unit (colors over contiguous arrays). The awake-index-list would bank the sleep/compute win cheaply but keep the gather ceiling and forbid awake-only state — re-introducing Box2D's split piecemeal anyway. The engine's ambition (best-in-class) and the already-sunk SIMD-solver investment both favor the correct substrate.

## 4. Target architecture (end state)

- **Identity layer.** Bodies and contacts in id-indexed arrays with a free-list id pool + generation. A `Body` record holds `setIndex` + `localIndex` (its hot-data location), topology (contact/shape/island links), mass scalars, and sleep bookkeeping — **no velocity/transform hot data**. We already have the contact half: `ContactPool` is id-keyed + generation-safe; broadphase proxies are stable-keyed.
- **Hot-data layer.** `BodySim` (transform/COM/mass/damping) + `BodyState` (velocity + position delta — awake-only) in per-set contiguous arrays; `ContactSim` (manifold + warm-start impulses + cached pose) in per-color arrays (awake+touching) or a set (sleeping/non-touching). Swap-remove compaction with back-reference patching; one `setIndex + localIndex` indirection from identity → hot data, and a `*Id` back-pointer hot data → identity.
- **Solver sets:** static / disabled / awake / per-sleeping-island. **Persistent incremental islands** drive set migration.
- **Constraint graph:** per-color contiguous `ContactSim` arrays + per-color body bitset; incremental coloring; scalar overflow color. Part 1's lane-wide solve runs gather-free over these.
- **Broadphase:** fat-AABB containment skip + move buffer; sleeping proxies remain in the tree, excluded from the move set; the residency grid (combat-sphere seam) maintained only for awake bodies (or lazily — Phase-B detail).
- **What carries forward unchanged:** warm-start-on-manifold (T3), the unified `Collide` narrowphase, the speculative/CCD machinery, the `Arcane::Simd` wrapper, the behavioral `[physics]` suite.

## 5. Phased roadmap (strangler)

Each phase is independently landable, behaviorally gated, and re-measured with `[STEPPROF]` before the next. Detailed per-phase specs/plans are written when the phase is reached (so B/C/D can incorporate what A/B taught us).

**Phase A — Persistent incremental islands.** Replace the per-step global union-find with islands maintained incrementally: merge on contact-link (serial union-find, deterministic), split deferred + quota-limited on unlink. Fixes the O(n²) sleep scan and is the prerequisite for set-migration sleep. *No storage change yet.* Gate: island-topology + determinism tests; `[physics]` green; `[STEPPROF]` unchanged (or island management drops to ~µs).

**Phase B — Identity/sim split + solver sets + sleep-by-migration.** Introduce the `Body` identity record (`setIndex`/`localIndex`) + `BodySim`/`BodyState` compacted per-set arrays + the four solver-set types + sleep/wake island migration (swap-remove + back-ref patch). Make sleep *actually trigger* (per-island quiescence the soft solver can satisfy). Fold in **broadphase coherence** (sleeping proxies leave the move set; awake proxies fat-AABB-gated; residency for awake bodies only). **The big sleep win lands here** — a settled scene's resting fraction costs ~zero everywhere. Gate: a 10k settle scene sleeps (the never-sleeping pile is fixed); `[STEPPROF]` shows the resting fraction drop out of narrowphase + broadphase; `[physics]` green; run-twice determinism.

**Phase C — Constraint-graph-owned compacted `ContactSim` + gather-free SIMD.** Move contacts into per-color contiguous arrays with incremental (persistent) coloring; **re-home Part 1's lane-wide solve onto contiguous lanes**, deleting the gather/scatter/dummy-slot/`iload` machinery. Gate: the SIMD solve throughput rises (lane-width invariance still holds; `[physics]` green); `[STEPPROF]` solve fraction drops on a churning scene.

**Phase D — Multithreading.** Parallelize the Step (narrowphase collide, colored solve, finalize, broadphase pair-find) over the compacted awake set + colors via the engine's enkiTS `JobSystem`. Colors are the parallel unit; serial union-find merge + serial pair-creation + scalar overflow preserve determinism. **The churn-half win lands here.** Gate: near-linear scaling on a churning scene; run-twice identical; `[physics]` green.

**Sequencing rationale (shrink-then-thread, Box2D's own order):** islands (A) are the prerequisite for sleep; sleep + sets (B) shrink the *awake working set* so later work isn't spent on resting bodies; compaction (C) removes the SIMD gather ceiling on the awake remainder; MT (D) divides the lean, compacted awake work across cores. Threading before shrinking would just parallelize wasted work — the mistake Erin Catto explicitly warns against.

## 6. Determinism, measurement, workload

- **Determinism is the only hard contract:** run-twice-identical must hold at every phase. Adopt Box2D's determinism patterns where parallelism enters (serial union-find for island merge, serial contact-pair creation, scalar overflow color, fixed color/block order). No fast-math (`/fp:strict`).
- **No exact goldens.** The `[physics]` behavioral-invariant suite + run-twice + (for the solver) lane-width invariance are the gate; numerics re-baseline deliberately where a structural change shifts a value within an invariant (never weakening an invariant), per `feedback_engine_evolves_not_frozen`.
- **`[STEPPROF]` per-phase breakdown is the regression/win gate.** Promote the throwaway profiling instrumentation used in the investigation into an opt-in, gated build switch so each phase's effect on narrowphase / broadphase / solve / sleep is measured, not assumed.
- **Workload target: 50/50 settle/churn at *real-game scale* (hundreds–low-thousands of bodies)**, with the 10k stress scene retained as the stress/regression gate. Settle scenes are won primarily by Phase B (sleep); churn scenes by Phases C+D (compacted SIMD + MT); broadphase coherence (B) helps both.

## 7. Non-goals / deferred / risks

- **Not a big-bang rewrite.** Strangler migration; each phase keeps the engine green and shippable.
- **Part 2 (SIMD joints) is deprioritized** — it is solver-completeness, not a perf lever (the solve is 3%); it can be done any time but is off the perf critical path. Part 1's contact SIMD is re-homed in Phase C.
- **B/C/D detailed designs are deferred to their own specs** (written when reached, after re-measuring). This doc fixes the target + decomposition + contract only.
- **Risks:** the migration touches the whole physics data model. Mitigations: phase isolation (each independently gated), the determinism contract, the behavioral suite as the safety net, and the existing identity-layer pieces (id-keyed `ContactPool`, stable proxies, warm-start-on-manifold) that reduce Phase-B/C surface. The largest single risk is Phase B (storage split + migration + back-ref patching) — it gets the most careful plan + the most behavioral gates.

## 8. Open questions (resolved per-phase, not now)

- Exact `Body`/`BodySim`/`BodyState` field partition and how the existing SoA columns map onto it (Phase B).
- Whether to keep the residency `SpatialGrid` per-step-for-awake vs lazy-on-query (Phase B; both fit the awake-only invariant).
- Graph color count (12 vs 24) + the incremental-coloring data structure (Phase C).
- The `JobSystem` seam + deterministic parallel reduction over colors (Phase D).
- Whether sensors / kinematic-only / disabled bodies get their own sets or fold into existing ones (Phase B).
