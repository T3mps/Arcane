# Arcane Physics — Solver-MT Scaling Rework (Design)

- **Date:** 2026-06-27
- **Status:** Design approved (brainstorming complete). Implementation plan pending.
- **Branch:** `feature/arcane-physics-phaseD1-solver-mt` (continues on the D1 branch @ `7726ab7b`; D1 alone is a measured regression, so the seam + bug fix + test it shipped land together with this rework as one mergeable unit).
- **Scope:** Make the colored `SoftStep` solver's multithreading *actually scale* by closing the two implementation gaps versus Box2D v3 that the 2026-06-27 research surfaced: **(Gap 1)** replace the ~190 fork-join `ParallelFor` dispatches/step with a single **persistent worker region** (Box2D `b2SolverStage` / atomic stage-sync / block-CAS work-stealing), and **(Gap 2)** replace the false-sharing `BodyStateSoA` (six parallel `float` arrays) with a tight **32-byte AoS `BodyState`** gathered via aligned-load + AoS->SoA transpose with a null-index branch. Box2D v3 is the **direct model**: it is the same algorithm (TGS-Soft + graph coloring) and it *succeeds* at exactly our case (single big island), so our slowdown is an implementation gap, not a wall.
- **Relates to:** Phase D1 (within-color solver MT — the seam, the `FinalizePositionsSoA`-serial bug fix, and `SolverMtInvarianceTest` this rework keeps and builds on); the SIMD solver Part 1 (the colored SoA lane-wide solve this re-lays-out and threads); the JobSystem API (`ITaskExecutor` — this rework deliberately adds **no** new entry, see §5.1); the physics data-model rearchitecture (the awake-set + persistent contacts + persistent coloring this parallelizes).
- **Reference:** `docs/superpowers/research/2026-06-27-box2d-v3-solver-mt-findings.md` (the two gaps + Box2D source citations + our code touchpoints) and `docs/superpowers/specs/2026-06-27-arcane-physics-phaseD1-solver-mt-design.md` (the D1 within-color design + its §8 gather-bound risk, which materialized).

---

## 1. Post-mortem — why D1 regressed, and the Box2D mechanism that fixes each failure

Phase D1 shipped a **correct** within-color MT solver: byte-identical at any thread count, an injected executor seam, and a real bug fix (`FinalizePositionsSoA` must stay serial — it mutates the shared `DynamicTree` broadphase). But measured at 10k (scene 8, the constant-whisk stress that keeps ~all bodies awake), the MT `solve` stage was a **regression**:

| Config | `solve` ms/step @ 10k | vs serial |
|---|---|---|
| Serial | 17.23 | 1.00x (baseline) |
| MT (grain 8) | 23.21 | **0.74x (slower)** |
| MT (grain 64) | ~19.6 | 0.88x (still slower) |

For comparison, Box2D v3 — the **same** TGS-Soft + graph-colored algorithm — gets **5.39x at 8 threads (67% efficiency)** on a single monolithic 5050-body island (`large_pyramid`, committed benchmark), and 7.17x on a many-island 10k scene. So the algorithm threads; our *implementation* did not. Two root causes, each with a direct Box2D fix:

### Failure 1 — dispatch storm (threading model)
D1 issues a fresh `ITaskExecutor::ParallelFor` **per color, per pass, per substep** — roughly `12 colors x 3 passes x 4 substeps + restitution + integrates` ~= **190 enkiTS enqueue+barrier round-trips per step**. Each round-trip is a full task-system enqueue, wake, and join. That grain 8->64 recovered ~40% of the loss is the tell: dispatch overhead was a first-order cost. We were paying the task system ~190 times to do work that should be dispatched **once**.

> **Box2D fix (Gap 1):** Box2D enqueues `workerCount` tasks **once per step**. Workers then run the **entire** substep loop inside a persistent region, advancing across colors/passes/substeps with a single atomic store (`atomicSyncBits`) plus a spin-wait — never re-entering the task system. A color barrier costs *one atomic store + one spin*, not an enqueue+join. ~190 round-trips -> 1.

