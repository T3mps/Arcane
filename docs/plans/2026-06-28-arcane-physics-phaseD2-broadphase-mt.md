# Arcane Physics Phase D2 — Broadphase Pair-Finding MT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Multithread `DynamicTree::UpdatePairs` (the measured ~71% of narrowphase Phase 1) by parallelizing its per-moved-proxy read-only tree descents, byte-identical at any worker count.

**Architecture:** Control inversion — split `UpdatePairs` into three `IBroadphase` seams (serial evict+collect-moved / read-only per-proxy query / serial merge+emit); `PhysicsWorld` (which owns the executor) drives `ParallelFor` over the moved proxies with per-worker stacks + key buffers. `DynamicTree` stays Jobs-free. Reuses the D1 executor — no new ABI. Box2D `b2FindPairsTask` model.

**Tech Stack:** C++20, MSVC (VS2026), `Arcane::ITaskExecutor::ParallelFor`, `std::span`, `std::unordered_set`, Catch2, STEPPROF (throwaway).

**Required reading for every task:**
- Spec: `docs/superpowers/specs/2026-06-28-arcane-physics-phaseD2-broadphase-mt-design.md`
- Prior-art: `docs/superpowers/specs/2026-06-27-arcane-physics-solver-mt-scaling-rework-design.md` (D1 executor seam + invariance pattern + the Amdahl/"measure-don't-assume" lessons).

## Global Constraints

- **Branch:** `feature/arcane-physics-phaseD2-narrowphase-mt` (the D2 branch; spec pivoted to broadphase). NOT pushed until the user says so; user FF-merges to local main + pushes.
- **Byte-identity is the gate** for the refactor (Task 2) and the parallelization (Task 3): `UpdatePairs` output, the `m_pairSet` contents, and post-step world state must stay byte-identical to today's single-method `UpdatePairs`. The existing oracle invariant `UpdatePairs() == Pairs() == brute-force` MUST stay green.
- **Control inversion — NO executor param on `DynamicTree`; NO Jobs include in the broadphase.** `PhysicsWorld` drives `ParallelFor`; `DynamicTree` exposes read-only `QueryProxyPairs` + serial `EvictTouchedAndCollectMoved`/`MergeAndEmit`.
- **Reuse the existing executor** (`PhysicsWorld::Executor()`/`SetExecutor` from D1) + plain fork-join `ParallelFor(count, grain, fn(begin,end,worker))`. NO new `ITaskExecutor`/JobSystem API. NOT the persistent region.
- **`QueryProxyPairs` is read-only on the tree** (descends `m_nodes`/`m_root`) with a CALLER-PROVIDED descent stack — no shared `m_stack`, no `m_pairSet` write. The merge into `m_pairSet`, STEP-1 evict, and STEP-3 emit/sort stay SERIAL.
- **`UpdatePairs(out)` is RETAINED as a serial wrapper** calling the three seams — tests, the oracle, and non-DynamicTree strategies use it unchanged.
- **Per-worker scratch reused** (resize-to-`WorkerCount`, clear per step) — zero steady-state alloc.
- **No new compilation unit** beyond the new test `.cpp` (Tests globs `src/**.cpp`; if not picked up, regenerate via bundled premake — do NOT hand-edit vcxproj).
- **Build:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"` (VS2026 — NOT on PATH, NOT the VS2022 one). `Arcane.slnx` Debug + Release.
- **Tests from the exe dir:** `cd Arcane\bin\<Config>-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<tags>"`.
- **clangd diagnostics are FALSE POSITIVES** (expects Clang 20). MSVC + tests + Loom smoke are truth.
- **STEPPROF measurement** (Tasks 1, 4) is THROWAWAY: instrument, build, run headless Loom scene 8 (`ARCANE_SANDBOX_SCENE=8`), record, REVERT. Serial baseline via `SetExecutor(nullptr)`.

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/Broadphase/Broadphase.hpp` | Modify | add 3 `IBroadphase` virtuals (seams) with serial-fallback defaults |
| `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.{hpp,cpp}` | Modify | split `UpdatePairs` into the 3 seams + serial wrapper; per-worker-stack query |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.{hpp,cpp}` | Modify | orchestrate the 3 seams via `ParallelFor` + per-worker scratch in `UpdateContacts` |
| `Arcane/Tests/src/BroadphaseMtInvarianceTest.cpp` | Create | thread-count-invariance gate |

