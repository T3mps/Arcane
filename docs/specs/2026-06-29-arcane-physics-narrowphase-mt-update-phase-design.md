# Arcane physics: narrowphase MT — update phase (persistent-contact recompute) — design

**Date:** 2026-06-29
**Status:** Design approved (brainstorming). Next: writing-plans.
**Arc:** continues the physics MT line — Phase C (compacted solver state) → D1 (solver-MT, `63270c80`) → D2 (broadphase pair-finding MT, `bad2303a`) → **this = narrowphase MT, update phase**.
**Baseline:** `main` @ `fce20ccf`. Branch off `main`.

---

## Problem

At real-game churn scale (2000 awake bodies, scene-8 whisk), `PhysicsWorld::Step` is ~13 ms and the **narrowphase (stage 2, `UpdateContacts`) is the dominant slice at ~5.6 ms (43%)** — and almost entirely serial (D2 made only the broadphase *pair-finding* MT, not the per-pair/per-contact `Collide` geometry).

Measured narrowphase internal split (Dist, steady-state):
- **create ~3.0 ms** — mover-mover `Collide` + per-step dynamic-vs-static rescan + kinematic-static.
- **update ~2.5 ms** — the persistent-contact manifold recompute (`ContactPool.ForEach`, ascending-id).
- merge ~0.005 ms (negligible).

This workstream parallelizes the **update phase** (the ~2.5 ms persistent-contact recompute) — the cleanest seam — establishing the per-worker-collect / serial-apply machinery + the byte-identity contract that the create-phase follow-up will reuse.

## Goal

Parallelize the update-phase manifold recompute across the task executor, **bit-for-bit identical to serial** at any worker count (a free determinism tripwire, like D2), with zero steady-state allocation.

## Non-goals

- **Create-phase MT** (the ~3.0 ms mover-mover + dynamic-vs-static rescan) — the next workstream; reuses this phase's machinery.
- The solve (~4.1 ms, already D1-MT, L3-bandwidth-bound at this scale) and the sleep pass (~2.25 ms) — separate candidates.
- No algorithmic change to what `Collide` computes, what gets reaped, or the island/event semantics. Pure parallelization of the existing recompute.

---

## Current structure (PhysicsWorld.cpp:2742–2914)

The update phase is one `m_contactPool.ForEach` (ascending-id determinism anchor). Per contact:
1. **Stale-handle reap** (2747–2765): dead/recycled fixture → `MarkSplitCandidate` + `ReleaseContactColor` + `m_contactPool.Destroy` → return. **Mutates** pool free-list, `m_bodyColorMask`, `m_colorContacts`, island flag.
2. **Speculative margin** (2771–2778): reads the two bodies' velocities → `margin`. Read-only.
3. **Fat-box separation** (2786–2803): `FatBoxesOverlap(c, extra)` (cheap AABB) → if separated, same reap as (1). Read + serial mutate on separation.
4. **BothAsleep skip** (2827–2831): `BothAsleep(c) && !eventOnly` → return (manifold persists). Read-only.
5. **Recompute** (2845–2913): warm-start snapshot (`oldManifold = c.manifold`) → `ComposeFixtureXf` (×2) → `Collide` → `c.touching` update → island merge/split edge (the only **shared** write: `m_pendingMerges.push_back` / `MarkSplitCandidate`) → warm-start carry-forward (fixed ≤2×2 loop matching new points to old by feature id). **All per-contact-local except the island edge.**
6. **Merge apply** (2918–2936): `std::sort(m_pendingMerges)` + `MergeIslands` per pair (canonical order).

**Verified parallel-safety facts:**
- `Collide` is a pure, re-entrant function: opt-in `NarrowphaseTrace*` is `nullptr` in the hot path; no static/`thread_local` mutable state in the narrowphase TUs (`Collide`/`Gjk`/`Epa`/`Mpr`/`GeometryKernel`); takes shapes + transforms, returns a `Manifold` by value.
- Body transforms are **stable** during stage 2: positions were committed by the previous step's solve (stage 3), and stage 1 already snapshotted prev. The recompute only READS `m_posX/Y`, `m_angle`, `m_fxShape`, `m_fxLocalPos/Angle`.
- The recompute writes only `c.manifold` + `c.touching` of the contact being processed — disjoint across distinct contact ids.

---

## Design — three-seam decomposition

Split the single `ForEach` into three seams (the D2 shape: serial gather → parallel core → serial apply).

### Seam 1 — serial reap + filter (cheap)

Walk `m_contactPool.ForEach` ascending-id (unchanged order). Per contact, run the existing cheap steps: stale-handle check, speculative-margin, fat-box-separation, BothAsleep+eventOnly skip.
- **Reap** dead/separated contacts immediately, exactly as today (`MarkSplitCandidate` + `ReleaseContactColor` + `Destroy`) — serial, ascending-id, byte-identical to current.
- **BothAsleep+!eventOnly** contacts: skip (not appended).
- **Survivors needing recompute:** append `id` to a reused `m_npRecompute` (`std::vector<std::uint32_t>`, `clear()`-not-realloc). Ascending order preserved.

This seam does NO `Collide` — only the cheap filters + the (already-serial) reaps. Pool mutation happens ONLY here (before the parallel pass), so the pool + color + free-list are stable during seam 2.

### Seam 2 — parallel manifold recompute (the win)

