# Arcane 2D Physics — Phase A: Persistent Incremental Islands — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Replace the per-step **global union-find** island/sleep pass (`Island::UpdateSleep`, an O(n) UF rebuild per Step + an O(n^2) whole-island sleep scan) with **PERSISTENT incremental islands** modeled on Box2D v3. A per-body `m_islandId` column plus a `PhysicsWorld`-owned island registry is maintained incrementally: a new dynamic body is its own 1-body island; a dynamic-dynamic touch-BEGIN merges islands (weighted union, canonical order); a touch-END / destroy marks the owning island a split candidate; a deferred, quota-limited split rebuilds at most `kMaxSplitsPerStep` candidate islands per Step. Sleep becomes O(island) (iterate each island's known members, no global scan). **The sleep contract (thresholds, whole-island-unit, exact freeze, joint reset, wake-the-whole-island) is preserved byte-for-behavior.** This is a STRUCTURAL change and the FOUNDATION for Phase B (sleep-by-set-migration, the 10k perf win); Phase A is NOT the perf win. **Determinism (run-twice-identical) is the hard contract.**

**Architecture:** Today `PhysicsWorld::Step` stage 5 calls `Island::UpdateSleep(*this, contacts, contactCount, joints, jointCount, dt)` (PhysicsWorld.cpp:1564-1570). `UpdateSleep` (Island.cpp:42-191) rebuilds a union-find over `m_uf` (`UnionFindScratch()`) from THIS step's `ContactConstraint` array, resets jointed-dynamic timers, accumulates per-body idle timers, then for every awake-past-threshold dynamic does an O(n) inner scan of ALL bodies sharing its UF root to decide whole-island sleep — O(n^2) for one big island. `IslandRootOf(i)` (PhysicsWorld.cpp:2691) walks `m_uf` to the root for color-by-island debug draw + a test. Phase A introduces a persistent `Island` registry owned by `PhysicsWorld` and a per-body `m_islandId` SoA column, maintained at the body/contact lifecycle seams, so islands SURVIVE across steps. `IslandRootOf` returns `m_islandId[i]`. The new `Island::UpdateSleep` iterates the persistent islands (members already known) for an O(island) sleep decision. `m_uf` / `UnionFindScratch()` / the old per-step UF are deleted.

**Tech Stack:** C++23, Core (presentation-free, /MD for Arcane.dll + static-CRT for ArcaneCore server flavor), glm + std + sibling Physics headers only (no SDL3/NVRHI/Batcher2D/ImGui), Catch2 (`[physics]` / `[physics][island]`), premake5 / MSBuild via `Arcane.slnx`. Branch `feature/arcane-persistent-islands` (already checked out, off `main`).

---

## Conventions

- **Build (Debug):**
  ```
  "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
  ```
  Append `-t:ArcaneTests` to build the test exe only (it pulls in `Core`). Append `-p:Configuration=Release` (replace Debug) for the Release gate.
- **Run tests (from the exe dir — required, the exe resolves data relative to cwd):**
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```
  Use `"[island]"` for the island subset, or a quoted full case name (e.g. `"PhysicsIsland: resting body sleeps then stays frozen"`) for one case. Release exe dir is `bin/Release-windows-x86_64-md/ArcaneTests`.
- **ArcaneCore static-CRT (server flavor):** build the `ArcaneCore` project (Server workspace) Debug + Release — it compiles the SAME `Arcane/Core/src` sources with the static CRT and must stay clean. The project file is `Server/ArcaneCore/ArcaneCore.vcxproj` (build it via the Server solution, or `MSBuild.exe "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` and again with `-p:Configuration=Release`).
- **New files → regen BOTH workspaces by ABSOLUTE path** (both Core and Tests globs are `%{prj.location}/src/**.cpp` / `**.hpp`, so a NEW `.cpp` requires a project regen). Run from `Arcane/` AND from `Server/` (NOT `GenerateProjects.bat` — it hangs on a `pause`; NOT a relative path):
  ```
  cd "D:/dev/starworks/Gacha/Arcane" && & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  cd "D:/dev/starworks/Gacha/Server" && & "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026
  ```
  (Phase A adds Island.hpp/.cpp content in place and one new test file. Island.hpp/.cpp ALREADY EXIST, so no regen is needed for them; the new test file `PhysicsPersistentIslandTest.cpp` IS new → regen.)
- **Kill stray procs before EVERY build** (a running `Loom.exe` locks the plugin copy; a stuck `ArcaneTests.exe` locks the exe):
  ```
  Get-Process Loom,ArcaneTests -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  ```
- **clangd / IDE diagnostics are FALSE POSITIVES — MSVC is truth.** Build flags are `/fp:strict /arch:AVX2`, no fast-math (determinism rule; `floatingpoint "Strict"` in premake5.lua:93).
- **DETERMINISM IS THE CONTRACT** — run-twice-identical. There are NO exact goldens for island/sleep; the `[physics]` + `[physics][island]` behavioral suites + the run-twice case (PhysicsIslandTest case 6) are the gate.
- House style: ASCII comments, explain WHY (not what). Commit per task with the trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. **Do NOT push.**

## File Structure