---

## Task 1: Measure-first — confirm STEP 2 (descents) dominates `UpdatePairs`

**Goal:** Prove the per-proxy tree descents (STEP 2), not the O(pairs) serial set-churn (STEP 1 evict + STEP 3 emit/sort), dominate the 10.28 ms `UpdatePairs`. THROWAWAY — no commit. If the set-churn dominates, STOP and report — the fix is algorithmic, not MT.

**Files:** temporary edits to `DynamicTree.cpp` (`UpdatePairs`) + `PhysicsWorld.cpp` (`Step` dump) + Sandbox serial toggle; all reverted.

- [ ] **Step 1: Bracket `UpdatePairs`'s three steps with chrono accumulators.** In `DynamicTree::UpdatePairs` (cpp ~365-458): time (a) STEP 1 evict (~371-388), (b) STEP 2 the per-moved-proxy descent loop (~395-440), (c) STEP 3 emit+sort (~448-457). Expose the accumulators (static) + a dump method; print a `[UPDATEPAIRSPROF]` block from `PhysicsWorld::Step` at step 480 (reset at 180). Also count: |m_moved|, |m_pairSet|, total nodes visited in STEP 2 if cheap to add.
- [ ] **Step 2: Build Release + run.** `MSBuild ... /p:Configuration=Release /m`; `set ARCANE_SANDBOX_SCENE=8`; `Arcane\bin\Release-windows-x86_64-md\Loom\Loom.exe --frames 500`. Capture the breakdown.
- [ ] **Step 3: Record + decide.** Note (a)/(b)/(c) ms + % of `UpdatePairs`. PROCEED only if (b) STEP 2 descents dominate. If (a)+(c) set-churn dominates, report it — the plan redirects to an algorithmic pair-set fix (not MT). Do NOT fabricate; if Loom won't run, report BLOCKED.
- [ ] **Step 4: Revert.** Remove all instrumentation; confirm `git status` clean (only pre-existing untracked files). NO commit.

---

## Task 2: Extract the 3 seams + serial `UpdatePairs` wrapper (byte-identical refactor)

**Goal:** Split `DynamicTree::UpdatePairs` into `EvictTouchedAndCollectMoved` / `QueryProxyPairs` / `MergeAndEmit`, reimplement `UpdatePairs(out)` as a serial wrapper calling them, and add the seams to `IBroadphase` with serial-fallback defaults. Pure refactor — byte-identical, oracle stays green. No threading yet.

**Files:** Modify `Broadphase.hpp`, `DynamicTree.hpp`, `DynamicTree.cpp`.

**Interfaces:**
- Produces (on `IBroadphase`, virtual; `DynamicTree` overrides):
  - `virtual void EvictTouchedAndCollectMoved(std::vector<std::uint32_t>& movedOut);` — base default: `movedOut.clear();` (no incremental support → no parallel work).
  - `virtual void QueryProxyPairs(std::uint32_t id, std::vector<std::uint32_t>& stack, std::vector<std::uint64_t>& out) const;` — base default: `(void)id;(void)stack;(void)out;` (no-op).
  - `virtual int MergeAndEmit(std::span<const std::vector<std::uint64_t>> perWorker, std::vector<BroadphasePair>& out);` — base default: `(void)perWorker; return Pairs(out);` (serial full recompute fallback).
  - `int UpdatePairs(std::vector<BroadphasePair>& out) override;` on `DynamicTree` becomes the serial wrapper.
- Consumes: existing `DynamicTree` internals (`m_pairSet`, `m_moved`, `m_removed`, `m_toErase`, `m_stack`, `m_nodes`, `m_root`, `LeafOf`, `FatOverlap`, `AabbOverlap`).

