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

## Design — Box2D-aligned: gather → parallel collide+flag → serial apply

This mirrors Box2D v3's `b2Collide` exactly (verified against `erincatto/box2d@main`
`src/physics_world.c`): a stable contact array is gathered, a parallel task computes
each manifold and *flags* state changes into a per-worker bit set (NO structural
mutation in the parallel pass), then a serial tail OR-reduces the bit sets and applies
all destroy / island-link / island-unlink mutations. We keep Arcane's `ContactPool`
(the one justified divergence — see "Box2D alignment" below) but adopt the technique
verbatim: parallel-flag, serial-apply, a contactId-keyed bit set, `minRange = 64`,
and deferred destruction.

### Seam 0 — serial gather (trivial)

`m_contactPool.ForEach` ascending-id → push each live id into a reused
`m_npContacts` (`std::vector<std::uint32_t>`, `clear()`-not-realloc). This is Box2D's
"gather a stable `b2ContactSim*` array before dispatch" step. The pool is NOT mutated
for the rest of the phase, so the array is stable across the parallel pass. (We gather
ALL live ids; the per-contact BothAsleep/eventOnly skip happens in the task, matching
Box2D's task-side fat-AABB filter.)

### Seam 1 — parallel collide + flag (the win) [= b2CollideTask]

`Executor()->ParallelFor(m_npContacts.size(), /*minRange=*/64, [&](begin, end, worker){ ... })`.
`minRange = 64` is Box2D's literal grain (its "≥40µs / 10K cycles per task" rationale);
below one block the executor runs it serially on worker 0, so settled/low-churn scenes
pay no MT overhead. `Executor()` is the same PhysicsWorld task executor D2 uses;
`WorkerCount()` sizes the per-worker bit sets.

Per `id` in `[begin, end)` (the current per-contact body, reads only):
- **Stale-handle** (`!FixtureSlotLive`) OR **fat-box separation** (`!FatBoxesOverlap`) →
  set `c.npState |= kNpDestroy` and set bit `id` in this worker's bit set. (No `Destroy`,
  no `ReleaseContactColor`, no `MarkSplitCandidate` here — deferred.) Mirrors Box2D
  flagging `b2_simDisjoint`.
- **BothAsleep && !eventOnly** → skip (manifold persists, no flag, no bit).
- else **recompute** (warm-start snapshot → `ComposeFixtureXf` → `Collide` → warm-start
  carry): write `c.manifold` + `c.touching` (disjoint per contact). On a touch transition,
  set `c.npState |= kNpStarted` / `kNpStopped` and set bit `id`. Mirrors Box2D's
  `b2_simStartedTouching` / `b2_simStoppedTouching`.

The only per-contact writes are to that contact's own `manifold` / `touching` /
`npState` (disjoint by id-range) + a set-bit in the worker's OWN bit set — contention-free.

### Seam 2 — serial apply [= b2Collide serial tail]