### Failure 2 — false sharing + hardware-gather cost (data layout)
D1's `BodyStateSoA` is six separate `std::vector<float>` (`vx,vy,w,dpx,dpy,dq`). Two consequences, both Box2D-avoided:
- **False sharing:** worker A writing `vx[12]` and worker B writing `vx[15]` touch the *same* 64-byte line (`vx[0..15]`). Disjoint *bodies* still collide on cache lines because the same field for 16 adjacent bodies is one line. This is the "memory-bandwidth contention that did not respond to grain" — grain can't fix line-level contention.
- **Gather-bound:** each solve pass does **six** `gather()` (the slow hardware-gather instruction) per body, x2 bodies (A,B), x every contact, x every pass. The D1 spec §8 flagged "the solver is gather-bound" as the real risk; it materialized.

> **Box2D fix (Gap 2):** `b2BodyState` is a tight **32-byte AoS** struct (`_Static_assert(sizeof==32)`, 32-byte aligned). Two adjacent bodies = one 64-byte line, and each body's scatter write is a *distinct* 16-byte half -> no false sharing. The gather is **aligned loads + an AoS->SoA transpose** (`UnpackLo/Hi`/`permute`), *not* `_mm_i32gather_ps` — predictable, prefetchable, and one load brings in a body's whole row. Read-only B (static/span) uses a **null-index branch** (inject identity, no memory touch) instead of a shared dummy slot, "to avoid multithreaded sharing and the associated cache flushing." This also speeds the **single-thread** SIMD path — our prior "SIMD win didn't land" was partly this layout.

### The discipline lesson (process, not just code)
D1's gate proved *correctness* (byte-identity) but its scaling result was a regression we shipped as "diagnosis." This rework's gate is **dual and non-negotiable**: byte-identity AND a measured `solve` speedup over serial. We **measure-first** (re-confirm the regression on the current branch), land each gap behind its own measurement, and **measure-after** each. A green test on an unscaled solver is not success. We do not assume the Box2D mechanism helps — we measure that it does, per gap.

---

## 2. Decisions (from brainstorming)

1. **Continue on the D1 branch.** D1's executor seam, the `FinalizePositionsSoA`-serial fix, and `SolverMtInvarianceTest` are the foundation; D1 alone is a regression (not independently mergeable); the whole thing lands as one unit. (A fresh branch would only duplicate D1's content.)
2. **Gap 2 (AoS) first, then Gap 1 (persistent region).** AoS is lower-risk, byte-identity-gated on the existing serial path, and delivers a measurable single-thread win on its own. Building the persistent region on the clean (no-false-sharing) substrate means its scaling number is not contaminated. Two independent measure-after points.
3. **Follow Box2D directly for the threading model and the stage machinery** (§5). In particular, the threading entry **reuses the existing `ParallelFor`** exactly as Box2D reuses its one generic `enqueueTask` — **no new JobSystem API**.
4. **Dual gate per gap — correctness AND measured speedup** (§7). Byte-identity is necessary but not sufficient.

---

## 3. Architecture overview

`SoftStep::Solve` keeps its serial pre-work (Prepare, SyncIn, color-bucket, Build batches), then runs the whole substep loop inside **one** persistent worker region, then does the serial post-work (`FinalizePositionsSoA`, which mutates the shared `DynamicTree`). The data the region reads/writes is the new AoS `BodyState` array.

