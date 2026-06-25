# SIMD Constraint Solver — Part 1: Infrastructure + Contact-SIMD — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Rewrite `SoftStep` from AoS sequential Gauss–Seidel to the Box2D-v3 graph-colored, SoA, lane-wide-SIMD form for **contacts** (joints are Part 2), single-threaded, over the `Arcane::Simd` wrapper — cutting the 10k stress `Step` time while preserving behavior + determinism.

**Architecture:** A solver-local **body-state SoA** (indexed by world slot) is synced world↔solver at the Step boundary; a per-step **greedy coloring** partitions contacts so no two in a color share a dynamic body; per color an **8-wide `ContactConstraintSimd`** batch is built from the persistent contacts (warm-start now stored ON the contact); the substep solve runs **per color, lane-wide** (`gather` body state → TGS-Soft math in `f32w` → `scatter`), with an **overflow** bucket solved scalar. Within a color all contacts touch disjoint dynamic bodies, so the solve is lane-width-invariant and deterministic.

**Tech Stack:** C++23, Core (presentation-free, /MD + static-CRT, `/fp:strict`, `/arch:AVX2`), `Arcane::Simd` (`f32w`/`b32w`/`i32w`), Catch2 `[physics]`/`[simd]`, premake5/MSBuild. SPEC: `docs/superpowers/specs/2026-06-25-arcane-simd-solver-design.md`. Branch `feature/arcane-simd-solver` (off `main`, spec already committed).

---

