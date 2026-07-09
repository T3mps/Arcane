# PhysicsWorld Decomposition — Step 2: ConstraintGraph Extraction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the contact subsystem — persistent contact pool, incremental contact coloring, and the `UpdateContacts`/`EmitContactConstraints` driving — out of the `PhysicsWorld` god-object into a contained `ConstraintGraph` collaborator, with zero behavior change (byte-identical full suite) and no perf regression.

**Architecture:** `ConstraintGraph` mirrors Box2D v3's contact subsystem boundary (contact.c + constraint_graph.c + the b2Collide driver in world.c — one cohesive unit; see survey 3). It owns the pool, the coloring, all narrowphase/create/emit scratch, and the create/update/destroy/emit drivers. It follows the proven `IslandManager` template exactly: no world back-pointer, methods take `PhysicsWorld& w`, PhysicsWorld befriends it (the same trust boundary the code has today), thin inline world forwarders keep every existing call site and test hook unchanged. The world keeps the flat entity SoA, `Step` orchestration, and the two stage-output hand-off buffers (`m_contactConstraints` → solver, `m_touchedEventPairs` → ContactManager). `ContactManager` (events) and the solver are untouched and never learn the graph exists.

**Tech Stack:** C++23, Arcane Core static lib (`/MD`), Catch2, msbuild/premake5, D3D12 + Vulkan via NVRHI for the exit gates.

**Companion surveys (read before executing; all line numbers below verified there at baseline):**
`.superpowers/sdd/pwdecomp2-survey-1-state-inventory.md` · `-2-callsite-hotness.md` · `-3-box2d-boundary.md` · `-4-test-gates.md`

## Global Constraints

- **Byte-identity is the gate.** Every task ends with the FULL Debug `~[gpu]` suite passing with the EXACT pinned assertion/case counts (re-pinned once by Task 0). Any asserted value change = the move was not mechanical — STOP and diagnose.
- **Move code, do NOT improve logic.** No reordering, no local renames, no "while I'm here". Field access changes ONLY as dictated by ownership: moved member `m_foo` → `m_foo` on the graph; stayed member `m_bar` → `w.m_bar`/`w.Bar()` under friendship. Shipped Box2D divergences (wake-at-create, static-in-color-0 allowed, assign-at-create) are FROZEN — do not "fix" toward Box2D.
- **Hot-path rule:** no per-substep loop may gain indirection. Survey 2 verdict is GREEN — the solver reads `ctx.contacts` from a once-per-Step `m_contactConstraints.data()` capture (cpp:1789-92); PRESERVE that hand-off and the `AwakeIndexData()`/`KinematicIndexData()` twins.
- **Determinism tripwires (survey 2 §G):** the create-order → `EnsurePair` LIFO ids → `AssignContactColor` lowest-free → ascending-color Gauss-Seidel chain, and the `MixContactId` warm-start keying, must move token-for-token. `exactlyOverlapping` (separation > 0) is NOT `c.touching` — do not substitute.
- **Model:** fable OK (the 2026-07-08 opus-only directive was lifted 2026-07-09). **Token economy lifted:** full `~[gpu]` per task, no `[physics]`-only shortcuts.
- **No `/fp:fast`; UTF-8 no BOM; ASCII comments. Units are MKS.**
- **Premake regen** needed when a `.cpp` is added or deleted (`ThirdParty\premake5\premake5.exe vs2026` from `Arcane/`; NOT GenerateProjects.bat, it hangs on `pause`).
- **Build/run:** MSBuild = `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nologo /v:minimal` via PowerShell (Git Bash mangles `/p:`; use `-p:` dashes there). Run tests FROM the exe dir; serialize `[gpu]`/Loom runs; foreground them. clangd diagnostics in this repo are noise — MSBuild is the oracle.
- **Commits:** `git commit -F - <<'EOF'` heredoc via Bash (no BOM), conventional-commit, NO Claude trailers, stage exact paths (never `-A`; parked Client/data noise).

