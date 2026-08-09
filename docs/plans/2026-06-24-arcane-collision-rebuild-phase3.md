# Physics Collision Rebuild — Phase 3: Persistent Contacts — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `GenerateContacts`' per-step from-scratch rebuild with a **persistent `Contact` array** — one pooled contact per overlapping fixture-pair, manifold recomputed at most once per step, surviving across steps — that feeds the **unchanged** SoftStep solver, collapsing the ~40% gen-rebuild cost.

**Architecture:** A new presentation-free Core `ContactPool` holds a persistent `Contact` per solver-relevant fixture-pair, keyed `(loFixtureSlot, hiFixtureSlot)` + generation. Each step, `UpdateContacts(dt)` (a) creates contacts for newly-overlapping broadphase pairs (mover↔mover via the Phase-2 `DynamicTree` move buffer / `UpdatePairs`; mover↔static via the `SpatialGrid` `StaticCandidates`), (b) destroys contacts whose fixtures' **fat** AABBs separated or whose handles went stale, (c) for the survivors, recomputes the manifold **once** via `Collide` (skipping pairs where both bodies sleep, reusing the cached manifold), then (d) the solver feed is a **walk** over touching contacts emitting `ContactConstraint`s with the same `MixContactId` so the solver's existing id-keyed warm-start cache matches. **The SoftStep solver, its warm-start cache, `ContactManager` (events), CCD, and islands are all unchanged** — events stay in `ContactManager::Step` until Phase 4.

**Tech Stack:** C++23, Core (presentation-free, static-CRT + /MD dual build), Catch2 (`Arcane/Tests`, `[physics]`), premake5 (`vs2026`), MSBuild. SPEC: `docs/superpowers/specs/2026-06-23-arcane-collision-module-rebuild-design.md` (§4, §7, §8, §10 Phase 3).

---

## Conventions (read once)

- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (Release/Dist: swap config).
- **Tests** from the exe dir: `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"`. Full gate = no filter, Debug AND Release; `[gpu]` both backends.
- **ArcaneCore static-CRT:** `"<msbuild>" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug|Release ...`. Core stays presentation-free (std + glm + sibling Physics headers only) + builds both CRTs.
- **New source files** → regen BOTH workspaces: `cd Arcane && "<root>/ThirdParty/premake5/premake5.exe" vs2026` AND `cd Server && "<root>/ThirdParty/premake5/premake5.exe" vs2026` (ArcaneCore globs `Arcane/Core/src/**.cpp`). NOT GenerateProjects.bat (hangs on a `pause`). `VCPKG_ROOT` is set.
- **clangd/IDE diagnostics are FALSE POSITIVES** (`glm/vec2.hpp not found`, `incomplete Astra::Registry`, `STL1000 Unexpected compiler version`). MSVC/MSBuild is the source of truth.
- **Kill stray Loom** before building on a plugin-lock error: `powershell -c "Get-Process Loom -ErrorAction SilentlyContinue | Stop-Process -Force"`.
- Branch: `feature/arcane-collision-rebuild-phase3` (already created off `main` @ `907d05d`). Commit per task; trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push.
- **DETERMINISM IS THE CONTRACT.** This is the hot Step path. Every task keeps the full `[physics]` suite green; the run-twice-identical determinism tests and the behavioral suites (stacks settle, pyramid stable, joints hold, CCD no-tunnel, resting penetration bounded) are the gate. Per `feedback_engine_evolves_not_frozen`, the persistent contacts SHOULD produce the same constraints as `GenerateContacts` (same `Collide`, same ids) — so **byte-identity vs the old path is a free regression tripwire** (Task 3 asserts it before the swap), not a permanent freeze.

## Architecture facts (verified from the codebase — rely on these; confirm exact lines by reading)

