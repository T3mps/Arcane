# Arcane Physics: Velocity Clamp + Statics-onto-Tree — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Box2D v3's missing max-velocity/translation clamp (fixes the unwalled-escape crash) plus a defensive SpatialGrid guard, and move static-body candidate lookup from `m_staticGrid` (SpatialGrid) onto a `DynamicTree` (~3x faster per the benchmark).

**Architecture:** Two independent engine changes. A = a per-body velocity clamp replicated verbatim from Box2D v3.1.1 `b2IntegrateVelocitiesTask` in `SoftStep::IntegrateVelocitiesRange`, + a non-finite/absurd-range guard in `SpatialGrid`. B = swap the static spatial index `SpatialGrid m_staticGrid` for `DynamicTree m_staticTree` (behavior-preserving; identical candidate set), migrating the debug-viz static overlay from grid-cells to tree-leaves.

**Tech Stack:** C++23 (Arcane Core/Physics), MSVC via msbuild, Catch2 (ArcaneTests). Box2D v3.1.1 vendored at `ThirdParty/box2d-3.1.1` is the parity reference.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-01-arcane-physics-clamp-and-statics-tree-design.md`.
- **Exact Box2D v3 parity for the clamp.** Reference `ThirdParty/box2d-3.1.1/src/solver.c` `b2IntegrateVelocitiesTask` (lines 65-129), `src/constants.h` `B2_MAX_ROTATION = 0.25f * B2_PI` (line 33), `src/types.c` `def.maximumLinearSpeed = 400.0f * b2_lengthUnitsPerMeter` (line 21). Arcane units == Box2D units (`kLinearSlop = 0.005`), so constants carry 1:1.
- **Acceptance = exact parity, `[physics]` byte-identical expected.** Normal scenes stay under the clamp (default 400) so the suite is unchanged. If exact-Box2D behavior legitimately changes a case (a body Box2D itself would clamp), re-baseline it WITH written justification naming the exceeded clamp — do NOT tune the constant to hide it. A change from a scene that should NOT exceed the clamp is a bug to investigate.
- **Determinism:** no `/fp:fast`; the clamp is a pure per-body function → ST==MT byte-identity preserved.
- **Build (PowerShell, not Git Bash):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m`.
- **Tests** from the exe dir: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[physics]"`. New `.cpp` test files require premake regen: `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\` (NOT `GenerateProjects.bat` — it hangs on `pause`).
- **clangd/IDE diagnostics are FALSE POSITIVES** — MSVC via msbuild is truth.
- **Staging:** stage ONLY per-task files by explicit path. NEVER `git add -A` — the tree has 10 unrelated parked changes (Client ui_screens ×5, AGENTS.md, Arcane/.screenshots/, Server/cpp_coding_style.txt, two 2026-06-24 docs). Branch is `feature/arcane-physics-clamp-statics-tree`.
- **Commit trailers** (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux
  ```

## File Structure

| File | Change |
|---|---|
| `Core/src/Arcane/Physics/PhysicsTypes.hpp` | add `kMaxRotation` constant (Task 1) |
| `Core/src/Arcane/Physics/PhysicsWorld.hpp` | `WorldDef.maxLinearVelocity` + member + accessor (T1); `m_staticGrid`→`m_staticTree`, `StaticGrid()`→`StaticTree()` (T3) |
| `Core/src/Arcane/Physics/PhysicsWorld.cpp` | ctor init (T1); static touch-points → tree (T3) |
| `Core/src/Arcane/Physics/Solver/SoftStep.cpp` | clamp in `IntegrateVelocitiesRange` (T1) |
| `Core/src/Arcane/Physics/Queries.cpp` | `StaticCandidates` reroute to tree (T3) |
| `Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp/.cpp` | non-finite/absurd guard (T2) |
| `Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp/.hpp` | static overlay grid→tree leaves (T4) |
| `Sandbox/src/Hud.cpp`, `Sandbox/src/SandboxApp.hpp` | HUD label (T4) |
| `Tests/src/PhysicsDebugAccessorsTest.cpp` | `StaticGrid().ForEachCell`→`StaticTree().ForEachLeaf` (T4) |
| `Tests/src/PhysicsVelocityClampTest.cpp` | NEW — clamp behavior (T1) |
| `Tests/src/PhysicsSpatialGridTest.cpp` | ADD guard + statics-oracle cases (T2, T3) |

---

### Task 1: Velocity/translation clamp (exact Box2D v3 parity)

**Files:**
- Modify: `Core/src/Arcane/Physics/PhysicsTypes.hpp` (add `kMaxRotation`)
- Modify: `Core/src/Arcane/Physics/PhysicsWorld.hpp` (`WorldDef.maxLinearVelocity`, member, accessor)
- Modify: `Core/src/Arcane/Physics/PhysicsWorld.cpp` (ctor init)
- Modify: `Core/src/Arcane/Physics/Solver/SoftStep.cpp` (`IntegrateVelocitiesRange`)
- Create: `Tests/src/PhysicsVelocityClampTest.cpp`

**Interfaces:**
- Produces: `WorldDef::maxLinearVelocity` (Real, default 400); `PhysicsWorld::MaxLinearVelocity() const -> Real`; `Arcane::Physics::kMaxRotation` (Real). The clamp mutates `m_bodyState[i]` velocity in-place in the integrate.

- [ ] **Step 1: Write the failing test** — `Tests/src/PhysicsVelocityClampTest.cpp`:

```cpp
// Box2D v3 b2IntegrateVelocitiesTask parity: per-body linear speed clamped to
// WorldDef.maxLinearVelocity; angular speed clamped to kMaxRotation * invDt.
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

using namespace Arcane::Physics;

TEST_CASE("Linear velocity is clamped to maxLinearVelocity", "[physics][clamp]")
{
    WorldDef def; // default maxLinearVelocity == 400
    PhysicsWorld w(def);
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
    d.fixedRotation = true; d.position = Vec2(0, 0);
    BodyHandle h = w.AddBody(d);
    w.SetVelocity(h, Vec2(Real(2000), Real(0)));      // 5x over the cap
    w.Step(Real(1) / Real(60));
    const Vec2 v = w.Velocity(h);
    const Real speed = std::sqrt(v.x * v.x + v.y * v.y);
    REQUIRE(speed <= Real(400) + Real(0.5));           // clamped to the cap
    REQUIRE(speed >= Real(400) - Real(0.5));           // exactly the cap (was 2000)
}

TEST_CASE("Sub-cap linear velocity is untouched", "[physics][clamp]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
    d.fixedRotation = true; d.position = Vec2(0, 0);
    BodyHandle h = w.AddBody(d);
    w.SetVelocity(h, Vec2(Real(100), Real(0)));        // well under 400
    w.Step(Real(1) / Real(60));
    const Vec2 v = w.Velocity(h);
    REQUIRE(std::abs(v.x - Real(100)) < Real(0.001));  // unchanged (no gravity/damping)
    REQUIRE(std::abs(v.y) < Real(0.001));
}

TEST_CASE("Angular velocity is clamped to kMaxRotation * invDt", "[physics][clamp]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeAabb(Real(8), Real(2));
    d.position = Vec2(0, 0); // rotation allowed
    BodyHandle h = w.AddBody(d);
    w.SetAngularVelocity(h, Real(1000));               // huge spin
    w.Step(Real(1) / Real(60));
    const Real maxAng = kMaxRotation * Real(60);       // invDt = 1/(1/60) = 60
    REQUIRE(std::abs(w.AngularVelocity(h)) <= maxAng + Real(0.01));
    REQUIRE(std::abs(w.AngularVelocity(h)) >= maxAng - Real(0.01));
}
```

> Note: `Velocity(BodyHandle)`, `SetVelocity`, `SetAngularVelocity`, `AngularVelocity` are existing public `PhysicsWorld` methods (`PhysicsWorld.hpp:442-451`).

- [ ] **Step 2: Build + run to verify it fails**

Run: build Debug, then `ArcaneTests.exe "[clamp]"` from the exe dir.
Expected: `[clamp]` cases FAIL — the over-cap body keeps ~2000 (no clamp yet). (`kMaxRotation` / `MaxLinearVelocity` may not compile yet — that is also a valid RED.) Regen premake first (new test file): `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\`.

- [ ] **Step 3: Add `kMaxRotation`** to `PhysicsTypes.hpp` next to `kLinearSlop` (line 145):

```cpp
        // Box2D v3 B2_MAX_ROTATION: max rotation per FULL step (quarter turn).
        // constants.h:33 -> 0.25f * B2_PI. Arcane units == Box2D units.
        inline constexpr Real kMaxRotation = Real(0.25) * Real(3.14159265358979323846);
```

- [ ] **Step 4: Add the WorldDef field + world member + accessor + ctor init.**

In `PhysicsWorld.hpp`, in the Soft Step solver config block after `contactPushMaxVelocity` (line 226):

```cpp
            // Box2D v3 max linear speed clamp (b2WorldDef::maximumLinearSpeed,
            // types.c:21 default 400 * lengthUnitsPerMeter). Bodies faster than
            // this are scaled down each velocity-integrate -- the safety net that
            // prevents runaway escape. Angular uses kMaxRotation * invDt.
            Real          maxLinearVelocity  = Real(400);
```

Add the private member near `m_contactPushMaxVelocity` (line 1430):

```cpp
            Real          m_maxLinearVelocity = Real(400);
```

Add the accessor near `ContactPushMaxVelocity()` (line 1132):

```cpp
            [[nodiscard]] Real MaxLinearVelocity() const noexcept { return m_maxLinearVelocity; }
```

In `PhysicsWorld.cpp` ctor init list near `m_contactPushMaxVelocity(def.contactPushMaxVelocity)` (line 134):

```cpp
            , m_maxLinearVelocity(def.maxLinearVelocity)
```

- [ ] **Step 5: Add the clamp to `IntegrateVelocitiesRange`** (`SoftStep.cpp:320-356`). Insert AFTER the damping block, replacing the final three writes. The exact current tail is:

```cpp
                m_bodyState[i].vx = vx;
                m_bodyState[i].vy = vy;
                m_bodyState[i].w  = wv;
```

Replace with (verbatim Box2D v3 `b2IntegrateVelocitiesTask` clamp, solver.c:108-125 — note `w` is the `PhysicsWorld&`, `wv` is angular velocity, `ctx.invDt` is the full-step inverse):

```cpp
                // Clamp to max linear speed (Box2D v3 b2IntegrateVelocitiesTask,
                // solver.c:108-114). Arcane units == Box2D units.
                const float maxLin   = static_cast<float>(w.MaxLinearVelocity());
                const float maxLinSq = maxLin * maxLin;
                if (vx * vx + vy * vy > maxLinSq)
                {
                    const float ratio = maxLin / std::sqrt(vx * vx + vy * vy);
                    vx *= ratio; vy *= ratio;
                }
                // Clamp to max angular speed = kMaxRotation * invDt (full-step
                // inverse), solver.c:75,116-122. Arcane exposes no allowFastRotation
                // opt-out (all bodies == Box2D default allowFastRotation=false), so
                // the clamp is unconditional.
                const float maxAng   = static_cast<float>(kMaxRotation * ctx.invDt);
                const float maxAngSq = maxAng * maxAng;
                if (wv * wv > maxAngSq)
                {
                    const float ratio = maxAng / std::abs(wv);
                    wv *= ratio;
                }
                m_bodyState[i].vx = vx;
                m_bodyState[i].vy = vy;
                m_bodyState[i].w  = wv;
```

Ensure `SoftStep.cpp` includes `<cmath>` (for `std::sqrt`/`std::abs`) and `<Arcane/Physics/PhysicsTypes.hpp>` (for `kMaxRotation`) — add if absent. `ctx.invDt` is a `SolverContext` field (`Solver.hpp:215`).

- [ ] **Step 6: Build + run to verify the clamp tests PASS**

Run: build Debug, `ArcaneTests.exe "[clamp]"` from the exe dir.
Expected: all 3 `[clamp]` cases PASS.

- [ ] **Step 7: Run the full `[physics]` suite — must stay byte-identical**

Run: `ArcaneTests.exe "[physics]"` from the exe dir.
Expected: ALL pass, same case/assertion counts as before this task (normal scenes never exceed 400). If ANY case changed: STOP. Determine whether it is a scene that legitimately exceeds the clamp (Box2D would clamp it too → re-baseline WITH written justification per the acceptance stance) or a regression (fix it). Report which.

- [ ] **Step 8: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp Arcane/Tests/src/PhysicsVelocityClampTest.cpp
git commit -m "feat(arcane/physics): Box2D v3 max-velocity/translation clamp in integrate

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 2: Defensive SpatialGrid guard (non-finite / absurd range)

**Files:**
- Modify: `Core/src/Arcane/Physics/Broadphase/SpatialGrid.cpp`
- Modify: `Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp` (private helper decl)
- Modify: `Tests/src/PhysicsSpatialGridTest.cpp` (add cases)

**Interfaces:**
- Consumes: existing `SpatialGrid::Insert/QueryAABB/CellRange`.
- Produces: a private `bool SpatialGrid::SaneBox(const Aabb2&) const` used to early-out `Insert`/`QueryAABB` on non-finite or absurdly-large boxes. Behavior UNCHANGED for valid boxes.

- [ ] **Step 1: Write the failing test** — add to `Tests/src/PhysicsSpatialGridTest.cpp`:

```cpp
#include <cmath>
#include <limits>

TEST_CASE("SpatialGrid survives a non-finite AABB", "[physics][grid]")
{
    SpatialGrid g(32.0f);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Aabb2 bad; bad.min = Vec2(Real(nan), Real(0)); bad.max = Vec2(Real(inf), Real(10));
    g.Insert(1u, bad);                 // must not hang / OOM / crash
    std::vector<std::uint32_t> out;
    const int n = g.QueryAABB(bad, out);   // must not hang / OOM / crash
    REQUIRE(n == 0);
    REQUIRE(out.empty());
    // A valid id nearby still works (grid is not corrupted).
    Aabb2 ok; ok.min = Vec2(0,0); ok.max = Vec2(10,10);
    g.Insert(2u, ok);
    g.QueryAABB(ok, out);
    REQUIRE(std::find(out.begin(), out.end(), 2u) != out.end());
}

TEST_CASE("SpatialGrid survives an absurdly large AABB", "[physics][grid]")
{
    SpatialGrid g(32.0f);
    Aabb2 huge; huge.min = Vec2(Real(-1e30), Real(-1e30)); huge.max = Vec2(Real(1e30), Real(1e30));
    g.Insert(1u, huge);                // must not attempt ~1e56 cells
    std::vector<std::uint32_t> out;
    const int n = g.QueryAABB(huge, out);
    REQUIRE(n == 0);                   // treated as empty (out-of-budget)
}
```

- [ ] **Step 2: Build + run to verify it fails** — the tests HANG or crash (billion-cell loop / bad_alloc) before the guard. Run `ArcaneTests.exe "[grid]"`; expected: hang/crash/timeout = RED. (If it OOMs the runner, that itself confirms the bug — proceed to implement.)

- [ ] **Step 3: Implement the guard in `SpatialGrid.cpp`.** Add the helper + a cell budget near the top of the file (after the includes; add `#include <cmath>`):

```cpp
namespace {
    // Max cells per axis a single box may span. Generous (a 64-unit cell x 65536
    // = ~4.2M units) so all legitimate content is under it; anything larger is
    // garbage input (non-finite -> huge, or escaped coords) -> treat as empty.
    constexpr int kMaxCellsPerAxis = 1 << 16;
}
```

Add the helper (implementation in .cpp; declare `bool SaneBox(const Aabb2&) const;` in the private section of `SpatialGrid.hpp`):

```cpp
bool SpatialGrid::SaneBox(const Aabb2& b) const
{
    if (!std::isfinite(b.min.x) || !std::isfinite(b.min.y) ||
        !std::isfinite(b.max.x) || !std::isfinite(b.max.y))
        return false;
    int x0, y0, x1, y1; CellRange(b, x0, y0, x1, y1);
    // Guard against a span so large the cell loop would never finish. Compute in
    // 64-bit to avoid int overflow in the subtraction.
    const long long spanX = static_cast<long long>(x1) - static_cast<long long>(x0);
    const long long spanY = static_cast<long long>(y1) - static_cast<long long>(y0);
    if (spanX < 0 || spanY < 0) return false;                 // wrapped -> garbage
    if (spanX > kMaxCellsPerAxis || spanY > kMaxCellsPerAxis) return false;
    return true;
}
```

Guard the entry points. In `Insert` (line 22), at the very top before `Remove(id)`:

```cpp
void SpatialGrid::Insert(std::uint32_t id, const Aabb2& box)
{
    if (!SaneBox(box)) { Remove(id); return; } // drop garbage; don't register
    Remove(id);
    // ... existing body unchanged ...
```

In `QueryAABB` (line 77), at the top:

```cpp
int SpatialGrid::QueryAABB(const Aabb2& box, std::vector<std::uint32_t>& out) const
{
    out.clear();
    if (!SaneBox(box)) return 0;
    // ... existing body unchanged ...
```

(`Move` calls `Insert`, so it is covered.)

> Note: `SaneBox` calls `CellRange`, which calls `CellCoord` on non-finite input producing a garbage-but-finite int — that is fine, `SaneBox` rejects via the `isfinite` check first (short-circuit) so `CellRange` only runs on finite input. Confirm the `isfinite` check precedes the `CellRange` call (it does above).

- [ ] **Step 4: Build + run to verify PASS** — `ArcaneTests.exe "[grid]"`: the two new cases pass (return empty, no hang), and the pre-existing `[grid]` cases still pass (valid boxes unaffected).

- [ ] **Step 5: Run `[physics]`** — `ArcaneTests.exe "[physics]"`: unchanged (the guard never triggers on valid content). Byte-identical.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.cpp Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp Arcane/Tests/src/PhysicsSpatialGridTest.cpp
git commit -m "fix(arcane/physics): guard SpatialGrid against non-finite/absurd AABBs

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 3: Statics onto a DynamicTree (behavior-preserving)

**Files:**
- Modify: `Core/src/Arcane/Physics/PhysicsWorld.hpp` (`m_staticGrid`→`m_staticTree`, `StaticGrid()`→`StaticTree()`)
- Modify: `Core/src/Arcane/Physics/PhysicsWorld.cpp` (5 static touch-points)
- Modify: `Core/src/Arcane/Physics/Queries.cpp` (`StaticCandidates`)
- Modify: `Tests/src/PhysicsSpatialGridTest.cpp` (add statics-oracle case)

**Interfaces:**
- Consumes: `DynamicTree` (`Update(id,box)` upsert, `Remove(id)`, `QueryAABB(box,out)` — `Broadphase/DynamicTree.hpp`, already included by `PhysicsWorld.hpp`).
- Produces: `PhysicsWorld::StaticTree() const -> const DynamicTree&` (replaces `StaticGrid()`). `StaticCandidates` static-body set is IDENTICAL to before.

- [ ] **Step 1: Write the failing test** — add to `Tests/src/PhysicsSpatialGridTest.cpp` a static-oracle case that pins behavior-preservation (this passes today via the grid; it must still pass after the tree swap — TDD here is "lock the invariant, then swap under it"):

```cpp
TEST_CASE("StaticCandidates set is stable across many statics (tree-backed)", "[physics][grid]")
{
    PhysicsWorld w;
    std::mt19937 rng(0xABCDEF);
    std::uniform_real_distribution<float> pos(-400.0f, 400.0f);
    std::vector<BodyHandle> handles;
    for (int i = 0; i < 200; ++i) {
        BodyDef d; d.type = BodyType::Static;
        d.position = Vec2(Real(pos(rng)), Real(pos(rng)));
        d.shape = MakeAabb(Real(6), Real(6));
        handles.push_back(w.AddBody(d));
    }
    // Brute-force oracle: statics whose slot-AABB overlaps the query.
    auto brute = [&](const Aabb2& q) {
        std::vector<std::uint32_t> out;
        for (std::uint32_t i = 0; i < w.Count(); ++i)
            if (w.Alive(i) && w.TypeSlot(i) == BodyType::Static && AabbOverlap(w.SlotAabb(i), q))
                out.push_back(i);
        std::sort(out.begin(), out.end());
        return out;
    };
    std::uniform_real_distribution<float> qc(-400.0f, 400.0f);
    for (int t = 0; t < 100; ++t) {
        const float cx = qc(rng), cy = qc(rng);
        Aabb2 q; q.min = Vec2(Real(cx-20), Real(cy-20)); q.max = Vec2(Real(cx+20), Real(cy+20));
        std::vector<Aabb2> spans; std::vector<std::uint32_t> statics, scratch;
        w.StaticCandidates(q, spans, statics, scratch);
        std::sort(statics.begin(), statics.end());
        REQUIRE(statics == brute(q));   // exact set, before AND after the tree swap
    }
}
```

- [ ] **Step 2: Build + run — verify it PASSES on the current (grid) code first**

Run: `ArcaneTests.exe "[grid]"`. Expected: the new oracle PASSES against the existing `m_staticGrid` (it is a true invariant). This is the safety net: it must STILL pass after Step 3's swap. (This task is a refactor guarded by an oracle, not a red→green feature.)

- [ ] **Step 3: Swap the static index to a DynamicTree.**

In `PhysicsWorld.hpp`: replace the member (line ~1403):

```cpp
            // Per-shape static index -> DynamicTree (benchmark: ~3x faster than a
            // SpatialGrid for static candidate lookup). Statics never move, so the
            // tree's move-buffer is simply unused. Behavior-preserving: QueryAABB
            // returns the same sorted, tight-narrowed candidate set.
            DynamicTree m_staticTree;
```

Replace the accessor (line 709):

```cpp
            // The per-shape static-body index (DynamicTree of static body slots).
            [[nodiscard]] const DynamicTree& StaticTree() const noexcept
            {
                return m_staticTree;
            }
```

In `PhysicsWorld.cpp`, swap the 5 touch-points (`Update` is upsert — first call inserts):
- Line 1015: `m_staticGrid.Insert(idx, SlotAabb(idx));` → `m_staticTree.Update(idx, SlotAabb(idx));`
- Line 506: `m_staticGrid.Move(bodySlot, SlotAabb(bodySlot));` → `m_staticTree.Update(bodySlot, SlotAabb(bodySlot));`
- Line 561: same as 506 → `m_staticTree.Update(bodySlot, SlotAabb(bodySlot));`
- Line 1558: `m_staticGrid.Move(i, SlotAabb(i));` → `m_staticTree.Update(i, SlotAabb(i));`
- Line 1140: `m_staticGrid.Remove(idx);` → `m_staticTree.Remove(idx);`

In `Queries.cpp:553`: `m_staticGrid.QueryAABB(box, gridScratch);` → `m_staticTree.QueryAABB(box, gridScratch);` (identical signature + sorted-candidate contract).

> `m_staticList`, `m_staticGridScratch`, and the `StaticCandidates` narrowing are UNCHANGED (spec §4). Whether the tree should subsume `m_staticList` is a deferred benchmark decision — do NOT touch it here.

- [ ] **Step 4: Build.** Task 4 fixes the remaining `StaticGrid()` consumers (debug viz + accessor test) — those live in `Arcane.dll` / `Sandbox` / `ArcaneTests`, so a full-solution Debug build will FAIL to compile until Task 4. To keep Task 3 self-contained and testable, build+run ONLY the Core + ArcaneTests physics after also applying Task 4's `PhysicsDebugAccessorsTest` one-line fix inline is NOT allowed (scope). Instead: build `Core` alone to prove Core compiles, and run the `[grid]` oracle by temporarily excluding the debug-accessor test is NOT allowed either.

  Resolution: **Tasks 3 and 4 are committed together** (they are one behavioral change — the static index type — plus its unavoidable consumer updates). Apply Task 3 AND Task 4 edits, then build the full solution, then run tests. The two tasks are described separately for review clarity but land in one build/commit. (If the reviewer wants them split, `StaticGrid()` can be kept as a deprecated alias returning nothing — but that is churn; prefer the joint commit.)

- [ ] **Step 5: (after Task 4 edits applied) Build full solution + run**

Run: build Debug (full solution), then `ArcaneTests.exe "[grid]"` and `ArcaneTests.exe "[physics]"`.
Expected: the statics-oracle case still PASSES (identical set via the tree), all `[grid]` + `[physics]` pass, byte-identical.

- [ ] **Step 6: Commit** (joint with Task 4 — see Task 4 Step 4).

---

### Task 4: Migrate the static debug-viz from grid-cells to tree-leaves

**Files:**
- Modify: `Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp` (static overlay), `.hpp` (comment)
- Modify: `Sandbox/src/Hud.cpp` (label), `Sandbox/src/SandboxApp.hpp` (comment)
- Modify: `Tests/src/PhysicsDebugAccessorsTest.cpp` (`StaticGrid().ForEachCell`→`StaticTree().ForEachLeaf`)

**Interfaces:**
- Consumes: `PhysicsWorld::StaticTree() const -> const DynamicTree&` (Task 3); `DynamicTree::ForEachLeaf(fn(id, tight, fat))` (`DynamicTree.hpp:98`).

- [ ] **Step 1: Update `PhysicsDebugAccessorsTest.cpp:59`.** Replace the `StaticGrid().ForEachCell(...)` block with a `StaticTree().ForEachLeaf(...)` walk that asserts the same thing the cell walk did (that inserted statics are enumerable). Mirror the existing tree-leaf assertions used for the mover `FixtureBroadphaseTree` in that same test file (read the file to match its style). Example shape:

```cpp
    int leaves = 0;
    w.StaticTree().ForEachLeaf(
        [&](std::uint32_t /*id*/, const Aabb2& tight, const Aabb2& fat) {
            (void)tight; (void)fat; ++leaves;
        });
    REQUIRE(leaves == /* number of static bodies added in this test */);
```

- [ ] **Step 2: Update `PhysicsDebugDraw.cpp:495-507`.** Replace the static-grid cell overlay (`world.StaticGrid()` + `ForEachCell` drawing cell rects) with a static-tree leaf overlay, mirroring the existing mover-tree overlay in the SAME file (search for `FixtureBroadphaseTree` / the `ForEachLeaf` overlay and copy its structure; draw each leaf's fat box with `kColStaticGrid`). Change the include comment on line 29 if needed. Keep the `opts.drawStaticGrid` flag name (renaming it ripples to Sandbox; a comment update suffices).

- [ ] **Step 3: Update the HUD label** in `Sandbox/src/Hud.cpp:549`: `ImGui::Checkbox("Static grid", &dbg.drawStaticGrid);` → `ImGui::Checkbox("Static tree", &dbg.drawStaticGrid);`. Update the comment in `SandboxApp.hpp:106` (`static-body SpatialGrid cells` → `static-body DynamicTree leaves`).

- [ ] **Step 4: Build full solution (Tasks 3+4 together) + run all tests + commit.**

Run: `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\` (if any new test file was added — none here beyond Task 1/2, but regen is harmless), then build Debug full solution.
Expected: compiles clean (all `StaticGrid()` consumers now updated).
Run: `ArcaneTests.exe "[physics]"` and `ArcaneTests.exe "[sandbox]"` (if present) from the exe dir. Expected: all pass, byte-identical `[physics]`.

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/Queries.cpp Arcane/Tests/src/PhysicsSpatialGridTest.cpp Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp Arcane/Sandbox/src/Hud.cpp Arcane/Sandbox/src/SandboxApp.hpp Arcane/Tests/src/PhysicsDebugAccessorsTest.cpp
git commit -m "perf(arcane/physics): static candidate lookup on a DynamicTree (was SpatialGrid)

Bench: tree ~3x faster than the grid for static candidate queries. Behavior-
preserving (identical tight-narrowed candidate set, oracle-guarded). Migrates the
static debug overlay from grid-cells to tree-leaves.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

## Post-plan: final verification

After Task 4: full `[physics]` run on the final HEAD (byte-identical, or A-driven re-baseline justified per Global Constraints), then a whole-branch review. Merge is the USER's call (honor-system).

## Self-Review (author checklist — completed)

- **Spec coverage:** §3 A1 clamp → Task 1 (exact Box2D solver.c:108-125, default 400, kMaxRotation, invDt); §3 A2 guard → Task 2; §4 B statics→tree → Task 3; the `StaticGrid()`/debug-viz ripple (discovered in planning, added to spec) → Task 4; §5 testing → each task's test steps + final [physics]; §2 units (1:1) → Task 1 constants.
- **Placeholder scan:** no TBD/TODO; the clamp code is verbatim from the vendored Box2D source (cited lines); test code is complete.
- **Type consistency:** `MaxLinearVelocity()`/`maxLinearVelocity`/`m_maxLinearVelocity`, `kMaxRotation`, `StaticTree()`/`m_staticTree`, `DynamicTree::Update/Remove/QueryAABB/ForEachLeaf` — consistent across tasks.
- **Task-3/4 coupling:** flagged explicitly — the static-index type change and its consumer (debug-viz + accessor-test) updates land in ONE build/commit because `StaticGrid()` consumers won't compile otherwise; described as two tasks for review clarity.
