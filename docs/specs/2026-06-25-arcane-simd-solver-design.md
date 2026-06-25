# Arcane — SIMD Constraint Solver (Box2D-v3 colored-SoA) (Design)

- **Date:** 2026-06-25
- **Status:** Design approved; implementation plan pending.
- **Scope:** Rewrite the `SoftStep` constraint solver (`Arcane/Core/src/Arcane/Physics/Solver/`) from AoS sequential Gauss–Seidel to the Box2D-v3 **graph-colored, Structure-of-Arrays, lane-wide SIMD** form — contacts AND all joint types — single-threaded, written against the `Arcane::Simd` wrapper.
- **Consumes:** `Arcane::Simd::f32w`/`b32w`/`i32w` (the wrapper foundation, merged to `main` @ `9a2d6e9`; design `docs/superpowers/specs/2026-06-24-arcane-simd-wide-float-design.md`).
- **Relates to:** `project_arcane_physics_perf_stress` (SIMD solver is the ranked next perf item); `project_arcane_simd_wide_float` (the wrapper this consumes); the persistent ContactPool (Phase 3); `feedback_engine_evolves_not_frozen` (determinism is the only hard contract; numerics re-baseline on purpose).
- **Completes (parked item):** the Phase-5 "relocate warm-start impulses onto the Contact" carry-forward (done here, since the SoA build needs it).

---

## 1. Context & Motivation

