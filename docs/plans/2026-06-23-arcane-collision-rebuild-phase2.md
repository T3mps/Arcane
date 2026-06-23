# Collision Module Rebuild — Phase 2 Implementation Plan (per-fixture broadphase + move buffer)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-key the mover broadphase from body-level (one fat proxy per body) to **per-fixture** proxies so the 37-wire whisk stops brute-forcing every fixture against every body — landing the perf fix for the ~89% provably-disjoint narrowphase waste — then add a **move buffer + incrementally-maintained pair set** so pair discovery is O(moved·log n) instead of a full O(n·log n) traversal twice per step.

**Architecture:** Two sub-phases, gate-green throughout. **2a (Tasks 1-3):** build a per-fixture `DynamicTree` proxy index ALONGSIDE the existing body-level `m_moverBroadphase`, validate its fixture-pair output against a brute-force oracle, then switch `GenerateContacts` + `ContactManager` to consume fixture-pairs, then retire the body-level broadphase. **2b (Tasks 4-5):** add a move buffer (dirty-proxy tracking) + a persistent pair set maintained incrementally, validate `incremental == full == brute-force`, switch the consumers. **Task 6:** full gate + perf re-measure + memory. The deferred `UpdateMoverProxies(bodySlot)` helper from Phase 1 is folded in at Task 1 (re-keying turns every broadphase-update site into a per-fixture loop, so one helper is the natural shape). The deferred `Residents()` durable-handle variant stays deferred (no combat-sphere consumer is built here).

**Tech Stack:** C++23, Core (static-CRT + /MD dual build), Catch2 (`Arcane/Tests`), premake5 (`vs2026`), MSBuild. Spec: `docs/superpowers/specs/2026-06-23-arcane-collision-module-rebuild-design.md` (§5 DynamicTree, §7 contact pipeline, §8 testing/determinism, §10 sequencing). Prior: `docs/superpowers/plans/2026-06-23-arcane-collision-rebuild-phase0-1.md` (landed: `SpatialGrid` statics + residency).

---

## Conventions (read once)

- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (Release: swap `-p:Configuration`).
- **Run tests:** from the exe dir — `cd "Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"`. Full gate = no filter, Debug AND Release.
- **ArcaneCore static-CRT:** `"<msbuild>" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug|Release -p:Platform=x64 -m -v:minimal -nologo`. Core must stay presentation-free (glm + std + sibling Physics headers only) and compile BOTH /MD and static-CRT.
- **New source files** require a project regen, BOTH workspaces: `cd "D:/dev/starworks/Gacha/Arcane" && "D:/dev/starworks/Gacha/ThirdParty/premake5/premake5.exe" vs2026` AND `cd "D:/dev/starworks/Gacha/Server" && "D:/dev/starworks/Gacha/ThirdParty/premake5/premake5.exe" vs2026` (ArcaneCore globs `Arcane/Core/src/**.cpp`). NOT GenerateProjects.bat (it hangs on a `pause`). `VCPKG_ROOT` is set.
- **clangd/IDE diagnostics are FALSE POSITIVES** in this repo (it even reports a bogus "compiler version" error); MSVC is the source of truth.
- **Kill stray Loom** before building if a plugin-lock error appears: `powershell -c "Get-Process Loom -ErrorAction SilentlyContinue | Stop-Process -Force"`.
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Work on branch `feature/arcane-sandbox-spawn-selector`. Do NOT push.
- **Determinism is the one hard contract** (run-twice identical). The static/fixture candidate ORDER may shift deliberately (sorted) — re-baseline a POSITION golden only if the failure is a pure ordering shift; a behavioral/invariant failure (a stack stops settling, a body tunnels, an overlap count changes) is a REAL regression, not a re-baseline (spec §8; `feedback_engine_evolves_not_frozen`).

## Architecture facts (from the codebase map — rely on these)