- [ ] **Step 1: Add `#include <span>` to Broadphase.hpp; declare the 3 virtuals** on `IBroadphase` with the defaults above (inline in the header, like the existing `UpdatePairs` default at Broadphase.hpp:125).
- [ ] **Step 2: Declare the 3 overrides on `DynamicTree`** (DynamicTree.hpp, public, next to `UpdatePairs`). Add a reused member buffer for the serial wrapper: `std::vector<std::uint64_t> m_findSerial;`.
- [ ] **Step 3: Implement `EvictTouchedAndCollectMoved` (DynamicTree.cpp).** Move today's STEP 1 evict (UpdatePairs ~371-388) here verbatim, then `movedOut.clear(); movedOut.reserve(m_moved.size()); for (auto id : m_moved) movedOut.push_back(id);` then `m_moved.clear(); m_removed.clear();`. (The snapshot+clear matches today's STEP 2-then-clear ordering — STEP 2 reads m_moved before the clear; here the caller reads `movedOut` instead.)
- [ ] **Step 4: Implement `QueryProxyPairs` (DynamicTree.cpp).** The STEP 2 inner body for one proxy, using the caller's `stack` (not `m_stack`): `const std::uint32_t leafA = LeafOf(id); if (leafA == kNull) return;` then the fat-descent over `stack` (push m_root; pop; FatOverlap(n.fat, fatA) cull; push children; leaf: `n.id != id && AabbOverlap(n.tight, tightA)` → compute `lo/hi` + `key` → `out.push_back(key)`). Read-only (const). Identical math to today's ~405-439.
- [ ] **Step 5: Implement `MergeAndEmit` (DynamicTree.cpp).** `for (const auto& buf : perWorker) for (std::uint64_t key : buf) m_pairSet.insert(key);` then today's STEP 3 emit+sort (~448-457) into `out`. Return `out.size()`.
- [ ] **Step 6: Reimplement `UpdatePairs(out)` as the serial wrapper.** `EvictTouchedAndCollectMoved(m_movedSerial);` (add a reused member `std::vector<std::uint32_t> m_movedSerial;`) → `m_findSerial.clear(); for (std::uint32_t id : m_movedSerial) QueryProxyPairs(id, m_stack, m_findSerial);` → `std::array<std::vector<std::uint64_t>,1>`-style span OR `std::span<const std::vector<std::uint64_t>>(&m_findSerial, 1)` → `return MergeAndEmit(that, out);`. This reproduces the original exactly.
- [ ] **Step 7: Build Debug + Release.** MSBuild both configs.
- [ ] **Step 8: Run the gate.** From the ArcaneTests exe dir (Debug AND Release): `.\ArcaneTests.exe "[physics],[determinism],[broadphase]"` (find the broadphase oracle test's tag — the `UpdatePairs == Pairs == brute-force` test; if its tag differs, run the file that holds it). Expected: ALL PASS, byte-identical (pure refactor — the oracle proves `UpdatePairs` output unchanged). ArcaneCore static-CRT clean. Loom smoke `Loom.exe --frames 180` exit 0.
- [ ] **Step 9: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/Broadphase/Broadphase.hpp Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.cpp
git commit -m "refactor(arcane/physics): split DynamicTree::UpdatePairs into evict/query/merge seams (serial wrapper, byte-identical)"
```

---

## Task 3: Parallelize in PhysicsWorld + invariance gate

**Goal:** Drive `QueryProxyPairs` via `ParallelFor` over the moved proxies with per-worker stacks + key buffers, merged serially. Byte-identical at any worker count.

**Files:** Modify `PhysicsWorld.hpp` (scratch members), `PhysicsWorld.cpp` (`UpdateContacts`); Create `Arcane/Tests/src/BroadphaseMtInvarianceTest.cpp`.

**Interfaces:**
- Consumes: the D1 executor (`Executor()`, always non-null); `ParallelFor(count, grain, fn(begin,end,worker))`; the Task-2 seams (`EvictTouchedAndCollectMoved`/`QueryProxyPairs`/`MergeAndEmit`) on `m_fixtureBroadphase` (an `IBroadphase*`).
- Produces (on `PhysicsWorld`, private, reused scratch):
  - `std::vector<std::uint32_t> m_bpMovedScratch;`
  - `std::vector<std::vector<std::uint64_t>> m_bpFindScratch;` (per-worker key buffers)
  - `std::vector<std::vector<std::uint32_t>> m_bpStackScratch;` (per-worker descent stacks)
  - `static constexpr std::size_t kBroadphaseGrain = <tuned>;` (start 64; tuned in Task 4).

- [ ] **Step 1: Write the failing invariance test.** Create `BroadphaseMtInvarianceTest.cpp` mirroring the active-pile pattern (a moving scene so the broadphase re-queries many proxies > grain). Assert post-step world state byte-identical across `SerialTaskExecutor`, `JobSystem(1)`, `JobSystem(0)`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
using namespace Arcane::Physics;
namespace {
    // ~800 dynamic boxes over a floor, 50 steps while settling -> many moving
    // proxies/step (UpdatePairs work-list > grain -> real broadphase MT).
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
TEST_CASE("broadphase thread-count invariance: serial == enki(1) == enki(N)", "[physics][determinism][broadmt]") {
    Arcane::SerialTaskExecutor serial; Arcane::JobSystem one(1); Arcane::JobSystem many(0);
    const auto a=RunActivePile(&serial,50);
    const auto b=RunActivePile(one.TaskExecutor(),50);
    const auto c=RunActivePile(many.TaskExecutor(),50);
    INFO("workers=" << many.TaskExecutor()->WorkerCount());
    if (many.TaskExecutor()->WorkerCount() <= 1u) WARN("single worker: broadphase MT path not exercised this run");
    REQUIRE(a.size()==b.size()); REQUIRE(a==b); REQUIRE(a==c);
}
```

- [ ] **Step 2: Run it to confirm it passes pre-parallelization.** `.\ArcaneTests.exe "[broadmt]"` — with Task-2's serial path (PhysicsWorld still calls `UpdatePairs`), all three executors run the same serial path → PASSES trivially. Pins the baseline before parallelization.
- [ ] **Step 3: Add the scratch members** to `PhysicsWorld.hpp` (the three above + `kBroadphaseGrain`).
- [ ] **Step 4: Replace the `UpdatePairs` call with the orchestration** in `UpdateContacts` (find `m_fixtureBroadphase->UpdatePairs(m_cpPairs)` ~2349):

```cpp
auto* bp = m_fixtureBroadphase.get(); // IBroadphase*
auto* exec = Executor();              // D1 member, always non-null
bp->EvictTouchedAndCollectMoved(m_bpMovedScratch);
const std::uint32_t W = exec->WorkerCount();
if (m_bpFindScratch.size() < W) m_bpFindScratch.resize(W);
if (m_bpStackScratch.size() < W) m_bpStackScratch.resize(W);
for (auto& s : m_bpFindScratch) s.clear();
exec->ParallelFor(m_bpMovedScratch.size(), kBroadphaseGrain,
    [&](std::size_t b, std::size_t e, std::uint32_t w) {
        for (std::size_t k = b; k < e; ++k)
            bp->QueryProxyPairs(m_bpMovedScratch[k], m_bpStackScratch[w], m_bpFindScratch[w]);
    });
bp->MergeAndEmit(std::span<const std::vector<std::uint64_t>>(m_bpFindScratch.data(), W), m_cpPairs);
```

Each worker descends read-only with its own stack + key buffer (disjoint). The serial `UpdatePairs(out)` wrapper remains for tests/oracle.
- [ ] **Step 5: Build Debug + Release.**
- [ ] **Step 6: Run the gate — the correctness proof.** `.\ArcaneTests.exe "[physics],[determinism],[broadphase],[broadmt]"` (Debug AND Release). The `broadphase thread-count invariance` case MUST pass byte-identical with `WorkerCount() > 1`; the oracle (`UpdatePairs == Pairs == brute-force`) stays green. Run the full filtered suite twice (run-twice determinism). ArcaneCore static-CRT clean; Loom smoke exit 0.
- [ ] **Step 7: Commit.**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Tests/src/BroadphaseMtInvarianceTest.cpp
git commit -m "feat(arcane/physics): parallel broadphase pair-finding (PhysicsWorld-driven, per-worker scratch, byte-identical MT)"
```

---

## Task 4: Measure-after + tune grain (the deliverable)

**Goal:** Prove parallel `UpdatePairs`/`narrow` beats serial at 10k; tune `kBroadphaseGrain`. Record speedup + efficiency + serial-fraction. If it doesn't scale, deliver the diagnosis.

**Files:** possibly `PhysicsWorld.cpp` (tuned `kBroadphaseGrain`); throwaway STEPPROF (reverted).

- [ ] **Step 1: Measure serial vs N (throwaway).** STEPPROF the `narrow` stage + a sub-timer around the `UpdatePairs` orchestration at 10k scene 8: `ARCANE_SOLVER_SERIAL=1` for serial, unset for N; `Loom.exe --frames 500`, steps 180-480. Record `UpdatePairs`/`narrow` ms/step both ways.
- [ ] **Step 2: Compute + record.** speedup = serial/N; efficiency = speedup / `WorkerCount()`. Gate: N `UpdatePairs` < serial. State the serial-fraction (evict + merge + emit/sort) as a fraction of `UpdatePairs` (the Amdahl ceiling). Honest note: tree descents are pointer-chasing → may be memory-latency-bound; the measurement settles whether it scales (D1 lesson).
- [ ] **Step 3: Tune the grain.** Sweep `kBroadphaseGrain` (16/32/64/128); re-measure; keep the best. Confirm small moved-lists degrade gracefully (one chunk, main-only).
- [ ] **Step 4: Re-run the correctness gate after tuning.** `.\ArcaneTests.exe "[physics],[determinism],[broadphase],[broadmt]"` Debug + Release — still byte-identical (grain changes partition sizes, not the set/sort result). Revert STEPPROF; confirm `git status` shows only the (optional) tuned constant.
- [ ] **Step 5: Commit (if changed) + write the result.** Commit the tuned grain with serial/N/speedup/efficiency/serial-fraction in the body. If no speedup, commit nothing and write the diagnosis (set-churn serial ceiling? pointer-chasing memory-bound? grain?) per spec §8.

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit -m "perf(arcane/physics): tune broadphase grain; UpdatePairs-MT beats serial at 10k (Nx, E%)"
```

---

## Task 5: Final holistic review + branch finish

- [ ] **Step 1: Holistic review.** Read the full branch diff vs `main`. Verify: `QueryProxyPairs` read-only (no `m_pairSet`/`m_stack` write, caller stacks); evict/merge/emit serial; `DynamicTree` Jobs-free (no executor include/param); the serial `UpdatePairs` wrapper + oracle intact; per-worker scratch reused (no per-step alloc); base `IBroadphase` defaults degrade non-tree strategies to serial `Pairs`; no leftover STEPPROF; ArcaneCore static-CRT clean.
- [ ] **Step 2: Full final gate.** Clean `-t:Rebuild` Debug + Release, both backends; full `ArcaneTests` green; `[gpu]` if on the GPU box; Loom smoke exit 0.
- [ ] **Step 3: Invoke `superpowers:finishing-a-development-branch`.** Present merge options; user FF-merges to local main + pushes. Then write the COMPLETE memory.

---

## Self-Review

**Spec coverage:**
- §3 control inversion (3 seams + serial wrapper, no param) → Task 2. ✓
- §3 PhysicsWorld-driven ParallelFor + per-worker scratch → Task 3. ✓
- §3/§4 reuse executor, Box2D b2FindPairsTask shape → Task 3 Step 4. ✓
- §5 determinism (byte-identical at any worker count) → Task 3 Step 6 invariance + oracle + run-twice. ✓
- §7 measure-first (STEP 2 dominates the set-churn) → Task 1. ✓
- §7 measure-after (deliverable) → Task 4. ✓
- §7 oracle gate retained → Tasks 2/3 Step 8/6. ✓
- §9 OUT (StaticCandidates MT, Collide-recompute MT, algorithmic pair-set, D3) → not in any task; Global Constraints scope it out. ✓
- §3 DynamicTree stays Jobs-free → Global Constraints + Task 5 Step 1. ✓

**Placeholder scan:** the STEP-1/STEP-2/STEP-3 bodies in Task 2 reference the existing line ranges + "verbatim/identical math" rather than re-transcribing the descent — the implementer moves committed code (not a placeholder); the invariance test + the orchestration block are complete; commands show expected output. The `<tuned>` grain is resolved in Task 3 (start 64) + Task 4 (sweep).

**Type consistency:** `EvictTouchedAndCollectMoved`/`QueryProxyPairs(id,stack,out)`/`MergeAndEmit(span,out)`, `m_findSerial`/`m_movedSerial` (DynamicTree), `m_bpMovedScratch`/`m_bpFindScratch`/`m_bpStackScratch`/`kBroadphaseGrain` (PhysicsWorld), the `[broadmt]` tag, and `std::span<const std::vector<std::uint64_t>>` are consistent across Tasks 2-4. ✓