- **`PhysicsWorld::Step`** (`Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` ~1382-1511): Stage 1 prev-snapshot + kinematic integrate (`UpdateMoverProxies`); Stage 2 `GenerateContacts(dt)` (builds `m_contactConstraints`); Stage 3 `Solver::Solve(ctx)` (SolverContext over `m_contactConstraints.data()`); Stage 4 `BulletSweep()` (CCD); Stage 5 `Island::UpdateSleep`; Stage 6 `m_contacts.Step(*this)` (ContactManager events).
- **`GenerateContacts`** (PhysicsWorld.cpp ~1513-2035): clears `m_contactConstraints`, rebuilds a per-fixture AABB cache, then two paths feed an `emit` lambda: (a) per dynamic body, `StaticCandidates(queryBox, genSpans, genStatics)` → tile-span + static-body fixture pairs; (b) `m_fixtureBroadphase->UpdatePairs(m_genPairs)` → sorted mover↔mover fixture-pairs. Orientation rule: A is dynamic; if both dynamic, lower BODY slot is A (swap bodies+fixtures together). The `emit` lambda fills a `ContactConstraint` and `cp.id = MixContactId(manifold.points[p].id, fixA, fixB)`; skips `manifold.pointCount <= 0`.
- **`Collide`** (`Narrowphase/Collide.hpp:89`): `Manifold Collide(const Shape& a, const Transform& xfA, const Shape& b, const Transform& xfB, Real speculativeMargin = 0, NarrowphaseTrace* trace = nullptr)`. `Manifold{ Vec2 normal; int pointCount; ManifoldPoint points[2]; NarrowphaseKind kind; }`; `ManifoldPoint{ Vec2 point; Real separation; Vec2 normal; uint32_t id; }`.
- **`MixContactId`** (PhysicsWorld.cpp ~53-69): `uint32_t MixContactId(uint32_t base, uint32_t fixA, uint32_t fixB)` — stable per `(base, fixA, fixB)`. `base` = `ManifoldPoint::id`.
- **`ContactConstraint` / `ContactConstraintPoint`** (`Solver/Solver.hpp:98-136`): see Task 4 for the fields the emit must fill. Warm-start lives on `cp.normalImpulse/tangentImpulse`; the **solver** (`SoftStep.cpp`) seeds them at `PrepareContacts` via `m_cache.find(cp.id)` (~135-145) and stores them after `Solve` via `m_cache.insert_or_assign(cp.id, ...)` (~597-608). **Phase 3 does NOT touch the solver or `m_cache` — keeping `cp.id` stable preserves warm-start.**
- **Broadphase** (`Broadphase/Broadphase.hpp:72-126`): `BroadphasePair{ uint32_t a, b; }` (a<b, fixture slots). `IBroadphase::UpdatePairs(out)` (DynamicTree move-buffer incremental, sorted). `world.FixtureBroadphase() const -> const IBroadphase&` + a non-const overload (UpdatePairs mutates). `world.FixtureBroadphaseTree() const -> const DynamicTree*` (added in debug-viz; for fat-box queries). `DynamicTree::ForEachLeaf(fn(id, tight, fat))` exists (debug-viz) — use it / the proxy fat box for the fat-overlap destruction test.
- **`SpatialGrid::StaticCandidates`** (used by GenerateContacts) → tile spans + static body slots overlapping a query box.
- **Fixture/body SoA** (PhysicsWorld.hpp ~906-954): `m_fxBody[fi]` (owning body slot), `m_fxGen[fi]` (0=dead), `m_fxShape[fi]`, `m_fxLocalPosX/Y/Angle[fi]`, `m_fxFriction/Restitution[fi]`, `m_fxSensor[fi]`, `m_bodyFixtures[bodySlot]` (vector of fixture slots). `BodyOfFixture(fi)`, `GetBodyFixture(BodyHandle, n)`, `HandleOf(i)`, `IsValid(FixtureHandle)`. `m_alive[i]`, `m_btype[i]` (`TypeSlot`), `m_sensor[i]` (`SensorSlot`), `m_awake[i]` (sleep). `kInvalidSlot = 0xFFFFFFFF`. `ComposeFixtureXf(bodyPos, bodyAngle, localPos, localAngle)`.
- **`ContactManager`** (`Physics/ContactManager.hpp/.cpp`): persistent `m_pairs` (events). **UNCHANGED in Phase 3** — it keeps running its own pass for Begin/Stay/End. (Phase 4 deletes it.)
- **Existing oracle/test style:** `Arcane/Tests/src/PhysicsFixtureBroadphaseTest.cpp` (brute-force pair oracle), `PhysicsBroadphaseTest.cpp`, `PhysicsDynamicsTest.cpp` (behavioral). API: `WorldDef`, `BodyDef{ type, position, shape, density, friction, restitution, fixedRotation, isSensor }`, `MakeAabb`/`MakeBox`/`MakeCircle`, `w.AddBody`, `w.Step(dt)`, `w.Position(h)`, `[physics]` tag.

## Design decisions (settled here — flag if you disagree during execution)

