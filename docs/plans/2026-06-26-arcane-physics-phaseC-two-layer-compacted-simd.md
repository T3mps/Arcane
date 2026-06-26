# Arcane 2D Physics — Phase C: Two-Layer Compacted Storage + Incremental Coloring + Gather-Free SIMD — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Fresh implementer per task + two-stage (spec -> quality) review + fix-subagents on findings, exactly as Phases A and B.

**Goal:** Adopt Box2D-v3's two-layer body model — a stable *identity* layer (world slot + generation) over compacted, migratable *hot-data* arrays (per-set `BodySim` + awake-only `BodyState`) — then re-home Part 1's lane-wide SIMD contact solve onto the dense awake-compacted `BodyState` (deleting the world-slot/dummy-at-worldCount/sparse-gather machinery), and make graph coloring incremental (assigned at contact-create, released at destroy) with persistent per-color contiguous `ContactSim` arrays.

**Architecture:** Three independently-landable, independently-gated stages in strangler order. **Stage 1** introduces the two-layer storage + sleep/wake/create/remove migration (oracle-gated against the current slot-SoA, then flips + deletes the old columns) — behavior-preserving, no solver change. **Stage 2** re-homes the solver onto the compacted `BodyState` (the throughput/locality win). **Stage 3** moves coloring to incremental + per-color persistent `ContactSim`. Each stage keeps the engine green + deterministic and is re-measured with `[STEPPROF]` before the next.

**Tech Stack:** C++23, Core (presentation-free; `/MD` for Arcane.dll + static-CRT for ArcaneCore), `Arcane::Simd` wrapper, glm + std + sibling Physics headers only, Catch2 (`[physics]` / `[physics][island]` / `[physics][awakeset]` / `[simd]` / `[determinism]` + a new `[physics][twolayer]`), premake5 (at the **repo root**) / MSBuild via `Arcane.slnx`. `/fp:strict /arch:AVX2`, no fast-math (determinism). Branch `feature/arcane-physics-phaseC-two-layer` off `main` (Phase A+B are already on local `main` @ `b127af9`).

---

## Conventions

- **Branch:** create `feature/arcane-physics-phaseC-two-layer` off `main` (`b127af9`). Phase A + B are already merged to local `main`. Do **NOT** push (the user merges/pushes manually after a visual + CI gate). Commit per task with the trailer:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```
- **Kill stray procs before EVERY build** (a running `Loom.exe` locks the plugin copy; a stuck `ArcaneTests.exe` locks the exe):
  ```
  Get-Process Loom,ArcaneTests -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  ```
- **Build (Debug):**
  ```
  "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
  ```
  Append `-t:ArcaneTests` to build the test exe only (it pulls in `Core`). Replace `Debug` with `Release` for the Release gate.
- **Run tests (FROM the exe dir — required; the exe resolves data relative to cwd):**
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" ; ./ArcaneTests.exe "[physics]"
  ```
  Use `"[twolayer]"` / `"[awakeset]"` / `"[island]"` / `"[simd]"` / `"[determinism]"` for subsets, or a quoted full case name for one case. Release exe dir is `bin/Release-windows-x86_64-md/ArcaneTests`. This machine HAS a capable GPU (RTX 3070) — run `[gpu]` tests (do NOT exclude them) in the final per-stage gate.
- **ArcaneCore static-CRT (server flavor):** the same `Arcane/Core/src` sources compile under the static CRT and must stay clean. Build Debug + Release:
  ```
  "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
  ```
  (and again `-p:Configuration=Release`).
- **New `.cpp`/`.hpp` files → regen BOTH workspaces by ABSOLUTE path** (the Core + Tests globs are `%{prj.location}/src/**.cpp`/`**.hpp`, so a NEW file requires a regen; members added in-place to existing files do NOT). Run from `Arcane/` AND from `Server/` (NOT `GenerateProjects.bat` — it hangs on a `pause`; NOT a relative path):
  ```
  cd "D:/dev/starworks/Gacha/Arcane" ; & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  cd "D:/dev/starworks/Gacha/Server" ; & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  ```
- **clangd / IDE diagnostics are FALSE POSITIVES — MSVC/MSBuild + the test run are the only truth.** ASCII comments, explain WHY. C++23. Commands run via the PowerShell tool (chain with `;`, not `&&`).
- **DETERMINISM IS THE HARD CONTRACT** — run-twice-identical at every task. There are NO exact goldens; the `[physics]` + `[island]` + `[awakeset]` + `[simd]` + `[determinism]` behavioral suites + the run-twice cases + the SIMD lane-width-invariance test are the gate. Numerics may be re-baselined **deliberately within an invariant** (never weakening one) per `feedback_engine_evolves_not_frozen` — but Phase C is a *storage/indexing* restructure that should NOT change physics numerics; a shifted `[physics]` invariant is a RED FLAG to diagnose, not to re-baseline, unless a root cause proves a numeric improvement.
- **The compaction does NOT eliminate the gather instruction** (a SIMD batch references 8 arbitrary bodies). The Stage-2 win is *cache locality + removing the sparse-world-slot overhead* (the worldCount-sized dummy, holes, the read-only-B idempotency dance shrinks to a single shared tail). Frame + measure it as a locality win, not a "gathers are gone" win. The larger throughput payoff is Phase D (MT over the compacted colors); Phase C is its substrate.

---

## Name glossary (use these EXACTLY across all tasks)