```
SoftStep::Solve(ctx):
  --- serial pre-region (unchanged order from D1) ---
  PrepareContacts / PrepareJoints              # scalar, O(contacts)
  bodyStates.Resize(solverCount)               # AoS, NO +1 dummy
  bodyStates.SyncInCompacted(world)            # fill AoS rows
  bucket ctx.contacts by persistent color      # m_colorRefs / m_overflowRefs
  Build SoA batches per color                  # m_colorBatches
  build the flat SolverStage list for the step # m_stages + m_blocks (reused scratch)
  --- the persistent region (Gap 1) ---
  executor->ParallelFor(workerCount, 1, [&](begin,end,_){ SolverWorker(begin); })
      # begin==0 -> main/orchestrator; begin>0 -> thief
      # workers run integrateVel . warmStart[c] . solve[c] . integratePos . relax[c]
      #   across all substeps, then restitution[c] . storeImpulses
      # overflow + joints solved main-serial inline (thieves spin)
  --- serial post-region ---
  FinalizePositionsSoA(ctx)                     # SERIAL: mutates DynamicTree (D1 fix)
```

The win is structural: the region replaces ~190 task-system round-trips with one dispatch + ~190 atomic-store-and-spin barriers, and the AoS layout removes the false sharing and the hardware-gather cost that capped both the serial and MT paths.

---

## 4. Gap 2 — the 32-byte AoS `BodyState`

### 4.1 Layout
Replace `BodyStateSoA` (six `std::vector<float>`) with one aligned AoS array:

```cpp
struct alignas(32) BodyState   // 32 B -> two bodies per 64 B line
{
    float vx, vy, w;           // linear + angular velocity
    float dpx, dpy, dq;        // TGS accumulated position / angle delta
    float _pad0, _pad1;        // pad to 32 B (a future flags word slot)
};
static_assert(sizeof(BodyState) == 32 && alignof(BodyState) == 32);
```