1. **Contact existence = fat-box hysteresis, creation from the existing broadphase.** Reuse the Phase-2 `UpdatePairs` (mover↔mover) + `StaticCandidates` (mover↔static) outputs as the **creation** source (`pool.EnsurePair` — create-if-absent, deduped by the `(lo,hi)` key). A contact is **destroyed** by the contact pass itself when the two fixtures' **fat** AABBs no longer overlap (or a handle goes stale). This gives Box2D-style fat-box persistence + warm-start stability **without** a risky new broadphase pair-creation API (deferred to Phase 5 if profiling wants the move-buffer-direct path). The broadphase only ever adds; the contact pool owns destruction (matches §5).
2. **Solver + warm-start cache UNCHANGED.** Emit `ContactConstraint`s with the same `MixContactId` → the solver's id-keyed `m_cache` provides warm-start continuity unchanged. (Physically relocating impulses onto the `Contact` is a Phase-5 refinement, not needed for the win.)
3. **Phase 3 contact set = solver-relevant pairs only** (mirrors `GenerateContacts`' current filters: skip same-body, skip if neither dynamic, skip sensors). Sensor/kinematic pairs + touch-state/events are Phase 4's expansion. So Phase 3's `Contact` carries only what the solver feed needs (fixtures, cached bodies, orientation, last manifold).
4. **Oracle-gate before the swap (Task 3).** Build the persistent contacts running **alongside** `GenerateContacts`, and assert the emitted constraint set is **equal** (same pairs, ids, normals, points) before deleting the old path (Task 4). Lowest-risk path; per decision in `feedback_engine_evolves_not_frozen` this byte-identity check is a tripwire, not a freeze.

## File Structure

- **Create** `Arcane/Core/src/Arcane/Physics/Contact.hpp` — the `Contact` POD + the `ContactPool` (pool + `(lo,hi)` dedup map + free list). Presentation-free.
- **Create** `Arcane/Core/src/Arcane/Physics/Contact.cpp` — `ContactPool` methods (EnsurePair/Destroy/Find/iteration).
- **Modify** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp/.cpp` — own a `ContactPool m_contactPool`; add `UpdateContacts(dt)` (the one-pass update + creation sources) + `EmitContactConstraints()` (the solver-feed walk); wire creation/destruction into the body/fixture lifecycle; swap `Step` Stage 2 from `GenerateContacts` to `UpdateContacts` + `EmitContactConstraints`.
- **Create** `Arcane/Tests/src/PhysicsContactPoolTest.cpp` — pool unit tests (`[physics]`).
- **Create** `Arcane/Tests/src/PhysicsPersistentContactTest.cpp` — lifecycle + manifold-once + oracle-equivalence + determinism tests (`[physics]`).

---

### Task 1: `Contact` + `ContactPool` (persistent pool + dedup, unused)

**Files:** Create `Contact.hpp`, `Contact.cpp`; Create test `PhysicsContactPoolTest.cpp`. (No Step-path change — the pool is defined + unit-tested but not yet consumed.)

- [ ] **Step 1: Write the failing test** — `Arcane/Tests/src/PhysicsContactPoolTest.cpp`:

```cpp
// ContactPool: a persistent, deduped, generation-safe pool of fixture-pair contacts.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/Contact.hpp>

using namespace Arcane::Physics;

TEST_CASE("ContactPool dedups by fixture-slot pair and is generation-safe", "[physics]")
{
    ContactPool pool;
    const FixtureHandle f1{ 3, 1 }, f2{ 7, 1 }, f3{ 9, 1 };

    // (a) EnsurePair creates exactly one contact per unordered pair.
    const std::uint32_t c12 = pool.EnsurePair(f1, f2);
    REQUIRE(c12 != ContactPool::kNone);
    REQUIRE(pool.EnsurePair(f2, f1) == c12);     // order-independent dedup
    REQUIRE(pool.Count() == 1);

    const std::uint32_t c13 = pool.EnsurePair(f1, f3);
    REQUIRE(c13 != c12);
    REQUIRE(pool.Count() == 2);

    // (b) Find returns the same id; the contact carries both handles.
    REQUIRE(pool.Find(f1, f2) == c12);
    const Contact& cc = pool.Get(c12);
    REQUIRE(((cc.a.index == 3 && cc.b.index == 7) || (cc.a.index == 7 && cc.b.index == 3)));

    // (c) Destroy frees the slot + removes the key; a later EnsurePair may recycle it.
    pool.Destroy(c12);
    REQUIRE(pool.Count() == 1);
    REQUIRE(pool.Find(f1, f2) == ContactPool::kNone);

    // (d) A recycled fixture slot (same index, new generation) is a DISTINCT pair.
    const FixtureHandle f2b{ 7, 2 };             // slot 7 recycled, gen bumped
    const std::uint32_t c12b = pool.EnsurePair(f1, f2b);
    REQUIRE(pool.Find(f1, f2) == ContactPool::kNone);   // old gen never aliases
    REQUIRE(pool.Find(f1, f2b) == c12b);
}
```

VERIFY `FixtureHandle{index,generation}` (Fixture.hpp). Settle the `ContactPool` API names you use below; keep these four assertions' intent.

- [ ] **Step 2: Regen both workspaces + build + verify COMPILE FAIL** (`[physics]`, new file + new symbols).

- [ ] **Step 3: Implement `Contact.hpp`** — presentation-free (`#include <cstdint>`, `<vector>`, `<unordered_map>`, `<Arcane/Physics/Fixture.hpp>`, `<Arcane/Physics/Narrowphase/Manifold.hpp>`):

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>
#include <Arcane/Physics/Fixture.hpp>
#include <Arcane/Physics/Narrowphase/Manifold.hpp>

namespace Arcane { namespace Physics {

    // One persistent contact per solver-relevant overlapping fixture-pair. Survives
    // across steps; the manifold is recomputed at most once per step (UpdateContacts).
    // Phase 3 carries only what the solver feed needs (events stay in ContactManager).
    struct Contact
    {
        FixtureHandle a{};            // canonical: A's body is dynamic (oriented at create)
        FixtureHandle b{};
        std::uint32_t bodyA = 0;      // cached body SLOT for A
        std::uint32_t bodyB = 0;      // cached body SLOT for B (kInvalidSlot for a tile span)
        bool          bIsBody = true; // false => B is a tile-span virtual fixture
        Manifold      manifold{};     // last computed (pointCount 0 => not touching)
        bool          touching = false;
    };

    // Pooled, deduped by the unordered fixture-slot pair, generation-safe.
    class ContactPool
    {
    public:
        static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

        // Create-if-absent; returns the contact id. Caller fills body slots/orientation
        // on first creation via Get(id) when the return is a fresh slot (Count grew).
        std::uint32_t EnsurePair(FixtureHandle a, FixtureHandle b);
        std::uint32_t Find(FixtureHandle a, FixtureHandle b) const;   // kNone if absent
        void          Destroy(std::uint32_t id);

        Contact&       Get(std::uint32_t id)       { return m_pool[id]; }
        const Contact& Get(std::uint32_t id) const { return m_pool[id]; }
        std::size_t    Count() const { return m_live; }

        // Visit live contacts in ascending id order (deterministic).
        void ForEach(const std::function<void(std::uint32_t id, Contact&)>& fn);
        void Clear();

    private:
        static std::uint64_t Key(FixtureHandle a, FixtureHandle b);   // (lo<<32)|hi over a
                                                                      // gen-mixed slot key
        std::vector<Contact>      m_pool;     // indexed by id; holes reused via m_free
        std::vector<std::uint8_t> m_alive;    // 1 if id is a live contact
        std::vector<std::uint32_t> m_free;    // recycled ids
        std::unordered_map<std::uint64_t, std::uint32_t> m_index;  // Key -> id
        std::size_t m_live = 0;
    };

}}  // namespace Arcane::Physics
```

`Key`: mix each handle's `index` + `generation` into a 32-bit half (e.g. `index ^ (generation * 0x9E3779B9)`), then pack `(min<<32)|max` so a recycled slot (new gen) never aliases. Implement `EnsurePair/Find/Destroy/ForEach/Clear` in `Contact.cpp`: `EnsurePair` looks up `m_index`; on miss, pop `m_free` or grow `m_pool`/`m_alive`, set the handles (caller fills body slots), insert into `m_index`, `++m_live`. `Destroy` erases the key, marks `!m_alive`, pushes the id to `m_free`, `--m_live`. `ForEach` iterates ids `0..m_pool.size()` skipping `!m_alive` (ascending id = deterministic).

- [ ] **Step 4: Build Debug + `[physics]` green** (new pool is isolated; nothing else changes). ArcaneCore static-CRT Debug clean (regen Server; new `.cpp` is globbed).

- [ ] **Step 5: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics/Contact.hpp Arcane/Core/src/Arcane/Physics/Contact.cpp Arcane/Tests/src/PhysicsContactPoolTest.cpp
git commit -m "feat(arcane/physics): persistent ContactPool (deduped, generation-safe fixture-pair pool)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `UpdateContacts(dt)` — the one-pass persistent update (built, not yet feeding the solver)

**Files:** Modify `PhysicsWorld.hpp/.cpp` (add `ContactPool m_contactPool` + `UpdateContacts(dt)` + the fat-overlap helper); Create test `PhysicsPersistentContactTest.cpp`. `UpdateContacts` is CALLED from `Step` BEFORE `GenerateContacts` (so the pool is populated each step) but its output is NOT yet consumed — `GenerateContacts` still feeds the solver. This lets Task 3 compare the two.

- [ ] **Step 1: Write the failing lifecycle test** — `Arcane/Tests/src/PhysicsPersistentContactTest.cpp`:

```cpp
// Persistent contacts: survive across steps, recompute the manifold at most once/step,
// destroy on fat-box separation + on body removal. (Pool populated each Step; not yet
// feeding the solver -- that is Task 4.)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>

using namespace Arcane::Physics;

TEST_CASE("Persistent contact survives across steps + destroys on separation", "[physics]")
{
    PhysicsWorld w;
    BodyDef d; d.shape = MakeAabb(Real(10), Real(10)); d.fixedRotation = true;
    d.type = BodyType::Static;  d.position = Vec2(0, 100);  w.AddBody(d);
    d.type = BodyType::Dynamic; d.position = Vec2(0, 81);   BodyHandle dyn = w.AddBody(d); // overlapping
    w.Step(Real(1) / Real(60));

    REQUIRE(w.DebugContactCount() >= 1);                 // a contact exists for the overlapping pair
    const std::uint32_t firstStepContacts = w.DebugContactCount();

    // Persists across a second step (NOT recreated from scratch).
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() == firstStepContacts);

    // Teleport the dynamic body far away -> fat boxes separate -> contact destroyed.
    w.SetPosition(dyn, Vec2(10000, 10000));
    w.Step(Real(1) / Real(60));
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() == 0);
}

