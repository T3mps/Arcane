# Arcane Physics Phase D2 — Narrowphase Multithreading (Design)

> **SUPERSEDED (2026-06-28):** the D2 target pivoted to broadphase pair-finding after measure-first found Phase-1 pair-finding (68.5%) dominates narrow, not the Collide recompute (21%). See `docs/superpowers/specs/2026-06-28-arcane-physics-phaseD2-broadphase-mt-design.md`.

- **Date:** 2026-06-28
- **Status:** SUPERSEDED — pivoted to broadphase-MT after profiling. See banner above.
- **Branch:** fresh `feature/arcane-physics-phaseD2-narrowphase-mt` off `main` (the solver-MT rework is merged + pushed @ `63270c80`).
- **Scope:** Multithread the `narrowphase` stage of `PhysicsWorld::Step` (`UpdateContacts`, the measured **38% / 18.4 ms** stage at 10k) by parallelizing the per-pair `Collide()` manifold recompute — the embarrassingly-parallel, **compute-bound** hot work — over the engine's existing executor, using **Box2D v3 as the direct model** (`b2Collide`/`b2CollideTask`: serial broadphase pair management -> parallel collide -> serial event/island processing). Determinism is preserved byte-identically at any thread count. This is **D2** of the physics-MT sequence (**D1 solver-MT** shipped; **D3 island/sleep MT** is out of scope here).
- **Relates to:** D1 solver-MT (the `PhysicsWorld` executor seam / `SetExecutor` this reuses — no new ABI; the `ParallelFor` fork-join primitive; the byte-identity-invariance gate discipline); the collision-rebuild phases 2-4 (per-fixture DynamicTree broadphase, the persistent `ContactPool` + one-pass `UpdateContacts`, events-as-byproduct — the pipeline this parallelizes).
- **Reference:** Box2D v3 (`github.com/erincatto/box2d`): `src/contact.c` (`b2CollideTask`, `b2UpdateContact`), `src/world.c` (`b2Collide`), `src/broad_phase.c` (`b2UpdateBroadPhasePairs`), `b2TaskContext` per-worker scratch. Prior physics-MT findings: `docs/superpowers/research/2026-06-27-box2d-v3-solver-mt-findings.md`. Solver-MT rework (the D1 conclusion that motivates D2): `docs/superpowers/specs/2026-06-27-arcane-physics-solver-mt-scaling-rework-design.md`.

---

## 1. Motivation

D1 multithreaded the `solve` stage but found it **L3/memory-bandwidth-bound** — the gather-heavy within-color solve only broke even with serial at 10k (~0.97x). The measured per-stage profile at 10k scene 8 (the constant whisk that keeps ~all bodies awake) is:

| Stage | ms/step | % |
|---|---|---|
| `narrow` (`UpdateContacts`) | 18.4 | **38%** |
| `solve` | 17.3 | 35% (D1; bandwidth-capped) |
| `sleep` (island/sleep) | 9.1 | 19% |
| emit / events / rest | ~3 | ~8% |

`narrow` is now the **#1 stage** and — unlike the solver — its hot work (`Collide()`: SAT / EPA / MPR per pair, reading immutable shape verts + read-only transforms) is **compute-bound**, so it should scale where the solver could not. It is also embarrassingly parallel: each pair's manifold recompute is independent (disjoint `Contact` slot, read-only broadphase tree during recompute), so the determinism risk is far lower than the solver's. D2 is therefore the highest-value, lowest-risk next physics-MT lever — exactly what the D1 spec flagged as the bigger remaining win.

## 2. Decisions (from brainstorming)