- **`IBroadphase`** (`Arcane/Core/src/Arcane/Physics/Broadphase/Broadphase.hpp`): `void Update(uint32_t id, const Aabb2& box)` (upsert), `void Remove(uint32_t id)`, `int QueryAABB(const Aabb2&, vector<uint32_t>&) const`, `int Pairs(vector<BroadphasePair>&) const`. `struct BroadphasePair { uint32_t a, b; }` (a<b, output sorted lexicographically). The interface is **id-agnostic** — re-keying body→fixture is a semantics change only, no interface change.
- **`DynamicTree`** (`Broadphase/DynamicTree.hpp/.cpp`) is the default mover broadphase. Fat AABBs (`Node::fat = tight ± kMargin`, `kMargin=8`), coherent-motion skip (no reinsert if the new tight box still fits in the stored fat box), index node pool + free list, `m_leafOfId` (dense `vector<uint32_t>` keyed by id → leaf). `Pairs()` is a FULL traversal each call (no move buffer today). Iteration is id-ascending; output `std::sort`ed by (a,b).
- **`m_moverBroadphase`** (`PhysicsWorld.hpp`, `unique_ptr<IBroadphase>`) built by `MakeBroadphase(def)` from `WorldDef::broadphase` (default `Tree`; alts `SpatialHash`, `SweepAndPrune`). Updated body-level at: `AddBody` (~789), `SetPosition` (~1019), `MovePosition` (~1039), `Step` stage-1 kinematic (~1312), `CommitSlotPosition` (inline `PhysicsWorld.hpp` ~714), `BulletSweep` (~2070); removed at `RemoveBody` (~901). Every call passes `SlotAabb(bodySlot)`. (Line numbers are post-Phase-1; locate by pattern.)
- **Fixture SoA** (`PhysicsWorld.hpp`): `m_fxShape[fi]`, `m_fxLocalPosX/Y[fi]`, `m_fxLocalAngle[fi]`, `m_fxFriction[fi]`, `m_fxRestitution[fi]`, `m_fxSensor[fi]`, `m_fxBody[fi]` (owning body slot), `m_fxGen[fi]` (0=dead), `m_fxCount`, `m_fxFree`. `m_bodyFixtures[bodySlot]` = `vector<uint32_t>` of fixture slots (insertion order; swap-and-pop on `DropFixture` so NOT order-stable — but slot ids are stable). `ComposeFixtureXf(bodyPos, bodyAngle, fxLocalPos, fxLocalAngle) -> Transform` is the canonical world-transform formula; `m_fxShape[fi].ComputeAABB(xf)` gives the fixture world AABB.
- **`GenerateContacts(Real dt)`** (`PhysicsWorld.cpp` ~1390-1957): clears + rebuilds `m_contactConstraints` each step. Builds `m_genFxAabb` (per-fixture world AABB) + `m_genBodyAabb` (per-body union). STATIC path (per dynamic body → `StaticCandidates` → nested fixture loop) and MOVER path (`m_moverBroadphase->Pairs(m_genPairs)` → per body-pair → nested fixture loop with `fxDisjoint` reject → `Collide` → `emit`). `emit(...)` builds a `ContactConstraint` with warm-start key `cp.id = MixContactId(manifoldPointId, fixA, fixB)`. **Orientation convention: A is always the dynamic body; if both dynamic, the lower body slot is A.**
- **`MixContactId(base, fixA, fixB)`** (`PhysicsWorld.cpp` ~53): order-SENSITIVE hash (`(base,fixA,fixB)` ≠ `(base,fixB,fixA)`). Warm-start continuity REQUIRES a stable per-step fixture-pair orientation.
- **`ContactManager::Step(PhysicsWorld&)`** (`ContactManager.cpp` ~113-201): independently calls `w.MoverBroadphase().Pairs(m_pairScratch)` for begin/stay/end EVENTS; keys pairs by BODY slot (`PairKey(a,b)`), calls `Touch(w,a,b,stamp)` → `w.SlotsOverlap(a,b)`. Body-pair keyed; knows nothing about fixtures. (Deleted in a later phase; here it must keep working with body-pairs.)
- **`SlotsOverlap(a,b)`**, **`StaticCandidates`**, **`m_staticGrid`** stay BODY-level — statics are not re-keyed here (the whisk is a mover; per-fixture statics are out of scope, see Non-Goals).

## File Structure

- Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` — add `m_fixtureBroadphase`, `UpdateMoverProxies` decl, fixture-proxy lifecycle; (2b) move-buffer/pair-set members + accessor; (Task 3) remove `m_moverBroadphase`.
- Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — `UpdateMoverProxies` impl + wire all move/add/drop sites; restructure `GenerateContacts` mover loop to fixture-pairs.
- Modify `Arcane/Core/src/Arcane/Physics/ContactManager.cpp/.hpp` — map fixture-pairs → body-pairs for the event pass.
- Modify `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp/.cpp` — (2b) dirty-proxy/move-buffer tracking. (`SpatialHash`/`SweepAndPrune` need a matching move-buffer seam only if kept as incremental oracles — see Task 4.)
- Create `Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp` — the brute-force fixture-pair oracle (2a) + incremental==full==brute-force oracle (2b).

## Non-Goals (explicit)

- **Statics stay body-level** on `m_staticGrid`; per-fixture static proxies are not needed for the whisk fix.
- **Persistent contacts** (manifold-once-per-step, warm-start-on-contact) are **Phase 3** — here `GenerateContacts` still rebuilds `m_contactConstraints` each step; 2b's persistent *pair set* is a precursor, not full contact persistence.
- **Events-as-byproduct / deleting `ContactManager::Step`** is **Phase 4** — here it keeps running, fed body-pairs derived from the fixture broadphase.
- **`Residents()` durable handles** — deferred until the combat-sphere consumer exists (no consumer here ⇒ speculative API).
- **Retiring `SpatialHash`/`SweepAndPrune`/`Baumgarte`/Lua goldens** — **Phase 5**; here `SpatialHash`/`SAP` are KEPT as the cross-strategy equivalence oracle.

---

# PHASE 2a — per-fixture proxies (the whisk fix)

### Task 1: Per-fixture broadphase + `UpdateMoverProxies`, built alongside (oracle-gated, not yet consumed)

**Goal:** stand up a per-fixture `DynamicTree` index that the oracle proves correct, while the body-level path still drives the suite (stays green).

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (members + `UpdateMoverProxies` decl + `FixtureBroadphase()` accessor)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`UpdateMoverProxies` impl; wire AddBody/AddFixture/DropFixture/RemoveBody/SetPosition/MovePosition/Step-stage-1/BulletSweep/CommitSlotPosition)
- Create: `Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp`

- [ ] **Step 1: Write the failing oracle test**

Create `Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp`:

```cpp
// Per-fixture broadphase oracle: the fixture-pair candidate set the world's
// per-fixture broadphase produces must EQUAL the brute-force O(n^2) set over all
// live fixture world-AABBs (a<b by fixture slot). Compound bodies (the whisk
// case) are the point: each fixture is its own proxy. Determinism + correctness
// gate for the body->fixture re-key.
#include <algorithm>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // BroadphasePair, AabbOverlap

using namespace Arcane::Physics;

