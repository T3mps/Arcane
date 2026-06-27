# Arcane Physics Solver-MT Scaling Rework — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the colored `SoftStep` solver's multithreading actually scale by closing the two Box2D-v3 implementation gaps that made Phase D1 a regression (0.74x at 10k): a 32-byte AoS `BodyState` with transpose gather (Gap 2), then a single persistent worker region (Gap 1).

**Architecture:** Gap 2 first (lower risk, byte-identity-gated, single-thread win), then Gap 1 on the clean substrate. Gap 1 reuses the existing `ITaskExecutor::ParallelFor` exactly as Box2D reuses its one generic task-enqueue — NO new JobSystem API. A new header-only `SolverStages` unit runs the whole substep loop inside one dispatch with atomic stage-sync + ring-CAS work stealing; the serial path (one worker) stays the deterministic reference.

**Tech Stack:** C++20, MSVC (VS2026), `Arcane::Simd` (AVX2/NEON/scalar), `std::atomic`, enkiTS (via `EnkiTaskExecutor`), Catch2 tests.

**Required reading for every task** (the detailed design + math + Box2D citations live here, referenced rather than re-transcribed):
- Spec: `docs/superpowers/specs/2026-06-27-arcane-physics-solver-mt-scaling-rework-design.md`
- Findings: `docs/superpowers/research/2026-06-27-box2d-v3-solver-mt-findings.md` (Box2D `solver.c`/`body.h`/`contact_solver.c` source map)

## Global Constraints

- **Branch:** work on `feature/arcane-physics-phaseD1-solver-mt` (continues D1). NOT pushed; user merges to local main (FF) then pushes.
- **No new compilation unit.** Keep all new code in HEADERS (`BodyState.hpp`, `SolverStages.hpp`) plus edits to existing `SoftStep.cpp` (sync-helper definitions stay there — they need `PhysicsWorld` accessors). Core premake globs `src/**.cpp/.hpp`, so headers need no regen; adding a new `.cpp` would. Avoid it.
- **Byte-identity is the Gap-2 + Gap-1 correctness gate.** Both gaps are pure re-layout / pure scheduling changes: the result must stay byte-identical to D1's. `dq` stays a scalar angle (NOT Box2D `b2Rot` cos/sin). No numerics/convergence change, no `kColorCount` change.
- **`FinalizePositionsSoA` stays SERIAL** (mutates the shared `DynamicTree` — the D1 bug fix). Never put it in a parallel stage.
- **Determinism:** fixed stage order = D1's exact order; within-color blocks are disjoint-write; atomics are control-flow only, never in FP math.
- **Build:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"` (VS2026 — NOT on PATH, NOT the VS2022 one). Build `Arcane.slnx` Debug + Release.
- **Tests run from the exe dir:** `cd Arcane\bin\<Config>-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<tags>"`.
- **clangd diagnostics are FALSE POSITIVES** (expects Clang 20). MSVC + the tests + the Loom smoke are truth.
- **STEPPROF measurement** (Tasks 1, 3, 6) is THROWAWAY: build with `ARCANE_STEPPROF=1`, add temporary Reset@180/Dump@480 + a serial toggle, run, record numbers in the task notes, then REVERT all instrumentation (do not commit it).

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/Solver/BodyState.hpp` | Create (replaces `BodyStateSoA.hpp`) | 32-byte AoS `BodyState` struct + `BodyStateStore` (aligned vector + Resize + Sync* declarations) |
| `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp` | Delete | superseded by `BodyState.hpp` |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` | Modify | swap the 6 per-field gathers for `GatherBodies`/`ScatterVel` (transpose); `-1` null sentinel in `PackLane`/`PadLane` |
| `Arcane/Core/src/Arcane/Physics/Solver/SolverStages.hpp` | Create | `StageType`/`SolverStage`/`SolverBlock` + `ExecuteBlock` dispatch + `SolverWorker` (main + thief) |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp` | Modify | `m_bodyState` type -> `BodyStateStore`; add `m_stages`/`m_blocks` reused scratch |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` | Modify | AoS sync defs + integrate loops + overflow path; `Solve()` builds the stage list + runs the region |
| `Arcane/Tests/src/SolverMtInvarianceTest.cpp` | Modify | add the `BodyState` alignment/size test; keep `RunPile` invariance (Gap-1 gate) |