TEST_CASE("Persistent contact destroyed when a body is removed", "[physics]")
{
    PhysicsWorld w;
    BodyDef d; d.shape = MakeAabb(Real(10), Real(10)); d.fixedRotation = true;
    d.type = BodyType::Static;  d.position = Vec2(0, 100);  BodyHandle s = w.AddBody(d);
    d.type = BodyType::Dynamic; d.position = Vec2(0, 81);   w.AddBody(d);
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() >= 1);

    w.RemoveBody(s);
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() == 0);                 // no stale contact to the dead body
}
```

VERIFY `MakeAabb`, `SetPosition`, `RemoveBody` signatures. Add a test-only `PhysicsWorld::DebugContactCount() const -> std::size_t` returning `m_contactPool.Count()` (read-only; OK to expose for the gate).

- [ ] **Step 2: Build + verify compile fail** (`DebugContactCount`/`m_contactPool`/`UpdateContacts` undefined).

- [ ] **Step 3: Add the pool + `UpdateContacts(dt)`.** In `PhysicsWorld.hpp`: `#include <Arcane/Physics/Contact.hpp>`; add private `ContactPool m_contactPool;` and scratch `std::vector<BroadphasePair> m_cpPairs;`; declare `void UpdateContacts(Real dt);` (private) and `std::size_t DebugContactCount() const noexcept { return m_contactPool.Count(); }` (public). In `PhysicsWorld.cpp` implement `UpdateContacts` — mirror `GenerateContacts`' transform composition + filters, but persist:

