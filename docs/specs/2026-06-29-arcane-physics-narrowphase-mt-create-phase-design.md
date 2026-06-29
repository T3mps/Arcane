# Arcane physics: narrowphase MT — create phase (contact create / dynamic-vs-static find) — design

**Date:** 2026-06-29
**Status:** Design approved (brainstorming). Next: writing-plans.
**Arc:** continues the physics MT line — Phase C (compacted solver state) -> D1 (solver-MT) -> D2 (broadphase pair-finding MT, `bad2303a`) -> narrowphase **update** phase MT (`feature/arcane-physics-narrowphase-mt`, byte-identical, ~4.6x) -> **this = narrowphase create phase MT** (gap G2 of the Box2D-v3 parity program).
**Program context:** `.superpowers/sdd/arcane-vs-box2d-mt-gap-analysis.md` (G2 of G2->G1->G3->G4).
**Baseline / branch:** `feature/arcane-physics-narrowphase-mt-create-phase`, stacked on the (unmerged) narrowphase-MT update-phase branch (`2e2697fb`); rebases onto `main` once that merges. Reuses the update-phase's gather/flag/serial-apply machinery, `Arcane::BitSet`, and per-worker scratch patterns.

---

## Problem

After the update-phase MT landed (~2.5 ms -> ~0.54 ms), the **contact CREATE phase is the largest remaining serial slice (~3.0 ms)** at real-game churn (2000-body scene-8). It is the per-step dynamic-vs-static candidate find + new-pair creation, and it runs entirely serially:

- **Stage 2b** (`PhysicsWorld.cpp` ~2561): `ForEachAwake` loop — per awake dynamic body, `StaticCandidates` queries the static `SpatialGrid` + dynamic tree for overlapping static/span fixtures, computes span manifolds, and `TryCreateContact`s each new pair. The candidate query (`StaticCandidates`, `Queries.cpp:537`) is the dominant cost.
- **Stage 2c** (~2720): kinematic-vs-static contact create, serial.

(Stage 2a, broadphase pair-FIND for mover-mover, is already MT from D2.)

## Goal

Parallelize the create-phase candidate find across the task executor, **bit-for-bit identical to serial** at any worker count (preserving the determinism tripwire, as the update phase did), with zero steady-state allocation. Match Box2D v3's **parallel-find / serial-create** structure exactly.

## Non-goals

- G1 (parallel finalize/sleep), G3 (parallel CCD), G4 (24-color) — later workstreams in the program.
- No algorithmic change to **what** gets created, which fixtures pair, the dedup semantics, or the color assignment outcome. Pure parallelization of the existing find, with creation deferred to a serial tail that reproduces today's create order exactly.
- The initial manifold `Collide` is NOT computed here — it already happens in the (parallel) UPDATE phase the same step. Create only finds pairs + inserts pool slots + assigns color.

---

## Current structure (PhysicsWorld.cpp create path)

Per the design-input map (`.superpowers/sdd/g2-create-mt-designinput.md`):

**Serial mutation points (must stay serial):**
1. `ContactPool::EnsurePair(hA, hB)` (~2369) — mutates `m_index` (unordered_map key->id), `m_pool`, `m_alive`, `m_free`, `m_live`.
2. Pool-slot field init via `m_contactPool.Get(id)` (bodyA/bodyB/bIsBody/solverRelevant/eventRelevant).
3. `AssignContactColor(id, ia, ib, aDyn, bDyn)` (~2398/2007) — **order-dependent**: reads `m_bodyColorMask[a]`/`[b]` to pick the lowest free color, then writes that bit + appends to `m_colorContacts[k]`. Two new contacts sharing a dynamic body MUST be assigned in a fixed order.
4. `m_spanContacts` / `m_spanCenters` appends (transient span contacts).

**Expensive parallelizable work (pure, read-only):**
- `StaticCandidates(query, ...)` (`Queries.cpp:537`) — `SpatialGrid::QueryAABB` tree/tile descent + fixture enumeration per awake dynamic body; inline span `Collide`; `ContactPool::Find` (read-only) dedup against existing contacts.