1. **Recompute-MT first, measure-gated.** Parallelize the Phase-2 per-pair `Collide()` recompute; defer Phase-1 pair-finding/query parallelization to a follow-up *if* measure-first shows it dominates. (§3, §7)
2. **Reuse the existing executor + plain `ParallelFor`.** `PhysicsWorld` already holds the executor from D1; pass it into `UpdateContacts`. The recompute is a single fork-join pass — plain `ParallelFor` (NOT the solver's persistent region). No new ABI. (§4)
3. **Follow Box2D v3 directly** for the structure (serial pair mgmt -> parallel collide -> serial events/islands) AND for the touch-transition handling: **per-worker scratch collected during the parallel collide, merged serially** (Box2D `b2TaskContext`), rather than a separate serial full-scan. (§4, §5)
4. **Dual gate — correctness AND measured speedup** (§7). Byte-identity is necessary but not sufficient; D2 must measurably speed up `narrow`, or deliver the diagnosis.

## 3. Architecture — restructure Phase 2 to isolate the parallel recompute

`UpdateContacts` (PhysicsWorld.cpp:2330) today runs Phase 1 (create) -> Phase 2 (a single ascending-id pool pass that **interleaves** the parallelizable manifold recompute with serial mutations: stale-handle reap, fat-box-separation destroy, `BothAsleep` skip, manifold recompute, warm-start carry-forward, island-merge-edge detection) -> Phase 3 (apply merges). D2 splits Phase 2 into three sub-phases so the expensive `Collide()` is a clean fork-join with every order-dependent step serial:

- **2a — serial filter + build work-list (cheap).** One ascending-id pass over `m_contactPool`: reap dead-handle contacts and fat-box-separated contacts (`ReleaseContactColor` + `Destroy` — the serial pool/color mutations), skip `BothAsleep` (cached manifold), and append every survivor needing a recompute to a dense **work-list** (`std::vector<uint32_t>` contact ids, reused scratch). No `Collide()`.
- **2b — PARALLEL manifold recompute (the hot work).** `executor->ParallelFor(workList.size(), grain, fn(begin,end,worker))`: each task, for its contact range, runs `Collide()` and the contact-local warm-start carry-forward (match new manifold points to old by feature id, copy impulses) — **writing only its own `Contact`'s manifold** (disjoint). It ALSO records any touch-state transition (began/ended touching) into **per-worker scratch** `m_touchScratch[worker]` (Box2D `b2TaskContext` model). The manifold write needs no scratch; only the small transition set uses the per-worker buffers, so there is no shared-write contention. Broadphase tree read-only here -> no races; byte-identical at any worker count.
- **2c — serial merge of transitions + island/event detection (cheap).** Concatenate `m_touchScratch[0..W)`, sort by contact id (canonical), and process the transitions in that order: queue island-merge edges (`m_pendingMerges`) and collect touched-event-pairs — the same logic Phase 2 does today, now driven by the merged change-set instead of inline. Then Phase 3 (sort + apply merges) is unchanged.

**Executor + primitive:** reuse `PhysicsWorld`'s executor (the D1 `SetExecutor` member); plain fork-join `ParallelFor` for 2b. `SerialTaskExecutor` (or 1 worker) is the deterministic reference. One grain knob (start ~Box2D's contact block size, tune empirically). Per-worker scratch sized to `executor->WorkerCount()`, reused across steps (resize-not-realloc, cleared per step).

**Untouched + serial:** Phase 1 (broadphase `UpdatePairs` + `TryCreateContact` + coloring); pool insert/destroy; contact coloring; island merge apply; event derivation — all stay serial in canonical order.

## 4. Box2D v3 alignment (the direct model)

| Box2D v3 | Arcane D2 |
|---|---|
| `b2UpdateBroadPhasePairs` (serial): create/destroy contacts as fat-AABBs begin/stop overlapping | Phase 1 create (broadphase `UpdatePairs` + `TryCreateContact`) + 2a destroy (fat-box separation reap) — serial |
| `b2CollideTask` (parallel): per-contact narrowphase manifold function + touch flag, over a contact range | 2b parallel `Collide()` recompute + warm-start carry-forward, over a work-list range |
| `b2TaskContext` per-worker scratch: each worker records touch begin/end + (in v3) island-link changes | 2b per-worker `m_touchScratch[worker]` recording touch-state transitions |
| Serial post-collide: merge per-worker contact-state changes, process begin/end events, update island connectivity | 2c serial merge + canonical sort + island-merge-edge queue + event collection, then Phase 3 apply |