## Conventions
- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (swap config for Release/Dist). Build just tests with `-t:ArcaneTests`.
- **Tests:** from the exe dir — `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"` (or `"[simd]"`, or no filter for the full gate).
- **ArcaneCore (static-CRT):** build `Server/ArcaneCore/ArcaneCore.vcxproj` Debug+Release.
- **New files → regen BOTH workspaces** (premake5 lives at the **repo root**, run by absolute path): `& "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026` from `Arcane/`, and the same exe from `Server/`. (NOT `GenerateProjects.bat` — it `pause`s; NOT a bare/`.\` relative path from `Arcane/` — premake is one level up, so use the absolute path.)
- **clangd/IDE diagnostics are FALSE POSITIVES** (incl. unknown intrinsics) — MSVC/MSBuild + the test run are the only truth.
- **Kill stray procs before building:** `Get-Process Loom,ArcaneTests -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue`.
- **Determinism is the contract.** No exact goldens — the `[physics]` behavioral suite + run-twice + lane-width-invariance are the gate. Commit per task with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer; do NOT push.
- **The scalar math reference is the current `SoftStep.cpp`** (read it before touching the solve): `PrepareContacts` (74), `WarmStart` (238), `SolveContacts` (283 — normal solve 320-379, friction 381-409), `ApplyRestitution` (472), `FinalizePositions` (436), the `Solve` driver (553). The lane-wide port must reproduce this math exactly, per lane.

## File Structure
| File | Responsibility |
|---|---|
| `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp` | packed `vx/vy/w/dpx/dpy/dq` by world slot + sync-in/out helpers |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactColoring.hpp/.cpp` | per-step greedy coloring → per-color contact-index lists + overflow |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` | the 8-wide SoA contact batch + builder + lane-wide solve passes |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp/.cpp` | rewritten driver: sync → color → build → substep(colored) → store → finalize |
| `Arcane/Core/src/Arcane/Physics/Contact.hpp` | + `normalImpulse`/`tangentImpulse` on the manifold points (warm-start) |
| `Arcane/Tests/src/PhysicsSimdSolverTest.cpp` | new `[physics]` tests: body-state round-trip, coloring, SoA-build, lane-width invariance |

---

### Task 1: BodyStateSoA + world↔solver sync

**Files:** Create `Solver/BodyStateSoA.hpp`; Create `Tests/src/PhysicsSimdSolverTest.cpp` (first case).

- [ ] **Step 1: failing test** — in `PhysicsSimdSolverTest.cpp`: build a tiny `PhysicsWorld`, add 2 dynamic bodies with known velocities/positions, `SyncIn` to a `BodyStateSoA`, mutate the SoA velocities, `SyncOut`, and assert the world velocities updated and untouched slots are unchanged. (Tag `[physics]`.)
- [ ] **Step 2: build + verify fail** (BodyStateSoA undefined).
- [ ] **Step 3: implement `BodyStateSoA.hpp`:**
```cpp
#pragma once
#include <vector>
#include <Arcane/Physics/PhysicsTypes.hpp>
namespace Arcane { namespace Physics {
class PhysicsWorld;
// Solver-local packed body state, indexed BY WORLD SLOT (sized to world.Count()).
// The lane-wide solve gathers/scatters these by the constraint's body-index lanes;
// the world Vec2 SoA is not gather-friendly, so we mirror a packed copy per Step.
struct BodyStateSoA {
    std::vector<float> vx, vy, w;     // velocity (lin x/y, ang)
    std::vector<float> dpx, dpy, dq;  // accumulated position/angle delta (TGS)
    void Resize(std::uint32_t n) {
        vx.assign(n,0.f); vy.assign(n,0.f); w.assign(n,0.f);
        dpx.assign(n,0.f); dpy.assign(n,0.f); dq.assign(n,0.f);
    }
    // Declared here, defined in SoftStep.cpp (needs the PhysicsWorld accessors).
    void SyncIn(const PhysicsWorld& world);   // world vel -> vx/vy/w; dp/dq = 0
    void SyncOut(PhysicsWorld& world) const;   // vx/vy/w -> world vel (awake dynamics)
};
}}
```
  Define `SyncIn`/`SyncOut` in `SoftStep.cpp` (it already includes `PhysicsWorld.hpp`): `SyncIn` loops `world.Count()`, and for each `Alive && Dynamic && Awake` slot writes `vx[i]=VelSlot(i).x` etc., zeroes `dp/dq`; non-awake/non-dynamic left 0. `SyncOut` writes `SetVelSlot(i,{vx[i],vy[i]})`/`SetAngVelSlot(i,w[i])` for the same predicate. (Position commit stays in `FinalizePositions`, ported in Task 5 to read `dp/dq` from here.)
- [ ] **Step 4: build + test pass.**
- [ ] **Step 5: regen both workspaces** (new files), rebuild, `[physics]` green.
- [ ] **Step 6: commit** `feat(arcane/physics): solver body-state SoA + world sync`.

### Task 2: Greedy graph coloring

**Files:** Create `Solver/ContactColoring.hpp/.cpp`; extend `PhysicsSimdSolverTest.cpp`.

- [ ] **Step 1: failing test** — feed `ColorConstraints` a hand-built list of `(bodyA, bodyB, aDynamic, bDynamic)` edges (e.g. a chain 0-1-2-3 all dynamic) and assert: (a) within each color no two edges share a dynamic body; (b) a static body (`!dyn`) may be shared within a color; (c) deterministic (same input → same output); (d) a star (one body in N edges, N>kColorCount) puts the excess in `overflow`.
- [ ] **Step 2: build + verify fail.**
- [ ] **Step 3: implement** a backend-agnostic greedy colorer:
```cpp
// ContactColoring.hpp
#pragma once
#include <cstdint>
#include <vector>
namespace Arcane { namespace Physics {
inline constexpr int kColorCount = 12;   // Box2D v3 value
struct ColorEdge { std::uint32_t a, b; bool aDyn, bDyn; std::uint32_t ref; }; // ref = caller's constraint index
struct Coloring {
    std::vector<std::vector<std::uint32_t>> colors;   // colors[k] = refs in color k
    std::vector<std::uint32_t> overflow;              // refs that found no free color
};
// edges walked in order; perBody[] is a scratch color-bitmask map sized to bodyCount.
Coloring ColorConstraints(const std::vector<ColorEdge>& edges, std::uint32_t bodyCount);
}}
```
  `.cpp`: `perBody` = `std::vector<std::uint32_t>(bodyCount, 0)` (bit k set = body used in color k). For each edge in order: find lowest `k` in `[0,kColorCount)` where `(!aDyn || !(perBody[a]&(1u<<k))) && (!bDyn || !(perBody[b]&(1u<<k)))`; if found, push `ref` to `colors[k]`, set the bit for each DYNAMIC endpoint; else push to `overflow`. Deterministic.
- [ ] **Step 4: build + test pass.**
- [ ] **Step 5: regen, rebuild, `[physics]` green. commit** `feat(arcane/physics): greedy constraint graph coloring`.

### Task 3: Relocate warm-start onto the persistent Contact (scalar solver still AoS)

**Files:** `Contact.hpp` (+ fields); `SoftStep.cpp` (Prepare reads contact, Store writes contact; retire `m_cache`); `SoftStep.hpp` (drop the cache members).

> This is a behavior-preserving refactor of the EXISTING scalar solver — de-risks the warm-start change BEFORE the SoA rewrite. The `[physics]` warm-start-continuity tests (a settled stack stays settled across steps) are the gate.

- [ ] **Step 1:** Read how the solver maps a `ContactConstraint` point back to its source `Contact` manifold point (the `cp.id` = `MixContactId(...)`). The Contact's manifold points must carry persistent `normalImpulse`/`tangentImpulse`. Add to the manifold point struct (in `Contact.hpp` / the `Manifold` point) two `Real` fields defaulting to 0, persisted across steps in the ContactPool.
- [ ] **Step 2:** In `SoftStep::PrepareContacts`, replace the `m_cache.find(cp.id)` warm-start seed with a read from the owning contact's manifold point. In the `Solve` Store loop, replace `m_cache.insert_or_assign` with a write back onto the contact's manifold point. Remove `m_cache`/`m_stamp`/`kCacheLife`/`CacheEntry` + `WarmStartCacheSize`. The world owns the persistent contacts, so the solver needs the contact handle per constraint — thread it through (the `ContactConstraint` already carries `points[].id`; add the source contact id/handle to the constraint at `EmitContactConstraints` so Store can find it, OR have the world expose `ContactPool` write-back keyed by `id`). Pick the lower-churn seam; keep `EmitContactConstraints` populating it.
- [ ] **Step 3:** build + full `[physics]` green (warm-start continuity holds — a stack settled at step N stays settled at N+1; restitution unchanged). Run-twice determinism green. ArcaneCore static-CRT clean.
- [ ] **Step 4: commit** `refactor(arcane/physics): warm-start impulses on the persistent Contact (retire m_cache)`.

### Task 4: ContactConstraintSimd — SoA batch + builder (no solve yet)

**Files:** Create `Solver/ContactConstraintSimd.hpp`; extend `PhysicsSimdSolverTest.cpp`.

- [ ] **Step 1: failing test** — build a handful of `ContactConstraint`s + a `Coloring`, call the builder, and assert the per-lane SoA fields equal the source scalar values for each contact (and that a partial final batch's padding lanes are masked: `invMass==0`, `valid` mask false).
- [ ] **Step 2: build + verify fail.**
- [ ] **Step 3: implement** `ContactConstraintSimd` (width = `f32w::width`): per-field lane arrays — `normalX/Y`; for each of 2 points: `anchorAx/y`, `anchorBx/y`, `baseSep`, `normalMass`, `tangentMass`, `normalImpulse`, `tangentImpulse`, `relVel`, `pointValid` (mask); `friction`, `restitution`, `biasRate`, `massScale`, `impulseScale`, `invMassA/B`, `invInertiaA/B`; `bodyIndexA/B` (int lanes), `dynB` (mask). A `Build(const ContactConstraint* ccs, const std::uint32_t* refs, int count, ...) -> std::vector<ContactConstraintSimd>` packs `count` contacts into `ceil(count/width)` batches; the last batch's unused lanes set `invMass=0`, `bodyIndex=0`, `pointValid=false`, impulses 0 (lane-wide no-op). Store lane data as plain `alignas(32) float field[width]` arrays (built scalar; loaded into `f32w` at solve time via `load`).
- [ ] **Step 4: build + test pass. regen, `[physics]` green. commit** `feat(arcane/physics): SoA contact-constraint batch + builder`.

### Task 5: Lane-wide contact solve + wire into SoftStep::Solve (THE rewrite)

**Files:** `Solver/ContactConstraintSimd.hpp` (the solve passes); `SoftStep.hpp/.cpp` (driver rewrite).

> Port the scalar math (SoftStep.cpp:238-539) to `f32w`, per color. Use the wrapper: `load/store`, `+ - *`, `mul_add`, `min/max`, `cmp_*`/`select`, `gather/scatter`. The transcription pattern (normal-solve inner block) — replicate it for warm-start, friction, restitution:

```cpp
// scalar (SoftStep.cpp ~362):  Real impulse = -cp.normalMass*massScale*(vn+bias) - impulseScale*cp.normalImpulse;
// lane-wide (n.x/n.y, rA, vA, wA gathered into f32w; s,bias,massScale,impulseScale as f32w):
using namespace Arcane::Simd;
f32w vn   = (dvx * nx) + (dvy * ny);                       // dot(dv, n)
f32w imp  = -nMass * mScale * (vn + bias) - iScale * nImp; // the impulse
f32w newI = max(nImp + imp, setzero());                    // accumulated clamp >= 0
imp       = newI - nImp; nImp = newI;
f32w px = nx * imp, py = ny * imp;                         // P = n*impulse
vAx = mul_add(px,  iMa, vAx);  vAy = mul_add(py,  iMa, vAy);  // vA += P*iMa
wA  = mul_add(iIa, (rAx*py - rAy*px), wA);                    // wA += iIa*(rA x P)
// dynB lane mask gates the B writes via select(dynB, vB-..., vB)
```
  - [ ] **Step 1:** Add to `ContactConstraintSimd` (or a free fn) the lane-wide `WarmStart`, `SolveNormalAndFriction(useBias, h, ...)`, `ApplyRestitution` operating on the `BodyStateSoA` (gather vA/wA/vB/wB + dpA/drA/dpB/drB by `bodyIndexA/B`; compute current separation `s = baseSep + dot((dpA+drA×rA)-(dpB+drB×rB), n)` lane-wide; the normal/friction/restitution math above; scatter velocities back). B-side updates gated by the `dynB` mask via `select`. Mirror SoftStep.cpp:283-419 / 472-539 exactly, per lane.
  - [ ] **Step 2:** Rewrite `SoftStep::Solve` (SoftStep.cpp:553): `EnsureScratch` → `m_bodyState.Resize(count); m_bodyState.SyncIn(w)` → build `ColorEdge`s from the world's solver-relevant touching contacts (A=dynamic orientation already holds) → `ColorConstraints` → build per-color `ContactConstraintSimd` batches (warm-start from contacts) → `PrepareContacts` (lane-wide effective masses + soft coeffs) → substep loop: `IntegrateVelocities` (over BodyStateSoA) → for each color: WarmStart, SolveNormalAndFriction(bias) → `IntegratePositions` (dp/dq += v*h over BodyStateSoA) → for each color: SolveNormalAndFriction(no-bias) → `ApplyRestitution` per color → Store impulses back onto contacts → `FinalizePositions` (port to read dp/dq from BodyStateSoA, same compound-COM commit) → `m_bodyState.SyncOut(w)`. **Overflow contacts: skip here (Task 6).** Integrate-velocities/positions now iterate the BodyStateSoA arrays (still scalar loops — they're O(n), not the hot path).
  - [ ] **Step 3: failing→passing via the gate:** build; run full `[physics]`. Re-baseline tolerances ONLY where the colored order shifts a value within an invariant (a stack still settles, ball still rests, energy still bounded) — do NOT weaken an invariant. Add a **lane-width-invariance** test: a forced-scalar (`ARCANE_SIMD_SCALAR`) build of the same solve over a fixed scene == the AVX2 build within 1e-5 (within-color independence ⇒ they should match tightly). Run-twice determinism green.
  - [ ] **Step 4: commit** `perf(arcane/physics): lane-wide colored SoA contact solve (SoftStep rewrite)`.

### Task 6: Overflow scalar path

**Files:** `SoftStep.cpp`.

- [ ] **Step 1: failing test** — a pathological body with > `kColorCount` contacts (a hub touching many bodies); assert it still settles + is bounded (overflow contacts are solved, not dropped).
- [ ] **Step 2:** Solve the `overflow` refs **sequentially, scalar** (one contact at a time, width-1 math on the BodyStateSoA — reuse the scalar reference math, or run them through the lane solve one-per-batch) AFTER the colored passes within each solve stage (warm-start/solve/relax/restitution), so they compose with the colored result. Sequential ⇒ scatter-safe even though they may share bodies.
- [ ] **Step 3:** build + the pathological test + full `[physics]` green. **commit** `feat(arcane/physics): overflow (un-colorable) contacts solved scalar`.

### Task 7: Full gate + 10k perf + memory

- [ ] **Step 1:** Full ArcaneTests Debug + Release (no filter, `[gpu]` both backends) green; ArcaneCore static-CRT Debug+Release clean.
- [ ] **Step 2:** Perf (Dist Loom scene 8 @ 10k — controller runs it, PAUSE for the user): report `Step` sim ms vs the ~14ms pre-restructure baseline. Expect a material drop (the lane-wide solve is the win).
- [ ] **Step 3:** Run-twice determinism + lane-width invariance hold.
- [ ] **Step 4: memory** — update `project_arcane_simd_wide_float` (or a new `project_arcane_simd_solver`) with Part 1 done + the perf delta; next = Part 2 (SIMD joints).

---

## Self-Review Notes
- **Spec coverage:** body-state SoA = T1; coloring = T2; warm-start-on-contact = T3; SoA batch = T4; lane-wide contact solve + driver rewrite = T5; overflow = T6; gate/perf = T7. Joints = Part 2 (separate plan). Determinism (run-twice + lane-width invariance) is gated in T5. The MT-readiness structural goal is satisfied (colors are the parallel unit) without being exercised.
- **De-risking order:** the three independently-testable pieces (body-state, coloring, warm-start-relocation) land BEFORE the big rewrite (T5), so T5 composes verified parts.
- **No exact goldens:** behavioral invariants + run-twice + lane-width invariance are the gate, per `feedback_engine_evolves_not_frozen`.
- **Soft spots for the executor:** (1) the contact→source-Contact write-back seam for warm-start (T3 step 2 — pick the lower-churn option). (2) the `dynB` mask gating B-side scatter (don't write static/kinematic bodies). (3) padding-lane masking (no-op via invMass=0). (4) `FinalizePositions` must read dp/dq from the BodyStateSoA, preserving the compound-COM commit (SoftStep.cpp:436-466). (5) the separation re-eval `s` uses the per-body dp/dq gathered by lane — the TGS heart; mirror SoftStep.cpp:332-334 exactly.