namespace
{
    // Brute-force fixture-pair set: every pair of live fixtures whose world AABBs
    // overlap, keyed (lo,hi) by fixture slot, sorted. Uses the world's public
    // per-fixture AABB accessor (added in Step 3).
    std::vector<BroadphasePair> BruteFixturePairs(const PhysicsWorld& w)
    {
        std::vector<std::uint32_t> fx; std::vector<Aabb2> box;
        w.LiveFixtureAabbs(fx, box); // parallel arrays of (slot, world-AABB)
        std::vector<BroadphasePair> out;
        for (std::size_t i = 0; i < fx.size(); ++i)
            for (std::size_t j = i + 1; j < fx.size(); ++j)
                if (AabbOverlap(box[i], box[j]))
                {
                    std::uint32_t a = fx[i], b = fx[j];
                    out.push_back(a < b ? BroadphasePair{a, b} : BroadphasePair{b, a});
                }
        std::sort(out.begin(), out.end());
        return out;
    }
}

TEST_CASE("Per-fixture broadphase pairs == brute-force (compound scene)", "[physics][fxbroadphase]")
{
    PhysicsWorld w;
    // A 3-fixture compound dynamic body + several singles, deliberately overlapping.
    auto addBox = [&](Real x, Real y, Real hw, Real hh, BodyType t) {
        BodyDef d; d.type = t; d.position = Vec2(x, y); d.fixedRotation = true;
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };
    BodyHandle c = addBox(0, 0, 10, 10, BodyType::Dynamic);
    // Two extra fixtures on `c` offset along +x so it spans several others.
    FixtureDef f; f.shape = MakeAabb(Real(10), Real(10));
    f.localPos = Vec2(40, 0);  w.AddFixture(c, f);
    f.localPos = Vec2(80, 0);  w.AddFixture(c, f);
    addBox(35,  0, 12, 12, BodyType::Dynamic);
    addBox(82,  0, 12, 12, BodyType::Dynamic);
    addBox(400, 0, 12, 12, BodyType::Dynamic); // far away -> no pairs
    addBox(40, -3, 14, 14, BodyType::Static);  // static fixtures also live in the tree? NO -- movers only

    w.Step(Real(1) / Real(60)); // commit positions -> proxies refreshed

    std::vector<BroadphasePair> got;
    w.FixtureBroadphase().Pairs(got);          // mover fixture proxies only
    std::sort(got.begin(), got.end());

    // Brute force over the SAME proxy set (movers' live fixtures).
    REQUIRE(got == BruteFixturePairs(w));
}
```

NOTE: `LiveFixtureAabbs` must enumerate the SAME set the per-fixture broadphase indexes — i.e. **mover (Dynamic/Kinematic) fixtures only**, not statics (statics live on `m_staticGrid`, not the fixture tree). Implement it that way in Step 3 (skip a fixture whose owning body is Static).

- [ ] **Step 2: Regen both workspaces + build + verify compile fail**

Run the Arcane regen (new test file), build Debug, `./ArcaneTests.exe "[fxbroadphase]"`.
Expected: COMPILE FAIL — `FixtureBroadphase()` / `LiveFixtureAabbs` don't exist yet.

- [ ] **Step 3: Add the per-fixture broadphase + accessors + `UpdateMoverProxies` (PhysicsWorld.hpp/.cpp)**

In `PhysicsWorld.hpp`, near `m_moverBroadphase`:

```cpp
            // Per-FIXTURE mover broadphase (collision-rebuild Phase 2): one proxy
            // per live fixture of a Dynamic/Kinematic body, keyed by fixture slot.
            // Built alongside m_moverBroadphase during the transition; replaces it
            // (Task 3). Same WorldDef::broadphase strategy selection.
            std::unique_ptr<IBroadphase> m_fixtureBroadphase;
```

Public accessors (near `MoverBroadphase()`):

```cpp
            [[nodiscard]] const IBroadphase& FixtureBroadphase() const noexcept
            { return *m_fixtureBroadphase; }

            // Test/oracle helper: parallel arrays of (live mover fixture slot,
            // world AABB) -- the exact set m_fixtureBroadphase indexes.
            void LiveFixtureAabbs(std::vector<std::uint32_t>& fxOut,
                                  std::vector<Aabb2>& boxOut) const;
```

Private decl:

```cpp
            // Refresh every live fixture proxy of body b in the fixture broadphase
            // + the body's residency. One seam for all the per-step move sites
            // (folds in the Phase-1 deferred UpdateMoverProxies). DURING THE 2a
            // TRANSITION it also refreshes the body-level m_moverBroadphase proxy;
            // that line is removed in Task 3.
            void UpdateMoverProxies(std::uint32_t b);
            // Register/unregister a single fixture proxy (mover bodies only).
            void AddFixtureProxy(std::uint32_t fi);
            void RemoveFixtureProxy(std::uint32_t fi);
            // World AABB of one fixture (ComposeFixtureXf + ComputeAABB).
            [[nodiscard]] Aabb2 FixtureAabb(std::uint32_t fi) const noexcept;