| File | Created/Modified | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/Island.hpp` | Modified | Replace the free-function-namespace contract with: the `Island` registry record struct, `kInvalidIsland`, the sleep thresholds (unchanged), and the new `Island::UpdateSleep(PhysicsWorld&, dt)` signature (the contact/joint array params are dropped — sleep now walks the persistent registry + the live pool). |
| `Arcane/Core/src/Arcane/Physics/Island.cpp` | Modified | New `Island::UpdateSleep`: per-body idle-timer accumulation (joint reset preserved) + O(island) whole-island sleep over the registry. The old global-UF build + O(n^2) scan + `UfFind` helper are deleted. |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | Modified | Add `kInvalidIsland`, the `Island` record vector (`m_islands`) + free-list (`m_islandFree`), the per-body `m_islandId` column, and the island-management API (`AllocIsland`/`FreeIsland`/`IslandOf`/`MergeIslands`/`SplitIsland`/`WakeIsland`/`MarkSplitCandidate`). Drop `UnionFindScratch()` + `m_uf`. `IslandRootOf` decl unchanged (semantics change in .cpp). Add `m_pendingMerges` + `m_splitCandidates` step scratch + `kMaxSplitsPerStep`. |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | Modified | Size `m_islandId` in `EnsureCapacity`; allocate a 1-body island on dynamic `AddBody`; merge on touch-BEGIN at the `:2244` seam (canonical order); mark split-candidate + wake on touch-END / `Destroy` / `DestroyContactsForBody`/`ForFixture`; process the deferred quota split + per-island sleep in `Step` stage 5; wake the whole island in `SetVelocity`/`ApplyImpulse`/`Wake`/`WakeMoverPair`; rewrite `IslandRootOf` to return `m_islandId[i]`; delete `m_uf` usage. |
| `Arcane/Tests/src/PhysicsPersistentIslandTest.cpp` | **Created** | New `[physics][island]` behavioral tests for the persistent registry: fresh body is its own island, static has none; touching chain merges to one island; separation splits (deferred); body losing all contacts becomes its own island; quota bounds splits but resolves; wake-island fan-out. (The existing `PhysicsIslandTest.cpp` 6 cases stay UNCHANGED and must stay green.) |

---

## Verified seams + facts (quote these — confirmed in the codebase)

- **`Island.hpp` today** is a free-function `namespace Island`: constants `kSleepLinVel2 = Real(4)` (line 59), `kSleepAngVel = Real(0.05)` (line 62), `kSleepTime = Real(0.5)` (line 66); the one function `void UpdateSleep(PhysicsWorld&, const ContactConstraint*, std::uint32_t contactCount, const JointConstraint*, std::uint32_t jointCount, Real dt)` (lines 85-90).
- **`Island.cpp` today:** `UfFind` path-halving helper (lines 31-39, anon namespace); `UpdateSleep` (lines 42-191): rebuild UF over `world.UnionFindScratch()` (lines 58-93), joint-reset pass (lines 102-121), per-body timer accumulation (lines 126-145), O(n^2) whole-island veto + apply-sleep (lines 162-190). The sleep apply is exactly: `world.SetAwakeSlot(i, false); world.SetVelSlot(i, Vec2(Real(0), Real(0))); world.SetAngVelSlot(i, Real(0));` (lines 186-188).
- **Body SoA** (`PhysicsWorld.hpp`): `m_sleepTimer` (`std::vector<Real>`, line 1090), `m_awake` (`std::vector<std::uint8_t>`, line 1091). Sized in `EnsureCapacity` at PhysicsWorld.cpp:187-188 (`m_sleepTimer.resize(next, Real(0)); m_awake.resize(next, std::uint8_t(1));`). Initialized in `AddBody` at PhysicsWorld.cpp:864-865 (`m_sleepTimer[idx] = Real(0); m_awake[idx] = 1;`). NO island column today.
- **Per-step UF scratch:** `std::vector<std::uint32_t> m_uf;` (PhysicsWorld.hpp:1389), accessor `std::vector<std::uint32_t>& UnionFindScratch() noexcept { return m_uf; }` (PhysicsWorld.hpp:795-798). `IslandRootOf(std::uint32_t i) const noexcept` (PhysicsWorld.cpp:2691-2711) walks `m_uf` to the root.
- **Accessors (all on `PhysicsWorld`, verified):** `Count()` (hpp:575), `Alive(i)` (hpp:576), `SensorSlot(i)` (hpp:577), `TypeSlot(i)` -> `BodyType` (hpp:579), `AwakeSlot(i)` (hpp:733), `VelSlot(i)` -> `Vec2` (hpp:739), `AngVelSlot(i)` -> `Real` (hpp:743), `SetVelSlot(i, Vec2)` (hpp:744), `SetAngVelSlot(i, Real)` (hpp:749), `SleepTimerSlot(i)` (hpp:783), `SetSleepTimerSlot(i, Real)` (hpp:784), `SetAwakeSlot(i, bool)` (hpp:785). `BodyType::Dynamic` from `PhysicsTypes.hpp`; `kInvalidSlot` from `PhysicsTypes.hpp`.
- **Contact (`Contact.hpp:45-79`):** `bodyA`/`bodyB` (slots, `kInvalidSlot` default), `bIsBody` (false => tile span), `manifold`, `touching`. NO island field (Phase A does NOT add one — splits walk member bodies' pool contacts). `ContactPool::ForEach` visits live contacts **ascending id (deterministic)** (Contact.hpp:126-129); `Get(id)`, `Count()`.
- **The touch-begin/end seam** is `c.touching = (c.manifold.pointCount > 0);` at **PhysicsWorld.cpp:2244**, inside the `m_contactPool.ForEach` UPDATE pass that recomputes each pair's manifold once per step. The OLD `c.touching` value can be read just BEFORE this line (there is also `oldManifold` snapshotted at line 2229 for warm-start). The contact's body slots are `c.bodyA` / `c.bodyB`; `c.bIsBody` is true for a real body-body pair (false for a tile span). The UPDATE pass does NOT process tile spans (those live in transient `m_spanContacts`), so every contact here with `c.bIsBody` is a body-body pair.
- **Destroy seams in the UPDATE pass:** stale-handle reap `m_contactPool.Destroy(id)` at **PhysicsWorld.cpp:2158**; fat-box separation `m_contactPool.Destroy(id)` at **PhysicsWorld.cpp:2185**.
- **Immediate lifecycle destruction:** `DestroyContactsForFixture(fixtureSlot)` (PhysicsWorld.cpp:1648-1658) and `DestroyContactsForBody(bodySlot)` (PhysicsWorld.cpp:1660-1670), both `m_contactPool.ForEach` + `Destroy`. `DestroyContactsForBody(idx)` is called from `RemoveBody` at **PhysicsWorld.cpp:1045**.
- **`UpdateSleep` call site:** `Step` stage 5, PhysicsWorld.cpp:1564-1570 (after `BulletSweep()` at :1555, before the stage-6 events block at :1572).
- **The event-pair canonical sort to mirror:** `std::sort(m_touchedEventPairs.begin(), m_touchedEventPairs.end());` (PhysicsWorld.cpp:1617) over `BroadphasePair{ a, b }` (a = min slot, b = max slot; `BroadphasePair` from `Broadphase/Broadphase.hpp:72`, default `operator<` lexicographic on `(a,b)`).
- **Wake paths (wake ONE body today):** `SetVelocity` sets `m_awake[i]=1; m_sleepTimer[i]=Real(0);` for a dynamic (PhysicsWorld.cpp:1208-1209); `ApplyImpulse` (both overloads) at :1226-1227 and :1252-1253; `Wake` at :1273-1274; `WakeMoverPair(fa, fb)` wakes `a`/`b` at :1787-1788 / :1792-1793.
- **`IslandRootOf` consumers:** `PhysicsDebugDraw.cpp:282` (`const std::uint32_t root = world.IslandRootOf(i); col = kIslandPalette[root % 8u];`) + `PhysicsIslandTest.cpp` case 5 only. External contract: equal for co-island members, distinct across islands.
- **Existing gate tests:** `PhysicsIslandTest.cpp` (`[physics][island]`, 6 cases): (1) resting body sleeps + frozen, (2) impulse wakes, (3) new contact wakes, (4) stack sleeps as a unit + disturbance wakes a neighbor, (5) `IslandRootOf` same-root topology (4-box stack while awake + 1 far body different root), (6) settle+sleep deterministic run-twice. `PhysicsStaticSettleTest.cpp` pins behavioral sleep at lines 199 (`CHECK_FALSE(w.IsAwake(cap))`) and 240. ALL must stay green.

### Name glossary (use these EXACTLY across all tasks)

- `kInvalidIsland` — `static constexpr std::uint32_t kInvalidIsland = 0xFFFFFFFFu;` (a body with no island; statics/kinematics).
- `kMaxSplitsPerStep` — `static constexpr std::uint32_t kMaxSplitsPerStep = 1u;` (Box2D's quota).
- `Island` — the registry record: `struct Island { std::vector<std::uint32_t> bodies; bool splitCandidate = false; };`.
- `m_islands` — `std::vector<Island>` (id-indexed pool; a freed id has an empty `bodies` and is on `m_islandFree`).
- `m_islandFree` — `std::vector<std::uint32_t>` (recycled island-id free list).
- `m_islandId` — `std::vector<std::uint32_t>` per-body SoA column (island id or `kInvalidIsland`).
- `m_pendingMerges` — `std::vector<BroadphasePair>` step scratch: dynamic-dynamic touch-BEGIN body-pairs collected this Step, sorted canonically before applying.
- `m_splitCandidates` — `std::vector<std::uint32_t>` step scratch: island ids marked `splitCandidate` (an ascending-sorted, dedup'd worklist for the quota processor).
- `AllocIsland()` -> `std::uint32_t` — mint/reuse an island id (empty `bodies`, `splitCandidate=false`).
- `FreeIsland(id)` — return an island id to the free list (clears `bodies`, `splitCandidate`).
- `IslandOf(slot)` -> `std::uint32_t` — `m_islandId[slot]` (or `kInvalidIsland`).
- `MergeIslands(idA, idB)` -> `std::uint32_t` — weighted union: relabel the smaller island's members into the larger, free the smaller, return the survivor id.
- `MarkSplitCandidate(islandId)` — set `m_islands[islandId].splitCandidate = true` (no-op for `kInvalidIsland`).
- `SplitIsland(islandId)` — rebuild one candidate island via a fresh local union-find over its member bodies' current touching pool contacts; reassign `m_islandId` for any new component; clear `splitCandidate`.
- `WakeIsland(slot)` — wake every member of `IslandOf(slot)` (set `m_awake=1; m_sleepTimer=0`); no-op if `kInvalidIsland`.

---

### Task 1: Island registry struct + `m_islandId` column + 1-body-island-on-AddBody + `IslandRootOf` reads it

**Files:** `Arcane/Core/src/Arcane/Physics/Island.hpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; new `Arcane/Tests/src/PhysicsPersistentIslandTest.cpp`.

This task lays the data structures and the create-time lifecycle without yet touching merge/split/sleep. After this task the registry exists, every dynamic body is its own island, statics/kinematics have `kInvalidIsland`, and `IslandRootOf` reads `m_islandId`. The old `Island::UpdateSleep` (still global-UF) is UNTOUCHED this task — but because `IslandRootOf` now reads `m_islandId` instead of `m_uf`, the existing `PhysicsIslandTest.cpp` **case 5** semantics change: with no merge yet, the 4 stacked boxes are 4 separate 1-body islands → `root0 != root3`. **That is expected to FAIL after this task and is FIXED by Task 2 (merge).** Do NOT weaken case 5. To keep the suite green BETWEEN tasks, this task's new tests cover the create-time facts; case 5 is allowed to be red until Task 2 (note it in the commit body). Run the rest of `[physics]` to confirm nothing else regresses.

- [ ] **Step 1: the `Island` registry struct + constants in `Island.hpp`.** Replace the `namespace Island { ... }` body's constants block and remove the old `UpdateSleep` free-function declaration's contract-comment-only changes are in Task 4 — but ADD the registry record + constants now. Edit `Island.hpp`: keep the file's top header comment, keep `#include <cstdint>` + `#include <Arcane/Physics/PhysicsTypes.hpp>`, keep the forward decls of `PhysicsWorld`/`ContactConstraint`/`JointConstraint`. Inside `namespace Island`, ADD ABOVE the thresholds:
  ```cpp
  // ----------------------------------------------------------------
  // Persistent island registry (Phase A -- replaces the per-step UF).
  // ----------------------------------------------------------------
  //
  // An Island is a connected component of DYNAMIC bodies joined by
  // touching dynamic-dynamic contacts. It SURVIVES across steps and is
  // maintained INCREMENTALLY at the body/contact lifecycle seams (create
  // = a 1-body island; touch-begin = merge; touch-end/destroy = mark a
  // split candidate; a deferred quota-limited pass rebuilds candidates).
  // Static/Kinematic bodies are NOT members (they anchor, they are not
  // island nodes -- the same dynamic-dynamic-only rule the old UF used).
  //
  // The registry lives on PhysicsWorld (m_islands, id-indexed, free-list
  // recycled). Sleep bookkeeping is DERIVED from members (per-body idle
  // timers stay per-body in m_sleepTimer); an island holds no timer.
  struct Island
  {
      // Member body SLOTS (dynamic only). Order is append-order; the
      // sleep + wake passes iterate it directly (no global scan).
      std::vector<std::uint32_t> bodies;

      // Set when a member's contact separated or a member was destroyed:
      // the deferred split pass (quota-limited, once per Step) re-derives
      // this island's connected components and clears the flag.
      bool splitCandidate = false;
  };

  // A body with no island (Static/Kinematic, or a transiently un-assigned
  // dynamic slot). m_islandId[slot] == kInvalidIsland.
  inline constexpr std::uint32_t kInvalidIsland = 0xFFFFFFFFu;

  // At most this many split-candidate islands are rebuilt per Step (Box2D
  // v3's value). Splits are deferred + amortized; the registry converges
  // over a few steps. Processed in ascending island-id order (determinism).
  inline constexpr std::uint32_t kMaxSplitsPerStep = 1u;
  ```
  Add `#include <vector>` to the `Island.hpp` includes (the struct needs it). Leave the three threshold constants (`kSleepLinVel2`/`kSleepAngVel`/`kSleepTime`) and the existing `UpdateSleep` declaration UNCHANGED for now (Task 4 rewrites the signature).

