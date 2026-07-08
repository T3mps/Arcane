# Arcane SpatialGrid Benchmark Spike — Design (Cycle 1)

Date: 2026-07-01
Status: Approved (brainstorm) — ready for planning
Branch (to be created): `feature/arcane-spatial-grid-benchmark`
Owner: physics workstream

## 1. Why this spike exists

Arcane's 2D physics is an almost-complete, faithful port of **Box2D v3** (same
graph-colored SoA SIMD solver, same island/sleep model, same enkiTS MT backend).
The **SpatialGrid is the one place we are *not* standing on Box2D's shoulders** —
it is our own innovation, motivated by the fact that our first game is a
tile-structured isometric tac-RPG. Because we can't inherit Box2D's design
justification for it, we have to **earn ours with numbers**.

This whole physics program has been disciplined about measure-first (e.g. the G1
island-split work: the planned MT target was projected at <1% and the real cost
turned out to be an unrelated O(pool x island) quadratic that only a Dist profile
revealed). Committing to a grid architecture on armchair reasoning would violate
that discipline. So **Cycle 1 gathers the data; Cycle 2 designs the feature with
every choice citing a number.**

## 2. Settled decisions (inputs to this spike — not re-litigated here)

These were decided in the brainstorm and are treated as fixed constraints:

- **Ownership: physics owns the spatial index 100%.** Every scene has a
  `PhysicsWorld`; every character (PC/enemy) is a body; the gameplay queries we
  care about (movement-range occupancy, "who is in this tile," adjacency/ring,
  AoE unit selection, LoS-to-a-unit) are all queries about **units, and units are
  bodies**. The one non-body thing (static walls/terrain) is already a separate
  layer (`TileGrid`/`IPassabilitySource`, which also answers LoS via
  `BlocksSight`). There is **no** standalone `Arcane::Spatial` module and **no**
  gameplay-owned second occupancy copy. The `SpatialGrid` class stays in
  `Arcane::Physics` (it is already Core, presentation-free, and
  standalone-instantiable; promoting it later would be a mechanical rename).
- **The `DynamicAABBTree` stays the primary mover broadphase** unless the data
  says otherwise. The grid's identity is a **query / occupancy index**, not a
  broadphase replacement — but Cycle 1 will still measure the grid-as-broadphase
  question honestly (see the matrix), because "is tree+grid faster than
  tree-alone" is a purely empirical question we should not pre-decide.
- **Opt-in per world; cell == tile.** Whatever Cycle 2 builds is opt-in so
  non-tile games pay nothing, and cell size/origin are semantic for queries
  (wired explicitly from the game's tile grid, dodging the `tileCellSize = 1`
  silent-default trap).
- **Memory-for-speed is acceptable.** Arcane targets modern systems (16-32 GB
  standard). A per-cell `vector<id>` occupancy grid is cheap even at 10k bodies;
  if a second structure buys pair-find or query wins, its bounded memory cost is
  an acceptable trade.

## 3. What Cycle 1 produces

The benchmark's output is **not a scalar "grid is faster" verdict** — grids win
on dense/uniform tile scenes and lose on sparse/heterogeneous ones (the classic
grid-vs-tree tradeoff). The deliverable is a **crossover map**: the (body count x
density) regime where each configuration wins. That map is what *validates the
opt-in premise* and drives the Cycle-2 architecture.

Concretely, Cycle 1 delivers:

1. A disposable, Core-only, CPU-only benchmark executable.
2. Representative scenes on the crossover axes.
3. A swept run over the config matrix, capturing the metrics below.
4. A findings document (`2026-07-01-arcane-spatial-grid-benchmark-findings.md`)
   with the crossover map, the raw CSV, and a recommendation for Cycle 2.

## 4. Non-goals (explicitly out of scope for Cycle 1)

- **No feature implementation.** Cycle 1 does not add the opt-in wiring, the
  query API, layers, or change how `m_staticGrid` / `m_residencyGrid` are
  configured. It only *measures*.
- **No Tracy.** Tracy is a Grimoire-era engine-wide integration, not part of this
  throwaway harness.
- **No permanent profiling seam in Core.** `PhysicsWorld` gets no phase-timing
  members. The bench times whole-`Step()` plus the directly-callable phases;
  finer breakdown, if ever needed, is temporary local instrumentation.
- **No semi-invasive `WorldDef` knobs** added just to benchmark (e.g. a
  "route statics into the mover tree" flag). The statics comparison is done as a
  structure-level micro-bench instead (see 6.3).
