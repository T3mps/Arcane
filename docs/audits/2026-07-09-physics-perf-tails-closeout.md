# Physics Perf/MT-Tails Closeout Ledger (2026-07-09)

The declare-done adjudication of section **C** ("Optional perf/MT tails") of
`2026-07-06-physics-arc-closeout.md`. Sibling to the B-parity ledger
(`2026-07-08-physics-parity-ledger.md`). The audit's recommendation for this whole
bucket was: *"mark all 'closed: revisit on game-consumer demand' — no code."* This
ledger records each item's **verified current state on main** and its disposition.

Governing principle: these are optimization tails, not correctness gaps. None blocks
gameplay; each is closed either because it is **already done**, is **serial-by-design**
(byte-identity / cheapness), is **data-gated** on a real game profile, or is an
**inherent hardware ceiling**. Verify-not-trust applied throughout — the 2026-07-06
audit predated two of these landing, and the code was checked before each call.

Baseline at close: main @`ceb602b7`; `[physics]` 30658/281, `[geometry]` 118689/12,
full `~[gpu]` 176633/502 — all green (no code changed by this ledger).

| ID | Perf/MT tail | Disposition |
|----|--------------|-------------|
| C1 | Create-phase narrowphase MT | **DONE** (already on main @cd01eb01) |
| C2 | D3 sleep-pass MT | **CLOSED — revisit on demand** (serial, cheap) |
| C3 | StaticCandidates MT | **DONE** (folded into C1 @cd01eb01) |
| C4 | Broadphase set-churn algorithmic floor | **CLOSED — inherent** (MT-seamed @bad2303a; residual is the floor) |
| C5 | SpatialGrid Cycle 2 feature | **DEFERRED — data-gated** (A6-dispositioned @a6ed7b82) |
| C6 | `m_staticList` consolidation | **CLOSED — revisit on demand** (current bag is O(1) + correct) |
| C7 | MT solve breaks even at 10k | **NO-ACTION — inherent** (L3-bandwidth ceiling) |

---

## C1 — Create-phase narrowphase MT — DONE (already on main)

**Audit premise (2026-07-06):** create-phase narrowphase is a ~3.0 ms serial tail;
the update-phase MT machinery is reusable to parallelize it.

**Verified state.** Already landed — commit `cd01eb01` (2026-06-29, *before* the audit
was written): *"parallel create-phase detect (Box2D parallel-find/serial-create,
byte-identical)."* The create-phase **detect** runs under
`Executor()->ParallelFor(awakeCount, kCreateGrain, ...)` (PhysicsWorld.cpp:2685),
each worker writing only its own `m_spanEntriesW[w]` / `m_newPairsW[w]` scratch
(structurally mutation-free). The broadphase mover-mover find is likewise per-worker
(`m_bpFindScratch[w]`, :2618-2625).

**The residual serial tail is serial-by-design, not an open item.** After the parallel
detect, a serial pass concatenates the per-worker buffers, `stable_sort`s by awake
index, and calls `TryCreateContact` + `AssignContactColor` (PhysicsWorld.cpp:2660-2672).
This is deliberately serial to keep the **persistent contact-coloring sequence
byte-identical** to the pre-MT path (the basis of the byte-identical MT solver). It is
exactly Box2D's own **parallel-find / serial-create** model — the correct terminal
state, not a tail to chase.

**Decision: DONE.** No code. `kCreateGrain = 16` (PhysicsWorld.hpp:1452) degrades to
serial below 16 awake bodies (no MT overhead on small scenes).

---

## C2 — D3 sleep-pass MT — CLOSED (revisit on demand)

**Verified state.** The island sleep pass is `Island::UpdateSleep(*this, dt)`, called
once per Step (PhysicsWorld.cpp:1913, stage 4). It is **serial** — Island.cpp contains
no `ParallelFor` / `Executor()` use. It walks the island set once, testing the combined
`|v| + |w|·maxExtent` sleep predicate per body and aging island sleep timers.

**Decision: CLOSED — revisit on game-consumer demand.** The pass is a single linear
island walk with no narrowphase or solve work; it has never shown up as a Step-time
cost worth the coordination overhead of parallelizing (which would also complicate the
sleep-timer accumulation determinism). Revisit only if a profiled scene shows the sleep
pass dominating — not before. No code.

---

## C3 — StaticCandidates MT — DONE (folded into C1)

**Audit premise:** static-candidate gathering per awake dynamic is a serial tail.

**Verified state.** Already parallel — `StaticCandidates(query, spans, statics, grid)`
is called **inside** the C1 create-phase `ParallelFor`, once per awake body, using
**per-worker** query scratch (`m_genStaticsW[worker]`, `m_gridScratchW[worker]`,
`m_genSpansW[worker]` — PhysicsWorld.cpp:2688-2712). Each worker owns a disjoint range
of the awake set, so the static-tree queries run concurrently with no shared mutation.

