# Arcane Physics Phase D2 — Narrowphase Multithreading Implementation Plan

> **SUPERSEDED (2026-06-28):** the D2 target pivoted to broadphase pair-finding after measure-first found Phase-1 pair-finding (68.5%) dominates narrow, not the Collide recompute (21%). See `docs/superpowers/plans/2026-06-28-arcane-physics-phaseD2-broadphase-mt.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Multithread the narrowphase (`UpdateContacts`, the measured 38% / 18.4 ms stage) by parallelizing the per-pair `Collide()` manifold recompute via the existing executor + fork-join `ParallelFor`, byte-identical at any worker count.

**Architecture:** Split the single ascending-id Phase-2 pool pass into 2a (serial filter + work-list build), 2b (parallel `Collide()` recompute over the work-list, disjoint manifold writes + per-worker touch/event scratch), 2c (serial canonical merge of the per-worker scratch into the existing event/merge machinery). Box2D-v3 model: serial pair-mgmt -> parallel collide -> serial events/islands. Reuses the D1 `PhysicsWorld` executor — no new ABI.

**Tech Stack:** C++20, MSVC (VS2026), `Arcane::ITaskExecutor::ParallelFor`, Catch2 tests, STEPPROF (throwaway).

**Required reading for every task:**
- Spec: `docs/superpowers/specs/2026-06-28-arcane-physics-phaseD2-narrowphase-mt-design.md`
- Prior-art: `docs/superpowers/specs/2026-06-27-arcane-physics-solver-mt-scaling-rework-design.md` (the D1 executor seam + invariance-test pattern + the Amdahl/bandwidth lessons) and `docs/superpowers/research/2026-06-27-box2d-v3-solver-mt-findings.md`.

## Global Constraints

- **Branch:** work on `feature/arcane-physics-phaseD2-narrowphase-mt` (already created off `main`). NOT pushed until the user says so; user FF-merges to local main + pushes.
- **Byte-identity is the gate** for the restructure (Task 2) and the parallelization (Task 3): the result must stay byte-identical to the current single-pass Phase 2 (it's a pure reorder + a disjoint parallelization). No narrowphase numerics change; no event-semantics change; no coloring change.
- **Reuse the existing executor** (`PhysicsWorld`'s `SetExecutor`/executor member from D1) + plain fork-join `ParallelFor(count, grain, fn(begin,end,worker))`. NO new `ITaskExecutor`/JobSystem API. NOT the solver's persistent region.
- **These stay SERIAL, in canonical ascending order:** Phase 1 (broadphase `UpdatePairs` + `TryCreateContact` + coloring); pool insert/destroy (`EnsurePair`/`Destroy`); `ReleaseContactColor`/`AssignContactColor`; island merge apply (Phase 3); event derivation (`ContactManager`). The broadphase tree is read-only during 2b (do not mutate it there).
- **2b writes only each contact's own manifold** (disjoint) + appends to its own `m_touchScratch[worker]`. No shared-container writes in 2b.
- **No new compilation unit.** All changes are in existing `PhysicsWorld.cpp`/`.hpp` + a new test `.cpp` (the Tests project globs, but a new `.cpp` there needs the test project to pick it up — Tests already globs `src/**.cpp`, so a regen-free add; if the test doesn't get compiled, regenerate via the bundled premake, do NOT hand-edit vcxproj).
- **Build:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"` (VS2026 — NOT on PATH, NOT the VS2022 one). Build `Arcane.slnx` Debug + Release.
- **Tests from the exe dir:** `cd Arcane\bin\<Config>-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<tags>"`.
- **clangd diagnostics are FALSE POSITIVES** (expects Clang 20). MSVC + the tests + the Loom smoke are truth.
- **STEPPROF measurement** (Tasks 1, 4) is THROWAWAY: instrument, build, run headless Loom scene 8 (`ARCANE_SANDBOX_SCENE=8`), record numbers, REVERT. Serial baseline via `SetExecutor(nullptr)`.

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | Modify | restructure `UpdateContacts` Phase 2 -> 2a/2b/2c; wire `ParallelFor` + per-worker scratch in 2b; canonical merge in 2c |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | Modify | new reused scratch members: the recompute work-list + per-worker touch scratch + (if needed) helper decls |
| `Arcane/Tests/src/NarrowphaseMtInvarianceTest.cpp` | Create | thread-count-invariance gate (mirrors `SolverMtInvarianceTest.cpp`) |

---

## Task 1: Measure-first — confirm the `Collide()` recompute dominates `narrow`

**Goal:** Prove the per-pair `Collide()` recompute (not Phase-1 pair-finding/queries) is the dominant slice of the 18.4 ms `narrow` at 10k. THROWAWAY — no commit. If Phase-1 dominates, STOP and report — the plan redirects.

**Files:** temporary edits to `PhysicsWorld.cpp` (`UpdateContacts` + `Step`) + the Sandbox serial toggle; all reverted.

- [ ] **Step 1: Add throwaway sub-phase timers inside `UpdateContacts`.** Bracket, with `std::chrono::high_resolution_clock` accumulators (static, dumped from `Step`): (a) Phase 1 (create: `UpdatePairs` + the awake/static + kinematic create loops, ~lines 2337-2602), (b) the Phase-2 `Collide()` recompute calls specifically (~line 2721, accumulate only the `Collide(...)` call cost), (c) the rest of Phase 2 (reap/fat-box/warm-start/island-tracking), (d) Phase 3 merges (~2779). Print a `[NARROWPROF]` block from `Step` at step 480.
- [ ] **Step 2: Add the serial toggle + STEPPROF.** As in the D1 measurement: a throwaway `if (std::getenv("ARCANE_SOLVER_SERIAL")) world.SetExecutor(nullptr);` in the Sandbox world setup (grep `SetExecutor` under `Arcane/Sandbox/src`); a static frame counter in `Step` with Reset@180/Dump@480.
- [ ] **Step 3: Build Release + run.** Build with MSBuild (Release). `set ARCANE_SANDBOX_SCENE=8` then `Arcane\bin\Release-windows-x86_64-md\Loom\Loom.exe --frames 500`. Capture the `[NARROWPROF]` breakdown.
- [ ] **Step 4: Record + decide.** Note ms/step for (a)/(b)/(c)/(d) and the % of `narrow` each is. PROCEED only if (b) `Collide()` recompute is the dominant slice (the spec's gate). If Phase-1 (a) dominates, report that as the finding — the plan must redirect to Phase-1 query MT before continuing.
- [ ] **Step 5: Revert.** Remove all instrumentation; confirm `git status` clean (only pre-existing untracked files). NO commit.

---

## Task 2: Restructure Phase 2 into 2a/2b/2c (SERIAL, byte-identical)

**Goal:** Split the single ascending-id Phase-2 pass into a serial filter+work-list (2a), a serial recompute-over-work-list (2b, inline — no executor yet), and a serial merge (2c) — reproducing today's result byte-identically. This isolates the recompute so Task 3 can parallelize it with a minimal diff.

**Files:** Modify `PhysicsWorld.cpp` (`UpdateContacts`), `PhysicsWorld.hpp` (scratch members).

**Interfaces:**
- Produces (on `PhysicsWorld`, private, reused scratch):
  - `std::vector<std::uint32_t> m_recomputeList;` — contact ids needing a `Collide()` recompute (built in 2a, consumed in 2b).
  - `std::vector<std::uint8_t> m_wasTouching;` — per-`m_recomputeList`-entry snapshot of the contact's pre-recompute touch-state (parallel to `m_recomputeList`), captured in 2a, read in 2c. (Or fold into a small struct `struct RecomputeEntry { std::uint32_t id; std::uint8_t wasTouching; };` and use `std::vector<RecomputeEntry> m_recomputeList;` — pick one and use it consistently.)
  - A 2b body callable `RecomputeManifold(RecomputeEntry&)` or an inline loop body that does exactly today's recompute + warm-start for one contact (see Step 3).
- Consumes: today's Phase-2 internals (`Collide`, `FatBoxesOverlap`, `BothAsleep`, the warm-start point-id match, the island-merge-edge detection, `m_touchedEventPairs`, `m_pendingMerges`).

- [ ] **Step 1: Read the current Phase 2** (`PhysicsWorld.cpp` ~2604-2797) and inventory exactly what each per-contact iteration does: dead-handle reap (`ReleaseContactColor`+`Destroy`, ~2610), fat-box-separation reap (~2651), `BothAsleep` skip (~2691), `Collide` recompute (~2721), warm-start carry-forward (~2708-2776), island-merge-edge detection + `m_pendingMerges` queue (~2741-2757), and where `m_touchedEventPairs` is appended. Confirm the work that is per-contact-local (recompute + warm-start) vs the shared-mutation work (Destroy, queue appends).
- [ ] **Step 2: Implement 2a (serial filter + work-list).** Replace the single pass with a first ascending-id `m_contactPool.ForEach` that: does the dead-handle reap and the fat-box-separation reap (the `Destroy`s — unchanged, serial); for a survivor that is NOT `BothAsleep`, snapshot its current touch-state (manifold `pointCount > 0`) into the `RecomputeEntry` and `push_back` it onto `m_recomputeList` (cleared at the top of `UpdateContacts`). A `BothAsleep` survivor keeps its cached manifold and is NOT added (exactly today's skip).
- [ ] **Step 3: Implement 2b (serial recompute over the work-list).** Iterate `m_recomputeList` and, for each entry, run EXACTLY today's recompute for that contact: `c.manifold = Collide(m_fxShape[c.a.index], xfA, m_fxShape[c.b.index], xfB, margin);` then the warm-start point-id carry-forward (copy old impulses to matched new points). Write ONLY `c.manifold`. Do NOT do island/merge/event work here yet (that moves to 2c). Keep this a clearly-separable loop body — Task 3 wraps it in `ParallelFor`.
- [ ] **Step 4: Implement 2c (serial merge: events + island edges).** Iterate `m_recomputeList` ascending and, per contact: compute new touch-state (`c.manifold.pointCount > 0`); append to `m_touchedEventPairs` if touching (exactly today's event collection); compare new vs the snapshotted `wasTouching` and queue the same island-merge edge (false->true) / split candidate (true->false) into `m_pendingMerges` that today's code does. Then Phase 3 (sort `m_pendingMerges` + `MergeIslands`) is unchanged. The existing `std::sort`+dedup of `m_touchedEventPairs` stays.
- [ ] **Step 5: Build Debug + Release.** `MSBuild ... /p:Configuration=Debug /m` then `Release`.
- [ ] **Step 6: Run the gate.** From the ArcaneTests exe dir (Debug AND Release): `.\ArcaneTests.exe "[physics],[determinism],[simd]"`. Expected: ALL PASS, byte-identical to pre-Task-2 counts (pure reorder — the contact set, manifolds, events, merges, and post-step world state are unchanged). ArcaneCore static-CRT clean. Loom smoke `Loom.exe --frames 180` exit 0.
- [ ] **Step 7: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp
git commit -m "refactor(arcane/physics): split narrowphase Phase 2 into filter/recompute/merge (serial, byte-identical)"
```

---

## Task 3: Parallelize 2b + per-worker touch scratch + invariance gate

**Goal:** Run 2b's `Collide()` recompute via `ParallelFor`, collecting touch/event data into per-worker scratch (Box2D `b2TaskContext` model), merged canonically in 2c. Byte-identical at any worker count.

**Files:** Modify `PhysicsWorld.cpp` (`UpdateContacts` 2b/2c), `PhysicsWorld.hpp` (per-worker scratch); Create `Arcane/Tests/src/NarrowphaseMtInvarianceTest.cpp`.

**Interfaces:**
- Consumes: the executor (`Executor()` / the `m_executor` member from D1, always non-null); `ParallelFor(count, grain, fn(begin,end,worker))`; `m_recomputeList` from Task 2.
- Produces:
  - `std::vector<std::vector<TouchEntry>> m_touchScratch;` — per-worker scratch (outer index = worker in `[0,WorkerCount())`), reused across steps (resize to `WorkerCount()` when it grows; each inner cleared per step). `struct TouchEntry { std::uint32_t contactId; std::uint8_t nowTouching; std::uint8_t wasTouching; };` (carry whatever 2c needs to drive the event + merge logic).
  - `static constexpr std::size_t kNarrowphaseGrain = <tuned>;` (start ~64 contacts; tuned in Task 4).

- [ ] **Step 1: Write the failing invariance test.** Create `NarrowphaseMtInvarianceTest.cpp` mirroring `SolverMtInvarianceTest.cpp`'s structure. Use a scene that SUSTAINS an active recompute work-list larger than the grain (a settling pile drains to `BothAsleep` — instead keep contacts active: a large pile run for only ~40-60 steps while still settling/awake, OR continuously-colliding bodies). Assert post-step world state (positions/velocities) byte-identical across `SerialTaskExecutor`, `JobSystem(1)`, `JobSystem(0)`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
using namespace Arcane::Physics;
namespace {
    // ~800 dynamic boxes over a floor, run 50 steps while still settling so
    // contacts are AWAKE + recomputing every step (work-list > grain -> real MT).
    std::vector<float> RunActivePile(Arcane::ITaskExecutor* exec, int steps) {
        WorldDef wd; wd.gravityY = Real(400);
        PhysicsWorld w(wd); w.SetExecutor(exec);
        { BodyDef fd; fd.type=BodyType::Static; fd.position=Vec2(Real(0),Real(5));
          fd.shape=MakeAabb(Real(40),Real(0.5)); w.AddBody(fd); }
        std::vector<BodyHandle> bodies; bodies.reserve(800);
        for (int i=0;i<800;++i){ int c=i%20, r=i/20;
          BodyDef bd; bd.type=BodyType::Dynamic;
          bd.position=Vec2(static_cast<Real>(c-10)*Real(1.0), Real(-1)-static_cast<Real>(r)*Real(1.2));
          bd.shape=MakeAabb(Real(0.45),Real(0.45)); bd.density=Real(1); bd.friction=Real(0.3); bd.fixedRotation=true;
          bodies.push_back(w.AddBody(bd)); }
        for (int s=0;s<steps;++s) w.Step(Real(1)/Real(60));
        std::vector<float> out; out.reserve(bodies.size()*5u);
        for (auto h:bodies){ const Vec2 p=w.Position(h); const Vec2 v=w.Velocity(h);
          out.push_back((float)p.x); out.push_back((float)p.y); out.push_back((float)w.GetAngle(h));
          out.push_back((float)v.x); out.push_back((float)v.y);} return out;
    }
}
TEST_CASE("narrowphase thread-count invariance: serial == enki(1) == enki(N)", "[physics][determinism][narrowmt]") {
    Arcane::SerialTaskExecutor serial; Arcane::JobSystem one(1); Arcane::JobSystem many(0);
    const auto a=RunActivePile(&serial,50);
    const auto b=RunActivePile(one.TaskExecutor(),50);
    const auto c=RunActivePile(many.TaskExecutor(),50);
    INFO("workers=" << many.TaskExecutor()->WorkerCount());
    if (many.TaskExecutor()->WorkerCount() <= 1u) WARN("single worker: narrowphase MT path not exercised this run");
    REQUIRE(a.size()==b.size()); REQUIRE(a==b); REQUIRE(a==c);
}
```

- [ ] **Step 2: Run it to confirm it passes serially (pre-parallelization baseline).** `.\ArcaneTests.exe "[narrowmt]"` — with Task-2's serial 2b, all three executors run the same serial path, so it PASSES trivially. (This pins the byte-identity baseline before parallelization; the real test is that it STILL passes after Step 3 makes enki(N) genuinely parallel.)
- [ ] **Step 3: Parallelize 2b.** Replace the serial 2b loop with:

```cpp
auto* exec = Executor(); // always non-null (D1 serial default)
m_touchScratch.resize(exec->WorkerCount());
for (auto& s : m_touchScratch) s.clear();
exec->ParallelFor(m_recomputeList.size(), kNarrowphaseGrain,
    [&](std::size_t begin, std::size_t end, std::uint32_t worker) {
        auto& scratch = m_touchScratch[worker];
        for (std::size_t k = begin; k < end; ++k) {
            const RecomputeEntry e = m_recomputeList[k];
            Contact& c = /* pool ref by e.id */;
            // EXACTLY Task-2's recompute body: Collide -> c.manifold; warm-start carry-forward.
            const bool nowTouching = (c.manifold.pointCount > 0);
            scratch.push_back(TouchEntry{ e.id, (std::uint8_t)nowTouching, e.wasTouching });
        }
    });
