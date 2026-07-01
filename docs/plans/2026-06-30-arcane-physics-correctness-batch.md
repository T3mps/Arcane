# Arcane Physics correctness/robustness batch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** Close four correctness/robustness gaps so the physics engine reliably handles anything: (A) sensors must not couple rigid islands, (B) dense dynamics resting on tile spans must not trip the no-sleeping-dynamic invariant, (C) joints must union islands so jointed constructs can sleep, (D) collision filters must be enforced.

**Architecture:** All four are targeted changes in `Core/Physics`. A/D are contact-creation gates; B restores the "island is uniformly awake" invariant at merge; C makes joints island edges + lets jointed constructs sleep. Each DELIBERATELY changes behavior only for the affected scene class (sensors present / tile-span piles / jointed / filtered) — every unaffected scene stays byte-identical, so the existing `[physics]` suite must stay green with new behavioral tests added per item.

**Tech Stack:** C++23, Arcane Core physics, Catch2 (`Arcane/Tests/src`), MSVC (VS2026), premake5.

## Global Constraints

- **Byte-identical for UNAFFECTED scenes.** The full `[physics]` suite stays green with NO re-baseline of non-affected cases; each item adds new behavioral tests that assert the intended NEW behavior. If an *existing* non-sensor / non-jointed / non-filtered / non-tile-span test changes output, STOP — that's a regression, not the intended change.
- /MD dynamic CRT, C++23, UTF-8 no BOM, ASCII comments only (no em-dashes). Presentation-free Core (glm + std + sibling Physics headers).
- clangd diagnostics in this workspace are FALSE POSITIVES; MSVC + ArcaneTests are truth.
- **Stage ONLY per-task files by explicit path. NEVER `git add -A`.** Unrelated working-tree changes (Client/data/ui_screens/*, AGENTS.md, Arcane/.screenshots/, Server/cpp_coding_style.txt, two 2026-06-24 docs) MUST stay untouched/unstaged.
- Build (PowerShell, NOT Git Bash): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m`.
- Premake (only if a NEW test file is added): `& "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026` from `Arcane\`.
- Tests from exe dir: `& "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe" "[physics]"`.
- Commit trailers (exact):
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` / `Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux`
- Branch: `feature/arcane-physics-correctness-batch` (already created off main `c8ebb67a`).

---

## Task A: Sensors must not couple rigid islands

**Files:** Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; Test: extend an existing `[physics][island]` test file or add `PhysicsSensorIslandTest.cpp`.

**Design:** `c.solverRelevant` is `(da||db) && !sensorA && !sensorB`, so for a dyn-dyn pair `solverRelevant == non-sensor`. Gate the two island-coupling sites on it. The merge/split serial tail keys off `kNpStarted`/`kNpStopped`, which are ONLY set by the touch classification — so gating the classification transitively cleans the tail.

- [ ] **Step 1** — In `TryCreateContact`, the `m_bodyContacts` insert (`PhysicsWorld.cpp:2409`): change `if (aDyn && bDyn)` to `if (aDyn && bDyn && solverRelevant)` (the `solverRelevant` local is in scope here). A dyn-dyn sensor contact is no longer recorded as an island edge.
- [ ] **Step 2** — In `UpdateOneContact`, the begin/end-touch classification (`PhysicsWorld.cpp:2474-2476`): add `c.solverRelevant &&` to the condition guarding the `kNpStarted`/`kNpStopped` set, so a dyn-dyn sensor contact never flags a merge/split. (Keep the existing `bIsBody`/both-Dynamic checks.)
- [ ] **Step 3 (defensive)** — the serial-tail `kNpDestroy` MarkSplitCandidate (`PhysicsWorld.cpp:2874`): add `c.solverRelevant` to its guard so destroying a touching sensor contact doesn't schedule a wasted split. (Harmless either way, but keeps intent consistent.)
- [ ] **Step 4 — Test** — New `[physics][island]` case: two dynamic bodies whose ONLY overlapping fixture pair is a SENSOR (set `isSensor`/fixture sensor on one) must have DIFFERENT `IslandRootOf(...)` while overlapping (they must not merge); and a sensor begin/stay event must still fire (assert via the event/DebugHasContact path). Add a second case: a real (non-sensor) dyn-dyn contact still merges (regression guard that the gate didn't over-fire).
- [ ] **Step 5** — Build + `[physics]` all green (existing non-sensor cases byte-identical). Commit (`PhysicsWorld.cpp` + the test; regen if a new test file was added).

Commit subject: `fix(arcane/physics): sensors no longer couple rigid islands (gate island edges on solverRelevant)`

---

## Task D: Enforce collision filtering

**Files:** Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; Test: `PhysicsCollisionFilterTest.cpp` (new).

**Design:** One gate in `TryCreateContact` (the single funnel for mover-mover, dyn-static, kin-static). Box2D rule: collide iff `(catA & maskB) != 0 && (catB & maskA) != 0`. Rejecting before `EnsurePair` means the pair never enters the pool -> no solve AND no event (events derive from the same pool). Body-body only; tile spans carry no per-fixture filter (out of scope).

- [ ] **Step 1 — Test first** — New `PhysicsCollisionFilterTest.cpp` (`[physics][filter]`): (a) two dynamics whose fixtures are filtered apart (`categoryBits`/`maskBits` so `(catA&maskB)==0`) fall THROUGH each other (never collide) and fire NO begin event; (b) two dynamics with compatible filters DO collide/rest (regression); (c) default filters (cat=1/mask=all) collide (nothing regressed). Use `IslandRootOf`/positions/`DebugHasContact` to assert. Run it against the CURRENT code and confirm case (a) FAILS (they currently collide) -> proves the gap.
- [ ] **Step 2** — In `TryCreateContact` (`PhysicsWorld.cpp:2302`), AFTER the same-body + liveness guards (after `:2316`, before the orientation/`EnsurePair` block): add
  ```cpp
  // Collision filter (Box2D rule): collide iff each side's category is in the
  // other's mask. A filtered-out pair never enters the pool -> no solve, no event.
  if (((m_fxFilterCat[fa] & m_fxFilterMask[fb]) == 0u) ||
      ((m_fxFilterCat[fb] & m_fxFilterMask[fa]) == 0u))
  {
      return;
  }
  ```
  (`fa`/`fb` are the fixture slots; the SoA is sized for all fixtures.)
- [ ] **Step 3** — Build + run `[physics][filter]` (case (a) now passes) + full `[physics]` (all green; default-filter scenes unchanged since cat=1/mask=all always collides). Regen (new test file). Commit.

Commit subject: `feat(arcane/physics): enforce per-fixture collision filtering at contact creation (Box2D category/mask)`

---

## Task B: Restore "island is uniformly awake" (fixes tile-span sleep-invariant trip)

**Files:** Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`; Test: `PhysicsIslandWakeMergeTest.cpp` (new) or extend `PhysicsPersistentIslandTest.cpp`.

**Design:** The `EmitContactConstraints` assert (`PhysicsWorld.cpp:3049`) trips because `MergeIslands` grafts an already-sleeping singleton (a body that slept early resting purely on tile spans) into an AWAKE island without waking it (the `WakeMoverPair` `moverIsMoving` gate at `:2286` declines to wake when the incoming body is near-idle). Restore Box2D's invariant that an island is uniformly awake: at merge, if the two islands differ in awake state, wake the sleeping one.

- [ ] **Step 1** — At the merge-application loop (`PhysicsWorld.cpp:2905-2916`, the `m_pendingMerges` loop that calls `MergeIslands(ia, ib)`): before/at each merge, if exactly one of the two bodies (`pr.a`/`pr.b`) is awake, wake the other's island so the merged island is uniformly awake. Concretely, for each pending pair where `IslandOf(a) != IslandOf(b)`: if `m_awake[a] != m_awake[b]`, call `WakeIsland(...)` on the sleeping side BEFORE `MergeIslands`. (A begin-touch always involves at least one moving/awake body, so the merged island should be awake.) Prefer putting the wake inside `MergeIslands` only if it cleanly has both bodies' awake state; otherwise do it in the loop where `pr.a`/`pr.b` are known. Keep the existing `kInvalidIsland`/`ia==ib` guards.
- [ ] **Step 2 — Test** — New `[physics][island]` case reproducing the trip: build a passability/tile-span bowl (mirror `PhysicsNarrowphaseMtTest.cpp`'s `RunCaptureSpans` geometry) with sleep ENABLED; let a body settle + sleep resting purely on the tile span (singleton island), then introduce a near-idle body that drifts into speculative contact with it; step enough for the merge; assert (a) no assertion/UB and (b) the merged island ends up consistent (both awake then settling, or both asleep) -- never a live mixed awake/asleep island feeding the solver. If reliably reproducing the drift is hard, at minimum assert that after any merge, no island simultaneously contains an awake dynamic and a sleeping dynamic that emits a constraint (an invariant check over islands post-step). Confirm this case would trip the assert on the pre-fix code (run in a Debug build where the assert is live).
- [ ] **Step 3** — Build + `[physics]` green (this also lets the G2 span test's sleep-disable workaround be revisited later -- NOT in this task; just note it). Commit.

Commit subject: `fix(arcane/physics): wake the sleeping island on cross-awake merge (uniform-awake invariant; fixes tile-span sleep trip)`

---

## Task C: Joints union islands + jointed constructs can sleep (Box2D)

**Files:** Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`, `Island.cpp`; Test: `PhysicsJointSleepTest.cpp` (new). (Opus implementer + opus review — largest/subtlest item.)

**Design (5 parts):** joints become island edges so jointed dynamics share an island and sleep as a unit; the never-sleep pin is removed; the solver skips asleep joints.

- [ ] **Step 1 — AddJoint merges islands.** In `AddJoint` (`PhysicsWorld.cpp:1257-1274`), after the existing `Wake(def.a)/Wake(def.b)`: if BOTH bodies are Dynamic, `MergeIslands(IslandOf(a), IslandOf(b))` (guard `ia != ib && both != kInvalidIsland`). A joint to a static/kinematic anchor unions nothing (the anchor is not an island member). Resolve `a`/`b` from the joint def's body handles to slots.
- [ ] **Step 2 — RemoveJoint / body-drop marks split + wakes.** In `RemoveJoint` (`PhysicsWorld.cpp:1276-1290`) and the joint-drop in `RemoveBody` (`:1214-1224`): if the joint connected two Dynamic bodies, `WakeIsland(a)` + `MarkSplitCandidate(IslandOf(a))` (mirrors the contact-removal pattern in `DestroyContactsForBody`), so the island re-derives components now that the joint edge is gone.
- [ ] **Step 3 — SplitIsland unions joint edges too.** In `SplitIsland`, after the per-body contact-adjacency union loop (`PhysicsWorld.cpp:3474-3483`): add a loop over `m_joints` -- for each joint whose BOTH bodies are Dynamic and both are members of this island (`m_splitLocalIndex[slot] != kSplitLocalNone`), union their local indices. This keeps a jointed pair in ONE component even when they share no touching contact (so removing a contact never wrongly splits a still-jointed pair). Iterate `m_joints` deterministically (index order); resolve each joint's `BodyA()/BodyB()` slots.
- [ ] **Step 4 — Drop the never-sleep pin.** In `Island::UpdateSleep` (`Island.cpp:39-62`): DELETE the joint-attached-dynamics sleep-timer-reset loop entirely (and the now-unused `joints`/`jointCount` params if nothing else uses them -- check; keep the signature if the call site is simpler to leave). Jointed dynamics now accumulate idle time normally and sleep as a unit via island membership (Steps 1/3). Update the stale comment.
- [ ] **Step 5 — Solver skips asleep joints.** At the `m_jointConstraints` rebuild (`PhysicsWorld.cpp:1727-1733`): only push a `JointConstraint` for a joint that has at least one AWAKE dynamic body (skip joints whose dynamic endpoints are all asleep). This keeps the solver's global joint passes (`SoftStep::PrepareJoints/SolveJoints`) from integrating sleeping constructs -- the perf win -- and respects the sleeping-frozen contract. A woken island's joints re-enter automatically next step (the array is rebuilt each step). Consider adding a Debug assert in the joint pass that no solved joint references a sleeping dynamic (mirrors the contact invariant).
- [ ] **Step 6 — Tests** — New `PhysicsJointSleepTest.cpp` (`[physics][joint]`): (a) a jointed dynamic pair/short chain at rest SLEEPS as a unit (assert `!IsAwake` for all members after settling -- currently they never sleep); (b) `IslandRootOf` is shared across jointed dynamics (they're one island); (c) an impulse on one member wakes the WHOLE jointed island; (d) removing the joint lets the pieces settle/sleep independently (island splits). Also confirm the existing `PhysicsJointsTest` cases still pass (joint SOLVING behavior unchanged while awake -- byte-identical for the awake solve).
- [ ] **Step 7** — Build + full `[physics]` green (existing joint-solve tests unchanged; new sleep behavior added). Commit. (Regen for the new test file.)

Commit subject: `feat(arcane/physics): joints union islands + jointed constructs sleep (Box2D island-joint model)`

---

## Self-Review (plan vs investigations)

- **Coverage:** A (2 gates + defensive) / D (1 gate) / B (wake-on-cross-awake-merge) / C (5-part joint-island) all map to the exact investigated sites. Each has a behavioral test asserting the NEW behavior + a regression guard that unaffected behavior is unchanged.
- **Byte-identity contract:** every change is a no-op for scenes without the trigger (no sensors / compatible-or-default filters / no cross-awake merge / no joints), so the existing `[physics]` suite stays green; the four new test files add the changed-behavior coverage. E (spatial grid) intentionally excluded (efficiency-only).
- **Ordering:** A, D (independent contact-creation gates) -> B (merge/wake) -> C (joints; builds on the island machinery, consistent with B's uniform-awake invariant since AddJoint wakes then merges). No cross-task signature collisions.