---

## Task 1: Measure-first — re-confirm the D1 regression baseline

**Goal:** Record the current serial-vs-N `solve` ms/step at 10k so the rework is judged against a real number. THROWAWAY — no code commit.

**Files:** temporary edits to `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (or wherever `Step` lives) + the Sandbox world setup; all reverted.

- [ ] **Step 1: Add throwaway STEPPROF dump.** In `PhysicsWorld::Step`, add a static step counter; `StepProf::Reset()` at step 180, `StepProf::Dump("solve-baseline")` at step 480.
- [ ] **Step 2: Add a throwaway serial toggle.** In the Sandbox world setup (where `world.SetExecutor(ctx->taskExecutor)` is called), gate it: `if (std::getenv("ARCANE_SOLVER_SERIAL")) world.SetExecutor(nullptr); else world.SetExecutor(ctx->taskExecutor);`.
- [ ] **Step 3: Build Release with STEPPROF.** Run:
  `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane\Arcane.slnx /p:Configuration=Release /p:AdditionalOptions="/DARCANE_STEPPROF=1" /m`
  (or temporarily `#define ARCANE_STEPPROF 1` atop `StepProf.hpp`; revert after).
- [ ] **Step 4: Run N-thread.** `set ARCANE_SANDBOX_SCENE=8` then headless `Loom.exe --frames 500` (the perf scene at 10k). Record the `solve` ms/step line.
- [ ] **Step 5: Run serial.** `set ARCANE_SOLVER_SERIAL=1` and re-run. Record the serial `solve` ms/step.
- [ ] **Step 6: Record + revert.** Note both numbers (expect ~17 ms serial / ~23 ms N — the regression). Revert ALL Step-3..Step-1 edits. Confirm `git status` is clean. NO commit.

---

## Task 2: Gap 2.1 — migrate `BodyStateSoA` -> 32-byte AoS `BodyState` (naive gather)