**Key prerequisite blocker:** `StaticCandidates` uses a single shared `mutable m_staticGridScratch` (`PhysicsWorld.hpp:1376`) as internal scratch for `SpatialGrid::QueryAABB`. This must become **caller-supplied per-worker scratch** (signature change + all call-site updates) before the find can run in parallel.

---

## Design — Box2D-aligned: parallel detect -> serial create

Mirrors Box2D v3's broadphase: `b2FindPairsTask` runs `b2ParallelFor` across moved proxies (each worker queries the AABB trees into private buffers), then contact creation is **strictly serial on the main thread** ("the only serial step in the broad phase"). This is the same contract D2 already implemented for the mover-mover FIND half; G2 extends it to the dynamic-vs-static find + the create tail.

### Prerequisite — per-worker query scratch (Task 1)

Change `StaticCandidates` (and `SpatialGrid::QueryAABB`) to take caller-supplied scratch instead of the shared `m_staticGridScratch`. Add per-worker scratch vectors (sized to `WorkerCount()`). Update all call sites. Pure refactor, serial behavior unchanged, full `[physics]` byte-identical.

### Seam 1 — parallel detect (the win)

`Executor()->ParallelFor(m_awakeBodies.size(), grain, [&](begin, end, worker){ ... })` (grain = Box2D-style 16-64; tune later). Per awake dynamic body in `[begin, end)`:
- Run `StaticCandidates` using `worker`'s private scratch -> overlapping static/span fixtures.
- Compute span manifolds into the worker's private span buffer.
- For each candidate that is NOT already a live contact (`ContactPool::Find`, read-only), emit a **candidate-pair record** `{ awakeIndex, bodyA, bodyB, fixtureA, fixtureB, kind }` into the worker's `m_newPairsScratch[worker]`, preserving per-body candidate order.

No pool insert, no color assign, no wake, no map mutation in this pass. The detect is order-independent: `StaticCandidates` reads only stable structures (the static grid + dynamic tree + fixtures), and creating a contact does not move proxies or alter the trees, so each body's candidate set is identical whether computed before or interleaved with creates.

### Seam 2 — serial create (in awake-index order)

Each `NewPairRecord` carries the **`awakeIndex`** (the position in `m_awakeBodies` of its owning body) plus a per-body `candidateSeq`. The serial tail emits creates ordered by `(awakeIndex, candidateSeq)` — reproducing today's `ForEachAwake` create order exactly. Do NOT rely on worker-id order: `ParallelFor` does not guarantee worker id == range order (worker 3 may own the first range), so the tail must order by the stable `awakeIndex` key, not by concatenating worker buffers in worker order. Because each worker handled a contiguous ascending awakeIndex range, the per-worker buffers are each already sorted by `awakeIndex`, so the tail is a cheap **k-way merge** (or a stable sort) on `awakeIndex` — analogous to the update phase's ascending-pool-id walk. For each merged candidate: `TryCreateContact` -> `EnsurePair` + pool-slot init + `AssignContactColor` (order-dependent, here in the serial tail) + span merge + wake/island link. Kinematic-static create (stage 2c) similarly: detect in parallel if measurable, else keep serial (it is small).

---

## Byte-identity argument

- Seam 1 computes each awake body's candidate set as a pure function of stable broadphase state; the set is independent of evaluation order or worker count, and of whether any creates have happened (creates don't move proxies / alter trees). Each worker writes only its own scratch.
- The `ContactPool::Find` dedup in seam 1 reads the pre-create pool, identical across workers; awake dynamic bodies do not share a (dynamic, static/span) pair, so no pair is detected by two workers.
- Seam 2 replays `TryCreateContact` ordered by the stable `(awakeIndex, candidateSeq)` key (k-way merge over the per-worker buffers), which is exactly today's `ForEachAwake` create order — independent of worker count or which worker handled which range. So `EnsurePair` id allocation and the order-dependent `AssignContactColor` produce identical pool ids, colors, and island links.