- **Identity = the world slot.** A `BodyHandle.index` is the world slot; `m_gen[slot]` is the generation. Cross-references (`Contact.bodyA/bodyB`, `m_islands[].bodies`, the static/residency grids, `m_fxBody`) keep pointing at the **world slot** — they are NOT rewritten to localIndex. Phase C adds ONE indirection from identity -> hot data.
- `SolverSet` — an enum tag for which hot-data array a body lives in: `kSetStatic = 0`, `kSetAwake = 1`, `kSetSleeping = 2`. (No per-sleeping-island sets — a single sleeping set; the awake-set already won the per-step sleep cost in Phase B, so per-island sets buy only batch-wake memcpy, deferred.)
- `m_setIndex` — `std::vector<std::uint8_t>` — per world slot: which `SolverSet` the body's hot data lives in. Sized in `EnsureCapacity`.
- `m_localIndex` — `std::vector<std::uint32_t>` — per world slot: the body's position within its set's contiguous `BodySim`/`BodyState` arrays. `kNoLocal = 0xFFFFFFFFu` when the slot has no hot data (dead).
- `BodySim` — the per-set contiguous hot-data array of transform/COM/mass/damping, in **SoA** (parallel `std::vector` columns, NOT an AoS struct, to match the existing layout + keep SIMD-friendly): `posX/posY, prevX/prevY, angle, localCenterX/localCenterY, invMass, invInertia, bodyMass, bodyInertia, linDamp`, plus a back-reference `bodyId` (world slot) per row for swap-remove patching. One `BodySim` instance per `SolverSet`.
- `BodyState` — the **awake-only** per-set contiguous hot-data of solver-mutated state, SoA: `vx/vy/w` (velocity) + `dpx/dpy/dq` (TGS position delta). Lives ONLY in the awake set (`kSetAwake`). Static + sleeping bodies have NO `BodyState` (their velocity is implicitly zero). Indexed by the awake set's `localIndex`.
- `kAwakeCount` / `AwakeBodyCount()` — the number of rows in the awake `BodySim`/`BodyState` (the solver's working-set size). The awake set holds awake DYNAMIC bodies **and all KINEMATIC bodies** (kinematics never sleep, and the solver reads their velocity as B-endpoints). The shared scatter-safe dummy is the single tail row at index `kAwakeCount`.
- `kSharedStatic` — the awake-set `localIndex` value used for any static-body B-endpoint in the solver: a single shared zero-velocity row appended to the awake `BodyState` (so a static B gathers zero velocity without occupying a per-static row). (Stage 2 detail.)
- `BodySimAt(slot)` / `BodyStateAt(slot)` — internal resolvers: `set = m_setIndex[slot]; local = m_localIndex[slot]; return m_sets[set].sim` row `local`. The existing slot accessors (`PosSlot`/`VelSlot`/`GetAngle`/`LocalCenterSlot`/`SetVelSlot`/...) are re-pointed to resolve through these in Stage 1.
- `MigrateBody(slot, fromSet, toSet)` — swap-remove the body's `BodySim` (+`BodyState` if leaving/entering awake) row from `fromSet`, append into `toSet`, patch the swapped row's owner `m_localIndex[bodyId]`, update `m_setIndex[slot]`/`m_localIndex[slot]`. O(1).
- `[STEPPROF]` — the opt-in gated per-Step-phase timing (`ARCANE_STEPPROF`, default-off, from Phase B). The per-stage measurement gate.

---

## Target architecture (end state, after Stage 3)

```
IDENTITY (id-indexed by world slot; stable for a body's lifetime; cross-refs point here)
  m_gen, m_alive, m_btype, m_sensor, m_evtOn, m_bullet, m_fixedRotation, m_massOverride,
  m_islandId, m_sleepTimer, m_awake, m_bodyFixtures            (UNCHANGED slot-indexed)
  m_setIndex[slot], m_localIndex[slot]                          (NEW: the one indirection)

HOT DATA (per-set contiguous SoA; swap-remove compacted; migrates on sleep/wake)
  m_sets[kSetStatic ].sim     : BodySim (transform/mass=0)         + bodyId back-ref
  m_sets[kSetAwake  ].sim      : BodySim (awake dyn + kinematics)   + bodyId back-ref
  m_sets[kSetAwake  ].state    : BodyState (vel + delta)  <-- the solver's dense working set
  m_sets[kSetSleeping].sim     : BodySim (sleeping dynamics)        + bodyId back-ref
                                 (no BodyState -- sleeping vel is implicitly 0)

CONTACT GRAPH (Stage 3)
  per-color contiguous ContactSim arrays, color assigned at create / released at destroy
  (incremental); overflow color scalar. Warm-start stays on the pool Contact's manifold.

SOLVER (Stage 2)
  lane-wide colored SoA solve indexes m_sets[kSetAwake].state by awake localIndex;
  bodyIndexA/B are awake localIndices; gather over the dense array; the shared dummy +
  kSharedStatic are tail rows. No SyncIn/SyncOut sparse mirror; no worldCount dummy.
```

What carries forward UNCHANGED: the unified `Collide` narrowphase, speculative/CCD, warm-start-on-manifold (the pool `ManifoldPoint.normalImpulse/tangentImpulse` + Step stage-3b writeback), the `Arcane::Simd` wrapper, the persistent islands (Phase A) + the awake flag/sleep decision (Phase B), `ContactManager` (events-only). The Phase-B `m_awakeBodies`/`m_awakeIndex` awake-set is SUBSUMED by `m_setIndex==kSetAwake` + `m_localIndex` (Stage 1 deletes `m_awakeBodies`/`m_awakeIndex`, re-pointing `ForEachAwake`/`AwakeBodies()` at the awake set's `bodyId` array).

---

## File Structure

| File | Created/Modified | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/SolverSet.hpp` | **Created** | `SolverSet` enum + the `BodySimSoA` + `BodyStateSoA2` SoA column structs + the `PhysicsSet { BodySimSoA sim; BodyStateSoA2 state; std::vector<std::uint32_t> bodyId; }` aggregate + swap-remove helpers. Presentation-free, header-only. (Distinct from the existing solver-local `Solver/BodyStateSoA.hpp`, which Stage 2 retires.) |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | Modified | `SolverSet`/`m_setIndex`/`m_localIndex`/`m_sets[3]` members + `kNoLocal`; re-point the slot accessors (`PosSlot`/`VelSlot`/`GetAngle`/`LocalCenterSlot`/`InvMassSlot`/... + their setters) to resolve through the sets; `MigrateBody` decl; `ForEachAwake`/`AwakeBodies()` re-pointed at the awake set; delete the slot-SoA transform/velocity column decls in Stage 1's flip task. |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | Modified | Size the new columns in `EnsureCapacity`; populate the sets in `AddBody`; swap-remove in `RemoveBody`; `MigrateBody` impl; route every direct `m_posX[slot]`/`m_angle[slot]`/... read in the Step pipeline (narrowphase/emit/CCD/queries) through the accessors; migration calls at the sleep/wake seams; the Stage-1 oracle cross-check (Debug). |
| `Arcane/Core/src/Arcane/Physics/Island.cpp` | Modified | At the sleep apply (`UpdateSleep`), `MigrateBody(slot, kSetAwake, kSetSleeping)`; the awake-set removal becomes the migration. |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` | Modified (Stage 2) | Re-home `Solve`: index the world's awake `BodyState` by localIndex; `bodyIndexA/B` = awake localIndex; delete the `SyncIn`/`SyncOut` sparse mirror + the worldCount dummy; the shared-static + dummy tail rows. |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` | Modified (Stage 2) | `PackLane`/`PadLane`/`Build` pack awake localIndices; gather/scatter over the dense `BodyState`; the dummy = `kAwakeCount`. |
| `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp` | Deleted (Stage 2) | The solver-local sparse mirror is retired (the solver reads the world's compacted `BodyState` directly). |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactColoring.hpp/.cpp` | Modified (Stage 3) | Incremental coloring: assign at contact-create, release at destroy; a persistent per-color membership + per-color contiguous `ContactSim` arrays. |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (contact seams) | Modified (Stage 3) | Color assign in `TryCreateContact`'s `if(r.created)` block; release at the 4 `Destroy(id)` sites; maintain the per-color arrays. |
| `Arcane/Tests/src/PhysicsTwoLayerTest.cpp` | **Created** | New `[physics][twolayer]` behavioral tests: the identity<->hot-data invariant (set membership + localIndex round-trip) across create/sleep/wake/remove/recycle; migration back-ref-patch correctness; the run-twice determinism through migration; (Stage 2) the awake-compacted solve equivalence + lane-width invariance + the [STEPPROF] solve-locality measurement; (Stage 3) incremental-coloring determinism + the per-color invariant. |

---

# STAGE 1 — Two-layer body storage + migration (the foundation)

**Outcome:** the world stores body hot-data in compacted per-set arrays (`m_sets[kSetStatic/kSetAwake/kSetSleeping]`) reached through `m_setIndex`/`m_localIndex`; sleep/wake/create/remove migrate rows with swap-remove + back-ref patching; the slot accessors resolve through the sets; the slot-SoA transform/velocity columns and the Phase-B `m_awakeBodies`/`m_awakeIndex` are deleted. **The solver is UNCHANGED in Stage 1** (its `SyncIn` still mirrors world velocity into the solver-local `BodyStateSoA` via the accessors — those resolve through the sets, so the solver sees identical values). Behavior + determinism are byte-preserved; this stage is pure storage indirection. **De-risk:** the sets are built + migrated as a MIRROR validated against the live slot-SoA by a Debug cross-check (the oracle) BEFORE the accessors flip onto them (Tasks 3-4), then the flip + delete is one task (Task 5) with the behavioral suite as the gate.

### Task 1: `SolverSet.hpp` — the per-set hot-data containers + swap-remove (pure, tested in isolation)

**Files:** Create `Arcane/Core/src/Arcane/Physics/SolverSet.hpp`; Create `Arcane/Tests/src/PhysicsTwoLayerTest.cpp` (accretes all Stage tests). Regen BOTH workspaces (two new files).

Build the data structures + the O(1) swap-remove with back-ref patch as a standalone unit, with no `PhysicsWorld` dependency, so the migration arithmetic is proven before it is wired.

- [ ] **Step 1: write the failing test.** Create `PhysicsTwoLayerTest.cpp`:
  ```cpp
  // Physics Phase C: two-layer compacted storage -- BEHAVIORAL tests.
  // Companion to PhysicsAwakeSetTest.cpp / PhysicsIslandTest.cpp.
  // PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <vector>
  #include <catch2/catch_test_macros.hpp>
  #include <Arcane/Physics/PhysicsTypes.hpp>
  #include <Arcane/Physics/Shapes.hpp>
  #include <Arcane/Physics/Body.hpp>
  #include <Arcane/Physics/PhysicsWorld.hpp>
  #include <Arcane/Physics/SolverSet.hpp>
  using namespace Arcane::Physics;
  namespace {
      constexpr Real kStep = Real(1) / Real(60);
      BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
          BodyDef d; d.type=BodyType::Static; d.position=pos; d.shape=MakeAabb(hw,hh); d.friction=Real(0.6); return w.AddBody(d);
      }
      BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
          BodyDef d; d.type=BodyType::Dynamic; d.position=pos; d.shape=MakeAabb(hw,hh); d.density=Real(1); d.friction=Real(0.4); d.fixedRotation=true; return w.AddBody(d);
      }
  }
  // The PhysicsSet append/swap-remove must keep bodyId<->localIndex a bijection.
  TEST_CASE("PhysicsTwoLayer: PhysicsSet swap-remove patches the back-reference", "[physics][twolayer]")
  {
      PhysicsSet set;
      // Append 4 rows for world slots 10,11,12,13.
      REQUIRE(set.Append(10u) == 0u);
      REQUIRE(set.Append(11u) == 1u);
      REQUIRE(set.Append(12u) == 2u);
      REQUIRE(set.Append(13u) == 3u);
      REQUIRE(set.Count() == 4u);
      // Write distinct transforms so we can prove the row data moves with the body.
      set.sim.posX[0]=Real(10); set.sim.posX[1]=Real(11); set.sim.posX[2]=Real(12); set.sim.posX[3]=Real(13);
      // Swap-remove local 1 (slot 11): the last row (slot 13) moves into local 1.
      const std::uint32_t movedBody = set.SwapRemove(1u);
      REQUIRE(movedBody == 13u);                 // caller patches m_localIndex[13] = 1
      REQUIRE(set.Count() == 3u);
      REQUIRE(set.bodyId[1] == 13u);             // slot 13 now lives at local 1
      REQUIRE(set.sim.posX[1] == Real(13));      // its row data came with it
      REQUIRE(set.bodyId[0] == 10u);
      REQUIRE(set.bodyId[2] == 12u);
      // Swap-remove the LAST row (no move).
      const std::uint32_t none = set.SwapRemove(2u);
      REQUIRE(none == PhysicsSet::kNoMove);       // nothing moved
      REQUIRE(set.Count() == 2u);
  }
  ```
- [ ] **Step 2: regen BOTH workspaces, build Debug `-t:ArcaneTests`, verify FAIL** (`SolverSet.hpp` does not exist).
- [ ] **Step 3: implement `SolverSet.hpp`.**
  ```cpp
  #pragma once
  // SolverSet.hpp -- Box2D-v3 two-layer hot-data containers (Phase C).
  //
  // The world's IDENTITY (world slot + generation + topology) stays slot-indexed in
  // PhysicsWorld; a body's HOT DATA (transform/mass = BodySim, velocity/delta =
  // BodyState) lives in one of three per-set CONTIGUOUS SoA arrays (Static/Awake/
  // Sleeping) reached via m_setIndex[slot] + m_localIndex[slot]. Sleep/wake MIGRATE
  // a body's row between sets (swap-remove from source, append to target, patch the
  // swapped row's owner localIndex). BodyState is AWAKE-ONLY (static + sleeping
  // bodies have implicit-zero velocity, no BodyState row).
  //
  // SoA (parallel std::vector columns, NOT AoS) to match the existing layout and
  // keep the solver gather-friendly. PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <vector>
  #include <Arcane/Physics/PhysicsTypes.hpp>
  namespace Arcane { namespace Physics {

      enum class SolverSet : std::uint8_t { Static = 0, Awake = 1, Sleeping = 2 };
      inline constexpr std::uint8_t  kSetStatic   = 0u;
      inline constexpr std::uint8_t  kSetAwake    = 1u;
      inline constexpr std::uint8_t  kSetSleeping = 2u;
      inline constexpr std::uint32_t kNoLocal     = 0xFFFFFFFFu; // slot has no hot data

      // Transform / COM / mass / damping -- present for EVERY body (all 3 sets).
      struct BodySimSoA {
          std::vector<Real> posX, posY, prevX, prevY, angle;
          std::vector<Real> localCenterX, localCenterY;
          std::vector<Real> invMass, invInertia, bodyMass, bodyInertia, linDamp;
          void Resize(std::size_t n) {
              posX.resize(n); posY.resize(n); prevX.resize(n); prevY.resize(n); angle.resize(n);
              localCenterX.resize(n); localCenterY.resize(n);
              invMass.resize(n); invInertia.resize(n); bodyMass.resize(n); bodyInertia.resize(n); linDamp.resize(n);
          }
          // Move row `src` onto row `dst` (used by swap-remove).
          void MoveRow(std::size_t dst, std::size_t src) {
              posX[dst]=posX[src]; posY[dst]=posY[src]; prevX[dst]=prevX[src]; prevY[dst]=prevY[src]; angle[dst]=angle[src];
              localCenterX[dst]=localCenterX[src]; localCenterY[dst]=localCenterY[src];
              invMass[dst]=invMass[src]; invInertia[dst]=invInertia[src]; bodyMass[dst]=bodyMass[src]; bodyInertia[dst]=bodyInertia[src]; linDamp[dst]=linDamp[src];
          }
          void PopBack() {
              posX.pop_back(); posY.pop_back(); prevX.pop_back(); prevY.pop_back(); angle.pop_back();
              localCenterX.pop_back(); localCenterY.pop_back();
              invMass.pop_back(); invInertia.pop_back(); bodyMass.pop_back(); bodyInertia.pop_back(); linDamp.pop_back();
          }
          void PushDefault() { Resize(posX.size() + 1); }
      };

      // Velocity + TGS delta -- AWAKE-ONLY (only kSetAwake carries a populated one).
      struct BodyStateSoA2 {
          std::vector<float> vx, vy, w, dpx, dpy, dq;
          void Resize(std::size_t n) { vx.resize(n); vy.resize(n); w.resize(n); dpx.resize(n); dpy.resize(n); dq.resize(n); }
          void MoveRow(std::size_t dst, std::size_t src) { vx[dst]=vx[src]; vy[dst]=vy[src]; w[dst]=w[src]; dpx[dst]=dpx[src]; dpy[dst]=dpy[src]; dq[dst]=dq[src]; }
          void PopBack() { vx.pop_back(); vy.pop_back(); w.pop_back(); dpx.pop_back(); dpy.pop_back(); dq.pop_back(); }
          void PushDefault() { Resize(vx.size() + 1); }
      };

      // One solver set: contiguous BodySim (+ BodyState for the awake set) + the
      // per-row back-reference to the owning world slot (for swap-remove patching).
      struct PhysicsSet {
          static constexpr std::uint32_t kNoMove = 0xFFFFFFFFu; // SwapRemove of the last row moves nothing
          BodySimSoA   sim;
          BodyStateSoA2 state;          // populated only for the awake set
          std::vector<std::uint32_t> bodyId; // bodyId[local] = owning world slot
          bool hasState = false;        // true only for the awake set

          [[nodiscard]] std::uint32_t Count() const noexcept { return static_cast<std::uint32_t>(bodyId.size()); }

          // Append a row for `slot`, return its localIndex. Row data is default
          // (the caller fills it via the slot accessors / a write-through).
          std::uint32_t Append(std::uint32_t slot) {
              const std::uint32_t local = Count();
              sim.PushDefault();
              if (hasState) state.PushDefault();
              bodyId.push_back(slot);
              return local;
          }
          // Swap-remove row `local`: move the last row into `local` (if not last),
          // pop. Returns the world slot of the MOVED row (so the caller patches its
          // m_localIndex), or kNoMove if `local` was already the last row.
          std::uint32_t SwapRemove(std::uint32_t local) {
              const std::uint32_t last = Count() - 1u;
              std::uint32_t moved = kNoMove;
              if (local != last) {
                  sim.MoveRow(local, last);
                  if (hasState) state.MoveRow(local, last);
                  bodyId[local] = bodyId[last];
                  moved = bodyId[local];
              }
              sim.PopBack();
              if (hasState) state.PopBack();
              bodyId.pop_back();
              return moved;
          }
      };
  } } // namespace Arcane::Physics
  ```
- [ ] **Step 4: build Debug `-t:ArcaneTests`, run `"[twolayer]"`** — PASS.
- [ ] **Step 5: ArcaneCore clean (Debug + Release), commit.** `feat(arcane/physics): SolverSet hot-data containers (BodySim/BodyState SoA + O(1) swap-remove w/ back-ref patch)`. Trailer.

### Task 2: Route Step-pipeline body reads through slot accessors (mechanical, byte-identical)

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (add any missing transform/sim slot accessors), `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` + `Queries.cpp` (route direct reads). Extend `PhysicsTwoLayerTest.cpp`.

The flip in Task 5 changes ONLY the accessor bodies; for that to cover all consumers, every direct `m_posX[slot]`/`m_posY[slot]`/`m_angle[slot]`/`m_prevX[slot]`/`m_localCenterX[slot]`/`m_bodyMass[slot]`/`m_bodyInertia[slot]` read in the Step pipeline must first go through a slot accessor. The solver already uses `PosSlot`/`GetAngle`/`LocalCenterSlot`/`VelSlot`/`InvMassSlot` (verified). This task introduces the MISSING accessors and routes the remaining direct reads — a mechanical, behavior-identical change.

- [ ] **Step 1: inventory the direct-read sites.** Grep for direct body-column reads outside the accessor definitions:
  ```
  rg -n "m_posX\[|m_posY\[|m_prevX\[|m_prevY\[|m_angle\[|m_localCenterX\[|m_localCenterY\[|m_bodyMass\[|m_bodyInertia\[|m_invMass\[|m_invInertia\[|m_linDamp\[|m_velX\[|m_velY\[|m_angVel\[" Arcane/Core/src/Arcane/Physics
  ```
  The verified hot sites (confirm exact lines): Stage-1 snapshot `PhysicsWorld.cpp:1533-1571`; `UpdateContacts` sub-loops (`:2139-2285`, `:2298-2360`, `:2391-2476`); `EmitContactConstraints` (`:2612-2668`); `BulletSweep` (`:2762-2876`); `Queries.cpp` (`QueryAABB`/raycast). Leave the accessor DEFINITIONS, `AddBody`/`RemoveBody`/`EnsureCapacity` init, and the mass-recompute (`AddBody`) writing raw columns — those are storage-layer (Task 3-5 handle them).
- [ ] **Step 2: add the missing slot accessors to `PhysicsWorld.hpp`** (next to `PosSlot`/`VelSlot`), each currently a raw read (NO behavior change):
  ```cpp
  [[nodiscard]] Vec2 PrevSlot(std::uint32_t i) const noexcept { return Vec2(m_prevX[i], m_prevY[i]); }
  [[nodiscard]] Vec2 LocalCenterSlot(std::uint32_t i) const noexcept { return Vec2(m_localCenterX[i], m_localCenterY[i]); }
  [[nodiscard]] Real BodyMassSlot(std::uint32_t i) const noexcept { return m_bodyMass[i]; }
  [[nodiscard]] Real BodyInertiaSlot(std::uint32_t i) const noexcept { return m_bodyInertia[i]; }
  // (PosSlot / GetAngle / VelSlot / InvMassSlot / InvInertiaSlot / LinDampSlot already exist.)
  ```
  Confirm `PosSlot`, `GetAngle`, `LocalCenterSlot` already exist (the solver uses them); only add the genuinely-missing ones. Keep all `[[nodiscard]] ... noexcept` + inline.
- [ ] **Step 3: route the direct reads through the accessors** at the Step-pipeline sites from Step 1 (e.g. `m_posX[i]` -> `PosSlot(i).x`, `m_angle[c.bodyA]` -> `GetAngle(c.bodyA)`). Purely mechanical; the accessors still read the raw columns, so the sim is byte-identical.
- [ ] **Step 4: build Debug, run `[physics]` + `[island]` + `[awakeset]`.** Expected: GREEN + byte-identical (accessors are pass-throughs). If any case shifts, a read was mis-routed — diagnose.
- [ ] **Step 5: full Debug + Release `[physics]`, ArcaneCore clean, commit.** `refactor(arcane/physics): route Step-pipeline body reads through slot accessors (no behavior change; preps the Phase-C storage flip)`. Trailer.

### Task 3: Build the per-set hot-data as a MIRROR + a Debug cross-check oracle (no flip)

**Files:** `PhysicsWorld.hpp` (members + `MigrateBody` decl), `PhysicsWorld.cpp` (`EnsureCapacity`/`AddBody`/`RemoveBody` populate the sets; the cross-check). Extend `PhysicsTwoLayerTest.cpp`.

Add `m_setIndex`/`m_localIndex`/`m_sets[3]` and populate them at create/remove so each body's hot-data row MIRRORS its slot-SoA columns. The slot-SoA stays authoritative; the accessors are NOT yet flipped. A Debug-only `CrossCheckSets()` asserts the mirror equals the slot-SoA after each Step — the oracle that makes Task 5's flip safe.

- [ ] **Step 1: failing invariant test.** Append a test that drives create/sleep/wake/remove/recycle and asserts the set-membership invariant via new read-only probes `BodySetOf(handle)` + `BodyLocalIndexOf(handle)` (added in Step 3):
  ```cpp
  TEST_CASE("PhysicsTwoLayer: set membership + localIndex track body lifecycle", "[physics][twolayer]")
  {
      auto checkInvariant = [](PhysicsWorld& w) {
          // For every live body: its set matches its type/awake state, its localIndex
          // is in range, and the set's back-ref round-trips to the world slot.
          for (std::uint32_t s = 0; s < w.Count(); ++s) {
              if (!w.Alive(s)) continue;
              const std::uint8_t set = w.SetIndexOf(s);
              const std::uint32_t loc = w.LocalIndexOf(s);
              const BodyType t = w.TypeSlot(s);
              if (t == BodyType::Static)        REQUIRE(set == kSetStatic);
              else if (t == BodyType::Kinematic) REQUIRE(set == kSetAwake);   // kinematics never sleep
              else /* Dynamic */                 REQUIRE(set == (w.AwakeSlot(s) ? kSetAwake : kSetSleeping));
              REQUIRE(w.SetBodyIdAt(set, loc) == s);   // back-ref round-trips
          }
      };
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const BodyHandle b0 = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      const BodyHandle b1 = AddBox(w, Vec2(Real(0), Real(-40)), Real(5), Real(5));
      checkInvariant(w);
      for (int k = 0; k < 700; ++k) { w.Step(kStep); }   // settle -> sleep -> migrate to sleeping set
      checkInvariant(w);
      w.ApplyImpulse(b1, Vec2(Real(0), Real(-8000)));     // wake -> migrate back to awake set
      checkInvariant(w);
      w.RemoveBody(b0); checkInvariant(w);
      const BodyHandle b2 = AddBox(w, Vec2(Real(0), Real(-60)), Real(5), Real(5)); (void)b2;
      checkInvariant(w);
  }
  ```
- [ ] **Step 2: build, verify FAIL** (`SetIndexOf`/`LocalIndexOf`/`SetBodyIdAt` do not exist).
- [ ] **Step 3: add the members + probes + population.**
  - `PhysicsWorld.hpp`: `#include <Arcane/Physics/SolverSet.hpp>`; members `std::vector<std::uint8_t> m_setIndex; std::vector<std::uint32_t> m_localIndex; PhysicsSet m_sets[3];` (init `m_sets[kSetAwake].hasState = true;` in the ctor). Probes: `SetIndexOf(slot)`, `LocalIndexOf(slot)`, `SetBodyIdAt(set, local)` (returns `m_sets[set].bodyId[local]`), `BodySetOf(handle)`/`BodyLocalIndexOf(handle)` (handle->slot then the above). `MigrateBody(slot, from, to)` decl.
  - `EnsureCapacity`: `m_setIndex.resize(next, kSetStatic); m_localIndex.resize(next, kNoLocal);`.
  - `AddBody`: after the type is known, choose the set (`Static->kSetStatic`, `Kinematic->kSetAwake`, `Dynamic->kSetAwake` (new dynamics start awake)); `m_setIndex[idx]=set; m_localIndex[idx]=m_sets[set].Append(idx);` then **write the row from the freshly-computed slot-SoA values** via a `WriteSimRow(idx)` helper (copies `m_posX[idx]`... into `m_sets[set].sim.*[local]`, and for the awake set copies velocity into `state.*`). Call `WriteSimRow(idx)` at the END of `AddBody` (after mass is computed).
  - `RemoveBody`: BEFORE the slot is recycled, swap-remove its row: `const std::uint32_t moved = m_sets[set].SwapRemove(local); if (moved != PhysicsSet::kNoMove) m_localIndex[moved] = local; m_setIndex[idx]=kSetStatic; m_localIndex[idx]=kNoLocal;`. Place this next to the existing `RemoveFromAwakeSet(idx)` seam.
- [ ] **Step 4: add `CrossCheckSets()` (Debug-only oracle) + a write-through on every slot-SoA mutation.** The mirror must stay equal to the slot-SoA. The cleanest: make the slot-SoA the single writer and re-derive the mirror each Step end via a `SyncSetsFromSlots()` that copies every live body's slot-SoA -> its set row. Call it at the END of `Step()`; then `CrossCheckSets()` (Debug) asserts equality. (This avoids dual-write churn during Stage 1: the mirror is rebuilt from the oracle each step. Task 5 reverses the data flow.) Implement `SyncSetsFromSlots()` (per live body: `WriteSimRow(slot)`), and in Debug a `CrossCheckSets()` that asserts each `m_sets[SetIndexOf(s)].sim.posX[LocalIndexOf(s)] == m_posX[s]` etc. for every live body.
- [ ] **Step 5: wire `SyncSetsFromSlots()` + `assert(CrossCheckSets())` at the end of `Step()`.** Build Debug, run `[physics]` + `[twolayer]` + `[island]` + `[awakeset]`. Expected: GREEN (slot-SoA still authoritative; the mirror is validated but unused). The cross-check proves the migration arithmetic (Task 1) holds across the whole suite.
- [ ] **Step 6: full Debug + Release, ArcaneCore clean, commit.** `feat(arcane/physics): per-set hot-data mirror + Debug cross-check oracle (slot-SoA still authoritative)`. Trailer.

### Task 4: Migrate rows on sleep/wake (still mirrored + cross-checked)

**Files:** `Island.cpp` (sleep seam), `PhysicsWorld.cpp` (`MigrateBody` impl + the wake seams). Extend `PhysicsTwoLayerTest.cpp`.

Implement `MigrateBody` and call it at the sleep (`Island::UpdateSleep`) and wake (`WakeIsland` + the single-body wake paths) seams so a body's hot-data row physically moves between the awake and sleeping sets. The slot-SoA is still authoritative + the cross-check still runs, so a migration bug is caught immediately.

- [ ] **Step 1: failing test.** Append a test that after sleep the body is in `kSetSleeping` with NO awake-state row, and after wake it is back in `kSetAwake`, AND the awake set's `Count()` equals the awake-dynamic + kinematic count:
  ```cpp
  TEST_CASE("PhysicsTwoLayer: sleep/wake migrates the hot-data row between sets", "[physics][twolayer]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const BodyHandle b = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      REQUIRE(w.BodySetOf(b) == kSetAwake);
      for (int k = 0; k < 700; ++k) { w.Step(kStep); }
      REQUIRE_FALSE(w.IsAwake(b));
      REQUIRE(w.BodySetOf(b) == kSetSleeping);
      REQUIRE(w.AwakeSetCount() == 0u);             // no awake dynamics/kinematics left
      w.ApplyImpulse(b, Vec2(Real(0), Real(-8000)));
      REQUIRE(w.IsAwake(b));
      REQUIRE(w.BodySetOf(b) == kSetAwake);
      REQUIRE(w.AwakeSetCount() == 1u);
  }
  ```
- [ ] **Step 2: build, verify FAIL** (no migration yet -> body stays in `kSetAwake` per Task 3's `SyncSetsFromSlots` rebuild; `AwakeSetCount` may not exist).
- [ ] **Step 3: implement `MigrateBody`.**
  ```cpp
  void PhysicsWorld::MigrateBody(std::uint32_t slot, std::uint8_t from, std::uint8_t to) noexcept
  {
      if (from == to) return;
      const std::uint32_t local = m_localIndex[slot];
      // 1) Append a row into the target; 2) copy the source row's CURRENT data into
      //    it; 3) swap-remove from the source + patch the moved row's back-index.
      const std::uint32_t newLocal = m_sets[to].Append(slot);
      CopySimRow(m_sets[to].sim, newLocal, m_sets[from].sim, local);
      if (m_sets[from].hasState && m_sets[to].hasState)
          CopyStateRow(m_sets[to].state, newLocal, m_sets[from].state, local);
      // (Awake->Sleeping drops state -- the target has none; Sleeping->Awake gets a
      //  fresh zero state row -- a woken body's velocity is set by the caller right
      //  after, or stays zero.)
      const std::uint32_t moved = m_sets[from].SwapRemove(local);
      if (moved != PhysicsSet::kNoMove) m_localIndex[moved] = local;
      m_setIndex[slot]   = to;
      m_localIndex[slot] = newLocal;
  }
  ```
  Provide `CopySimRow`/`CopyStateRow` free helpers in `SolverSet.hpp` (field-by-field copy between two SoA instances at given indices).
- [ ] **Step 4: call `MigrateBody` at the seams.**
  - **Sleep** (`Island::UpdateSleep`, where it currently does `SetAwakeSlot(b,false); RemoveFromAwakeSet(b); ...`): replace `RemoveFromAwakeSet(b)` with `world.MigrateBody(b, kSetAwake, kSetSleeping);` (the awake flag write stays). The velocity-zero writes stay (they zero the slot-SoA, which `SyncSetsFromSlots` mirrors; once the row is in the sleeping set it carries the zeroed velocity in `BodySim`-adjacent state... but sleeping has NO state -> the zeroing must happen via the slot-SoA, which is still authoritative in Stage 1).
  - **Wake** (`WakeIsland` member loop + the single-body wake paths `SetVelocity`/`ApplyImpulse`x2/`Wake`): replace `AddToAwakeSet(b)` with `if (m_setIndex[b]==kSetSleeping) MigrateBody(b, kSetSleeping, kSetAwake);` (a kinematic or already-awake body is a no-op). Order: migrate BEFORE writing the new velocity (so the awake-state row exists).
  - Add `AwakeSetCount()` = `m_sets[kSetAwake].Count()`.
- [ ] **Step 5: reconcile `SyncSetsFromSlots()`.** Now that rows migrate, `SyncSetsFromSlots` must NOT rebuild membership (migration owns it) — it only refreshes the ROW DATA of each live body in its current set from the slot-SoA. Update it to: for each live body, `WriteSimRow(slot)` into `m_sets[SetIndexOf(slot)]` at `LocalIndexOf(slot)`. Keep `assert(CrossCheckSets())`.
- [ ] **Step 6: build Debug, run `[twolayer]` + `[physics]` + `[island]` + `[awakeset]`.** Expected GREEN; the cross-check holds across sleep/wake migrations. Run the determinism suite + the awake-set run-twice case. If the cross-check trips, the migration patch is wrong — diagnose (most likely a missing `m_localIndex[moved]` patch or a copy-before-remove ordering bug).
- [ ] **Step 7: full Debug + Release, ArcaneCore clean, commit.** `feat(arcane/physics): MigrateBody on sleep/wake (swap-remove between awake/sleeping sets; oracle still gating)`. Trailer.

### Task 5: Flip the accessors onto the sets; delete the slot-SoA hot columns + the Phase-B awake-set (THE flip)

**Files:** `PhysicsWorld.hpp` (re-point accessors, delete column decls + `m_awakeBodies`/`m_awakeIndex`), `PhysicsWorld.cpp` (delete column init + the slot-SoA writes; re-point `ForEachAwake`/`AwakeBodies`; delete `SyncSetsFromSlots`/`CrossCheckSets`/the mirror). Extend `PhysicsTwoLayerTest.cpp`.

Reverse the data flow: the per-set arrays become AUTHORITATIVE, the slot-SoA hot columns are deleted, and the accessors resolve through `m_setIndex`/`m_localIndex`. The Phase-B `m_awakeBodies`/`m_awakeIndex` are subsumed by the awake set (`ForEachAwake` walks `m_sets[kSetAwake].bodyId`). The behavioral suite + determinism are the gate (the oracle is gone — it has done its job across Tasks 3-4).

- [ ] **Step 1: re-point the slot accessors** (`PhysicsWorld.hpp`) to resolve through the sets, e.g.:
  ```cpp
  [[nodiscard]] Vec2 PosSlot(std::uint32_t i) const noexcept {
      const PhysicsSet& s = m_sets[m_setIndex[i]]; const std::uint32_t l = m_localIndex[i];
      return Vec2(s.sim.posX[l], s.sim.posY[l]);
  }
  [[nodiscard]] Vec2 VelSlot(std::uint32_t i) const noexcept {
      if (m_setIndex[i] != kSetAwake) return Vec2(Real(0), Real(0)); // static/sleeping -> implicit zero
      const PhysicsSet& s = m_sets[kSetAwake]; const std::uint32_t l = m_localIndex[i];
      return Vec2(Real(s.state.vx[l]), Real(s.state.vy[l]));
  }
  void SetVelSlot(std::uint32_t i, Vec2 v) noexcept {
      if (m_setIndex[i] != kSetAwake) return; // a non-awake body has no velocity state (zero); callers wake first
      PhysicsSet& s = m_sets[kSetAwake]; const std::uint32_t l = m_localIndex[i];
      s.state.vx[l]=static_cast<float>(v.x); s.state.vy[l]=static_cast<float>(v.y);
  }
  // ...same pattern for PrevSlot/GetAngle/LocalCenterSlot/InvMassSlot/InvInertiaSlot/
  //    LinDampSlot/BodyMassSlot/BodyInertiaSlot/AngVelSlot/SetAngVelSlot/SnapPrevToPos/
  //    CommitSlotPosition (writes posX/posY/angle into the set row).
  ```
  Velocity/angVel are `float` in `BodyState` (matching the solver); cast at the `Real`<->`float` boundary in the accessors (document the precision boundary).
- [ ] **Step 2: delete the slot-SoA hot columns + their init + writes.** Remove the decls `m_posX/m_posY/m_prevX/m_prevY/m_angle/m_localCenterX/m_localCenterY/m_invMass/m_invInertia/m_bodyMass/m_bodyInertia/m_linDamp/m_velX/m_velY/m_angVel` from `PhysicsWorld.hpp`; remove their `EnsureCapacity` resizes + their `AddBody`/`RemoveBody` direct writes (those now write through the set row via `WriteSimRow`/`CommitSlotPosition`/`SetVelSlot`). `AddBody`'s mass computation writes into the set row. Keep `m_sleepTimer/m_awake/m_islandId/m_btype/...` (identity/topology — slot-indexed, unchanged).
- [ ] **Step 3: subsume the Phase-B awake-set.** Delete `m_awakeBodies`/`m_awakeIndex`/`AddToAwakeSet`/`RemoveFromAwakeSet`/`kNotAwake`. Re-point `ForEachAwake` to walk the awake set's `bodyId` but **DYNAMIC-only** (the awake set now also holds kinematics; the Phase-B `ForEachAwake` contract is awake DYNAMICS): `for (std::uint32_t l=0; l<m_sets[kSetAwake].Count(); ++l) { const std::uint32_t s = m_sets[kSetAwake].bodyId[l]; if (m_btype[s]==Dynamic) fn(s); }`. Re-point `AwakeBodies()` accordingly (or replace its few callers with `ForEachAwake`). Every old `AddToAwakeSet`/`RemoveFromAwakeSet` call site is now a `MigrateBody` (Task 4) — verify none remain.
- [ ] **Step 4: delete the mirror.** Remove `SyncSetsFromSlots()`/`CrossCheckSets()`/the end-of-`Step` calls (the sets are now authoritative; there is nothing to cross-check against).
- [ ] **Step 5: build Debug.** Fix compile errors (any remaining direct `m_posX[...]` reference is now a missing-symbol error — route it through the accessor; this is the safety net that catches a missed Task-2 site). Run `[physics]` + `[twolayer]` + `[island]` + `[awakeset]` + `[determinism]`. Expected GREEN with identical behavior (the accessors return the same values, now from the set rows). Diagnose any shift (a likely culprit: a static/sleeping `VelSlot` returning zero where the slot-SoA previously held a stale nonzero — confirm that is correct, i.e. no consumer relied on a stale velocity).
- [ ] **Step 6: run-twice determinism through migration.** Append:
  ```cpp
  TEST_CASE("PhysicsTwoLayer: create/sleep/wake/remove is deterministic across two runs", "[physics][twolayer]")
  {
      auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
          std::vector<BodyHandle> boxes;
          for (int i = 0; i < 6; ++i) boxes.push_back(AddBox(w, Vec2(Real(0), Real(-10) - Real(9)*static_cast<Real>(i)), Real(4), Real(4)));
          for (int k = 0; k < 200; ++k) w.Step(kStep);
          w.RemoveBody(boxes[2]);
          const BodyHandle nb = AddBox(w, Vec2(Real(30), Real(-10)), Real(4), Real(4));
          w.ApplyImpulse(boxes[5], Vec2(Real(120), Real(-3000)));
          for (int k = 0; k < 500; ++k) w.Step(kStep);
          pos.clear(); awake.clear();
          for (std::size_t i = 0; i < boxes.size(); ++i) { if (i==2) continue; pos.push_back(w.Position(boxes[i])); awake.push_back(w.IsAwake(boxes[i])?1:0); }
          pos.push_back(w.Position(nb)); awake.push_back(w.IsAwake(nb)?1:0);
      };
      std::vector<Vec2> p1,p2; std::vector<int> a1,a2; run(p1,a1); run(p2,a2);
      REQUIRE(p1.size()==p2.size());
      for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); REQUIRE(a1[i]==a2[i]); }
  }
  ```
- [ ] **Step 7: full Debug + Release, ArcaneCore clean, commit.** `refactor(arcane/physics): two-layer storage authoritative -- accessors resolve through m_sets; delete slot-SoA hot columns + Phase-B awake-set`. Trailer.

### Task 6: Stage-1 gate + `[STEPPROF]` no-regression

- [ ] **Step 1: full Debug gate.** Kill strays, build Debug, run `[physics]` then the WHOLE suite incl. `[gpu]`:
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" ; ./ArcaneTests.exe "[physics]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" ; ./ArcaneTests.exe
  ```
- [ ] **Step 2: full Release gate** (whole suite from the Release exe dir; determinism under NDEBUG + optimizer).
- [ ] **Step 3: ArcaneCore static-CRT gate** (Debug + Release): clean.
- [ ] **Step 4: `[STEPPROF]` no-regression.** Temporarily `#define ARCANE_STEPPROF 1`, build Release, run the hidden `[stepprof]` churn/settled measurement (2000-box pile). Expected: narrowphase + solve within noise of the pre-Stage-1 baseline (narrow ~4.2ms churn, solve ~1.1ms churn) — the indirection added 2 loads per access; confirm it did NOT materially regress narrowphase. Record numbers in the commit body. Revert the `#define`.
- [ ] **Step 5: visual gate prep.** Note in the commit that the user should run the Dist `Loom.exe` Sandbox (scenes 0-8) before merge. Commit: `test(arcane/physics): Stage-1 two-layer storage full gate (Debug+Release [gpu] both + ArcaneCore + determinism + STEPPROF no-regression)`. Trailer.

---

# STAGE 2 — Re-home the lane-wide solve onto the compacted awake state (the win)

**Outcome:** the solver indexes a DENSE awake-compacted body-state scratch (`awakeCount+1`, contiguous, indexed by the awake-set `localIndex`) instead of the sparse `worldCount+1` slot-indexed mirror. `SyncIn`/`SyncOut` become CONTIGUOUS copies (`awakeCount` rows), not per-slot sparse gathers; the SIMD `bodyIndexA/B` lanes carry awake `localIndex`; the scatter-safe dummy + the shared-static row collapse to the single tail row at `awakeCount`. Lane-width invariance + determinism must still hold (the colored solve is unchanged math over re-indexed lanes). **This is the gather-locality win the milestone targets** — measure it with `[STEPPROF]` (expect the solve fraction to drop on the churn scene; it will NOT go to zero — the gather instruction remains, only its locality + the sparse-slot overhead improve).

### Task 7: Re-home the solve onto the awake-compacted scratch (`localIndex` packing + contiguous sync + dummy = awakeCount)

**Files:** `Solver/SoftStep.cpp` (the `Solve` driver + the `SyncIn`/`SyncOut`/`Integrate*`/`Finalize` passes), `Solver/SoftStep.hpp` (the scratch member type), `Solver/ContactConstraintSimd.hpp` (`PackLane`/`PadLane` index mapping + the `bs` param type), DELETE `Solver/BodyStateSoA.hpp` (superseded by `BodyStateSoA2` in `SolverSet.hpp`). Extend `PhysicsTwoLayerTest.cpp`. **Regen** (a deleted file changes the Core glob).

The awake set's `localIndex` (from Stage 1) is the dense solver index. The solver keeps a `BodyStateSoA2 m_solverState` scratch sized `awakeCount+1`: `SyncIn` copies the awake set's `state` rows in contiguously (and zeros dp/dq + the dummy tail); the solve runs over `m_solverState` by `localIndex`; `SyncOut` copies the awake DYNAMICS' velocity back. `PackLane` maps each body slot to its awake `localIndex` (A: `m_localIndex[bodyA]`; B dynamic/kinematic: `m_localIndex[bodyB]`; B static or span or padding: `dummyIndex = awakeCount`, the shared zero tail).

- [ ] **Step 1: equivalence baseline test.** Append a test capturing the PRE-re-home settle result as the regression reference (it must be byte-identical after the re-home — pure re-indexing, no math change):
  ```cpp
  TEST_CASE("PhysicsTwoLayer: awake-compacted solve settles identically + deterministically", "[physics][twolayer]")
  {
      auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
          std::vector<BodyHandle> boxes;
          for (int i = 0; i < 5; ++i) boxes.push_back(AddBox(w, Vec2(Real(0), Real(-10) - Real(9)*static_cast<Real>(i)), Real(4), Real(4)));
          for (int k = 0; k < 900; ++k) w.Step(kStep);
          pos.clear(); awake.clear();
          for (const BodyHandle b : boxes) { pos.push_back(w.Position(b)); awake.push_back(w.IsAwake(b)?1:0); }
      };
      std::vector<Vec2> p1,p2; std::vector<int> a1,a2; run(p1,a1); run(p2,a2);
      REQUIRE(p1.size()==p2.size());
      for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); REQUIRE(a1[i]==a2[i]); }
  }
  ```
  Build + run on the PRE-re-home build -> PASS (the baseline the re-home must preserve).
- [ ] **Step 2: change the scratch + sync.** In `SoftStep.hpp`, change `m_bodyState`'s type to `BodyStateSoA2` (from `SolverSet.hpp`); `#include <Arcane/Physics/SolverSet.hpp>`; remove `#include <Arcane/Physics/Solver/BodyStateSoA.hpp>` and DELETE that file. In `SoftStep.cpp::Solve`:
  - `const std::uint32_t awakeCount = w.AwakeSetCount();` (the Stage-1 accessor = `m_sets[kSetAwake].Count()`; the awake set holds awake dynamics + kinematics — the solver's dense index space). `const std::int32_t dummyIndex = (std::int32_t)awakeCount;`. `m_bodyState.Resize(awakeCount + 1u);`.
  - Replace `SyncIn(w)` with a CONTIGUOUS copy of the awake set's state: `for l in [0,awakeCount): m_bodyState.vx[l]=awakeState.vx[l]; ...; m_bodyState.dpx[l]=dpy[l]=dq[l]=0;` then zero the dummy tail row `awakeCount`. (Kinematic rows are copied too — their velocity is read as B-endpoints.) Provide it as a method `SyncInCompacted(w)` on the solver.
  - The coloring edge build (`SoftStep.cpp:597-610`): map slots to localIndex — `e.a = w.LocalIndexOf(cc.bodyA); e.b = bDyn ? w.LocalIndexOf(cc.bodyB) : e.a;` and color over `awakeCount` bodies (`ColorConstraints(m_edges, awakeCount)`). (Stage 3 replaces this with the persistent coloring; Stage 2 keeps the per-step greedy but over localIndex.)
- [ ] **Step 3: re-index `PackLane`.** In `ContactConstraintSimd.hpp::PackLane`, the source `ContactConstraint` carries world slots (`cc.bodyA`/`cc.bodyB`); add a slot->localIndex remap. Cleanest: `Build`/`PackLane` take a `const PhysicsWorld& w` (or a `const std::uint32_t* slotToLocal` map) and set `dst.bodyIndexA[L] = (int32)w.LocalIndexOf(cc.bodyA);` and `dst.bodyIndexB[L] = cc.bodyBIsBody ? (int32)w.LocalIndexOf(cc.bodyB) : dummyIndex;`. A static B's `LocalIndexOf` would be its (sleeping/static-set) localIndex — WRONG for the solver, which needs the shared zero tail. So gate it: `dst.bodyIndexB[L] = (cc.bodyBIsBody && (cc.invMassB > 0 || w.SetIndexOf(cc.bodyB)==kSetAwake)) ? (int32)w.LocalIndexOf(cc.bodyB) : dummyIndex;` — i.e. a dynamic OR kinematic (awake-set) B uses its localIndex; a STATIC B (not in the awake set) uses the dummy (shared zero, since statics are zero-velocity and read-only). `PadLane` already targets `dummyIndex`. (Document: kinematic = awake-set B with `invMassB==0` but real authored velocity in its awake-state row; static = not-awake-set B, velocity implicitly zero -> the dummy zero tail is correct.)
- [ ] **Step 4: the integrate/finalize/SyncOut passes** in `SoftStep.cpp` iterate the awake DYNAMICS by localIndex. `ForEachAwake` (Stage 1: awake dynamics only, gives world slots) -> for each, `l = w.LocalIndexOf(slot)`; operate on `m_bodyState.*[l]`. `FinalizePositionsSoA` commits via `w.CommitSlotPosition(slot, p, a)` (which writes the BodySim row). `SyncOut` copies `m_bodyState.vx/vy/w[l]` back to the awake set's `state` for each awake dynamic (kinematics are read-only -> not written). NOTE: dp/dq live in `m_bodyState` (the scratch), not the awake set's state — the awake set's `state.dpx/dpy/dq` are unused scratch columns (or drop them from the awake set's BodyStateSoA2 and keep dp/dq solver-only; simpler: keep the columns, leave them zero in the awake set — the solver scratch owns dp/dq).
- [ ] **Step 5: build Debug, run `[twolayer]` + `[physics]` + `[simd]` + `[island]` + `[awakeset]`.** Expected: the equivalence test + the whole suite GREEN with byte-identical settle results (pure re-indexing). Diagnose any shift: most likely a static-vs-kinematic B mis-gate (a static B accidentally using its sleeping-set localIndex instead of the dummy -> gathering a sleeping body's stale velocity).
- [ ] **Step 6: lane-width invariance.** Run the `[simd]` lane-width-invariance test (`PhysicsSimdSolverTest.cpp` "lane-wide solve is packing-width invariant + matches scalar"). It pins WIDE==NARROW bit-identical + the scalar-oracle match + the scatter-corruption guard — these must STILL hold (the re-home changed the index space, not the math). If the scatter-corruption guard fails, the dummy/shared-static collapse let a read-only lane clobber a real row — fix the `dynB` mask / dummy targeting.
- [ ] **Step 7: full Debug + Release + ArcaneCore clean, commit.** `perf(arcane/physics): re-home the lane-wide solve onto the awake-compacted state (localIndex lanes; contiguous sync; dummy=awakeCount; delete the sparse worldCount mirror)`. Trailer.

### Task 8: Stage-2 gate + `[STEPPROF]` solve-locality win

- [ ] **Step 1: full Debug + Release gate** (whole suite incl. `[gpu]` both backends; `[simd]` lane-width invariance; `[determinism]` run-twice).
- [ ] **Step 2: ArcaneCore static-CRT** Debug + Release clean.
- [ ] **Step 3: `[STEPPROF]` win measurement.** `#define ARCANE_STEPPROF 1`, build Release, run the hidden `[stepprof]` churn/settled measurement. Compare the `solve` bucket vs the Stage-1 baseline (churn ~1.08ms). Expected: a measurable drop (dense contiguous state -> better gather locality + no worldCount-sized sparse mirror copy). Record the before/after in the commit body. If the solve did NOT drop, note it (the locality win may be small at this scale; the structural value is the Phase-D substrate) — do NOT treat a small delta as a failure (the user accepted that the perf payoff is modest + the architecture is the goal). Revert the `#define`.
- [ ] **Step 4: commit.** `test(arcane/physics): Stage-2 re-home full gate + STEPPROF solve-locality measurement`. Body: the gate + the measured solve delta. Trailer.

---

# STAGE 3 — Incremental coloring + persistent per-color membership

**Outcome:** a contact's color is assigned ONCE at create (lowest free color for its dynamic endpoints, maintained by a per-body color bitmask) and released at destroy — never recomputed per Step. The solver groups the per-step emitted constraints by their source contact's persistent color (replacing the per-step greedy `ColorConstraints`). The win: the O(contacts x colors) per-step recolor pass is gone; the persistent color membership is the stable parallel-work substrate Phase D needs.

**Determinism note (the crux this stage must re-establish):** today's determinism anchor for coloring is that `EmitContactConstraints` canonical-sorts the contacts before the per-step recolor. Incremental coloring assigns the color at CREATE (in `UpdateContacts`' broadphase-pair + static-candidate order) — so the **create order must be deterministic** for the color assignment to be run-twice-identical. It is (same broadphase tree -> same pair order; ascending-id pool destroy), and the colored solve is within-color order-independent (coloring guarantees one contact per body per color) — but this MUST be pinned by a run-twice test (Task 11).

### Task 9: Persistent color on the Contact + incremental assign/release (maintained, not yet consumed)

**Files:** `Contact.hpp` (the `color` field), `PhysicsWorld.hpp` (`m_bodyColorMask` + the per-color membership + the assign/release helpers), `PhysicsWorld.cpp` (assign in `TryCreateContact`, release at the 4 `Destroy(id)` sites). Extend `PhysicsTwoLayerTest.cpp`.

Add the persistent coloring maintained at the contact lifecycle seams. The solver does NOT yet consume it (it keeps the per-step `ColorConstraints`); a Debug cross-check asserts the persistent coloring is a VALID coloring (no two same-color contacts share a dynamic body) — the oracle for Task 10.

- [ ] **Step 1: failing test.** Append a test that after stepping a multi-body pile, the persistent coloring is valid (probe `ContactColorOf(contactId)` + a validity walk):
  ```cpp
  TEST_CASE("PhysicsTwoLayer: persistent contact coloring is valid (no same-color dynamic-body clash)", "[physics][twolayer]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
      for (int i = 0; i < 30; ++i) AddBox(w, Vec2(Real(-40) + Real(3)*static_cast<Real>(i%20), Real(-20) - Real(9)*static_cast<Real>(i/20)), Real(4), Real(4));
      for (int k = 0; k < 60; ++k) w.Step(kStep);     // build a churny contact graph
      // No two touching contacts in the SAME color may share a DYNAMIC body.
      REQUIRE(w.ValidatePersistentColoring());        // Debug+Release probe (walks the pool)
  }
  ```
- [ ] **Step 2: build, verify FAIL** (`ValidatePersistentColoring`/`ContactColorOf` do not exist).
- [ ] **Step 3: add the storage + helpers.**
  - `Contact.hpp`: add `std::uint8_t color = kInvalidColor;` (`inline constexpr std::uint8_t kInvalidColor = 0xFFu;` = overflow/unassigned). Reset to `kInvalidColor` on pool-slot recycle (in `ContactPool::EnsurePair`'s reset path).
  - `PhysicsWorld.hpp`: `std::vector<std::uint32_t> m_bodyColorMask;` (per body slot: a 12-bit color-occupancy mask; sized in `EnsureCapacity`, default 0). `std::vector<std::vector<std::uint32_t>> m_colorContacts;` (per color: the contact ids; sized `kColorCount`) — the persistent membership. Helpers `AssignContactColor(id, aSlot, bSlot, aDyn, bDyn)` + `ReleaseContactColor(id)`.
  - `AssignContactColor` (called when a NEW solver-relevant body-body contact is created):
    ```cpp
    void PhysicsWorld::AssignContactColor(std::uint32_t id, std::uint32_t a, std::uint32_t b, bool aDyn, bool bDyn) {
        // Lowest color free for BOTH dynamic endpoints (a static endpoint never blocks).
        int chosen = -1;
        for (int k = 0; k < kColorCount; ++k) {
            const std::uint32_t bit = 1u << k;
            const bool aFree = !aDyn || !(m_bodyColorMask[a] & bit);
            const bool bFree = !bDyn || !(m_bodyColorMask[b] & bit);
            if (aFree && bFree) { chosen = k; break; }
        }
        Contact& c = m_contactPool.Get(id);
        if (chosen < 0) { c.color = kInvalidColor; return; }   // overflow
        c.color = static_cast<std::uint8_t>(chosen);
        const std::uint32_t bit = 1u << chosen;
        if (aDyn) m_bodyColorMask[a] |= bit;
        if (bDyn) m_bodyColorMask[b] |= bit;
        m_colorContacts[chosen].push_back(id);
    }
    ```
  - `ReleaseContactColor(id)`: if `c.color != kInvalidColor`, clear the bit for each dynamic endpoint (one contact per body per color -> the bit clears cleanly), swap-remove `id` from `m_colorContacts[c.color]`, set `c.color = kInvalidColor`. (Read the endpoints from the Contact's cached `bodyA`/`bodyB` + recompute dyn-ness from `m_btype`/`m_invMass`.)
  - `ValidatePersistentColoring()` (Debug+Release probe): for each color k, walk `m_colorContacts[k]`, assert no dynamic body appears twice; assert each listed contact's `color==k`.
  - `ContactColorOf(id)` probe.
- [ ] **Step 4: call assign at create / release at destroy.**
  - **Create:** in `TryCreateContact`'s `if (r.created)` block (`PhysicsWorld.cpp:2077-2082`), after setting `c.bodyA/bodyB`, if `solverRelevant && bIsBody` -> `AssignContactColor(r.id, ia, ib, da, db);` (the oriented slots + dyn flags computed earlier in the function). A sensor/non-solver contact stays `kInvalidColor` (never solved).
  - **Destroy:** at each `m_contactPool.Destroy(id)` site (`:2383`, `:2420`, `:1826`, `:1853`), call `ReleaseContactColor(id)` IMMEDIATELY BEFORE `Destroy(id)` (while the Contact still holds its cached endpoints + color).
  - **`EnsureCapacity`:** `m_bodyColorMask.resize(next, 0u);`. Ctor: `m_colorContacts.resize(kColorCount);`.
- [ ] **Step 5: build Debug, run `[twolayer]` + `[physics]` + `[island]`.** Expected GREEN: the persistent coloring is maintained + valid (Task 9 test passes) while the solver still uses the per-step `ColorConstraints` (behavior byte-identical). The `ValidatePersistentColoring` probe is the oracle for Task 10. NOTE: a body's color mask must be CLEAR when it has no contacts — add a Debug assertion in `RemoveBody` that `m_bodyColorMask[idx]==0` after `DestroyContactsForBody` (every contact released its bits).
- [ ] **Step 6: full Debug + Release + ArcaneCore clean, commit.** `feat(arcane/physics): persistent incremental contact coloring (assign-at-create / release-at-destroy; per-body color mask); not yet consumed`. Trailer.

### Task 10: Feed the solver from the persistent coloring (replace the per-step recolor)

**Files:** `Solver/SoftStep.cpp` (the `Solve` driver coloring section), `PhysicsWorld.hpp`/`.cpp` (expose the per-color membership + per-constraint color to the solver context). Extend `PhysicsTwoLayerTest.cpp`.

Replace the per-step `ColorConstraints(m_edges, ...)` with a grouping of the per-step emitted constraints by their source contact's persistent color. The Build packs per persistent color; constraints whose contact is `kInvalidColor` (overflow) OR which are spans (`sourceContactId == kNoContact`) go to the overflow scalar path.

- [ ] **Step 1: tag emitted constraints with their persistent color.** In `EmitContactConstraints` (`PhysicsWorld.cpp`), when a constraint is emitted with `out.back().sourceContactId = id` (`:2716`), also set `out.back().color = m_contactPool.Get(id).color;` (add `std::uint8_t color = kInvalidColor;` to `ContactConstraint` in `Solver.hpp`). Spans leave the default `kInvalidColor`.
- [ ] **Step 2: replace the coloring step in `SoftStep::Solve`.** Delete the `m_edges` build + `m_coloring = ColorConstraints(...)` (`SoftStep.cpp:597-611`). Instead bucket the emitted constraints by `cc.color`:
  ```cpp
  // Group the emitted constraints by their persistent contact color. A
  // kInvalidColor constraint (overflow contact OR a span) -> the scalar overflow
  // path. Within-color order = emit (canonical-sorted) order; the colored solve
  // is order-independent (one contact per dynamic body per color), so this is
  // deterministic + bit-identical regardless of bucket order.
  for (auto& bucket : m_colorRefs) bucket.clear();      // m_colorRefs: vector<vector<uint32>>[kColorCount]
  m_overflowRefs.clear();
  for (std::uint32_t c = 0; c < ctx.contactCount; ++c) {
      const std::uint8_t col = ctx.contacts[c].color;
      if (col < kColorCount) m_colorRefs[col].push_back(c);
      else                   m_overflowRefs.push_back(c);
  }
  ```
  Then Build per color from `m_colorRefs[k]`, and the overflow path solves `m_overflowRefs` (the existing `Overflow*` functions, fed from `m_overflowRefs` instead of `m_coloring.overflow`). `m_colorRefs`/`m_overflowRefs` are new `SoftStep` members; `m_coloring`/`m_edges` are deleted. `LastOverflowCount()` returns `m_overflowRefs.size()`.
- [ ] **Step 3: build Debug, run `[twolayer]` + `[physics]` + `[simd]` + `[island]` + `[awakeset]`.** Expected GREEN with byte-identical settle results (a VALID coloring -- whether per-step-greedy or persistent -- yields the same solve, since the colored solve is coloring-order-independent given validity). Diagnose any shift (most likely: a constraint whose contact color is stale -- e.g. color not re-validated when a body's contact set changed; or a span mis-bucketed).
- [ ] **Step 4: prove no within-color clash at solve time.** Add a Debug assertion in `Build` (or just before it) that within each `m_colorRefs[k]`, no dynamic body slot appears twice (the active emitted subset of a valid persistent coloring is still valid). Run `[physics]` Debug to exercise it. This documents WHY grouping-by-persistent-color is solve-safe.
- [ ] **Step 5: full Debug + Release + ArcaneCore clean, commit.** `perf(arcane/physics): solver consumes the persistent contact coloring (delete the per-step greedy recolor)`. Trailer.

### Task 11: Stage-3 gate + determinism + `[STEPPROF]`

- [ ] **Step 1: incremental-coloring run-twice determinism.** Append a run-twice test over a create/destroy-churning scene (bodies removed + re-added so contacts are destroyed + recreated, exercising color release + reassign), asserting bit-identical final positions + awake states AND identical per-contact colors across two runs:
  ```cpp
  TEST_CASE("PhysicsTwoLayer: incremental coloring is deterministic across two runs (create/destroy churn)", "[physics][twolayer]")
  {
      auto run = [](std::vector<Vec2>& pos) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
          std::vector<BodyHandle> b;
          for (int i = 0; i < 16; ++i) b.push_back(AddBox(w, Vec2(Real(-20) + Real(3)*static_cast<Real>(i), Real(-20)), Real(4), Real(4)));
          for (int k = 0; k < 120; ++k) w.Step(kStep);
          w.RemoveBody(b[4]); w.RemoveBody(b[9]);                       // destroy contacts -> release colors
          for (int k = 0; k < 60; ++k) w.Step(kStep);
          b.push_back(AddBox(w, Vec2(Real(0), Real(-30)), Real(4), Real(4)));  // recreate -> reassign
          for (int k = 0; k < 200; ++k) w.Step(kStep);
          pos.clear(); for (std::size_t i = 0; i < b.size(); ++i) { if (i==4||i==9) continue; pos.push_back(w.Position(b[i])); }
      };
      std::vector<Vec2> p1,p2; run(p1); run(p2);
      REQUIRE(p1.size()==p2.size());
      for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); }
  }
  ```
- [ ] **Step 2: full Debug + Release gate** (whole suite incl. `[gpu]` both backends; `[simd]` lane-width invariance; `[determinism]`).
- [ ] **Step 3: ArcaneCore static-CRT** Debug + Release clean.
- [ ] **Step 4: `[STEPPROF]`.** `#define ARCANE_STEPPROF 1`, build Release, run the churn measurement. Expected: the `solve` bucket's coloring sub-cost is gone (no per-step recolor); confirm no regression elsewhere. Record. Revert.
- [ ] **Step 5: commit.** `test(arcane/physics): Stage-3 incremental coloring full gate + determinism + STEPPROF`. Trailer.

---

# Phase C completion gate + memory

### Task 12: Phase-C holistic gate + memory update

- [ ] **Step 1: whole-Phase-C gate.** Kill strays. Full ArcaneTests **Debug AND Release**, whole suite (incl. `[gpu]` D3D12 + Vulkan — assert `Arcane::RenderErrorCount()==0`, no validation noise), from each config's exe dir. Record the assertion/case counts.
- [ ] **Step 2: ArcaneCore static-CRT** Debug + Release: clean.
- [ ] **Step 3: determinism + lane-width invariance** consolidated: `[determinism]` + `[simd]` + the `[twolayer]` run-twice cases (Stages 1/2/3) all green both configs.
- [ ] **Step 4: `[STEPPROF]` Phase-C summary.** Build a measuring Release, dump the churn + settled breakdown; record the Stage-0 (pre-Phase-C) -> Stage-1 -> Stage-2 -> Stage-3 progression of the `solve` + `narrow` buckets in the final commit body. State plainly whether the gather-locality win landed and its size (it is expected MODEST single-threaded; the architecture + the Phase-D substrate are the deliverable).
- [ ] **Step 5: visual gate handoff.** The user runs the Dist `Loom.exe` Sandbox (scenes 0-8: settle piles, the whisk stress, the 10k stress, spawn/drag/throw) before merge. Note this in the commit; do NOT merge or push.
- [ ] **Step 6: update memory + final commit.** Update `project_arcane_physics_rearchitecture` (Phase C DONE + the measured result + the two-layer end state; next = Phase D multithreading; pause for the user before D). Update `project_arcane_physics_phaseC_progress` (the as-built record + any carry-forwards). Final commit: `feat(arcane/physics): Phase C -- Box2D-v3 two-layer compacted storage + incremental coloring + gather-free SIMD re-home`. Body: the full gate + the STEPPROF progression. Trailer. **Do NOT push** (the user merges after the visual + CI gate).

---

## Self-Review Notes

**Spec coverage (`docs/superpowers/specs/2026-06-25-...-data-model-rearchitecture-design.md` §4-5, Phase C):**
- Identity layer (slot + generation, cross-refs point here) + the `setIndex`/`localIndex` indirection -> Stage 1 (Tasks 3-5). Cross-refs (`Contact.bodyA/bodyB`, island lists, grids, `m_fxBody`) keep pointing at the world slot -> NO cross-ref rewrite needed (the slot IS the identity), which removes the spec's largest-risk item (remap-cross-refs-on-migration) because only ONE indirection (`m_setIndex`/`m_localIndex`) is added, not a re-keying.
- Per-set contiguous `BodySim` + awake-only `BodyState`, swap-remove migration + back-ref patching -> Stage 1 (`SolverSet.hpp` Task 1; `MigrateBody` Task 4). Sets = Static/Awake/Sleeping (single sleeping set, NOT per-island — justified: Phase B already won the per-step sleep cost; per-island sets buy only batch-wake memcpy, a deferred refinement; noted in the glossary).
- Constraint graph = per-color contiguous arrays + incremental coloring + scalar overflow -> Stage 3. Part 1's lane-wide solve re-homed onto the dense lanes -> Stage 2.
- Determinism + lane-width invariance gate at every stage; `[STEPPROF]` per-stage measurement -> every stage's gate task.

**Determinism (the hard contract):** preserved at every task. Stage 1/2 are pure storage/indexing changes (no math change) -> behavioral suites must stay byte-identical (a shift = a bug to diagnose, NOT a re-baseline). Stage 3's incremental coloring re-establishes determinism via the deterministic create order + the within-color order-independence of the colored solve (Task 11 run-twice pins it). The awake-set iteration order is append/swap-remove (non-ascending, inherited from Phase B) -- safe because every rerouted loop does independent per-body work.

**The de-risking discipline (oracle-gated swaps, as in Phase A/3):**
1. Stage 1 builds the sets as a MIRROR validated by a Debug cross-check against the live slot-SoA (Tasks 3-4) BEFORE flipping authority + deleting the old columns (Task 5). The migration arithmetic is proven across the whole suite while the old code is still the source of truth.
2. Stage 3 maintains the persistent coloring + a `ValidatePersistentColoring` oracle (Task 9) BEFORE the solver consumes it (Task 10).
3. Task 2 (route reads through accessors) localizes the Stage-1 flip to the accessor bodies -- and any missed direct `m_posX[...]` reference becomes a compile error after the columns are deleted (a free safety net).

**Soft spots (called out for the implementer + reviewer):**
1. **The Stage-1 flip (Task 5) is the highest-risk single task** — deleting the slot-SoA columns + flipping authority. Mitigations: the oracle cross-check ran clean across Tasks 3-4; the accessor routing (Task 2) means consumers don't change; missed sites are compile errors; the behavioral + determinism suites gate it. If the cross-check ever trips in Tasks 3-4, STOP and fix before Task 5.
2. **`VelSlot` on a non-awake body returns zero** (static/sleeping have no `BodyState`). Verify no consumer relied on a stale nonzero velocity of a non-awake body (sleep already zeroes velocity, so this is consistent — but a kinematic that goes... kinematics never leave the awake set, so they always have a real velocity row). Tasked at Stage 1 Task 5 Step 5.
3. **The narrowphase indirection cost** (Stage 1 adds 2 loads per transform access on the 76%-of-churn narrowphase). Gated by the Stage-1 `[STEPPROF]` no-regression check (Task 6 Step 4). If narrowphase regresses materially, surface it (the collide math should dominate the 2 extra loads — Box2D accepts the same indirection).
4. **Spans -> overflow in Stage 3** (Task 10). Fine for body/Sandbox scenes (spans are the tile-collision path, rarely hit); a tile-heavy scene would grow the overflow scalar path. Noted as a deferrable refinement (color spans per-step against the persistent occupancy if measurement ever demands it).
5. **The Stage-2 win is locality, not gather-elimination** (the conventions section + Task 8 Step 3 state this). Do NOT treat a modest solve delta as a failure — the user accepted the architecture (+ Phase-D substrate) as the goal.
6. **`kColorCount=12` is kept** (the 32-bit per-body mask caps at 32; `m_bodyColorMask` is `uint32`). Bumping to 24 is a one-line change but the existing `PhysicsSimdSolverTest.cpp:990`-area overflow test assumes 12 — out of Phase-C scope.

**Type/name consistency check:** `SolverSet`/`kSetStatic/Awake/Sleeping`, `m_setIndex`/`m_localIndex`/`m_sets[3]`, `PhysicsSet`/`BodySimSoA`/`BodyStateSoA2`, `MigrateBody`, `AwakeStateCount`/`AwakeSetCount`, `LocalIndexOf`/`SetIndexOf`/`SetBodyIdAt`, `AssignContactColor`/`ReleaseContactColor`/`ContactColorOf`/`ValidatePersistentColoring`, `m_bodyColorMask`/`m_colorContacts`, `m_colorRefs`/`m_overflowRefs`, `ContactConstraint::color`, `Contact::color`/`kInvalidColor` — used consistently across tasks. The awake set's row count is `AwakeSetCount()` (= `m_sets[kSetAwake].Count()`, introduced in Stage 1 Task 4) — used by name in both Stage 1 and Stage 2 (the solver's dense index-space size); there is no separate `AwakeStateCount`.

**Stage independence:** each stage keeps the engine green + deterministic + shippable. The user MAY choose to land Stage 1+2 (storage + the solve re-home) and re-evaluate Stage 3 (incremental coloring) after the Stage-2 `[STEPPROF]`, since each is independently gated on the same branch. The plan does not force all three before merge — though the user chose the full scope, so the default is all three.