---

## Baseline (branch `feature/arcane-physicsworld-decomp-constraintgraph` off main @5273b14d)

- `[physics]` = 30658 / 281 · full `~[gpu]` = **176633 / 502** (pre-Task-0)
- Release `[perf]` `PhysicsPerfTripwireTest` ≈ 51 ms (this machine) vs 14000 ms ceiling
- `Loom --frames 180` exits 0 on both backends
- **Task 0 grows the suite** (two new gap tests). After Task 0, re-capture `~[gpu]` totals and PIN those numbers for Tasks 1-4. Record them in the Task-0 commit message.

## Ownership split (the crux — survey 3 §2, adopted verbatim)

**MOVES into ConstraintGraph:**

| Item | Current location |
|---|---|
| `m_contactPool`, `m_cpPairs` | hpp:1619-1620 |
| `m_bodyColorMask`, `m_colorContacts` | hpp:1638-1639 |
| `m_npContacts`, `m_npStateBits` | hpp:1361-1362 |
| `NewPairRecord`, `m_newPairs`, `SpanEntry`, `m_genSpansW`, `m_genStaticsW`, `m_gridScratchW`, `m_spanEntriesW`, `m_newPairsW`, `m_allSpans` | hpp:1690-1709 |
| `m_spanContacts`, `m_spanCenters` | hpp:1676-1677 |
| `EmitSortKey`, `m_emitKeys`, `m_emitOrder`, `m_emitSorted` (mutable) | hpp:1723-1729 |
| `m_bpMovedScratch`, `m_bpFindScratch`, `m_bpStackScratch`, `kBroadphaseGrain`, `kCreateGrain` | hpp:1447-1455 (sole driver = UpdateContacts stage 1a) |
| `m_pendingMerges` (rides — fill 2941 / drain 2957-2984 both inside UpdateContacts) | hpp:1351 |
| Methods: `AssignContactColor` 2034-64, `ReleaseContactColor` 2066-2112, `ContactColorOf` 2114-21, `ValidatePersistentColoring` 2123-76, `ColoredContactCount` 2178-89, `BothAsleep` 2191-2211, `FatBoxesOverlap` 2213-61, `WakeMoverPair` 2263-2334, `TryCreateContact` 2336-2461, `UpdateOneContact` 2463-2544, `UpdateContacts` 2546-2985, `EmitContactConstraints` 2987-3196, `DestroyContactsForFixture` 1962-86, `DestroyContactsForBody` 1988-2013, `ReleaseAndDestroyContact` 3436-41, `MixContactId` (anon ns) 36-69 | PhysicsWorld.cpp |
| Step-inline lifts: stage-3b warm-start write-back 1807-38 → `WritebackImpulses`; stage-6 event derivation 1879-1932 → `CollectTouchedEventPairs` | PhysicsWorld.cpp |

**STAYS on PhysicsWorld:** `m_contactConstraints` (hpp:1432, solver hand-off), `m_touchedEventPairs` (hpp:1654, ContactManager hand-off), `FixtureSlotLive` (cpp:1939-44), `SlotsOverlap`/`ForEachContact`/`OnContact` glue (event-side), awake/sleep SoA + awake-set mechanism, body/fixture SoA, broadphase, executor, `m_splitCandidates` (island Step scratch), ALL public test hooks as thin forwarders (survey 4 §2 list).

**STAYS elsewhere:** island topology + `m_bodyContacts` adjacency (IslandManager), events (ContactManager), solver (never learns the graph exists).

**New seams:** `friend class ConstraintGraph;` on PhysicsWorld (T1); `ConstraintGraph::Pool() const` → `const ContactPool&` for IslandManager's 4 read-only sites (NO friend-of-graph — friendship rays stay world→collaborator); `Grow(next)` (mask growth); `DebugBodyMaskClear(slot)` (RemoveBody assert probe).

## File Structure