**Goal:** Replace the six parallel `float` arrays with one aligned AoS array, keeping behavior byte-identical and the dummy-slot scheme intact (null-index is Task 3). Use a correct-but-naive per-lane gather so the suite stays green.

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Solver/BodyState.hpp`
- Delete: `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp`
- Modify: `SoftStep.hpp` (`m_bodyState` type + include), `SoftStep.cpp` (sync defs, integrate loops, overflow), `ContactConstraintSimd.hpp` (gather/scatter), `SolverMtInvarianceTest.cpp` (alignment test)

**Interfaces:**
- Produces:
  - `struct alignas(32) BodyState { float vx,vy,w, dpx,dpy,dq, pad0,pad1; };` (`sizeof==32`, `alignof==32`).
  - `class BodyStateStore` with: `void Resize(std::uint32_t n);` (assigns `n` zeroed rows; KEEP the `+1` dummy sizing at the call site in `Solve` for now), `BodyState* data() noexcept;` / `const BodyState* data() const noexcept;`, `std::size_t size() const noexcept;`, `BodyState& operator[](std::size_t);` / `const BodyState& operator[](std::size_t) const;`, and the four sync methods `SyncIn`/`SyncInCompacted`/`SyncOut`/`SyncOutCompacted` (declared here, defined in `SoftStep.cpp`).
  - In `ContactConstraintSimd.hpp`: `struct BodyStateW { Arcane::Simd::f32w vx,vy,w,dpx,dpy,dq; };`, `BodyStateW GatherBodies(const BodyState* states, Arcane::Simd::i32w idx) noexcept;`, `void ScatterVel(BodyState* states, Arcane::Simd::i32w idx, Arcane::Simd::f32w vx, Arcane::Simd::f32w vy, Arcane::Simd::f32w w, Arcane::Simd::b32w write) noexcept;`. Gap-2.1 implements these per-lane scalar (still correct).
- Consumes: existing `SimdSolve::*` math; `PhysicsWorld` awake/kinematic accessors (unchanged).

- [ ] **Step 1: Write the failing alignment test.** In `SolverMtInvarianceTest.cpp` add:

```cpp
#include <Arcane/Physics/Solver/BodyState.hpp>
TEST_CASE("BodyState is a 32-byte 32-aligned AoS row", "[physics][solvermt]")
{
    STATIC_REQUIRE(sizeof(Arcane::Physics::BodyState) == 32);
    STATIC_REQUIRE(alignof(Arcane::Physics::BodyState) == 32);
    Arcane::Physics::BodyStateStore s; s.Resize(33);
    REQUIRE(reinterpret_cast<std::uintptr_t>(s.data()) % 32u == 0u);  // aligned storage
}
```

- [ ] **Step 2: Run it to confirm it fails.** `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "[solvermt]"`. Expected: compile error (`BodyState` undefined) — the failing state.
- [ ] **Step 3: Create `BodyState.hpp`.** Define `BodyState` (the 8-float struct + the two `static_assert`s) and `BodyStateStore`. Storage: `std::vector<BodyState> m_states;` — MSVC's `std::allocator` honors 32-byte over-alignment for an over-aligned element type, so the default vector is fine; the test pins it. Port the four `SyncIn*`/`SyncOut*` DECLARATIONS verbatim from `BodyStateSoA.hpp` (same signatures). Copy the rich doc comments forward (keep the index-space explanation). Resize assigns `n` value-initialized `BodyState{}` (all zero).
- [ ] **Step 4: Port the sync definitions in `SoftStep.cpp`.** Rewrite `BodyStateSoA::SyncIn/SyncInCompacted/SyncOut/SyncOutCompacted` (lines ~92-203) as `BodyStateStore::...`, changing `vx[i]=..` -> `m_states[i].vx=..` (and `w[i]`->`.w`, `dpx[i]`->`.dpx`, etc.). Behavior identical (same predicates, same values, same dp/dq-zeroing).
- [ ] **Step 5: Port the three integrate loops in `SoftStep.cpp`.** In `IntegrateVelocitiesSoA`/`IntegratePositionsSoA`/`FinalizePositionsSoA` change `m_bodyState.vx[i]` -> `m_bodyState[i].vx` (etc.). Keep `FinalizePositionsSoA` SERIAL and the two ParallelFor integrate loops exactly as they are (still over `AwakeBodies()` with `kSolverBodyGrain`). Port the overflow path (`OverflowWarmStart/Solve/Restitution`, `DenseB`) the same way (`m_bodyState.vx[ob.ia]` -> `m_bodyState[ob.ia].vx`).
- [ ] **Step 6: Add the naive gather/scatter in `ContactConstraintSimd.hpp`.** Define `BodyStateW`, `GatherBodies`, `ScatterVel`. Naive impl: per-lane scalar into a stack `alignas(32) float tmp[kWidth]` then `load(tmp)`; scatter writes `states[idx[L]].vx = vlane` for each lane where `write` is set. Replace the 6 `GatherBody(bs.vx, ia)` + scatter calls in `SimdSolve::WarmStart/SolveNormalAndFriction/ApplyRestitution` with one `GatherBodies(states, ia)` per body + the `ScatterVel(states, ia, vAx, vAy, wA, allTrue)` / `ScatterVel(states, ib, vBx, vBy, wB, dyn)` pair. The `select(dyn, vBx, GatherBody(bs.vx, ib))` idiom collapses into `ScatterVel(..., write=dyn)` (skip-on-false). `GatherBody`/`ScatterBody` helpers are removed.
- [ ] **Step 7: Update includes + member type.** `SoftStep.hpp`: include `BodyState.hpp` (not `BodyStateSoA.hpp`), `m_bodyState` -> `BodyStateStore`. `ContactConstraintSimd.hpp`: include `BodyState.hpp`. Delete `BodyStateSoA.hpp`.
- [ ] **Step 8: Build Debug + Release.** `MSBuild ... Arcane.slnx /p:Configuration=Debug /m` then `Release`. Expected: clean compile (ignore clangd).
- [ ] **Step 9: Run the gate.** From the ArcaneTests exe dir (Debug AND Release): `.\ArcaneTests.exe "[physics],[determinism],[simd],[solvermt]"`. Expected: ALL PASS, byte-identical (the alignment test passes; the physics/simd/determinism counts match pre-Task-2). Also build ArcaneCore static-CRT clean.
- [ ] **Step 10: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/Solver/BodyState.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp Arcane/Tests/src/SolverMtInvarianceTest.cpp
git rm Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp
git commit -m "refactor(arcane/physics): BodyStateSoA -> 32-byte AoS BodyState (naive gather, byte-identical)"
```