- [ ] **Step 2: registry storage + API decls in `PhysicsWorld.hpp`.** Add the include `#include <Arcane/Physics/Island.hpp>` is ALREADY transitively available via Island.cpp; PhysicsWorld.hpp must include it for the `Island` type — add `#include <Arcane/Physics/Island.hpp>` near the other Physics includes at the top of PhysicsWorld.hpp (check it is not already present; if a forward decl suffices it is NOT, because `std::vector<Island::Island>`... — note the type is `Island::Island`, the struct inside the namespace). In the **private members** section, next to `m_sleepTimer`/`m_awake` (after line 1092 `m_bullet`), add:
  ```cpp
  // ---- persistent island registry (Phase A) -----------------------
  //
  // m_islandId[slot] is the body's persistent island id (Island::
  // kInvalidIsland for Static/Kinematic or an un-assigned dynamic slot).
  // m_islands is the id-indexed record pool; freed ids are recycled via
  // m_islandFree. Maintained incrementally at the lifecycle seams; the
  // old per-step union-find (m_uf) is gone.
  std::vector<std::uint32_t>      m_islandId;   // per-body island id
  std::vector<Island::Island>     m_islands;    // id-indexed record pool
  std::vector<std::uint32_t>      m_islandFree; // recycled island ids

  // Step scratch (zero steady-state alloc -- clear() keeps capacity).
  // m_pendingMerges: dynamic-dynamic touch-BEGIN body-pairs collected in
  // the UpdateContacts pass, sorted canonically then applied (determinism).
  // m_splitCandidates: island ids to rebuild this Step (quota-limited).
  std::vector<BroadphasePair>     m_pendingMerges;
  std::vector<std::uint32_t>      m_splitCandidates;
  ```
  **This task is ADD-ONLY to the header — do NOT delete `m_uf` / `UnionFindScratch()` yet.** The still-global `Island::UpdateSleep` keeps calling `UnionFindScratch()` until Task 4 rewrites it, so deleting them now breaks `Island.cpp`'s compile. They become dead-weight after Task 4 and are deleted in Task 6. ADD the island-management API declarations (private methods, near the other private helpers — co-locate with the contact-lifecycle helpers):
  ```cpp
  // ---- island registry management (Phase A) -----------------------
  // Mint or reuse an island id (empty members, not a split candidate).
  std::uint32_t AllocIsland();
  // Return an island id to the free list (clears members + flag).
  void          FreeIsland(std::uint32_t id) noexcept;
  // m_islandId[slot] (or kInvalidIsland). Inline -> zero call cost.
  [[nodiscard]] std::uint32_t IslandOf(std::uint32_t slot) const noexcept
  {
      return slot < m_islandId.size() ? m_islandId[slot] : Island::kInvalidIsland;
  }
  // Weighted union: relabel the smaller island's members into the larger,
  // free the smaller, return the survivor id. Pass two DISTINCT live ids.
  std::uint32_t MergeIslands(std::uint32_t idA, std::uint32_t idB);
  // Flag an island for the deferred split pass (no-op for kInvalidIsland).
  void          MarkSplitCandidate(std::uint32_t islandId) noexcept;
  // Rebuild one candidate island into 1+ connected components (fresh local
  // UF over its members' current touching pool contacts); clears the flag.
  void          SplitIsland(std::uint32_t islandId);
  // Wake every member of the body's island (set awake, reset timer).
  void          WakeIsland(std::uint32_t slot) noexcept;
  ```

