# Arcane physics: align sleep + contact stiffness with Box2D v3 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A resting pile of dynamic bodies sleeps. Replace Arcane's Lua-ported separate sleep gates with Box2D v3's combined extent-weighted per-body-thresholded sleep test, add static-contact stiffening, and align the hertz clamp.

**Architecture:** Three independent behavioral changes, each its own commit + re-baseline, in dependency order: (1) sleep test (predicate-only — narrow re-baseline), (2) `staticSoftness` for dyn-vs-static contacts (solver — broad re-baseline), (3) hertz-cap alignment (no-op at 4 substeps). A final commit reverts the throwaway repro instrumentation.

**Tech Stack:** C++23, Arcane Core physics (`Arcane/Core/src/Arcane/Physics`), Catch2 (`ArcaneTests`), headless `Loom` repro rig.

**Spec:** `docs/superpowers/specs/2026-06-28-arcane-physics-sleep-box2d-alignment-design.md`
**Findings:** `docs/superpowers/research/2026-06-28-arcane-deep-penetration-never-settle-findings.md`

## Global Constraints

- **Branch:** already on `feature/arcane-physics-sleep-box2d-alignment` (off `main` @ `bad2303a`). Do not commit to `main`.
- **Core is presentation-free + C++23-clean:** glm + std + sibling Physics headers only. No SDL3/NVRHI/ImGui. `namespace Arcane::Physics`.
- **Determinism:** index-ordered iteration, no wall-clock, no fast-math (`/fp:strict` in engine; no `/fp:fast`). UTF-8 without BOM, ASCII comments only.
- **Engine evolves — byte-identity is NOT the goal here.** Steps 1 and 2 intentionally change behavior; re-baseline the affected tests deliberately. The MT solver paths are untouched (sleep + `PrepareContacts` run serial).
- **Build (VS2026):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m` (use `/p:Configuration=Dist` for the headless rig).
- **Run tests FROM the exe dir:** `cd "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[physics]"`.
- **clangd diagnostics are FALSE POSITIVES** (expects Clang 20). MSVC + ArcaneTests are truth.
- **Commit message trailers:** end every commit body with
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux`.

## File structure

- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` — SoA arrays `m_maxExtent`, `m_sleepThreshold`; world scalar `m_sleepThresholdDefault`; `WorldDef::sleepThreshold`, `BodyDef::sleepThreshold`; accessors `MaxExtentSlot`/`SleepThresholdSlot`; decl `RecomputeMaxExtent`, `SetAngularVelocity`; `ContactConstraint::bodyBIsStatic` (Step 2).
- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — `RecomputeMaxExtent` impl; wire into `AddBody` + `RecomputeBodyMass`; per-body `sleepThreshold` set in `AddBody`; `m_sleepThresholdDefault` from ctor; `SetAngularVelocity` impl; `moverIsMoving` rewrite (`~2349`); `EmitContactConstraints` sets `bodyBIsStatic` (Step 2).
- `Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp` — `ContactConstraint::bodyBIsStatic` field (Step 2).
- `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` — `PrepareContacts` static/dynamic softness split (Step 2); hertz-cap `0.25→0.125` (Step 3).
- `Arcane/Core/src/Arcane/Physics/Island.hpp` — retire `kSleepLinVel2`/`kSleepAngVel`; keep `kSleepTime`.
- `Arcane/Core/src/Arcane/Physics/Island.cpp` — combined sleep predicate.
- `Arcane/Tests/src/PhysicsSleepThresholdTest.cpp` — NEW: maxExtent, threshold override, combined-predicate sleep tests.
- Re-baseline (audit, mostly should stay green): `PhysicsAwakeSetTest.cpp`, `PhysicsIslandTest.cpp`, `PhysicsStaticSettleTest.cpp`.

---

## Step 1 — Box2D-faithful sleep test (commit 1)

### Task 1: Per-body `maxExtent` (cached)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (SoA decl + accessor + `RecomputeMaxExtent` decl, near `m_sleepTimer` ~1267 and `RecomputeBodyMass` decl)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`RecomputeMaxExtent` impl; calls in `AddBody` ~1051 and `RecomputeBodyMass` ~388; resize site ~196)
- Test: `Arcane/Tests/src/PhysicsSleepThresholdTest.cpp` (new)

**Interfaces:**
- Produces: `Real PhysicsWorld::MaxExtentSlot(std::uint32_t i) const noexcept` — max distance from body COM to any fixture point (vert distance + shape radius), body-local. `void PhysicsWorld::RecomputeMaxExtent(std::uint32_t slot)`.

- [ ] **Step 1: Write the failing test** — add to a NEW file `Arcane/Tests/src/PhysicsSleepThresholdTest.cpp`:

```cpp
// Sleep-threshold + maxExtent + combined-predicate tests (Box2D-faithful sleep).
// PRESENTATION-FREE + C++23-clean.
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
using namespace Arcane::Physics;
namespace { constexpr Real kStep = Real(1) / Real(60); }

