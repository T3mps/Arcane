# Island contact linkage + SplitIsland rewrite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `PhysicsWorld::SplitIsland`'s O(poolSize x islandSize) whole-pool scan with a Box2D-v3-faithful per-body contact adjacency so a split walks only an island's own contacts — O(islandBodies + islandEdges) — with byte-identical island output.

**Architecture:** Add a per-body contact-id adjacency `m_bodyContacts` (a `vector<vector<uint32_t>>` keyed by body slot, mirroring the existing `m_bodyFixtures`), holding each dynamic body's dyn-dyn body contacts. Maintain it at exactly two cold-ish points — contact create and destroy — so merge/split/sleep/wake never touch it (a contact's body slots are stable; only its island changes). Rewrite `SplitIsland` to union members via that adjacency, using a reused per-slot scratch `m_splitLocalIndex` for O(1) member lookup, keeping the DSU and the component->island-id assignment loop byte-for-byte.

**Tech Stack:** C++23, Arcane Core physics (`Arcane/Core/src/Arcane/Physics`), Catch2 tests (`Arcane/Tests/src`), MSVC (VS2026), premake5.

**Spec:** `docs/superpowers/specs/2026-06-29-arcane-physics-island-split-linkage-design.md`

## Global Constraints

- **Byte-identical island result is the contract.** The full `[physics]` suite (esp. `[physics][island]`, `PhysicsDeterminismTest`, the `[physics][mt]` guards) must stay green with **no re-baseline**. The split's connected-component grouping and island-id assignment must be bit-identical to the current code.
- **/MD dynamic CRT**, C++23, UTF-8 no BOM, ASCII comments only (no em-dashes), no `/fp:fast`. Presentation-free Core (glm + std + sibling Physics headers only).
- **clangd diagnostics in this workspace are FALSE POSITIVES.** MSVC + ArcaneTests are the truth.
- **Stage ONLY the per-task files by explicit path. NEVER `git add -A`.** The working tree carries unrelated uncommitted Loom-refactor changes (`Client/data/ui_screens/*`, `AGENTS.md`, `Arcane/.screenshots/`, `Server/cpp_coding_style.txt`, two 2026-06-24 docs) that must stay untouched.
- **Build:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m` (run via PowerShell, NOT Git Bash — Git Bash mangles the `/p:` switch).
- **Premake (only when files are added/removed):** `ThirdParty\premake5\premake5.exe vs2026` run from `Arcane\` (the `.bat` hangs on `pause`; the premake exe is at repo-root `ThirdParty/premake5/premake5.exe`).
- **Tests from the exe dir:** `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[physics]"`.
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux
  ```
- **Branch:** `feature/arcane-physics-island-split-linkage` (already created, stacked on the G2 create-phase tip).

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | Modify | Declare `m_bodyContacts`, `m_splitLocalIndex`, `kSplitLocalNone`; declare helpers `DetachContactAdjacency`, `ReleaseAndDestroyContact`, `DebugValidateBodyContacts`. |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | Modify | Size + reset the adjacency; attach at create; centralize destroy; rewrite `SplitIsland`; implement helpers + validator. |
| `Arcane/Tests/src/PhysicsBodyContactsTest.cpp` | Create | Task 1 adjacency-invariant test (`[physics][island]`). |
| `Arcane/Tests/src/PhysicsPersistentIslandTest.cpp` | Modify | Task 2 multi-component fracture equivalence test. |

Both new files are picked up by the `Arcane/Tests/src/**.cpp` glob — adding `PhysicsBodyContactsTest.cpp` requires a premake regen (Task 1, Step 5).

---

## Task 1: Per-body contact adjacency structure + maintenance

Adds `m_bodyContacts` and keeps it consistent at contact create/destroy/recycle. `SplitIsland` is **not** touched in this task — the adjacency is write-only here, so engine behavior is unchanged and the full suite stays green. Deliverable: a self-validating adjacency invariant.

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`
- Test: `Arcane/Tests/src/PhysicsBodyContactsTest.cpp` (create)

**Interfaces:**
- Produces (consumed by Task 2):
  - `std::vector<std::vector<std::uint32_t>> m_bodyContacts;` — per-body-slot list of dyn-dyn body contact pool ids.
  - `std::vector<std::uint32_t> m_splitLocalIndex;` + `static constexpr std::uint32_t kSplitLocalNone = 0xFFFFFFFFu;` — per-slot scratch, all-`kSplitLocalNone` between `SplitIsland` calls.
  - `void DetachContactAdjacency(std::uint32_t id, const Contact& c) noexcept;` — remove `id` from both endpoints' lists (dyn-dyn only; no-op otherwise).
  - `void ReleaseAndDestroyContact(std::uint32_t id, const Contact& c) noexcept;` — `DetachContactAdjacency` + `ReleaseContactColor(id)` + `m_contactPool.Destroy(id)`.
  - `bool DebugValidateBodyContacts() const;` — true iff the adjacency exactly mirrors the live dyn-dyn body contacts.

- [ ] **Step 1: Declare the members, sentinel, helpers, and validator (PhysicsWorld.hpp)**

Add `m_bodyContacts` right after the `m_bodyFixtures` declaration (currently `PhysicsWorld.hpp:1226`):

```cpp
            // Per-body dyn-dyn contact adjacency (keyed by body SoA slot), the
            // Box2D b2ContactEdge analogue used by SplitIsland to walk only an
            // island's own contacts. Holds the POOL IDS of each dynamic body's
            // dyn-dyn body contacts (both endpoints carry the id). Maintained ONLY
            // at contact create + destroy; merge/split/sleep/wake never touch it
            // (body slots are stable -- only m_islandId changes). Mirrors the
            // m_bodyFixtures vector-of-vectors lifecycle.
            std::vector<std::vector<std::uint32_t>> m_bodyContacts;
```

Add the scratch + sentinel near the `m_islandId` / `m_awakeIndex` per-slot columns (after `m_splitCandidates`, currently `PhysicsWorld.hpp:1350`):

```cpp
            // SplitIsland scratch: member body slot -> local DSU index, O(1).
            // All-kSplitLocalNone between calls (SplitIsland sets only its
            // members, then resets only those). Sized in EnsureCapacity.
            static constexpr std::uint32_t kSplitLocalNone = 0xFFFFFFFFu;
            std::vector<std::uint32_t>      m_splitLocalIndex;
```

Declare the helpers + validator in the private/method section (next to `MarkSplitCandidate` / `SplitIsland` declarations; search for `void SplitIsland(`):

```cpp
            // Remove contact `id` from both endpoints' m_bodyContacts (dyn-dyn
            // only; no-op for non-dyn-dyn or already-absent). Reads c BEFORE any
            // pool Destroy frees it.
            void DetachContactAdjacency(std::uint32_t id, const Contact& c) noexcept;
            // The canonical pooled-contact teardown: detach adjacency + release
            // the persistent color (while c still holds it) + pool.Destroy.
            void ReleaseAndDestroyContact(std::uint32_t id, const Contact& c) noexcept;
```

Declare the validator near the other `Debug*` accessors (e.g. after `DebugContactCount`, `PhysicsWorld.hpp:1059`):

```cpp
            // Test invariant: true iff m_bodyContacts exactly mirrors the live
            // dyn-dyn body contacts (every such contact's id appears once in BOTH
            // endpoints' lists, and every id in every list is a live dyn-dyn body
            // contact incident to that slot, with no duplicates).
            [[nodiscard]] bool DebugValidateBodyContacts() const;
```

- [ ] **Step 2: Size + reset the adjacency (PhysicsWorld.cpp)**

In `EnsureBodyAuxCapacity` (after `m_bodyFixtures.resize(next);`, currently `PhysicsWorld.cpp:256`):

```cpp
            m_bodyContacts.resize(next);
```

In `EnsureCapacity` (after `m_bodyColorMask.resize(next, 0u);`, currently `PhysicsWorld.cpp:213`):

```cpp
            // SplitIsland scratch column. All-sentinel; SplitIsland writes only
            // its members then resets them, so the tail stays sentinel.
            m_splitLocalIndex.resize(next, kSplitLocalNone);
```

In `AddBody`, mirror the fixture recycle-clear (after `m_bodyFixtures[idx].clear();`, currently `PhysicsWorld.cpp:1038`):

```cpp
            m_bodyContacts[idx].clear(); // fresh slot (may be recycled)
```

In `RemoveBody`, mirror the fixture clear (next to `m_bodyFixtures[idx].clear();`, currently `PhysicsWorld.cpp:1241`). `DestroyContactsForBody` (called earlier in `RemoveBody`) already drains it; this is the defensive reset:

```cpp
                m_bodyContacts[idx].clear();
```

- [ ] **Step 3: Implement helpers + validator (PhysicsWorld.cpp)**

Add a file-local free helper near the top of the `.cpp` anonymous namespace (or just above `SplitIsland`), then the methods. Place the methods next to `SplitIsland`:

```cpp
        // Remove the first occurrence of `id` from `v` by swap-with-back + pop.
        // No-op if absent. Order within m_bodyContacts is irrelevant to SplitIsland
        // (connected components are union-order-invariant), so swap-remove is safe.
        static void SwapRemoveId(std::vector<std::uint32_t>& v, std::uint32_t id) noexcept
        {
            for (std::size_t k = 0; k < v.size(); ++k)
            {
                if (v[k] == id) { v[k] = v.back(); v.pop_back(); return; }
            }
        }

        void PhysicsWorld::DetachContactAdjacency(std::uint32_t id, const Contact& c) noexcept
        {
            // Only dyn-dyn body contacts were ever attached (see TryCreateContact).
            if (!c.bIsBody || c.bodyA == kInvalidSlot || c.bodyB == kInvalidSlot) { return; }
            if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
                TypeSlot(c.bodyB) != BodyType::Dynamic) { return; }
            if (c.bodyA < m_bodyContacts.size()) { SwapRemoveId(m_bodyContacts[c.bodyA], id); }
            if (c.bodyB < m_bodyContacts.size()) { SwapRemoveId(m_bodyContacts[c.bodyB], id); }
        }

        void PhysicsWorld::ReleaseAndDestroyContact(std::uint32_t id, const Contact& c) noexcept
        {
            DetachContactAdjacency(id, c); // reads c before the pool frees the slot
            ReleaseContactColor(id);       // free the color while c still holds it
            m_contactPool.Destroy(id);
        }

        bool PhysicsWorld::DebugValidateBodyContacts() const
        {
            // 1) every id in every list is a live dyn-dyn body contact incident to
            //    that slot, with no duplicates within the list.
            for (std::uint32_t slot = 0; slot < m_bodyContacts.size(); ++slot)
            {
                const std::vector<std::uint32_t>& list = m_bodyContacts[slot];
                for (std::size_t k = 0; k < list.size(); ++k)
                {
                    const std::uint32_t id = list[k];
                    for (std::size_t j = k + 1; j < list.size(); ++j)
                    {
                        if (list[j] == id) { return false; } // duplicate
                    }
                    if (!m_contactPool.Alive(id)) { return false; }
                    const Contact& c = m_contactPool.Get(id);
                    if (!c.bIsBody) { return false; }
                    if (c.bodyA != slot && c.bodyB != slot) { return false; }
                    if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
                        TypeSlot(c.bodyB) != BodyType::Dynamic) { return false; }
                }
            }
            // 2) every live dyn-dyn body contact appears in BOTH endpoints' lists.
            //    (const ForEach overload binds here; it already skips dead ids.)
            bool ok = true;
            m_contactPool.ForEach([&](std::uint32_t id, const Contact& c)
            {
                if (!c.bIsBody || c.bodyA == kInvalidSlot || c.bodyB == kInvalidSlot) { return; }
                if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
                    TypeSlot(c.bodyB) != BodyType::Dynamic) { return; }
                auto has = [&](std::uint32_t s) -> bool {
                    if (s >= m_bodyContacts.size()) { return false; }
                    const std::vector<std::uint32_t>& l = m_bodyContacts[s];
                    for (std::uint32_t x : l) { if (x == id) { return true; } }
                    return false;
                };
                if (!has(c.bodyA) || !has(c.bodyB)) { ok = false; }
            });
            return ok;
        }
```

NOTE on `m_contactPool` access in the validator: `ContactPool` already has a const `ForEach` overload (`Contact.hpp:170`) and a const `Get` (`:157`) — use them as written. It does NOT expose a public aliveness predicate, so add this trivial accessor to `ContactPool` in `Contact.hpp` (next to `Get`):

```cpp
            [[nodiscard]] bool Alive(std::uint32_t id) const noexcept
            { return id < m_alive.size() && m_alive[id] != 0; }
```

- [ ] **Step 4: Attach at create + route the three destroy sites (PhysicsWorld.cpp)**

In `TryCreateContact`, inside `if (r.created)`, immediately AFTER the `AssignContactColor` block (currently ends at `PhysicsWorld.cpp:2399`):

```cpp
                // Per-body contact adjacency (G1 island-split linkage): a dyn-dyn
                // body contact is an island edge -> record it on BOTH endpoints so
                // SplitIsland can walk only this island's contacts. aDyn/bDyn are
                // the ORIENTED dyn flags (m_btype[ia]/m_btype[ib]); bodyA is
                // canonical-dynamic, so this fires exactly for dyn-dyn pairs.
                if (aDyn && bDyn)
                {
                    m_bodyContacts[ia].push_back(r.id);
                    m_bodyContacts[ib].push_back(r.id);
                }
```

Replace the destroy tail `ReleaseContactColor(id); m_contactPool.Destroy(id);` with `ReleaseAndDestroyContact(id, c);` at all THREE sites:
- `DestroyContactsForFixture` (currently `PhysicsWorld.cpp:1943-1944`) — the lambda param is `Contact& c`.
- `DestroyContactsForBody` (currently `PhysicsWorld.cpp:1971-1972`) — param `Contact& c`.
- `UpdateContacts` serial tail `kNpDestroy` branch (currently `PhysicsWorld.cpp:2869-2870`) — local `Contact& c`.

Each becomes simply:

```cpp
                    ReleaseAndDestroyContact(id, c);
```

- [ ] **Step 5: Create the invariant test + regen projects**

Create `Arcane/Tests/src/PhysicsBodyContactsTest.cpp`:

```cpp
// Per-body contact adjacency (m_bodyContacts) invariant guard.
//
// Builds a churning mixed-shape pile so contacts are created, destroyed, and
// updated every step, and asserts DebugValidateBodyContacts() holds throughout:
// m_bodyContacts mirrors the live dyn-dyn body contacts exactly. This guards the
// create/destroy/recycle maintenance independently of SplitIsland.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <cstdint>
#include <vector>

using namespace Arcane::Physics;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);
}

TEST_CASE("Body-contact adjacency mirrors live dyn-dyn contacts", "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    // Wide static floor (gravity +y -> bodies fall down onto it).
    {
        BodyDef fd;
        fd.type     = BodyType::Static;
        fd.position = Vec2(Real(0), Real(200));
        fd.shape    = MakeAabb(Real(300), Real(20));
        w.AddBody(fd);
    }

    // 90 dynamic mixed-shape bodies raining into a pile (heavy contact churn).
    std::uint32_t seed = 1234567u;
    auto rnd = [&](Real a, Real b) -> Real {
        seed = seed * 1664525u + 1013904223u;
        return a + (b - a) * Real((seed >> 8) & 0xFFFF) / Real(65535);
    };
    for (int i = 0; i < 90; ++i)
    {
        BodyDef d;
        d.type     = BodyType::Dynamic;
        d.density  = Real(1);
        d.friction = Real(0.4);
        d.position = Vec2(rnd(Real(-200), Real(200)), rnd(Real(-150), Real(150)));
        if (i % 3 == 0)      { d.shape = MakeCircle(rnd(Real(6), Real(12))); }
        else if (i % 3 == 1) { d.shape = MakeAabb(Real(8), Real(8)); d.fixedRotation = true; }
        else                 { d.shape = MakeCapsule(Real(10), Real(5)); }
        w.AddBody(d);
    }

    REQUIRE(w.DebugValidateBodyContacts()); // holds before any step
    for (int k = 0; k < 200; ++k)
    {
        w.Step(kStep);
        REQUIRE(w.DebugValidateBodyContacts()); // and after every step
    }
}
```

Regenerate the solution so the new TU compiles in (run from `Arcane\`):

Run (PowerShell): `& "D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026` (cwd `D:\dev\starworks\Gacha\Arcane`)
Expected: `Generated Tests/ArcaneTests.vcxproj...` / `Done`.

- [ ] **Step 6: Build + run the invariant test and the full island suite**

Run (PowerShell):
```
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m
& "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe" "[physics]"
```
Expected: build succeeds; `All tests passed` (the new case + every existing `[physics]` case — no behavior change, adjacency is write-only this task).

- [ ] **Step 7: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp \
        Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp \
        Arcane/Core/src/Arcane/Physics/Contact.hpp \
        Arcane/Tests/src/PhysicsBodyContactsTest.cpp
git commit -m "$(cat <<'EOF'
feat(arcane/physics): per-body contact adjacency (Box2D b2ContactEdge analogue)

Adds m_bodyContacts (vector<vector>, mirrors m_bodyFixtures): each dynamic
body's dyn-dyn body-contact pool ids, recorded on both endpoints. Maintained
ONLY at contact create (TryCreateContact, dyn-dyn) and destroy (centralized via
ReleaseAndDestroyContact at the 3 destroy sites); merge/split/sleep/wake leave
it untouched (body slots are stable). DebugValidateBodyContacts() + a churn test
guard the invariant. SplitIsland still uses the old whole-pool scan (next task);
behavior unchanged, full [physics] green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux
EOF
)"
```
(Only stage `Contact.hpp` if Step 3 added const accessors there.)

---

## Task 2: Rewrite `SplitIsland` to walk per-body adjacency + perf confirm

Replaces the O(poolSize x islandSize) whole-pool union with an O(islandBodies + islandEdges) walk over `m_bodyContacts`, byte-identical. Then re-measures (throwaway, reverted) to confirm the win.

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`SplitIsland`, currently `:3416-3509`)
- Test: `Arcane/Tests/src/PhysicsPersistentIslandTest.cpp` (add a multi-component fracture case)

**Interfaces:**
- Consumes (from Task 1): `m_bodyContacts`, `m_splitLocalIndex`, `kSplitLocalNone`.

- [ ] **Step 1: Add a multi-component fracture equivalence test**

This guards the rewrite's hardest path (>=3 components -> repeated `AllocIsland` + the first-seen-root assignment loop). It is **green on the current code** (the old split already computes connected components correctly) and MUST stay green after the rewrite. Append to `PhysicsPersistentIslandTest.cpp` (uses the file's existing `AddFloor`/`AddBox` helpers + `IslandRootOf(h.index)`):

```cpp
// Multi-component fracture: a connected chain that loses TWO internal links
// must split into THREE distinct islands, each internally connected. Guards
// the SplitIsland rewrite (per-body-adjacency walk) against the whole-pool scan
// it replaces -- byte-identical connected-component grouping.
TEST_CASE("PhysicsPersistentIsland: chain fractures into three islands",
          "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    AddFloor(w, Vec2(Real(0), Real(5)), Real(400), Real(5));

    // Six boxes resting side-by-side in a row, each touching its neighbour ->
    // one connected island. Half-width 5, placed every 10 units (faces touch).
    const Real hw = Real(5), hh = Real(5);
    std::vector<BodyHandle> b;
    for (int i = 0; i < 6; ++i)
    {
        b.push_back(AddBox(w, Vec2(Real(i) * Real(2) * hw, -hh), hw, hh));
    }
    for (int k = 0; k < 40; ++k) { w.Step(kStep); }
    // All in one island once settled + merged.
    for (int i = 1; i < 6; ++i)
    {
        REQUIRE(w.IslandRootOf(b[0].index) == w.IslandRootOf(b[i].index));
    }

    // Fling boxes 2 and 4 up/away so the 1-2, 2-3, 3-4, 4-5 links break, leaving
    // components {0,1}, {3}, {5} reachable plus the flung pair -- the point is
    // MULTIPLE disconnected components, forcing >=2 AllocIsland calls in split.
    w.SetVelocity(b[2], Vec2(Real(600), Real(-2500)));
    w.SetVelocity(b[4], Vec2(Real(-600), Real(-2500)));
    for (int k = 0; k < 60; ++k) { w.Step(kStep); } // quota=1/step -> let splits resolve

    // {0,1} stay together (still touching on the floor) and are distinct from the
    // flung boxes. Distinct components must have distinct island roots; same
    // component shares a root.
    CHECK(w.IslandRootOf(b[0].index) == w.IslandRootOf(b[1].index));
    CHECK(w.IslandRootOf(b[0].index) != w.IslandRootOf(b[2].index));
    CHECK(w.IslandRootOf(b[2].index) != w.IslandRootOf(b[4].index));
}
```

- [ ] **Step 2: Build + run the new test against the CURRENT (unchanged) `SplitIsland`**

Run (PowerShell):
```
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m
& "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe" "[physics][island]"
```
Expected: PASS. (Proves the guard is valid before the rewrite. If the chosen geometry does not reliably produce 3 distinct roots, adjust spawn spacing / fling velocities until it does, then re-confirm green.)

- [ ] **Step 3: Rewrite `SplitIsland` (PhysicsWorld.cpp)**

Replace the body of `SplitIsland` (currently `:3416-3509`) with the version below. The DSU `find` and the component->island-id assignment loop are **unchanged**; only the membership lookup (now O(1) via `m_splitLocalIndex`) and the edge source (now per-body `m_bodyContacts` instead of the whole-pool `ForEach`) change. Keep the realloc-hazard discipline: never hold an `Island&` across an `AllocIsland()`.

```cpp
        void PhysicsWorld::SplitIsland(std::uint32_t islandId)
        {
            // Re-derive the connected components of one candidate island. Bodies
            // joined by a touching dyn-dyn contact share a component; the FIRST
            // component reuses islandId, others get fresh ids. The contact walk is
            // scoped to the island's OWN contacts via per-body adjacency
            // (m_bodyContacts) -> O(islandBodies + islandEdges), replacing the old
            // O(poolSize x islandSize) whole-pool scan. Byte-identical components.
            if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
            {
                return;
            }
            m_islands[islandId].splitCandidate = false;

            // Snapshot members (the rebuild reassigns m_islandId + may reuse this id).
            std::vector<std::uint32_t> members = m_islands[islandId].bodies;
            const std::uint32_t n = static_cast<std::uint32_t>(members.size());
            if (n <= 1)
            {
                return; // 0 or 1 member: nothing to fracture
            }

            // O(1) member-slot -> local index. Scratch is all-sentinel on entry.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_splitLocalIndex[members[i]] = i;
            }

            std::vector<std::uint32_t> parent(n);
            for (std::uint32_t i = 0; i < n; ++i) { parent[i] = i; }
            auto find = [&](std::uint32_t x) -> std::uint32_t
            {
                while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
                return x;
            };

            // Union members joined by a TOUCHING dyn-dyn contact, walking only the
            // island's own contacts. Each edge is visited from both endpoints; the
            // union is idempotent and connected components are union-order-invariant,
            // so the resulting partition is identical to the old whole-pool walk.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint32_t slot = members[i];
                for (const std::uint32_t cid : m_bodyContacts[slot])
                {
                    const Contact& c = m_contactPool.Get(cid);
                    if (!c.touching) { continue; }
                    const std::uint32_t other = (c.bodyA == slot) ? c.bodyB : c.bodyA;
                    if (other >= m_splitLocalIndex.size()) { continue; }
                    const std::uint32_t j = m_splitLocalIndex[other];
                    if (j == kSplitLocalNone) { continue; } // other body not in island
                    parent[find(i)] = find(j);
                }
            }

            // Group by local root; FIRST component reuses islandId, others alloc a
            // fresh id. UNCHANGED from the original (byte-identical id assignment).
            // SAFETY: AllocIsland() may emplace_back + REALLOCATE m_islands -- never
            // hold an Island& across it; index m_islands[isl] freshly by id.
            m_islands[islandId].bodies.clear();
            std::vector<std::uint32_t> rootLocal;   // distinct local roots, first-seen order
            std::vector<std::uint32_t> rootIsland;  // parallel island id per root
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint32_t r = find(i);
                std::uint32_t ri = 0xFFFFFFFFu;
                for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(rootLocal.size()); ++k)
                {
                    if (rootLocal[k] == r) { ri = k; break; }
                }
                std::uint32_t isl;
                if (ri == 0xFFFFFFFFu)
                {
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

            // Reset only the touched scratch entries -> keep O(island), all-sentinel
            // between calls.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_splitLocalIndex[members[i]] = kSplitLocalNone;
            }
        }
```

- [ ] **Step 4: Build + run the full `[physics]` suite (byte-identity gate)**

Run (PowerShell):
```
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m
& "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe" "[physics]"
```
Expected: `All tests passed` — the new fracture case, `PhysicsPersistentIslandTest`, `PhysicsIslandTest`, `PhysicsDeterminismTest`, and the `[physics][mt]` byte-identity guards all green. **A failure here means the rewrite changed island output — STOP and fix; do not re-baseline.**

- [ ] **Step 5: Throwaway perf re-measure (Dist), then revert**

Confirm the quadratic is gone. Temporarily instrument the stage-5 split loop and run scene 8 at 2000 + 10000 in Dist; the throwaway is reverted before commit (mirrors the update-phase instrumentation revert `162374d1`).

1. In `Arcane/Core/src/Arcane/Physics/StepProf.hpp` set `#define ARCANE_STEPPROF 1`.
2. In `PhysicsWorld.cpp` add `#include <chrono>`, `#include <cstdio>`, `#include <cstdlib>` (throwaway) and wrap the stage-5 split-candidate scan + `SplitIsland` loop (currently `:1814-1830`) in `std::chrono` timers gated on `std::getenv("ARCANE_SLEEPPROF")`, accumulating ns and printing `[SPLITPROF] ... SplitIsland=%.4fms` every 300 steps (same shape as the measure-first pass; see the spec's measurement section).
3. Create a throwaway `Arcane/Tests/src/SleepProfTmp.cpp` (tag `[sleepprof]`) that builds scene 8 via `Arcane::Sandbox::BuildStressTestN(reg, N)` (N from `ARCANE_SLEEPPROF_BODIES`, default 2000), mints bodies with one `Arcane::PhysicsSystem phys(1.0f/60.0f); phys(reg);`, then `world.Step(1.0f/60.0f)` x600. (Model it on `SandboxVisualsTest.cpp`'s scene-8 setup: `RegisterSceneComponents`/`RegisterPhysicsComponents`, a `PhysicsResource` with `WorldDef{gravityY=900}`.)
4. Regen (`premake5.exe vs2026` from `Arcane\`), build **Dist**, run:
   `$env:ARCANE_SLEEPPROF="1"; foreach ($n in 2000,10000){ $env:ARCANE_SLEEPPROF_BODIES="$n"; & "...\Dist-windows-x86_64-md\ArcaneTests\ArcaneTests.exe" "[sleepprof]" }`
5. Record the `SplitIsland` ms/step at 2000 and 10000 (expected: sub-ms at 10k, down from ~24ms).
6. **Revert all throwaway:** `git checkout -- Arcane/Core/src/Arcane/Physics/StepProf.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (restores the SplitIsland rewrite from the index? NO — it is not committed yet). IMPORTANT: the rewrite from Step 3 is uncommitted, so do NOT `git checkout` PhysicsWorld.cpp. Instead, remove ONLY the throwaway lines from StepProf.hpp + PhysicsWorld.cpp by hand (the `ARCANE_STEPPROF 1`, the 3 throwaway includes, and the SPLITPROF timer block), and `rm Arcane/Tests/src/SleepProfTmp.cpp`. Then regen again. Verify `git diff` on StepProf.hpp is empty and PhysicsWorld.cpp shows ONLY the SplitIsland rewrite.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp \
        Arcane/Tests/src/PhysicsPersistentIslandTest.cpp
git commit -m "$(cat <<'EOF'
perf(arcane/physics): SplitIsland walks per-body adjacency, not the whole pool

Re-derives island connected components over m_bodyContacts (the island's own
contacts) with an O(1) m_splitLocalIndex membership lookup, replacing the
O(poolSize x islandSize) whole-pool ForEach + linear localOf scan. DSU and the
first-seen-root island-id assignment loop are unchanged -> byte-identical island
output (full [physics] green, no re-baseline). Measured (Dist, scene 8 + whisk):
SplitIsland ~24ms/step@10k -> <REPLACE with measured sub-ms>; the sleep stage is
no longer quadratic. Multi-component fracture test added. MT-offload still TODO.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux
EOF
)"
```

---

## Review gates (subagent-driven-development)

Each task gets the standard two-stage review (spec-compliance + code-quality), opus on both given the byte-identity/determinism criticality. In addition, per the user directive:

### Final review: direct Box2D v3 faithfulness comparison

After Task 2 passes, run a dedicated review that fetches the **actual Box2D v3 source** (erincatto/box2d `main`) and compares our implementation against it — not just against this spec:
- `m_bodyContacts` vs `b2Body.headContactKey` + `b2Contact.edges[2]` (per-body contact-edge graph).
- `ReleaseAndDestroyContact` / `DetachContactAdjacency` vs `b2DestroyContact` / `b2UnlinkContact` (edge removal + island-split marking).
- The create-site attach vs `b2CreateContact` / `b2LinkContact`.
- `SplitIsland` vs `b2SplitIsland` (component re-derivation; how Box2D scopes the traversal to the island, its determinism, and what it offloads to `b2SplitIslandTask`).

The reviewer reports where Arcane is faithful, where it deliberately differs (e.g. per-body `vector<vector>` vs intrusive edge list; split kept serial — MT-offload deferred), and any correctness gap the comparison surfaces. Findings route through `superpowers:receiving-code-review` (verify before acting; do not blindly apply).

---

## Self-Review (plan vs spec)

- **Spec coverage:** structure (`m_bodyContacts` + `m_splitLocalIndex`) -> Task 1 Step 1-2; create maintenance -> T1 S4; destroy centralization (`ReleaseAndDestroyContact` at 3 sites) -> T1 S4; slot recycle resets -> T1 S2; `SplitIsland` rewrite + byte-identity -> T2 S3-4; verification (full `[physics]` + fracture test + re-measure/revert) -> T1 S6, T2 S2/S4/S5; Box2D v3 comparison (user directive) -> Review gates. Scope boundaries (candidate-scan untouched, MT-offload deferred, tile-span bug out) honored. No `Clear()` path exists (verified) -> not in plan.
- **Placeholder scan:** the only intentional fill is `<REPLACE with measured sub-ms>` in the Task 2 commit message, filled at Step 5. All code blocks are complete.
- **Type consistency:** `m_bodyContacts`, `m_splitLocalIndex`, `kSplitLocalNone`, `DetachContactAdjacency`, `ReleaseAndDestroyContact`, `DebugValidateBodyContacts`, `SwapRemoveId`, `IslandRootOf` used consistently across Tasks 1-2.