```
void PhysicsWorld::UpdateContacts(Real dt)
{
    // --- 1. CREATE: ensure a contact exists for every solver-relevant broadphase pair ---
    // (a) mover<->mover: the Phase-2 incremental fixture-pair set.
    m_fixtureBroadphase->UpdatePairs(m_cpPairs);            // sorted (fa<fb) fixture slots
    for (const BroadphasePair& p : m_cpPairs)
        TryCreateContact(p.a, p.b);                        // helper: applies the filters + orientation

    // (b) mover<->static (+ tile spans): per awake dynamic body, query StaticCandidates,
    //     ensure a contact per (dynamic fixture, static fixture) and per (dynamic fixture, span).
    //     MIRROR GenerateContacts' static path (genSpans/genStatics, fxListA/fxListB).
    //     For a tile span, key the contact on (dynFixtureSlot, span virtual id) -- spans are
    //     stable per cell, so reuse a deterministic span fixture-slot encoding (see note).

    // --- 2. UPDATE + DESTROY: one pass over the pool ---
    m_contactPool.ForEach([&](std::uint32_t id, Contact& c)
    {
        // Stale handle (body/fixture removed or gen bumped) -> destroy.
        if (!FixtureSlotLive(c.a) || (c.bIsBody && !FixtureSlotLive(c.b))) { m_contactPool.Destroy(id); return; }
        // Fat-box separation -> destroy (the broadphase only adds; the pool owns destroy).
        if (!FatBoxesOverlap(c)) { m_contactPool.Destroy(id); return; }
        // Both bodies asleep -> reuse the cached manifold (skip the narrowphase).
        if (BothAsleep(c)) return;
        // Recompute the manifold ONCE.
        const Transform xfA = FixtureWorldXf(c.bodyA, c.a.index);
        const Transform xfB = c.bIsBody ? FixtureWorldXf(c.bodyB, c.b.index) : SpanXf(c);
        const Real margin = SpecMargin(c, dt);             // mirror GenerateContacts' velocity margin
        c.manifold  = Collide(m_fxShape[c.a.index], xfA, ShapeOfB(c), xfB, margin);
        c.touching  = (c.manifold.pointCount > 0);
    });
}
```