TEST_CASE("PhysicsSleep: maxExtent equals circle radius for a centered circle", "[physics][sleep]")
{
    WorldDef wd; PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1);
    const BodyHandle b = w.AddBody(d);
    const std::uint32_t s = w.SlotOf(b);
    CHECK_THAT(static_cast<double>(w.MaxExtentSlot(s)),
               Catch::Matchers::WithinAbs(10.0, 1e-4));
}

TEST_CASE("PhysicsSleep: maxExtent equals box half-diagonal for an AABB", "[physics][sleep]")
{
    WorldDef wd; PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeAabb(Real(3), Real(4)); d.density = Real(1); d.fixedRotation = true;
    const BodyHandle b = w.AddBody(d);
    const std::uint32_t s = w.SlotOf(b);
    // corner distance from center = sqrt(3^2 + 4^2) = 5, radius 0.
    CHECK_THAT(static_cast<double>(w.MaxExtentSlot(s)),
               Catch::Matchers::WithinAbs(5.0, 1e-4));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `.\ArcaneTests.exe "[physics][sleep]"`
Expected: COMPILE FAIL — `MaxExtentSlot` / `SlotOf` not members. (If `SlotOf` already exists, only `MaxExtentSlot` fails.)

- [ ] **Step 3: Add the SoA array + accessor + helper decl** in `PhysicsWorld.hpp`.

Next to `m_sleepTimer` (~line 1267):
```cpp
            std::vector<Real>          m_sleepTimer;          // island sleep (P2.4)
            std::vector<Real>          m_maxExtent;           // body COM->farthest-point dist (sleep test)
            std::vector<Real>          m_sleepThreshold;      // per-body sleep speed gate (px/s)
```
In the public accessor region (next to `SleepTimerSlot` ~934):
```cpp
            [[nodiscard]] Real MaxExtentSlot(std::uint32_t i) const noexcept { return m_maxExtent[i]; }
            [[nodiscard]] Real SleepThresholdSlot(std::uint32_t i) const noexcept { return m_sleepThreshold[i]; }
```
In the private method region (near the `RecomputeBodyMass` declaration):
```cpp
            // Recompute the cached COM->farthest-fixture-point distance (+radius)
            // used by the sleep velocity test. Call after COM is finalized.
            void RecomputeMaxExtent(std::uint32_t slot);
```
Add a public `SlotOf` ONLY if it does not already exist (check first; many handle->slot resolvers exist). If absent, add next to `HandleOf`:
```cpp
            [[nodiscard]] std::uint32_t SlotOf(BodyHandle h) const noexcept; // handle -> SoA slot
```

- [ ] **Step 4: Resize the new arrays** wherever `m_sleepTimer` is resized in `PhysicsWorld.cpp` (~line 196, the capacity-grow site):
```cpp
            m_sleepTimer.resize(next, Real(0));
            m_maxExtent.resize(next, Real(0));
            m_sleepThreshold.resize(next, Real(0));
```

- [ ] **Step 5: Implement `RecomputeMaxExtent`** in `PhysicsWorld.cpp` (place after `RecomputeBodyMass`, ~line 397):
```cpp
        void PhysicsWorld::RecomputeMaxExtent(std::uint32_t slot)
        {
            // maxExtent = max over fixtures of ( max over core verts of
            // |vert_bodyLocal - COM| + shape.radius ). Body-local frame; COM is
            // m_localCenter. Box2D's sim->maxExtent. Used by the sleep velocity
            // test ( |v| + |w|*maxExtent ). 0 for a body with no fixtures.
            Real maxExt = Real(0);
            const Real comX = m_localCenterX[slot];
            const Real comY = m_localCenterY[slot];
            for (const std::uint32_t fi : m_bodyFixtures[slot])
            {
                if (fi >= m_fxCount || m_fxGen[fi] == 0u) { continue; }
                const Shape& s = m_fxShape[fi];
                const Real la = m_fxLocalAngle[fi];
                const Real lc = std::cos(la), ls = std::sin(la);
                const Real lpx = m_fxLocalPosX[fi], lpy = m_fxLocalPosY[fi];
                for (const Vec2& v : s.verts)
                {
                    // vert in body-local frame: localPos + R(localAngle)*v
                    const Real px = lpx + lc * v.x - ls * v.y;
                    const Real py = lpy + ls * v.x + lc * v.y;
                    const Real dx = px - comX, dy = py - comY;
                    const Real d = std::sqrt(dx * dx + dy * dy) + s.radius;
                    if (d > maxExt) { maxExt = d; }
                }
            }
            m_maxExtent[slot] = maxExt;
        }
```
If `SlotOf` was added in Step 3, implement it mirroring the existing handle->slot resolution used by e.g. `Position(BodyHandle)` (find that resolver and reuse it).

- [ ] **Step 6: Wire the helper into both mass paths.**

In `RecomputeBodyMass`, immediately after the COM stores (~line 388 `m_localCenterY[bodySlot] = comY;` and before `m_invInertia` store, OR at the very end before the closing brace) add:
```cpp
            RecomputeMaxExtent(bodySlot);
```
Also add `m_maxExtent[bodySlot] = Real(0);` in the two early-return branches of `RecomputeBodyMass` (static/kinematic ~277, and the degenerate-mass ~349) so a recycled slot has no stale extent.

In `AddBody`, at the END of the auto-fixture block, immediately after the single-fixture COM stores (~line 1051 `m_localCenterY[idx] = md.centroid.y;` / the `else` branch ~1057), and after the closing brace of that `{ ... }` block, add:
```cpp
            RecomputeMaxExtent(idx);
```
(At that point `m_bodyFixtures[idx]` holds the auto-fixture and `m_localCenterX/Y[idx]` is final for both the Dynamic and Static/Kinematic branches.)

- [ ] **Step 7: Run to verify the maxExtent tests pass**

Build (Debug), then `.\ArcaneTests.exe "[physics][sleep]"`
Expected: the two maxExtent tests PASS.

- [ ] **Step 8: Commit** (folded into Step 1's final commit after Task 4 — do NOT commit mid-step-1; Tasks 1-4 are one commit. Run the build green and continue to Task 2.)

### Task 2: Per-body `sleepThreshold` params (WorldDef + BodyDef + override)

**Files:**
- Modify: `PhysicsWorld.hpp` (`WorldDef::sleepThreshold`, `BodyDef::sleepThreshold`, `m_sleepThresholdDefault`)
- Modify: `PhysicsWorld.cpp` (mirror in ctor; set per-body in `AddBody`)
- Test: `PhysicsSleepThresholdTest.cpp`

**Interfaces:**
- Consumes: `MaxExtentSlot`, `SleepThresholdSlot` (Task 1).
- Produces: `WorldDef::sleepThreshold` (Real, default `Real(5)`), `BodyDef::sleepThreshold` (Real, default `Real(-1)` = inherit), per-body `m_sleepThreshold[slot]`.

- [ ] **Step 1: Write the failing test** (append to `PhysicsSleepThresholdTest.cpp`):
```cpp
TEST_CASE("PhysicsSleep: per-body sleepThreshold override beats the world default", "[physics][sleep]")
{
    WorldDef wd; wd.sleepThreshold = Real(5); PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1);
    const BodyHandle inherit = w.AddBody(d);
    d.sleepThreshold = Real(12);                 // per-body override
    const BodyHandle override = w.AddBody(d);
    CHECK(w.SleepThresholdSlot(w.SlotOf(inherit))  == Real(5));
    CHECK(w.SleepThresholdSlot(w.SlotOf(override)) == Real(12));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `.\ArcaneTests.exe "PhysicsSleep: per-body sleepThreshold override*"`
Expected: COMPILE FAIL — `WorldDef::sleepThreshold` / `BodyDef::sleepThreshold` not members.

- [ ] **Step 3: Add the params.** In `PhysicsWorld.hpp`:

`BodyDef` (after `linearDamping`, ~line 147):
```cpp
            // Per-body sleep speed gate (px/s). < 0 inherits WorldDef::sleepThreshold.
            Real sleepThreshold = Real(-1);
```
`WorldDef` (next to `contactPushMaxVelocity`, ~line 222):
```cpp
            // Sleep speed gate (px/s): a body is idle when |v| + |w|*maxExtent
            // < sleepThreshold. Box2D b2DefaultBodyDef is 0.05 m/s; in this pixel
            // world ~5 px/s (tuned for the gravity-900 sandbox; see findings doc).
            Real sleepThreshold = Real(5);
```
Add the world scalar member next to the other mirrored params (find where `m_contactPushMaxVelocity` or equivalent is stored; if the world reads `WorldDef` fields via stored members, add `m_sleepThresholdDefault`; if it stores the whole `WorldDef`, skip and read `m_def.sleepThreshold`). Add accessor if the pattern uses accessors:
```cpp
            [[nodiscard]] Real SleepThresholdDefault() const noexcept { return m_sleepThresholdDefault; }
```

- [ ] **Step 4: Mirror in ctor + set per-body in `AddBody`.**

In the `PhysicsWorld` ctor where the other contact params are copied from `WorldDef`, add:
```cpp
            m_sleepThresholdDefault = def.sleepThreshold;
```
In `AddBody`, next to `m_sleepTimer[idx] = Real(0);` (~line 884):
```cpp
            m_sleepThreshold[idx] = (def.sleepThreshold >= Real(0))
                                        ? def.sleepThreshold
                                        : m_sleepThresholdDefault;
```

- [ ] **Step 5: Run to verify it passes**

Run: `.\ArcaneTests.exe "[physics][sleep]"`
Expected: all `[physics][sleep]` tests so far PASS.

- [ ] **Step 6:** Continue to Task 3 (no separate commit).

### Task 3: `SetAngularVelocity` (scaffolding the sleep test needs)

**Files:**
- Modify: `PhysicsWorld.hpp` (decl next to `SetVelocity` ~433)
- Modify: `PhysicsWorld.cpp` (impl mirroring `SetVelocity`)

**Interfaces:**
- Produces: `void PhysicsWorld::SetAngularVelocity(BodyHandle h, Real w)` — sets angular velocity and wakes the body (mirrors `SetVelocity`).

- [ ] **Step 1: Read `SetVelocity`'s implementation** (`grep "void PhysicsWorld::SetVelocity"`) to copy its handle-resolution + wake pattern exactly.

- [ ] **Step 2: Declare** in `PhysicsWorld.hpp` after `SetVelocity` (~433):
```cpp
            void SetAngularVelocity(BodyHandle h, Real w);
```

- [ ] **Step 3: Implement** in `PhysicsWorld.cpp` next to `SetVelocity`, mirroring its handle resolve + wake (use the same `IsValid`/slot-resolve + `Wake`/`m_angVel[slot]=w` the linear setter uses):
```cpp
        void PhysicsWorld::SetAngularVelocity(BodyHandle h, Real w)
        {
            // Mirror SetVelocity: resolve the handle, set angular velocity, wake
            // the body (a deliberate velocity change must un-sleep it).
            if (!IsValid(h)) { return; }
            const std::uint32_t s = SlotOf(h);
            m_angVel[s] = w;
            Wake(h);
        }
```
(Match the EXACT guard/resolve/wake calls `SetVelocity` uses; the above is the shape — adjust names to the real linear setter.)

- [ ] **Step 4: Build** (Debug) to confirm it compiles. Continue to Task 4.

### Task 4: Combined sleep predicate + wake consistency + retire constants (HEADLINE FIX) — commit Step 1

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Island.cpp` (sleep idle test ~78)
- Modify: `Arcane/Core/src/Arcane/Physics/Island.hpp` (retire 2 constants ~93/96)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`moverIsMoving` ~2349)
- Test: `PhysicsSleepThresholdTest.cpp`

**Interfaces:**
- Consumes: `MaxExtentSlot`, `SleepThresholdSlot`, `SetAngularVelocity` (Tasks 1-3).

- [ ] **Step 1: Write the failing headline test** (append to `PhysicsSleepThresholdTest.cpp`):
```cpp
// THE regression guard this bug lacked: a slowly-ROLLING body (the never-settle
// blocker class) must sleep. With the OLD separate gates (|w| < 0.05) a body at
// w=0.08 never idled -> never slept. The combined test ( |v| + |w|*maxExtent )
// counts 0.08*10 = 0.8 px/s < 5 -> idle -> sleeps.
TEST_CASE("PhysicsSleep: a slowly-rolling body sleeps (combined test)", "[physics][sleep]")
{
    WorldDef wd; wd.gravityY = Real(0); wd.sleepThreshold = Real(5); PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1); // NOT fixedRotation -> can roll
    const BodyHandle b = w.AddBody(d);
    w.SetAngularVelocity(b, Real(0.08));         // rolls at 0.08 rad/s; |v|=0
    for (int k = 0; k < 120; ++k) { w.Step(kStep); } // > kSleepTime (0.5s = 30 steps)
    REQUIRE_FALSE(w.IsAwake(b));                  // combined sleepVel 0.8 < 5 -> sleeps
}
// Counter-guard: a body genuinely spinning fast must NOT sleep.
TEST_CASE("PhysicsSleep: a fast-spinning body never sleeps", "[physics][sleep]")
{
    WorldDef wd; wd.gravityY = Real(0); wd.sleepThreshold = Real(5); PhysicsWorld w(wd);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(0));
    d.shape = MakeCircle(Real(10)); d.density = Real(1);
    const BodyHandle b = w.AddBody(d);
    w.SetAngularVelocity(b, Real(2.0));          // 2.0*10 = 20 px/s >> 5
    for (int k = 0; k < 120; ++k) { w.Step(kStep); REQUIRE(w.IsAwake(b)); }
}
```

- [ ] **Step 2: Run to verify the rolling test fails (RED)**

Run: `.\ArcaneTests.exe "PhysicsSleep: a slowly-rolling body sleeps*"`
Expected: FAIL — with the current separate-gate predicate (`|w| < 0.05`), `w=0.08` never idles, so `IsAwake` stays true and `REQUIRE_FALSE` fails. (The fast-spinning counter-test PASSES already.)

- [ ] **Step 3: Rewrite the sleep idle test** in `Island.cpp` (the block ~64-86). Replace:
```cpp
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
```
with the Box2D-faithful combined, extent-weighted, per-body-thresholded test:
```cpp
                // ---- per-body idle-timer update (awake dynamics) ----------------
                // Box2D v3 b2FinalizeBodiesTask: a body is idle when its combined
                // linear+angular speed |v| + |w|*maxExtent is below its per-body
                // sleepThreshold. The angular term is weighted by the body's
                // farthest-point extent so a slow roll on a large body counts as
                // motion proportional to how fast its surface moves -- this is what
                // lets a circle with a small residual roll (the never-settle blocker
                // class) finally sleep. Otherwise reset to 0.
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    if (!world.Alive(i) ||
                        world.TypeSlot(i) != BodyType::Dynamic ||
                        !world.AwakeSlot(i))
                    {
                        continue;
                    }
                    const Vec2 v  = world.VelSlot(i);
                    const Real wv = world.AngVelSlot(i);
                    const Real sleepVel = std::sqrt(v.x * v.x + v.y * v.y)
                                        + std::fabs(wv) * world.MaxExtentSlot(i);
                    if (sleepVel < world.SleepThresholdSlot(i))
                    {
                        world.SetSleepTimerSlot(i, world.SleepTimerSlot(i) + dt);
                    }
                    else
                    {
                        world.SetSleepTimerSlot(i, Real(0));
                    }
                }
```
Update the block comment at ~65 to match (remove the `kSleepLinVel2`/`kSleepAngVel` reference).

- [ ] **Step 4: Update the wake predicate** in `PhysicsWorld.cpp` `moverIsMoving` (~2349) to the SAME test so wake mirrors sleep:
```cpp
            auto moverIsMoving = [&](std::uint32_t s) -> bool {
                // Same combined test as Island::UpdateSleep: a mover "is moving"
                // (and thus wakes a sleeping neighbour) iff it is NOT idle, i.e.
                // |v| + |w|*maxExtent >= its sleepThreshold. Keeps wake + sleep
                // predicates consistent at the threshold margin.
                const Real lin = std::sqrt(m_velX[s] * m_velX[s] + m_velY[s] * m_velY[s]);
                return (lin + std::fabs(m_angVel[s]) * m_maxExtent[s]) >= m_sleepThreshold[s];
            };
```

- [ ] **Step 5: Retire the dead constants** in `Island.hpp`. Delete `kSleepLinVel2` (~93) and `kSleepAngVel` (~96). KEEP `kSleepTime`. Update the surrounding comment (lines ~89-96) to describe the combined per-body test. Build will flag any remaining referencer (there should be none after Steps 3-4; the throwaway PENPROF block at `PhysicsWorld.cpp:1714/1745` still references them and is reverted in Task 7 — temporarily switch those two lines to the combined form OR guard the build: replace the PENPROF `fastL/fastA` lines with `const bool fastL = (std::sqrt(v2) + ang * m_maxExtent[s]) >= m_sleepThreshold[s]; const bool fastA = false;` to keep the throwaway compiling until Task 7 removes it).

- [ ] **Step 6: Run the new tests (GREEN)**

Run: `.\ArcaneTests.exe "[physics][sleep]"`
Expected: ALL `[physics][sleep]` tests PASS (rolling body now sleeps; fast-spinning never sleeps; maxExtent + threshold tests still pass).

- [ ] **Step 7: Run the existing sleep tests — keep green / re-baseline**

Run: `.\ArcaneTests.exe "[physics][awakeset] [physics][island]" ; .\ArcaneTests.exe "[physics]"`
Expected: PASS. The existing pile/settle tests use `fixedRotation` boxes at gravity 400 and should sleep at least as readily with the more-forgiving predicate. If any "still awake at step K" assertion now flips (a borderline body sleeps a few steps earlier), re-bless it deliberately and note why in the commit. Update the now-stale comment in `PhysicsAwakeSetTest.cpp:124-127` ("|v| < 2.0") to reference the new combined `sleepThreshold`.

- [ ] **Step 8: Validate + tune the default on the repro rig.** Build Dist, then from `Arcane/bin/Dist-windows-x86_64-md/Loom/`:
```
set ARCANE_SANDBOX_SCENE=8 & set ARCANE_NO_WHISK=1 & set ARCANE_PILE_COUNT=300 & set ARCANE_PENPROF=1 & set ARCANE_PENPROF_EVERY=300
.\Loom.exe --frames 6000
```
Confirm `awake` drains toward 0 (pile sleeps). Repeat at `ARCANE_PILE_COUNT=1000`. If the pile does NOT fully sleep, raise `WorldDef::sleepThreshold` default (Task 2 Step 3) by ~1 px/s and re-run; pick the lowest value that reliably sleeps both. Record the chosen value + the before/after `awake` counts in the commit message. Re-confirm the whisk-on scene still never sleeps (`unset ARCANE_NO_WHISK`).

- [ ] **Step 9: Commit Step 1**
```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp \
        Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp \
        Arcane/Core/src/Arcane/Physics/Island.hpp \
        Arcane/Core/src/Arcane/Physics/Island.cpp \
        Arcane/Tests/src/PhysicsSleepThresholdTest.cpp
# (also `git add` any re-baselined existing test files touched in Step 7)
git commit  # message: feat(arcane/physics): Box2D-faithful combined sleep test (fixes never-settle)
```
Include in the body: the root cause one-liner, the chosen `sleepThreshold` default + rig evidence (awake N->~0), and that `kSleepLinVel2`/`kSleepAngVel` are retired. End with the required trailers.

---

## Step 2 — `staticSoftness` for dynamic-vs-static contacts (commit 2)

### Task 5: Stiffer soft coefficients vs the static world

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp` (`ContactConstraint::bodyBIsStatic`)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`EmitContactConstraints` sets the flag ~3030)
- Modify: `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` (`PrepareContacts` ~203-221)
- Test: `PhysicsSleepThresholdTest.cpp` (or a new `PhysicsStaticSoftnessTest.cpp`)

**Interfaces:**
- Consumes: `MakeSoft` (`SoftCoeffs.hpp`), `ContactConstraint` (`Solver.hpp`).
- Produces: `ContactConstraint::bodyBIsStatic` (bool); `PrepareContacts` selects `staticSoft = MakeSoft(2*contactHertz, ...)` for static contacts.

- [ ] **Step 1: Write the failing test** — assert a static-flagged constraint gets the 2×-hertz soft coefficients. Simplest deterministic approach: a unit test that builds two `ContactConstraint`s (one `bodyBIsStatic=true`, one `false`), runs `PrepareContacts` via a minimal world, and checks `biasRate`. Because `PrepareContacts` is a private `SoftStep` method, prefer a behavioral test instead: a dynamic box resting on a STATIC floor settles to LESS penetration than the same box resting on a (heavy) DYNAMIC platform, after N steps. Add:
```cpp
TEST_CASE("PhysicsStaticSoftness: dyn-vs-static is stiffer than dyn-vs-dynamic", "[physics][softness]")
{
    auto restPenetration = [](bool floorStatic) {
        WorldDef wd; wd.gravityY = Real(400); PhysicsWorld w(wd);
        BodyDef fd; fd.position = Vec2(Real(0), Real(10)); fd.shape = MakeAabb(Real(200), Real(10));
        fd.type = floorStatic ? BodyType::Static : BodyType::Dynamic;
        fd.density = Real(1000); fd.fixedRotation = true; // heavy if dynamic
        const BodyHandle floor = w.AddBody(fd);
        BodyDef bd; bd.type = BodyType::Dynamic; bd.position = Vec2(Real(0), Real(-6));
        bd.shape = MakeAabb(Real(5), Real(5)); bd.density = Real(1); bd.fixedRotation = true;
        const BodyHandle box = w.AddBody(bd);
        for (int k = 0; k < 400; ++k) { w.Step(kStep); }
        (void)floor;
        return w.Position(box).y; // larger y = deeper into the floor (y down)
    };
    // Stiffer static contact => box rests slightly higher (less sink) than on a
    // softer dynamic platform. Assert the static case does not sink MORE.
    CHECK(restPenetration(true) <= restPenetration(false) + Real(0.01));
}
```
(If this proves too coupled, fall back to a direct unit check by making `PrepareContacts` test-visible via a friend or a small public `DebugPrepareContacts` hook — decide during implementation; keep the assertion on `biasRate` of a static vs dynamic constraint.)

- [ ] **Step 2: Run to verify it fails/baselines**

Run: `.\ArcaneTests.exe "[physics][softness]"`
Expected: PASS or FAIL depending on current behavior — record the baseline number. (Before the change, static and dynamic contacts use the SAME softness, so the two penetrations are near-equal; the `<=` may already hold by luck. The test's VALUE is as a regression guard post-change; if it can't distinguish, replace with the direct `biasRate` unit check below.)

- [ ] **Step 3: Add the flag** in `Solver.hpp` `ContactConstraint` (after `bodyBIsBody`, ~112):
```cpp
            // B is the immovable STATIC world (a static body or a tile span), NOT
            // a kinematic body. Selects the stiffer staticSoftness in PrepareContacts
            // (Box2D: dyn-vs-static contacts use 2x the contact hertz).
            bool bodyBIsStatic = false;
```

- [ ] **Step 4: Set the flag** in `EmitContactConstraints` (`PhysicsWorld.cpp` ~3030, where `cc.bodyBIsBody`/`cc.invMassB` are set):
```cpp
            cc.bodyBIsBody = bIsBody;
            // Static world = a tile span (no body) OR a Static-typed body. A
            // kinematic B stays on the normal (softer) contact softness.
            cc.bodyBIsStatic = !bIsBody ||
                (static_cast<BodyType>(m_btype[bIdx]) == BodyType::Static);
```

- [ ] **Step 5: Split the softness** in `SoftStep::PrepareContacts` (`SoftStep.cpp` ~210-221). Replace the single `contactSoft` setup:
```cpp
            const Real h = ctx.subDt;
            const Real maxHertz = (h > Real(0)) ? (Real(0.25) / h) : w.ContactHertz();
            const Real contactHertz = std::min(w.ContactHertz(), maxHertz);
            const SoftCoeffs contactSoft = MakeSoft(contactHertz, w.ContactDampingRatio(), h);
```
with two coefficient sets (Box2D uses 2x hertz vs static):
```cpp
            const Real h = ctx.subDt;
            const Real maxHertz = (h > Real(0)) ? (Real(0.25) / h) : w.ContactHertz();
            const Real contactHertz = std::min(w.ContactHertz(), maxHertz);
            const SoftCoeffs contactSoft = MakeSoft(contactHertz, w.ContactDampingRatio(), h);
            const SoftCoeffs staticSoft  = MakeSoft(Real(2) * contactHertz, w.ContactDampingRatio(), h);
```
and in the per-constraint loop, replace the three `cc.* = contactSoft.*` assignments:
```cpp
                const SoftCoeffs& soft = cc.bodyBIsStatic ? staticSoft : contactSoft;
                cc.biasRate     = soft.biasRate;
                cc.massScale    = soft.massScale;
                cc.impulseScale = soft.impulseScale;
```

- [ ] **Step 6: Build + run the softness test + full physics suite**

Run: `.\ArcaneTests.exe "[physics][softness]"` then `.\ArcaneTests.exe "[physics]"`
Expected: softness test PASS. Physics suite: stacking/static-settle behavioral tests that assert exact rest positions/penetration WILL shift (this is the intended broad re-baseline) — re-bless deliberately, confirming each new value is physically sensible (slightly less sink against statics), not a blow-up.

- [ ] **Step 7: Validate on the rig.** Re-run the Dist no-whisk pile (Task 4 Step 8 commands); confirm reduced residual jitter / faster blocker drain and no new instability (penetration stays shallow, no velocity growth).

- [ ] **Step 8: Commit Step 2**
```bash
git add Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp \
        Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp \
        Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp \
        Arcane/Tests/src/PhysicsSleepThresholdTest.cpp
# (+ any re-baselined static/stacking test files)
git commit  # message: feat(arcane/physics): staticSoftness (2x hertz) for dyn-vs-static contacts
```
End with the required trailers; note the re-baselined tests + rig evidence.

---

## Step 3 — hertz-cap alignment (commit 3)

### Task 6: `0.25/h → 0.125/h`

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` (~211)

- [ ] **Step 1: Change the cap** in `PrepareContacts` (~211). Replace:
```cpp
            // High contact hertz that, with the small sub-step, appears rigid --
            // but never out-running the sub-step solve rate (Box2D v3 clamps to
            // 0.25 * substepCount / dt == 0.25 / h).
            const Real maxHertz = (h > Real(0)) ? (Real(0.25) / h) : w.ContactHertz();
```
with:
```cpp
            // Clamp contact hertz so it never out-runs the sub-step solve rate.
            // Box2D v3 (physics_world.c) clamps to 0.125 * inv_h == 0.125 / h.
            // No effect at the default 4 substeps (min(30, 30)); correct for any
            // sub-4 substep count.
            const Real maxHertz = (h > Real(0)) ? (Real(0.125) / h) : w.ContactHertz();
```
(Apply the same `0.125` to the `staticSoft` line's basis only via `contactHertz`, which is already derived from this `maxHertz` — no second edit needed.)

- [ ] **Step 2: Build + run full physics suite**

Run: `.\ArcaneTests.exe "[physics]"`
Expected: PASS with NO re-baseline (at 4 substeps `min(30, 0.125*240)=30` = the old `min(30, 0.25*240)=30`; byte-identical). If anything changes, STOP — it means a test runs sub-4 substeps; investigate before proceeding.

- [ ] **Step 3: Commit Step 3**
```bash
git add Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp
git commit  # message: fix(arcane/physics): align contact hertz cap 0.25/h -> 0.125/h (Box2D v3)
```
End with the required trailers.

---

## Step 4 — revert throwaway instrumentation (commit 4)

### Task 7: Remove the repro instrumentation

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (remove the `ARCANE_PENPROF` block + the two `#include` lines marked `THROWAWAY`)
- Modify: `Arcane/Sandbox/src/Scenes.cpp` (remove the `ARCANE_PILE_COUNT` override marked `THROWAWAY`)

- [ ] **Step 1: Remove the PENPROF block** — delete the entire `// ---- THROWAWAY (ARCANE_PENPROF) ...` block inserted after `EmitContactConstraints` in `Step`, and the two `#include <cstdio>` / `#include <cstdlib>` lines marked `THROWAWAY` near the top. (`ARCANE_NO_WHISK` in `Scenes.cpp` is NOT throwaway — leave it.)

- [ ] **Step 2: Remove the count override** in `Scenes.cpp` `BuildStressTest` — restore it to:
```cpp
        void BuildStressTest(Astra::Registry& reg)
        {
            BuildStressTestImpl(reg, kStressBodyCount);
        }
```

- [ ] **Step 3: Confirm `StepProf.hpp` is untouched** by this work (its `M` state predates this branch; leave it as found unless it was part of the throwaway — it was not).

- [ ] **Step 4: Build + full physics suite**

Run: `.\ArcaneTests.exe "[physics]"`
Expected: PASS.

- [ ] **Step 5: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Sandbox/src/Scenes.cpp
git commit  # message: chore(arcane/physics): revert throwaway never-settle repro instrumentation
```
End with the required trailers.

---

## Final verification

- [ ] Build all configs touched: `msbuild Arcane.slnx /p:Configuration=Debug /m` and `/p:Configuration=Dist /m`.
- [ ] `.\ArcaneTests.exe ~[gpu]` (full non-GPU suite) green.
- [ ] Interactive sanity: `Loom.exe` scene 8 (default whisk) still churns; a hand-built no-whisk pile (or scene 1 "Box stack" / scene 2 "Pyramid") settles AND visibly sleeps (velocity vectors vanish).
- [ ] Push the branch; let CI go green; hand to T3mps for review/merge (team workflow).

## Self-review notes (addressed)

- **Spec coverage:** sleep test (Task 1-4), WorldDef+BodyDef threshold (Task 2), wake consistency (Task 4 Step 4), staticSoftness (Task 5), hertz-cap (Task 6), throwaway revert (Task 7), TDD regression test + re-baseline (throughout). All spec sections mapped.
- **maxExtent on both mass paths** (AddBody single-fixture + RecomputeBodyMass compound) via one helper — covered (Task 1 Step 6).
- **`SlotOf`/`SetVelocity` exact names:** the plan instructs to confirm the real linear-setter + handle-resolver names before mirroring (Task 3 Step 1, Task 1 Step 3) rather than assuming — adjust to the codebase's actual API.
- **Dead-constant referencers:** the throwaway PENPROF block references the retired constants; Task 4 Step 5 patches it to compile until Task 7 deletes it.