---

## Task 3: Gap 2.2 — transpose gather + null-index branch (the single-thread win)

**Goal:** Replace the naive per-lane gather with aligned-load + AoS->SoA transpose, and the dummy slot with a `-1` null-index branch. Byte-identical; measure the single-thread `solve` win.

**Files:** Modify `ContactConstraintSimd.hpp` (transpose `GatherBodies`/`ScatterVel`, `-1` sentinel in `PackLane`/`PadLane`), `SoftStep.cpp` (`Solve` sizing `solverCount` not `+1`; overflow `DenseB` returns `-1`; overflow reads identity for `ib<0`), `SoftStep.hpp` (drop the dummy-slot comment).

**Interfaces:**
- Produces: `static constexpr std::int32_t kNullBodyIndex = -1;` (in `ContactConstraintSimd.hpp` or `Solver.hpp`). `GatherBodies`/`ScatterVel` signatures unchanged from Task 2 (impl swapped). `Build` drops the `dummyIndex` parameter (or repurposes it to `kNullBodyIndex`); a read-only B / padding lane packs `bodyIndexB = -1`.
- Consumes: `Arcane::Simd` ops (`load`, `iload`, plus whatever transpose primitives the wrapper exposes; if none, use backend `#if` with AVX2 intrinsics directly — see findings doc "b2GatherBodies").

- [ ] **Step 1: Write the failing lane-width invariance assertion (if not already covered).** Confirm `PhysicsSimdSolverTest.cpp` already bit-matches the forced-scalar oracle to the 8-wide path; if a dedicated null-index case is missing, add a test that a contact against a static B (now `-1`) produces the same body-A result as against a zero-velocity dynamic B. Run it; expect FAIL only if the sentinel isn't wired yet (else it documents the contract).
- [ ] **Step 2: Implement the transpose gather.** In `GatherBodies`, AVX2 path (width 8): per lane select `const BodyState* p = idx[L] >= 0 ? states + idx[L] : &kIdentityState;` (a `static constexpr BodyState kIdentityState{}`), eight `_mm256_load_ps((const float*)p_L)`, then the 8x8 `_mm256_unpacklo/hi_ps` + `_mm256_permute2f128_ps` transpose into `BodyStateW{vx,vy,w,dpx,dpy,dq}` (mirror Box2D `b2GatherBodies`). NEON (width 4): the 4-wide analog. Scalar (width 1): `BodyStateW{ f32w(p->vx), ... }` — the oracle. The transpose only reorders bits, so values are bit-identical to Task 2's naive gather.
- [ ] **Step 3: Implement the masked transpose scatter.** In `ScatterVel`, transpose `(vx,vy,w)` lanes back and store only lanes where `write` is set AND `idx[L] >= 0` (`-1` lanes never written). A always passes `write=all-true`; A is never `-1`.
- [ ] **Step 4: Thread the `-1` sentinel.** `PackLane`: a read-only B (static or span) sets `bodyIndexB = -1` (was `dummyIndex`); `dynB`/kinematic mapping unchanged (kinematic keeps its real dense row). `PadLane`: both indices `-1`. In `Solve`, size `m_bodyState.Resize(solverCount)` (drop the `+1`); remove `dummyIndex`. Overflow `DenseB`: return `-1` for static/span; `OverflowSetup` + the overflow passes read `vB=0` and skip the B write when `ib < 0`.
- [ ] **Step 5: Build Debug + Release.** As Task 2 Step 8.
- [ ] **Step 6: Run the gate.** `.\ArcaneTests.exe "[physics],[determinism],[simd],[solvermt]"` (Debug + Release): ALL PASS, byte-identical to Task 2; lane-width invariance holds; alignment test still green. ArcaneCore static-CRT clean.
- [ ] **Step 7: Measure the single-thread win (throwaway).** Repeat Task 1's STEPPROF procedure but SERIAL only (`ARCANE_SOLVER_SERIAL=1`, scene 8, 10k). Record the new serial `solve` ms; it must beat Task 1's serial baseline (the AoS+transpose win). Revert instrumentation. Record the delta in the commit/PR notes.
- [ ] **Step 8: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp Arcane/Tests/src/PhysicsSimdSolverTest.cpp
git commit -m "perf(arcane/physics): AoS transpose gather + null-index branch (single-thread solve win)"
```

---

## Task 4: Gap 1.1 — `SolverStages` unit + stage list, driven SERIALLY (main-only)

**Goal:** Replace the per-color `ParallelFor` blocks in `Solve` with a flat stage list executed by a single main worker through the Box2D `b2ExecuteMainStage` protocol (atomics present but uncontended). Proves the stage machinery reproduces D1's exact order byte-identically before any real threads.

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Solver/SolverStages.hpp`
- Modify: `SoftStep.hpp` (add `m_stages`/`m_blocks` scratch), `SoftStep.cpp` (`Solve` builds the stage list + runs one main worker), `SolverMtInvarianceTest.cpp` (small stage-coverage unit test)

