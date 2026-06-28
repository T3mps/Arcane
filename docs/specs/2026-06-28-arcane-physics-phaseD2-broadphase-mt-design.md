# Arcane Physics Phase D2 — Broadphase Pair-Finding Multithreading (Design)

- **Date:** 2026-06-28
- **Status:** Design approved (brainstorming complete). Implementation plan pending.
- **Branch:** `feature/arcane-physics-phaseD2-narrowphase-mt` (the D2 branch; target pivoted from narrowphase-recompute to broadphase pair-finding after measure-first — see §1).
- **Supersedes:** `docs/superpowers/specs/2026-06-28-arcane-physics-phaseD2-narrowphase-mt-design.md` (the Collide-recompute design) and its plan. Measure-first proved that design targeted the wrong slice; both are parked (kept in git history — the measure-first finding that parked them is exactly why measure-first exists).
- **Scope:** Multithread the dominant cost of the `narrow` stage — `DynamicTree::UpdatePairs`, the broadphase pair re-query (measured **10.28 ms / 71% of Phase 1 / ~53% of the whole `narrow` stage** at 10k) — by parallelizing its per-moved-proxy tree descents over the engine's existing executor, using **Box2D v3 `b2FindPairsTask` as the direct model** (parallel per-proxy tree queries -> per-worker scratch -> serial merge). Determinism preserved byte-identically at any thread count. This is **D2** of the physics-MT sequence (**D1 solver-MT** shipped; **D3 island/sleep MT** out of scope).
- **Relates to:** D1 solver-MT (the `PhysicsWorld` executor seam this reuses — no new ABI; the `ParallelFor` fork-join primitive; the byte-identity-invariance + Amdahl lessons); the collision-rebuild phases (the per-fixture `DynamicTree` broadphase + `SpatialGrid` statics this parallelizes).
- **Reference:** Box2D v3 `src/broad_phase.c` (`b2UpdateBroadPhasePairs`, `b2FindPairsTask`, the per-worker move-result buffers); prior physics-MT findings `docs/superpowers/research/2026-06-27-box2d-v3-solver-mt-findings.md`; the D1 conclusion that motivates D2 `docs/superpowers/specs/2026-06-27-arcane-physics-solver-mt-scaling-rework-design.md`.

---

## 1. The pivot (why this supersedes the narrowphase-recompute design)

D2's first plan targeted the per-pair `Collide()` manifold recompute. **Measure-first invalidated that assumption** at 10k scene 8 (the constant whisk that keeps ~all bodies moving):

| Sub-phase of `narrow` (19.3 ms) | ms/step | % |
|---|---|---|
| **Phase 1 — broadphase pair-find + create** | **13.2-14.4** | **68.5%** |
| Phase 2 — `Collide()` recompute *(old target)* | 4.13 | 21.4% |
| Phase 2 — rest | 1.95 | 10.1% |
| Phase 3 — merges | 0.02 | 0.1% |

And within Phase 1:

| Phase-1 sub-component | ms/step | % of Phase 1 |
|---|---|---|
| **`DynamicTree::UpdatePairs` (mover-mover re-query)** | **10.28** | **71.4%** |
| `StaticCandidates` queries (dyn-static, SpatialGrid) | 1.27 | ~9% |
| mover-pair create (`WakeMoverPair`+`TryCreateContact`) | 1.40 | ~10% |
| span-collide / static-create / kinematic | <0.4 | <3% |

Counts/step: ~10,025 moved proxies re-queried, ~15,143 pairs, only ~416 NEW contacts. So `narrow`'s time is in **finding pairs**, not recomputing manifolds. Parallelizing `Collide()` (21%) would uncork ~4 ms against a ~13 ms serial Phase-1 ceiling. `UpdatePairs` (71% of Phase 1) is the real lever. At 10k whisk ~everything moves, so the broadphase re-queries ~all proxies — inherently O(n) query work, exactly what parallelizes.

## 2. Decisions (from brainstorming)