- [ ] **Step 3: write the failing test FIRST.** Create `Arcane/Tests/src/PhysicsPersistentIslandTest.cpp`. Mirror the `PhysicsIslandTest.cpp` helpers (AddFloor/AddCircle/AddBox, `kStep`). Add ONE `[physics][island]` test for this task's facts (a `IslandRootOf`-distinctness probe — two SEPARATED dynamic bodies have DIFFERENT roots, and a static body's root is its own slot / never shares a dynamic's root). Use the public API only (`IslandRootOf(slot)` is public; `BodyHandle.index` is the slot):
  ```cpp
  // Physics Phase A: persistent incremental islands -- BEHAVIORAL tests.
  // Companion to PhysicsIslandTest.cpp (the original 6-case sleep suite,
  // which stays UNCHANGED). These cover the persistent-registry facts:
  // create-time 1-body islands, merge on touch, deferred split, and the
  // wake-the-whole-island fan-out. Determinism is the contract.
  //
  // PRESENTATION-FREE + C++20-clean.

  #include <cmath>
  #include <cstdint>
  #include <vector>

  #include <catch2/catch_test_macros.hpp>

  #include <Arcane/Physics/PhysicsTypes.hpp>
  #include <Arcane/Physics/Shapes.hpp>
  #include <Arcane/Physics/Body.hpp>
  #include <Arcane/Physics/PhysicsWorld.hpp>

  using namespace Arcane::Physics;

  namespace
  {
      constexpr Real kStep = Real(1) / Real(60);

      BodyHandle AddFloor(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
      {
          BodyDef def;
          def.type     = BodyType::Static;
          def.position = pos;
          def.shape    = MakeAabb(hw, hh);
          def.friction = Real(0.6);
          return w.AddBody(def);
      }
      BodyHandle AddBox(PhysicsWorld& w, Vec2 pos, Real hw, Real hh)
      {
          BodyDef def;
          def.type          = BodyType::Dynamic;
          def.position      = pos;
          def.shape         = MakeAabb(hw, hh);
          def.density       = Real(1);
          def.friction      = Real(0.4);
          def.fixedRotation = true;
          return w.AddBody(def);
      }
  }

  // ---------------------------------------------------------------------------
  // Create-time: a fresh dynamic body is its own 1-body island; two separated
  // dynamics have DISTINCT island roots; a static body never shares a dynamic's
  // root (statics are not island members).
  // ---------------------------------------------------------------------------
  TEST_CASE("PhysicsPersistentIsland: fresh dynamic body is its own island; static has none",
            "[physics][island]")
  {
      WorldDef wd;
      wd.gravityY = Real(0); // no gravity -> bodies stay put, never touch
      PhysicsWorld w(wd);

      const BodyHandle floor = AddFloor(w, Vec2(Real(0), Real(500)), Real(50), Real(5));
      const BodyHandle b0    = AddBox(w, Vec2(Real(-200), Real(0)), Real(5), Real(5));
      const BodyHandle b1    = AddBox(w, Vec2(Real(200),  Real(0)), Real(5), Real(5));

      // Before any Step: each dynamic is its own island (assigned at AddBody).
      const std::uint32_t r0 = w.IslandRootOf(b0.index);
      const std::uint32_t r1 = w.IslandRootOf(b1.index);
      CHECK(r0 != r1); // two far-apart dynamics -> different islands

      // The static body is not a member; its root must not collide with a
      // dynamic's island id. IslandRootOf returns the body's own slot for a
      // non-member (the kInvalidIsland fallback below), distinct from r0/r1.
      const std::uint32_t rf = w.IslandRootOf(floor.index);
      CHECK(rf != r0);
      CHECK(rf != r1);

      // Stepping with no contacts keeps them distinct (no merge happens).
      for (int k = 0; k < 5; ++k) { w.Step(kStep); }
      CHECK(w.IslandRootOf(b0.index) != w.IslandRootOf(b1.index));
  }
  ```
  NOTE on the static `IslandRootOf`: `IslandRootOf` returns `m_islandId[i]`; for a static that is `kInvalidIsland`. To keep the contract "distinct across islands" meaningful for the debug-draw consumer (which only calls it for Dynamic bodies anyway), the rewrite (Step 5 below) returns the body's own slot index when `m_islandId[i] == kInvalidIsland`, so a static's root is its slot (distinct from any real island id, which is a small dense id never equal to a high slot in these tests — but to be SAFE the test asserts inequality against the two dynamic roots, which is robust because island ids are allocated densely from 0 and the floor's slot is 0 while b0/b1 get islands 0 and 1... — see Step 5: return slot for non-members, and island ids are a SEPARATE id space; to guarantee no collision, `IslandRootOf` returns `slot | 0x80000000u` for a non-member). **Implement the non-member return as `return slot | 0x80000000u;`** (high-bit-tagged slot) so a non-member root can NEVER equal a real island id (island ids are dense small `< 2^31`). Update the test comment to reflect that. This keeps case-5's "far body has a different root" working too (the far dynamic IS a member, so it returns its small island id, distinct from the stack's island id).

- [ ] **Step 4: regen + build + verify the NEW test FAILS** (the registry/API does not exist yet → compile error is the expected "fail" here; the test cannot link). Kill stray procs, regen BOTH workspaces (new test file), build ArcaneTests Debug. Expected: **compile/link error** (missing `AllocIsland` etc.) OR, once Step 5 lands the impl, the test passes. (Standard TDD: the test is written first; the implementation in Step 5 makes it pass.) Run after Step 5.

- [ ] **Step 5: implement the registry + create-time lifecycle + `IslandRootOf` rewrite in `PhysicsWorld.cpp`.**
  - In `EnsureCapacity` (after PhysicsWorld.cpp:189 `m_bullet.resize(...)`), size the new column: `m_islandId.resize(next, Island::kInvalidIsland);` (a never-touched tail slot has no island).
  - In `AddBody`, after the dynamics-state init block (after line 866 `m_bullet[idx] = ...`), assign the create-time island. A dynamic body becomes its own 1-body island; static/kinematic get `kInvalidIsland`:
    ```cpp
    // ---- persistent island assignment (Phase A) ---------------------
    // A new DYNAMIC body is its own 1-body island; static/kinematic are
    // not island members (they anchor). The slot may be recycled, so
    // overwrite unconditionally.
    if (def.type == BodyType::Dynamic)
    {
        const std::uint32_t isl = AllocIsland();
        m_islands[isl].bodies.push_back(idx);
        m_islandId[idx] = isl;
    }
    else
    {
        m_islandId[idx] = Island::kInvalidIsland;
    }
    ```
  - Implement the registry methods (place them with the other private helpers, e.g. just before `IslandRootOf` near PhysicsWorld.cpp:2683):
    ```cpp
    // ---- island registry management (Phase A) -----------------------

    std::uint32_t PhysicsWorld::AllocIsland()
    {
        if (!m_islandFree.empty())
        {
            const std::uint32_t id = m_islandFree.back();
            m_islandFree.pop_back();
            m_islands[id].bodies.clear();      // capacity kept (zero re-alloc)
            m_islands[id].splitCandidate = false;
            return id;
        }
        const std::uint32_t id = static_cast<std::uint32_t>(m_islands.size());
        m_islands.emplace_back();
        return id;
    }

    void PhysicsWorld::FreeIsland(std::uint32_t id) noexcept
    {
        // Defensive: never free an invalid/out-of-range id.
        if (id == Island::kInvalidIsland || id >= m_islands.size())
        {
            return;
        }
        m_islands[id].bodies.clear();
        m_islands[id].splitCandidate = false;
        m_islandFree.push_back(id);
    }

    std::uint32_t PhysicsWorld::MergeIslands(std::uint32_t idA, std::uint32_t idB)
    {
        // Weighted union: keep the LARGER island; relabel the smaller's
        // members + append, then free the smaller id. Stable membership ->
        // fewer relabels -> the island id of a big pile is sticky.
        if (idA == idB)
        {
            return idA;
        }
        std::uint32_t big   = idA;
        std::uint32_t small = idB;
        if (m_islands[big].bodies.size() < m_islands[small].bodies.size())
        {
            std::swap(big, small);
        }
        for (const std::uint32_t s : m_islands[small].bodies)
        {
            m_islandId[s] = big;
            m_islands[big].bodies.push_back(s);
        }
        // A merge unites two pools; if either was a split candidate the
        // merged island still needs a re-check on the next separation, but
        // a fresh merge is not itself a split -- clear is fine (a later
        // touch-END re-marks it).
        FreeIsland(small);
        return big;
    }

    void PhysicsWorld::MarkSplitCandidate(std::uint32_t islandId) noexcept
    {
        if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
        {
            return;
        }
        m_islands[islandId].splitCandidate = true;
    }

    void PhysicsWorld::WakeIsland(std::uint32_t slot) noexcept
    {
        const std::uint32_t isl = IslandOf(slot);
        if (isl == Island::kInvalidIsland)
        {
            return; // static/kinematic -> nothing to wake on itself
        }
        for (const std::uint32_t b : m_islands[isl].bodies)
        {
            m_awake[b]      = 1;
            m_sleepTimer[b] = Real(0);
        }
    }
    ```
    (Add `#include <utility>` for `std::swap` if not already included — it is needed for `MergeIslands`. `SplitIsland` is a stub for THIS task; full impl in Task 3. Define a minimal stub so the build links: `void PhysicsWorld::SplitIsland(std::uint32_t) {}` — replaced in Task 3.)
  - Rewrite `IslandRootOf` (PhysicsWorld.cpp:2691-2711) body to read `m_islandId`:
    ```cpp
    std::uint32_t PhysicsWorld::IslandRootOf(std::uint32_t i) const noexcept
    {
        // Phase A: the persistent island id IS the root (equal for all
        // co-island members, distinct across islands). A non-member
        // (static/kinematic, or an un-assigned slot) has no island; return
        // a high-bit-tagged slot so it can never collide with a real island
        // id (ids are dense + small, < 2^31). Consumed by PhysicsDebugDraw
        // (color-by-island, Dynamic only) + the island tests.
        if (i >= m_islandId.size() || m_islandId[i] == Island::kInvalidIsland)
        {
            return i | 0x80000000u;
        }
        return m_islandId[i];
    }
    ```
  - Leave `m_uf` / `UnionFindScratch()` and the old `Island::UpdateSleep` (which still calls `UnionFindScratch()`) UNTOUCHED this task — they keep the build green until Task 4 rewrites `UpdateSleep` and Task 6 deletes the dead UF. This task only ADDS the registry; it deletes nothing.

- [ ] **Step 6: build + run.** Kill strays, build ArcaneTests Debug (after Step 4's regen). Expected: the new `PhysicsPersistentIsland: fresh dynamic body is its own island; static has none` test **PASSES**. Run the full `[island]` subset: case 5 (`IslandRootOf returns the same root for all members of one island`) **FAILS** (no merge yet → the 4 boxes are 4 islands; `root0 != root3`). All OTHER `[island]` cases (1,2,3,4,6) and the broader `[physics]` suite **PASS** (sleep still runs via the OLD global-UF `UpdateSleep` — untouched this task). Record the case-5 red as expected-and-fixed-in-Task-2.
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[island]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```

- [ ] **Step 7: ArcaneCore static-CRT clean.** Build `Server/ArcaneCore/ArcaneCore.vcxproj` Debug + Release (the new code is in `Core/src`, so it compiles under the static CRT too). Expected: clean.

- [ ] **Step 8: commit.** `feat(arcane/physics): persistent island registry + m_islandId column (create-time 1-body islands; IslandRootOf reads it)`. Note in the body that `PhysicsIslandTest` case 5 is intentionally red until Task 2 (merge) lands. Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

### Task 2: Merge on touch-BEGIN (the :2244 seam, canonical-order weighted union)

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; extend `PhysicsPersistentIslandTest.cpp`.

Wire the merge. On a dynamic-dynamic contact's **false→true** `touching` transition, collect the body-pair into `m_pendingMerges`; after the UPDATE pass, sort `m_pendingMerges` canonically (mirroring the `m_touchedEventPairs` sort at :1617) and apply the weighted-union merges in that order. This fixes case 5 (the stacked boxes become one island).

- [ ] **Step 1: failing test.** Append to `PhysicsPersistentIslandTest.cpp` a `[physics][island]` test: drop a small dynamic box onto another resting dynamic box (or a 3-box stack on a floor); after enough steps for them to touch and stay touching but BEFORE they sleep (check `IsAwake`), assert all stacked boxes share ONE `IslandRootOf` root, and a far-away dynamic box has a different root:
  ```cpp
  // ---------------------------------------------------------------------------
  // Merge: dynamic bodies in a touching chain coalesce into ONE island.
  // ---------------------------------------------------------------------------
  TEST_CASE("PhysicsPersistentIsland: touching chain merges to one island",
            "[physics][island]")
  {
      WorldDef wd;
      wd.gravityY = Real(400);
      PhysicsWorld w(wd);

      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const Real hw = Real(5), hh = Real(5);
      const Real gap = Real(0.5);
      const BodyHandle b0 = AddBox(w, Vec2(Real(0), -hh),                              hw, hh);
      const BodyHandle b1 = AddBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)),         hw, hh);
      const BodyHandle b2 = AddBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)*Real(2)), hw, hh);
      const BodyHandle far = AddBox(w, Vec2(Real(500), -hh), hw, hh);

      // Settle into contact while still awake (mirrors PhysicsIslandTest case 5).
      for (int k = 0; k < 30; ++k) { w.Step(kStep); }
      REQUIRE(w.IsAwake(b0));
      REQUIRE(w.IsAwake(b1));
      REQUIRE(w.IsAwake(b2));

      const std::uint32_t r0 = w.IslandRootOf(b0.index);
      CHECK(r0 == w.IslandRootOf(b1.index));
      CHECK(r0 == w.IslandRootOf(b2.index));
      CHECK(r0 != w.IslandRootOf(far.index)); // isolated body -> different island
  }
  ```

- [ ] **Step 2: build + verify FAIL** (no merge yet → the three boxes are three islands). Kill strays, build Debug, run the new case; expect the `r0 == IslandRootOf(b1)` CHECK to fail.

- [ ] **Step 3: implement the merge collection + canonical apply.** In the UPDATE pass (`m_contactPool.ForEach` at PhysicsWorld.cpp:2151), at the `c.touching = (c.manifold.pointCount > 0);` line (2244), snapshot the OLD value and detect the begin transition for a dynamic-dynamic pair:
  ```cpp
  // Snapshot the previous touch-state BEFORE the overwrite so the island
  // registry can react to false->true (merge) / true->false (split) edges.
  const bool wasTouching = c.touching;
  c.touching = (c.manifold.pointCount > 0);

  // Phase A island maintenance. Only DYNAMIC-DYNAMIC pairs are island
  // edges (statics/kinematics anchor; a tile span has c.bIsBody == false
  // and never reaches this pass). Collect begin/end EDGES here; merges are
  // APPLIED after the pass in a canonical order (determinism), and splits
  // are deferred + quota-limited.
  if (c.bIsBody &&
      c.bodyB != kInvalidSlot &&
      TypeSlot(c.bodyA) == BodyType::Dynamic &&
      TypeSlot(c.bodyB) == BodyType::Dynamic)
  {
      if (!wasTouching && c.touching)
      {
          // false->true: queue a merge of the two bodies' islands.
          const std::uint32_t a = c.bodyA < c.bodyB ? c.bodyA : c.bodyB;
          const std::uint32_t b = c.bodyA < c.bodyB ? c.bodyB : c.bodyA;
          m_pendingMerges.push_back(BroadphasePair{ a, b });
      }
      else if (wasTouching && !c.touching)
      {
          // true->false: the island may have fractured -> split candidate.
          MarkSplitCandidate(IslandOf(c.bodyA));
      }
  }
  ```
  At the START of `UpdateContacts` (before the UPDATE `ForEach`), clear the scratch: `m_pendingMerges.clear();` (find the function entry — it is the method containing the `m_contactPool.ForEach` at :2151; clear right before that loop, or at the top of the method body). AFTER the UPDATE `ForEach` closes (after PhysicsWorld.cpp:2263 `});`), apply the merges in canonical order:
  ```cpp
  // ---- apply queued island merges in a canonical order ----------------
  // Sort by (min,max) body slot (mirrors the m_touchedEventPairs sort) so
  // the merge sequence is run-twice-identical regardless of pool emission
  // order. Each pair re-resolves its bodies' CURRENT islands (an earlier
  // merge this step may have already united them -> MergeIslands is a
  // no-op when both resolve to the same id).
  std::sort(m_pendingMerges.begin(), m_pendingMerges.end());
  for (const BroadphasePair& pr : m_pendingMerges)
  {
      const std::uint32_t ia = IslandOf(pr.a);
      const std::uint32_t ib = IslandOf(pr.b);
      if (ia != Island::kInvalidIsland &&
          ib != Island::kInvalidIsland &&
          ia != ib)
      {
          MergeIslands(ia, ib);
      }
  }
  ```
  (Do NOT dedup `m_pendingMerges` — duplicate pairs resolve to a same-island no-op after the first merge, so dedup is unnecessary; the sort alone fixes the order. Adding `std::unique` is optional and behavior-neutral.)

- [ ] **Step 4: build + run.** Kill strays, build Debug. Expected: the new `touching chain merges to one island` test PASSES; `PhysicsIslandTest` **case 5 now PASSES** (the 4-box stack shares one root, the far body differs). Run `[island]` then `[physics]` — all green (sleep still via the old global-UF `UpdateSleep`, which is fine; merge only affects `m_islandId`, which `UpdateSleep` does not yet read).
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[island]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```