```

In `PhysicsWorld.cpp`, construct `m_fixtureBroadphase` in the ctor init list next to `m_moverBroadphase`: `, m_fixtureBroadphase(MakeBroadphase(def))`.

Implement:

```cpp
        Aabb2 PhysicsWorld::FixtureAabb(std::uint32_t fi) const noexcept
        {
            const std::uint32_t b = m_fxBody[fi];
            const Transform xf = ComposeFixtureXf(
                Vec2(m_posX[b], m_posY[b]), m_angle[b],
                Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]), m_fxLocalAngle[fi]);
            return m_fxShape[fi].ComputeAABB(xf);
        }

        void PhysicsWorld::AddFixtureProxy(std::uint32_t fi)
        {
            const std::uint32_t b = m_fxBody[fi];
            if (static_cast<BodyType>(m_btype[b]) == BodyType::Static) return; // statics -> m_staticGrid
            m_fixtureBroadphase->Update(fi, FixtureAabb(fi));
        }

        void PhysicsWorld::RemoveFixtureProxy(std::uint32_t fi)
        {
            m_fixtureBroadphase->Remove(fi); // no-op if absent (e.g. a static fixture never registered)
        }

        void PhysicsWorld::UpdateMoverProxies(std::uint32_t b)
        {
            const Aabb2 bodyBox = SlotAabb(b);
            m_moverBroadphase->Update(b, bodyBox); // BODY-LEVEL (transition; removed Task 3)
            m_residencyGrid.Move(b, bodyBox);      // body residency (unchanged granularity)
            const Vec2 pos(m_posX[b], m_posY[b]);
            const Real ang = m_angle[b];
            for (const std::uint32_t fi : m_bodyFixtures[b])
            {
                if (fi >= m_fxCount || m_fxGen[fi] == 0u) continue;
                const Transform xf = ComposeFixtureXf(
                    pos, ang, Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]), m_fxLocalAngle[fi]);
                m_fixtureBroadphase->Update(fi, m_fxShape[fi].ComputeAABB(xf));
            }
        }

        void PhysicsWorld::LiveFixtureAabbs(std::vector<std::uint32_t>& fxOut,
                                            std::vector<Aabb2>& boxOut) const
        {
            fxOut.clear(); boxOut.clear();
            for (std::uint32_t fi = 0; fi < m_fxCount; ++fi)
            {
                if (m_fxGen[fi] == 0u) continue;
                const std::uint32_t b = m_fxBody[fi];
                if (b >= m_count || m_alive[b] == 0u) continue;
                if (static_cast<BodyType>(m_btype[b]) == BodyType::Static) continue;
                fxOut.push_back(fi);
                boxOut.push_back(FixtureAabb(fi));
            }
        }
```

**Wire the lifecycle (replace the existing per-site `m_moverBroadphase->Update(i, box); m_residencyGrid.Move(i, box);` PAIRS with `UpdateMoverProxies(i);`):**
- `SetPosition`, `MovePosition`, `Step` stage-1 kinematic, `BulletSweep`, `CommitSlotPosition` (inline hpp): the Phase-1 cached pattern `const Aabb2 moverBox = SlotAabb(i); m_moverBroadphase->Update(i, moverBox); m_residencyGrid.Move(i, moverBox);` becomes `UpdateMoverProxies(i);`. (CommitSlotPosition is inline in the header — `UpdateMoverProxies` is a member so it is callable there.)
- `AddBody` mover branch: it currently does the cached pattern + `m_residencyGrid.Insert(idx, moverBox)`. Keep the `m_moverBroadphase->Update` + residency Insert as-is here (the body has NO fixtures yet at this point — the back-compat fixture is created later in AddBody). The fixture proxy is registered by the back-compat fixture creation (next bullet).
- **Back-compat fixture creation in `AddBody`** (the inline block ~795 that fills `m_bodyFixtures[idx]` with the primary fixture): after the fixture slot is populated, call `AddFixtureProxy(thatFixtureSlot)`.
- **`AddFixture`** (after `AllocFixtureSlot` + `RecomputeBodyMass`): add `AddFixtureProxy(fi);` (registers the new fixture's proxy for a mover; no-op for a static).
- **`DropFixture`** (after the swap-pop unlink, before/after `RecomputeBodyMass`): add `RemoveFixtureProxy(fi);` (the dropped fixture slot — call BEFORE bumping `m_fxGen`/clearing, or capture fi first; `Remove` just needs the id).
- **`RemoveBody`** mover branch (currently `m_moverBroadphase->Remove(idx); m_residencyGrid.Remove(idx);`): ALSO remove each fixture proxy. Since `RemoveBody` later iterates `m_bodyFixtures[idx]` to recycle fixtures (~910), add `RemoveFixtureProxy(fi)` inside that existing fixture-recycle loop (before bumping `m_fxGen[fi]`).

- [ ] **Step 4: Regen + build Debug + run the oracle test → pass**

Run: build Debug; `./ArcaneTests.exe "[fxbroadphase]"`. Expected: PASS (per-fixture pairs == brute-force).

- [ ] **Step 5: Full `[physics]` suite stays green (old path still drives it)**

Run: `./ArcaneTests.exe "[physics]"`. Expected: ALL pass, unchanged count + the new case. The fixture broadphase is built + validated but NOT yet consumed by GenerateContacts/ContactManager, so behavior is identical.

- [ ] **Step 6: ArcaneCore static-CRT (Debug) + commit**

Build ArcaneCore Debug (regen Server first — new test file doesn't touch Core, but the new Core members do; ArcaneCore globs Core sources so a Server regen is only needed if NEW Core FILES were added — none here, only edits — so no Server regen needed; just rebuild ArcaneCore). Expect clean.

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp
git commit -m "feat(arcane/physics): per-fixture mover broadphase + UpdateMoverProxies (oracle-gated, not yet consumed)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Switch `GenerateContacts` + `ContactManager` to fixture-pairs (the behavior change — whisk fix lands)

**Goal:** consume the per-fixture broadphase's fixture-pairs. The fixture-pair iteration machinery (Collide + emit + MixContactId) is ALREADY fixture-aware; we replace the body-pair outer loop + nested fixture loop with a direct fixture-pair walk.

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`GenerateContacts` mover-mover section, ~1793-1956)
- Modify: `Arcane/Core/src/Arcane/Physics/ContactManager.cpp` (the `Pairs()` consumption ~124-135)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (a `BodyOfFixture` accessor for ContactManager)

THE ORACLE TEST (Task 1) IS THE BEHAVIORAL SPEC for the broadphase set. The full `[physics]` suite is the behavioral spec for the contact result. There is no new test code here — the gate is "full suite green + perf improves."

- [ ] **Step 1: Add a `BodyOfFixture` seam for ContactManager**

In `PhysicsWorld.hpp` (public):

```cpp
            // Owning body slot of a fixture slot (ContactManager maps fixture-pairs
            // -> body-pairs for the event machine). No bounds beyond fixture count.
            [[nodiscard]] std::uint32_t BodyOfFixture(std::uint32_t fi) const noexcept
            { return m_fxBody[fi]; }