Implement the helpers as small private methods/lambdas mirroring `GenerateContacts`:
- `TryCreateContact(fa, fb)`: resolve `a=m_fxBody[fa]`, `b=m_fxBody[fb]`; apply the SAME filters as the GenerateContacts mover loop (skip `a==b`; skip `!alive`; skip `sensor[a]||sensor[b]`; skip `!da && !db`); apply the SAME orientation (A dynamic; both-dynamic → lower body slot is A; swap fixtures with bodies); `id = m_contactPool.EnsurePair(handleA, handleB)`; if freshly created (Count grew / handles unset), fill `c.bodyA/bodyB/bIsBody`.
- `FatBoxesOverlap(c)`: AABB-overlap of the two fixtures' FAT boxes — read them from `m_fixtureBroadphase` (the `DynamicTree` proxy fat box via `FixtureBroadphaseTree()`/`ForEachLeaf` map, or a `FatAabbOfFixture(slot)` accessor you add) for movers; for a static B use its tight AABB + `kAabbMargin`. (Fat-box, not tight, gives warm-start hysteresis.)
- `FixtureSlotLive(h)`: `h.index < m_fxGen.size() && m_fxGen[h.index] == h.generation && m_fxGen[h.index] != 0`.
- `BothAsleep(c)`: `!m_awake[c.bodyA] && (!c.bIsBody || !m_awake[c.bodyB])` (static/span count as "asleep").
- `FixtureWorldXf` / `SpecMargin` / span handling: reuse GenerateContacts' exact code.

**Tile-span note:** spans are transient per-query in GenerateContacts. For persistence, either (i) skip persisting span contacts in Phase 3 and recreate them each step from `StaticCandidates` (a span contact is cheap + the static grid is already incremental), OR (ii) encode a stable span key from `(cellX, cellY)`. **Recommended: (i)** — `EnsurePair` span contacts each step keyed by a deterministic `(dynFixture, packedCell)` and let unreferenced span contacts destroy on fat-separation like any other. Document the choice.

Call `UpdateContacts(dt)` from `Step` immediately BEFORE `GenerateContacts(dt)` (Stage 2). Do NOT consume its output yet.

- [ ] **Step 4: Build + the two lifecycle tests pass + full `[physics]` green** (behavior unchanged — `GenerateContacts` still feeds the solver; `UpdateContacts` is a side pool). Paste summaries. ArcaneCore static-CRT Debug clean.

- [ ] **Step 5: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Tests/src/PhysicsPersistentContactTest.cpp
git commit -m "feat(arcane/physics): UpdateContacts -- persistent one-pass contact update (pool populated, not yet feeding the solver)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Oracle-gate — the contact walk emits the SAME constraints as `GenerateContacts`

**Files:** Modify `PhysicsWorld.hpp/.cpp` (add `EmitContactConstraints(out)` that walks the pool → a `std::vector<ContactConstraint>`, WITHOUT touching `m_contactConstraints`); extend `PhysicsPersistentContactTest.cpp` with the equivalence + manifold-once + determinism oracle.

- [ ] **Step 1: Write the failing equivalence test** — append to `PhysicsPersistentContactTest.cpp`:

```cpp
// The persistent-contact solver feed must produce the SAME constraint set as the
// legacy GenerateContacts path (same pairs/ids/normals/points) -- the pre-swap oracle.
TEST_CASE("Persistent contact walk == GenerateContacts constraint set", "[physics]")
{
    PhysicsWorld w;
    // A small mixed scene: a static floor + a 3-box stack + a circle, stepped to settle.
    BuildOracleScene(w);                                 // helper in this TU
    for (int i = 0; i < 30; ++i) w.Step(Real(1) / Real(60));

    // The world ran GenerateContacts this step (m_contactConstraints). Now ask the pool
    // to emit its constraints and compare as sorted sets by (bodyA,bodyB,id).
    std::vector<ContactConstraint> fromPool;
    w.DebugEmitPoolConstraints(fromPool);                // EmitContactConstraints into 'out'

    std::vector<ContactConstraint> fromGen;
    w.DebugCopyActiveConstraints(fromGen);               // copy of m_contactConstraints

    REQUIRE(SameConstraintSet(fromPool, fromGen));       // helper: sort + compare key fields
}
```

`SameConstraintSet`: sort both by `(bodyA, bodyB, points[0].id)` and compare `bodyA/bodyB/pointCount/normal (within 1e-4)/each point's id + baseSeparation (within 1e-4)`. Add the two `Debug*` accessors (read-only). NOTE per `feedback_engine_evolves_not_frozen`: this equality is the **pre-swap tripwire** — if a deliberate improvement later changes the manifold, re-baseline this test on purpose.

- [ ] **Step 2: Build + verify compile fail** (`EmitContactConstraints`/`Debug*` undefined).

