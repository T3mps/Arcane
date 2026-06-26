# Arcane 2D Physics — Phase B: Awake-Set + Sleep-by-Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a sleeping island cost ~zero per `Step` by maintaining an explicit **awake-set** of awake dynamic body slots, migrating islands in/out of it on sleep/wake, and switching the per-Step hot loops to iterate the awake-set instead of `0..Count` — **over the EXISTING slot SoA** (no `Body`/`BodySim`/`BodyState` record split; that is Phase C). Plus the **quiescence** work so a connected pile actually settles below the sleep threshold and sleeps (without which there is nothing to migrate out). Determinism (run-twice-identical) is the hard contract.

**Architecture:** Phase A gave us a persistent island registry (per-body `m_islandId` + id-indexed `m_islands` pool) and per-island sleep. Today, even with a sleeping island, the Step's ~7 body loops still iterate every slot `0..m_count` and branch on `m_awake[i]`; the solver's `BodyStateSoA` `SyncIn` copies every slot's velocity; sleeping proxies still appear in `UpdatePairs`. Phase B adds an incrementally-maintained `m_awakeBodies` list (awake **dynamic** slots) + an O(1) membership index, maintained at the create/sleep/wake/remove seams, and reroutes the hot loops to walk it. The `BodyStateSoA` stays world-slot-indexed and `ContactConstraint::bodyA/bodyB` stay world slots — **the SIMD batch build is untouched** (Phase C compacts it). Phase B is a *loop-domain* change (iterate the awake set), not a *storage* change. It also lands the quiescence fix so piles sleep, a zero-velocity-kinematic proxy gate, and promotes the throwaway `[STEPPROF]` instrumentation into a gated build switch (the per-phase measurement gate).

**Tech Stack:** C++23, Core (presentation-free, /MD for Arcane.dll + static-CRT for ArcaneCore), glm + std + sibling Physics headers only, Catch2 (`[physics]` / `[physics][island]` / a new `[physics][awakeset]`), premake5 / MSBuild via `Arcane.slnx`. Branch `feature/arcane-physics-phaseB-awake-set` (to be created OFF the Phase A branch `feature/arcane-persistent-islands` — Phase B builds directly on the persistent islands; do NOT branch off main, which lacks Phase A).

---

## Conventions

- **Branch:** create `feature/arcane-physics-phaseB-awake-set` off `feature/arcane-persistent-islands` (Phase A HEAD `d427ab4`). Phase A is NOT yet merged to main; Phase B depends on it. Do NOT push (the user merges/pushes manually after a visual + CI gate). Commit per task with the trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
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
  Use `"[awakeset]"` / `"[island]"` for subsets, or a quoted full case name for one case. Release exe dir is `bin/Release-windows-x86_64-md/ArcaneTests`. This machine HAS a capable GPU (RTX 3070) — run `[gpu]` tests (do NOT exclude them) in the final gate.
- **ArcaneCore static-CRT (server flavor):** the same `Arcane/Core/src` sources compile under the static CRT and must stay clean. Build Debug + Release:
  ```
  "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
  ```
  (and again `-p:Configuration=Release`).