OR-reduce the per-worker bit sets into one (`BitSet::InPlaceUnion`), then walk set bits
in ascending id (block scan + `std::countr_zero`, Box2D's `b2CTZ64`). For each flagged
contact id, read `c.npState` and apply (clearing `npState`):
- `kNpDestroy` → `MarkSplitCandidate` (if it was a touching dyn-dyn pair) + `ReleaseContactColor` + `m_contactPool.Destroy` (Box2D's `b2DestroyContact`).
- `kNpStarted` (false→true) → queue the dyn-dyn merge edge into `m_pendingMerges` (Box2D's `b2LinkContact`).
- `kNpStopped` (true→false) → `MarkSplitCandidate` (Box2D's `b2UnlinkContact`).

Then the **existing** `std::sort(m_pendingMerges)` + `MergeIslands` loop (unchanged).

---

## Byte-identity argument

- Seam 0 gathers ids in ascending order (the pool's deterministic order).
- Seam 1: each contact's manifold recompute + flag is a pure function of stable body
  state + its own old manifold → independent of evaluation order or worker count;
  writes are disjoint by id; each worker sets bits only in its own bit set.
- Seam 2: the OR-reduced bit set is walked in **ascending id** (CTZ block scan), so
  destroys / `ReleaseContactColor` / merge-queue / split-marks are applied in exactly the
  same ascending-id order as today's serial `ForEach`. Merges are then `std::sort`-ed (the
  existing contract). → identical island + pool + color state regardless of worker count.

Therefore the whole update phase is **bit-for-bit identical** serial vs MT at any worker
count — verified by a new invariance test, and by the existing `[physics]` suite staying
green with no re-baseline. (Per [[feedback_engine_evolves_not_frozen]], byte-identity here
is a free tripwire on an untouched-semantics path, not a frozen goal — but it IS achievable
and we assert it. Box2D itself relies on the same ascending-contactId CTZ walk for a
deterministic serial tail.)

---

## Box2D alignment (verified against `erincatto/box2d@main`)

| Aspect | Box2D v3 (`src/physics_world.c`) | This design | Match? |
|---|---|---|---|
| Pipeline shape | serial pair-create → `b2Collide` → solve | serial create (existing) → this phase → solve | ✓ |
| Parallel pass | `b2CollideTask`: manifold + flag, no structural mutation | seam 1: same | ✓ |
| State-change collection | per-worker `b2BitSet` keyed on stable `contactId`, OR-reduced, CTZ walk | per-worker `BitSet` keyed on pool id, OR-reduced, CTZ walk | ✓ (adopted) |
| Destruction | DEFERRED — flag `b2_simDisjoint` in parallel, `b2DestroyContact` in serial tail | flag `kNpDestroy` in parallel, `Destroy` in serial tail | ✓ (adopted) |
| Grain | `minRange = 64` | `minRange = 64` | ✓ (adopted) |
| Island merge | `b2LinkContact`, serial, after collide | `MergeIslands` via `m_pendingMerges`, serial, after | ✓ |
| Per-contact flags | `simFlags` (started/stopped/disjoint) | `Contact::npState` (kNpStarted/Stopped/Destroy) | ✓ (equivalent) |
| **Contact storage** | graph-resident `b2ContactSim` per color + non-touching array | flat `ContactPool` (gathered into `m_npContacts`) | **DIVERGE (kept)** |

**The one divergence — `ContactPool` vs graph-resident sims — is intentional and out of
scope.** Box2D stores contact sims inside the constraint graph (per color) + a non-touching
array, and `b2Collide` gathers them. Arcane keeps a flat `ContactPool` (the existing data
model; the solver feed is the separate per-step `EmitContactConstraints`). We gather the
live pool ids into `m_npContacts` (Box2D's "gather" step) and parallelize over that — the MT
technique overlays cleanly on the pool. Restructuring to graph-resident sims is a Phase-C-
scale change with no bearing on this parallelization, so it stays as-is.

## Components / files

- **New `Arcane::BitSet`** (Core utility, header-only, e.g. `Arcane/Core/src/Arcane/Util/BitSet.hpp`) — the `b2BitSet` equivalent: `std::vector<std::uint64_t>` blocks, `Resize(bitCount)` / `ClearAll()` / `Set(i)` / `InPlaceUnion(other)`, and a `ForEachSetBit(fn)` block-scan using `std::countr_zero` (C++23, = Box2D's `b2CTZ64`). Small, reusable, unit-tested independently.
- `Contact.hpp`: add a transient `std::uint8_t npState = 0;` to `Contact` (Box2D's `simFlags` equivalent) with `kNpDestroy/kNpStarted/kNpStopped` bit constants. Set in the parallel pass, read+cleared in the serial tail. Not persisted/serialized (transient per-step).
- `PhysicsWorld.hpp`: new reused scratch — `std::vector<std::uint32_t> m_npContacts;` (the gathered stable id list) + `std::vector<BitSet> m_npStateBits;` (one per worker, each `Resize`d to pool id capacity per step, like Box2D sizing `contactStateBitSet` to `contactIdCapacity`).
- `PhysicsWorld.cpp` `UpdateContacts`: refactor the update `ForEach` (2742–2936) into seam 0 (gather) + seam 1 (`ParallelFor`, `minRange=64`) + seam 2 (bit-set walk + the existing merge apply). The create phase (2412–2738) and the EmitConstraints/solve path are untouched.
- Reuse `Executor()` / `ITaskExecutor::ParallelFor` + `WorkerCount()` (no new executor plumbing — same as D2).
- Factor the per-contact recompute body into one helper so the serial-fallback (small `m_npContacts`) and parallel paths share a single definition (no drift).

## Testing

- **New `BitSet` unit test** (`[bitset]`): Set/ClearAll/InPlaceUnion + `ForEachSetBit` walks set bits in ascending order; sparse + block-boundary cases.
- **New MT==serial byte-identity test** (`[physics][mt]`): build a churning scene (mixed shapes, a kinematic mover), step N times with `WorkerCount==1` and again with `WorkerCount==K` (inject a multi-worker executor like the solver MT-invariance test does), assert identical final body positions/angles, contact manifolds, and island assignments. Mirrors `SolverMtInvarianceTest` + D2's byte-identity gate.
- **Determinism**: the existing run-twice determinism tests must stay green.
- **Full `[physics]` suite green, no re-baseline** (byte-identical).
- **Perf A/B**: STEPPROF/NPPROF update-phase ms at 1 vs K workers on the 2000-body churn rig (already built, throwaway). Expect the update slice (~2.5 ms) to drop toward `~2.5/K + barrier overhead`; confirm `minRange=64` keeps settled/low-churn scenes on the serial path (no regression).

## Risks / verify-during-impl

- **Preserve the BothAsleep + eventOnly skip exactly** in seam 1 — it governs which contacts recompute (and thus the solver feed). A survivor mis-classified would change behavior.
- **`npState` must be transient + reset**: set only in seam 1, consumed+cleared in seam 2. A fresh/recycled `Contact` must start `npState=0` (zero-init the field). Not serialized.
- **Bit-set sizing**: `Resize` each worker's `BitSet` to the pool's id capacity each step (Box2D sizes `contactStateBitSet` to `contactIdCapacity`); `ClearAll` before the parallel pass. Grow-only backing → zero steady-state alloc.
- **`minRange=64`**: Box2D's value; confirm settled/low-churn scenes (small `m_npContacts`) stay on the serial path and don't regress.
- **`Collide` purity** is load-bearing — re-confirm no static state is introduced if the narrowphase changes underneath this.

## Rollout

Branch off `main`. TDD: write the MT==serial invariance test first (RED with a forced multi-worker executor before the refactor is correct), refactor into seams, GREEN. Then perf A/B + threshold tune. Revert the throwaway STEPPROF/NPPROF/PILE_COUNT instrumentation in a final commit. Then the team flow (push branch, CI green, merge).

## Out of scope (future)

- **Create-phase narrowphase MT** (~3.0 ms): the dynamic-vs-static rescan + mover-mover `Collide`, with the create-phase mutation hazards (pool `unordered_map` insert, wake-set append, color assign). Reuses this phase's per-worker-collect / serial-apply pattern.
- Sleep-pass MT (~2.25 ms) and any solve revisit.
