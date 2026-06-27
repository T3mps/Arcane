# Box2D v3 Solver Multithreading — Findings (technical reference for the Arcane solver-MT rework)

Date: 2026-06-27. Source: Box2D v3 (`github.com/erincatto/box2d`, `main`) + box2d.org blog posts (Solver2D 2024-02, Releasing 3.0 2024-08, SIMD Matters 2024-08, Simulation Islands 2023-10) + committed `benchmark/*.csv`. Researched after Arcane's Phase-D1 within-color solver MT measured 12–26% SLOWER than serial at 10k.

## Why this matters
Box2D v3 IS the algorithm Arcane's `SoftStep` solver models (TGS-Soft + graph coloring). Box2D **successfully** multithreads the colored solver: committed benchmark `large_pyramid` (a single monolithic 5050-body island — pure within-island graph-colored solver MT, exactly Arcane's case) hits **5.39× at 8 threads (67% efficiency)** on an AMD 7950X with all cores on one CCD/L3; `many_pyramids` (~10k, many islands) hits 7.17×. So Arcane's slowdown is an IMPLEMENTATION gap, not a fundamental wall. Two concrete gaps cause it; close BOTH the way Box2D does.

## GAP 1 — Threading model: persistent parallel region (1 dispatch/step), NOT per-color ParallelFor (~190/step)
Arcane currently issues a fresh `ITaskExecutor::ParallelFor` per color per pass ≈ **~190 enkiTS round-trips per step** (12 colors × ~4 passes × 4 substeps), each a full enqueue+barrier. Grain 8→64 recovered ~40% of the loss — confirming dispatch overhead is real.

Box2D issues **`workerCount` tasks ONCE per step** (`b2SolveGraph` → enqueue `workerCount` × `b2SolverTask`). Workers then run the ENTIRE substep loop inside a persistent spin-loop; they never re-enter the task system between colors/passes/substeps. Mechanism (`src/solver.c`, `src/solver.h`):
- All stage descriptors are pre-built once per step into a flat arena array. `b2SolverStage{ type, b2SyncBlock* blocks, blockCount, colorIndex, b2AtomicInt completionCount }`; `b2SyncBlock{ b2SolverBlock block; b2AtomicInt syncIndex }`. Stage order per substep: prepareJoints, prepareContacts, integrateVelocities, then PER ACTIVE COLOR { warmStart, solve×ITER, } integratePositions, PER COLOR relax×ITER, PER COLOR restitution, storeImpulses. (`6 + 4C` stages/substep.)
- One worker wins the orchestrator role (CAS on `mainClaimed`); it drives stages via `b2ExecuteMainStage`: store `atomicSyncBits = (syncIndex<<16)|stageIndex` (ONE atomic store = the whole "advance to next stage" signal), participate in claiming blocks, then spin `while (completionCount != blockCount) b2Pause();`, reset, advance.
- Thief workers spin on `atomicSyncBits`; when it changes they decode the stage and claim blocks. **Color barrier cost = 1 atomic store + 1 spin-wait, NOT a task-system round-trip.**
- Block claiming = staggered ring-order CAS on `blocks[i].syncIndex` (previous→current); first CAS winner runs each block (work stealing, no central counter).
- Block sizing (`b2ComputeBlockCount`): contacts → `minSize=4` wide-constraints (32 contacts), target `4*workerCount` blocks; bodies → `minSize=32`.
- Overflow color (the last of 24) is solved SERIALLY by the orchestrator, inline between parallel color stages (thieves spin).
- `b2ParallelFor` (fresh enqueue+finish) is used ONLY for post-solve `FinalizeBodies` + `BulletBodies` (2–3 dispatches/step), NOT the solver stages.

→ To match: build a persistent solver task that runs the whole substep loop with an atomic-word stage sync + block-CAS work stealing over Arcane's `ITaskExecutor`. NOTE: our current `ITaskExecutor::ParallelFor` (blocking, fork-join) is the WRONG primitive for this — the rework needs either (a) a "run N persistent workers until I say stop" executor entry, or (b) a Box2D-style stage/block orchestration layer that uses one `ParallelFor(workerCount, 1, …)` whose body is the persistent spin-loop. Decide this in the spec (it touches the JobSystem API).