**Interfaces:**
- Produces:
  - `enum class StageType : std::uint8_t { IntegrateVelocities, WarmStart, Solve, IntegratePositions, Relax, Restitution, StoreImpulses };`
  - `struct SolverBlock { int begin; int end; std::atomic<int> syncIndex; };`
  - `struct SolverStage { StageType type; int colorIndex; SolverBlock* blocks; int blockCount; std::atomic<int> completionCount; };`
  - A `SolverWorker(SolverStageContext& sc, std::uint32_t workerIndex)` free function: `workerIndex == 0` runs the main driver (Task 4); the thief branch is added in Task 5.
  - `struct SolverStageContext` bundling refs the block dispatch needs: `SoftStep* solver; SolverContext* ctx; SolverStage* stages; int stageCount; int substepCount; std::atomic<std::uint32_t> stageSync;` (+ whatever `ExecuteBlock` reads).
- Consumes: `SimdSolve::WarmStart/SolveNormalAndFriction/ApplyRestitution/StoreImpulses` range overloads (begin,end); the integrate-range bodies (factor the per-body loop bodies in `IntegrateVelocitiesSoA`/`IntegratePositionsSoA` into `[begin,end)` callables the stage can invoke); `OverflowWarmStart/Solve/Restitution` (main-serial); the joint bridge.

- [ ] **Step 1: Write the failing stage-coverage test.** In `SolverMtInvarianceTest.cpp`:

```cpp
#include <Arcane/Physics/Solver/SolverStages.hpp>
TEST_CASE("ExecuteStage visits every block exactly once (serial main)", "[physics][solvermt]")
{
    using namespace Arcane::Physics;
    std::array<SolverBlock,5> blk{};            // 5 blocks
    for (int i=0;i<5;++i){ blk[i].begin=i; blk[i].end=i+1; blk[i].syncIndex.store(0);}    
    SolverStage st{}; st.type=StageType::IntegrateVelocities; st.blocks=blk.data(); st.blockCount=5;
    std::array<int,5> hits{}; 
    ExecuteStageForTest(st, /*prevSync*/0, /*curSync*/1, [&](int b){ hits[b]++; });
    for (int h : hits) REQUIRE(h == 1);          // each block run once
    REQUIRE(st.completionCount.load() == 5);
}
```

(`ExecuteStageForTest` = a thin test-only shim exposing the ring-CAS claim loop with an injectable per-block callback, so the claim protocol is unit-tested in isolation.)