Box2D does **not** create/destroy contacts inside the parallel collide (that is the serial broadphase pair update) — exactly why 2a's destroy stays serial and runs before 2b. Box2D's parallel collide only updates manifolds + touch state for existing contacts — exactly 2b.

## 5. Determinism — byte-identical at any worker count

1. 2b writes only each contact's own manifold + does contact-local warm-start, so partition/worker-count/claim-order cannot change any float.
2. `Collide()` is deterministic (no wall-clock, no per-call heap alloc, no fast-math — `/fp:precise`; verified in the narrowphase map).
3. Touch transitions are collected into per-worker scratch but **processed in 2c in canonical (sorted) order**, independent of which worker recorded which — so merge/event order is worker-count-invariant.
4. The serial seams (2a reap, 2c merge, Phase 1 create, coloring, Phase 3 apply, event derivation) run in canonical ascending order, unchanged from today.

Therefore **serial == enki(1) == enki(N)**, by construction. The restructure (2a/2b/2c) must also reproduce today's single-pass Phase 2 result byte-identically; the determinism + run-twice gates pin this.

## 6. The parallel surface (precise)

- **Parallelized** (`ParallelFor` over the work-list, per contact, disjoint): `Collide()` manifold recompute; warm-start point-id carry-forward; touch-state-transition recording into `m_touchScratch[worker]`.
- **Serial:** building the work-list (2a, incl. all pool/color destroys); merging + canonically sorting the per-worker transition scratch and queueing merges/events (2c); applying island merges (Phase 3); all of Phase 1; `EmitContactConstraints`; everything downstream.
- **Worker index:** used to index `m_touchScratch[worker]` (the first narrowphase consumer of the `ParallelFor` worker index, plumbed for D3).

## 7. Testing + gates (correctness AND scaling — both required)