- **`dq` stays a scalar angle delta** (NOT Box2D's `b2Rot` cos/sin pair). Changing it would shift the numerics and break byte-identity; keeping it makes Gap 2 a *pure re-layout* of the same float values. `IntegratePositionsSoA`/`FinalizePositionsSoA` keep their scalar-angle math unchanged.
- Storage is a 32-byte-aligned `std::vector<BodyState>` sized `solverCount` (awake dynamics + kinematics). **The `+1` scatter-safe dummy tail is dropped** — replaced by the null-index branch (§4.3). If the platform `std::allocator` does not honor 32-byte over-alignment, use an explicit aligned allocator; verify the data pointer is 32-aligned in a test.
- The struct + container + sync helpers replace `BodyStateSoA.hpp`. The type is no longer "SoA" — rename the file/type accordingly (e.g. `BodyState` + a small `BodyStateStore` holding the aligned vector and the Sync helpers). Mechanical, but rename it so the name stops lying.

### 4.2 The transpose gather/scatter (the actual perf fix)
Replace the per-field hardware gather (`SimdSolve::GatherBody` doing six `gather()` per body) with a backend-specialized helper that gathers a *whole* body row per body:

- **AVX2 (width 8):** for the eight `bodyIndex` lanes, do eight **aligned 256-bit loads** of the eight `BodyState` rows, then an **8x8 AoS->SoA transpose** (`_mm256_unpacklo/hi_ps` + `_mm256_permute2f128_ps`) to produce lane vectors `vx[8],vy[8],w[8],dpx[8],dpy[8],dq[8]`. One transpose feeds all six fields; the row load is contiguous and prefetchable.
- **NEON (width 4):** the 4-lane analog (per-lane `vld1q` of rows + `vtrn`/`vzip` transpose) or per-lane struct reads — whichever the SIMD wrapper supports cleanly.
- **Scalar (width 1):** a plain struct read — this **is** the bit-exact oracle.

The transpose only *reorders* bits — it computes nothing — so the gathered lane values are exactly the stored floats. The solve math (arithmetic order, per-lane ops) is unchanged, so the result stays byte-identical to D1 and lane-width invariance holds. The helper lives in a small physics-SIMD header built on the `Arcane::Simd` primitives (NOT a portable `Simd` primitive — transpose width is lane-count-specific). Scatter is symmetric: transpose `vx/vy/w` lanes back to rows and store the dynamic lanes.

### 4.3 Null-index branch (replaces the dummy slot)
- A read-only B (static body or tile-span) packs `bodyIndexB = kNullBodyIndex` (= `-1`). A padding lane likewise.
- **Gather:** a `-1` lane reads a shared zero **identity** `BodyState` (no real-body memory touch), exactly Box2D's `B2_NULL_INDEX` path. Implementation: per-lane pointer select `idx >= 0 ? &states[idx] : &kIdentity` before the row load, then transpose.
- **Scatter:** skip lanes where `!dynB` (covers both null-B and kinematic-B). A always scatters (A is always an awake dynamic, never null).
- **Kinematic B is unchanged:** it keeps a real dense row (gathered for the relative-velocity push term, never scattered).
- This removes the one-hot dummy-slot write contention (all read-only-B lanes hammering one line) that would otherwise cap Gap 1 under MT.

### 4.4 Blast radius
- `BodyStateSoA` -> `BodyState` + store; the four sync helpers (`SyncIn`/`SyncInCompacted`/`SyncOut`/`SyncOutCompacted`) and the three integrate loops (`IntegrateVelocitiesSoA`/`IntegratePositionsSoA`/`FinalizePositionsSoA`) index AoS rows (`states[i].vx` etc.).
- `SimdSolve::{WarmStart,SolveNormalAndFriction,ApplyRestitution}` swap the six per-field `GatherBody`/`ScatterBody` calls for the transpose gather/scatter.
- `ContactConstraintSimd::PackLane`/`PadLane` thread the `-1` null sentinel (drop `dummyIndex`); the scalar overflow path (`OverflowSetup`/`DenseB`) returns `-1` for static/span and reads identity (zero vB, skip B write) for `ib < 0`.

### 4.5 Gate
Existing `[physics]`/`[determinism]`/`[simd]` suites green **unchanged** (a pure re-layout passes them as-is); lane-width invariance (forced-scalar oracle vs 8-wide) holds; an aligned-storage test asserts 32-byte alignment; **and** a STEPPROF single-thread `solve` measurement at 10k shows AoS+transpose beats the old SoA serial number (Gap 2's own deliverable, before any MT).

---

## 5. Gap 1 — the persistent worker region (direct Box2D model)

### 5.1 The threading entry — reuse `ParallelFor`, no new API
Box2D does **not** add a bespoke "persistent workers" primitive for the solver; it reuses its one generic `b2EnqueueTaskCallback`:
`enqueueTaskFcn(b2SolverTask, workerCount, /*minRange*/1, stepContext, ...)`. The callback receives `(startIndex, endIndex, threadIndex, ctx)` and **uses `startIndex` as the worker index**, relying on `itemCount = workerCount, minRange = 1` to make one item per worker. This maps 1:1 onto our existing `ITaskExecutor::ParallelFor`:

```cpp
const std::uint32_t workerCount = ctx.executor->WorkerCount();
ctx.executor->ParallelFor(workerCount, /*minBatch*/1,
    [&](std::size_t begin, std::size_t /*end*/, std::uint32_t /*enkiThread*/)
    {
        SolverWorker(stepCtx, static_cast<std::uint32_t>(begin)); // worker index = begin
    });
```

- **Worker index = `begin`** (the partition start), NOT enki's `worker`/threadnum. `begin` is unique per partition; threadnum can repeat if one thread takes two partitions. (Box2D uses `startIndex` for exactly this reason.)
- **`begin == 0` => the main/orchestrator** (exactly one — the partition covering index 0 always exists); `begin > 0` => thief.
- **Why `ParallelFor` is safe here** (reversing the research doc's "wrong primitive" caveat — that caveat only bites a body that blocks on a *specific* peer): Box2D's worker body does **not** block on a specific peer. The main **self-completes every block alone** (claim-at-execution, §5.3); thieves are pure optimization. So correctness *and* liveness hold for 1 worker, N workers, or enki coalescing partitions onto one thread (the coalesced case just yields fewer effective workers = slower, never wrong). `SerialTaskExecutor::ParallelFor(1,…)` -> `fn(0,1,0)` -> `begin==0` -> main runs the whole sequence serially = the deterministic reference path.
- **`ParallelFor` (fork-join) is retained** only for post-solve finalize/CCD, never for the solver stages. No `ITaskExecutor`/`JobSystem` change.

### 5.2 Stage list (mirrors `solver.c`, in D1's exact order)
A dedicated `SolverStages` unit (kept out of `SoftStep.cpp` so each file stays focused) owns the stage/block machinery. `SoftStep::Solve` builds a flat stage list per step into reused scratch (`m_stages`, `m_blocks` -> zero steady-state alloc), in **D1's exact order** so the result is unchanged:

```
per substep:  integrateVel . warmStart[c0..cC] . solve[c0..cC](bias) . integratePos . relax[c0..cC]
once at end:  restitution[c0..cC] . storeImpulses
```

Each colored type is **one stage per color** (a color boundary is a barrier — adjacent colors share bodies, Gauss-Seidel). Structures mirror Box2D:

```cpp
struct SolverBlock { int startIndex; int count; std::atomic<int> syncIndex; };
struct SolverStage {
    StageType type;             // IntegrateVelocities, WarmStart, Solve,
                                // IntegratePositions, Relax, Restitution, StoreImpulses
    SolverBlock* blocks; int blockCount;
    int colorIndex;             // which color (for the colored types)
    std::atomic<int> completionCount;
};
```

Block sizing follows Box2D: contact blocks `minBlockSize ~= 4` wide-constraints, body blocks `~= 32`, target `~= blocksPerWorker * workerCount`.

### 5.3 Orchestration (Box2D `b2ExecuteMainStage` / `b2ExecuteStage`)
- **Main (`begin==0`)** drives the sequence: for each stage, store `atomicSyncBits = (syncIndex << 16) | stageIndex` (the single-store "advance to next stage" signal), run `ExecuteStage`, then spin `while (completionCount != blockCount) pause();`, reset `completionCount = 0`, advance `syncIndex`.
- **Thieves (`begin>0`)** spin reading `atomicSyncBits`; on change, decode `stageIndex` + `syncIndex` and run `ExecuteStage` for that stage; repeat until the main signals "done" (a sentinel stage index).
- **`ExecuteStage`** = staggered **ring-order CAS**: starting at a worker-specific offset, `compare_exchange(block.syncIndex, prev -> cur)`; the first CAS winner runs the block (`ExecuteBlock`) and does `completionCount.fetch_add(1)`. Work-stealing, no central counter. The main rings around and claims everything still unclaimed -> self-completion.
- **Overflow + joints = main-serial inline.** After the colored stages of a pass, the main runs `Overflow{WarmStart,Solve,Restitution}` and (when present) the joint bridge (`SyncVelToWorld`/`SolveJoints`/`SyncVelFromWorld`) serially while thieves spin on the next `syncBits` — exactly how Box2D solves its overflow color (and overflow joints) by the main worker between parallel stages. One code path; no fast/slow split. (Scene 8 has no joints, so that branch is cold; correctness matters, perf does not.)

### 5.4 Deliberate divergence from Box2D (scope guard)
Box2D also parallelizes `PrepareContacts`/`PrepareJoints` as stages. We keep `Prepare` + `Build batches` **serial pre-region** (as D1 has them): they are O(contacts), not the substep x color x pass bottleneck, and keeping them out of the region makes the byte-identity gate trivial. Parallelizing prepare is a clean follow-up, not part of this milestone's win.

---

## 6. Determinism — byte-identical at any worker count

The D1 proof, now at **block** granularity:
1. **Stage order is fixed** and serial-equivalent — the main advances stages in D1's exact sequence; a color boundary is a hard barrier (spin until `completionCount == blockCount`).
2. **Within a color, blocks are disjoint-write.** The coloring invariant guarantees each dynamic body appears in <=1 contact of a color, so partitioning the color's *contacts* into blocks also partitions its *bodies*. Claim order, worker count, and which-worker-runs-which-block therefore cannot change any float.
3. **Body-stages** partition the awake range -> disjoint ranges.
4. **Atomics are control-flow only** (`syncBits`, `completionCount`, `syncIndex` CAS) — they never enter the FP math, so there is no atomic-ordering-induced FP non-determinism.
5. **Overflow + joints are main-serial**, fixed order.

The serial path (`SerialTaskExecutor`, one worker = main) and the N-worker path execute the same stages over the same disjoint blocks; only *who* runs a block differs, and a block's math is independent of that. Hence byte-identical: **serial == enki(1) == enki(N)**.

---

## 7. Testing + gates (both required, per gap)

- **Gap 2 (correctness):** existing `[physics]`/`[determinism]`/`[simd]` suites green **unchanged** (pure re-layout); lane-width invariance (forced-scalar oracle vs 8-wide) holds; an aligned-storage test asserts the `BodyState` array is 32-byte aligned and `sizeof == 32`.
- **Gap 2 (perf):** STEPPROF single-thread `solve` ms at 10k beats the old SoA serial number (AoS + transpose gather win, before any MT). State the achieved delta.
- **Gap 1 (correctness):** `SolverMtInvarianceTest` (RunPile, 500 boxes — sized > both grains so MT genuinely runs) asserts post-step world state (positions + velocities) is **byte-identical** across `SerialTaskExecutor`, `JobSystem(1)`, and `JobSystem(0)` (N threads). Existing run-twice determinism gates stay green.
- **Gap 1 (perf) — the headline deliverable:** STEPPROF scene-8 @ 10k, `solve` ms/step at executor threads = 1 vs N. PASSES only on a **measurable speedup over the serial number**; the report states achieved speedup + efficiency. If it does not beat serial, the deliverable is the **diagnosis** (grain? still gather/L3-bound? color sizes too small at 10k? residual sharing?), not a green check — the D1 discipline lesson, applied.
- **Build gates:** Arcane Debug + Release, both backends (D3D12 + Vulkan); ArcaneCore static-CRT clean (PhysicsWorld + solver + `SolverStages` compile under /MT); headless Loom smoke exit 0.
- **Tooling notes:** msbuild is NOT on PATH — use `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` (VS2026; the VS2022 one cannot build this toolset). clangd diagnostics are false positives (expects Clang 20); MSVC + the tests + the Loom smoke are truth. Run `ArcaneTests.exe` from its exe dir. STEPPROF: `#define ARCANE_STEPPROF 1` + Reset@step180/Dump@step480 in `PhysicsWorld::Step`, scene via `ARCANE_SANDBOX_SCENE=8`; the instrumentation is throwaway/reverted. Verify any commit-chain claim with `git rev-parse <sha>^` (subagents misreport the parent SHA by grabbing the branch base).

---

## 8. Risks

- **The ~67% single-island efficiency ceiling is real.** Box2D itself gets only ~67% on one big island (gather/L3 bound — "scales well as long as cores share an L2/L3"). Solve-MT is **additive on top of the Gap-2 single-thread win**, not a clean N×. The §7 gate measures the achieved efficiency rather than assuming it.
- **Step-level cap.** Even a perfect solver-MT caps the *Step* win at ~the solve fraction (~36% at 10k); narrowphase (39%) is the larger lever and is out of scope (D2). This rework's deliverable is the *solve stage* beating serial, not a whole-Step target.
- **Stage-machinery correctness.** Spin/CAS/atomic-sync is subtle. Mitigated by the byte-identity invariance gate (§7) + the disjointness proof (§6) + keeping the serial path (`begin==0` alone) as the in-suite reference.
- **Residual memory/L3 bandwidth ceiling on the transpose gather.** Gap-2's own single-thread measurement isolates it *before* Gap 1 stacks on, so a bandwidth wall is attributed correctly instead of blamed on the threading.
- **Over-alignment portability.** `std::vector<BodyState>` must yield 32-byte-aligned storage; if the toolchain allocator doesn't honor it, use an aligned allocator (tested).

---

## 9. Scope / non-goals / future

- **IN:** Gap 2 (AoS `BodyState` + transpose gather + null-index branch) then Gap 1 (persistent region via reused `ParallelFor` + `SolverStages` stage/block runner). Replaces D1's per-color `ParallelFor` dispatch and `BodyStateSoA`.
- **KEEP:** the `SetExecutor`/`SolverContext::executor` seam; `FinalizePositionsSoA` **serial** (the DynamicTree bug fix — never re-parallelize); `SolverMtInvarianceTest`.
- **OUT:** D2 narrowphase MT (the 39% stage), D3 island/sleep MT (the 18% stage), parallel `Prepare`/`Build`, SoA->colored joints, any numerics/convergence change, raising `kColorCount`, a new `ITaskExecutor` entry.
- **FUTURE (explicit):** D2 narrowphase MT (the bigger Step lever); parallel prepare; colored SoA joints (Part 2); island-level parallelism as a complement for many-small-island scenes.

---

## 10. Implementation sequencing (for the plan to expand)

1. **Measure-first.** Re-confirm the D1 regression on the current branch (STEPPROF scene-8 @10k: serial vs N `solve` ms). Record the baseline; the rework is judged against it.
2. **Gap 2.1 — AoS `BodyState` + store + sync helpers** (rename off `BodyStateSoA`); rewrite the three integrate loops to index AoS rows. Gate: existing suites green (still using a naive per-lane gather), aligned-storage test.
3. **Gap 2.2 — transpose gather/scatter + null-index branch**; swap the six-gather calls; thread the `-1` sentinel through the packer + overflow. Gate: suites green, lane-width invariance, **single-thread `solve` STEPPROF beats old serial**.
4. **Gap 1.1 — `SolverStages` unit + stage-list build** (serial driver first: main-only, `begin==0`, no thieves) — prove byte-identity with the stage runner replacing the per-color `ParallelFor` while still single-threaded.
5. **Gap 1.2 — the persistent region** (`ParallelFor(workerCount,1,…)`, main + thieves, ring-CAS claiming, atomic stage-sync, overflow/joints main-serial). Gate: `SolverMtInvarianceTest` byte-identical serial == enki(1) == enki(N); full suites green.
6. **Gap 1.3 — measure-after + tune.** STEPPROF scene-8 @10k `solve` at 1 vs N threads; tune block sizing; record speedup + efficiency. PASS = measurable speedup over serial, else the documented diagnosis.
7. **Final holistic review** of the whole branch diff, then `superpowers:finishing-a-development-branch`.

---

## 11. Code touchpoints

- Solver driver + the per-color `ParallelFor` blocks (Gap 1 target): `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` (`Solve()`, lines ~825-872; `IntegrateVelocitiesSoA`/`IntegratePositionsSoA`; `FinalizePositionsSoA` stays SERIAL).
- New: `Arcane/Core/src/Arcane/Physics/Solver/SolverStages.hpp/.cpp` (stage/block structs + main/thief orchestration + ring-CAS `ExecuteStage`).
- AoS layout (Gap 2): replace `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp` with the `BodyState` struct + store; rename the type/file off "SoA".
- Transpose gather/scatter + null-index (Gap 2): `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` (`GatherBody`/`ScatterBody` -> transpose helpers; `PackLane`/`PadLane` -> `-1` sentinel) + the scalar overflow path in `SoftStep.cpp` (`OverflowSetup`/`DenseB`).
- Executor seam (reused, unchanged): `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp` + `Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp`; `SolverContext::executor` (`Solver.hpp`).
- Awake set: `PhysicsWorld::AwakeBodies()` / `AwakeIndexOf()` / `AwakeCount()` / `KinematicIndexOf()`.
- Determinism gate: `Arcane/Tests/src/SolverMtInvarianceTest.cpp`.