**Decision: DONE.** No separate work exists — C3 is subsumed by C1's landing
(`cd01eb01`). No code.

---

## C4 — Broadphase set-churn algorithmic floor — CLOSED (inherent)

**Verified state.** The `DynamicTree::UpdatePairs` path was MT-seamed by Phase D2
(`bad2303a`): `EvictTouchedAndCollectMoved` snapshots + clears the touched region
(DynamicTree.cpp:372), `QueryProxyPairs` runs per-worker over moved proxies (:417), and
`MergeAndEmit` folds the per-worker buffers back into `m_pairSet` (:480). The parallel
region measured 10.84 ms → 4.94 ms (2.19× @20w), byte-identical.

**The remaining cost is the algorithmic floor, by definition.** The residual serial work
is the `m_pairSet` insert/erase churn itself (DynamicTree.cpp:389, :401) — the
hash-set membership updates that *must* be serialized to produce one deterministic pair
set. That is the "set-churn algorithmic floor" the audit named: not a missing
parallelization, but the irreducible serial core of maintaining a single canonical pair
set. Reducing it further is an algorithm change (a different pair-set structure), not an
MT change, and it is nowhere near a measured bottleneck.

**Decision: CLOSED — inherent.** Revisit only if a game map's broadphase pair churn is
profiled as a Step-time dominator. No code.

---

## C5 — SpatialGrid Cycle 2 feature — DEFERRED (data-gated)

**Verified state.** Already dispositioned by **A6** (`a6ed7b82`, docs-only close;
the ~2500-line `Arcane/Bench/` harness was dropped, the findings doc kept). The Cycle-1
findings (`2026-07-01-arcane-spatial-grid-benchmark-findings.md`) conclude the grid's
real identity is a **narrow, opt-in tile/neighbor-occupancy index for large tile-dense
worlds** — *not* a general broadphase-or-query replacement — and that **statics should
move off the grid onto the tree** (static-candidate lookup is ~3× faster in a tree).
Cycle 2 (opt-in tile-occupancy index + opt-in mover-broadphase) is explicitly **gated
on a real game map showing the profile**, not on curiosity.

**Decision: DEFERRED — data-gated.** The gate is unchanged and unmet (no game map
exists yet to profile). The findings doc is the durable artifact; Cycle 2 opens only
when a consumer's real tile-dense scene demands it. No code.

---

## C6 — `m_staticList` consolidation — CLOSED (revisit on demand)

**Verified state.** `m_staticList` is an unsorted, no-duplicate index bag:
`push_back` on static-body add (PhysicsWorld.cpp:1004), O(1) swap-and-pop on remove
(:1125-1131). Order-independence is safe because pair keys are canonically `(min,max)`
keyed, so static-loop order does not affect determinism (comment :1122-1124). It is
already minimal and correct.

**Decision: CLOSED — revisit on game-consumer demand.** The "deferred benchmark
decision" was whether to change the static-body representation; the SpatialGrid bench
answered the adjacent question (statics belong on the **tree**, per C5) but found no
reason to alter the `m_staticList` bag itself — it is an O(1) structure with no measured
cost. Any future change here is subsumed by the C5 data-gate (statics-on-tree), not a
standalone list refactor. No code.

---

## C7 — MT solve breaks even at 10k — NO-ACTION (inherent)

**Verified state.** The solver-MT scaling rework (`63270c80`) established that the MT
constraint solve is **L3-bandwidth-bound** and breaks even against the serial solve only
at ~10k bodies; below that, the memory-traffic and coordination cost exceed the parallel
win. This is a property of the hardware memory hierarchy applied to the SoA solver's
gather/scatter pattern, not a tunable.

**Decision: NO-ACTION — inherent ceiling.** There is nothing to fix: the persistent
graph-coloring MT solve is byte-identical and already breaks even where the bandwidth
allows. Scenes below 10k correctly run the serial solve (the executor grain selects it).
This item is recorded, not actioned. (Related: B6 — the 24-color parity divergence —
is accepted for the same reason; the color count only governs MT-solve batch
granularity, which is bandwidth-bound regardless.)

---

## Closeout

Section C is **DONE by decision** (2026-07-09): C1/C3 already landed on main, C2/C4/C6
closed as serial-by-design or inherent (revisit on real game-consumer demand), C5
deferred behind the standing A6 data-gate, C7 an inherent hardware ceiling with no
action. No engine or test code was changed by this adjudication.

**Remaining in the physics arc closeout:** section **D2** (the post-arc memory/CLAUDE.md
doc sweep) is the final bookkeeping act; after it, the **PhysicsWorld decomposition**
(the first foundations task) begins against the now-frozen behavioral baseline.