```

Each task writes only its own `Contact`'s manifold (disjoint) + its own `m_touchScratch[worker]`. No shared writes. The broadphase tree is read-only here.
- [ ] **Step 4: Rewrite 2c to merge the per-worker scratch canonically.** Concatenate `m_touchScratch[0..W)` into one list, sort by `contactId` (canonical) — OR feed each entry into the EXISTING `m_touchedEventPairs` append + `m_pendingMerges` queue and rely on the existing `std::sort`+dedup of `m_touchedEventPairs` and Phase-3's `std::sort` of `m_pendingMerges` for canonicalization. Drive the same event-collection (touching -> event pair) and island-edge logic (nowTouching vs wasTouching) Task 2 put in 2c, now sourced from the merged scratch. The downstream sorts make the result worker-count-invariant.
- [ ] **Step 5: Build Debug + Release.**
- [ ] **Step 6: Run the gate — the correctness proof.** `.\ArcaneTests.exe "[physics],[determinism],[simd],[narrowmt]"` (Debug AND Release). The `narrowphase thread-count invariance` case MUST pass byte-identical with `WorkerCount() > 1`. Run the full filtered suite twice (run-twice determinism). ArcaneCore static-CRT clean; Loom smoke exit 0.
- [ ] **Step 7: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Tests/src/NarrowphaseMtInvarianceTest.cpp
git commit -m "feat(arcane/physics): parallel narrowphase Collide recompute (per-worker touch scratch, byte-identical MT)"
```