- **No re-baseline of existing `[physics]` tests.** The spike is purely additive;
  the existing suite stays byte-identical and green.

## 5. The benchmark executable

- **Placement:** a new `Arcane/Bench/` project — one standalone
  `kind "ConsoleApp"`, mirroring the `Loom`/`ArcaneTests` project declarations in
  `Arcane/premake5.lua` but **minus** the Arcane.dll / NVRHI / SDL / Astra links.
- **Links:** `Core` + `enkiTS` (MT is a first-class benchmark axis — see 6.1),
  plus `ws2_32` if the linker pulls Core's `TcpSocket` TU. Include dirs:
  `Core/src` + `glm` + `enkiTS/src`. No window, no GPU, no shaders, no data copy.
  The bench stays **Core-only** — it does NOT link `Arcane.dll`. (The engine's
  enki-backed `EnkiTaskExecutor` lives in `Arcane.dll`; the bench supplies its own
  thin enki-backed `ITaskExecutor` instead — see 6.1 — so MT stays available
  without dragging in the engine DLL / GPU stack.)
- **Disposability:** a single project directory we can delete wholesale when the
  physics engine work concludes. Kept deliberately small and dependency-light.
- **Determinism:** no wall-clock in the sim path, no `/fp:fast` (engine rule).
  Wall-clock is used ONLY to time (chrono), never to drive the sim. Fixed RNG
  seeds for scene generation so runs are reproducible.
