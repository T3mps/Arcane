# Arcane Physics Phase D1 — Within-Color Solver Multithreading (Design)

- **Date:** 2026-06-27
- **Status:** Design approved (brainstorming complete). Implementation plan pending.
- **Scope:** Multithread the colored `SoftStep` solver — the `solve` stage of `PhysicsWorld::Step` — over the engine's `ITaskExecutor` seam, using **within-color parallelism** (Box2D-v3 model): a serial color loop with each color's contacts solved in parallel, plus parallel per-awake-body integrate/sync loops. Inject the executor via `PhysicsWorld::SetExecutor` (serial default), consumed from the plugin ABI's `EngineContext::taskExecutor`. **Determinism is preserved byte-identically at any thread count.** Phase D1 is the FIRST of three sequenced physics-MT milestones; **narrowphase (D2) and island/sleep (D3) are out of scope** here.
- **Relates to:** the JobSystem API milestone (`Arcane::ITaskExecutor` + `FunctionRef` + the `EngineContext::taskExecutor` ABI field this is the first consumer of); the physics data-model rearchitecture Phases A–C (the awake-set + persistent contacts + incremental graph coloring this parallelizes); the SIMD solver Part 1 (the colored SoA lane-wide solve this threads).
- **Non-goals:** NO narrowphase MT (D2), NO island/sleep MT (D3), NO cross-color or per-island parallelism (rejected — see §3), NO change to the solver's numerics/convergence (the parallel result is byte-identical to serial), NO new executor primitives (uses the shipped `ParallelFor`), NO solver SoA-layout rework (if D1's scaling gate reveals a memory-bandwidth wall, the *diagnosis* is the deliverable — the rework would be a follow-up).

---

## 1. Motivation (measured, not assumed)

A fresh STEPPROF measurement (scene 8 stress, 10k bodies, 300-step steady-state window, Release) gives the CURRENT per-stage profile — which differs sharply from the stale pre-Phase-B numbers:

| Stage | ms/step | % | (pre-Phase-B) |
|---|---|---|---|
| `narrow` | 20.46 | **39%** | (47%) |
| `solve` | 18.77 | **36%** | (3%) |
| `sleep` | 9.44 | **18%** | (part of 41%) |
| emit / events / rest | ~3.6 | ~7% | — |
| **Total** | **52.3 ms** | ~19 FPS (sim) | |

Phases B/C drained the broadphase-refresh half, which **exposed the solver as 36%** — the #2 cost, tied with narrowphase, NOT the 3% rounding error the old data showed. (Scene 8's constant whisk keeps ~all 10k bodies awake, so the solver does full work every step.) Measure-first flipped the target: the solver is now genuinely worth threading. It is also the **cleanest** stage to parallelize (already graph-colored for independence) and so is the right first milestone to prove the whole physics-MT machinery end-to-end.

## 2. Decisions (from brainstorming)

1. **Sequence the physics-MT work; solver first.** D1 = solver only; D2 = narrowphase; D3 = island/sleep. Each is shippable + separately gated.
2. **Within-color parallelism** (§3) — the Box2D-v3 model; the only option that keeps the result byte-identical at any thread count.
3. **Executor injected, serial by default** (§4) — `PhysicsWorld::SetExecutor(ITaskExecutor*)`; the Sandbox wires `ctx->taskExecutor` (first consumer of that ABI field); tests drive the invariance gate with `SerialTaskExecutor` vs a `JobSystem`.
4. **Dual gate — correctness AND scaling** (§7). Thread-count invariance is necessary but not sufficient; D1 must *measurably* speed up the `solve` stage, or deliver the diagnosis of why it doesn't (the SIMD-solver "win didn't land" lesson).

## 3. Architecture — within-color parallelism (and the rejected alternatives)

`SoftStep` is a colored TGS-Soft Gauss-Seidel solver. Each contact has a persistent color in `[0, kColorCount)`; the coloring invariant is **no dynamic body appears twice in the same color**. Therefore a color's contacts touch **disjoint dynamic bodies**.