```

- [ ] **Step 2: Restructure the `GenerateContacts` mover-mover loop to fixture-pairs**

Replace `m_moverBroadphase->Pairs(m_genPairs)` with `m_fixtureBroadphase->Pairs(m_genPairs)`. The pairs are now FIXTURE slots `(fa, fb)` (a<b by fixture slot). Replace the body-pair processing + nested fixture loop with, per fixture-pair `(fa, fb)`:

```
ia = m_fxBody[fa]; ib = m_fxBody[fb];
if (ia == ib) continue;                         // two fixtures of the SAME body never collide
if (!alive[ia] || !alive[ib]) continue;
if (sensor pair / filter) handle as today (sensors -> events only, no constraint);
da = isDynamic(ia); db = isDynamic(ib);
if (!da && !db) continue;                        // kinematic-kinematic: no dynamic response
// ORIENT so A is dynamic; if both dynamic, lower BODY slot is A. Swap (ia,fa)<->(ib,fb) together.
if (!da || (da && db && ib < ia)) { swap(ia,ib); swap(fa,fb); }
wake sleeping dynamics (as today);
if (m_awake[ia] == 0) continue;
xfA = FixtureWorldXf(ia, fa); xfB = FixtureWorldXf(ib, fb);
pairMargin = max(kSkin, sqrt(max(speedSqA, speedSqB)) * dt);   // same speculative margin as today
Manifold m = Collide(m_fxShape[fa], xfA, m_fxShape[fb], xfB, pairMargin);
emit(ia, ib, /*bIsBody=*/true, centerB(ib), m,
     m_fxFriction[fa], m_fxFriction[fb], m_fxRestitution[fa], m_fxRestitution[fb], fa, fb);
```

Key points the implementer MUST preserve (verify against the existing `emit` + the map):
- The **A=dynamic / lower-body-slot orientation** must be applied to BOTH the body indices AND the fixture indices TOGETHER, so `emit(..., fa, fb)` gets fixtures in the same A/B order every step ⇒ `MixContactId(base, fa, fb)` warm-start key is stable. (This is the #1 warm-start-continuity risk.)
- The per-body **broadphase AABB cull** (`m_genBodyAabb[a]` expand + `AabbOverlap`) and the per-fixture **`fxDisjoint`** reject are now REDUNDANT for the mover path (the per-fixture broadphase already returns only fat-overlapping fixture pairs) — DROP them from the mover loop. (`m_genBodyAabb`/`m_genFxAabb` are STILL built + STILL used by the STATIC path — do not remove the build loop or the static-path usage.)
- **Sensors / collision filter:** keep the existing sensor + category/mask handling (a sensor or filtered pair produces events, not a constraint). Apply the same checks per fixture-pair (`m_fxSensor[fa]`/`[fb]`, `m_fxFilterCat/Mask`).
- The **STATIC path** (per dynamic body → `StaticCandidates` → its nested fixture loop) is UNCHANGED — it is body-driven and keeps `m_genBodyAabb`/`m_genFxAabb`/`fxDisjoint`.
- The legacy single-shape fallback (a body with no fixtures) is now unreachable for movers (every mover body has ≥1 fixture proxy) — you may keep a defensive assert but the live path is fixture-pairs.

- [ ] **Step 3: Switch `ContactManager::Step` to map fixture-pairs → body-pairs**

In `ContactManager.cpp`, the candidate-pair source `w.MoverBroadphase().Pairs(m_pairScratch)` becomes `w.FixtureBroadphase().Pairs(m_pairScratch)`. The pairs are now fixture slots; the event machine needs BODY pairs. Map + dedup before `Touch`:

```cpp
            w.FixtureBroadphase().Pairs(m_pairScratch);
            // Fixture-pairs -> unique body-pairs (a<b, same body's two fixtures dropped).
            m_bodyPairScratch.clear();
            for (const BroadphasePair& fp : m_pairScratch)
            {
                std::uint32_t a = w.BodyOfFixture(fp.a), b = w.BodyOfFixture(fp.b);
                if (a == b) continue;
                if (a > b) std::swap(a, b);
                m_bodyPairScratch.push_back(BroadphasePair{a, b});
            }
            std::sort(m_bodyPairScratch.begin(), m_bodyPairScratch.end());
            m_bodyPairScratch.erase(std::unique(m_bodyPairScratch.begin(), m_bodyPairScratch.end()),
                                    m_bodyPairScratch.end());
            // ...then iterate m_bodyPairScratch calling Touch(w, bp.a, bp.b, stamp) (as before).
