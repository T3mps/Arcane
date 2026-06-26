# Arcane 2D Physics — Phase C: Awake-Compacted Solver State + Incremental Coloring — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Fresh implementer per task + two-stage (spec -> quality) review + fix-subagents on findings, exactly as Phases A and B.

**Goal:** Re-home Part 1's lane-wide SIMD contact solve onto a DENSE, awake-compacted body-state scratch (built per-step from the Phase-B awake-set — no body-storage restructure), and make graph coloring INCREMENTAL (color assigned once at contact-create, released at destroy) with persistent per-color membership feeding the solver. Together these (a) bank the gather-locality win Part 1 couldn't get (it was indexing a sparse `worldCount` array) and (b) lay the stable, body-independent substrate Phase D (multithreading) needs.

**Scope decision (recorded):** the FULL Box2D-v3 two-layer *body* migration (per-set `BodySim`/`BodyState` + setIndex/localIndex + sleep/wake row migration) was evaluated and **dropped** — it delivers no measurable benefit over this lighter cut (identical solve-perf; it adds an indirection to the 76%-of-churn narrowphase for no locality gain on our contact-centric access; MT does not need it because the colored solve already guarantees within-color body-disjointness). Phase C keeps the existing stable-slot body SoA exactly as it is and changes ONLY the solver's internal index space + the coloring's lifecycle.

**Architecture:** Two independently-landable, independently-gated stages. **Stage 1** compacts the solver's body-state scratch to a dense `solverCount`-sized array indexed by the Phase-B awake-set index (awake dynamics) plus a small persistent kinematic list (kinematic B-endpoints), with a single shared scatter-safe tail for static/span/padding — and re-homes the SIMD gather/scatter/pack onto it. **Stage 2** moves coloring to incremental (assign-at-create, release-at-destroy) with persistent per-color membership the solver groups by. Each stage keeps the engine green + deterministic and is re-measured with `[STEPPROF]`.

**Tech Stack:** C++23, Core (presentation-free; `/MD` for Arcane.dll + static-CRT for ArcaneCore), `Arcane::Simd` wrapper, glm + std + sibling Physics headers only, Catch2 (`[physics]` / `[physics][island]` / `[physics][awakeset]` / `[simd]` / `[determinism]` + a new `[physics][phasec]`), premake5 (at the **repo root**) / MSBuild via `Arcane.slnx`. `/fp:strict /arch:AVX2`, no fast-math (determinism). Branch `feature/arcane-physics-phaseC-compacted-solve` off `main` (Phase A+B are already on local `main` @ `b127af9`).

---

## Conventions