- **Create:** `Arcane/Core/src/Arcane/Physics/ConstraintGraph.hpp` / `.cpp` — the collaborator (IslandManager.{hpp,cpp} is the shape template).
- **Modify:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` / `.cpp` — remove moved members/methods, add member + friend + forwarders, redirect Step stages 2/3b/6.
- **Modify:** `Arcane/Core/src/Arcane/Physics/IslandManager.cpp` — 4 pool refs reroute to `w.m_graph.Pool()`.
- **Modify (T0):** `Arcane/Tests/src/PhysicsCompactedSolveTest.cpp`, `Arcane/Tests/src/PhysicsPersistentContactTest.cpp` — the two gap tests.
- **No changes:** Contact.{hpp,cpp} (ContactPool class unchanged), ContactManager.*, Solver/*.

---

## Task 0: Land the two coverage-gap tests on the baseline (they join the byte-identity oracle)

Survey 4 §3: two candidate-move behaviors are invisible to run-twice determinism — the lowest-free color VALUE semantics (G1; `ContactColorOf` currently has zero callers) and the stage-3b pool write-back (G2; no test reads the pool's stored impulses). Land both BEFORE any move so they gate the whole extraction.

**Files:**
- Modify: `Arcane/Tests/src/PhysicsCompactedSolveTest.cpp` (append after the churn case, ~line 137)
- Modify: `Arcane/Tests/src/PhysicsPersistentContactTest.cpp` (inside "Persistent contact walk == GenerateContacts constraint set", after line 231)

**Interfaces:**
- Consumes: existing public hooks `ContactColorOf(id)`, `ColoredContactCount()`, `ValidatePersistentColoring()`, `DebugEmitPoolConstraints(out)`, `SetPosition(handle, Vec2)`; the TU-local helpers `AddBox(w, pos, hw, hh)` / `kStep` (PhysicsCompactedSolveTest) — verify their exact signatures at the top of that TU before writing.
- Produces: the re-pinned `~[gpu]` baseline numbers for Tasks 1-4.

- [ ] **Step 1: Write the G1 color-reuse test** in PhysicsCompactedSolveTest.cpp. Three overlapping dynamic boxes in a row force contacts (b0,b1)/(b1,b2) sharing dynamic b1 → colors 0/1; destroy + recreate the first and assert lowest-free reuse. (Verify `ContactColorOf` returns `std::uint8_t` per hpp:865; integral comparison below is fine.)

```cpp
// Decomp step 2, T0 (gap G1): pin the lowest-free color VALUE semantics -- assign
// at create picks the LOWEST free color of the dynamic endpoints, and release at
// destroy makes that color immediately reusable. ValidatePersistentColoring proves
// the partition is VALID and the churn case proves it is DETERMINISTIC per binary,
// but neither pins WHICH color a contact gets: a consistent change (highest-free
// search, or a missed release + fresh assign) would pass both while re-ordering
// the colored Gauss-Seidel solve -- a cross-build float change run-twice cannot
// see. Also gives ContactColorOf (the [phasec] probe) its first caller.
TEST_CASE("PhysicsCompacted: destroyed contact's color is reused lowest-free on recreate",
          "[physics][phasec]")
{
    WorldDef wd;
    wd.gravityX = Real(0); // zero-g: the row of boxes stays put (drift over ~6 steps
    wd.gravityY = Real(0); // is micrometres -- far inside the fat-box keep-alive)
    PhysicsWorld w(wd);
    // Three dynamic boxes in a row, each overlapping its neighbour by 0.1 m:
    // contacts (b0,b1) and (b1,b2) share dynamic b1 -> forced into colors 0 and 1.
    // Create order is deterministic (sorted broadphase pairs over ascending slots)
    // and the pool starts empty, so (b0,b1) = pool id 0 / color 0 and
    // (b1,b2) = pool id 1 / color 1.
    BodyHandle b0 = AddBox(w, Vec2(Real(0),   Real(0)), Real(0.4), Real(0.4));
    AddBox(w, Vec2(Real(0.7), Real(0)), Real(0.4), Real(0.4));
    AddBox(w, Vec2(Real(1.4), Real(0)), Real(0.4), Real(0.4));
    w.Step(kStep);
    REQUIRE(w.ValidatePersistentColoring());
    REQUIRE(w.ColoredContactCount() == 2u);
    REQUIRE(w.ContactColorOf(0) == 0);  // (b0,b1): lowest free of two clean bodies
    REQUIRE(w.ContactColorOf(1) == 1);  // (b1,b2): b1 already holds color 0

    w.SetPosition(b0, Vec2(Real(1000), Real(1000)));  // fat boxes separate
    w.Step(kStep);
    w.Step(kStep);
    REQUIRE(w.ColoredContactCount() == 1u);  // (b0,b1) destroyed, color 0 released

    w.SetPosition(b0, Vec2(Real(0), Real(0)));        // back into overlap
    w.Step(kStep);
    REQUIRE(w.ColoredContactCount() == 2u);           // recreated
    REQUIRE(w.ContactColorOf(0) == 0);  // LIFO id reuse + lowest-free color reuse
    REQUIRE(w.ValidatePersistentColoring());
}
```

- [ ] **Step 2: Write the G2 write-back read** in PhysicsPersistentContactTest.cpp, inside the walk-oracle case immediately after `REQUIRE(SameConstraintSet(fromPool, fromGen));` (line 231). Verify the exact `ContactConstraint` point field names against `Solver/Solver.hpp:102-165` (`pointCount`, `points[p].normalImpulse`) before compiling.

```cpp
    // Decomp step 2, T0 (gap G2): the pool-emitted constraints must carry NONZERO
    // warm-start impulses here -- nonzero ONLY if Step stage 3b wrote the solver's
    // impulses back onto the pooled manifolds last step. The warm-start liveness
    // asserts elsewhere read post-solve constraints (nonzero even with a broken
    // write-back) and SameConstraintSet above deliberately skips impulses, so this
    // is the suite's only direct read of the pool's STORED impulses.
    bool anyWarmImpulse = false;
    for (const ContactConstraint& cc : fromPool)
    {
        for (int p = 0; p < cc.pointCount; ++p)
        {
            if (cc.points[p].normalImpulse > Real(0)) { anyWarmImpulse = true; }
        }
    }
    REQUIRE(anyWarmImpulse);