```

Add `std::vector<BroadphasePair> m_bodyPairScratch;` to `ContactManager.hpp` next to `m_pairScratch`. Touch/SlotsOverlap/PairKey/events logic is UNCHANGED (still body-keyed). `MoverBroadphase()` is still used by nothing else after this; leave its accessor (removed when `m_moverBroadphase` is in Task 3).

- [ ] **Step 4: Build Debug + full `[physics]` suite — behavior preserved**

Run: build Debug; `./ArcaneTests.exe "[physics]"`. Expected: ALL pass. The contact RESULT is behavior-preserving (same fixture-pairs reach Collide; only the discovery path changed). If a POSITION golden trips ONLY because the contact/candidate ORDER changed (fixture-slot order vs old body+nested order), re-baseline it DELIBERATELY and report old/new + why it is ordering-only. A behavioral failure (stack stops settling, body tunnels, joint slips, energy explodes, ActiveContactCount changes meaningfully) is a REAL regression — STOP and report, do NOT re-baseline.

- [ ] **Step 5: Perf smoke — the whisk fix should appear**

Build Release; `cd "Arcane/bin/Release-windows-x86_64-md/Loom"`; let scene 8 reach the dense steady state (needs ~7s+ wall — use a large frame count, e.g. `ARCANE_SANDBOX_SCENE=8 ./Loom.exe --frames 15000 --no-vsync --perf 2>&1 | grep PERF | tail -5`). Expected: dense-steady-state `sim` ms MATERIALLY below the Phase-0+1 baseline (~99 ms/frame) — the per-fixture proxies eliminate the whisk's provably-disjoint narrowphase. Record the number.

- [ ] **Step 6: ArcaneCore static-CRT (Debug) + commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/ContactManager.cpp Arcane/Core/src/Arcane/Physics/ContactManager.hpp
git commit -m "perf(arcane/physics): GenerateContacts + events consume per-fixture broadphase pairs

The 37-fixture whisk's proxies are now per-shape, so the broadphase no longer
produces the body-union pairs that brute-forced every fixture against every body
(the ~89% provably-disjoint narrowphase waste). Mover-mover contacts walk
fixture-pairs directly; ContactManager maps them to body-pairs for events.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Retire the body-level `m_moverBroadphase`

**Goal:** remove the now-dead body-level broadphase (no consumer after Task 2).

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp/.cpp`, `Arcane/Core/src/Arcane/Physics/ContactManager.*` (remove the dead `MoverBroadphase()` accessor if unused).

- [ ] **Step 1: Confirm no remaining consumer**

`grep -rn "m_moverBroadphase\|MoverBroadphase" Arcane/Core/src Arcane/Tests`. The only writes should be in `UpdateMoverProxies` / `AddBody` / `RemoveBody` / (the move sites already route through `UpdateMoverProxies`). Confirm `Pairs()` on it is no longer read (both consumers switched in Task 2). If `PhysicsInvariantsTest` "broadphase strategy does not change the result" reads `WorldDef::broadphase`, that selection now flows to `m_fixtureBroadphase` (both are `MakeBroadphase(def)`) — the test still exercises strategy-equivalence on the fixture broadphase. Confirm `PhysicsBroadphaseTest` constructs standalone strategy instances (independent of the world) and is unaffected.

- [ ] **Step 2: Remove `m_moverBroadphase`**

Delete the member, its ctor init, the `m_moverBroadphase->Update(b, bodyBox)` line inside `UpdateMoverProxies`, the `AddBody`/`RemoveBody` body-level Update/Remove, and the `MoverBroadphase()` accessor (and any now-dead `m_genPairs` body-level usage that moved to the fixture path). Keep `m_residencyGrid` updates. `m_fixtureBroadphase` is now the sole mover broadphase.

- [ ] **Step 3: Build Debug + full `[physics]` suite green**

Run: `./ArcaneTests.exe "[physics]"`. Expected: ALL pass (pure removal of a dead path).

- [ ] **Step 4: ArcaneCore static-CRT (Debug) + commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/ContactManager.cpp Arcane/Core/src/Arcane/Physics/ContactManager.hpp
git commit -m "refactor(arcane/physics): retire the dead body-level mover broadphase

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

# PHASE 2b — move buffer + incremental pair set