1. **Target `UpdatePairs` STEP 2** (the per-moved-proxy read-only tree descents). (§3)
2. **Control inversion — no executor param on `DynamicTree`.** `PhysicsWorld` (which already owns the executor) drives the `ParallelFor`; `DynamicTree` exposes a read-only per-proxy query + serial evict/merge/emit halves, staying a pure, Jobs-free broadphase data structure. (§3)
3. **Reuse the existing executor + plain `ParallelFor`** (the D1 seam). No new ABI; not the solver's persistent region. (§3)
4. **Follow Box2D `b2FindPairsTask`:** parallel per-proxy queries -> per-worker scratch -> serial merge. (§4)
5. **Dual gate — correctness AND measured speedup**, with measure-first gating on STEP 2 actually dominating the serial set-churn (§7).

## 3. Architecture — control inversion

`DynamicTree::UpdatePairs(out)` today is one method doing three steps: STEP 1 evict every `m_pairSet` key touching a moved/removed proxy (serial set mutation); STEP 2 for each LIVE moved proxy descend the tree on its fat box, tight-filter, insert canonical `(lo<<32|hi)` keys (the hot work — read-only tree, but writes the shared `m_pairSet` and uses the shared `m_stack`); STEP 3 emit a sorted `BroadphasePair` vector. The MT split exposes the three as public seams, none thread-aware:

```cpp
// STEP 1 (serial): evict touched pairs; fill `movedOut` with the live moved proxy ids
//                  (snapshots m_moved into an indexable vector), then clears m_moved/m_removed.
void DynamicTree::EvictTouchedAndCollectMoved(std::vector<std::uint32_t>& movedOut);

// STEP 2 (read-only, parallel-safe): append canonical (lo<<32|hi) overlap keys for `id`
//   to `out`, using a CALLER-PROVIDED descent stack (no shared m_stack -> no race).
void DynamicTree::QueryProxyPairs(std::uint32_t id,
                                  std::vector<std::uint32_t>& stack,
                                  std::vector<std::uint64_t>& out) const;

// STEP 3 (serial): insert all per-worker collected keys into m_pairSet (dedups), emit sorted.
int DynamicTree::MergeAndEmit(std::span<const std::vector<std::uint64_t>> perWorker,
                              std::vector<BroadphasePair>& out);
```

`PhysicsWorld` orchestrates — executor never leaves it; per-worker stacks + key buffers are `PhysicsWorld`-owned reused scratch:

```cpp
auto* exec = Executor();                                  // D1 member, always non-null
tree.EvictTouchedAndCollectMoved(m_movedScratch);          // serial STEP 1
const std::uint32_t W = exec->WorkerCount();
m_findScratch.resize(W); for (auto& s : m_findScratch) s.clear();
m_stackScratch.resize(W);
exec->ParallelFor(m_movedScratch.size(), kBroadphaseGrain,
    [&](std::size_t b, std::size_t e, std::uint32_t w) {
        for (std::size_t k = b; k < e; ++k)
            tree.QueryProxyPairs(m_movedScratch[k], m_stackScratch[w], m_findScratch[w]);
    });
tree.MergeAndEmit(m_findScratch, out);                     // serial merge + STEP 3
```

- **`UpdatePairs(out)` is RETAINED as a serial wrapper** (calls the three internally: evict -> serial `QueryProxyPairs` loop over `movedScratch` into one buffer -> `MergeAndEmit`). Non-MT callers, tests, and the oracle gate use it unchanged.
- **The shared-`m_stack` race is gone by construction** — the parallel path uses caller-provided per-worker stacks; `m_stack` stays for the serial `QueryAABB`/`Pairs`/wrapper paths only.
- **`DynamicTree` stays Jobs-free** (no `ITaskExecutor` include/param) and unit-testable in isolation; `QueryProxyPairs` is a reusable read-only primitive.
- **Untouched + serial:** STEP 1 evict; the merge into `m_pairSet`; STEP 3 emit/sort; the mover-pair create loop (`TryCreateContact` + coloring); all of Phase 2/3; `StaticCandidates` (deferred, §9).

## 4. Box2D v3 alignment