- [ ] **Step 2: Run it; confirm it fails.** `.\ArcaneTests.exe "[solvermt]"` — compile error (`SolverStages.hpp`/`ExecuteStageForTest` undefined).
- [ ] **Step 3: Create `SolverStages.hpp`.** Define the enum/structs above + `ExecuteStage(stage, prevSync, curSync, runBlock)` = the staggered ring-order CAS claim loop (`compare_exchange_strong(block.syncIndex, prevSync -> curSync)`; winner runs `runBlock(blockIndex)` then `completionCount.fetch_add(1, release)`), and `ExecuteStageForTest` wrapping it. Follow findings-doc "GAP 1" + spec Section 5.3 (Box2D `b2ExecuteStage`).
- [ ] **Step 4: Factor the integrate-range bodies.** In `SoftStep.cpp` extract the per-body loop bodies of `IntegrateVelocitiesSoA`/`IntegratePositionsSoA` into helpers callable as `[begin,end)` over `AwakeBodies()` (so a stage block can run a sub-range). Keep the existing public methods delegating to them (serial full-range) so nothing else breaks yet.
- [ ] **Step 5: Implement `ExecuteBlock` + the main driver.** In `SolverStages.hpp` add `ExecuteBlock(SolverStageContext&, const SolverStage&, int blockIndex)` = a `switch (stage.type)` dispatching to the right range call (`WarmStart`/`Solve(useBias=true)`/`Relax==Solve(useBias=false)`/`Restitution`/`StoreImpulses` on `solver->m_colorBatches[stage.colorIndex]` over the block's `[begin,end)`; `IntegrateVelocities`/`IntegratePositions` over the awake range). Add `ExecuteMainStage(sc, stage, syncBits)` = store `sc.stageSync = syncBits`, run `ExecuteStage(stage, prev, cur, runBlock)`, spin `while (stage.completionCount.load(acquire) != stage.blockCount) /*pause*/;`, reset `completionCount=0`. `SolverWorker(sc, 0)` = the main loop: for each substep, `ExecuteMainStage` over `{IntegrateVelocities, WarmStart[c0..], (overflow warm-start), [joints], Solve[c0..](bias), (overflow solve), IntegratePositions, [joints], Relax[c0..], (overflow relax)}`; then once `{Restitution[c0..], (overflow restitution), StoreImpulses[c0..]}`. Overflow + joints run main-serial inline (call the existing `Overflow*`/`SyncVelTo/FromWorld`+`SolveJoints`) between `ExecuteMainStage` calls.
- [ ] **Step 6: Build the stage list + wire `Solve`.** In `SoftStep.cpp` `Solve`, after Build batches, populate `m_stages`/`m_blocks` (reused scratch; resize-not-realloc): one stage per non-empty color for the colored types, one stage each for the body types, block partition per spec Section 5.2 (contacts `minBlockSize~=4` constraints, bodies `~=32`, target `~=4*workerCount` — use `workerCount=1` here). Replace the entire substep loop + restitution + store section (lines ~825-884) with a single `SolverWorker(sc, 0)` call. Keep Prepare/SyncIn/bucket/Build (pre) and `SyncOutCompacted`+`FinalizePositionsSoA` (post) exactly as they are.
- [ ] **Step 7: Build Debug + Release.** As before.
- [ ] **Step 8: Run the gate.** `.\ArcaneTests.exe "[physics],[determinism],[simd],[solvermt]"` (Debug + Release): ALL PASS byte-identical to Task 3 (the serial stage runner == the old per-color loop); the new stage-coverage test passes. ArcaneCore static-CRT clean; Loom smoke `Loom.exe --frames 180` exit 0.
- [ ] **Step 9: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/Solver/SolverStages.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp Arcane/Tests/src/SolverMtInvarianceTest.cpp
git commit -m "refactor(arcane/physics): SolverStages stage list + serial main-driver (replaces per-color ParallelFor, byte-identical)"
```

---

## Task 5: Gap 1.2 — the persistent MT region (main + thieves, ring-CAS stealing)

**Goal:** Turn on real multithreading: dispatch the whole substep loop ONCE via `ParallelFor(workerCount, 1, …)`, worker index = `begin`, `begin==0` = main, `begin>0` = thieves spinning on the stage-sync word. Byte-identical at any worker count.

**Files:** Modify `SolverStages.hpp` (add the thief branch to `SolverWorker`), `SoftStep.cpp` (`Solve` dispatches via `ParallelFor(workerCount,1,…)`; build blocks with the real `workerCount`).

**Interfaces:**
- Consumes: `ctx.executor->ParallelFor` (unchanged), `ctx.executor->WorkerCount()`. `SolverWorker(sc, workerIndex)` from Task 4 (extended).
- Produces: nothing new — same observable result, now parallel.

- [ ] **Step 1: Confirm the invariance test is the gate.** `RunPile` (500 boxes, 120 steps) already compares `serial == enki(1) == enki(N)` — at Task 4 all three ran 1 worker, so it was trivially equal. After this task enki(N) genuinely runs N workers. The test must STAY byte-identical. (No new test needed; this is the failing-then-passing target. Optionally bump `RunPile` to 200 steps for a longer stress.)
- [ ] **Step 2: Add the thief branch to `SolverWorker`.** For `workerIndex > 0`: spin reading `sc.stageSync` (`load(acquire)`); when it changes, decode `(syncIndex, stageIndex)`, run `ExecuteStage` for `sc.stages[stageIndex]` (claim+run blocks via CAS), record `lastSync`; loop until the main publishes a terminal sentinel stage value. The thief NEVER advances stages or runs overflow/joints (main-only). Follow spec Section 5.3 + findings-doc Box2D `b2SolverTask` thief path.
- [ ] **Step 3: Make the main publish a terminal sentinel.** After the last stage, `ExecuteMainStage` (or `SolverWorker(0)`) stores a terminal `stageSync` value that the thief loop checks to exit. Ensure no thief can deadlock: the main self-completes every block (claim-at-execution), so a thief that never runs is harmless.
- [ ] **Step 4: Dispatch the region in `Solve`.** Replace the `SolverWorker(sc, 0)` call with:

```cpp
const std::uint32_t workerCount = ctx.executor->WorkerCount();
ctx.executor->ParallelFor(workerCount, /*minBatch*/1,
    [&](std::size_t begin, std::size_t /*end*/, std::uint32_t /*enkiThread*/)
    { SolverWorker(sc, static_cast<std::uint32_t>(begin)); });
```

Rebuild blocks with the real `workerCount` target (`~=4*workerCount` blocks per stage, min sizes per Section 5.2). Reset every stage's `completionCount` and every block's `syncIndex` at stage-list build (per step).
- [ ] **Step 5: Build Debug + Release.** As before.
- [ ] **Step 6: Run the gate — the critical correctness proof.** `.\ArcaneTests.exe "[physics],[determinism],[simd],[solvermt]"` (Debug + Release). The `"solver thread-count invariance: serial == enki(1) == enki(N)"` case MUST pass byte-identical with `WorkerCount() > 1`. Run the full suite twice (run-twice determinism). ArcaneCore static-CRT clean; Loom smoke exit 0.
- [ ] **Step 7: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/Solver/SolverStages.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp Arcane/Tests/src/SolverMtInvarianceTest.cpp
git commit -m "feat(arcane/physics): persistent solver worker region (one dispatch/step, ring-CAS stealing, byte-identical MT)"
```

---

## Task 6: Gap 1.3 — measure-after + tune block sizing (the headline deliverable)

**Goal:** Prove the MT `solve` now beats serial at 10k. Tune the block-size constants. Record speedup + efficiency. If it does NOT beat serial, deliver the diagnosis (not a green check).

**Files:** possibly `SolverStages.hpp`/`SoftStep.cpp` (tuned `minBlockSize`/`blocksPerWorker` constants); throwaway STEPPROF instrumentation (reverted).

- [ ] **Step 1: Measure serial vs N (throwaway).** Repeat Task 1's STEPPROF procedure (scene 8, 10k, 300-step window): `ARCANE_SOLVER_SERIAL=1` for the baseline, unset for N threads. Record `solve` ms/step for each.
- [ ] **Step 2: Compute + record.** speedup = serial/N; efficiency = speedup / `WorkerCount()`. Compare N against BOTH the new serial (Task 3) and the D1 baseline (Task 1). The gate: N `solve` ms < new-serial `solve` ms (measurable speedup). State the numbers.
- [ ] **Step 3: Tune block sizing.** Sweep `minBlockSize` (contacts: try 4, 8; bodies: 32, 64) and `blocksPerWorker` (2, 4, 8); re-measure; keep the best. If small colors dominate (many sub-`width` colors), confirm tiny stages degrade gracefully (one block, main-only).
- [ ] **Step 4: Re-run the correctness gate after tuning.** `.\ArcaneTests.exe "[physics],[determinism],[simd],[solvermt]"` (Debug + Release) — still byte-identical (tuning changes block *sizes*, not the disjoint-write property). Revert all STEPPROF instrumentation; confirm `git status` shows only the (optional) tuned constants.
- [ ] **Step 5: Commit (if constants changed) + write the result.** Commit any tuned constants; record the final `solve` serial/N/speedup/efficiency in the commit body. If no speedup landed, instead commit nothing and write the diagnosis (grain? still gather/L3-bound? color sizes too small at 10k? residual sharing?) per spec Section 8.

```bash
git add Arcane/Core/src/Arcane/Physics/Solver/SolverStages.hpp
git commit -m "perf(arcane/physics): tune solver block sizing; solve-MT beats serial at 10k (Nx, E%)"
```

---

## Task 7: Final holistic review + branch finish

**Goal:** Review the whole branch diff, then hand off to the user for merge.

- [ ] **Step 1: Holistic review.** Read the full diff `2439a9c8..HEAD` (or `git diff main...HEAD`). Verify: no `FinalizePositionsSoA` parallelization; no numerics change beyond layout/scheduling; the `-1` sentinel is consistent across packer/overflow/scatter; atomics use correct acquire/release; the dummy slot is fully gone; no leftover STEPPROF instrumentation; ArcaneCore static-CRT clean.
- [ ] **Step 2: Full final gate.** Clean `-t:Rebuild` Debug + Release, both backends; `[gpu]` if on the GPU box; full `ArcaneTests` green; Loom smoke exit 0.
- [ ] **Step 3: Invoke `superpowers:finishing-a-development-branch`.** Present merge options; user merges to local main (FF) then pushes. Then write the COMPLETE memory + supersede `project_arcane_phaseD1_inprogress.md`.

---

## Self-Review

**Spec coverage:**
- §4 Gap 2 (AoS + transpose + null-index) -> Tasks 2 (layout) + 3 (transpose/null-index). ✓
- §5.1 reuse `ParallelFor`, worker=`begin`, `begin==0`=main -> Task 5 Step 4. ✓
- §5.2/5.3 `SolverStages` stage list + ring-CAS + main/thief -> Tasks 4 (serial main) + 5 (thieves). ✓
- §5.4 prepare stays serial pre-region -> Tasks 4/5 keep Prepare/Build pre-region. ✓
- §6 determinism (byte-identity at any worker count) -> Task 5 Step 6 invariance gate. ✓
- §7 dual gate per gap -> Task 3 Step 7 (Gap-2 perf), Task 5 Step 6 (Gap-1 correctness), Task 6 (Gap-1 perf). ✓
- §10 sequencing (measure-first -> 2.1 -> 2.2 -> 1.1 -> 1.2 -> 1.3 -> review) -> Tasks 1-7. ✓
- Keepers (executor seam, serial FinalizePositions, invariance test) -> untouched; called out in Global Constraints + Task 7. ✓

**Placeholder scan:** no TBD/TODO; tests show actual code; commands show expected output; the intricate intrinsic bodies (AVX2 transpose, ring-CAS, thief loop) are specified by structure + signatures + a precise pointer to the Box2D source map in the findings doc and the spec sections (the detailed design is committed there, not a placeholder).

**Type consistency:** `BodyState`/`BodyStateStore`, `GatherBodies`/`ScatterVel`/`BodyStateW`, `StageType`/`SolverBlock`/`SolverStage`/`SolverWorker`/`SolverStageContext`/`ExecuteStage`/`ExecuteBlock`/`ExecuteMainStage`, `kNullBodyIndex`, `m_stages`/`m_blocks` are used consistently across Tasks 2-6. Integrate-range helpers (Task 4 Step 4) are consumed by `ExecuteBlock` (Task 4 Step 5). ✓