Therefore the create phase is **bit-for-bit identical** serial vs MT at any worker count — verified by a new invariance test and by the existing `[physics]` suite staying green with **no re-baseline**. (NOTE: we deliberately replay in awake-iteration order rather than a canonical (bodySlot, fixtureSlot) sort. A canonical sort would be cleaner but would re-baseline color assignment; order-preserving replay keeps byte-identity and the free determinism tripwire, consistent with the update-phase MT.)

---

## Box2D alignment (verified against erincatto/box2d main)

| Aspect | Box2D v3 | This design | Match? |
|---|---|---|---|
| Pair find | `b2FindPairsTask` parallel over moved proxies, private per-worker buffers | `ParallelFor` over awake bodies, `StaticCandidates` into per-worker scratch | ✓ |
| Creation | strictly serial on main ("only serial step in broad phase") | serial create tail | ✓ |
| Dedup | per-worker found-pairs, deduped at create | read-only `ContactPool::Find` in detect + serial create | ✓ (equivalent) |
| Move-driven | only moved proxies generate pairs | awake bodies drive the static find (Arcane's awake set) | ~ (Arcane awake-set vs Box2D move-buffer; both incremental) |
| Order | merged pair arrays | awake-iteration-order replay (byte-identical) | DIVERGE (we keep byte-identity; Box2D doesn't promise it) |

## Components / files

- `PhysicsWorld.hpp`: per-worker scratch — `m_staticGridScratch[w]` (move the shared one to per-worker), `m_newPairsScratch[w]` (`std::vector<std::vector<NewPairRecord>>`), `m_spanContactsScratch[w]`; a `NewPairRecord` struct.
- `Queries.cpp` / `SpatialGrid`: `StaticCandidates` + `QueryAABB` take caller-supplied scratch.
- `PhysicsWorld.cpp` `UpdateContacts`: replace the serial stage-2b `ForEachAwake` create loop with seam-1 `ParallelFor` (detect) + seam-2 serial create. Stage 2c per measurement.
- Reuse `Executor()`/`ParallelFor`/`WorkerCount()` (no new plumbing).

## Testing

- **New MT==serial byte-identity test** (`[physics][mt]`): a churning create-heavy scene (bodies continually entering new static overlaps), step N times at WorkerCount 1 vs K; assert identical final positions/angles + contact pool/island state. Mirrors the update-phase MT test.
- **Full `[physics]` suite green, no re-baseline** (byte-identical).
- **Perf A/B** (throwaway StepProf/NPPROF): create-phase ms at 1 vs K workers on the 2000-body scene-8 rig; expect the ~3.0 ms find to drop toward `~3.0/K + barrier`. Grain tuned so settled/low-churn scenes stay on the serial path.

## Risks / verify-during-impl

- **`StaticCandidates` purity:** must be fully re-entrant once scratch is per-worker — re-confirm no other shared mutable state is touched.
- **`AssignContactColor` order:** must remain in the serial tail; verify the awake-iteration-order replay exactly matches the current `ForEachAwake` create order (the byte-identity hinge).
- **Awake-set stability during detect:** the awake body list must not be mutated during seam 1 (wakes/sleeps are applied elsewhere); confirm.
- **Mover-mover created pairs (from D2 stage 2a):** confirm where those are created and that they are handled consistently (likely the same serial create tail) without double-creation vs the static find.
- **Span contacts:** transient per-step; ensure per-worker span buffers merge into `m_spanContacts` in the same order as today.

## Rollout

TDD: write the MT==serial create invariance test first (RED with a forced multi-worker executor before the parallel detect is wired), refactor `StaticCandidates` to per-worker scratch (byte-identical), wire seam 1 + seam 2, GREEN. Perf A/B + grain tune. Revert throwaway instrumentation in a final commit. Then push, CI green, hand to T3mps.

## Out of scope (future — the rest of the parity program)

- **G1** parallel finalize/sleep (~2.25 ms, Box2D `b2FinalizeBodiesTask`).
- **G3** parallel CCD (`BulletSweep`).
- **G4** 24-color scheme + 1-block fast path (the one numerics re-baseline).
- **G6** per-worker scratch 64B padding (fold in if a phase becomes contention-bound).