| Box2D v3 | Arcane D2 |
|---|---|
| Move buffer of moved proxies | `m_moved` -> `m_movedScratch` snapshot (STEP 1) |
| `b2FindPairsTask` (parallel): per moved proxy, query the tree, collect new pair candidates into per-worker `b2MoveResult` buffers | `ParallelFor` over `m_movedScratch`; `QueryProxyPairs` per proxy into per-worker `m_findScratch[w]` |
| Serial: merge per-worker results, create contacts | serial `MergeAndEmit` (into `m_pairSet`) + the existing serial create path |
| The TREE exposes the query; the WORLD layer owns the task dispatch | `DynamicTree` exposes `QueryProxyPairs`; `PhysicsWorld` owns the executor + `ParallelFor` |

Box2D's tree is a pure data structure with the task dispatch at the world layer — exactly the control inversion in §3.

## 5. Determinism — byte-identical at any worker count

1. `QueryProxyPairs` is read-only over `m_nodes` with caller-provided per-worker stacks -> no shared mutable state, deterministic per proxy (same fat-descent + tight-filter + canonical key as today's inline STEP 2).
2. Found keys go into per-worker scratch, merged serially into `m_pairSet` (an unordered_set -> order-independent membership; a pair found from both endpoints dedups).
3. STEP 3 sorts the emitted `BroadphasePair` vector -> identical regardless of insert order / worker count.
4. STEP 1 evict, the merge, and STEP 3 are serial and unchanged.

Therefore **serial == enki(1) == enki(N)**. The retained serial `UpdatePairs` wrapper + the existing oracle invariant (`UpdatePairs() == Pairs() == brute-force`) pin that the split reproduces today's set exactly.

## 6. The parallel surface (precise)

- **Parallelized** (`ParallelFor` over `m_movedScratch`, per proxy, read-only): `QueryProxyPairs` tree descents (fat-box descent + tight-filter), appending keys to per-worker `m_findScratch[w]` with per-worker `m_stackScratch[w]`.
- **Serial:** STEP 1 evict + the `m_moved`/`m_removed` snapshot+clear; the `MergeAndEmit` insert into `m_pairSet`; STEP 3 emit+sort; everything downstream (create, Phase 2/3).
- **Worker index:** indexes `m_findScratch[w]` / `m_stackScratch[w]` (the broadphase consumer of the `ParallelFor` worker index).

## 7. Testing + gates (correctness AND scaling — both required)

- **Measure-first (Task 1) — the key gate (Amdahl risk).** Instrument `UpdatePairs` STEP1 (evict) / STEP2 (descents) / STEP3 (emit+sort) + the merge at 10k scene 8, serial. PROCEED only if **STEP 2 descents dominate**. If the set-churn (STEP1 + merge + STEP3, all O(pairs) serial) dominates, that is an *algorithmic* problem (full evict/rebuild/sort every step), not an MT one -> redirect to that finding. Throwaway instrumentation (reverted).
- **Correctness:** a new `BroadphaseMtInvarianceTest` asserting `UpdatePairs` output (and full-Step world state) byte-identical across `SerialTaskExecutor` / `JobSystem(1)` / `JobSystem(0)`, on a scene with >grain moved proxies (a whisk/continuously-moving scene). The existing oracle gate (`UpdatePairs() == Pairs() == brute-force`) stays green; run-twice determinism + existing `[physics]`/`[determinism]` suites green.
- **Scaling (deliverable):** STEPPROF `UpdatePairs`/`narrow` ms/step serial vs N at 10k. PASS = measurable speedup; report achieved speedup + efficiency + the serial-fraction. (Honest expectation: Amdahl-capped by the O(pairs) set churn, and the tree descent is pointer-chasing -> possibly memory-latency-bound; the measurement settles it rather than assuming.) Else the documented diagnosis.
- **Build:** Arcane Debug+Release both backends; ArcaneCore static-CRT clean; headless Loom smoke exit 0. (VS2026 msbuild; tests from exe dir.)

## 8. Risks

- **Set-churn Amdahl ceiling (top risk).** STEP1 evict + the merge-insert + STEP3 emit/sort are all O(pairs ~15k) serial. If they are a large fraction of the 10.28 ms, MT of STEP 2 caps low. Mitigation: measure-first sizes it; if it dominates, the real lever is *algorithmic* (avoid the full evict/rebuild/sort each step — Box2D keeps no full pair-set), a documented redirect, not a hidden failure.
- **Tree descent is pointer-chasing** through `m_nodes` (cache-miss-prone) -> like the solver's gather, it may be memory-latency-bound and scale sub-linearly. Box2D scales `b2FindPairsTask` well, so it is feasible; measure-after confirms (the D1 "measure, don't assume" lesson).
- **Extraction byte-identity:** `QueryProxyPairs` must reproduce the inline STEP 2 exactly (fat-descent order, tight-filter, canonical key). The oracle + invariance gates pin it; the serial wrapper keeps the old path testable.
- **Per-worker scratch lifecycle:** `m_findScratch`/`m_stackScratch` must be reused (resize-to-`WorkerCount`, clear per step) to hold the zero-steady-state-alloc contract.

## 9. Scope / non-goals / future

- **IN:** parallelize `UpdatePairs` STEP 2 via control inversion (the 3 `DynamicTree` seams + `PhysicsWorld` orchestration + retained serial wrapper).
- **OUT (deferred follow-ups):** `StaticCandidates` query MT (1.27 ms — same per-worker-query pattern, needs per-worker grid scratch since it writes `mutable m_staticGridScratch`); the parked Collide-recompute MT (4 ms); any algorithmic pair-set rework (only if measure-first proves the set churn is the ceiling); D3 island/sleep MT.
- **KEEP:** serial evict/merge/emit/sort; pool insert + coloring serial; the oracle gate; the serial `UpdatePairs` wrapper; `DynamicTree` Jobs-free.

## 10. Implementation sequencing (for the plan to expand)

1. **Measure-first.** Instrument `UpdatePairs` STEP1/STEP2/STEP3 split at 10k; confirm STEP 2 descents dominate. Record; redirect if the set-churn dominates. (Throwaway.)
2. **Extract the 3 seams + serial wrapper.** Split `UpdatePairs` into `EvictTouchedAndCollectMoved` / `QueryProxyPairs` / `MergeAndEmit`; reimplement `UpdatePairs(out)` as the serial wrapper calling them. Gate: full suites + oracle byte-identical (pure refactor).
3. **Parallelize in PhysicsWorld.** Drive `QueryProxyPairs` via `ParallelFor` over `m_movedScratch` with per-worker `m_stackScratch`/`m_findScratch`; `MergeAndEmit` the per-worker results. Add `BroadphaseMtInvarianceTest`. Gate: byte-identical serial == enki(1) == enki(N); full suites + oracle green.
4. **Measure-after + tune grain.** STEPPROF `UpdatePairs`/`narrow` serial vs N at 10k; tune `kBroadphaseGrain`; record speedup + efficiency + serial-fraction. PASS = measurable speedup, else diagnosis.
5. **Final holistic review**, then `superpowers:finishing-a-development-branch`.

## 11. Code touchpoints

- Broadphase (the split target): `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.{hpp,cpp}` — `UpdatePairs` (cpp ~365-458) -> the 3 seams + serial wrapper; `m_pairSet`/`m_moved`/`m_removed`/`m_stack` members; the read-only descent (the STEP-2 loop ~395-440).
- Orchestration: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — `UpdateContacts` Phase 1 (~2349 `m_fixtureBroadphase->UpdatePairs(...)`); new `PhysicsWorld` scratch members `m_movedScratch` / `m_findScratch` / `m_stackScratch`; reuse the `Executor()` member (D1).
- Broadphase interface: `Arcane/Core/src/Arcane/Physics/Broadphase/Broadphase.hpp` (the `IBroadphase` contract — add the seams there if `UpdatePairs` is on the interface).
- Executor seam (reused, no change): `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp`; `PhysicsWorld`'s `SetExecutor`/executor member.
- Determinism gate: new `Arcane/Tests/src/BroadphaseMtInvarianceTest.cpp`; the existing `UpdatePairs == Pairs == brute-force` oracle test (find + keep green).