After the collision rebuild, the 10k stress scene is **solver-bound**: one `Step` of 10k bodies takes ~14ms (the `/arch:AVX2` merge cut it from ~28ms by auto-vectorizing the scalar solver, but that's the ceiling of auto-vectorization). The hot path is `SoftStep::SolveContacts` (8 calls/step = 2 passes × 4 substeps), an **AoS sequential Gauss–Seidel** loop: it walks `ctx.contacts[c]` one constraint at a time, reads body velocities from the world SoA, applies the impulse, and **writes velocities back immediately** so the next contact sees the update. `WarmStart`/`ApplyRestitution`/`SolveJoints` do the same.

That read-modify-write-per-constraint structure is exactly what blocks SIMD: two constraints in one 8-lane batch that share a body would race. **Box2D v3** removes the hazard with **graph coloring** (partition constraints into colors where no two in a color share a dynamic body), an **8-wide SoA constraint batch** per color, a **solver-local body-state SoA** gathered/scattered by body index, and an **overflow** scalar color for the un-colorable remainder. The constraint *math* (TGS Soft) is unchanged — only its data layout and batching.

Target: one `Step` of 10k bodies from ~14ms → low single-digit ms (≤ ~8ms for a solid 60 FPS; the lane-wide solve is ~the AVX2 width on the solve, with multithreading — a later spec — additive on top because colors are the natural parallel unit).

## 2. Goals / Non-Goals

**Goals**
- Rewrite `SoftStep` to the colored-SoA form: **contacts AND all joint types** solved lane-wide via `Arcane::Simd`.
- Preserve the `ISolver` seam, the TGS-Soft algorithm, and observable behavior (settling, restitution, friction, kinematic push, energy bounded).
- Per-step **greedy graph coloring** over the unified (contacts + joints) constraint set.
- A solver-local **body-state SoA** (`v.x/v.y/w/dp.x/dp.y/dq`) synced world↔solver at the Step boundary.
- Relocate **warm-start impulses onto the persistent Contact** (retire `m_cache`).
- Preserve **determinism** (run-twice-identical) and re-baseline behavior deliberately (the colored solve order differs from the sequential sweep).
- Headline: materially cut the 10k stress `Step` time; report sim ms before/after.

**Non-Goals (own specs / later)**
- **Multithreading** — the next spec. Coloring makes it additive (parallel-for over a color's batches with a deterministic reduction); this spec is single-threaded but structures everything so MT drops in.
- **Incremental coloring** maintained on the persistent graph (Box2D's b2ConstraintGraph) — per-step greedy is used here; incremental is a later optimization only if coloring ever shows in a profile (it won't — coloring is O(constraints), ~1/8 of one solve pass).
- SIMD narrowphase/broadphase — separate, smaller wins.
- `f64` solver, runtime CPU dispatch, non-AVX2 x86 — inherited from the wrapper spec's deferrals.

## 3. Decisions (resolved in brainstorming)

| Question | Decision |
|---|---|
| Solver shape | **Rewrite `SoftStep` in place** (one solver, Box2D way). No permanent scalar-solver oracle; validation = behavioral suite + run-twice + the lane-width-invariance check + the pre-rewrite numerics as a transient anchor. Baumgarte stays as a loose cross-check ISolver until Phase 5. |
| Coloring | **Per-step greedy recolor** over the unified contacts+joints set (pool/registration-id order; lowest color whose two **dynamic** bodies are free; static/kinematic `invMass==0` bodies don't constrain coloring). |
| Warm-start | **Relocated onto the persistent Contact** (manifold points carry accumulated normal/tangent impulse; `m_cache` retired). Completes the Phase-5 item. |
| Joints | **Full lane-wide SIMD** for every joint type (Distance/Revolute/Prismatic/Weld/Mouse/Wheel/Motor), colored into the same graph, solving against the body-state SoA. |
| Body-state SoA | Indexed **directly by world slot** (sized to `world.Count()`, no compaction remap — simpler than b2; gather tolerates sparsity). |

## 4. Architecture

### 4.1 Body-state SoA
A solver-owned packed structure, sized to the world high-water slot count, **indexed by world slot**:
```
struct BodyStateSoA {
    std::vector<float> vx, vy;     // linear velocity
    std::vector<float> w;          // angular velocity
    std::vector<float> dpx, dpy;   // accumulated position delta (TGS)
    std::vector<float> dq;         // accumulated angle delta (TGS)
};
```
- **Sync in** (Step start): one pass over awake dynamic bodies — write `vx/vy/w` from `VelSlot`/`AngVelSlot`, zero `dp/dq`. (Static/kinematic stay 0 / are read but never written.)
- **Solve** reads/writes ONLY the body-state SoA via `gather`/`scatter` on the constraint's body-index lanes — never the world SoA.
- **Sync out** (finalize): write velocities back to the world; commit `dp/dq` into world position/angle (the existing compound-COM `FinalizePositions` math, now reading the SoA).

`invMass`/`invInertia` are carried **on the constraint SoA** (per-lane), not gathered from a body array (they're constant over the Step) — exactly as the current `ContactConstraint` already caches them.

### 4.2 Graph coloring (per-step greedy)
`ContactColoring` (new): each Step, after contacts are updated and joints enumerated, walk the **solver-relevant touching contacts** then the **active joints** in a fixed id order; for each constraint assign the lowest color index `k` such that neither of its two **dynamic** bodies has bit `k` set in a per-body color bitmask; set the bits. A small fixed color count (e.g. `kColorCount = 12`, Box2D's value) covers typical graphs; constraints that find no free color (a body with > `kColorCount` constraints) go to a single **overflow** bucket.
- Static/kinematic bodies (`invMass==0`) are **never** marked in the bitmask — they're read-only in the solve, so unbounded constraints may share them in one color.
- Output: `colors[k]` = a list of constraint refs (contact or joint), plus `overflow`. Deterministic (fixed walk order, fixed assignment rule).

### 4.3 SoA constraint batches
Per color, contacts and joints are packed into width-wide SoA batches (`f32w::width` lanes — 8 on AVX2):
- **`ContactConstraintSimd`** — per field an array of lanes: `normalX/Y`, per-point `anchorAx/y`, `anchorBx/y`, `baseSeparation`, `normalMass`, `tangentMass`, `normalImpulse`, `tangentImpulse`, `relativeVelocity`; `friction`, `restitution`, `biasRate`, `massScale`, `impulseScale`, `invMassA/B`, `invInertiaA/B`, `bodyIndexA/B` (i32w lanes), a `dynB` mask, and a per-point `valid` mask (1-point manifolds mask point 2).
- **`JointConstraintSimd`** (per type) — the type's constraint state in SoA lanes (anchors, axes, effective masses, bias, accumulated impulses, the two body-index lanes).
- **Partial final batch** (count not a multiple of width) is padded with **masked no-op lanes** (`invMass=0`, `bodyIndex` pointing at a safe slot, impulses 0) so the lane-wide math is a no-op on padding.

### 4.4 Lane-wide solve
The TGS-Soft math, ported to `f32w` and run per color (contacts then joints, fixed order):
- `gather` body velocities (`vx/vy/w`) and position deltas (`dp/dq`) by the `bodyIndexA/B` lanes.
- Compute the lane-wide normal+friction impulse (contacts) or the type's constraint impulse (joints) — the exact current scalar math, vectorized; comparisons (`s > 0`, the Coulomb clamp) via `cmp_*`/`select`; `min`/`max`/`clamp` via the wrapper.
- `scatter` the updated velocities back. **Conflict-free within a color** (coloring guarantees disjoint dynamic bodies in a lane batch), so the scatter is safe; the masked padding lanes scatter no-ops.
- `WarmStart`, `SolveContacts(biased)`, `Relax(no-bias)`, `ApplyRestitution` all become per-color lane-wide passes; the substep loop structure (integrate-vel → warm-start → solve → integrate-pos → relax) is unchanged.

### 4.5 Joints
Each joint type gets: an **SoA batch builder** (scalar Prepare → packed lanes) and a **lane-wide `SolveVelocity`** operating on the body-state SoA, colored alongside contacts. The `Joint` module is refactored so the solve reads/writes the body-state SoA (not the world SoA). Joint Prepare stays scalar (per-Step, cheap); only the per-substep velocity solve is lane-wide.

### 4.6 Overflow + warm-start
- **Overflow** bucket: the un-colorable remainder (a body with more constraints than colors), so its constraints **may share bodies** → solved **sequentially/scalar, one constraint at a time** (the same TGS-Soft math, width-1) which is inherently scatter-safe. Correctness-complete, rarely hit.
- **Warm-start on the Contact**: the persistent Contact's manifold points gain `normalImpulse`/`tangentImpulse` fields; the SoA build reads them at Prepare, the Store pass writes the accumulated impulses back onto the contact. `m_cache` (the id-keyed `unordered_map`) is retired.

## 5. Data flow per `Step`
1. **Sync** world → body-state SoA (awake dynamics; zero `dp/dq`).
2. **Color** the unified contacts+joints set (greedy) → `colors[]` + overflow.
3. **Build** per-color SoA batches (contacts + per-type joints), seeding warm-start from the contacts.
4. **Prepare** (lane-wide): effective masses, soft coefficients, rest-relative velocity.
5. **Substep × `substepCount`**: integrate-velocities (lane-wide over body-state) → per color {warm-start, solve biased, joint solve} → integrate-positions → per color {relax, joint relax}.
6. **Restitution** (lane-wide per color).
7. **Store** accumulated impulses back onto the contacts.
8. **Sync** body-state → world + finalize positions (compound-COM commit).

## 6. Determinism
- **Run-twice-identical** holds: coloring is a deterministic function of the (id-ordered) constraint set; lane assignment + per-color solve order are fixed; no wall-clock/RNG.
- **Lane-width invariance** (a strong property): within a color, all constraints touch disjoint dynamic bodies, so they're independent — solving them 8-wide, 4-wide, or 1-wide yields the **same** result (per-lane math is FMA-identical). So AVX2 / future NEON / scalar all produce the same solve, and the wrapper's scalar backend (width 1) is a free bit-oracle for the AVX2 path. Across colors the order is fixed.
- **Numerics change vs today** (colored order ≠ the sequential sweep) — re-baselined behaviorally. There are NO exact goldens (consistent with the collision rebuild); the `[physics]` behavioral-invariant suite is the gate. Baumgarte remains a loose cross-check ISolver until Phase 5.
- `/fp:strict` (already on Core) keeps operators from auto-contracting; only explicit `mul_add` fuses — the lane-width-invariance relies on this.

## 7. Testing
- **`[physics]` behavioral suite stays green** (stack settles, ball rests, restitution rebounds, friction stops a slide, kinematic pushes dynamic, energy bounded, sleep) — re-baseline tolerances where the colored order shifts a value; do NOT loosen an invariant.
- **Coloring**: no two constraints in a color share a dynamic body; static/kinematic shared freely; deterministic; overflow correctness.
- **SoA build**: the packed batch fields equal the scalar per-constraint values (incl. padding-lane masking, 1-vs-2-point manifolds).
- **Lane-width invariance**: a width-1 build of the solve == the width-8 build within tolerance (the free scalar oracle).
- **Run-twice determinism** + the cross-broadphase/CCD invariants still hold.
- **Perf**: 10k stress `Step` sim ms before/after (the headline), Debug+Release; ArcaneCore static-CRT clean.

## 8. File structure
```
Arcane/Core/src/Arcane/Physics/Solver/
  SoftStep.hpp/.cpp        rewritten: drives sync -> color -> build -> substep(colored) -> store -> finalize
  BodyStateSoA.hpp         the solver-local packed body state (+ sync in/out helpers)
  ContactColoring.hpp/.cpp the per-step greedy coloring over contacts+joints
  ContactConstraintSimd.hpp the SoA contact batch + builder + lane-wide solve
  JointConstraintSimd.hpp   the per-type SoA joint batches + lane-wide solves
Arcane/Core/src/Arcane/Physics/
  Contact.hpp              + warm-start impulse fields on the manifold points
  Joints/*                 refactor SolveVelocity onto the body-state SoA + SoA builders per type
```
(`.inl`/header split per existing Core conventions; new files → regen both workspaces.)

## 9. Sequencing (one spec, phased plan)
The plan builds incrementally, each phase independently green:
1. **Infrastructure + contact-SIMD** — body-state SoA + greedy coloring + `ContactConstraintSimd` + the lane-wide contact solve + warm-start-on-contact + overflow. **Delivers the 10k perf win** and the full behavioral gate. (This is the bulk of the determinism/coloring risk.)
2. **Joints** — refactor the joints module onto the body-state SoA, then add the per-type SoA solver one joint type at a time (Distance → Revolute → Prismatic → Weld → Mouse → Wheel → Motor), each gated by its joint-scene behavioral tests.

## 10. Risks / open items
- **Biggest risk: the coloring + scatter-safety + determinism re-baseline** — the contact phase (1) front-loads it; the behavioral suite + lane-width-invariance check are the gate.
- **Joints module refactor** (solve against body-state SoA) touches every joint type — phased one at a time, each behaviorally gated.
- **Warm-start relocation** changes the persistent Contact + Prepare/Store; the `[physics]` warm-start-continuity tests must hold (a settled stack stays settled across steps).
- **MT-readiness** is a structural goal but NOT exercised here — the next spec validates it; this spec must not bake in anything that blocks a deterministic parallel reduction over colors.
- **Overflow path** must be correctness-complete even though rarely hit (test a pathological high-degree body).