`Executor()->ParallelFor(m_npRecompute.size(), kNarrowphaseGrain, [&](begin, end, worker){ ... })`, with a **min-size threshold**: below `kNarrowphaseMtMinContacts`, run the body serially on worker 0 (avoid MT overhead on settled scenes). `Executor()` is the PhysicsWorld task executor D2's broadphase MT already uses; `WorkerCount()` sizes the per-worker scratch.

Per `id` in `[begin, end)`: the exact recompute body from lines 2845–2913 (warm-start snapshot → `ComposeFixtureXf` → `Collide` → `c.touching` → warm-start carry). Writes only `c.manifold`/`c.touching` (disjoint). The island merge/split edge is **collected per-worker** instead of pushed inline:
- false→true (merge): `m_npMergeScratch[worker].push_back(BroadphasePair{lo, hi})`.
- true→false (split): `m_npSplitScratch[worker].push_back(IslandOf(c.bodyA))`.

No `m_pendingMerges`, no `m_islands`, no pool/color writes in this seam.

### Seam 3 — serial apply (cheap)

- **Split marks:** for each per-worker `m_npSplitScratch[w]` (in worker order, then the ids within), `MarkSplitCandidate(islandId)`. `MarkSplitCandidate` is an idempotent flag-set, so the result is the same set of flags regardless of order — but apply in a fixed (worker-ascending) order for cleanliness.
- **Merges:** append every `m_npMergeScratch[w]` into `m_pendingMerges`, then the **existing** `std::sort(m_pendingMerges)` + `MergeIslands` loop (2925–2936), unchanged. The sort canonicalizes order, so the union-from-per-worker is order-independent.

---

## Byte-identity argument

- Seam 1 reaps + builds `m_npRecompute` in ascending-id order — identical to the serial reap order.
- Seam 2: each survivor's manifold recompute is a pure function of stable body state + its own old manifold → the per-contact result is independent of evaluation order or worker count. Disjoint writes.
- Seam 3: merges are `std::sort`-ed before apply (already the contract today); split flags are idempotent. → the island result is identical regardless of the per-worker collection order.

Therefore the whole update phase is **bit-for-bit identical** serial vs MT at any worker count — verified by a new invariance test, and by the existing `[physics]` suite staying green with no re-baseline. (Per [[feedback_engine_evolves_not_frozen]], byte-identity here is a free tripwire on an untouched-semantics path, not a frozen goal — but it IS achievable and we assert it.)

## Components / files

- `PhysicsWorld.hpp`: new reused scratch members — `std::vector<std::uint32_t> m_npRecompute;` and `std::vector<std::vector<BroadphasePair>> m_npMergeScratch;` + `std::vector<std::vector<std::uint32_t>> m_npSplitScratch;` (sized to `WorkerCount()` lazily, like D2's `m_bpFindScratch`).
- `PhysicsWorld.cpp` `UpdateContacts`: refactor the update `ForEach` (2742–2936) into the three seams. The create phase (2412–2738) and the EmitConstraints/solve path are untouched.
- Reuse `Executor()` / `ITaskExecutor::ParallelFor` + `WorkerCount()` (no new executor plumbing — same as D2).
- Possibly a tiny helper for the per-contact recompute body so seam 2's serial-fallback and parallel paths share one definition (avoid duplication / drift).

## Testing

- **New MT==serial byte-identity test** (`[physics][mt]`): build a churning scene (mixed shapes, a kinematic mover), step N times with `WorkerCount==1` and again with `WorkerCount==K` (inject a multi-worker executor like the solver MT-invariance test does), assert identical final body positions/angles, contact manifolds, and island assignments. Mirrors `SolverMtInvarianceTest` + D2's byte-identity gate.
- **Determinism**: the existing run-twice determinism tests must stay green.
- **Full `[physics]` suite green, no re-baseline** (byte-identical).
- **Perf A/B**: STEPPROF/NPPROF update-phase ms at 1 vs K workers on the 2000-body churn rig (already built, throwaway). Expect the update slice (~2.5 ms) to drop toward `~2.5/K + barrier overhead`; record the speedup and the break-even contact count (the threshold).

## Risks / verify-during-impl

- **Preserve the BothAsleep + eventOnly skip exactly** in seam 1 — it governs which contacts recompute (and thus the solver feed). A survivor mis-classified would change behavior.
- **Threshold/grain tuning**: pick `kNarrowphaseMtMinContacts` + `kNarrowphaseGrain` so settled/low-churn scenes (small `m_npRecompute`) stay serial and don't regress (mirrors D2's grain + the solver's serial-default). Measure the break-even.
- **Scratch sizing**: size the per-worker buffers to `WorkerCount()` once (grow-only), `clear()` each step → zero steady-state alloc.
- **`Collide` purity** is load-bearing — re-confirm no static state is introduced if the narrowphase changes underneath this.

## Rollout

Branch off `main`. TDD: write the MT==serial invariance test first (RED with a forced multi-worker executor before the refactor is correct), refactor into seams, GREEN. Then perf A/B + threshold tune. Revert the throwaway STEPPROF/NPPROF/PILE_COUNT instrumentation in a final commit. Then the team flow (push branch, CI green, merge).

## Out of scope (future)

- **Create-phase narrowphase MT** (~3.0 ms): the dynamic-vs-static rescan + mover-mover `Collide`, with the create-phase mutation hazards (pool `unordered_map` insert, wake-set append, color assign). Reuses this phase's per-worker-collect / serial-apply pattern.
- Sleep-pass MT (~2.25 ms) and any solve revisit.