- **New files → regen BOTH workspaces by ABSOLUTE path** (both Core and Tests globs are `%{prj.location}/src/**.cpp`/`**.hpp`, so a NEW `.cpp` requires a regen). Run from `Arcane/` AND from `Server/` (NOT `GenerateProjects.bat` — it hangs on a `pause`; NOT a relative path):
  ```
  cd "D:/dev/starworks/Gacha/Arcane" ; & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  cd "D:/dev/starworks/Gacha/Server" ; & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  ```
  (Phase B's new test file `PhysicsAwakeSetTest.cpp` IS new → regen. `m_awakeBodies`/`m_awakeIndex` are added in place to existing files → no regen for those.)
- **clangd / IDE diagnostics are FALSE POSITIVES — MSVC/MSBuild + the test run are the only truth.** Build flags `/fp:strict /arch:AVX2`, no fast-math (determinism). ASCII comments, explain WHY. C++23. Commands run via the PowerShell tool (chain with `;`, not `&&`).
- **DETERMINISM IS THE CONTRACT** — run-twice-identical. There are NO exact goldens; the `[physics]` + `[physics][island]` + the new `[physics][awakeset]` behavioral suites + the run-twice determinism cases are the gate. The awake-set iteration order is NOT ascending-slot (it is insertion/swap-remove order); this is determinism-safe ONLY because every per-body op in the rerouted loops is independent (no cross-body accumulation) — a run-twice test pins it.

---

## Name glossary (use these EXACTLY across all tasks)

- `kNotAwake` — `static constexpr std::uint32_t kNotAwake = 0xFFFFFFFFu;` (a body slot not in the awake-set: static, kinematic, sleeping, or dead). Lives in `PhysicsWorld.hpp` near the island constants.
- `m_awakeBodies` — `std::vector<std::uint32_t>` — the dense list of awake **DYNAMIC** body slots (the solver/integrate working set). Append-on-wake/create, swap-remove-on-sleep/destroy. Order is NOT ascending-slot.
- `m_awakeIndex` — `std::vector<std::uint32_t>` — per-body-slot column: this slot's position in `m_awakeBodies`, or `kNotAwake` if not in the set. Enables O(1) membership test + O(1) swap-remove. Sized in `EnsureCapacity` next to `m_awake`.
- `AddToAwakeSet(slot)` — append a dynamic slot to `m_awakeBodies` + set `m_awakeIndex[slot]`; no-op if already present (`m_awakeIndex[slot] != kNotAwake`) or non-dynamic.
- `RemoveFromAwakeSet(slot)` — swap-remove a slot from `m_awakeBodies` (patch the swapped element's `m_awakeIndex`); set `m_awakeIndex[slot] = kNotAwake`; no-op if not present.
- `ForEachAwake(fn)` — visit each awake dynamic slot (iterates `m_awakeBodies`). The replacement for the `for (i=0..count) if(awake&&dynamic)` loops.
- `[STEPPROF]` — the opt-in gated per-Step-phase timing instrumentation (Task 1). Build-switch `ARCANE_STEPPROF` (off by default).

---

## Verified seams + facts (quoted from a code read of `feature/arcane-persistent-islands`; confirm exact current lines before editing — anchors are approximate)

**Body storage + lifecycle (PhysicsWorld.hpp/.cpp):**
- Body SoA columns indexed by slot: `m_posX/m_posY`, `m_prevX/m_prevY`, `m_velX/m_velY`, `m_angle`, `m_angVel`, `m_invMass`, `m_invInertia`, `m_btype` (`uint8_t` BodyType; Static=0/Kinematic=1/Dynamic=2), `m_alive` (`uint8_t`), `m_awake` (`uint8_t`), `m_sleepTimer` (`Real`), `m_islandId` (`uint32_t`), `m_linDamp`, `m_localCenterX/Y`, etc. Declared ~PhysicsWorld.hpp:1096-1138.
- `EnsureCapacity(n)` (~cpp:149-194) grows all body-SoA vectors in lockstep (amortized doubling); `m_awake` defaults to 1, `m_islandId` to `kInvalidIsland`. ADD `m_awakeIndex` resize here (default `kNotAwake`).
- `AddBody(def)` (~cpp:826-1023): pops a free slot or appends (`m_count`/`m_free`), inits SoA, the Phase-A island block (Dynamic → `AllocIsland`, ~cpp:876-885). New dynamic bodies start `m_awake=1`. ADD `AddToAwakeSet(idx)` for Dynamic here.
- `RemoveBody(h)` (~cpp:1025-1142): sets `m_alive=0`, bumps gen, `DestroyContactsForBody`, the Phase-A island-release block (~cpp:1072-1096), pushes `idx` to `m_free`. ADD `RemoveFromAwakeSet(idx)` here.
- `m_count` = high-water mark; loops iterate `[0,m_count)` testing `m_alive`. `m_free` = LIFO recycled-slot stack.

**The awake gate skip-sites today (the loops to reroute) — all gate `!m_awake[i]` / `!AwakeSlot(i)`:**
1. Stage-1 prev-snapshot + kinematic integrate — `for (i=0..m_count)` NO awake gate (snaps ALL alive; integrates kinematics). ~PhysicsWorld.cpp:1487-1508. `UpdateMoverProxies(i)` for kinematics is UNCONDITIONAL (no velocity/move gate).
2. `UpdateContacts` dynamic static-candidate sub-loop — `for (i=0..m_count)`, skips non-Dynamic/`!m_awake[i]`/sensor. ~PhysicsWorld.cpp:2038-2043.
3. `UpdateContacts` kinematic sub-loop — `for (i=0..m_count)` kinematics. ~PhysicsWorld.cpp:2201.
4. `EmitContactConstraints` — per-contact awake gate `m_awake[aIdx]==0 → skip`. ~PhysicsWorld.cpp:2496. (Per-CONTACT, not a body loop — leave as-is in Phase B.)
5. Solver `SyncIn` — `for (i=0..count)` copies ALL slots' velocity into `BodyStateSoA`; awake dynamics also zero `dp/dq`. ~SoftStep.cpp:77-109.
6. Solver `SyncOut` — `for (i=0..count)`, awake-dynamic gate, `SetVelSlot`. ~SoftStep.cpp:111-124.
7. `IntegrateVelocitiesSoA` — `for (i=0..count)`, awake-dynamic gate, gravity+damping. ~SoftStep.cpp:246-278.
8. `IntegratePositionsSoA` — `for (i=0..count)`, awake-dynamic gate, dp/dq accumulate. ~SoftStep.cpp:280-298.
9. `FinalizePositionsSoA` — `for (i=0..count)`, awake-dynamic gate, `CommitSlotPosition(i,...)`. ~SoftStep.cpp:300-329.
10. `Island::UpdateSleep` body loops — `for (i=0..count)`, awake/dynamic gates. ~Island.cpp:42-136. (Phase-A; per-island sleep apply.)
11. `BulletSweep` — `for (i=0..count)`, `m_bullet` gate, no awake gate. ~PhysicsWorld.cpp:1601-1610. (Bullets are rare; leave as-is in Phase B — flag only.)

**Solver coupling (SoftStep.cpp / BodyStateSoA / ContactConstraintSimd.hpp):**
- `BodyStateSoA`: six `std::vector<float>` columns (`vx,vy,w,dpx,dpy,dq`) indexed by RAW WORLD SLOT, sized `count+1` (the `+1` is the dummy slot at index `count`). `SyncIn` mirrors world→SoA; `SyncOut` writes velocity back; integrate/finalize use the dp/dq accumulators.
- `ContactConstraint::bodyA/bodyB` are WORLD SLOTS; `ContactConstraintSimd::PackLane` stores them as `bodyIndexA/B[L]`; the lane-wide passes `iload` + `GatherBody(bs.vx, ia)` by world slot. **Phase B does NOT touch any of this** (it stays world-slot-indexed; Phase C compacts). The dummy slot = `count`.
- The solver gathers B-endpoint velocity for static/kinematic B from `BodyStateSoA[B_slot]`; the scatter `select(dyn=false, …)` re-gathers the unchanged value, so static/kinematic SoA velocities MUST be present and correct. Statics are always 0 (write-once); kinematics change via `SetVelocity`.
- Sleep apply (Island::UpdateSleep): `SetAwakeSlot(b,false); SetVelSlot(b,{0,0}); SetAngVelSlot(b,0)`. Wake (`WakeIsland`): `m_awake[b]=1; m_sleepTimer[b]=0` for all island members. Wake call sites: `SetVelocity`/`ApplyImpulse`×2/`Wake`/`WakeMoverPair`/`AddJoint`/contact-create-wake.

**Broadphase (Broadphase/*, PhysicsWorld.cpp):**
- `m_fixtureBroadphase` = `DynamicTree`, keyed by FIXTURE slot; fat-AABB margin `kMargin=8`; containment fast-path (tight-fits-fat → no reinsert, but still marks `m_moved`). Move-buffer = `std::unordered_set<uint32_t> m_moved`, inserted UNCONDITIONALLY on every `Update()`. `UpdatePairs(out)` drains/rebuilds the pair set from moved proxies, clears `m_moved`.
- `m_residencyGrid`/`m_staticGrid` = `SpatialGrid` (NOT IBroadphase), keyed by BODY slot; `Move(id,box)` = remove+insert (unconditional).
- `UpdateMoverProxies(b)` (~cpp:736-760): `m_residencyGrid.Move(b, SlotAabb(b))` (unconditional) + per-fixture `m_fixtureBroadphase->Update(fi, …)`.
- **FACT:** sleeping DYNAMIC bodies already pay ~zero proxy refresh (the awake gate in `FinalizePositionsSoA` blocks `CommitSlotPosition`→`UpdateMoverProxies`). Remaining waste: (a) zero-velocity kinematics call `UpdateMoverProxies` every Step unconditionally (Stage 1), and (b) sleeping bodies' proxies stay in the tree so they still appear in `UpdatePairs` output (their contacts are `BothAsleep`-gated in narrowphase, so the cost is the pair-set membership, not narrowphase).

---

## File Structure

| File | Created/Modified | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | Modified | `kNotAwake`, `m_awakeBodies`/`m_awakeIndex` members, `AddToAwakeSet`/`RemoveFromAwakeSet`/`ForEachAwake` decls + the inline `ForEachAwake`, `AwakeBodies()` accessor (for the solver). The `[STEPPROF]` macro hooks (Task 1). |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | Modified | Size `m_awakeIndex` in `EnsureCapacity`; `AddToAwakeSet` on dynamic `AddBody`; `RemoveFromAwakeSet` on `RemoveBody`; the awake-set maintenance in the sleep apply + every wake path (route through the set); reroute Stage-1 snapshot + the `UpdateContacts` dynamic sub-loop to the awake-set; the zero-velocity-kinematic proxy gate; `[STEPPROF]` scope timers. Implement `AddToAwakeSet`/`RemoveFromAwakeSet`. |
| `Arcane/Core/src/Arcane/Physics/Island.cpp` | Modified | In `UpdateSleep`, when an island sleeps, remove its members from the awake-set (the set is the new sleep seam); the per-body idle-timer loop iterates the awake-set instead of `0..count`. |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` | Modified | Reroute `SyncIn`/`SyncOut`/`IntegrateVelocitiesSoA`/`IntegratePositionsSoA`/`FinalizePositionsSoA` to iterate the awake-set (+ a kinematic refresh pass for `SyncIn`). `ContactConstraint`/SIMD batch build UNCHANGED. |
| `Arcane/Core/src/Arcane/Physics/StepProf.hpp` | **Created** | The `[STEPPROF]` gated build switch: `ARCANE_STEPPROF`-conditional scoped timers + a per-phase accumulator dumped on demand. Header-only, presentation-free, no-op when the switch is off. |
| `Arcane/Tests/src/PhysicsAwakeSetTest.cpp` | **Created** | New `[physics][awakeset]` behavioral tests: the awake-set invariant (set == awake-dynamic slots) across create/sleep/wake/remove/recycle; iterate-awake-only equivalence (a settled scene's positions/velocities/sleep-states are bit-identical to the pre-Phase-B baseline within an invariant); a 10k-ish settle scene actually sleeps (quiescence); run-twice determinism with the awake-set. |

---

### Task 1: Promote `[STEPPROF]` to a gated build switch (the measurement gate)

**Files:** Create `Arcane/Core/src/Arcane/Physics/StepProf.hpp`; Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (wrap each Step stage in a scope timer); Modify `Arcane/Core/premake5.lua` ONLY IF a per-config define is needed (prefer a header `#ifndef ARCANE_STEPPROF` default-off so NO premake change is needed).

This is the prerequisite measurement instrument: every later task's win/regression is judged by `[STEPPROF]`. It MUST be zero-cost when off (the default) so it never affects the determinism/behavioral gate. It is opt-in via `-p:ARCANE_STEPPROF=1`-style define OR a manual `#define ARCANE_STEPPROF 1` at the top of `StepProf.hpp` for a local measuring build (document both; default OFF).

- [ ] **Step 1: write the failing test.** Create `Arcane/Tests/src/PhysicsAwakeSetTest.cpp` (this file accretes all Phase B tests). Add a compile/smoke test that the StepProf API exists and is a no-op by default:
  ```cpp
  // Physics Phase B: awake-set + sleep-by-migration -- BEHAVIORAL tests.
  // Companion to PhysicsIslandTest.cpp / PhysicsPersistentIslandTest.cpp.
  // PRESENTATION-FREE + C++23-clean.
  #include <cstdint>
  #include <vector>
  #include <catch2/catch_test_macros.hpp>
  #include <Arcane/Physics/PhysicsTypes.hpp>
  #include <Arcane/Physics/Shapes.hpp>
  #include <Arcane/Physics/Body.hpp>
  #include <Arcane/Physics/PhysicsWorld.hpp>
  #include <Arcane/Physics/StepProf.hpp>
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
  // StepProf is a no-op by default: a Step still runs and the scoped timers compile away.
  TEST_CASE("PhysicsAwakeSet: StepProf is a no-op when ARCANE_STEPPROF is off", "[physics][awakeset]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      for (int k = 0; k < 10; ++k) { w.Step(kStep); }
      // The default build defines ARCANE_STEPPROF as 0 -> the accumulator is empty.
      REQUIRE(StepProf::Enabled() == false);
  }
  ```
- [ ] **Step 2: regen BOTH workspaces (new test file + new StepProf.hpp), build Debug, verify FAIL** (StepProf.hpp does not exist → compile error).
- [ ] **Step 3: implement `StepProf.hpp`.**
  ```cpp
  // StepProf: opt-in per-Step-phase timing. ZERO cost when ARCANE_STEPPROF is
  // off (the default) -- the scoped timer compiles to nothing and Enabled()
  // is constexpr false. Turn on for a measuring build only (it is NOT in the
  // determinism/behavioral gate path). Presentation-free, header-only.
  #pragma once
  #include <cstdint>
  #ifndef ARCANE_STEPPROF
  #define ARCANE_STEPPROF 0
  #endif
  #if ARCANE_STEPPROF
  #include <chrono>
  #include <array>
  #include <cstdio>
  #endif
  namespace Arcane::Physics::StepProf
  {
      // Stable phase ids (one per Step stage). Keep in lockstep with the
      // ARCANE_STEPPROF_SCOPE call sites in PhysicsWorld::Step.
      enum class Phase : std::uint32_t {
          Stage1Snapshot = 0, Narrowphase, EmitConstraints, Solve,
          WarmStartWriteback, Bullet, IslandSleep, Events, Count
      };
      constexpr bool Enabled() noexcept { return ARCANE_STEPPROF != 0; }
  #if ARCANE_STEPPROF
      // One accumulator per phase (nanoseconds + call count), process-global.
      struct Acc { std::uint64_t ns = 0; std::uint64_t calls = 0; };
      inline std::array<Acc, static_cast<std::size_t>(Phase::Count)>& Table() {
          static std::array<Acc, static_cast<std::size_t>(Phase::Count)> t{}; return t;
      }
      struct Scope {
          Phase p; std::chrono::high_resolution_clock::time_point t0;
          explicit Scope(Phase ph) : p(ph), t0(std::chrono::high_resolution_clock::now()) {}
          ~Scope() {
              const auto t1 = std::chrono::high_resolution_clock::now();
              auto& a = Table()[static_cast<std::size_t>(p)];
              a.ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
              a.calls += 1;
          }
      };
      inline void Reset() { for (auto& a : Table()) { a = Acc{}; } }
      inline void Dump(const char* tag) {
          std::printf("[STEPPROF] %s\n", tag);
          const char* names[] = {"stage1","narrow","emit","solve","wswb","bullet","sleep","events"};
          for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::Count); ++i) {
              const auto& a = Table()[i];
              const double ms = a.calls ? (double)a.ns / 1e6 / (double)a.calls : 0.0;
              std::printf("  %-8s %8.4f ms/step  (%llu calls)\n", names[i], ms, (unsigned long long)a.calls);
          }
      }
  #else
      struct Scope { explicit Scope(Phase) noexcept {} };
      inline void Reset() noexcept {}
      inline void Dump(const char*) noexcept {}
  #endif
  }
  #define ARCANE_STEPPROF_SCOPE(phase) ::Arcane::Physics::StepProf::Scope arcane_stepprof_scope_##__LINE__{ ::Arcane::Physics::StepProf::Phase::phase }
  ```
- [ ] **Step 4: wrap each Step stage in `PhysicsWorld::Step`** with `ARCANE_STEPPROF_SCOPE(...)` at the top of each stage's block (Stage1Snapshot, Narrowphase = `UpdateContacts`, EmitConstraints, Solve, WarmStartWriteback, Bullet, IslandSleep, Events). Each scope is a local RAII object; off-build it is an empty struct → zero cost. Include `<Arcane/Physics/StepProf.hpp>` in PhysicsWorld.cpp.
- [ ] **Step 5: build Debug, run the new test + `[physics]`.** Expected: green (StepProf off by default → `Enabled()==false`; Step unaffected). Confirm the full `[physics]` suite is byte-unchanged (StepProf adds no behavior).
- [ ] **Step 6: ArcaneCore clean (Debug + Release), commit.** `feat(arcane/physics): [STEPPROF] opt-in gated per-Step-phase timing (zero-cost when off)`. Trailer.

---

### Task 2: The awake-set data structure + incremental maintenance (behavior-preserving)

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`, `Arcane/Core/src/Arcane/Physics/Island.cpp`; extend `PhysicsAwakeSetTest.cpp`.

Add `m_awakeBodies` + `m_awakeIndex` and maintain them at create/sleep/wake/remove — but DO NOT yet reroute any loop. After this task the set always equals "the awake dynamic slots", verified by an invariant test, while the Step still iterates `0..count` (so behavior is byte-identical). This isolates the data-structure correctness from the loop-rerouting (Task 3/4).

- [ ] **Step 1: failing invariant test.** Append to `PhysicsAwakeSetTest.cpp` a test that drives create/sleep/wake/remove and asserts the awake-set membership matches the awake-dynamic predicate at each point, using a new read-only probe `AwakeBodies()` (added in Step 3):
  ```cpp
  // The awake-set must, at all times, contain EXACTLY the awake dynamic slots.
  TEST_CASE("PhysicsAwakeSet: set membership tracks awake-dynamic slots", "[physics][awakeset]")
  {
      auto checkInvariant = [](PhysicsWorld& w) {
          // Every entry of m_awakeBodies is an awake dynamic; every awake dynamic
          // slot is in the set exactly once.
          const std::vector<std::uint32_t>& set = w.AwakeBodies();
          std::vector<std::uint8_t> seen(w.Count(), 0u);
          for (const std::uint32_t s : set) {
              REQUIRE(s < w.Count());
              REQUIRE(w.Alive(s));
              REQUIRE(w.TypeSlot(s) == BodyType::Dynamic);
              REQUIRE(w.AwakeSlot(s));
              REQUIRE(seen[s] == 0u); // no duplicates
              seen[s] = 1u;
          }
          for (std::uint32_t i = 0; i < w.Count(); ++i) {
              const bool awakeDyn = w.Alive(i) && w.TypeSlot(i) == BodyType::Dynamic && w.AwakeSlot(i);
              REQUIRE((seen[i] != 0u) == awakeDyn);
          }
      };

      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const BodyHandle b0 = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      const BodyHandle b1 = AddBox(w, Vec2(Real(0), Real(-40)), Real(5), Real(5));
      checkInvariant(w);                                  // 2 awake dynamics
      for (int k = 0; k < 700; ++k) { w.Step(kStep); }
      checkInvariant(w);                                  // settled -> asleep -> set should be empty of them
      // Disturb -> wake -> back in the set.
      w.ApplyImpulse(b1, Vec2(Real(0), Real(-8000)));
      checkInvariant(w);
      // Remove a body -> out of the set; recycle the slot -> a fresh awake dynamic.
      w.RemoveBody(b0);
      checkInvariant(w);
      const BodyHandle b2 = AddBox(w, Vec2(Real(0), Real(-60)), Real(5), Real(5));
      (void)b2;
      checkInvariant(w);
  }
  ```
- [ ] **Step 2: build + verify FAIL** (`AwakeBodies()` does not exist → compile error).
- [ ] **Step 3: add the storage + API to `PhysicsWorld.hpp`.** Near the island constants add `static constexpr std::uint32_t kNotAwake = 0xFFFFFFFFu;`. Near `m_islandId` add:
  ```cpp
  // ---- awake-set (Phase B) ----------------------------------------
  // m_awakeBodies is the dense list of AWAKE DYNAMIC body slots -- the
  // solver/integrate working set. m_awakeIndex[slot] is the slot's
  // position in that list (or kNotAwake). Maintained incrementally at
  // create/sleep/wake/remove; iteration order is NOT ascending-slot
  // (append/swap-remove) -- safe because the rerouted loops do only
  // independent per-body work (a run-twice test pins it).
  std::vector<std::uint32_t> m_awakeBodies;
  std::vector<std::uint32_t> m_awakeIndex;   // per-body-slot position, or kNotAwake
  ```
  Add the public read-only probe + the inline visitor + the private maintainers:
  ```cpp
  // Read-only awake-set view (tests + solver iterate this).
  [[nodiscard]] const std::vector<std::uint32_t>& AwakeBodies() const noexcept { return m_awakeBodies; }
  // Visit each awake dynamic slot (the replacement for for(i=0..count) if(awake&&dynamic)).
  template <typename Fn> void ForEachAwake(Fn&& fn) const { for (const std::uint32_t s : m_awakeBodies) { fn(s); } }
  ```
  ```cpp
  // ---- awake-set maintenance (Phase B) ----------------------------
  void AddToAwakeSet(std::uint32_t slot) noexcept;       // append (idempotent; dynamic only)
  void RemoveFromAwakeSet(std::uint32_t slot) noexcept;  // swap-remove (idempotent)
  ```
- [ ] **Step 4: implement the maintainers in `PhysicsWorld.cpp`** (near the island helpers):
  ```cpp
  void PhysicsWorld::AddToAwakeSet(std::uint32_t slot) noexcept
  {
      // Only awake dynamics belong in the set; idempotent (already-present is a no-op).
      if (slot >= m_awakeIndex.size()) { return; }
      if (static_cast<BodyType>(m_btype[slot]) != BodyType::Dynamic) { return; }
      if (m_awakeIndex[slot] != kNotAwake) { return; }
      m_awakeIndex[slot] = static_cast<std::uint32_t>(m_awakeBodies.size());
      m_awakeBodies.push_back(slot);
  }
  void PhysicsWorld::RemoveFromAwakeSet(std::uint32_t slot) noexcept
  {
      if (slot >= m_awakeIndex.size()) { return; }
      const std::uint32_t pos = m_awakeIndex[slot];
      if (pos == kNotAwake) { return; }
      const std::uint32_t last = static_cast<std::uint32_t>(m_awakeBodies.size() - 1u);
      const std::uint32_t moved = m_awakeBodies[last];
      m_awakeBodies[pos] = moved;        // swap-remove
      m_awakeIndex[moved] = pos;         // patch the moved element's back-index
      m_awakeBodies.pop_back();
      m_awakeIndex[slot] = kNotAwake;
  }
  ```
- [ ] **Step 5: wire the seams (NO loop rerouting yet).**
  - `EnsureCapacity` (~cpp:189, next to `m_awake`): `m_awakeIndex.resize(next, kNotAwake);`.
  - `AddBody`, in the Dynamic branch right after the Phase-A island assignment (~cpp:885): `AddToAwakeSet(idx);` (a new dynamic starts awake). For the else/non-dynamic branch leave it out (statics/kinematics never enter the set). NOTE: a recycled slot's `m_awakeIndex[idx]` may carry a stale value from a prior life — set `m_awakeIndex[idx] = kNotAwake;` BEFORE the `AddToAwakeSet(idx)` call (so the idempotency guard does not wrongly skip), for BOTH branches.
  - `RemoveBody`, in the island-release area (~cpp:1072): `RemoveFromAwakeSet(idx);` BEFORE clearing other state (the slot is still typed Dynamic here). After removal the slot's `m_awakeIndex` is `kNotAwake`.
  - **Sleep apply** (`Island::UpdateSleep`, where it does `SetAwakeSlot(b,false)`): the awake-set is the new sleep seam — call `RemoveFromAwakeSet(b)` immediately after `SetAwakeSlot(b,false)`. (Add it inside the per-island sleep apply loop in Island.cpp. `RemoveFromAwakeSet` is public-or-friend accessible from Island.cpp via `world.` — add a thin public forwarder `void SleepRemoveFromAwakeSet(std::uint32_t)` IF `RemoveFromAwakeSet` must stay private; prefer making the two maintainers callable from Island.cpp by declaring them public, or add a `friend`. Simplest: keep them private and add the awake-set removal in `SetAwakeSlot` itself — see next bullet.)
  - **CLEANEST SEAM:** make `SetAwakeSlot(i,bool on)` itself maintain the set: when `on` transitions to false → `RemoveFromAwakeSet(i)`; when true → `AddToAwakeSet(i)`. Since EVERY sleep/wake path already routes through `SetAwakeSlot` / `m_awake[i]=1`, centralizing here covers Island sleep AND every wake path in one place. AUDIT: the wake paths set `m_awake[i]=1` DIRECTLY (not via `SetAwakeSlot`) in `WakeIsland`/`SetVelocity`/`ApplyImpulse`/`Wake`/`WakeMoverPair`. So either (a) route those through a shared `MarkAwake(i)` that also calls `AddToAwakeSet(i)`, or (b) make `WakeIsland` (the common fan-out) call `AddToAwakeSet(b)` for each member, and the direct `m_awake[i]=1` single-body safety-net writes also call `AddToAwakeSet(i)`. Choose (b): in `WakeIsland`'s member loop add `AddToAwakeSet(b);` next to `m_awake[b]=1`, and in each wake path's explicit single-body set add `AddToAwakeSet(i);`. For sleep, add `RemoveFromAwakeSet(b)` next to the `SetAwakeSlot(b,false)` in Island.cpp (declare the two maintainers public so Island.cpp can call them, OR add the removal in `SetAwakeSlot`). DECISION for this plan: **declare `AddToAwakeSet`/`RemoveFromAwakeSet` PUBLIC** (they are cheap, noexcept, idempotent; the island module already calls many public `World` methods), and call them explicitly at each sleep/wake/create/remove seam. Document each seam.
- [ ] **Step 6: build + run.** Kill strays, build Debug. The invariant test PASSES; full `[island]` + `[physics]` GREEN and BYTE-UNCHANGED (no loop rerouted yet → the sim is identical; the set is maintained in parallel). If any `[physics]` case shifts, the set maintenance accidentally changed behavior — diagnose (it must be pure bookkeeping).
- [ ] **Step 7: ArcaneCore clean (Debug + Release), commit.** `feat(arcane/physics): incremental awake-set (m_awakeBodies/m_awakeIndex) maintained at create/sleep/wake/remove`. Trailer.

---

### Task 3: Reroute the solver hot loops to iterate the awake-set

**Files:** `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp`; extend `PhysicsAwakeSetTest.cpp`.

Switch `SyncIn`/`SyncOut`/`IntegrateVelocitiesSoA`/`IntegratePositionsSoA`/`FinalizePositionsSoA` from `for (i=0..count)` to `world.ForEachAwake(...)`. The `BodyStateSoA` stays world-slot-indexed (sized `count+1`), so awake bodies write to their own slot exactly as before; the ONLY change is which slots are visited. `SyncIn` additionally needs static/kinematic B-endpoint velocities present in the SoA (the solver gathers them) — handle that explicitly (see Step 3).

- [ ] **Step 1: failing/equivalence test.** Append an equivalence test: a settled multi-box scene's per-body positions, velocities, and sleep-states after N steps must be bit-identical whether the solver iterates `0..count` or the awake-set. Since we cannot run both in one build, pin it as a run-twice + a known-good behavioral snapshot: assert that a settle scene reaches the SAME final state as `PhysicsIslandTest`'s determinism case expects (positions frozen, all asleep), and add a run-twice determinism case specific to the awake-set ordering:
  ```cpp
  // Rerouting the solver loops to the awake-set must not change the result:
  // a settle scene is run-twice-identical AND ends fully asleep + frozen.
  TEST_CASE("PhysicsAwakeSet: awake-only solve is deterministic + settles identically", "[physics][awakeset]")
  {
      auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
          std::vector<BodyHandle> boxes;
          for (int i = 0; i < 5; ++i) { boxes.push_back(AddBox(w, Vec2(Real(0), Real(-10) - Real(9)*static_cast<Real>(i)), Real(4), Real(4))); }
          for (int k = 0; k < 900; ++k) { w.Step(kStep); }
          pos.clear(); awake.clear();
          for (const BodyHandle b : boxes) { pos.push_back(w.Position(b)); awake.push_back(w.IsAwake(b)?1:0); }
      };
      std::vector<Vec2> p1, p2; std::vector<int> a1, a2;
      run(p1, a1); run(p2, a2);
      REQUIRE(p1.size() == p2.size());
      for (std::size_t i = 0; i < p1.size(); ++i) {
          REQUIRE(p1[i].x == p2[i].x); REQUIRE(p1[i].y == p2[i].y); REQUIRE(a1[i] == a2[i]);
      }
  }
  ```
- [ ] **Step 2: build + run the test on the PRE-reroute build** (it should already PASS — it is the baseline the reroute must preserve). Record the final positions/awake states informally (they are the regression reference).
- [ ] **Step 3: reroute the loops in `SoftStep.cpp`.**
  - `SyncIn` (~77-109): split into (a) a `ForEachAwake` pass that copies awake dynamics' `vx/vy/w` AND zeros `dpx/dpy/dq`; (b) ensure static/kinematic B-endpoint velocities are present. Statics are always 0: rely on `BodyStateSoA::Resize` zero-init + a one-time write (statics never change). Kinematics change via `SetVelocity` — add a SMALL pass over a kinematic list OR (simplest, correct, low-cost) keep a single `for (i=0..count) if Kinematic` pass that copies kinematic velocity (kinematics are rare vs the dynamic pile; this preserves correctness without a new list). So `SyncIn` = `ForEachAwake` (dynamics) + a kinematic-only `for(i=0..count)` copy. (If profiling later shows the kinematic scan matters, add an `m_kinematicBodies` list — defer; YAGNI.) IMPORTANT: do NOT zero a sleeping dynamic's SoA velocity here — sleeping dynamics are no longer synced; their `BodyStateSoA[slot]` is stale but UNREAD (no awake contact references a sleeping A; a sleeping B endpoint is only referenced if its island is awake, which would have synced it — verify: a contact between awake-A and sleeping-B implies they would merge to one island and B would be awake; a sleeping pair is `BothAsleep`-gated out of `EmitContactConstraints`, so no constraint references a sleeping slot. CONFIRM this in Step 5 via the invariant that `EmitContactConstraints` only emits awake-A contacts (seam #4) — a sleeping B with an awake A cannot exist because they would share an island).
  - `SyncOut` (~111-124): `ForEachAwake` → `SetVelSlot`/`SetAngVelSlot`. (Drop the per-slot awake/dynamic gate — the set IS the gate.)
  - `IntegrateVelocitiesSoA` (~246-278), `IntegratePositionsSoA` (~280-298), `FinalizePositionsSoA` (~300-329): `ForEachAwake` → the same body math. Drop the `!Awake`/`!Dynamic` per-slot gates (the set is the gate); KEEP the `InvMassSlot(i) <= 0` guard in IntegrateVelocities (a zero-invMass dynamic is degenerate). `FinalizePositionsSoA` still calls `CommitSlotPosition(i,…)` which refreshes proxies — now only for awake bodies (already true via the old gate; unchanged).
  - The `count`/`dummyIndex` for the SIMD batch (`m_bodyState.Resize(count+1)`, `dummyIndex=count`) STAYS = `world.Count()` (BodyStateSoA is still world-slot-indexed). Do NOT change it.
- [ ] **Step 4: build + run.** Kill strays, build Debug. Expected: the equivalence/determinism test PASSES; full `[island]` + `[physics]` GREEN with the SAME final states as the Step-2 baseline (a settle scene still settles + sleeps; positions match within the invariant). If a position shifted, the reroute changed which bodies integrate — diagnose (most likely a sleeping B-endpoint that WAS being synced and is now stale; confirm the `EmitContactConstraints` awake-A gate prevents any constraint from referencing a sleeping slot, or extend `SyncIn` to also sync any slot referenced by an emitted constraint).
- [ ] **Step 5: prove the sleeping-slot-unreferenced invariant.** Add a debug assertion (Debug-only, behind `assert`) in `EmitContactConstraints` that every emitted `cc.bodyA`/`cc.bodyB` (when `bodyBIsBody`) is either awake-dynamic or static/kinematic — never a sleeping dynamic. Run `[physics]` Debug to exercise it across the suite. (This documents WHY the SyncIn reroute is safe.)
- [ ] **Step 6: ArcaneCore clean (Debug + Release), commit.** `refactor(arcane/physics): solver hot loops iterate the awake-set (SyncIn/SyncOut/Integrate/Finalize); BodyStateSoA stays world-slot-indexed`. Trailer.

---

### Task 4: Reroute the Stage-1 snapshot + the UpdateContacts dynamic sub-loop; snap prev-on-sleep

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`, `Arcane/Core/src/Arcane/Physics/Island.cpp`; extend `PhysicsAwakeSetTest.cpp`.

The Stage-1 prev-snapshot currently snaps ALL alive bodies every step; a sleeping body's position is frozen, so its `prev` only needs setting ONCE at sleep time. Reroute Stage 1 to snap kinematics + awake dynamics, and snap `prev=pos` for an island's members AT the moment it sleeps. Reroute the `UpdateContacts` dynamic static-candidate sub-loop to `ForEachAwake`.

- [ ] **Step 1: failing test.** Append a test that render-lerp (`DrawPosition`) of a SLEEPING body is stable (prev==pos, so any alpha gives the frozen position) and that a sleeping body's `DrawPosition` does not drift across steps:
  ```cpp
  TEST_CASE("PhysicsAwakeSet: sleeping body render-lerp is frozen (prev==pos)", "[physics][awakeset]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const BodyHandle b = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      for (int k = 0; k < 700; ++k) { w.Step(kStep); }
      REQUIRE_FALSE(w.IsAwake(b));
      const Vec2 mid = w.DrawPosition(b, Real(0.5));
      const Vec2 pos = w.Position(b);
      REQUIRE(mid.x == pos.x); REQUIRE(mid.y == pos.y); // prev==pos -> no lerp drift
      for (int k = 0; k < 60; ++k) { w.Step(kStep); }
      const Vec2 mid2 = w.DrawPosition(b, Real(0.5));
      REQUIRE(mid2.x == pos.x); REQUIRE(mid2.y == pos.y); // still frozen
  }
  ```
- [ ] **Step 2: build + run** — should PASS today (the all-bodies snapshot keeps prev==pos for a frozen body). It is the invariant the reroute must preserve.
- [ ] **Step 3: snap prev-on-sleep.** In `Island::UpdateSleep`'s per-island sleep apply (where it sleeps a member), add `world.SnapPrevToPos(b);` (a new thin public method `void SnapPrevToPos(std::uint32_t i){ m_prevX[i]=m_posX[i]; m_prevY[i]=m_posY[i]; }`) so a freshly-slept body's `prev` equals its frozen `pos`. (Also call it once for safety; it is idempotent.)
- [ ] **Step 4: reroute Stage 1.** Replace the Stage-1 `for (i=0..m_count)` snapshot loop: snap `prev` for (a) all KINEMATIC bodies (they integrate) and (b) all AWAKE DYNAMIC bodies via `ForEachAwake`; integrate kinematics as today. Sleeping dynamics are NOT snapped (their prev was set at sleep time and pos is frozen). Statics: never move, prev==pos always; snap them once at `AddBody` (set `m_prevX/Y=pos`) — confirm `AddBody` already sets prev=pos (it does). So Stage 1 becomes: a kinematic pass (`for i: if Kinematic { snap+integrate+UpdateMoverProxies }` — see Task 6 for the move-gate) + `ForEachAwake` snap of awake dynamics.
- [ ] **Step 5: reroute the UpdateContacts dynamic sub-loop** (~cpp:2038): replace `for (i=0..m_count) if(Dynamic && awake && !sensor)` with `ForEachAwake([&](uint32_t i){ if (m_sensor[i]) return; ... })`. The body of the loop (static-candidate query + contact create/update) is unchanged. The kinematic sub-loop (~cpp:2201) stays `for(i=0..count)` (kinematics are not in the awake-set; defer a kinematic list to YAGNI).
- [ ] **Step 6: build + run.** Kill strays, build Debug. Expected: the render-lerp test + full `[island]` + `[physics]` GREEN, same final states. A sleeping body's `DrawPosition` is frozen. If a sleeping body's contacts with a NEW awake mover are missed (because the awake mover's sub-loop only queries static candidates, and the body-body pair comes from the fixture broadphase `UpdatePairs` which is unchanged), confirm wake-on-contact still fires (PhysicsIslandTest case 3). The static-candidate sub-loop only handles dynamic-vs-STATIC; dynamic-vs-dynamic still flows through `m_fixtureBroadphase->UpdatePairs` + the pool walk, which is awake-agnostic — so a moving body into a sleeper still creates the contact and `WakeMoverPair` wakes it. VERIFY case 3 passes.
- [ ] **Step 7: ArcaneCore clean (Debug + Release), commit.** `refactor(arcane/physics): Stage-1 snapshot + UpdateContacts dynamic sub-loop iterate the awake-set; snap prev-on-sleep`. Trailer.

---

### Task 5: Quiescence — make a connected pile actually settle below threshold and sleep (MEASUREMENT-DRIVEN)

**Files:** investigation across `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` + `Island.cpp` (the fix is determined by measurement — do NOT pre-commit to a specific change); extend `PhysicsAwakeSetTest.cpp`.

**This is the crux of the perf win and is inherently investigation-gated.** A fully-connected pile is ONE island; "island sleeps as a unit" means one body jittering above `kSleepLinVel2` vetoes sleep for the whole pile. Phase A did NOT change that. The awake-set machinery (Tasks 2-4) only pays off if islands ACTUALLY sleep. This task measures whether a dense settle scene sleeps, diagnoses why not, and applies the MINIMAL fix — re-baselining numerics deliberately within the sleep invariant per `feedback_engine_evolves_not_frozen`. Do NOT weaken the sleep contract; make piles genuinely quiescent.

- [ ] **Step 1: the measurement test (RED = the pile does NOT fully sleep).** Append a test that builds a dense settled pile and asserts it sleeps within a generous budget:
  ```cpp
  // A dense settled pile must fully sleep (every body asleep) within the budget.
  // This is the Phase B sleep WIN: a connected pile that quiesces migrates out of
  // the awake set. If it FAILS, the soft solver leaves residual jitter above the
  // sleep threshold -> Task 5's investigation finds the minimal quiescence fix.
  TEST_CASE("PhysicsAwakeSet: a dense settled pile fully sleeps", "[physics][awakeset]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));
      // A pyramid/grid pile of ~60 boxes -> one connected island.
      std::vector<BodyHandle> boxes;
      const Real hw = Real(4), hh = Real(4);
      for (int row = 0; row < 6; ++row) {
          const int n = 10 - row;
          for (int c = 0; c < n; ++c) {
              const Real x = (static_cast<Real>(c) - static_cast<Real>(n)*Real(0.5)) * (Real(2)*hw + Real(0.2));
              const Real y = Real(-5) - static_cast<Real>(row) * (Real(2)*hh + Real(0.2));
              boxes.push_back(AddBox(w, Vec2(x, y), hw, hh));
          }
      }
      // Generous budget: 5 seconds of sim.
      for (int k = 0; k < 300; ++k) { w.Step(kStep); }
      int awake = 0; for (const BodyHandle b : boxes) { if (w.IsAwake(b)) ++awake; }
      REQUIRE(w.AwakeBodies().empty());   // the whole pile migrated out of the awake set
      (void)awake;
  }
  ```
- [ ] **Step 2: measure.** Build Debug + a measuring build with `-DARCANE_STEPPROF=1` (or a local `#define ARCANE_STEPPROF 1`). Run the test. If it FAILS (some bodies never sleep), instrument WHY: count how many bodies are awake at steady state; log the max `|v|`/`|angVel|` of the still-awake bodies vs `kSleepLinVel2`/`kSleepAngVel`; determine whether the residual is a few edge bodies (a tuning issue) or a systemic floor jitter (a relax-pass issue). Use the `[STEPPROF]` dump to confirm `sleep` cost is ~µs (it is) and that the WIN must come from the pile actually sleeping (so the awake-set shrinks).
- [ ] **Step 3: diagnose + choose the MINIMAL fix (decision tree — pick exactly one, by evidence, NOT by guess).**
  - **If the relax pass leaves a small residual velocity floor:** verify the second solve pass runs with `useBias=false` (the relax pass) and that `IntegratePositionsSoA` does NOT re-inject bias velocity into the stored velocity (Box2D separates "solve velocity" from "relax velocity" so the STORED velocity used for sleep is the relaxed one). The likely minimal fix: ensure the velocity that `Island::UpdateSleep` reads (`VelSlot`) is the POST-relax velocity, and that the relax pass actually drives it below `kSleepLinVel2`. Confirm against SoftStep.cpp's sub-step order (researcher: `SolveNormalAndFriction(useBias=true)` then `IntegratePositions` then `SolveNormalAndFriction(useBias=false)` + `OverflowSolve`).
  - **If a few perpetually-jittering edge bodies veto an otherwise-quiet island:** consider Box2D's actual sleep criterion — it uses the body's FARTHEST-POINT linear speed (linear + |angVel|*radius), and a per-body `sleepTime` min across the island. Confirm our threshold is comparable; a too-tight `kSleepLinVel2` (=4 → |v|<2) may be unreachable for a soft solver's residual. Re-baseline the threshold ONLY if the evidence shows the residual floor is a stable physical artifact of the soft solver (document the measured floor + the new threshold; never set it so loose that a visibly-moving body sleeps — gate with a "a thrown body does NOT sleep mid-flight" test).
  - **If neither:** the pile genuinely never quiesces (a solver-stability issue) — STOP and escalate to the user with the measurement; do not hack the sleep test to pass.
- [ ] **Step 4: apply the minimal fix + re-baseline behaviorally.** Implement the chosen fix. Re-run the existing sleep tests (`PhysicsIslandTest` cases 1,4; `PhysicsStaticSettleTest` 199/240; the Phase-A persistent-island cases) — they MUST stay green (or be deliberately re-baselined within the invariant if the threshold moved, with the change + rationale recorded). Add a guard test that a body in motion does NOT sleep (the threshold did not get too loose):
  ```cpp
  TEST_CASE("PhysicsAwakeSet: a body in clear motion never sleeps", "[physics][awakeset]")
  {
      WorldDef wd; wd.gravityY = Real(0); PhysicsWorld w(wd);
      const BodyHandle b = AddBox(w, Vec2(Real(0), Real(0)), Real(5), Real(5));
      w.SetVelocity(b, Vec2(Real(50), Real(0)));   // |v|=50, far above threshold
      for (int k = 0; k < 120; ++k) { w.Step(kStep); REQUIRE(w.IsAwake(b)); }
  }
  ```
- [ ] **Step 5: build + run.** The dense-pile sleep test PASSES; the motion-never-sleeps guard PASSES; full `[island]` + `[physics]` GREEN. The `[STEPPROF]` measuring build shows the awake-set drains to ~empty for the settled pile (the win).
- [ ] **Step 6: ArcaneCore clean (Debug + Release), commit.** `fix(arcane/physics): dense connected piles quiesce + sleep (the awake-set drains) [+ the measured rationale]`. Record the measurement + the chosen minimal fix + any threshold re-baseline in the commit body. Trailer.

---

### Task 6: Broadphase coherence — gate zero-velocity kinematic proxy refresh (the small remaining waste)

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; extend `PhysicsAwakeSetTest.cpp`.

Sleeping dynamics already skip proxy refresh (the awake gate in `FinalizePositionsSoA`). The remaining per-step waste the research found: every alive KINEMATIC body calls `UpdateMoverProxies` (incl. `m_residencyGrid.Move`) UNCONDITIONALLY in Stage 1, even with zero velocity. Add a moved-this-step gate so a stationary kinematic skips the proxy churn. (Sleeping-proxy exclusion from `UpdatePairs` is a `DynamicTree` change — assess via `[STEPPROF]`; only do it if measurement shows the pair-set membership of sleeping proxies is a material cost. Default: defer it — the `BothAsleep` narrowphase gate already neutralizes most of it.)

- [ ] **Step 1: failing test.** Append a test that a stationary kinematic does NOT churn its proxy every step (observable via a contact-stability proxy: a stationary kinematic resting under a settled dynamic does not keep the dynamic awake). Simpler + robust: assert that a zero-velocity kinematic placed away from everything produces no pairs and the scene fully sleeps (a moving-proxy-every-step would not break sleep, so instead assert the cheaper observable — the kinematic's residency entry is unchanged across steps if we expose a probe; if no probe exists, gate this task on `[STEPPROF]` showing the Stage-1 cost drop and on `[physics]` staying green). Pragmatic test:
  ```cpp
  TEST_CASE("PhysicsAwakeSet: a stationary kinematic does not prevent the scene from sleeping", "[physics][awakeset]")
  {
      WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      BodyDef kd; kd.type = BodyType::Kinematic; kd.position = Vec2(Real(150), Real(-50)); kd.shape = MakeAabb(Real(5), Real(5));
      const BodyHandle k = w.AddBody(kd);          // zero velocity, off to the side
      const BodyHandle b = AddBox(w, Vec2(Real(0), Real(-20)), Real(5), Real(5));
      for (int n = 0; n < 700; ++n) { w.Step(kStep); }
      REQUIRE_FALSE(w.IsAwake(b));                  // dynamic still sleeps
      (void)k;
  }
  ```
- [ ] **Step 2: build + run** — passes today (kinematic far away). It is a regression guard for the gate.
- [ ] **Step 3: add the moved-gate to the Stage-1 kinematic integrate.** In Stage 1, only call `UpdateMoverProxies(i)` for a kinematic when it actually moved this step: `if (m_velX[i] != Real(0) || m_velY[i] != Real(0)) { m_posX[i]+=...; m_posY[i]+=...; UpdateMoverProxies(i); }` (a zero-velocity kinematic neither integrates nor refreshes proxies). NOTE: also handle `SetVelocity`/`SetAngle`/`SetPosition`/`MovePosition` on a kinematic — those already call `UpdateMoverProxies` at the mutation site (verified: `SetPosition`/`SetAngle` refresh proxies), so a kinematic that is repositioned out-of-step still updates. The Stage-1 gate only skips the per-step re-integrate of a NON-moving kinematic.
- [ ] **Step 4: build + run.** Kill strays, build Debug. The guard test + full `[island]` + `[physics]` GREEN. A moving kinematic still updates its proxy every step (verify `PhysicsIslandTest` case 3, which drives a kinematic pusher: `SetVelocity` on the pusher gives it nonzero velocity → the Stage-1 gate passes it through). Confirm case 3 passes.
- [ ] **Step 5: (OPTIONAL, measurement-gated) sleeping-proxy move-set exclusion.** If `[STEPPROF]` shows narrowphase/`UpdatePairs` still dominated by sleeping-proxy pair membership on the dense settled scene, add a `DynamicTree` path to keep sleeping proxies in the tree but exclude them from `m_moved` (they don't move) — they already are excluded (a sleeping dynamic never calls `Update()`), so the only residue is the pair-set keeping their static pairs. Assess + decide; default DEFER (out of Phase B unless measurement demands it). Record the `[STEPPROF]` decision in the commit body.
- [ ] **Step 6: ArcaneCore clean (Debug + Release), commit.** `perf(arcane/physics): skip proxy refresh for zero-velocity kinematics (Stage-1 move-gate)`. Trailer.

---

### Task 7: Full gate + `[STEPPROF]` measure + determinism + memory

**Files:** extend `PhysicsAwakeSetTest.cpp`; (the gate runs the whole suite).

- [ ] **Step 1: determinism run-twice with the awake-set ordering.** Append a run-twice determinism test over a scene that exercises create + sleep + wake + remove + recycle (so the awake-set goes through append + swap-remove cycles), asserting bit-identical final positions, velocities, awake states, AND island roots across two runs (the awake-set's non-ascending order must not perturb determinism):
  ```cpp
  TEST_CASE("PhysicsAwakeSet: create/sleep/wake/remove is deterministic across two runs", "[physics][awakeset]")
  {
      auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake) {
          WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
          AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
          std::vector<BodyHandle> boxes;
          for (int i = 0; i < 6; ++i) { boxes.push_back(AddBox(w, Vec2(Real(0), Real(-10) - Real(9)*static_cast<Real>(i)), Real(4), Real(4))); }
          for (int k = 0; k < 200; ++k) { w.Step(kStep); }
          w.RemoveBody(boxes[2]);                                  // swap-remove from the awake-set mid-life
          const BodyHandle nb = AddBox(w, Vec2(Real(30), Real(-10)), Real(4), Real(4)); // recycle a slot
          w.ApplyImpulse(boxes[5], Vec2(Real(120), Real(-3000)));  // wake fan-out
          for (int k = 0; k < 500; ++k) { w.Step(kStep); }
          pos.clear(); awake.clear();
          for (std::size_t i = 0; i < boxes.size(); ++i) { if (i==2) continue; pos.push_back(w.Position(boxes[i])); awake.push_back(w.IsAwake(boxes[i])?1:0); }
          pos.push_back(w.Position(nb)); awake.push_back(w.IsAwake(nb)?1:0);
      };
      std::vector<Vec2> p1, p2; std::vector<int> a1, a2;
      run(p1, a1); run(p2, a2);
      REQUIRE(p1.size() == p2.size());
      for (std::size_t i = 0; i < p1.size(); ++i) { REQUIRE(p1[i].x == p2[i].x); REQUIRE(p1[i].y == p2[i].y); REQUIRE(a1[i] == a2[i]); }
  }
  ```
- [ ] **Step 2: full Debug gate.** Kill strays, build Debug, run `[physics]` then the WHOLE suite (incl. `[gpu]`):
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" ; ./ArcaneTests.exe "[physics]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" ; ./ArcaneTests.exe
  ```
  Expected: ALL green (the 6 PhysicsIslandTest cases + the PhysicsPersistentIslandTest cases + the new PhysicsAwakeSet cases + PhysicsStaticSettleTest + the whole suite).
- [ ] **Step 3: full Release gate.** Build Release, run `[physics]` + whole suite from the Release exe dir. Expected: green (determinism under NDEBUG + the optimizer).
- [ ] **Step 4: ArcaneCore static-CRT gate** (Debug + Release): clean.
- [ ] **Step 5: `[STEPPROF]` win measurement.** Build a measuring build (`ARCANE_STEPPROF=1`), run the dense-pile sleep scene + (if wired) the Sandbox 10k `--perf` stress scene, and DUMP the per-phase breakdown for (a) the settled steady state and (b) a churning state. Expected at steady state: the awake-set drains → narrowphase + integrate + solve + Stage-1 costs drop ~proportional to the resting fraction (the sleep win). Record the before/after `[STEPPROF]` numbers in the commit body. (Phase B's target is the SETTLE half; the churn half is Phases C+D.)
- [ ] **Step 6: commit + memory.** Commit: `feat(arcane/physics): Phase B awake-set + sleep-by-migration -- settled islands cost ~zero in the Step`. Body: the full Debug+Release+ArcaneCore green gate, the run-twice determinism, and the `[STEPPROF]` settle-scene win. Trailer. Then update the `project_arcane_physics_rearchitecture` memory (Phase B done + the measured result; next = Phase C — constraint-graph-owned compacted `ContactSim` + gather-free SIMD, re-homing Part 1's solver). **Do NOT push.**

---

## Self-Review Notes

**Spec coverage (the chosen "lighter awake-set Phase B" scope):**
- Explicit awake-set + incremental maintenance at create/sleep/wake/remove → Task 2. O(1) membership + swap-remove via `m_awakeIndex`. Determinism-safe (run-twice test in Task 7).
- Iterate-awake-only in the hot loops: solver loops (SyncIn/SyncOut/Integrate×2/Finalize) → Task 3; Stage-1 snapshot + UpdateContacts dynamic sub-loop → Task 4. `BodyStateSoA` + `ContactConstraint` stay world-slot-indexed (Phase C compacts) — explicitly preserved.
- Sleep-by-migration: sleeping an island removes its members from the awake-set (Task 2 seam); waking re-adds them. The migration IS the awake-set membership change (no per-set storage move — that is the Phase-C/full-split scope the user deferred).
- Quiescence (make piles actually sleep so the migration fires) → Task 5 (measurement-driven; the crux). Without it the awake-set never drains on a connected pile and the win does not materialize.
- Broadphase coherence (the small remaining win) → Task 6 (zero-velocity kinematic gate; sleeping-proxy exclusion deferred unless `[STEPPROF]` demands it). The big broadphase waste (sleeping dynamics) is ALREADY handled (the awake gate in FinalizePositionsSoA).
- `[STEPPROF]` gated build switch (the per-phase measurement gate; spec TODO) → Task 1 (prerequisite).

**Determinism (the hard contract):** the awake-set iteration order is append/swap-remove (NOT ascending-slot). This is safe because every rerouted loop does independent per-body work (no cross-body accumulation; the SIMD contact solve is unchanged and still iterates contacts/colors in canonical order). Pinned by the run-twice tests in Tasks 3 + 7. The sleep numerics may be deliberately re-baselined in Task 5 within the sleep invariant (never weakening it — guarded by the "body in motion never sleeps" test) per `feedback_engine_evolves_not_frozen`.

**Soft spots:**
1. **SyncIn + sleeping B-endpoints (Task 3).** The reroute assumes no emitted constraint references a sleeping slot. This holds because `EmitContactConstraints` gates on awake-A and a touching dynamic-dynamic pair shares one island (so a sleeping B with an awake A cannot exist). Task 3 Step 5 adds a Debug assertion proving it. If the assertion ever trips, extend SyncIn to also sync any constraint-referenced slot.
2. **Quiescence (Task 5) is investigation-gated.** The fix is chosen by measurement, not guessed. The decision tree forbids hacking the sleep test to pass and escalates if the pile genuinely never quiesces. This is the highest-risk task and the actual source of the perf win.
3. **Kinematic handling.** Kinematics are NOT in the awake-set; SyncIn keeps a small `for(i=0..count)` kinematic copy and Stage 1 keeps a kinematic pass (with a moved-gate in Task 6). A dedicated `m_kinematicBodies` list is YAGNI-deferred (kinematics are rare vs the dynamic pile).
4. **Recycled-slot stale `m_awakeIndex` (Task 2).** A recycled slot may carry a stale index — Task 2 Step 5 resets `m_awakeIndex[idx]=kNotAwake` before `AddToAwakeSet` so the idempotency guard is correct.

**Gates:** determinism (run-twice in Tasks 3+7), behavioral sleep contract (all existing island + static-settle cases stay green; the new awake-set cases pin create/sleep/wake/remove + quiescence + the motion-never-sleeps guard), Debug + Release + ArcaneCore all green, and the `[STEPPROF]` settle-scene win is the perf gate (the 10k churn half is Phases C+D, not B).