- **Measure-first (Task 1) — critical here.** Add STEPPROF sub-scopes *inside* `UpdateContacts` (Phase-1 pair-find+create / 2a filter / 2b recompute / 2c merge+events) and measure at 10k scene 8, serial. PASS-to-proceed only if **2b recompute is the dominant slice** of the 18.4 ms `narrow`. If Phase-1 pair-finding/queries dominate instead, D2's deliverable redirects to that diagnosis + the Phase-1 design (the measure-first flip that reshaped D1). Throwaway instrumentation (reverted).
- **Correctness — thread-count invariance:** a new `NarrowphaseMtInvarianceTest` runs a scene that **sustains an active recompute work-list larger than the grain** (a whisk-style / continuously-colliding scene — a settling pile would drain to `BothAsleep` and not exercise MT) with the executor as (i) `SerialTaskExecutor`, (ii) `JobSystem(1)`, (iii) `JobSystem(0)` (N threads), asserting post-step world state (positions/velocities) + the emitted contact/event set are **byte-identical** across all three. Plus existing `[physics]`/`[determinism]`/`[simd]` suites green and the run-twice gates.
- **Scaling (the deliverable):** STEPPROF `narrow` (and the 2b sub-scope) ms/step at executor threads = 1 vs N at 10k. D2 PASSES only on **measurable speedup**; the report states achieved speedup + efficiency. Thesis: compute-bound -> scales meaningfully (unlike D1's bandwidth-bound solve). If it does NOT scale, the deliverable is the diagnosis (work-list too small after sleep? shape-vert/transform reads memory-bound? grain?), not a green check.
- **Build gates:** Arcane Debug+Release both backends; ArcaneCore static-CRT clean; headless Loom smoke exit 0. (VS2026 msbuild path; run tests from exe dir.)

## 8. Risks

- **Recompute may be a smaller fraction of `narrow` than Phase-1 pair-finding/queries** (broadphase `UpdatePairs` + per-awake-dynamic `StaticCandidates` at 10k). This is THE top risk — **measure-first (Task 1) sizes it before committing effort**, mirroring D1's measure-first flip. Mitigation: redirect to the Phase-1 design if so.
- **Restructure byte-identity:** the 2a/2b/2c split must reproduce today's single-pass Phase 2 exactly (transition-capture timing, destroy-before-recompute ordering, canonical 2c order). Mitigation: the invariance + run-twice gates; the restructure is a pure reorder of existing steps.
- **Compute-bound scaling ceiling:** if the work-list is small (heavy sleep) or shape-vert/transform reads saturate memory, scaling caps. Mitigation: the §7 measure-after measures it; the invariance test scene keeps the work-list large.
- **Per-worker scratch growth:** `m_touchScratch` must be reused (resize-not-realloc) to keep the zero-steady-state-alloc contract.

## 9. Scope / non-goals / future

- **IN:** the 2a/2b/2c restructure + parallel Phase-2b `Collide()` recompute (+ per-worker touch-transition scratch) via the reused executor + `ParallelFor`.
- **OUT:** Phase-1 static-candidate query parallelization (a follow-up if measure-first shows it dominates — it needs per-worker scratch for `m_genSpans`/`m_genStatics` + the create/pool seam kept serial); D3 island/sleep MT (the 19% stage); any narrowphase numerics change; any executor ABI change.
- **KEEP:** serial pool insert/destroy, coloring, merge apply, event derivation; the tree-read-only-during-recompute property.
- **FUTURE:** D2.1 Phase-1 query MT (if warranted); D3 island/sleep MT; then the whole-Step MT picture (narrow + solve + sleep) compounds.

## 10. Implementation sequencing (for the plan to expand)

1. **Measure-first.** STEPPROF sub-scopes in `UpdateContacts`; confirm 2b recompute dominates `narrow` at 10k. Record; redirect if not. (Throwaway.)
2. **Restructure Phase 2 -> 2a/2b/2c, SERIAL.** Extract the work-list build (2a), a serial recompute-over-work-list (2b run inline, no executor yet), and the transition-merge (2c). Gate: full suites byte-identical (pure reorder).
3. **Parallelize 2b.** Wire the executor + `ParallelFor` over the work-list; add per-worker `m_touchScratch` + the canonical 2c merge. Gate: `NarrowphaseMtInvarianceTest` byte-identical serial == enki(1) == enki(N); full suites green.
4. **Measure-after + tune.** STEPPROF `narrow` serial vs N at 10k; tune grain; record speedup + efficiency. PASS = measurable speedup, else diagnosis.
5. **Final holistic review**, then `superpowers:finishing-a-development-branch`.

## 11. Code touchpoints

- Narrowphase driver (the restructure target): `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — `UpdateContacts` (~2330; the Phase-2 ascending-id pass ~2604-2777), the STEPPROF `Narrowphase` scope (1637).
- Narrowphase kernel (read-only, parallel-safe): `Arcane/Core/src/Arcane/Physics/Narrowphase/Collide.{hpp,cpp}` (`Collide(shapeA, xfA, shapeB, xfB, margin)`).
- Persistent contacts + pool (serial create/destroy): `Arcane/Core/src/Arcane/Physics/Contact.{hpp,cpp}` (`ContactPool`, `EnsurePair`, `Destroy`, `Key`/`MixHandle`), `ContactManager.{hpp,cpp}` (event derivation).
- Broadphase (read-only during recompute): `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.{hpp,cpp}` (`UpdatePairs`, `TryGetFatBox`).
- Executor seam (reused, no change): `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp`; `PhysicsWorld`'s `SetExecutor`/executor member; new per-worker scratch members on `PhysicsWorld` (`m_touchScratch`, the work-list vector).
- Coloring (serial, untouched): `Arcane/Core/src/Arcane/Physics/Solver/ContactColoring.{hpp,cpp}` (`AssignContactColor`/`ReleaseContactColor`).
- Determinism gate: new `Arcane/Tests/src/NarrowphaseMtInvarianceTest.cpp` (mirrors `SolverMtInvarianceTest.cpp`).