> **Design note (read first):** the spec (§5) says the move buffer produces NEW pairs and "pair destruction is owned by the contact pipeline." Full contact persistence is Phase 3, so here the broadphase (or a thin pair-manager on the world) maintains a **persistent pair set** itself: each step it processes the move buffer to ADD new fat-overlap pairs for moved proxies AND PRUNE pairs that separated (a moved proxy's former partner whose fat boxes no longer overlap). `Pairs()` returns this incrementally-maintained set. `GenerateContacts` + `ContactManager` keep rebuilding from the full set (Phase 3 later moves the set into the contact layer and the broadphase reverts to add-only). The oracle gate: `incremental set == full-traversal set == brute-force`, every step, across randomized add/remove/move sequences.

### Task 4: Move buffer + incremental pair set in `DynamicTree` (oracle-gated, not yet consumed)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp/.cpp`
- Modify: `Arcane/Core/src/Arcane/Physics/Broadphase/Broadphase.hpp` (extend `IBroadphase` with an OPTIONAL incremental API — default no-op so `SpatialHash`/`SAP` need no change)
- Test: extend `Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp`

- [ ] **Step 1: Write the failing incremental-equivalence oracle**

Append to `PhysicsFixtureBroadphaseTest.cpp`:

```cpp
#include <random>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>

// The incrementally-maintained pair set (move buffer) must equal a full Pairs()
// recompute AND the brute-force O(n^2) set, after EVERY mutation, across a long
// randomized sequence of inserts / moves / removes. Guards the riskiest new code.
TEST_CASE("DynamicTree incremental pairs == full == brute-force", "[physics][fxbroadphase][movebuffer]")
{
    std::mt19937 rng(0xBADC0DE);
    std::uniform_real_distribution<float> pos(-300.f, 300.f), ext(4.f, 40.f);
    DynamicTree tree;
    std::vector<std::pair<std::uint32_t, Aabb2>> live; // id -> tight box

    auto boxAt = [&](float x, float y, float w, float h) {
        Aabb2 a; a.min = Vec2(x, y); a.max = Vec2(x + w, y + h); return a; };
    auto brute = [&]() {
        std::vector<BroadphasePair> out;
        for (std::size_t i = 0; i < live.size(); ++i)
            for (std::size_t j = i + 1; j < live.size(); ++j)
                if (AabbOverlap(live[i].second, live[j].second)) {
                    std::uint32_t a = live[i].first, b = live[j].first;
                    out.push_back(a < b ? BroadphasePair{a,b} : BroadphasePair{b,a}); }
        std::sort(out.begin(), out.end()); return out; };

    std::uint32_t nextId = 0;
    for (int iter = 0; iter < 600; ++iter)
    {
        const int op = rng() % 3;
        if (op == 0 || live.size() < 4) {                 // insert
            Aabb2 b = boxAt(pos(rng), pos(rng), ext(rng), ext(rng));
            std::uint32_t id = nextId++; tree.Update(id, b); live.push_back({id, b});
        } else if (op == 1) {                              // move
            auto& e = live[rng() % live.size()];
            e.second = boxAt(pos(rng), pos(rng), ext(rng), ext(rng)); tree.Update(e.first, e.second);
        } else {                                           // remove
            std::size_t k = rng() % live.size();
            tree.Remove(live[k].first); live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
        }

        // Drive the move buffer to update the incremental set, then compare.
        std::vector<BroadphasePair> incr; tree.UpdatePairs(incr);   // incremental maintained set
        std::vector<BroadphasePair> full; tree.Pairs(full);          // full traversal
        std::sort(incr.begin(), incr.end()); std::sort(full.begin(), full.end());
        const std::vector<BroadphasePair> bf = brute();
        REQUIRE(incr == bf);
        REQUIRE(full == bf);
    }
}
```

- [ ] **Step 2: Build + verify compile fail** (`UpdatePairs` undefined). Run `./ArcaneTests.exe "[movebuffer]"`.

- [ ] **Step 3: Add the move buffer + incremental pair set to `DynamicTree`**

In `Broadphase.hpp`, extend `IBroadphase` with an optional incremental seam (default = full recompute, so `SpatialHash`/`SAP` inherit correct-but-non-incremental behavior and the equivalence oracle still holds):

```cpp
    // Incremental pair maintenance. Default: full recompute (Pairs). DynamicTree
    // overrides with a move-buffer + persistent pair set. Returns the CURRENT full
    // overlap set (maintained, not necessarily recomputed). Determinism: output
    // sorted (a,b); identical to Pairs() for the same proxy state.
    virtual int UpdatePairs(std::vector<BroadphasePair>& out)
    { return Pairs(out); }
```

In `DynamicTree`:
- Add a **move buffer**: in `Update(id, box)`, when a proxy is re-inserted (its tight box left the fat box → the existing reinsert branch), push `id` to `m_moved` (dedup via a generation-stamped `m_movedStamp` vector or an in-buffer flag). A coherent-motion fast-path update (still inside fat box) does NOT touch the move buffer. `Remove(id)` also records the removal (its former pairs must be pruned).
- Add a **persistent pair set** `m_pairSet` (a sorted `vector<BroadphasePair>` or a hash set keyed by `(uint64)a<<32|b`). `UpdatePairs(out)`:
  - For each moved proxy `id` (and each removed proxy), PRUNE every pair in `m_pairSet` touching `id` whose endpoints no longer fat-overlap (or whose endpoint was removed).
  - For each moved proxy `id`, query the tree (fat descent) for proxies overlapping `id`'s fat box → for each `other` with `other != id` and tight-AABB overlap, ADD the canonical `(lo,hi)` pair to `m_pairSet` (dedup).
  - Emit `m_pairSet` sorted into `out`.
- **Determinism:** process `m_moved` in sorted (ascending id) order; the pruned/added sets are order-independent because `m_pairSet` is finally emitted sorted. Document this.
- Keep the existing `Pairs()` (full traversal) intact — the oracle compares against it, and Task 5 may keep it for `QueryAABB`/debug.

NOTE the pruning subtlety: a pair `(a,b)` separates if EITHER endpoint moved such that their fat boxes no longer overlap. Processing each moved id's existing pairs covers this (if only `a` moved, `(a,b)` is re-checked via `a`'s pair list; if both moved, it's covered by either). Use the tight-box `AabbOverlap` for the final membership test (a pair is in the set iff tight boxes overlap), consistent with `Pairs()` which emits on tight overlap. (Fat boxes drive descent/candidacy; tight boxes drive membership — same as `Pairs()`.)

- [ ] **Step 4: Build Debug + run the oracle → pass**

Run: `./ArcaneTests.exe "[movebuffer]"`. Expected: PASS (incremental == full == brute-force across 600 randomized mutations). Then `./ArcaneTests.exe "[physics]"` — still green (UpdatePairs not yet consumed).

- [ ] **Step 5: ArcaneCore static-CRT (Debug) + commit**

```bash
git add Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.cpp Arcane/Core/src/Arcane/Physics/Broadphase/Broadphase.hpp Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp
git commit -m "feat(arcane/physics): DynamicTree move buffer + incremental pair set (oracle-gated, not yet consumed)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Switch the consumers to the incremental pair set

**Files:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (GenerateContacts), `Arcane/Core/src/Arcane/Physics/ContactManager.cpp`.

- [ ] **Step 1: Route both consumers through `UpdatePairs`**

GenerateContacts (and ContactManager::Step) call `m_fixtureBroadphase->Pairs(...)` today. The move buffer must be drained ONCE per step (not twice). Drain it in GenerateContacts (the first consumer in the step): `m_fixtureBroadphase->UpdatePairs(m_genPairs)`. ContactManager::Step runs LATER in the same step (stage 6, after solve/CCD) — proxies may have moved again via `CommitSlotPosition`/`BulletSweep`, so it must ALSO see a current set: call `UpdatePairs(m_pairScratch)` there too (it drains any moves since GenerateContacts, then returns the full current set). Confirm draining twice in one step is correct (the second drain processes only the proxies moved by the solver/CCD between the two calls) and the oracle's per-mutation invariant guarantees correctness after each drain.

- [ ] **Step 2: Build Debug + full `[physics]` suite green**

Run: `./ArcaneTests.exe "[physics]"`. Expected: ALL pass (same pair set, maintained incrementally; behavior-identical). Re-baseline only a pure-ordering golden shift; behavioral failure = regression.

- [ ] **Step 3: Perf re-measure (mostly-resting + churn)**

Build Release. The move buffer's win shows on MOSTLY-RESTING scenes (few proxies move/step). Scene 8 is never-settling churn (most proxies move every step) so the move-buffer delta there is small — that's expected. Record scene-8 dense-steady-state `sim` (should be ≈ Task 2's number, not worse). If available, note any settled-scene improvement.

- [ ] **Step 4: ArcaneCore static-CRT (Debug) + commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/ContactManager.cpp
git commit -m "perf(arcane/physics): broadphase pair discovery via the incremental move buffer

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Phase 2 full gate + perf re-measure + memory

**Files:** none (verification only).

- [ ] **Step 1: Full ArcaneTests Debug + Release** (no filter, [gpu] both backends, from each exe dir). Expected: `All tests passed` both configs (≥ the Phase-0+1 baseline 76350/369 + the new fxbroadphase/movebuffer cases).
- [ ] **Step 2: ArcaneCore static-CRT Debug + Release** (`ArcaneCore.vcxproj` both configs). Expected: `ArcaneCore.lib`, no errors.
- [ ] **Step 3: Perf re-measure (the headline number).** Release, scene 8 dense steady state (`--frames 15000 --no-vsync --perf`, tail the steady-state lines). Compare `sim` ms to the Phase-0+1 baseline (~99 ms/frame / ~10 FPS). Expect a MATERIAL drop (the whisk fix). Record the before/after for the memory + the Phase-3 baseline.
- [ ] **Step 4: Update the memory pointer.** Update `project_arcane_collision_rebuild_phase01` (or add a `project_arcane_collision_rebuild_phase2` memory + MEMORY.md line): Phase 2 landed (per-fixture broadphase + move buffer), the whisk-fix perf delta, gate green; deferred `Residents()` handle still open; next = Phase 3 (persistent contacts: move the pair set into a persistent `Contact` array, manifold-once-per-step, warm-start-on-contact).

---

## Self-Review Notes (addressed)

- **Spec coverage:** §5 (per-fixture proxies + fat AABBs [reused] + move buffer + incremental pairs) = Tasks 1-5. §7 contact pipeline: the fixture-pair Collide/emit/MixContactId path is reused (Task 2); full persistent contacts are explicitly Phase 3 (Non-Goals). §8 testing: brute-force fixture-pair oracle (Task 1) + incremental==full==brute-force oracle (Task 4) + the existing cross-strategy equivalence tests (kept). §10 item 2 = this plan; the whisk fix lands at Task 2.
- **Deferred items:** `UpdateMoverProxies` folded in at Task 1 (the re-key makes it the natural seam). `Residents()` durable handle stays deferred (Non-Goals — no consumer).
- **Determinism:** fixture-pair orientation (A=dynamic/lower-body-slot, fixtures swapped together) keeps `MixContactId` warm-start keys stable (Task 2 Step 2, flagged as the #1 risk). Move buffer processed in sorted id order; `m_pairSet` emitted sorted (Task 4 Step 3). Position goldens re-baselineable ONLY for pure ordering shifts; behavioral failures are regressions.
- **Gate-green increments:** new structures built ALONGSIDE + oracle-validated BEFORE the consumer switch (Tasks 1→2, 4→5), so the suite is green at every commit; the body-level path is removed only once dead (Task 3).
- **Type consistency:** `m_fixtureBroadphase`/`FixtureBroadphase()`/`UpdateMoverProxies`/`AddFixtureProxy`/`RemoveFixtureProxy`/`FixtureAabb`/`LiveFixtureAabbs`/`BodyOfFixture`/`UpdatePairs`/`m_bodyPairScratch` are referenced consistently across Tasks 1-5; `BroadphasePair`/`AabbOverlap`/`ComposeFixtureXf`/`MakeAabb`/`MakeCircle`/`FixtureDef.localPos`/`m_fxBody`/`m_fxGen`/`m_fxCount` are existing symbols (verified in the map).
- **Risk callouts for the executor:** (1) ContactManager silently treating fixture ids as body ids — mitigated by the explicit map+dedup (Task 2 Step 3). (2) Warm-start key permutation — mitigated by paired A/B orientation (Task 2 Step 2). (3) Move-buffer pruning correctness — mitigated by the 600-mutation incremental==brute-force oracle (Task 4). (4) AddBody registers the fixture proxy AFTER the back-compat fixture exists, not at the body-broadphase line (Task 1 Step 3).