## GAP 2 — Data layout: 32-byte AoS body state, NOT SoA (false sharing)
Arcane's `BodyStateSoA{ vx[], vy[], w[], dpx[], dpy[], dq[] }` (separate arrays) causes **false sharing**: thread A writing `vx[12]` and thread B writing `vx[15]` hit the SAME 64-byte cache line (`vx[0..15]`). This is the "memory-bandwidth contention" the measurement saw that did NOT respond to grain — it's largely false sharing.

Box2D's `b2BodyState` is a tight **32-byte AoS struct** = `{ b2Vec2 linearVelocity; float angularVelocity; uint32 flags; b2Vec2 deltaPosition; b2Rot deltaRotation; }` (8 floats), `_Static_assert(sizeof==32)`, 32-byte aligned. Key properties (`src/body.h`, `src/contact_solver.c`):
- Split from the COLD heavy `b2BodySim` (~80 B: transforms, mass, damping…) which the inner solve never touches. Only the 32-B state participates in gather/scatter. Two adjacent bodies = one 64-B cache line; each body's scatter write is a distinct 16-B half.
- Stores DELTA position/rotation (not absolute) → static bodies have trivial identity state, never written.
- Lives in the DENSE awake `b2SolverSet.bodyStates` array (sleeping bodies are MOVED to other solver sets, vacating slots → hole-free dense). Arcane already has an awake-set (`AwakeBodies()` / `AwakeIndexOf`) — analogous.
- Box2D STILL GATHERS scattered indices (`b2GatherBodies`): two aligned `_mm_load_ps` per body (NOT hardware `_mm_i32gather_ps`) + `UnpackLo/Hi` AoS→SoA transpose. Predictable/prefetchable. NO dummy static slot — uses `B2_NULL_INDEX` branch (injects identity, no memory touch) — explicitly "to avoid multithreaded sharing and the associated cache flushing." Scatter is symmetric, skips non-`b2_dynamicFlag` bodies.
- Constraint data (`b2ContactConstraintWide`) is FULLY SoA (every field a wide vector except the body `indexA/indexB`) → constraint reads STREAM; only body state is gathered.

→ To match: replace `BodyStateSoA` with a tight 32-byte AoS `BodyState` array (aligned), gather via aligned load+transpose, drop the scatter-safe dummy slot for a null-index branch. **This also speeds the SINGLE-THREAD SIMD path** (our prior "SIMD is gather-bound" was partly this layout), so it's a win even before MT.

## Caveats (set expectations honestly)
- Even Box2D gets only **~67% efficiency on a single big island** (gather/L3 ceiling is real; Catto: "scales well as long as cores share an L2/L3"). Solver-MT is ADDITIVE on top of a ~2× single-thread SIMD win, not a clean N×.
- Box2D's BIGGEST wins (7.17×) come from **island-level** parallelism + threading ALL stages (broadphase + narrowphase, both scalar-parallel). Arcane's current Step ≈ narrow 34% / solve 32% / sleep 17% (10k measured). A perfect solver-MT alone caps the Step win ≈ 26%; threading narrowphase (D2) compounds it.
- Box2D uses enkiTS too — so the task LIBRARY isn't the differentiator; the persistent-region USAGE pattern is.

## Arcane code touchpoints
- Solver driver + per-color passes + per-body loops: `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` (`Solve()`; the `for (auto& batches : m_colorBatches) ParallelFor(...)` blocks; `IntegrateVelocitiesSoA`/`IntegratePositionsSoA`; `FinalizePositionsSoA` is SERIAL — see below).
- SoA to replace with AoS: `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp`.
- SIMD batches + gather/scatter: `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` (`ContactConstraintSimd::Build`, `SimdSolve::WarmStart/SolveNormalAndFriction/ApplyRestitution` incl. the Task-2 `(…,begin,end)` range overloads).
- Executor seam (may need a new "persistent workers" entry — see Gap 1): `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp` + the enkiTS impl `Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp`. `SolverContext::executor` carries it (`Solver.hpp`).
- Awake set: `PhysicsWorld::AwakeBodies()` / `AwakeIndexOf()` / `AwakeCount()`.
- KNOWN HAZARD (already fixed in D1): `FinalizePositionsSoA` must stay SERIAL — `CommitSlotPosition → UpdateMoverProxies` mutates the shared `DynamicTree` broadphase (concurrent tasks race → heap corruption). Don't re-parallelize it.
- Determinism gate: `Arcane/Tests/src/SolverMtInvarianceTest.cpp` (`RunPile`, 500 boxes — sized > both grains to genuinely exercise MT). Serial == enki(1) == enki(N) must stay BYTE-IDENTICAL through any rework.