```

- [ ] **Step 3: Build Debug, run the two touched suites first** (fast signal): from the ArcaneTests exe dir, `.\ArcaneTests.exe "[phasec]"` then `.\ArcaneTests.exe "Persistent contact walk*"` — expect the new asserts GREEN on the baseline. If G1's id/color pins fail, diagnose against the real create order (print `ContactColorOf(0/1)` once) — the SCENE may need adjusting, the production code may NOT.
- [ ] **Step 4: Full `~[gpu]` run; RECORD the new totals** (expect 176633 + ~10 assertions / 502 + 1 cases; exact numbers go into the commit message and gate Tasks 1-4).
- [ ] **Step 5: Commit** — `test(arcane/physics): pin color lowest-free reuse + stage-3b writeback observability (ConstraintGraph decomp T0)`.

## Task 1: Scaffold ConstraintGraph + wire it as a member (no logic moved)

**Files:** Create `ConstraintGraph.hpp`/`.cpp`; modify `PhysicsWorld.hpp`.

**Interfaces:**
- Produces: `class Arcane::Physics::ConstraintGraph {};` (empty), `PhysicsWorld::m_graph` value member, `friend class ConstraintGraph;` beside the IslandManager friend (hpp:318).

- [ ] **Step 1:** Create `ConstraintGraph.hpp` mirroring IslandManager.hpp's shape: pragma-once, header doc stating the ownership thesis (contact pool + coloring + narrowphase/emit driving; mirrors Box2D contact.c + constraint_graph.c + b2Collide; hot hand-off buffers stay world-owned; delegation via `PhysicsWorld& w` under friendship), `namespace Arcane { namespace Physics {`, forward-declare `class PhysicsWorld;`, empty class body with a `// T1: scaffold only` marker.
- [ ] **Step 2:** Create `ConstraintGraph.cpp`: `#include <Arcane/Physics/ConstraintGraph.hpp>` + `#include <Arcane/Physics/PhysicsWorld.hpp>`, empty namespace body.
- [ ] **Step 3:** PhysicsWorld.hpp: add the include, `friend class ConstraintGraph;` at hpp:318 (document: same trust rationale as IslandManager), and `ConstraintGraph m_graph;` next to `IslandManager m_islandMgr;`.
- [ ] **Step 4:** Premake regen (new .cpp): from `Arcane/`, `ThirdParty\premake5\premake5.exe vs2026`.
- [ ] **Step 5:** Build Debug — clean. Full `~[gpu]` — Task-0-pinned counts, unchanged.
- [ ] **Step 6: Commit** — `refactor(arcane/physics): scaffold ConstraintGraph collaborator (no logic moved)`.

## Task 2: Move the persistent coloring (b2ConstraintGraph proper)

The smallest cohesive piece: `m_bodyColorMask` + `m_colorContacts` + the five coloring methods. The pool has NOT moved yet — coloring bodies read it as `w.m_contactPool` (friendship makes this verbatim). These methods temporarily take `PhysicsWorld& w` / `const PhysicsWorld& w` solely for pool access; Task 3 switches those reads to the member pool and drops parameters that become unused.

**Files:** ConstraintGraph.{hpp,cpp}, PhysicsWorld.{hpp,cpp}.

**Interfaces (produced on ConstraintGraph):**
- `void AssignContactColor(PhysicsWorld& w, std::uint32_t id, std::uint32_t a, std::uint32_t b, bool aDyn, bool bDyn)` — body verbatim from cpp:2034-64 (`m_contactPool` → `w.m_contactPool`).
- `void ReleaseContactColor(PhysicsWorld& w, std::uint32_t id)` — from cpp:2066-2112 (reads `w.m_btype`).
- `[[nodiscard]] std::uint8_t ContactColorOf(const PhysicsWorld& w, std::uint32_t id) const`
- `[[nodiscard]] bool ValidatePersistentColoring(const PhysicsWorld& w) const`
- `[[nodiscard]] std::size_t ColoredContactCount() const noexcept`
- `void Grow(std::uint32_t next)` — the `m_bodyColorMask.resize(next, 0u)` seam (from cpp:206).
- `[[nodiscard]] bool DebugBodyMaskClear(std::uint32_t slot) const noexcept` — `return slot >= m_bodyColorMask.size() || m_bodyColorMask[slot] == 0u;` (the RemoveBody assert probe).
- Ctor sizes `m_colorContacts` to `kColorCount` (from cpp:146).

- [ ] **Step 1:** Cut `m_bodyColorMask`/`m_colorContacts` decls (hpp:1638-1639, with their comment block — fix the hpp:1631 "solver's per-color bucket list" stale claim while the comment MOVES: it is lifecycle/validation state; the solver buckets by `cc.color` into its own `m_colorRefs`) into ConstraintGraph.hpp privates.
- [ ] **Step 2:** Move the five method bodies verbatim (cpp:2034-2189) into ConstraintGraph.cpp; apply ONLY the `m_contactPool` → `w.m_contactPool` and `m_btype` → `w.m_btype` rewrites.
- [ ] **Step 3:** World-side updates: ctor line 146 → delete (graph ctor does it); `EnsureCapacity` cpp:206 → `m_graph.Grow(next);`; RemoveBody assert cpp:1152 → `assert(m_graph.DebugBodyMaskClear(idx) && "...unchanged text...");`; call sites `AssignContactColor(...)` at cpp:2443 → `m_graph.AssignContactColor(*this, ...)` and `ReleaseContactColor(id)` at cpp:3439 → `m_graph.ReleaseContactColor(*this, id)`. World's public hooks become forwarders with UNCHANGED signatures: `ContactColorOf(id)` → `m_graph.ContactColorOf(*this, id)`, `ValidatePersistentColoring()` → `m_graph.ValidatePersistentColoring(*this)`, `ColoredContactCount()` → `m_graph.ColoredContactCount()`. `AssignContactColor`/`ReleaseContactColor` world decls: delete (their only callers were internal; the hpp:846-878 public block keeps only the three probe forwarders — verify with a grep that no test calls Assign/Release directly; survey 4 says none do).
- [ ] **Step 4:** Build Debug — clean. Full `~[gpu]` — Task-0-pinned counts (this includes the new G1 test now gating color VALUES).
- [ ] **Step 5: Commit** — `refactor(arcane/physics): move persistent contact coloring into ConstraintGraph (byte-identical)`.

## Task 3: Move the pool + lifecycle + driver + feed (THE block, one mutation domain)

Plan-time scope call (Step-1 lesson: entangled state moves together): the pool, its scratch, and every method that mutates or walks it move in ONE task. A staged split (pool first, driver later) would force temporary mutable-pool accessor seams and re-tokenize `UpdateContacts`/`EmitContactConstraints` twice — more transcription risk than one large verbatim move. The diff is big but mechanically reviewable: one deletion block, one insertion block, seam lines.

**Files:** ConstraintGraph.{hpp,cpp}, PhysicsWorld.{hpp,cpp}, IslandManager.cpp.

**Interfaces (produced on ConstraintGraph; all bodies verbatim, `m_<moved>` stays member-spelled, stayed world state → `w.`):**
- `void UpdateContacts(PhysicsWorld& w, Real dt)` — from cpp:2546-2985. Interior stayed-state: broadphase (`w.FixtureBroadphase()`), executor (`w.Executor()`), body/fixture SoA, `w.StaticList()`, awake set views; island calls via the world's existing inline forwarders (`w.WakeIsland/w.MergeIslands/w.MarkSplitCandidate/w.IslandOf/w.IslandIdCount`) and `w.m_islandMgr.AttachContactAdjacency`; `m_pendingMerges` rides as a member.
- `void TryCreateContact(PhysicsWorld& w, std::uint32_t fa, std::uint32_t fb)` (private) — cpp:2336-2461; the `AssignContactColor` call becomes member-internal (drop the `*this` indirection added in T2).
- `void WakeMoverPair(PhysicsWorld& w, std::uint32_t fa, std::uint32_t fb)` (private) — cpp:2263-2334; mutates `w.m_awake`/`w.m_sleepTimer` + `w.AddToAwakeSet`/`w.WakeIsland` (order contract preserved by the wholesale move).
- `void UpdateOneContact(const PhysicsWorld& w, std::uint32_t id, Contact& c, Real moveDt, Real threshSq) noexcept` (private) — cpp:2463-2544; calls `w.FixtureSlotLive(...)` (stays world).
- `bool BothAsleep(const PhysicsWorld& w, const Contact& c) const noexcept`, `bool FatBoxesOverlap(const PhysicsWorld& w, const Contact& c, Real extraMargin) const noexcept` (private) — cpp:2191-2261.
- `void EmitContactConstraints(const PhysicsWorld& w, std::vector<ContactConstraint>& out) const` — cpp:2987-3196 (mutable emit scratch preserves constness; `MixContactId` moves into ConstraintGraph.cpp's anon ns verbatim, cpp:36-69).
- `void WritebackImpulses(const std::vector<ContactConstraint>& ccs)` — the stage-3b lift, cpp:1807-1838 verbatim (reads ccs, writes pool manifolds; clamp semantics untouched).
- `void CollectTouchedEventPairs(std::vector<BroadphasePair>& out) const` — the stage-6 lift, cpp:1879-1932 verbatim INCLUDING the `exactlyOverlapping` separation>0 lambda and the sort+unique; reads only the pool, no `w` needed.
- `void DestroyContactsForFixture(PhysicsWorld& w, std::uint32_t fixtureSlot)`, `void DestroyContactsForBody(PhysicsWorld& w, std::uint32_t bodySlot)` — cpp:1962-2013.
- `void ReleaseAndDestroyContact(PhysicsWorld& w, std::uint32_t id, const Contact& c) noexcept` — cpp:3436-41; ordering FROZEN: `w.m_islandMgr.DetachContactAdjacency(w, id, c)` → `ReleaseContactColor(id)` (member now) → `m_pool.Destroy(id)`.
- `[[nodiscard]] const ContactPool& Pool() const noexcept { return m_pool; }` — the IslandManager read seam.
- Debug backings: `DebugContactCount()`, `DebugHasContact(w, a, b)` (the inline pool scan from hpp:1073-96 moves here).

- [ ] **Step 1:** Move all member decls listed in the ownership table (pool, cpPairs, np/create/span/emit/broadphase-MT scratch, pendingMerges, grain constants, the three private structs) into ConstraintGraph.hpp privates. `m_contactPool` renames to `m_pool` ONLY if done consistently in the same commit across every moved body (otherwise keep `m_contactPool` — zero-rename is the safer default; DECIDE ONCE, prefer keeping the name).
- [ ] **Step 2:** Move the method bodies verbatim (deletion technique from Step 1: `sed -i 'A,Bd'` bottom-to-top for contiguous ranges, then re-Read files before further Edit calls). Apply the two mechanical rewrite classes only.
- [ ] **Step 3:** T2 signature cleanup (same commit): coloring methods switch `w.m_contactPool` → member; `ContactColorOf(w, id)` → `ContactColorOf(id) const`, `ValidatePersistentColoring(w)` → `ValidatePersistentColoring() const` — pool is member; `ReleaseContactColor` keeps `w` only if it still reads `w.m_btype` (it does — keep). Update the world forwarders accordingly.
- [ ] **Step 4:** World-side redirects: Step stage 2 (cpp:1740-1742) → `m_graph.UpdateContacts(*this, dt); m_contactConstraints.clear(); m_graph.EmitContactConstraints(*this, m_contactConstraints);` · stage 3b block → `m_graph.WritebackImpulses(m_contactConstraints);` (keep the StepProf scope) · stage 6 derivation → `m_graph.CollectTouchedEventPairs(m_touchedEventPairs); m_contacts.Step(*this, m_touchedEventPairs);` (keep the Events scope) · `DropFixture` cpp:539 → `m_graph.DestroyContactsForFixture(*this, fi);` · `RemoveBody` cpp:1143 → `m_graph.DestroyContactsForBody(*this, idx);` · delete the world method bodies + private decls; keep/add thin public forwarders with EXACT signatures for: `DebugContactCount`, `DebugHasContact`, `DebugEmitPoolConstraints`, `DebugCopyActiveConstraints` (still world — copies `m_contactConstraints`), `ActiveContactCount` (still world), `ForEachContactConstraint` (still world), `ReleaseAndDestroyContact` (forwarder for IslandManager-side callers if any remain — grep; survey says all destroys funnel through moved code, so likely deletable — keep only if referenced).
- [ ] **Step 5:** IslandManager.cpp reroute: the 4 `w.m_contactPool` refs (SplitIsland :127; DebugValidateBodyContacts :462-463, :473) → `w.m_graph.Pool()`.
- [ ] **Step 6:** Build Debug — clean (Debug arms the color-mask assert + moved-code asserts). Full `~[gpu]` — Task-0-pinned counts EXACT. Extra confidence pass (cheap, survey 4's process note): run `.\ArcaneTests.exe "[determinism]"` twice back-to-back — identical pass counts.
- [ ] **Step 7: Commit** — `refactor(arcane/physics): move contact pool + narrowphase driver + solver feed into ConstraintGraph (byte-identical)`.

## Task 4: Comment/doc sweep (comment-only)

Survey 3 risk C6 enumerates the stale sites; sweep source only (docs/ stays historical).

- [ ] **Step 1:** Fix: IslandManager.hpp:9-10 ("a future Step-2 collaborator" → past tense, names ConstraintGraph) · PhysicsWorld.hpp:1426-1431 ISolver section comment (verify "WORLD-OWNED ContactConstraint pool" is still true — it is; adjust wording only if it names moved members) · cpp:3428-3435 ReleaseAndDestroyContact comment if any residue remains world-side · the ContactPool::Clear teardown caveat (cpp:1958-1961 pre-move) re-pointed at the graph · `grep -n "m_contactPool\|m_colorContacts\|m_bodyColorMask\|coloring" PhysicsWorld.{hpp,cpp}` and fix any comment still claiming world ownership.
- [ ] **Step 2:** Build + full `~[gpu]` — pinned counts (no code delta expected; comment-only diff).
- [ ] **Step 3: Commit** — `docs(arcane/physics): ConstraintGraph decomp comment sweep`.

## Task 5: Exit — full regression, perf, GPU-verify, review, push

- [ ] **Step 1:** Full Debug suite INCLUDING `[gpu]` (this machine has the GPU): `.\ArcaneTests.exe` bare — everything green, `RenderErrorCount()==0` latched by the gpu tests (PhysicsDebugDrawTest exercises the drawContacts path over the moved derivation).
- [ ] **Step 2:** Build Release; run Release `.\ArcaneTests.exe "[perf]"` — `PhysicsPerfTripwireTest` within budget (≈51 ms baseline; a material move = an inner read went indirect, find it).
- [ ] **Step 3:** Loom GPU-verify, serialized, from the Loom exe dir: `--backend vulkan --frames 180` then `--backend dx12 --frames 180` — both exit 0 (drives Hud/SandboxApp/PhysicsDebugDraw forwarder consumers in-process).
- [ ] **Step 4:** Shrink + anemic check: `wc -l` PhysicsWorld.cpp expect ≈ −1450 (≈2100 remaining), hpp ≈ −350; ConstraintGraph holds real state (pool + coloring + scratch) and behavior — not a forwarding shell.
- [ ] **Step 5:** Whole-branch review (independent subagent): byte-identity argument per task, move-don't-reorder honored, seam cleanliness (no hot-path indirection, no lateral friendship), stale comments. Apply comment-only fixes.
- [ ] **Step 6:** Push the branch; USER (or user-authorized) FF-merge to main.

---

## Self-Review (against the surveys)

- **Coverage:** survey 3's split table → ownership table above (all rows). Entanglements (a)-(e) → T3 (a: pendingMerges rides; b: WakeMoverPair moves; c: Pool() accessor + IslandManager reroute; d: CollectTouchedEventPairs; e: WritebackImpulses). Survey 4's G1/G2 → T0; forwarder list → T2 step 3 + T3 step 4. Risk register C1-C5 → Global Constraints + T3 gates; C6 → T4.
- **Deviation from survey 3 declared:** S3's T3/T4/T5 staged split is MERGED into one T3 here (interim mutable-pool seams would re-tokenize hot code twice; Step-1's scope-rebalance lesson applied at plan time). S3's T2/T6 retained as T2/T4.
- **Placeholder scan:** T0 contains full test code; move tasks cite exact line ranges + the two mechanical rewrite classes (verbatim-move plans reproduce no moved code by design — Step-1 precedent). One deliberate runtime-verify note (G1 id/color pins; T0 step 3 says how to resolve).
- **Type consistency:** `m_graph` member name used throughout; `Pool()` returns `const ContactPool&` BY REFERENCE (risk C5); signatures in T3 interfaces match T2's productions after the declared step-3 cleanup.