- [ ] **Step 5: ArcaneCore clean** (Debug + Release), then **commit.** `feat(arcane/physics): merge islands on dynamic-dynamic touch-begin (canonical-order weighted union)`. Trailer.

---

### Task 3: Split-candidate marking + the deferred quota-limited split rebuild

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; extend `PhysicsPersistentIslandTest.cpp`.

Mark split candidates at ALL the separation/destroy seams, then implement `SplitIsland` (a fresh local union-find over one candidate island's members using their CURRENT touching pool contacts) and drive it from `Step` with the `kMaxSplitsPerStep` quota.

- [ ] **Step 1: failing test.** Append two `[physics][island]` tests: (a) a touching chain that is then pulled apart splits into separate islands (deferred, over a few steps); (b) a body that loses all its touching contacts becomes its own 1-body island. Use velocity to separate (e.g. settle a 2-box stack, then `SetVelocity` the top box up and away):
  ```cpp
  // ---------------------------------------------------------------------------
  // Split: separating a touching pair fractures the island (deferred); a body
  // that loses all contacts becomes its own 1-body island.
  // ---------------------------------------------------------------------------
  TEST_CASE("PhysicsPersistentIsland: separating a chain splits the island (deferred)",
            "[physics][island]")
  {
      WorldDef wd;
      wd.gravityY = Real(400);
      PhysicsWorld w(wd);

      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const Real hw = Real(5), hh = Real(5);
      const Real gap = Real(0.5);
      const BodyHandle b0 = AddBox(w, Vec2(Real(0), -hh),                      hw, hh);
      const BodyHandle b1 = AddBox(w, Vec2(Real(0), -hh - (Real(2)*hh + gap)), hw, hh);

      for (int k = 0; k < 30; ++k) { w.Step(kStep); }
      REQUIRE(w.IsAwake(b0));
      REQUIRE(w.IsAwake(b1));
      REQUIRE(w.IslandRootOf(b0.index) == w.IslandRootOf(b1.index)); // merged

      // Fling the top box far up + sideways so its contact with b0 separates.
      w.SetVelocity(b1, Vec2(Real(400), Real(-2000)));
      // A few steps for the contact to drop + the deferred split to resolve
      // (quota = 1 per step, but only 1 candidate here, so it resolves quickly).
      for (int k = 0; k < 20; ++k) { w.Step(kStep); }

      CHECK(w.IslandRootOf(b0.index) != w.IslandRootOf(b1.index)); // split
  }
  ```

- [ ] **Step 2: build + verify FAIL** (no split yet → they stay one island, the CHECK fails).

- [ ] **Step 3: mark candidates at the destroy/separation seams.** (The touch-END false→true/true→false marking landed in Task 2 at the :2244 seam.) Add marking at the destroy seams:
  - UPDATE-pass stale-handle reap (PhysicsWorld.cpp:2158, the `m_contactPool.Destroy(id); return;` inside `if (!FixtureSlotLive(...))`): BEFORE the `Destroy`, mark the dynamic-dynamic owner:
    ```cpp
    if (!FixtureSlotLive(c.a) || (c.bIsBody && !FixtureSlotLive(c.b)))
    {
        // A destroyed contact may fracture the island.
        if (c.bIsBody && c.touching &&
            c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
            c.bodyA < m_islandId.size() &&
            TypeSlot(c.bodyA) == BodyType::Dynamic &&
            c.bodyB < m_islandId.size() &&
            TypeSlot(c.bodyB) == BodyType::Dynamic)
        {
            MarkSplitCandidate(IslandOf(c.bodyA));
        }
        m_contactPool.Destroy(id);
        return;
    }
    ```
  - UPDATE-pass fat-box separation (PhysicsWorld.cpp:2185, the `m_contactPool.Destroy(id); return;` inside `if (!FatBoxesOverlap(c, extra))`): same guard + `MarkSplitCandidate(IslandOf(c.bodyA))` before the `Destroy`. (Use the identical guarded block.)
  - `DestroyContactsForFixture` (PhysicsWorld.cpp:1648-1658) and `DestroyContactsForBody` (PhysicsWorld.cpp:1660-1670): inside each `ForEach`, before `m_contactPool.Destroy(id)`, if the destroyed contact is a touching dynamic-dynamic pair, mark BOTH bodies' islands as split candidates AND wake the affected island (a removed body can free a pile that must re-settle). Use:
    ```cpp
    // (inside DestroyContactsForBody's ForEach, before Destroy)
    if (c.bIsBody && c.touching &&
        c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
        c.bodyA < m_islandId.size() && c.bodyB < m_islandId.size() &&
        TypeSlot(c.bodyA) == BodyType::Dynamic &&
        TypeSlot(c.bodyB) == BodyType::Dynamic)
    {
        MarkSplitCandidate(IslandOf(c.bodyA));
        MarkSplitCandidate(IslandOf(c.bodyB));
        WakeIsland(c.bodyA);
        WakeIsland(c.bodyB);
    }
    ```
    (Same block in `DestroyContactsForFixture`. The `bodySlot`/`fixtureSlot` being removed has its own `m_islandId` slot still valid here — `RemoveBody` bumps the gen + sets `m_alive=0` but does NOT clear `m_islandId` yet; that cleanup is below.)
  - In `RemoveBody`, after `DestroyContactsForBody(idx)` (PhysicsWorld.cpp:1045), release the removed body's island membership so a recycled slot does not inherit a stale island:
    ```cpp
    // Release the removed body's island membership (Phase A). Erase the
    // slot from its island's member list; if the island is now empty, free
    // it; otherwise mark it a split candidate (losing a member can fracture
    // the remaining pile). A recycled slot is reassigned a fresh island in
    // AddBody. Order-stable: erase-by-value over the small member vector.
    {
        const std::uint32_t isl = IslandOf(idx);
        if (isl != Island::kInvalidIsland)
        {
            auto& bodies = m_islands[isl].bodies;
            for (std::size_t i = 0; i < bodies.size(); ++i)
            {
                if (bodies[i] == idx)
                {
                    bodies[i] = bodies.back();
                    bodies.pop_back();
                    break;
                }
            }
            if (bodies.empty())
            {
                FreeIsland(isl);
            }
            else
            {
                MarkSplitCandidate(isl);
            }
            m_islandId[idx] = Island::kInvalidIsland;
        }
    }
    ```
    (Place this immediately AFTER the `DestroyContactsForBody(idx);` call. The swap-erase order does not affect determinism because `SplitIsland` re-derives membership from contacts, not member order, and the sleep/wake passes are order-insensitive to member-list order — see Self-Review.)

- [ ] **Step 4: implement `SplitIsland` + the deferred quota driver.** Replace the Task-1 stub `void PhysicsWorld::SplitIsland(std::uint32_t) {}` with:
  ```cpp
  void PhysicsWorld::SplitIsland(std::uint32_t islandId)
  {
      // Re-derive the connected components of one candidate island using a
      // FRESH local union-find over its CURRENT member bodies joined by
      // their touching dynamic-dynamic pool contacts. Bodies that no longer
      // share any touching contact fall into separate components; a body
      // with no touching contact becomes its own 1-body island. O(island).
      // Walk the pool ascending-id (deterministic). Clears the flag.
      if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
      {
          return;
      }
      m_islands[islandId].splitCandidate = false;

      // Snapshot the members (the rebuild reassigns m_islandId + may reuse
      // this island id for the largest component).
      std::vector<std::uint32_t> members = m_islands[islandId].bodies;
      if (members.size() <= 1)
      {
          return; // 0 or 1 member: nothing to fracture
      }

      // Local UF keyed by a body's position WITHIN `members` (a small dense
      // index space, not the global slot id) so the parent array is tiny.
      // memberOf[slot] -> local index; -1 (kNone) if not a member.
      // Build a slot->local map via a flat lookup over members (members is
      // small; a linear find is fine and avoids a hash alloc).
      const std::uint32_t n = static_cast<std::uint32_t>(members.size());
      std::vector<std::uint32_t> parent(n);
      for (std::uint32_t i = 0; i < n; ++i) { parent[i] = i; }

      auto localOf = [&](std::uint32_t slot) -> std::uint32_t
      {
          for (std::uint32_t i = 0; i < n; ++i)
          {
              if (members[i] == slot) { return i; }
          }
          return 0xFFFFFFFFu; // not a member of this island
      };
      auto find = [&](std::uint32_t x) -> std::uint32_t
      {
          while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
          return x;
      };

      // Union members joined by a touching dynamic-dynamic pool contact.
      // ForEach is ascending-id (deterministic). A contact whose bodies are
      // not BOTH in `members` is skipped (it joins to outside this island,
      // which cannot happen for an intact island but is cheap to guard).
      m_contactPool.ForEach([&](std::uint32_t /*id*/, const Contact& c)
      {
          if (!c.bIsBody || !c.touching) { return; }
          if (c.bodyA == kInvalidSlot || c.bodyB == kInvalidSlot) { return; }
          if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
              TypeSlot(c.bodyB) != BodyType::Dynamic) { return; }
          const std::uint32_t la = localOf(c.bodyA);
          const std::uint32_t lb = localOf(c.bodyB);
          if (la == 0xFFFFFFFFu || lb == 0xFFFFFFFFu) { return; }
          parent[find(la)] = find(lb);
      });

      // Group members by their local root. The component containing local
      // root R reuses `islandId` for the FIRST root encountered (keeps the
      // id sticky); every other distinct root gets a fresh island id.
      // Reset this island's member list; re-fill the kept component.
      m_islands[islandId].bodies.clear();
      // rootIsland[localRoot] -> the island id assigned to that component.
      // Linear scan over members keeps it alloc-light + deterministic.
      std::vector<std::uint32_t> rootLocal;   // distinct local roots, in first-seen order
      std::vector<std::uint32_t> rootIsland;  // parallel island id per root
      for (std::uint32_t i = 0; i < n; ++i)
      {
          const std::uint32_t r = find(i);
          // find r in rootLocal
          std::uint32_t ri = 0xFFFFFFFFu;
          for (std::uint32_t k = 0; k < rootLocal.size(); ++k)
          {
              if (rootLocal[k] == r) { ri = k; break; }
          }
          std::uint32_t isl;
          if (ri == 0xFFFFFFFFu)
          {
              // First member of a new component: the FIRST component reuses
              // islandId; subsequent components allocate fresh ids.
              isl = rootLocal.empty() ? islandId : AllocIsland();
              rootLocal.push_back(r);
              rootIsland.push_back(isl);
          }
          else
          {
              isl = rootIsland[ri];
          }
          const std::uint32_t slot = members[i];
          m_islandId[slot] = isl;
          m_islands[isl].bodies.push_back(slot);
      }
  }
  ```
  (Note: `AllocIsland()` may grow `m_islands` and invalidate references to `m_islands[islandId]`. The code above reads `m_islands[islandId].bodies.clear()` ONCE before any `AllocIsland`, then only ever indexes `m_islands[isl]` fresh after each `AllocIsland` — never holds a dangling reference across an `AllocIsland`. Verified: every `m_islands[...]` access is by index, re-evaluated, so a vector realloc is safe.)
  Drive the deferred split from `Step` stage 5. Add the candidate-collection + quota processing right BEFORE the `Island::UpdateSleep` call (PhysicsWorld.cpp:1564). At this point `m_islands[].splitCandidate` flags were set during this Step's `UpdateContacts`:
  ```cpp
  // ---- deferred island split (quota-limited, Phase A) -----------------
  // Process AT MOST kMaxSplitsPerStep split-candidate islands per Step
  // (Box2D's amortization). Collect candidates ascending-id (determinism),
  // then split the first quota of them; the rest carry their flag to the
  // next Step. A single big pile that fractures resolves over a few steps.
  m_splitCandidates.clear();
  for (std::uint32_t id = 0; id < m_islands.size(); ++id)
  {
      if (m_islands[id].splitCandidate && !m_islands[id].bodies.empty())
      {
          m_splitCandidates.push_back(id);
      }
  }
  std::uint32_t processed = 0;
  for (const std::uint32_t id : m_splitCandidates)
  {
      if (processed >= Island::kMaxSplitsPerStep) { break; }
      SplitIsland(id);
      ++processed;
  }
  ```
  (Iterating `m_islands` by ascending id IS the canonical order; `m_splitCandidates` preserves it. A candidate freed/emptied since marking is skipped by the `!bodies.empty()` guard.)

- [ ] **Step 5: build + run.** Expected: the new split test PASSES (b0/b1 end in different islands after separation). Run `[island]` + `[physics]` — all green. (The old `UpdateSleep` still owns sleep; splits only reshape `m_islandId`.)
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[island]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```

- [ ] **Step 6: ArcaneCore clean** (Debug + Release), then **commit.** `feat(arcane/physics): split-candidate marking + deferred quota-limited island split rebuild`. Trailer.

---

### Task 4: Per-island O(island) sleep replacing the global-UF `UpdateSleep`

**Files:** `Arcane/Core/src/Arcane/Physics/Island.hpp`, `Arcane/Core/src/Arcane/Physics/Island.cpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; (tests: existing cases gate it).

Rewrite `Island::UpdateSleep` to iterate the persistent registry. Per body: accumulate idle timer exactly as today (idle iff `v^2 < kSleepLinVel2 && |angVel| < kSleepAngVel`, else reset; jointed dynamics reset to 0). Per ISLAND (O(island)): if EVERY awake-dynamic member is past `kSleepTime`, sleep the whole island (`SetAwakeSlot(b,false)` + zero linear AND angular velocity). The O(n^2) global scan is gone.

- [ ] **Step 1: change the `UpdateSleep` signature in `Island.hpp`.** The new pass walks the registry + the live joint list; it no longer needs the contact array (merges/splits already maintained `m_islandId`). Replace the declaration (Island.hpp:85-90) with:
  ```cpp
  // ----------------------------------------------------------------
  // UpdateSleep: the per-Step island sleep pass (Step stage 5).
  // ----------------------------------------------------------------
  //
  // Phase A: iterates the PERSISTENT island registry (members already
  // known -- no per-step union-find rebuild, no O(n^2) global scan).
  // Resets the sleep timer of joint-attached dynamics (jointed bodies
  // never sleep), advances each awake dynamic's idle timer by `dt`, then
  // for each island sleeps it AS A UNIT iff every awake-dynamic member is
  // past kSleepTime (clearing awake + zeroing linear & angular velocity).
  //
  // `joints`/`jointCount` is the joint constraint list (nullptr/0 if no
  // joints). `dt` is the full Step timestep. No-op when the world has no
  // dynamics. Thresholds + whole-island-unit + exact-freeze are UNCHANGED
  // from the global-UF version this replaces.
  void UpdateSleep(PhysicsWorld& world,
                   const JointConstraint* joints,
                   std::uint32_t jointCount,
                   Real dt);
  ```
  (Drop the `contacts`/`contactCount` params; keep the joint params — the joint-reset pass is preserved verbatim.)

- [ ] **Step 2: rewrite `Island::UpdateSleep` in `Island.cpp`.** Delete the anon-namespace `UfFind` helper (lines 26-40) and the whole old body. Add `#include <Arcane/Physics/PhysicsWorld.hpp>` is already present; keep `<cmath>`. New body — note it needs registry access, so add public accessors on `PhysicsWorld` (Step 3) and call them. The pass:
  ```cpp
  void UpdateSleep(PhysicsWorld& world,
                   const JointConstraint* joints,
                   std::uint32_t jointCount,
                   Real dt)
  {
      const std::uint32_t count = world.Count();
      if (count == 0)
      {
          return;
      }

      // ---- joint-attached dynamics reset their sleep timer ------------
      // Jointed dynamic bodies never sleep so target joints keep authority
      // (ports the original behavior verbatim). The solver Prepared each
      // joint earlier this Step, so BodyA()/BodyB() are resolved slots.
      for (std::uint32_t k = 0; k < jointCount; ++k)
      {
          const Joint* j = joints[k].joint;
          if (j == nullptr)
          {
              continue;
          }
          const std::uint32_t a = j->BodyA();
          const std::uint32_t b = j->BodyB();
          if (a != kInvalidSlot && a < count &&
              world.Alive(a) && world.TypeSlot(a) == BodyType::Dynamic)
          {
              world.SetSleepTimerSlot(a, Real(0));
          }
          if (b != kInvalidSlot && b < count &&
              world.Alive(b) && world.TypeSlot(b) == BodyType::Dynamic)
          {
              world.SetSleepTimerSlot(b, Real(0));
          }
      }

      // ---- per-body idle-timer update (awake dynamics) ----------------
      // Idle: linear speed^2 < kSleepLinVel2 AND |angVel| < kSleepAngVel
      // -> accumulate dt; otherwise reset to 0 (UNCHANGED thresholds).
      for (std::uint32_t i = 0; i < count; ++i)
      {
          if (!world.Alive(i) ||
              world.TypeSlot(i) != BodyType::Dynamic ||
              !world.AwakeSlot(i))
          {
              continue;
          }
          const Vec2 v  = world.VelSlot(i);
          const Real v2 = v.x * v.x + v.y * v.y;
          const Real wv = world.AngVelSlot(i);
          if (v2 < kSleepLinVel2 && std::fabs(wv) < kSleepAngVel)
          {
              world.SetSleepTimerSlot(i, world.SleepTimerSlot(i) + dt);
          }
          else
          {
              world.SetSleepTimerSlot(i, Real(0));
          }
      }

      // ---- per-island sleep decision (O(island), no global scan) ------
      // For each island: if EVERY awake-dynamic member is past kSleepTime,
      // sleep the WHOLE island as a unit (clear awake + zero linear AND
      // angular velocity for each member). A member already asleep is
      // skipped (it does not veto -- a settled member that slept earlier
      // stays asleep). An island whose members are all asleep is a no-op.
      // Members iterated in island member-list order (deterministic given
      // a deterministic registry; the decision is order-INSENSITIVE -- it
      // is an all-members predicate, then a uniform apply).
      world.ForEachIsland([&](const std::vector<std::uint32_t>& bodies)
      {
          bool anyAwake = false;
          bool allIdlePastThreshold = true;
          for (const std::uint32_t b : bodies)
          {
              if (!world.Alive(b) || world.TypeSlot(b) != BodyType::Dynamic)
              {
                  continue; // defensive: a stale member is ignored
              }
              if (!world.AwakeSlot(b))
              {
                  continue; // already asleep -> does not veto
              }
              anyAwake = true;
              if (world.SleepTimerSlot(b) <= kSleepTime)
              {
                  allIdlePastThreshold = false;
                  break;
              }
          }
          if (anyAwake && allIdlePastThreshold)
          {
              for (const std::uint32_t b : bodies)
              {
                  if (world.Alive(b) &&
                      world.TypeSlot(b) == BodyType::Dynamic &&
                      world.AwakeSlot(b))
                  {
                      world.SetAwakeSlot(b, false);
                      world.SetVelSlot(b, Vec2(Real(0), Real(0)));
                      world.SetAngVelSlot(b, Real(0));
                  }
              }
          }
      });
  }
  ```
  (Semantic match to the old code: the old per-candidate scan slept a body iff no co-island member had `sleepT <= kSleepTime`; equivalently, the whole island sleeps iff every awake member is `> kSleepTime`. The new predicate is exactly that, computed once per island. A member slept earlier in the SAME pass cannot happen now — the apply is a single uniform sweep AFTER the predicate, so there is no order dependence within an island.)

- [ ] **Step 3: add the registry-iteration accessor on `PhysicsWorld`.** `Island::UpdateSleep` needs to walk islands without exposing the raw vectors. Add a public `ForEachIsland` (near the P2.4 seam accessors, hpp:775+):
  ```cpp
  // Visit each LIVE island's member-slot list (Phase A sleep seam). A live
  // island has a non-empty member list; freed ids (empty) are skipped.
  // Iterated ascending island-id (deterministic). Const callback (sleep
  // mutates bodies through the slot accessors, not the island record).
  void ForEachIsland(
      const std::function<void(const std::vector<std::uint32_t>&)>& fn) const
  {
      for (const Island::Island& isl : m_islands)
      {
          if (!isl.bodies.empty())
          {
              fn(isl.bodies);
          }
      }
  }
  ```
  (`#include <functional>` and `#include <vector>` are already included by PhysicsWorld.hpp via its existing members; verify `<functional>` is present — it is used by `ForEachContact`'s `std::function` param, so yes.)

- [ ] **Step 4: update the `Step` call site.** At PhysicsWorld.cpp:1564-1570, replace the call to match the new signature (drop the contact args):
  ```cpp
  Island::UpdateSleep(
      *this,
      m_jointConstraints.empty() ? nullptr : m_jointConstraints.data(),
      static_cast<std::uint32_t>(m_jointConstraints.size()),
      dt);
  ```
  (The deferred-split block from Task 3 runs immediately before this; keep that order.)

- [ ] **Step 5: build + run.** Kill strays, build Debug. Expected: `PhysicsIslandTest` **case 1** (resting sleeps + frozen) and **case 4** (stack sleeps as a unit + frozen) PASS; the full `[island]` + `[physics]` suites green; `PhysicsStaticSettleTest` (lines 199, 240 — capsule sleeps) green. Verify NO O(n^2) global scan remains: `Island.cpp` has no `UfFind`, no `UnionFindScratch`, no nested `for (j ...)` over all bodies.
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[island]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```

- [ ] **Step 6: ArcaneCore clean** (Debug + Release), then **commit.** `refactor(arcane/physics): per-island O(island) sleep over the persistent registry (replaces the global-UF O(n^2) scan)`. Trailer.

---

### Task 5: Wake-the-whole-island in the wake paths + RemoveBody wake

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; (tests: existing cases 2,3,4 gate it).

Change the single-body wake paths to wake the body's WHOLE island via `WakeIsland`. Box2D wakes the island on any member disturbance (pins case 4's neighbor-wake). For a `kInvalidIsland` body `WakeIsland` is a no-op on itself, but those paths still need the body itself awake — see the per-path notes.

- [ ] **Step 1: failing test (or confirm an existing one tightens).** Append a `[physics][island]` test that pins WHOLE-island wake on `ApplyImpulse`: settle a 3-box stack until ALL asleep, then `ApplyImpulse` the MIDDLE box; assert immediately that ALL THREE members are awake (not just the impulsed one):
  ```cpp
  // ---------------------------------------------------------------------------
  // Wake fan-out: disturbing ONE member of a sleeping island wakes the WHOLE
  // island immediately (Box2D-style island wake), not just the disturbed body.
  // ---------------------------------------------------------------------------
  TEST_CASE("PhysicsPersistentIsland: impulse on one member wakes the whole island",
            "[physics][island]")
  {
      WorldDef wd;
      wd.gravityY = Real(400);
      PhysicsWorld w(wd);

      AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
      const Real hw = Real(4), hh = Real(4);
      const int N = 3;
      std::vector<BodyHandle> boxes;
      for (int i = 0; i < N; ++i)
      {
          const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
          boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
      }
      for (int k = 0; k < 700; ++k) { w.Step(kStep); }
      for (int i = 0; i < N; ++i) { REQUIRE_FALSE(w.IsAwake(boxes[i])); }

      // Impulse the MIDDLE box -> the whole island wakes at once.
      w.ApplyImpulse(boxes[1], Vec2(Real(0), Real(-8000)));
      for (int i = 0; i < N; ++i)
      {
          CHECK(w.IsAwake(boxes[i])); // every member awake (island wake)
      }
  }
  ```

- [ ] **Step 2: build + verify FAIL** (today `ApplyImpulse` wakes only `boxes[1]` → the other two stay asleep, the CHECKs on `boxes[0]`/`boxes[2]` fail). (If the stack happens to be split into per-box islands by the deferred-split churn, this would already pass — but a settled stable stack is ONE island, so it fails. If it does not fail, diagnose why the stack is not one island before proceeding — that is a Task 2/3 bug, not a reason to weaken the test.)

- [ ] **Step 3: route the wake paths through `WakeIsland`.**
  - `SetVelocity` (PhysicsWorld.cpp:1206-1210): after setting `m_awake[i]=1; m_sleepTimer[i]=Real(0);` for a dynamic, add `WakeIsland(i);`:
    ```cpp
    if (bt == BodyType::Dynamic)
    {
        m_awake[i]      = 1;
        m_sleepTimer[i] = Real(0);
        WakeIsland(i); // wake the whole island, not just this body
    }
    ```
  - `ApplyImpulse(h, impulse)` (PhysicsWorld.cpp:1226-1227): after `m_awake[i]=1; m_sleepTimer[i]=Real(0);`, add `WakeIsland(i);` (BEFORE the velocity mutation is fine; the order of the awake flag vs velocity does not matter).
  - `ApplyImpulse(h, impulse, worldPoint)` (PhysicsWorld.cpp:1252-1253): same — add `WakeIsland(i);` after the awake/timer set.
  - `Wake(h)` (PhysicsWorld.cpp:1273-1274): after `m_awake[i]=1; m_sleepTimer[i]=Real(0);`, add `WakeIsland(i);`.
  - `WakeMoverPair(fa, fb)` (PhysicsWorld.cpp:1785-1794): the two wake blocks each wake one body; route each through `WakeIsland`. Replace:
    ```cpp
    if (da && m_awake[a] == 0 && (!db || m_awake[b] != 0))
    {
        m_awake[a] = 1;
        m_sleepTimer[a] = Real(0);
        WakeIsland(a); // wake the sleeper's whole island (Box2D contact wake)
    }
    if (db && m_awake[b] == 0 && (!da || m_awake[a] != 0))
    {
        m_awake[b] = 1;
        m_sleepTimer[b] = Real(0);
        WakeIsland(b);
    }
    ```
  (`WakeIsland(i)` sets `m_awake/m_sleepTimer` for ALL members INCLUDING `i`, so the explicit single-body set above each call is redundant but harmless — keep it so a `kInvalidIsland` dynamic, e.g. a transient un-assigned slot, still wakes itself. `WakeIsland` early-returns for `kInvalidIsland`, so the explicit set is the safety net.)
  - `RemoveBody`'s island wake: already handled in Task 3 Step 3 (the `DestroyContactsForBody` block calls `WakeIsland`). Confirm it is present; no new code here.

- [ ] **Step 4: build + run.** Expected: the new whole-island-wake test PASSES; `PhysicsIslandTest` **case 2** (impulse wakes), **case 3** (contact wakes), **case 4** (disturbance wakes a neighbor) all PASS. Case 4's neighbor-wake is now satisfied IMMEDIATELY by the island wake (the old path relied on the next-step contact-graph propagation; the island wake is stronger and still satisfies `anyNeighborAwake`). Full `[island]` + `[physics]` green.
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[island]"
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```

- [ ] **Step 5: ArcaneCore clean** (Debug + Release), then **commit.** `feat(arcane/physics): wake the whole island on body disturbance (SetVelocity/ApplyImpulse/Wake/WakeMoverPair)`. Trailer.

---

### Task 6: Determinism + delete the dead per-step UF + full gate

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`, `Arcane/Core/src/Arcane/Physics/Island.hpp`; (tests: case 6 + full suites gate it).

Remove the now-dead `m_uf` / `UnionFindScratch()` and any leftover references; tidy the `Island.hpp` forward decls; run the full determinism + behavioral gate Debug AND Release, plus ArcaneCore, plus a perf-smoke sanity that island management stays sub-millisecond.

- [ ] **Step 1: failing test (determinism, run-twice).** `PhysicsIslandTest` **case 6** (settle+sleep deterministic across two runs) already exists and is the gate; ALSO append a stronger persistent-registry determinism test to `PhysicsPersistentIslandTest.cpp`: build a scene with a stack + a separating body (exercising merge AND split AND sleep), run it TWICE, and assert bit-identical final positions + awake states + `IslandRootOf` roots:
  ```cpp
  // ---------------------------------------------------------------------------
  // Determinism: a scene exercising merge + split + sleep is bit-identical
  // across two runs (positions, awake state, island roots).
  // ---------------------------------------------------------------------------
  TEST_CASE("PhysicsPersistentIsland: merge+split+sleep is deterministic across two runs",
            "[physics][island]")
  {
      auto run = [](std::vector<Vec2>& pos, std::vector<int>& awake,
                    std::vector<std::uint32_t>& roots)
      {
          WorldDef wd;
          wd.gravityY = Real(400);
          PhysicsWorld w(wd);

          AddFloor(w, Vec2(Real(0), Real(5)), Real(200), Real(5));
          const Real hw = Real(4), hh = Real(4);
          std::vector<BodyHandle> boxes;
          for (int i = 0; i < 4; ++i)
          {
              const Real y = -(Real(2) * hh + Real(0.1)) * static_cast<Real>(i + 1);
              boxes.push_back(AddBox(w, Vec2(Real(0), y), hw, hh));
          }
          for (int k = 0; k < 200; ++k) { w.Step(kStep); }
          // Disturb the top -> a split candidate forms, then re-settles.
          w.ApplyImpulse(boxes[3], Vec2(Real(150), Real(-3000)));
          for (int k = 0; k < 500; ++k) { w.Step(kStep); }

          pos.clear(); awake.clear(); roots.clear();
          for (const BodyHandle b : boxes)
          {
              pos.push_back(w.Position(b));
              awake.push_back(w.IsAwake(b) ? 1 : 0);
              roots.push_back(w.IslandRootOf(b.index));
          }
      };

      std::vector<Vec2> p1, p2; std::vector<int> a1, a2;
      std::vector<std::uint32_t> r1, r2;
      run(p1, a1, r1);
      run(p2, a2, r2);

      REQUIRE(p1.size() == p2.size());
      for (std::size_t i = 0; i < p1.size(); ++i)
      {
          REQUIRE(p1[i].x == p2[i].x);
          REQUIRE(p1[i].y == p2[i].y);
          REQUIRE(a1[i] == a2[i]);
          REQUIRE(r1[i] == r2[i]); // island ids reproduce run-to-run
      }
  }
  ```

- [ ] **Step 2: build + verify the new determinism test PASSES** (the registry maintenance is already deterministic: merges sorted, splits ascending-id quota, contact walks via `ForEach`). If it FAILS, the divergence is a real determinism bug — diagnose (a non-deterministic island-id assignment, a member-order-dependent decision, or a `ForEach`-vs-map walk). Do not proceed until green.

- [ ] **Step 3: delete the dead per-step UF.** Now that nothing reads `m_uf`:
  - In `PhysicsWorld.hpp`: DELETE `std::vector<std::uint32_t> m_uf;` and its preceding "island sleep scratch" comment (the block at hpp:1383-1389), and DELETE the `UnionFindScratch()` accessor + its comment (hpp:789-798).
  - In `Island.hpp`: the forward decl `struct ContactConstraint;` is no longer referenced by the new `UpdateSleep` signature (only `JointConstraint` is). Keep the `ContactConstraint` forward decl ONLY if still referenced; since the new signature dropped the contact params, DELETE the now-unused `struct ContactConstraint;` forward decl (Island.cpp no longer needs `Solver.hpp`'s `ContactConstraint` either — but it DOES still include `Solver.hpp` for `JointConstraint`; keep that include). Verify `Island.cpp` no longer references `ContactConstraint`.
  - Grep the whole tree to confirm zero remaining references: `m_uf`, `UnionFindScratch`, `UfFind`. Expected: none (PhysicsWorld.cpp:2691's `IslandRootOf` was rewritten in Task 1; the only other `m_uf` users were `UpdateSleep` + `IslandRootOf`, both replaced).

- [ ] **Step 4: full Debug gate.** Kill strays, regen is NOT needed (no new files since Task 1's regen), build ArcaneTests Debug. Run:
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```
  Expected: ALL `[physics]` + `[physics][island]` green (the 6 original island cases + the new persistent-island cases + PhysicsStaticSettleTest). Then run the full suite to catch any cross-cutting regression:
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe
  ```
  Expected: green (exclude `[gpu]` with `~[gpu]` only if this machine lacks a capable GPU; otherwise run all).

- [ ] **Step 5: full Release gate.** Build ArcaneTests Release (`-p:Configuration=Release`), run `[physics]` from the Release exe dir:
  ```
  cd "D:/dev/starworks/Gacha/Arcane/bin/Release-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"
  ```
  Expected: green (Release exercises `NDEBUG` + the optimizer; determinism must hold under both).

- [ ] **Step 6: ArcaneCore static-CRT gate.** Build `Server/ArcaneCore/ArcaneCore.vcxproj` Debug + Release. Expected: clean (the island code lives in `Core/src`, compiled identically under the static CRT).

- [ ] **Step 7: perf-smoke sanity (island management stays cheap).** Confirm the structural change did not introduce a per-step cost regression in island management. There is no dedicated `[STEPPROF]` island counter required, but sanity-check via the Sandbox stress scene if available, OR simply confirm the deferred-split quota is honored (no full-registry rebuild per step) by code review: `SplitIsland` is O(island) and runs at most `kMaxSplitsPerStep` per Step; `MergeIslands` is O(smaller island); the sleep pass is O(total members) = O(dynamics), same as before but without the O(n^2) inner scan. State this finding in the commit body. (If a `[STEPPROF]`/`--perf` harness exists for the physics stress scene, run it and confirm island-mgmt time is ~microseconds; reference the harness invocation from the collision-rebuild Phase 1 plan if present. Skip if no such harness is wired — the algorithmic argument + the green behavioral suite is the gate for Phase A, since the 10k perf win is explicitly Phase B.)

- [ ] **Step 8: commit.** `refactor(arcane/physics): delete the dead per-step union-find (m_uf/UnionFindScratch); persistent islands are the sole island structure`. Note the full Debug+Release+ArcaneCore green gate + the determinism run-twice pass in the body. Trailer. **Do NOT push** (the user merges/pushes manually after a visual + CI gate).

---

## Self-Review Notes

**Spec coverage (every design element mapped to a task):**
- Persistent `Island` registry (free-list + id pool, `bodies` + `splitCandidate`) → Task 1 (struct in `Island.hpp`; `m_islands`/`m_islandFree`/`AllocIsland`/`FreeIsland` in PhysicsWorld). Kept as a `PhysicsWorld`-owned registry with `Island::` free helpers operating through `PhysicsWorld` methods — the **lower-churn option** (the existing `Island::UpdateSleep` free function stays a free function; only its signature changes). Stated explicitly.
- Per-body `m_islandId` SoA column sized in `EnsureCapacity` next to `m_awake`/`m_sleepTimer`; `kInvalidIsland` for statics/kinematics → Task 1.
- `IslandRootOf(i)` returns `m_islandId[i]` (high-bit-tagged slot for non-members so it never collides with a dense island id) → Task 1; consumers (`PhysicsDebugDraw.cpp:282`, `PhysicsIslandTest.cpp` case 5) verified.
- Body-create 1-body island; statics/kinematics excluded → Task 1.
- Merge on touch-BEGIN at the `c.touching` seam (PhysicsWorld.cpp:2244), canonical sorted-pair order mirroring `m_touchedEventPairs` (:1617), weighted union → Task 2.
- Split-candidate marking at touch-END (:2244), pool `Destroy` (:2158 stale, :2185 fat-box), `DestroyContactsForBody`/`ForFixture` (:1648-1670) + RemoveBody (:1045) with WAKE → Task 3.
- Deferred quota-limited split (`kMaxSplitsPerStep = 1`, ascending island-id, fresh local UF over members' current touching pool contacts) → Task 3.
- Per-island O(island) sleep replacing the O(n^2) `UpdateSleep`; thresholds (`kSleepLinVel2=4`, `kSleepAngVel=0.05`, `kSleepTime=0.5`) + whole-island-unit + exact-freeze + joint-reset all preserved → Task 4.
- Wake-the-whole-island in all wake paths (`SetVelocity`/`ApplyImpulse`×2/`Wake` :1206-1274, `WakeMoverPair` :1744-1795) → Task 5.
- Determinism (sorted merges, ascending-id quota splits, `ForEach` ascending-id walks, no RNG/wall-clock) + delete dead `m_uf`/`UnionFindScratch`/old per-step UF → Tasks 2/3 (determinism) + Task 6 (deletion + gate).

**Gates:**
- **Determinism:** `PhysicsIslandTest` case 6 (existing) + the new `merge+split+sleep deterministic` case (Task 6) assert bit-identical positions, awake states, AND island roots across two runs. This is the hard contract.
- **Behavioral sleep contract:** all 6 existing `PhysicsIslandTest` cases + `PhysicsStaticSettleTest` (lines 199, 240) stay green; the new `PhysicsPersistentIslandTest` cases pin the create/merge/split/wake facts. No exact goldens exist — the behavioral suite IS the gate.
- Debug + Release + ArcaneCore static-CRT all clean (Task 6).

**Soft spots (where execution must be careful):**
1. **Merge canonical ordering.** A step can produce multiple touch-BEGIN edges; the merge SEQUENCE affects which island id survives (weighted union keeps the larger). Determinism requires the same sequence every run → `m_pendingMerges` is sorted by `(min,max)` body slot before applying, mirroring the proven `m_touchedEventPairs` sort. Each merge re-resolves CURRENT islands (an earlier merge this step may have united the pair already → a same-id no-op). Soft spot: if a future change collects merges from a non-deterministic source (e.g. an `unordered_map` walk), determinism breaks — they MUST come from the ascending-id `ForEach` UPDATE pass. Verified the collection site is the `ForEach` pass.
2. **Deferred-split rebuild correctness.** `SplitIsland` re-derives components from the CURRENT touching pool contacts (ascending-id `ForEach`), NOT from member-list order, so the result is order-insensitive to how members were appended. The largest/first component reuses the island id (sticky id, fewer relabels). Soft spot: an `AllocIsland` inside `SplitIsland` can realloc `m_islands` — every access is by index (never a held reference across an `AllocIsland`), verified safe. Soft spot: the quota means a multi-way fracture resolves over several steps; the sleep pass tolerates a transiently-too-large island (it only ever OVER-groups, never under-groups, so it can only DELAY sleep by a step or two, never sleep a still-moving body — the per-body idle predicate still gates each member). Tests assert eventual resolution within a generous step budget.
3. **Wake-island fan-out.** `WakeIsland` walks `m_islands[isl].bodies`; if the registry transiently over-groups (a split not yet processed), wake touches a superset — harmless (waking an already-correct neighbor is a no-op vs. waking a body that should be a separate island, which just re-settles). The explicit single-body `m_awake`/`m_sleepTimer` set is KEPT in every wake path as the `kInvalidIsland` safety net (a dynamic with no island still wakes itself). Verified `WakeIsland` early-returns for `kInvalidIsland`.
4. **Preserving exact-freeze.** The sleep apply is byte-identical to the old code: `SetAwakeSlot(b,false); SetVelSlot(b, Vec2(0,0)); SetAngVelSlot(b, Real(0));`. A sleeping body is skipped by the next step's solver/integrate (the existing awake-gate), so position is frozen exactly (case 1 + case 4 assert `p1 == p0` bitwise). The new per-island predicate is mathematically equivalent to the old per-candidate veto (whole island sleeps iff every awake member is past `kSleepTime`), so the FRAME on which an island sleeps is unchanged → the frozen position is unchanged → run-twice + the existing cases hold.
5. **Build-order trap (Task 1 self-contained).** Task 1 must NOT delete `m_uf`/`UnionFindScratch` (the still-global `Island::UpdateSleep` references them until Task 4 rewrites it). Task 1 Step 5 explicitly corrects this: ADD-only, defer the deletion to Task 6. Between Task 1 and Task 2, `PhysicsIslandTest` case 5 is intentionally red (no merge yet) — flagged in the Task 1 commit body so a reviewer does not mistake it for a regression.