- [ ] **Step 3: Implement `EmitContactConstraints(std::vector<ContactConstraint>& out)`** — walk `m_contactPool.ForEach` in ascending id order; for each **touching** contact (`c.touching`), build a `ContactConstraint` with the SAME field assignments as the `GenerateContacts` `emit` lambda (invMass/invInertia from `c.bodyA/bodyB`, `normal = c.manifold.normal`, friction `sqrt(fricA*fricB)`, restitution `max(restA,restB)`, per-point `anchorA = point - comA`, `anchorB = point - comB`, `baseSeparation = -separation`, `id = MixContactId(c.manifold.points[p].id, c.a.index, c.b.index)`), skipping `pointCount <= 0`. Collect into a scratch, then **sort by `(bodyA, bodyB, fixtureA, fixtureB)`** for determinism before returning (§7). Wire `DebugEmitPoolConstraints` → `EmitContactConstraints`; `DebugCopyActiveConstraints` → copy `m_contactConstraints`.

- [ ] **Step 4: Build + the equivalence test passes + full `[physics]` green.** If the sets differ, the persistent path diverged — investigate (orientation, filters, span handling, margin) until equal. Paste summaries. ArcaneCore static-CRT Debug clean.

- [ ] **Step 5: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Tests/src/PhysicsPersistentContactTest.cpp
git commit -m "test(arcane/physics): oracle-gate -- persistent contact walk == GenerateContacts constraints

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: The swap — `Step` feeds the solver from the persistent contacts

**Files:** Modify `PhysicsWorld.hpp/.cpp` — `Step` Stage 2 now calls `UpdateContacts(dt)` then `EmitContactConstraints(m_contactConstraints)` (filling the pool-owned `m_contactConstraints` the solver already reads); delete the legacy `GenerateContacts` body (keep any helper lambdas it owned that `UpdateContacts` reuses, hoisted to private methods).

- [ ] **Step 1: Make the swap.** In `Step`, replace `GenerateContacts(dt);` with:
```cpp
UpdateContacts(dt);
m_contactConstraints.clear();
EmitContactConstraints(m_contactConstraints);   // sorted; ids stable -> solver warm-start matches
```
Remove `GenerateContacts` (and its now-dead per-step AABB-cache rebuild). Keep `m_contactConstraints` as the solver-facing array (unchanged downstream: `Solver::Solve`, `ForEachContactConstraint`, `ActiveContactCount` all keep working). The debug-viz accessors (`ForEachContactConstraint`/`ActiveContactCount`/`StaticGrid`/...) are unaffected.

- [ ] **Step 2: Build + the FULL `[physics]` suite green — this is the behavioral + determinism gate.** Specifically confirm: the run-twice-identical determinism tests, stacks-settle / pyramid-stable / joints-hold / CCD-no-tunnel / resting-penetration-bounded behavioral suites, and the debug-viz `[debugviz]` accessor tests all pass. Paste the full `[physics]` summary. If a determinism/golden test changed, the persistent path perturbed something — investigate; per `feedback_engine_evolves_not_frozen`, re-baseline a golden ONLY if the change is a deliberate improvement, not an accident.

- [ ] **Step 3: Full ArcaneTests Debug (no filter) + ArcaneCore static-CRT Debug clean.** Paste the assertion/case summary (expect the baseline 77677/388 + the new `[physics]` contact tests; no regressions).

- [ ] **Step 4: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit -m "perf(arcane/physics): feed the solver from persistent contacts (retire GenerateContacts' per-step rebuild)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Lifecycle integration — create/destroy contacts at the body/fixture seams