---

## Task 4: Measure-after + tune grain (the deliverable)

**Goal:** Prove the parallel `narrow` beats serial at 10k, and tune `kNarrowphaseGrain`. Record speedup + efficiency. If it doesn't scale, deliver the diagnosis.

**Files:** possibly `PhysicsWorld.cpp` (tuned `kNarrowphaseGrain`); throwaway STEPPROF (reverted).

- [ ] **Step 1: Measure serial vs N (throwaway).** STEPPROF the `narrow` stage (the existing `Narrowphase` scope) + the 2b sub-scope at 10k scene 8: `ARCANE_SOLVER_SERIAL=1` for serial, unset for N threads; `Loom.exe --frames 500`, steps 180-480 window. Record `narrow` ms/step both ways.
- [ ] **Step 2: Compute + record.** speedup = serial/N; efficiency = speedup / `WorkerCount()`. The gate: N `narrow` < serial `narrow` (measurable speedup). Compute-bound -> expect meaningful scaling (the D2 thesis vs D1's bandwidth-bound 0.97x). State the numbers + the serial-fraction (2a+2c) as a fraction of `narrow`.
- [ ] **Step 3: Tune the grain.** Sweep `kNarrowphaseGrain` (e.g., 16/32/64/128); re-measure N-thread `narrow`; keep the best. Confirm tiny work-lists degrade gracefully (one chunk, main-only).
- [ ] **Step 4: Re-run the correctness gate after tuning.** `.\ArcaneTests.exe "[physics],[determinism],[simd],[narrowmt]"` Debug + Release — still byte-identical (grain changes partition sizes, not the disjoint-write property). Revert all STEPPROF instrumentation; confirm `git status` shows only the (optional) tuned constant.
- [ ] **Step 5: Commit (if constants changed) + write the result.** Commit the tuned grain with the serial/N/speedup/efficiency in the body. If no speedup landed, commit nothing and write the diagnosis (work-list too small? shape-vert/transform reads memory-bound? serial 2a/2c fraction dominating?) per spec §8.

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit -m "perf(arcane/physics): tune narrowphase grain; narrow-MT beats serial at 10k (Nx, E%)"
```

---

## Task 5: Final holistic review + branch finish

- [ ] **Step 1: Holistic review.** Read the full branch diff (`git diff main...HEAD`). Verify: 2b writes only per-contact manifold + per-worker scratch (no shared write); broadphase tree not mutated in 2b; all serial seams (create/destroy/color/merge/events) intact + canonical; per-worker scratch reused (no per-step alloc); no leftover STEPPROF instrumentation; event/merge semantics unchanged; ArcaneCore static-CRT clean.
- [ ] **Step 2: Full final gate.** Clean `-t:Rebuild` Debug + Release, both backends; full `ArcaneTests` green; `[gpu]` if on the GPU box; Loom smoke exit 0.
- [ ] **Step 3: Invoke `superpowers:finishing-a-development-branch`.** Present merge options; user FF-merges to local main + pushes. Then write the COMPLETE memory.

---

## Self-Review

**Spec coverage:**
- §3 2a/2b/2c restructure -> Task 2 (serial) + Task 3 (parallel). ✓
- §3/§4 reuse executor + `ParallelFor`, no new ABI -> Task 3 Step 3. ✓
- §3/§5 per-worker touch scratch (Box2D model) + canonical 2c merge -> Task 3 Steps 3-4. ✓
- §3 serial seams (create/destroy/color/merge/events) untouched -> Global Constraints + Task 2 Steps 2/4. ✓
- §5/§7 determinism (byte-identical at any worker count) -> Task 3 Step 6 invariance gate + run-twice. ✓
- §7 measure-first (confirm recompute dominates) -> Task 1. ✓
- §7 measure-after (the deliverable) -> Task 4. ✓
- §7 build gates -> Tasks 2/3/5. ✓
- §9 OUT-of-scope (Phase-1 query MT, D3, numerics, ABI) -> not in any task; Global Constraints forbid them. ✓

**Placeholder scan:** the recompute body in Tasks 2/3 references "EXACTLY today's recompute" + the line numbers rather than transcribing the warm-start point-match loop verbatim — the implementer copies it from the existing code (it is committed, not a placeholder); the invariance test code is complete; commands show expected output. The `Contact& c = /* pool ref by e.id */` in Task 3 Step 3 is resolved by Task 2 (the pool access pattern the implementer used in 2b serial).

**Type consistency:** `m_recomputeList`/`RecomputeEntry{id,wasTouching}`, `m_touchScratch`/`TouchEntry{contactId,nowTouching,wasTouching}`, `kNarrowphaseGrain`, `Executor()`/`WorkerCount()`/`ParallelFor` are used consistently across Tasks 2-4. The `[narrowmt]` tag is consistent. ✓