- **Regeneration:** after adding the project, run
  `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\` to regenerate
  `Arcane.slnx` (do NOT use `GenerateProjects.bat` — it hangs on `pause`).

## 6. The matrix

Scenes are built **directly through the `PhysicsWorld` API** (test-style
`AddBody` with `MakeAabb`/`MakeCircle`), NOT via the Astra-registry scene builders
in `Sandbox/src/Scenes.cpp` — this keeps the bench Core-only and free of Astra /
Sandbox coupling.

### 6.1 Mover-broadphase sweep (end-to-end)

Real end-to-end sweep over `WorldDef.broadphase`:

| Config | `BroadphaseKind` | Meaning |
|--------|------------------|---------|
| Tree   | `Tree`           | the current primary (`DynamicTree`) |
| Hash   | `Hash`           | `SpatialHash` — a pair-producing grid broadphase; the honest "grid-as-mover-broadphase" read |
| Sap    | `Sap`            | `SweepAndPrune` — reference point |

Measured per config: whole-`Step()` ms, broadphase pair-find ms, #candidate
pairs. All three already implement `IBroadphase` (`Update`/`Pairs`/`QueryAABB`),
selected via `MakeBroadphase(def)` — so this is a config sweep, not new code.

MT axis (open-ended): the task executor is a **first-class, open-ended
dimension**, not an afterthought — we may or may not end up parallelizing the
grid itself, and the bench must be able to measure either outcome. `PhysicsWorld`
already takes an injected executor (`SetExecutor(ITaskExecutor*)`; null falls back
to the Core `SerialTaskExecutor`). Because the engine's `EnkiTaskExecutor` lives
in `Arcane.dll` and the bench is Core-only, the bench supplies a **small
bench-local enki-backed `ITaskExecutor`** (a thin wrapper over
`enki::TaskScheduler` + `ParallelFor`, linking `enkiTS` directly) and injects it.
Run the full matrix **serial** (`SerialTaskExecutor`) as the reproducible
baseline, and run the enki executor across the scales where MT is relevant
(`N >= 1000`) to capture the MT picture without a combinatorial blow-up. Structure
the harness so a future parallel grid path (query fan-out or parallel upkeep) is a
drop-in config, not a rewrite. Serial and MT rows are clearly labeled in the
output.

### 6.2 Scenes (the crossover axes)

The scenes are the axes along which grid-vs-tree performance actually diverges,
so they are described in **algorithmic / spatial-statistics terms, not
game-specific ones** — the bench is a general engine tool, and these are the
properties that drive the two structures' behavior:

- **occupancy** — bodies per cell (fill factor) at the chosen cell size.
- **spatial distribution** — uniform / blue-noise vs clustered (Gaussian blobs);
  clustering is what stresses a tree's fat-AABB overlap and a grid's per-cell
  bucket length.
- **extent** — world bounds relative to body count (density ~ N / extent^2).
- **static:dynamic ratio** — fraction of bodies that never move (broadphase
  re-pair cost and residency upkeep are paid only by movers).
- **motion** — per-step displacement magnitude and coherence (how far AABBs move
  each step), which drives broadphase re-pairing and residency `Move` churn.

Each scene is a labeled point in that parameter space, parameterized by
`N in {100, 1000, 10000}`:

- **uniform-dense:** high occupancy (~1 body/cell), uniform distribution, small
  extent, all-dynamic, low coherent motion — the grid's best case / the tree's
  fat-AABB worst case.
- **uniform-sparse:** low occupancy (<< 1 body/cell), uniform/random
  distribution, large extent, all-dynamic — the tree's best case / the grid's
  worst (many empty cells).
- **clustered-mixed:** medium occupancy, clustered distribution (several dense
  blobs on an otherwise empty bounded field), a mix of static + dynamic bodies,
  moderate coherent motion of the dynamics. This is the point in the space that
  *approximates* a tile-structured game (bounded field, clustered movers, some
  static obstacles) WITHOUT encoding any game semantics — it is defined purely by
  its clustering / ratio / motion parameters.

All scene parameters (cell size, extent, occupancy target, cluster count/spread,
static ratio, velocity distribution, RNG seed) are explicit constants in the bench
and echoed into the findings doc — no hidden magic, fully reproducible. Scenes
exercise motion so the broadphase and residency upkeep see realistic churn, not a
static snapshot.

### 6.3 Statics: structure-level micro-bench

Rather than adding a "statics in the mover tree" `WorldDef` knob (semi-invasive,
and we'd throw it away), compare the two structures head-to-head on the **same
static AABB set**, in isolation:

- **grid path:** `SpatialGrid` (as `m_staticGrid` uses it) — `Insert` the static
  AABBs, then time `QueryAABB` over a stream of query boxes.
- **tree path:** a **bench-local** `DynamicTree` — `Insert`/`Update` the same
  AABBs, then time `QueryAABB` over the same query stream.

Metrics: build/insert ms, per-query ms, #candidates returned, memory. This
answers "should statics live in the grid or the tree" (the "for static bodies,
too" question) without touching `PhysicsWorld`.

### 6.4 Query micro-bench (the grid's real identity)

The tree has **no semantic notion of a cell**, so this is not head-to-head speed
at the same query — it is measuring the cost of the queries the grid exists to
serve, with the tree+filter as the only available alternative implementation:

- **tile(x,y):** occupants of a single cell — grid direct vs tree `QueryAABB`
  over that cell's AABB + tight filter.
- **ring(cx,cy,k):** the k-ring of neighbor cells — grid direct vs tree
  `QueryAABB` over the ring's bounding AABB + per-cell filter.
- **region(AABB):** `Residents(region)` (grid) vs tree `QueryAABB` + filter.

Metrics: ns/query at low/medium/high occupancy, for each scene/scale.

### 6.5 Residency upkeep (standalone micro-bench)

The per-step cost of keeping the residency index live is inside `Step`'s commit
path and cannot be A/B-toggled without a `WorldDef` off-switch we deliberately
excluded (4). So measure it as a **standalone micro-bench**: replicate per-step
residency maintenance on a standalone `SpatialGrid` — each step, `Move` every
mover to its new AABB — and time it. This isolates the marginal cost the
residency index adds without touching `PhysicsWorld`.

## 7. Metrics summary

| Metric | How measured | Isolates |
|--------|--------------|----------|
| whole-Step ms | chrono around `world.Step(dt)`, averaged over K steps | end-to-end cost |
| pair-find ms + #pairs | `FixtureBroadphase().Pairs()/UpdatePairs()` directly | broadphase quality |
| static query ms + #cand | grid vs bench-local tree `QueryAABB` on same set (6.3) | statics structure |
| query latency (ns) | grid direct vs tree+filter for tile/ring/region (6.4) | query identity |
| residency upkeep ms | A/B Step delta (6.5) | index maintenance |
| structure memory | reported node/cell/vector footprint | memory-for-speed trade |

All timings: warm up, then average over K steps/queries; report mean (and
min/median if cheap). Output as CSV (one row per config x scene x N) plus a
human-readable summary table.

## 8. Determinism checks (three distinct notions)

The bench uses a body-state FNV hash (over the SoA `posX/posY/angle` + linear/
angular velocity) to gate three DIFFERENT properties — do not conflate them:

1. **Run-to-run determinism (HARD ASSERT):** same broadphase + same seed, two
   constructions, K steps -> identical hash. Guards bench reproducibility so the
   timings mean something.
2. **ST==MT parity (HARD ASSERT):** same broadphase, `SerialTaskExecutor` vs the
   bench enki executor -> identical hash after K steps. This is the engine's MT
   byte-identity invariant; a mismatch is a real bug (or a bench misuse of the
   executor), not a benchmark to report.
3. **Cross-broadphase equivalence (MEASURE + REPORT, not asserted):** Tree vs Hash
   vs Sap may legitimately diverge — they fatten AABBs differently, so they can
   create different speculative pairs and drift apart over K steps. Report the max
   per-body position delta as a finding (it tells us whether the strategies are
   semantically equivalent); do NOT hard-assert byte-identity across strategies. A
   small drift is expected; bodies flying apart is a bug to investigate. This means
   the cross-strategy perf comparison is fair because each strategy simulates the
   SAME scene at the SAME scale for K steps — not because the trajectories are
   bit-identical.

## 9. Output artifacts

1. **CSV** — raw rows (config, scene, N, all metrics), written to a path the
   findings doc references (e.g. under the bench's working dir or `docs/`).
2. **Findings doc** — `docs/superpowers/specs/2026-07-01-arcane-spatial-grid-benchmark-findings.md`:
   - the crossover map (which config wins in which N x density regime),
   - the statics verdict (grid vs tree),
   - the query-cost picture (grid O(1) vs tree+filter),
   - the residency-upkeep cost,
   - a concrete recommendation feeding Cycle 2 (unify-or-separate, grid-as-
     broadphase yes/no, where the opt-in crossover sits).

## 10. Success criteria

- The bench builds Core-only (Debug + Release), runs headless, and exits 0.
- It produces the CSV + findings doc covering the full 6.1-6.5 matrix.
- The hard-assert determinism gates pass (run-to-run + ST==MT for every scene);
  cross-broadphase divergence is measured and reported (8). Any hard-assert
  failure is root-caused and documented, not papered over.
- Existing `[physics]` suite stays green and byte-identical (no re-baseline).
- The findings doc gives Cycle 2 an unambiguous, data-backed recommendation.

## 11. Cycle 2 (deferred — data-gated, not designed here)

Once the crossover map exists, a **second** brainstorm -> spec -> plan -> implement
cycle designs the actual feature, with each decision citing a number:

- opt-in wiring (`WorldDef` flag + cell size/origin derived from the tile config,
  unifying with the existing `tileCellSize`/`tileOrigin`/`passability`),
- the first-class query API surface (tile / ring / region / neighbors /
  k-nearest / layer-filtered / cell-raycast-for-LoS — scoped by what the tac-RPG
  actually needs, YAGNI),
- layers/filtering (units vs statics vs sensor-triggers),
- static handling (grid vs tree, per 6.3),
- whether `m_staticGrid` and `m_residencyGrid` unify or stay separate.

Cycle 2 is intentionally NOT specified here; specifying it now would be designing
on assumptions the benchmark exists to replace.

## 12. Risks / open questions

- **MT via a bench-local executor** — the bench is Core-only, so MT runs use a
  small bench-local enki-backed `ITaskExecutor` (6.1), injected via `SetExecutor`,
  NOT `Arcane.dll`'s `EnkiTaskExecutor`. Keep serial and MT rows clearly separated.
  If the local executor proves fiddly, ship serial as the baseline and add MT rows
  as a fast follow — but the harness must stay open-ended (executor is an injected
  axis), since we may parallelize the grid later.
- **Scene representativeness** — the `clustered-mixed` scene is the most
  decision-relevant and the easiest to make unrepresentative. Its spatial-
  statistics parameters (occupancy, cluster count/spread, static ratio, motion
  distribution, extent, seed) are explicit constants echoed into the findings so
  the verdict is interpretable and reproducible — stated in algorithmic terms, not
  game terms, so the bench stays a general engine tool.
- **`ws2_32` / incidental Core deps** — linking Core may drag in TU dependencies
  (TcpSocket -> ws2_32). Add the minimal link to satisfy the linker; do not pull
  in unrelated subsystems.
- **Query micro-bench fairness** — the tree+filter alternative must be a *fair*
  implementation of each query (correct bounding AABB + tight per-cell filter),
  or the comparison overstates the grid's advantage. The determinism/oracle
  mindset applies: the tree path must return the same occupant set as the grid.