**Files:** Modify `PhysicsWorld.cpp` — at the lifecycle sites that already update broadphase proxies (`RemoveBody`, `DropFixture`, and the slot-recycle paths), proactively destroy the affected contacts so a removed body never leaves a stale contact even for the one step before the update pass catches it. (Task 2's update pass already destroys stale-handle contacts defensively; this makes destruction immediate + tested.)

- [ ] **Step 1: Write the failing test** — append to `PhysicsPersistentContactTest.cpp`:

```cpp
// Dropping a fixture (compound body) destroys exactly that fixture's contacts immediately.
TEST_CASE("DropFixture destroys that fixture's persistent contacts", "[physics]")
{
    PhysicsWorld w;
    BodyDef d; d.shape = MakeAabb(Real(10), Real(10)); d.fixedRotation = true;
    d.type = BodyType::Static;  d.position = Vec2(0, 100); w.AddBody(d);
    d.type = BodyType::Dynamic; d.position = Vec2(0, 81);  BodyHandle dyn = w.AddBody(d);
    const FixtureHandle fx = w.GetBodyFixture(dyn, 0);
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() >= 1);

    w.DropFixture(dyn, fx);                               // VERIFY the real DropFixture signature
    REQUIRE(w.DebugContactCount() == 0);                 // destroyed immediately, before the next Step
}
```

VERIFY `DropFixture`'s real signature (grep PhysicsWorld.hpp). Adjust to it.

- [ ] **Step 2: Build + verify it fails** (contact lingers until the next Step's update pass).

- [ ] **Step 3: Add `DestroyContactsForFixture(fixtureSlot)` + `DestroyContactsForBody(bodySlot)`** (private): `m_contactPool.ForEach` → destroy any contact whose `a.index`/`b.index` equals the fixture slot (or whose `bodyA`/`bodyB` equals the body slot). Call `DestroyContactsForFixture` from `DropFixture` and `DestroyContactsForBody` from `RemoveBody` (at the same place they tear down broadphase proxies). Keep the update-pass stale-handle guard (defense in depth).

- [ ] **Step 4: Build + the test passes + full `[physics]` green.** ArcaneCore static-CRT Debug clean.

- [ ] **Step 5: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Tests/src/PhysicsPersistentContactTest.cpp
git commit -m "feat(arcane/physics): destroy persistent contacts at RemoveBody/DropFixture (immediate, +End-safe)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Full gate + perf smoke + determinism re-baseline

**Files:** none (verification) + possibly a re-baselined determinism golden (only if deliberately changed).

- [ ] **Step 1: Full ArcaneTests Debug + Release** (no filter, `[gpu]` both backends). Expect all green (baseline 77677/388 + the new `[physics]` contact-pool/lifecycle/oracle tests). Paste both summaries.
- [ ] **Step 2: ArcaneCore static-CRT Debug + Release** clean (`ArcaneCore.lib`). Core stayed presentation-free (the pool is std + Physics headers only).
- [ ] **Step 3: Perf smoke (the gate's measurable win).** Build Dist; run the stress scene through the perf harness: `bin\Dist-windows-x86_64-md\Loom\Loom.exe --perf` (or the gated `ARCANE_SANDBOX_SCENE=8` path — VERIFY the exact harness flag from the Phase-0 perf harness). Report the step/phase timing vs the Phase-2 baseline (scene-8 dense steady state ~0.7 ms/frame sim). Phase 3 should hold or improve (the gen-rebuild collapse shows most on settled piles). This is a VISUAL/measurement gate — the controller runs Loom; report the numbers.
- [ ] **Step 4: Determinism.** Confirm run-twice-identical holds. If a behavioral golden moved because the persistent path is a deliberate improvement (not a bug), re-baseline it ON PURPOSE with a one-line note; otherwise it must be unchanged.
- [ ] **Step 5: Update memory** — add/refresh a `project_arcane_collision_rebuild_phase3` memory (persistent ContactPool; UpdateContacts one-pass; solver+warm-start unchanged via stable MixContactId; oracle-gated swap; ContactManager still owns events → Phase 4 next) + the MEMORY.md line.

---

## Self-Review Notes (addressed)

- **Spec coverage (§7 Phase 3):** persistent `Contact` per fixture-pair + `(lo,hi)` dedup = Task 1; one update pass (separated→destroy / else recompute once) = Task 2; warm-start match via `MixContactId` = Task 3/4 (solver `m_cache` unchanged); solver feed = walk emitting `ContactConstraint`s = Task 3/4; broadphase-only-adds + contact-owns-destroy = Task 2 (decision 1); determinism sort by `(bodyA,bodyB,fixtureA,fixtureB)` = Task 3; removal/recycle robustness = Task 1 (gen key) + Task 5. §8 testing: brute-force-style lifecycle + the GenerateContacts oracle + run-twice determinism + behavioral suites + perf smoke = Tasks 2/3/4/6.
- **Out of Phase 3 scope (do NOT do here):** deleting `ContactManager::Step` / events-as-byproduct (Phase 4); physically relocating warm-start impulses onto the `Contact` (Phase 5); removing the interim AABB cull / `SweepAndPrune` / `Baumgarte` / Lua goldens (Phase 5); a move-buffer-direct fat-pair creation API (decision 1 — deferred).
- **Determinism:** every task re-runs full `[physics]`; the swap (Task 4) is gated by the oracle equivalence (Task 3) + run-twice + behavioral suites. The `ContactPool::ForEach` ascending-id order + the pre-handoff sort are the only ordering seams, both fixed.
- **Known soft spots for the executor:** (1) the **fat-box accessor** for the destruction test — confirm how to read a fixture proxy's fat AABB from the `DynamicTree` (via the debug-viz `FixtureBroadphaseTree()`/`ForEachLeaf` or a small `FatAabbOfFixture(slot)` you add); for a static B, use tight + `kAabbMargin`. (2) **Tile-span persistence** — decision 1(i) recreates span contacts each step (simplest); confirm spans don't churn warm-start (they have stable `MixContactId` from their cell). (3) The **orientation rule** MUST match `GenerateContacts` exactly (A dynamic; both-dynamic → lower body slot is A; swap fixtures with bodies) or the oracle (Task 3) fails — that's the test catching it. (4) **Both-asleep skip** reuses the cached manifold; confirm asleep bodies are already excluded from the solver so a stale manifold never reaches it.
```