- **Branch:** create `feature/arcane-physics-phaseC-compacted-solve` off `main` (`b127af9`). Do **NOT** push (the user merges/pushes manually after a visual + CI gate). Commit per task with the trailer:
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
  Use `"[phasec]"` / `"[awakeset]"` / `"[island]"` / `"[simd]"` / `"[determinism]"` for subsets, or a quoted full case name for one case. Release exe dir is `bin/Release-windows-x86_64-md/ArcaneTests`. This machine HAS a capable GPU (RTX 3070) — run `[gpu]` tests (do NOT exclude them) in the final per-stage gate.
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
  (Phase C's new test file `PhysicsCompactedSolveTest.cpp` IS new -> regen. The `m_kinematicBodies` members + the `Contact::color` field are added in place -> no regen for those.)
- **clangd / IDE diagnostics are FALSE POSITIVES — MSVC/MSBuild + the test run are the only truth.** ASCII comments, explain WHY. C++23. Commands run via the PowerShell tool (chain with `;`, not `&&`).
- **DETERMINISM IS THE HARD CONTRACT** — run-twice-identical at every task. There are NO exact goldens; the `[physics]` + `[island]` + `[awakeset]` + `[simd]` + `[determinism]` behavioral suites + the run-twice cases + the SIMD lane-width-invariance test are the gate. Phase C is a *solver-indexing* + *coloring-lifecycle* change that should NOT shift physics numerics; a shifted `[physics]` invariant is a RED FLAG to diagnose, not to re-baseline.
- **The compaction does NOT eliminate the gather instruction** (a SIMD batch references 8 arbitrary bodies). The Stage-1 win is *cache locality + dropping the sparse-`worldCount` overhead* (the dense scratch is `solverCount`-sized, no holes; the dummy shrinks to one shared tail). Frame + measure it as a locality win, not a "gathers are gone" win. The larger throughput payoff is Phase D (MT over the colored, compacted work); Phase C is its substrate.

---

## Name glossary (use these EXACTLY across all tasks)

- **The body SoA is UNCHANGED.** `m_posX/m_velX/m_angle/...` stay slot-indexed exactly as today. Phase C does NOT add `setIndex`/`localIndex`, per-set arrays, `BodySim`, or migration. Velocity stays authoritative in `m_velX/m_velY/m_angVel`.
- `awakeCount` — `m_awakeBodies.size()` — the number of awake DYNAMIC bodies (Phase B's awake-set). Their dense index is `m_awakeIndex[slot] ∈ [0, awakeCount)` (already maintained by Phase B; a bijection over `m_awakeBodies`).
- `m_kinematicBodies` — `std::vector<std::uint32_t>` — a NEW dense list of KINEMATIC body slots (the solver's read-only B-endpoint working set), maintained at create/remove like the awake-set. `m_kinematicIndex[slot]` — its position, or `kNotKinematic = 0xFFFFFFFFu`. (Kinematics never sleep, so this needs no sleep/wake maintenance.)
- `kinematicCount` — `m_kinematicBodies.size()`.
- `solverCount` — `awakeCount + kinematicCount` — the size of the dense solver body-state scratch (the dense index space). The scratch is sized `solverCount + 1`.
- `solverIndex(slot)` — the dense index of a body in the solver scratch: an awake dynamic -> `m_awakeIndex[slot]` (∈ `[0, awakeCount)`); a kinematic -> `awakeCount + m_kinematicIndex[slot]` (∈ `[awakeCount, solverCount)`); a static / span / padding -> `kSolverDummy = solverCount` (the single shared scatter-safe tail row, zero velocity). NOTE: every constraint endpoint resolves to one of these — A is always an awake dynamic (orientation rule + awake-A emit gate); a dynamic B is always awake (island unity); a static B reads the zero tail.
- `m_bodyState` — the existing solver-local `BodyStateSoA` (`vx/vy/w/dpx/dpy/dq`), Stage 1 RE-SIZED to `solverCount + 1` and RE-INDEXED by `solverIndex` (was `worldCount + 1`, world-slot-indexed).
- `Contact::color` / `kInvalidColor` — the persistent color a solver-relevant body-body contact is assigned at create (Stage 2); `kInvalidColor = 0xFFu` = overflow/unassigned.
- `m_bodyColorMask` — `std::vector<std::uint32_t>` — per body slot: a `kColorCount`-bit occupancy mask for the incremental coloring (Stage 2).
- `m_colorContacts` — `std::vector<std::vector<std::uint32_t>>` — per color: the persistent member contact ids (Stage 2).
- `[STEPPROF]` — the opt-in gated per-Step-phase timing (`ARCANE_STEPPROF`, default-off, from Phase B). The per-stage measurement gate.

---

## File Structure

| File | Created/Modified | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | Modified | `m_kinematicBodies`/`m_kinematicIndex`/`kNotKinematic` members + `KinematicBodies()`/`ForEachKinematic` + `AddToKinematicSet`/`RemoveFromKinematicSet` decls (Stage 1); `m_bodyColorMask`/`m_colorContacts` + `AssignContactColor`/`ReleaseContactColor`/`ContactColorOf`/`ValidatePersistentColoring` (Stage 2). NO body-storage change. |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | Modified | Size the kinematic columns in `EnsureCapacity`; maintain `m_kinematicBodies` in `AddBody`/`RemoveBody` (Stage 1); the coloring assign at `TryCreateContact` + release at the 4 `Destroy(id)` sites (Stage 2). |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` | Modified | Re-home `Solve`: size `m_bodyState` to `solverCount+1`; `SyncIn`/`SyncOut`/`IntegrateVelocitiesSoA`/`IntegratePositionsSoA`/`FinalizePositionsSoA` index by `solverIndex`; the dense scratch fill (awake dynamics + kinematics); dummy = `solverCount` (Stage 1). Replace the per-step `ColorConstraints` with grouping by the persistent `Contact::color` (Stage 2). |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp` | Modified | `m_colorRefs`/`m_overflowRefs` members (Stage 2, replace `m_coloring`/`m_edges`). |
| `Arcane/Core/src/Arcane/Physics/Solver/BodyStateSoA.hpp` | Modified | Doc: the scratch is now `solverCount+1` + `solverIndex`-indexed (was `worldCount+1`/world-slot). No structural change (same six columns). |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` | Modified | `PackLane` maps body slots -> `solverIndex` (A: `m_awakeIndex[bodyA]`; B dyn: `m_awakeIndex[bodyB]`; B kinematic: `awakeCount + m_kinematicIndex[bodyB]`; B static/span/padding: `dummyIndex = solverCount`). The gather/scatter are over the dense scratch. |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactColoring.hpp/.cpp` | Modified (Stage 2) | Retire the per-step `ColorConstraints` driver call (the greedy algorithm helper may be kept for tests / removed); the live coloring becomes the incremental assign/release in `PhysicsWorld`. |
| `Arcane/Core/src/Arcane/Physics/Contact.hpp` | Modified (Stage 2) | `std::uint8_t color = kInvalidColor;` on `Contact`; reset on pool-slot recycle. |
| `Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp` | Modified (Stage 2) | `std::uint8_t color = kInvalidColor;` on `ContactConstraint` (emit tags it from the pool). |
| `Arcane/Tests/src/PhysicsCompactedSolveTest.cpp` | **Created** | New `[physics][phasec]` tests: the kinematic-set invariant (Stage 1); the awake-compacted solve equivalence + lane-width invariance + run-twice determinism + the `[STEPPROF]` solve-locality measurement (Stage 1); the persistent-coloring validity + incremental-coloring determinism through create/destroy churn (Stage 2). |

---

# STAGE 1 — Awake-compacted solver state + gather-free re-home (the win)

**Outcome:** the solver's `m_bodyState` scratch is DENSE (`solverCount+1`, indexed by `solverIndex`) instead of sparse (`worldCount+1`, world-slot-indexed). The repeated per-substep gathers/scatters hit a packed array (locality win); the scatter-safe dummy collapses to one shared tail at `solverCount`. The body SoA + all physics numerics are byte-unchanged (pure re-indexing). Reuses the Phase-B `m_awakeBodies`/`m_awakeIndex` for awake dynamics; adds a small persistent `m_kinematicBodies` list for kinematic B-endpoints.

### Task 1: The kinematic solver-set (`m_kinematicBodies`/`m_kinematicIndex`) maintained at create/remove

**Files:** `PhysicsWorld.hpp` (members + maintainers + probes), `PhysicsWorld.cpp` (`EnsureCapacity`/`AddBody`/`RemoveBody`). Create `PhysicsCompactedSolveTest.cpp` (regen BOTH workspaces). 

Add a dense list of kinematic body slots (the solver's read-only B-endpoint working set), maintained like the Phase-B awake-set but with NO sleep/wake path (kinematics never sleep). The solver does NOT use it yet (Task 2 does); this isolates the bookkeeping.

- [ ] **Step 1: write the failing invariant test.** Create `PhysicsCompactedSolveTest.cpp`:
  ```cpp
  // Physics Phase C: awake-compacted solver state + incremental coloring -- BEHAVIORAL tests.
  // Companion to PhysicsAwakeSetTest.cpp / PhysicsSimdSolverTest.cpp. PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <vector>
  #include <catch2/catch_test_macros.hpp>
  #include <Arcane/Physics/PhysicsTypes.hpp>
  #include <Arcane/Physics/Shapes.hpp>
  #include <Arcane/Physics/Body.hpp>
  #include <Arcane/Physics/PhysicsWorld.hpp>
  using namespace Arcane::Physics;
  namespace {
      constexpr Real kStep = Real(1) / Real(60);
      BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
          BodyDef d; d.type=BodyType::Static; d.position=pos; d.shape=MakeAabb(hw,hh); d.friction=Real(0.6); return w.AddBody(d);
      }
      BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
          BodyDef d; d.type=BodyType::Dynamic; d.position=pos; d.shape=MakeAabb(hw,hh); d.density=Real(1); d.friction=Real(0.4); d.fixedRotation=true; return w.AddBody(d);
      }
      BodyHandle AddKinematic(PhysicsWorld& w, Vec2 pos, Real hw, Real hh) {
          BodyDef d; d.type=BodyType::Kinematic; d.position=pos; d.shape=MakeAabb(hw,hh); return w.AddBody(d);
      }
  }
  // The kinematic-set must, at all times, contain EXACTLY the live kinematic slots.
  TEST_CASE("PhysicsCompacted: kinematic-set tracks live kinematic slots", "[physics][phasec]")
  {
      auto checkInvariant = [](PhysicsWorld& w) {
          const std::vector<std::uint32_t>& set = w.KinematicBodies();
          std::vector<std::uint8_t> seen(w.Count(), 0u);
          for (const std::uint32_t s : set) {
              REQUIRE(s < w.Count());
              REQUIRE(w.Alive(s));
              REQUIRE(w.TypeSlot(s) == BodyType::Kinematic);
              REQUIRE(seen[s] == 0u);
              seen[s] = 1u;
          }
          for (std::uint32_t i = 0; i < w.Count(); ++i) {
              const bool kin = w.Alive(i) && w.TypeSlot(i) == BodyType::Kinematic;
              REQUIRE((seen[i] != 0u) == kin);
          }
      };
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const BodyHandle k0 = AddKinematic(w, Vec2(Real(-30), Real(-10)), Real(5), Real(5));
      const BodyHandle d0 = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      const BodyHandle k1 = AddKinematic(w, Vec2(Real(30), Real(-10)), Real(5), Real(5));
      checkInvariant(w);
      for (int kk = 0; kk < 60; ++kk) w.Step(kStep);
      checkInvariant(w);
      w.RemoveBody(k0); checkInvariant(w);          // swap-remove from the kinematic-set
      const BodyHandle k2 = AddKinematic(w, Vec2(Real(0), Real(-50)), Real(5), Real(5));
      checkInvariant(w);                             // recycle a slot as a fresh kinematic
      (void)d0; (void)k1; (void)k2;
  }
  ```
- [ ] **Step 2: regen BOTH workspaces, build Debug `-t:ArcaneTests`, verify FAIL** (`KinematicBodies()` does not exist).
- [ ] **Step 3: add the members + maintainers + probes** (mirror the Phase-B awake-set exactly, `PhysicsWorld.cpp:3164-3206` is the template). `PhysicsWorld.hpp`: `std::vector<std::uint32_t> m_kinematicBodies; std::vector<std::uint32_t> m_kinematicIndex; static constexpr std::uint32_t kNotKinematic = 0xFFFFFFFFu;`. `KinematicBodies()` accessor; `template<typename Fn> void ForEachKinematic(Fn&&) const`; `void AddToKinematicSet(std::uint32_t) noexcept; void RemoveFromKinematicSet(std::uint32_t) noexcept;`. Implement the two maintainers as swap-remove-with-back-ref-patch (copy `AddToAwakeSet`/`RemoveFromAwakeSet` but gate on `BodyType::Kinematic`).
- [ ] **Step 4: wire the seams.**
  - `EnsureCapacity`: `m_kinematicIndex.resize(next, kNotKinematic);`.
  - `AddBody`, in the Kinematic case: `m_kinematicIndex[idx] = kNotKinematic; AddToKinematicSet(idx);` (reset-before-add guards the recycled-slot stale index, exactly like the awake-set).
  - `RemoveBody`, next to `RemoveFromAwakeSet(idx)` (the slot is still typed Kinematic here): `RemoveFromKinematicSet(idx);`.
- [ ] **Step 5: build Debug, run `[phasec]` + `[physics]` + `[awakeset]`.** Expected GREEN + byte-identical (the list is maintained but unused). 
- [ ] **Step 6: ArcaneCore clean (Debug + Release), commit.** `feat(arcane/physics): kinematic solver-set (m_kinematicBodies) maintained at create/remove`. Trailer.

### Task 2: Re-home the lane-wide solve onto the dense `solverCount` scratch

**Files:** `Solver/SoftStep.cpp` (the `Solve` driver + `SyncIn`/`SyncOut`/`Integrate*`/`Finalize`), `Solver/ContactConstraintSimd.hpp` (`PackLane`/`PadLane` index mapping), `Solver/BodyStateSoA.hpp` (doc), `PhysicsWorld.hpp` (an `AwakeIndexOf(slot)`/`KinematicIndexOf(slot)` accessor if not already exposed). Extend `PhysicsCompactedSolveTest.cpp`.

Switch the solver's index space from world slot to `solverIndex`. The body velocity is read from the world (`VelSlot`) into the dense scratch (`SyncIn`), the solve runs over the dense scratch, and `SyncOut` writes awake dynamics' velocity back. `PackLane` maps each endpoint slot to its `solverIndex`.

- [ ] **Step 1: equivalence baseline test.** Append a settle equivalence + run-twice determinism test (the re-home is pure re-indexing -> must be byte-identical), build + run on the PRE-re-home build to confirm it PASSES as the regression reference:
  ```cpp
  TEST_CASE("PhysicsCompacted: solve settles identically + deterministically", "[physics][phasec]")
  {
      auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
          // include a kinematic plate pushing a dynamic so a kinematic B-endpoint is exercised
          BodyDef kd; kd.type=BodyType::Kinematic; kd.position=Vec2(Real(0),Real(-8)); kd.shape=MakeAabb(Real(60),Real(2));
          const BodyHandle k = w.AddBody(kd); w.SetVelocity(k, Vec2(Real(3), Real(0)));
          std::vector<BodyHandle> boxes;
          for (int i = 0; i < 5; ++i) boxes.push_back(AddBox(w, Vec2(Real(0), Real(-20) - Real(9)*static_cast<Real>(i)), Real(4), Real(4)));
          for (int kk = 0; kk < 600; ++kk) w.Step(kStep);
          pos.clear(); awake.clear();
          for (const BodyHandle b : boxes) { pos.push_back(w.Position(b)); awake.push_back(w.IsAwake(b)?1:0); }
          (void)k;
      };
      std::vector<Vec2> p1,p2; std::vector<int> a1,a2; run(p1,a1); run(p2,a2);
      REQUIRE(p1.size()==p2.size());
      for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); REQUIRE(a1[i]==a2[i]); }
  }
  ```
- [ ] **Step 2: expose the index accessors** in `PhysicsWorld.hpp` (inline): `AwakeIndexOf(slot)` (= `m_awakeIndex[slot]`), `KinematicIndexOf(slot)` (= `m_kinematicIndex[slot]`), `AwakeCount()` (= `m_awakeBodies.size()`), `KinematicCount()` (= `m_kinematicBodies.size()`). These let the solver + `PackLane` compute `solverIndex` without friending internals.
- [ ] **Step 3: re-home `SoftStep::Solve`** (`SoftStep.cpp:562-679`):
  - `const std::uint32_t awakeCount = w.AwakeCount(); const std::uint32_t solverCount = awakeCount + w.KinematicCount(); const std::int32_t dummyIndex = (std::int32_t)solverCount;`.
  - `m_bodyState.Resize(solverCount + 1u);` (was `count + 1u`).
  - Replace `SyncIn(w)` with a `SyncInCompacted(w)` that fills the dense scratch: awake dynamics via `ForEachAwake` -> `m_bodyState.vx[w.AwakeIndexOf(s)] = VelSlot(s).x; ...; dp/dq[...]=0`; kinematics via `ForEachKinematic` -> `m_bodyState.vx[awakeCount + w.KinematicIndexOf(s)] = VelSlot(s).x; ...`; the dummy tail (`solverCount`) stays zero (Resize zeroes it). (NOTE: statics need no row — they map to the zero dummy.)
  - The coloring edge build (`:597-610`): map endpoints to `solverIndex` (Stage 2 replaces this whole block; for Stage 1 keep the per-step greedy but over `solverIndex`): `e.a = w.AwakeIndexOf(cc.bodyA); e.b = bDyn ? w.AwakeIndexOf(cc.bodyB) : e.a;` and `ColorConstraints(m_edges, awakeCount)` (only dynamics constrain coloring -> the `awakeCount` index space suffices; kinematics are read-only, never block a color).
- [ ] **Step 4: re-index `PackLane`** (`ContactConstraintSimd.hpp:220-294`). Have `Build`/`PackLane` take the `PhysicsWorld&` (or three small params: `awakeIndex` ptr, `kinematicIndex` ptr, `awakeCount`). Set:
  ```cpp
  dst.bodyIndexA[L] = (int32)w.AwakeIndexOf(cc.bodyA);   // A is always an awake dynamic
  const bool bDyn = cc.bodyBIsBody && cc.invMassB > Real(0);
  dst.dynB[L] = bDyn ? 1.0f : 0.0f;
  if (bDyn)                              dst.bodyIndexB[L] = (int32)w.AwakeIndexOf(cc.bodyB);
  else if (cc.bodyBIsBody && /*kinematic*/ w.KinematicIndexOf(cc.bodyB) != PhysicsWorld::kNotKinematic)
                                         dst.bodyIndexB[L] = (int32)(awakeCount + w.KinematicIndexOf(cc.bodyB));
  else                                   dst.bodyIndexB[L] = dummyIndex;  // static (zero) / span / handled below
  ```
  `PadLane` targets `dummyIndex` (= `solverCount`) as today. (Document: a kinematic B has a real dense row with its authored velocity -> the relative-velocity term drives the push; a static B reads the zero tail; the scatter writes `select(dynB=0, new, gathered)` = the unchanged gathered value, idempotent for kinematic/static/dummy.)
- [ ] **Step 5: re-index the integrate/finalize/sync-out passes** (`SoftStep.cpp:257-319`, `126-136`). `IntegrateVelocitiesSoA`/`IntegratePositionsSoA`/`FinalizePositionsSoA` iterate awake DYNAMICS via `ForEachAwake` and index `m_bodyState.*[w.AwakeIndexOf(slot)]`; `FinalizePositionsSoA` commits via `w.CommitSlotPosition(slot, p, a)` (unchanged). `SyncOut` writes awake dynamics' velocity back: `ForEachAwake` -> `SetVelSlot(slot, {vx[AwakeIndexOf(slot)], ...})` (kinematics are read-only -> never written back).
- [ ] **Step 6: build Debug, run `[phasec]` + `[physics]` + `[simd]` + `[island]` + `[awakeset]`.** Expected: the equivalence test + the whole suite GREEN, byte-identical settle (pure re-indexing). Diagnose any shift: most likely a kinematic-vs-static B mis-gate (a kinematic B falling through to the dummy -> its push velocity lost; or a static B using a stale kinematic index).
- [ ] **Step 7: lane-width invariance.** Run the `[simd]` lane-width-invariance test (`PhysicsSimdSolverTest.cpp` "lane-wide solve is packing-width invariant + matches scalar"). It must still hold (the math is unchanged; only the index space moved). The dummy/scatter-corruption guard must pass (the shared tail at `solverCount` absorbs read-only/padding scatters). NOTE: that test seeds its own `dummyIndex = bodyCount` + a `world.Count()+1` SoA; if it asserts the OLD sizing, re-baseline its `dummyIndex`/sizing to the compacted contract (a deliberate test re-baseline within the same invariant) — record it in the commit.
- [ ] **Step 8: full Debug + Release + ArcaneCore clean, commit.** `perf(arcane/physics): re-home the lane-wide solve onto a dense solverCount scratch (awake-set + kinematic indices; dummy=solverCount; drop the sparse worldCount SoA)`. Trailer.

### Task 3: Stage-1 gate + `[STEPPROF]` solve-locality measurement

- [ ] **Step 1: full Debug + Release gate** (whole suite incl. `[gpu]` both backends; `[simd]` lane-width invariance; `[determinism]` run-twice).
- [ ] **Step 2: ArcaneCore static-CRT** Debug + Release clean.
- [ ] **Step 3: `[STEPPROF]` measurement.** `#define ARCANE_STEPPROF 1`, build Release, run the hidden `[stepprof]` churn/settled measurement (2000-box pile, the helper from `PhysicsAwakeSetTest.cpp`'s pattern — or add a `[.][stepprof]` case here). Compare the `solve` bucket vs the Stage-0 baseline (churn ~1.08ms). Expected: a measurable but MODEST drop (dense contiguous scratch -> better gather locality + no `worldCount` allocation/copy). Record before/after in the commit body. A small delta is EXPECTED + acceptable (the user accepted that the single-threaded payoff is modest; the structural value is the Phase-D substrate). Revert the `#define`.
- [ ] **Step 4: commit.** `test(arcane/physics): Stage-1 awake-compacted solve full gate + STEPPROF solve-locality measurement`. Trailer. (User runs the Dist `Loom.exe` Sandbox before merge — note it.)

---

# STAGE 2 — Incremental coloring + persistent per-color membership

**Outcome:** a solver-relevant body-body contact's color is assigned ONCE at create (lowest free color for its dynamic endpoints, via a per-body color bitmask) and released at destroy — never recomputed per Step. The solver groups the per-step emitted constraints by their source contact's persistent color (replacing the per-step greedy `ColorConstraints`). The win: the O(contacts x colors) per-step recolor is gone; the persistent color membership is the stable parallel-work substrate Phase D needs.

**Determinism note (the crux this stage re-establishes):** today's anchor is that `EmitContactConstraints` canonical-sorts contacts before the per-step recolor. Incremental coloring assigns at CREATE (in `UpdateContacts`' broadphase-pair + static-candidate order). That create order is deterministic (same broadphase tree -> same pair order; ascending-id pool destroy) and the colored solve is within-color order-independent (one contact per dynamic body per color) — but this MUST be pinned by a run-twice create/destroy-churn test (Task 6).

### Task 4: Persistent color on the Contact + incremental assign/release (maintained, not yet consumed)

**Files:** `Contact.hpp` (the `color` field + recycle reset), `PhysicsWorld.hpp` (`m_bodyColorMask`/`m_colorContacts` + helpers + the `ValidatePersistentColoring` oracle), `PhysicsWorld.cpp` (assign in `TryCreateContact`, release at the 4 `Destroy(id)` sites). Extend `PhysicsCompactedSolveTest.cpp`.

Maintain the persistent coloring at the contact lifecycle seams (`TryCreateContact` `if(r.created)` block `PhysicsWorld.cpp:2077-2082`; the four `Destroy(id)` sites `:2383`, `:2420`, `:1826`, `:1853`). The solver does NOT yet consume it (keeps the per-step `ColorConstraints` over `solverIndex` from Stage 1); a `ValidatePersistentColoring` oracle asserts validity — the gate for Task 5.

- [ ] **Step 1: failing validity test.** Append:
  ```cpp
  TEST_CASE("PhysicsCompacted: persistent contact coloring is valid", "[physics][phasec]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
      for (int i = 0; i < 30; ++i) AddBox(w, Vec2(Real(-40) + Real(3)*static_cast<Real>(i%20), Real(-20) - Real(9)*static_cast<Real>(i/20)), Real(4), Real(4));
      for (int k = 0; k < 60; ++k) w.Step(kStep);
      REQUIRE(w.ValidatePersistentColoring());     // no two same-color contacts share a dynamic body
  }
  ```
- [ ] **Step 2: build, verify FAIL** (`ValidatePersistentColoring` does not exist).
- [ ] **Step 3: add storage + helpers.**
  - `Contact.hpp`: `std::uint8_t color = kInvalidColor;` (`inline constexpr std::uint8_t kInvalidColor = 0xFFu;`); reset to `kInvalidColor` in `ContactPool::EnsurePair`'s slot-reset (`Contact.cpp` MISS path).
  - `PhysicsWorld.hpp`: `std::vector<std::uint32_t> m_bodyColorMask;` (sized in `EnsureCapacity`, default 0); `std::vector<std::vector<std::uint32_t>> m_colorContacts;` (ctor: `resize(kColorCount)`). Helpers `AssignContactColor(id,a,b,aDyn,bDyn)` (lowest-free over the two dynamic endpoints' masks -> set bits + push to `m_colorContacts[k]`; no free color -> `c.color=kInvalidColor` overflow), `ReleaseContactColor(id)` (clear the bits — one contact per body per color, so the bit clears cleanly — swap-remove from `m_colorContacts[c.color]`, set `c.color=kInvalidColor`), `ContactColorOf(id)`, `ValidatePersistentColoring()` (per color walk `m_colorContacts[k]`, assert no dynamic body twice + each member's `color==k`). (Algorithm bodies as in the verified research; `kColorCount=12` from `ContactColoring.hpp:39`.)
- [ ] **Step 4: call assign/release.** Create: in `TryCreateContact` `if(r.created)` after `c.bodyA/bodyB` set, `if (solverRelevant && bIsBody) AssignContactColor(r.id, ia, ib, da, db);`. Destroy: `ReleaseContactColor(id)` immediately before each `m_contactPool.Destroy(id)`. `EnsureCapacity`: `m_bodyColorMask.resize(next, 0u);`.
- [ ] **Step 5: build Debug, run `[phasec]` + `[physics]` + `[island]`.** Expected GREEN: coloring maintained + valid while the solver still uses the per-step `ColorConstraints` (behavior byte-identical). Add a Debug assertion in `RemoveBody` that `m_bodyColorMask[idx]==0` after `DestroyContactsForBody` (every contact released its bits).
- [ ] **Step 6: full Debug + Release + ArcaneCore clean, commit.** `feat(arcane/physics): persistent incremental contact coloring (assign-at-create / release-at-destroy); not yet consumed`. Trailer.

### Task 5: Feed the solver from the persistent coloring (delete the per-step recolor)

**Files:** `Solver/SoftStep.cpp`/`.hpp` (consume the persistent coloring), `PhysicsWorld.cpp`/`Solver.hpp` (tag emitted constraints with color). Extend `PhysicsCompactedSolveTest.cpp`.

Replace the per-step `ColorConstraints(m_edges, ...)` with grouping the emitted constraints by their source contact's persistent color. Overflow color OR span (`kNoContact`) -> the scalar overflow path.

- [ ] **Step 1: tag emitted constraints with color.** `Solver.hpp`: `std::uint8_t color = kInvalidColor;` on `ContactConstraint`. `EmitContactConstraints` (`PhysicsWorld.cpp`): where it sets `out.back().sourceContactId = id` (`:2716`), also `out.back().color = m_contactPool.Get(id).color;`. Spans keep `kInvalidColor`.
- [ ] **Step 2: replace the coloring step in `SoftStep::Solve`.** Delete the `m_edges` build + `m_coloring = ColorConstraints(...)` (`:597-611`). Bucket by `cc.color`:
  ```cpp
  for (auto& bucket : m_colorRefs) bucket.clear();   // m_colorRefs: array<vector<uint32>, kColorCount>
  m_overflowRefs.clear();
  for (std::uint32_t c = 0; c < ctx.contactCount; ++c) {
      const std::uint8_t col = ctx.contacts[c].color;
      if (col < kColorCount) m_colorRefs[col].push_back(c);
      else                   m_overflowRefs.push_back(c);   // overflow contact OR span
  }
  ```
  Build per color from `m_colorRefs[k]`; the overflow path solves `m_overflowRefs` (feed the existing `Overflow*` functions from `m_overflowRefs` instead of `m_coloring.overflow`). New `SoftStep` members `m_colorRefs`/`m_overflowRefs`; delete `m_coloring`/`m_edges`. `LastOverflowCount()` -> `m_overflowRefs.size()`.
- [ ] **Step 3: build Debug, run `[phasec]` + `[physics]` + `[simd]` + `[island]` + `[awakeset]`.** Expected GREEN, byte-identical settle (a valid coloring -- greedy or persistent -- yields the same solve; the colored solve is coloring-order-independent given validity). Diagnose any shift (a stale `Contact::color`, or a span mis-bucketed).
- [ ] **Step 4: Debug within-color clash assertion** in/around `Build`: within each `m_colorRefs[k]` no dynamic body slot appears twice (the active emitted subset of a valid persistent coloring is still valid). Run `[physics]` Debug to exercise it.
- [ ] **Step 5: full Debug + Release + ArcaneCore clean, commit.** `perf(arcane/physics): solver consumes the persistent contact coloring (delete the per-step greedy recolor)`. Trailer.

### Task 6: Stage-2 gate + incremental-coloring determinism + `[STEPPROF]`

- [ ] **Step 1: create/destroy-churn run-twice determinism.** Append a run-twice test that removes + re-adds bodies (destroy + recreate contacts -> release + reassign colors), asserting bit-identical final positions across two runs:
  ```cpp
  TEST_CASE("PhysicsCompacted: incremental coloring is deterministic across two runs (create/destroy churn)", "[physics][phasec]")
  {
      auto run = [](std::vector<Vec2>& pos) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
          std::vector<BodyHandle> b;
          for (int i = 0; i < 16; ++i) b.push_back(AddBox(w, Vec2(Real(-20) + Real(3)*static_cast<Real>(i), Real(-20)), Real(4), Real(4)));
          for (int k = 0; k < 120; ++k) w.Step(kStep);
          w.RemoveBody(b[4]); w.RemoveBody(b[9]);
          for (int k = 0; k < 60; ++k) w.Step(kStep);
          b.push_back(AddBox(w, Vec2(Real(0), Real(-30)), Real(4), Real(4)));
          for (int k = 0; k < 200; ++k) w.Step(kStep);
          pos.clear(); for (std::size_t i = 0; i < b.size(); ++i) { if (i==4||i==9) continue; pos.push_back(w.Position(b[i])); }
      };
      std::vector<Vec2> p1,p2; run(p1); run(p2);
      REQUIRE(p1.size()==p2.size());
      for (std::size_t i=0;i<p1.size();++i){ REQUIRE(p1[i].x==p2[i].x); REQUIRE(p1[i].y==p2[i].y); }
  }
  ```
- [ ] **Step 2: full Debug + Release gate** (whole suite incl. `[gpu]` both backends; `[simd]`; `[determinism]`).
- [ ] **Step 3: ArcaneCore static-CRT** Debug + Release clean.
- [ ] **Step 4: `[STEPPROF]`.** `#define ARCANE_STEPPROF 1`, build Release, run the churn measurement; confirm the per-step recolor sub-cost is gone + no regression. Record. Revert.
- [ ] **Step 5: commit.** `test(arcane/physics): Stage-2 incremental coloring full gate + determinism + STEPPROF`. Trailer.

---

# Phase C completion gate + memory

### Task 7: Phase-C holistic gate + memory update

- [ ] **Step 1: whole-Phase-C gate.** Kill strays. Full ArcaneTests **Debug AND Release**, whole suite (incl. `[gpu]` D3D12 + Vulkan — assert `Arcane::RenderErrorCount()==0`, no validation noise), from each config's exe dir. Record assertion/case counts.
- [ ] **Step 2: ArcaneCore static-CRT** Debug + Release: clean.
- [ ] **Step 3: determinism + lane-width invariance** consolidated: `[determinism]` + `[simd]` + the `[phasec]` run-twice cases all green both configs.
- [ ] **Step 4: `[STEPPROF]` Phase-C summary.** Measuring Release; dump churn + settled; record the Stage-0 -> Stage-1 -> Stage-2 progression of the `solve` bucket. State plainly the size of the gather-locality win (expected MODEST single-threaded; the substrate for Phase D is the deliverable).
- [ ] **Step 5: visual gate handoff.** The user runs the Dist `Loom.exe` Sandbox (scenes 0-8: settle piles, the whisk stress, the 10k stress, spawn/drag/throw) before merge. Note in the commit; do NOT merge or push.
- [ ] **Step 6: update memory + final commit.** Update `project_arcane_physics_rearchitecture` (Phase C DONE as the LIGHTER cut — full two-layer body migration dropped as unbeneficial; the awake-compacted solve + incremental coloring landed; next = Phase D MT, for which this is the substrate; pause for the user before D). Update `project_arcane_physics_phaseC_progress` (as-built + carry-forwards). Final commit: `feat(arcane/physics): Phase C -- awake-compacted solver state + incremental coloring (Phase-D substrate)`. Body: the full gate + the STEPPROF progression. Trailer. **Do NOT push.**

---

## Self-Review Notes

**Scope coverage (the chosen LIGHTER Phase C):**
- Awake-compacted solver state + gather-free re-home -> Stage 1 (Tasks 1-3): reuse Phase-B `m_awakeBodies`/`m_awakeIndex` for awake dynamics + a new persistent `m_kinematicBodies` for kinematic B-endpoints; dense `solverCount+1` scratch; `PackLane` maps to `solverIndex`; shared zero tail for static/span/padding. No body-storage restructure.
- Incremental coloring + persistent per-color membership -> Stage 2 (Tasks 4-6): assign-at-create / release-at-destroy via a per-body color mask; the solver groups emitted constraints by `Contact::color`; overflow + spans -> scalar overflow.
- The DROPPED full two-layer body migration is documented in the header (no measurable benefit; identical solve-perf; narrowphase-indirection cost; not an MT prerequisite).

**Determinism (the hard contract):** Stage 1 is pure re-indexing (no math change) -> behavioral suites must stay byte-identical (a shift = a bug, NOT a re-baseline; the ONE allowed re-baseline is the `[simd]` test's `dummyIndex`/sizing contract, which is a test-only constant, recorded in Task 2 Step 7). Stage 2 re-establishes coloring determinism via the deterministic create order + within-color order-independence (Task 6 run-twice pins it). The awake/kinematic-set iteration order is append/swap-remove (non-ascending, inherited from Phase B) — safe because every per-body op is independent.

**The de-risking discipline (oracle-gated, as in Phase A/B):**
1. Task 1 maintains the kinematic-set but the solver does not use it (invariant test only) before Task 2 consumes it.
2. Task 4 maintains the persistent coloring + a `ValidatePersistentColoring` oracle BEFORE the solver consumes it (Task 5).
3. The Stage-1 equivalence baseline (Task 2 Step 1) + the lane-width-invariance test (Task 2 Step 7) bracket the re-home.

**Soft spots (called out for the implementer + reviewer):**
1. **Kinematic vs static B-endpoint gating (Task 2 Step 4)** is the subtlest point: a kinematic B must map to its real dense row (its authored velocity drives the push), a static B must map to the zero dummy. The gate uses `invMassB>0` (dynamic) / `KinematicIndexOf != kNotKinematic` (kinematic) / else dummy. A mis-gate silently loses a kinematic push or gathers a stale velocity — the Task 2 equivalence test includes a kinematic plate to catch it.
2. **The Stage-1 win is locality, not gather-elimination** (conventions + Task 3 Step 3). Do NOT treat a modest solve delta as a failure — the user accepted the architecture (Phase-D substrate) as the goal.
3. **Spans -> overflow (Task 5).** Fine for body/Sandbox scenes (spans are the tile-collision path, rarely hit); a tile-heavy scene grows the overflow scalar path — a deferrable refinement.
4. **`kColorCount=12` kept** (32-bit `m_bodyColorMask` caps at 32; the `PhysicsSimdSolverTest.cpp:990` overflow scene assumes 12). Out of Phase-C scope.
5. **The `PhysicsSimdSolverTest` dummy/sizing contract** (Task 2 Step 7): it constructs its own `BodyStateSoA` sized `bodyCount+1` with `dummyIndex=bodyCount`. The re-home changes the solver's sizing but the TEST builds its own SoA directly, so it may keep its own contract OR be re-pointed at the compacted contract — verify which, and if the test feeds `PackLane`/`Build`, update its index expectations to `solverIndex` (a deliberate, recorded test re-baseline within the lane-width invariant).

**Type/name consistency:** `m_kinematicBodies`/`m_kinematicIndex`/`kNotKinematic`/`KinematicBodies()`/`ForEachKinematic`/`AddToKinematicSet`/`RemoveFromKinematicSet`/`KinematicIndexOf`/`KinematicCount`; `AwakeIndexOf`/`AwakeCount`; `solverIndex`/`solverCount`/`dummyIndex`; `m_bodyColorMask`/`m_colorContacts`/`AssignContactColor`/`ReleaseContactColor`/`ContactColorOf`/`ValidatePersistentColoring`; `Contact::color`/`ContactConstraint::color`/`kInvalidColor`; `m_colorRefs`/`m_overflowRefs` — used consistently across tasks.

**Stage independence:** each stage keeps the engine green + deterministic + shippable on the one branch; the user may land Stage 1 and re-evaluate Stage 2 after the Stage-1 `[STEPPROF]`, though the default is both.