- **(a) Within-color — CHOSEN.** Serial color loop (`k = 0…kColorCount`, Gauss-Seidel: color `k`'s velocity updates feed `k+1`); within each color, solve its contacts in parallel. Because the color's bodies are disjoint, the within-color solve is effectively *Jacobi*: every contact reads the pre-color body state and writes only its own (disjoint) bodies — no contact observes another's write. Order, partition, and thread count cannot change any result.
- **(b) Cross-color (DAG-schedule independent colors in parallel) — REJECTED.** Breaks the Gauss-Seidel ordering → shifts numerics → loses determinism. No.
- **(c) Per-island (whole island per thread) — REJECTED as the foundation.** Islands are independent, but scene 8's whisk connects everything into ~one island, so it yields zero benefit exactly where needed. Within-color works regardless of island topology. (A possible future complement for many-small-island scenes, not D1.)

## 4. Executor injection (closes the ABI loop)

```cpp
// PhysicsWorld
void SetExecutor(ITaskExecutor* exec) noexcept;   // set-once; nullptr -> the serial default
// Member: ITaskExecutor* m_executor = &m_serialExecutor;  // SerialTaskExecutor m_serialExecutor;
```
- The world ALWAYS holds a valid executor (defaults to an owned `SerialTaskExecutor` member) — no null-checks in the hot loop; serial fallback for tests/headless/server.
- `SolverContext` gains `ITaskExecutor* executor;`; `PhysicsWorld` fills it from `m_executor` when calling `SoftStep::Solve`.
- **Wiring:** the **Sandbox** (owns the physics step) calls `world.SetExecutor(ctx->taskExecutor)` at world creation — the first consumer of the `EngineContext::taskExecutor` field added in the JobSystem-API milestone. Headless Loom (hosting Sandbox) thus runs the parallel solver, which is how the scaling gate is measured. Tests inject `SerialTaskExecutor` (default) or a `JobSystem` to compare 1-thread vs N-thread vs serial.

## 5. The parallel surface inside `SoftStep::Solve`

**Parallelized** via `executor.ParallelFor(count, minBatch, fn(begin,end,worker))`:
- **Per-color constraint passes** — warm-start, bias-solve, relax, restitution. For each color `k`: `ParallelFor` over color `k`'s contact count; `fn` SIMD-solves its sub-range `[begin,end)` against `m_bodyState`. Disjoint bodies → no races.
- **Per-awake-body loops** — SyncIn (gather world→`m_bodyState`), integrate-velocities (gravity), integrate-positions, SyncOut (scatter back), finalize-positions. `ParallelFor` over the dense awake/solver index range; per-body disjoint writes.

**Serial (the determinism seams):** the color *loop* (sequential `k`; each color is a `ParallelFor` that BLOCKS before the next — the inter-color sync point), and the **scalar overflow** solve (the uncolorable contacts, after all colored passes — typically empty/single-digit).

**Worker index:** unused in D1 (disjoint writes need no per-worker accumulation; writes go straight to `m_bodyState[disjoint]`). It is plumbed for D2's narrowphase per-thread contact scratch.

## 6. Grain / dispatch overhead

~150–200 `ParallelFor` dispatches per step (≈4 substeps × ≈3–4 per-color passes × ≈12 colors, + the per-body loops). A `minBatch` grain (start **64 contacts**, tune empirically) makes small colors run as a single inline batch on the calling thread — the executor already degenerates to that when `count ≤ minBatch` (and `SerialTaskExecutor` always runs inline). Grain is the one tuning knob; no per-color special-casing in the solver. enki per-dispatch overhead is ~µs; ~200 × µs is sub-ms against an 18.8 ms solve, but the scaling gate (§7) confirms overhead doesn't dominate.

## 7. Testing + gates (correctness AND scaling — both required)

- **Thread-count invariance (correctness):** a `[physics]`/`[determinism]` test runs a representative scene's Step with the solver executor as (i) `SerialTaskExecutor`, (ii) a `JobSystem(1)`, (iii) a `JobSystem(0)` (N threads), and asserts the post-step world state (positions/velocities) is **byte-identical** across all three. Proven in-suite, exactly as the JobSystem-API milestone proved its executor invariance. Plus the existing run-twice determinism gates stay green.
- **Full suites green:** `[physics]`, `[determinism]`, `[simd]` byte-identical to pre-D1 (the serial path is unchanged; the parallel path must match it). Re-baseline NOTHING — within-color parallel == serial by construction (§3).
- **Scaling (the anti-"win-didn't-land" gate):** STEPPROF on scene 8, `solve` stage ms/step at executor threads = 1 vs N. D1 PASSES only if `solve` shows **measurable speedup** at N threads; the report states the achieved speedup + efficiency. If it does NOT scale, D1's deliverable is the **diagnosis** (grain too coarse/fine? gather/scatter memory-bandwidth-bound? color sizes too small at 10k? false sharing on `m_bodyState`?), feeding the follow-up — NOT a green checkmark on an unscaled solver.
- **Build gates:** Arcane Debug+Release both backends; ArcaneCore static-CRT clean (PhysicsWorld + solver compile under /MT with the executor seam); headless Loom smoke exit 0. Use the explicit VS2026 msbuild (`...\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`; not on PATH).

## 8. Risks

- **Memory-bandwidth ceiling (the real risk).** The solver is gather-bound (`m_bodyState` gather/scatter). Threads may scale sub-linearly. Mitigation: the §7 scaling gate *measures* this rather than assuming; a poor result is a documented finding (candidate follow-up: tighter SoA / cache-blocking), not a hidden failure.
- **Dispatch overhead on small colors.** Mitigation: `minBatch` grain + inline degeneration (§6); the scaling gate confirms net win.
- **Determinism regression.** Mitigation: within-color disjoint-write property (§3) makes parallel == serial by construction; the byte-identical invariance gate (§7) pins it; serial color loop + serial overflow + fixed bucket order unchanged.
- **False sharing on `m_bodyState`.** Adjacent bodies solved by different workers could share cache lines. Mitigation: the disjoint-body partition is contiguous per worker (range-based), minimizing boundary sharing; if the scaling gate shows it, padding/alignment is the follow-up.

## 9. Out of scope / future (explicit)
- **D2 — narrowphase MT** (the 39% `narrow` stage): partition awake dynamics; parallel broadphase-query + narrowphase collide into per-worker scratch; serial pair-creation/contact-pool seam preserves determinism. Needs the worker index + a concurrent-pool-or-merge design.
- **D3 — island/sleep MT** (the 18% `sleep` stage): per-island-independent split + `UpdateSleep`.
- **Solver SoA / cache-blocking rework** — only if D1's scaling gate proves a bandwidth wall.
