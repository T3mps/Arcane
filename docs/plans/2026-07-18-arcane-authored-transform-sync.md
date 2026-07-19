# Authored-Transform Ownership + Physics Sync — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `LocalTransform` the authored source of truth for physics entities in Arcane's `PhysicsSystem` — pos/rot ownership flips with sim state (stateless paused reconcile), and `lt.scale` resizes the collider.

**Architecture:** All changes live in the Arcane scene layer (`PhysicsComponents.hpp` + `PhysicsSystem.hpp`) — no editor code, no Manifold2D change. Birth-time scale is applied in the CREATE pass and cached on `PhysicsBodyRef.appliedScale`. A new paused-only PASS 3.5 reconciles author edits into the body (pos/rot teleport when diverged; fixture rebuild when scale changes) before the unchanged PASS 4 write-back reflects the now-matching body pose back.

**Tech Stack:** C++23, Astra ECS (reflection + `SystemTraits` views), Manifold2D `PhysicsWorld`, Catch2 tests. Spec: `docs/superpowers/specs/2026-07-18-arcane-authored-transform-sync-design.md`.

## Global Constraints

- **Units are MKS (meters).** Never author px-scale content. `LocalTransform.scale` defaults `{1,1}`.
- **Determinism:** `/fp:precise` (workspace rule — no `/fp:fast`). UTF-8 without BOM, ASCII comments only.
- **`/MD` dynamic CRT** (engine boundary) — unchanged; header-only edits.
- **Arcane stays editor-free.** This is engine `PhysicsSystem` behavior only; Grimoire is not touched. Editor gizmos/undo are SPEC #2, a separate later plan.
- **No serialized-schema change:** `appliedScale` is runtime-only, reflected `Serializable(false)` exactly like `PhysicsBodyRef::handle`; re-seeded on load.
- **Build (MSBuild not on PATH → PowerShell, full path):**
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m`
- **Run tests from the exe's own output dir** (CWD-relative asset paths in the suite):
  `Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location`
- **New test file needs project regen** (premake globs `Tests/src/**.cpp`): `& "Arcane\GenerateProjects.bat"` once after creating `AuthoredTransformSyncTest.cpp`.
- **Commits:** `type(scope): summary` — **NO AI trailers** (this repo's convention; matches the spec commit `8f16b979`).
- **Regression floor:** capture the live `~[gpu]` count before starting (memory's last figure: `~[gpu] 27686/307`). The existing `[physics]` case count must not drop; this plan only ADDS `[transform-sync]` cases.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp` | Modify | Add `PhysicsBodyRef::appliedScale` (runtime baseline) + reflect `Serializable(false)`. |
| `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` | Modify | Add `MakeScaledShape` helper + scale param on `MakeFixtureDef`; apply scale in CREATE pass + seed `appliedScale`; add PASS 3.5 paused reconcile (pos/rot + scale rebuild) + `RebuildScaledFixtures`/`AngleDelta` helpers + epsilon constants. |
| `Arcane/Tests/src/AuthoredTransformSyncTest.cpp` | Create | All `[transform-sync]` tests. |

Verified API (against `ThirdParty/Manifold2D/include/Manifold2D/Physics/PhysicsWorld.hpp`):
`SetPosition`(:421, teleport)/`SetAngle`(:481)/`SetVelocity`(:442)/`SetAngularVelocity`(:446)/`Position`(:416)/`GetAngle`(:480)/`Velocity`(:437)/`AddFixture`(:359)/`DropFixture`(:364)/`FixtureCount`(:370)/`GetBodyFixture(bh,i)`(used in `PhysicsSystemTest.cpp:491`)/`GetFixtureWorldPos`(:376)/`LiveFixtureAabbs`(:765). `Aabb2` = `{ Vec2 min, max }` (Broadphase.hpp:66). Shape builders: `MakeCircle(r)`, `MakeCapsule(halfLen,radius)`, `MakeAabb(halfW,halfH)`.

---

## Task 1: Birth-time scale + `appliedScale` baseline

Apply `lt.scale` to fixtures at body creation and seed the cached baseline. This makes a body authored at scale ≠ 1 born at the correct collider size, and lays the `MakeScaledShape` + `appliedScale` foundation the reconcile pass (Tasks 2–3) reuses.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp` (`PhysicsBodyRef` struct + reflect block)
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` (`MakeFixtureDef`, CREATE pass; new `MakeScaledShape`)
- Create: `Arcane/Tests/src/AuthoredTransformSyncTest.cpp`

**Interfaces:**
- Produces: `struct PhysicsBodyRef { Phys::BodyHandle handle{}; glm::vec2 appliedScale{1,1}; }`
- Produces: `inline Phys::Shape MakeScaledShape(const Fixture& f, glm::vec2 scale)`
- Produces: `inline Phys::FixtureDef MakeFixtureDef(const Fixture& f, glm::vec2 scale = glm::vec2(1,1))` (scale param added; default keeps existing single call site behavior explicit)
- Produces (test helpers, file-local): `Astra::Entity BuildAabbBody(reg, pos, half, type, scale)`, `glm::vec2 Fixture0HalfExtents(reg)`

- [ ] **Step 1: Write the failing tests** — create `Arcane/Tests/src/AuthoredTransformSyncTest.cpp`:

```cpp
// Authored-transform ownership + physics sync (SPEC #1). LocalTransform is the
// authored source of truth for a physics entity; this file exercises the engine
// PhysicsSystem sync. Task 1: birth-time scale + appliedScale baseline.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <vector>

using namespace Arcane;
namespace Phys = Manifold2D::Physics;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    // Build reg + zero-gravity world + one Aabb-fixture body. Gravity is zeroed so
    // a Dynamic body stays put across paused/played steps (position assertions stay
    // deterministic). Returns the body entity.
    Astra::Entity BuildAabbBody(Astra::Registry& reg, glm::vec2 pos, glm::vec2 half,
                                Phys::BodyType type, glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        RegisterSceneComponents(reg);
        RegisterPhysicsComponents(reg);
        Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
        reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });

        Astra::Entity e = reg.CreateEntity();
        LocalTransform lt; lt.position = pos; lt.scale = scale;
        reg.AddComponent<LocalTransform>(e, lt);
        reg.AddComponent<WorldTransform>(e, WorldTransform{});
        RigidBody2D rb; rb.type = type;
        reg.AddComponent<RigidBody2D>(e, rb);
        Collider2D col; Fixture fx;
        fx.kind = Phys::ShapeKind::Aabb; fx.halfW = half.x; fx.halfH = half.y;
        col.fixtures.push_back(fx);
        reg.AddComponent<Collider2D>(e, col);
        reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});
        return e;
    }

    // World-AABB half-extents of fixture[0] (single-fixture body at angle 0 -> the
    // world AABB equals the fixture box, so half-extents == the scaled half-dims).
    glm::vec2 Fixture0HalfExtents(Astra::Registry& reg)
    {
        auto* res = reg.GetResource<PhysicsResource>();
        std::vector<std::uint32_t> fx;
        std::vector<Phys::Aabb2>   boxes;
        res->world->LiveFixtureAabbs(fx, boxes);
        REQUIRE(!boxes.empty());
        const Phys::Aabb2& b = boxes[0];
        return glm::vec2((b.max.x - b.min.x) * 0.5f, (b.max.y - b.min.y) * 0.5f);
    }
}

TEST_CASE("create applies uniform scale to collider", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {2.0f,2.0f});

    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);  // CREATE pass mints the body with scaled fixtures

    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));   // 0.5 * 2
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
    CHECK(reg.GetComponent<PhysicsBodyRef>(e)->appliedScale == glm::vec2(2.0f, 2.0f));
}

TEST_CASE("create leaves scale-1 collider unchanged", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {1.0f,1.0f});
    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(0.5f).margin(1e-4f));
    CHECK(he.y == Approx(0.5f).margin(1e-4f));
}

TEST_CASE("create applies non-uniform scale per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {2.0f,1.0f});
    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));   // 0.5 * 2
    CHECK(he.y == Approx(0.5f).margin(1e-4f));   // 0.5 * 1
}

TEST_CASE("create scales multi-fixture local offsets per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    RegisterPhysicsComponents(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });

    Astra::Entity e = reg.CreateEntity();
    LocalTransform lt; lt.position = {0.0f, 0.0f}; lt.scale = {2.0f, 1.0f};
    reg.AddComponent<LocalTransform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    RigidBody2D rb; rb.type = Phys::BodyType::Kinematic;
    reg.AddComponent<RigidBody2D>(e, rb);
    Collider2D col;
    { Fixture f; f.kind = Phys::ShapeKind::Aabb; f.halfW = 0.3f; f.halfH = 0.3f; col.fixtures.push_back(f); }
    { Fixture f; f.kind = Phys::ShapeKind::Aabb; f.halfW = 0.3f; f.halfH = 0.3f; f.localPos = {2.0f, 0.0f}; col.fixtures.push_back(f); }
    reg.AddComponent<Collider2D>(e, col);
    reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});

    PhysicsSystem sys(kDt, /*stepWorld=*/false);
    sys(reg);

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    REQUIRE(res->world->FixtureCount(bh) == 2u);
    // Body at origin, angle 0: fixture[1] world pos = scaled localPos = (2*2, 0*1) = (4,0).
    const Phys::FixtureHandle fh1 = res->world->GetBodyFixture(bh, 1u);
    const Phys::Vec2 wp = res->world->GetFixtureWorldPos(fh1);
    CHECK(static_cast<float>(wp.x) == Approx(4.0f).margin(1e-4f));
    CHECK(static_cast<float>(wp.y) == Approx(0.0f).margin(1e-4f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run (from repo root):
```powershell
& "Arcane\GenerateProjects.bat"
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: compile error (`appliedScale` not a member of `PhysicsBodyRef`) — a build failure IS the red state for this task; the field/scale wiring does not exist yet.

- [ ] **Step 3: Add `appliedScale` to `PhysicsBodyRef`** — in `PhysicsComponents.hpp`, replace the struct (currently `:145`):

```cpp
    struct PhysicsBodyRef
    {
        Phys::BodyHandle handle{};   // default = kInvalidBody (index=0, gen=0)

        // Runtime baseline: the lt.scale most recently baked into this body's
        // fixtures. The paused reconcile pass rebuilds fixtures when lt.scale
        // diverges from this. Serializable(false), re-seeded on load exactly as
        // `handle` is; the physics world exposes no fixture-dims read-back, so
        // this cached baseline is how a scale change is detected (Unity/Unreal
        // "baked scale" model). Still trivially copyable (glm::vec2 is POD) ->
        // the binary snapshot path memcpies it harmlessly.
        glm::vec2 appliedScale{1.0f, 1.0f};
    };
```

And extend the reflect block (currently `:218`):

```cpp
    ASTRA_REFLECT_TYPE(PhysicsBodyRef)
        ASTRA_REFLECT_FIELD(PhysicsBodyRef, handle)
            ASTRA_REFLECT_ATTR(Serializable, false)
        ASTRA_REFLECT_FIELD(PhysicsBodyRef, appliedScale)
            ASTRA_REFLECT_ATTR(Serializable, false)
    ASTRA_END_REFLECT_TYPE()
```

- [ ] **Step 4: Add `MakeScaledShape` + scale-aware `MakeFixtureDef`** — in `PhysicsSystem.hpp`, add `#include <cmath>` (for `std::abs`/`std::max` — already present at `:77` region via `<cassert>`; add `<algorithm>` and `<cmath>` if missing). Insert `MakeScaledShape` just above `MakeFixtureDef` (`:128`):

```cpp
    // -------------------------------------------------------------------------
    // MakeScaledShape: build a Manifold2D Shape from a Fixture descriptor scaled
    // by an authored LocalTransform.scale. Aabb scales per-axis exact; Circle uses
    // max(|sx|,|sy|) (a circle has no distinguished axis; max never shrinks below
    // the larger authored axis); Capsule scales its length by |sx| and radius by
    // |sy| (a scalar-radius capsule is approximate under non-uniform scale -- the
    // round caps stay circular; documented in the design spec). Uniform scale is
    // exact for every shape.
    // -------------------------------------------------------------------------
    inline Phys::Shape MakeScaledShape(const Fixture& f, glm::vec2 scale)
    {
        const float sx   = std::abs(scale.x);
        const float sy   = std::abs(scale.y);
        const float sMax = std::max(sx, sy);
        switch (f.kind)
        {
        case Phys::ShapeKind::Circle:  return Phys::MakeCircle(f.radius * sMax);
        case Phys::ShapeKind::Capsule: return Phys::MakeCapsule(f.halfLen * sx, f.radius * sy);
        case Phys::ShapeKind::Aabb:    return Phys::MakeAabb(f.halfW * sx, f.halfH * sy);
        case Phys::ShapeKind::Polygon:
            assert(false && "PhysicsSystem: ShapeKind::Polygon not supported");
            return Phys::MakeCircle(f.radius * sMax);
        }
        return Phys::MakeCircle(f.radius * sMax);
    }
```

Then rewrite `MakeFixtureDef` to take a scale and route shape + local offset through it (replace the body of the function `:128-169`):

```cpp
    inline Phys::FixtureDef MakeFixtureDef(const Fixture& f,
                                           glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        Phys::FixtureDef fd;

        // Shape geometry (scaled).
        fd.shape = MakeScaledShape(f, scale);

        // Local transform. Offset scales per-axis with the entity's scale (signed,
        // so a mirrored scale mirrors the offset); localAngle is unaffected.
        fd.localPos   = Phys::Vec2(f.localPos.x * scale.x, f.localPos.y * scale.y);
        fd.localAngle = static_cast<Phys::Real>(f.localAngle);

        // Material.
        fd.density     = static_cast<Phys::Real>(f.density);
        fd.friction    = static_cast<Phys::Real>(f.friction);
        fd.restitution = static_cast<Phys::Real>(f.restitution);

        // Collision filter.
        fd.categoryBits = f.categoryBits;
        fd.maskBits     = f.maskBits;

        fd.isSensor = f.isSensor;

        return fd;
    }
```

- [ ] **Step 5: Apply scale in the CREATE pass + seed `appliedScale`** — in `PhysicsSystem.hpp` PASS 2, replace the primary-fixture shape switch (`:255-270`) with:

```cpp
                    // Primary fixture shape, scaled by the authored LocalTransform.scale.
                    def.shape = MakeScaledShape(fx0, lt.scale);
```

Replace the `def.localPos` line (`:283`) with the scaled offset:

```cpp
                    def.localPos      = Phys::Vec2(fx0.localPos.x * lt.scale.x,
                                                   fx0.localPos.y * lt.scale.y);
```

Pass scale to additional fixtures — in the `fixtures[1..]` loop (`:301-305`):

```cpp
                    for (std::size_t i = 1; i < col.fixtures.size(); ++i)
                    {
                        Phys::FixtureDef fd = MakeFixtureDef(col.fixtures[i], lt.scale);
                        world.AddFixture(handle, fd);
                    }
```

Seed the baseline where the handle is stored (`:312-313`):

```cpp
                    ref.handle       = handle;
                    ref.appliedScale = lt.scale;
                    entityToBody[entity] = handle;
```

- [ ] **Step 6: Run tests to verify they pass**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: PASS (4 assertions/cases). If `LiveFixtureAabbs` returns empty pre-Step (broadphase proxy not yet populated at CREATE), fall back to `res->world->FixtureAabb(res->world->GetBodyFixture(bh,0).index)` for the dims read and confirm the fixture-slot addressing against `PhysicsWorld.hpp:1131`.

- [ ] **Step 7: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp Arcane/Tests/src/AuthoredTransformSyncTest.cpp
git commit -m "feat(arcane): apply LocalTransform.scale to colliders at body create + cache appliedScale"
```

---

## Task 2: Paused pos/rot stateless reconcile

Add PASS 3.5 (paused-only) with the pos/rot branch: when paused, an author edit to `lt.position`/`lt.rotation` is pushed into the body (teleport + zero velocity) before the unchanged PASS 4 reflects it back. Scale branch is added in Task 3.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` (new PASS 3.5 + `AngleDelta` helper + epsilon constants)
- Modify: `Arcane/Tests/src/AuthoredTransformSyncTest.cpp` (append tests)

**Interfaces:**
- Consumes: `PhysicsBodyRef::appliedScale` (Task 1), `MakeFixtureDef(f, scale)` (Task 1).
- Produces: `inline float AngleDelta(float a, float b)` (shortest-arc absolute radians).
- Produces: `inline constexpr float kAuthorPosEps` / `kAuthorRotEps`.

- [ ] **Step 1: Write the failing tests** — append to `AuthoredTransformSyncTest.cpp`:

```cpp
TEST_CASE("paused author move teleports body, zeroes velocity, is not stomped", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mint at (0,0)

    // Author moves the entity while paused (as a gizmo/inspector edit would).
    reg.GetComponent<LocalTransform>(e)->position = glm::vec2(5.0f, 5.0f);
    paused(reg);                                   // reconcile: body <- lt, then write-back

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    const Phys::Vec2 bp = res->world->Position(bh);
    const Phys::Vec2 bv = res->world->Velocity(bh);
    CHECK(static_cast<float>(bp.x) == Approx(5.0f).margin(1e-4f));
    CHECK(static_cast<float>(bp.y) == Approx(5.0f).margin(1e-4f));
    CHECK(static_cast<float>(bv.x) == Approx(0.0f).margin(1e-4f));
    CHECK(static_cast<float>(bv.y) == Approx(0.0f).margin(1e-4f));
    // Not stomped back to (0,0) by the same frame's write-back.
    const glm::vec2 lp = reg.GetComponent<LocalTransform>(e)->position;
    CHECK(lp.x == Approx(5.0f).margin(1e-4f));
    CHECK(lp.y == Approx(5.0f).margin(1e-4f));
}

TEST_CASE("paused author rotate sets body angle, is not stomped", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);

    reg.GetComponent<LocalTransform>(e)->rotation = 1.0f;   // radians
    paused(reg);

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    CHECK(static_cast<float>(res->world->GetAngle(bh)) == Approx(1.0f).margin(1e-4f));
    CHECK(reg.GetComponent<LocalTransform>(e)->rotation == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("author-while-paused then play resumes from authored pose", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<LocalTransform>(e)->position = glm::vec2(3.0f, 0.0f);
    paused(reg);                                   // reconcile pushes (3,0) into the body

    PhysicsSystem play(kDt, /*stepWorld=*/true);
    play(reg);                                     // stepping resumes from (3,0), no snap-back

    const glm::vec2 lp = reg.GetComponent<LocalTransform>(e)->position;
    CHECK(lp.x == Approx(3.0f).margin(1e-3f));     // kinematic, zero velocity -> stays at 3
    CHECK(lp.y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("play mode ignores author lt edits (body owns pose)", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    PhysicsSystem play(kDt, /*stepWorld=*/true);
    play(reg);                                     // mint + step; body at ~origin

    reg.GetComponent<LocalTransform>(e)->position = glm::vec2(9.0f, 9.0f); // bogus author edit
    play(reg);                                     // Play: PASS 4 overwrites lt from the body

    const glm::vec2 lp = reg.GetComponent<LocalTransform>(e)->position;
    CHECK(lp.x == Approx(0.0f).margin(1e-3f));     // body owns; edit discarded
    CHECK(lp.y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("untouched paused body is not spuriously teleported", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    for (int i = 0; i < 5; ++i) paused(reg);       // no author edits between ticks

    const glm::vec2 lp = reg.GetComponent<LocalTransform>(e)->position;
    CHECK(lp.x == Approx(2.0f).margin(1e-4f));
    CHECK(lp.y == Approx(3.0f).margin(1e-4f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: `paused author move...` and `paused author rotate...` and `author-while-paused then play...` FAIL — the paused edit is stomped by PASS 4 (body still at mint pose, `lt` overwritten). `play mode ignores...` and `untouched paused...` should already PASS (they assert existing behavior); that's fine.

- [ ] **Step 3: Add `AngleDelta` + epsilon constants** — in `PhysicsSystem.hpp`, add near `MakeScaledShape` (namespace scope, header-only):

```cpp
    // Author-edit detection tolerances. pos in meters, rot in radians. Small: they
    // only guard against SetAngle->GetAngle normalization round-trip noise, not any
    // meaningful author nudge (a real gizmo/inspector edit is orders larger).
    inline constexpr float kAuthorPosEps = 1e-5f;
    inline constexpr float kAuthorRotEps = 1e-5f;

    // Shortest-arc absolute angle difference (radians).
    inline float AngleDelta(float a, float b)
    {
        constexpr float kPi  = 3.14159265358979323846f;
        constexpr float kTau = 6.28318530717958647692f;
        float d = a - b;
        while (d >  kPi) d -= kTau;
        while (d < -kPi) d += kTau;
        return std::abs(d);
    }
```

- [ ] **Step 4: Add PASS 3.5 (pos/rot branch)** — in `PhysicsSystem.hpp` `operator()`, insert between the STEP block (ends `:358`) and PASS 4 WRITE-BACK (begins `:360`):

```cpp
            // ------------------------------------------------------------------
            // PASS 3.5: AUTHOR RECONCILE (paused only). When the sim is frozen the
            // AUTHOR owns pos/rot: push a diverged LocalTransform edit into the body
            // BEFORE PASS 4 reflects the (now-matching) body pose back. Stateless --
            // a paused body cannot move itself, so the live body pose is the baseline
            // and any divergence is an author edit. Skipped while stepping (Play =
            // body owns pos/rot; PASS 4 drives lt as before). Scale handled in Task 3.
            // ------------------------------------------------------------------
            if (!m_stepWorld)
            {
                auto view = reg.CreateView<PhysicsBodyRef, LocalTransform, Collider2D, RigidBody2D>();
                view.ForEach([&](Astra::Entity   /*entity*/,
                                 PhysicsBodyRef&  ref,
                                 LocalTransform&  lt,
                                 Collider2D&      /*col*/,
                                 RigidBody2D&     /*rb*/)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))       return;

                    // POS/ROT: stateless author reconcile.
                    const Phys::Vec2 bp = world.Position(ref.handle);
                    const float      ba = static_cast<float>(world.GetAngle(ref.handle));
                    if (std::abs(lt.position.x - static_cast<float>(bp.x)) > kAuthorPosEps ||
                        std::abs(lt.position.y - static_cast<float>(bp.y)) > kAuthorPosEps ||
                        AngleDelta(lt.rotation, ba) > kAuthorRotEps)
                    {
                        world.SetPosition(ref.handle, Phys::Vec2(lt.position.x, lt.position.y));
                        world.SetAngle(ref.handle, static_cast<Phys::Real>(lt.rotation));
                        world.SetVelocity(ref.handle, Phys::Vec2(0.0f, 0.0f));
                        world.SetAngularVelocity(ref.handle, static_cast<Phys::Real>(0));
                    }
                });
            }
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: PASS (all Task 1 + Task 2 cases). Also confirm no regression in the adjacent physics suites:
```powershell
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[physics][pause]"; Pop-Location
```
Expected: PASS — the reconcile pass must not perturb `PhysicsSystem no-step skips contact generation` (minted bodies have `appliedScale == lt.scale` and `body == lt`, so reconcile no-ops).

- [ ] **Step 6: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp Arcane/Tests/src/AuthoredTransformSyncTest.cpp
git commit -m "feat(arcane): paused author-reconcile for LocalTransform pos/rot (stateless, un-stomps inspector/gizmo edits)"
```

---

## Task 3: Paused scale → collider rebuild

Add the scale branch to PASS 3.5: when `lt.scale` diverges from `appliedScale`, rebuild the body's fixtures at the new scaled dims (adds new before dropping old, so the body never transiently holds zero fixtures), and update the baseline.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` (`RebuildScaledFixtures` helper + scale branch in PASS 3.5)
- Modify: `Arcane/Tests/src/AuthoredTransformSyncTest.cpp` (append tests)

**Interfaces:**
- Consumes: `MakeFixtureDef(f, scale)` (Task 1), PASS 3.5 view (Task 2).
- Produces: `inline void RebuildScaledFixtures(Phys::PhysicsWorld&, Phys::BodyHandle, const Collider2D&, glm::vec2 scale)`.

- [ ] **Step 1: Write the failing tests** — append to `AuthoredTransformSyncTest.cpp`:

```cpp
TEST_CASE("paused scale-up rebuilds fixtures at effective size, pose preserved", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);

    reg.GetComponent<LocalTransform>(e)->scale = glm::vec2(2.0f, 2.0f);
    paused(reg);                                   // reconcile: rebuild fixtures

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    CHECK(res->world->FixtureCount(bh) == 1u);     // count preserved across rebuild
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));      // 0.5 * 2
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
    const Phys::Vec2 bp = res->world->Position(bh); // body did not move
    CHECK(static_cast<float>(bp.x) == Approx(2.0f).margin(1e-4f));
    CHECK(static_cast<float>(bp.y) == Approx(3.0f).margin(1e-4f));
    CHECK(reg.GetComponent<PhysicsBodyRef>(e)->appliedScale == glm::vec2(2.0f, 2.0f));
}

TEST_CASE("paused non-uniform scale rebuilds per-axis", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<LocalTransform>(e)->scale = glm::vec2(3.0f, 1.0f);
    paused(reg);
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.5f).margin(1e-4f));      // 0.5 * 3
    CHECK(he.y == Approx(0.5f).margin(1e-4f));      // 0.5 * 1
}

TEST_CASE("unchanged scale does not rebuild fixtures (no compounding)", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<LocalTransform>(e)->scale = glm::vec2(2.0f, 2.0f);
    paused(reg);                                   // rebuild once

    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    const Phys::FixtureHandle before = res->world->GetBodyFixture(bh, 0u);
    for (int i = 0; i < 3; ++i) paused(reg);       // scale unchanged -> no rebuild
    const Phys::FixtureHandle after = res->world->GetBodyFixture(bh, 0u);

    // appliedScale suppresses re-rebuild: the fixture identity is unchanged.
    // (FixtureHandle is a {index, generation} slot handle, like BodyHandle.)
    CHECK(before.index == after.index);
    CHECK(before.generation == after.generation);
    // Dims did not compound.
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("mint at scale then reconcile does not double-apply", "[transform-sync]")
{
    Astra::Registry reg;
    BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic, {2.0f,2.0f});
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // CREATE bakes scale 2 (he == 1.0)
    paused(reg);                                   // reconcile: scale unchanged -> no rebuild
    const glm::vec2 he = Fixture0HalfExtents(reg);
    CHECK(he.x == Approx(1.0f).margin(1e-4f));      // NOT 2.0 (no double-scale)
    CHECK(he.y == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("body still steps after a scale rebuild", "[transform-sync]")
{
    Astra::Registry reg;
    // Gravity on for this one: prove the rebuilt body is a live dynamic body.
    RegisterSceneComponents(reg);
    RegisterPhysicsComponents(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 10.0f;
    reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });
    Astra::Entity e = reg.CreateEntity();
    LocalTransform lt; lt.position = {0,0};
    reg.AddComponent<LocalTransform>(e, lt);
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    RigidBody2D rb; rb.type = Phys::BodyType::Dynamic;
    reg.AddComponent<RigidBody2D>(e, rb);
    Collider2D col; Fixture fx; fx.kind = Phys::ShapeKind::Aabb; fx.halfW = 0.5f; fx.halfH = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Collider2D>(e, col);
    reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});

    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<LocalTransform>(e)->scale = glm::vec2(2.0f, 2.0f);
    paused(reg);                                   // rebuild fixtures

    PhysicsSystem play(kDt, /*stepWorld=*/true);
    for (int i = 0; i < 10; ++i) play(reg);        // must fall under gravity
    CHECK(reg.GetComponent<LocalTransform>(e)->position.y > 0.1f);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: `paused scale-up...` and `paused non-uniform scale...` FAIL (fixtures never rebuilt — dims stay at 0.5; scale branch not implemented). The `unchanged scale...`, `mint at scale...`, and `body still steps...` cases may fail or pass depending on ordering, but must all PASS after Step 4.

- [ ] **Step 3: Add `RebuildScaledFixtures`** — in `PhysicsSystem.hpp`, add near `MakeScaledShape` (after `MakeFixtureDef`; needs `<vector>` — already included `:80`):

```cpp
    // -------------------------------------------------------------------------
    // RebuildScaledFixtures: rebuild every fixture of `bh` at (descriptor x scale),
    // preserving the body pose. Adds the new scaled fixtures BEFORE dropping the old
    // ones, so the body never transiently holds zero fixtures (sidesteps any
    // "body must keep >= 1 fixture" invariant). AddFixture / DropFixture recompute
    // body mass internally. The body is not moved, so pose is preserved.
    // -------------------------------------------------------------------------
    inline void RebuildScaledFixtures(Phys::PhysicsWorld& world, Phys::BodyHandle bh,
                                      const Collider2D& col, glm::vec2 scale)
    {
        // Capture current fixtures BEFORE adding new ones (their indices are stable
        // until we mutate; the new fixtures append after them).
        const std::uint32_t n = world.FixtureCount(bh);
        std::vector<Phys::FixtureHandle> old;
        old.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            old.push_back(world.GetBodyFixture(bh, i));

        // Add authored fixtures at the new scaled dims.
        for (const Fixture& f : col.fixtures)
        {
            Phys::FixtureDef fd = MakeFixtureDef(f, scale);
            world.AddFixture(bh, fd);
        }

        // Drop the pre-rebuild fixtures.
        for (Phys::FixtureHandle fh : old)
            world.DropFixture(fh);
    }
```

- [ ] **Step 4: Add the scale branch to PASS 3.5** — in `PhysicsSystem.hpp`, inside the PASS 3.5 lambda, add the scale branch BEFORE the pos/rot branch (rebuild does not move the body, so ordering is safe), and change the `Collider2D&` parameter from `/*col*/` to `col`:

```cpp
                view.ForEach([&](Astra::Entity   /*entity*/,
                                 PhysicsBodyRef&  ref,
                                 LocalTransform&  lt,
                                 Collider2D&      col,
                                 RigidBody2D&     /*rb*/)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))       return;

                    // SCALE: rebuild fixtures when lt.scale changed. Exact compare --
                    // physics never writes scale, so appliedScale can't drift; this
                    // both detects the edit and suppresses per-frame re-rebuild.
                    if (lt.scale != ref.appliedScale)
                    {
                        RebuildScaledFixtures(world, ref.handle, col, lt.scale);
                        ref.appliedScale = lt.scale;
                    }

                    // POS/ROT: stateless author reconcile.
                    const Phys::Vec2 bp = world.Position(ref.handle);
                    /* ...unchanged from Task 2... */
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: PASS (all `[transform-sync]` cases). If `world.DropFixture` on the `AddBody`-created primary fixture asserts or leaves the body invalid, the verification point in spec §5(b) has fired — switch `RebuildScaledFixtures` to re-mint the body (`RemoveBody` + rebuild via the CREATE path) instead of drop/add; the `body still steps after a scale rebuild` test is the gate for this. If `FixtureHandle` has no `.index`/`.generation` members, replace the identity check in `unchanged scale does not rebuild` with `res->world->IsValid(before)` + dims-stable only, and note the weaker assertion.

- [ ] **Step 6: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp Arcane/Tests/src/AuthoredTransformSyncTest.cpp
git commit -m "feat(arcane): paused scale->collider rebuild via appliedScale baseline"
```

---

## Task 4: Compatibility verification + full-suite gate

Confirm the create-pass scale change doesn't regress existing scenes/tests. Deliverable: a clean full-suite run and a documented Sandbox scale audit.

**Files:**
- Read-only: `Arcane/Sandbox/**` (scale audit)
- Modify: `.superpowers/sdd/progress.md` (append completion note)

- [ ] **Step 1: Audit Sandbox scene builders for authored scale ≠ 1**

Run:
```powershell
Select-String -Path "Arcane\Sandbox\src\*.hpp","Arcane\Sandbox\src\*.cpp" -Pattern "\.scale\s*=|lt\.scale|LocalTransform" | Select-Object -First 40
```
Expected: no builder sets `LocalTransform::scale` to anything but the default `{1,1}` (Sandbox authors dims directly in meters). If any builder sets scale ≠ 1, its collider size now changes (a correctness win, not a bug) — note it and confirm that scene's `[sandbox]` expectations still hold in Step 2.

- [ ] **Step 2: Full-suite regression gate (Debug)**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m
Push-Location "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "~[gpu]"; Pop-Location
```
Expected: PASS. The `[physics]`, `[pause]`, `[sandbox]`, `[interp]` case counts are unchanged from the pre-feature baseline; the total is the baseline PLUS the new `[transform-sync]` cases (14 cases). Record the new `~[gpu]` count (e.g. `27686/307` → `27686 + <new asserts>/321`).

- [ ] **Step 3: Release-config gate (determinism parity)**

Run:
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Release /m
Push-Location "Arcane\bin\Release-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "[transform-sync]"; Pop-Location
```
Expected: PASS (same as Debug — the reconcile path is `/fp:precise` and allocation-light).

- [ ] **Step 4: Append the completion note + commit**

Add a dated entry to `.superpowers/sdd/progress.md` bottom summarizing SPEC #1 landing (files, gate numbers, the two verification-point outcomes from Task 3 Step 5). Then:

```powershell
git add .superpowers/sdd/progress.md
git commit -m "docs(arcane): record SPEC #1 authored-transform sync completion + gates"
```

---

## Self-Review

**Spec coverage** (each spec §3–§8 requirement → task):
- §3 ownership table (Play body-owns / Edit author-owns) → Tasks 2 (pos/rot) + 3 (scale); Play-owns regression → Task 2 `play mode ignores...`.
- §4a stateless pos/rot rule (teleport + zero velocity, PASS 4 unchanged) → Task 2.
- §4b scale→collider (create + rebuild, per-shape dims, `appliedScale`) → Tasks 1 (create) + 3 (rebuild).
- §4c create pass + mint-while-paused seeding → Task 1 (seed) + Task 3 `mint at scale...`.
- §4d Play resume no snap-back → Task 2 `author-while-paused then play...`.
- §5 verification points (Static-body edits; zero-fixture rebuild) → surfaced in Task 3 Step 5 + Task 2 (Static covered indirectly; **added explicitly below**).
- §6 tests (7 spec cases) → all present, expanded to 14 concrete cases.
- §8 compat (Sandbox scale-1 audit, traits unchanged, no serialized change) → Task 4 + Task 1 (Serializable(false)).

**Gap found & fixed:** the spec §5(1) Static-body verification (editor must move a *static* collider while paused) had no dedicated test. `SetPosition`/`SetAngle` on a Static body is exactly what the reconcile pass calls — add this case to Task 2 Step 1 so the Static path is gated, not assumed:

```cpp
TEST_CASE("paused author move works on a static body", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Static);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    reg.GetComponent<LocalTransform>(e)->position = glm::vec2(4.0f, 0.0f);
    paused(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::Vec2 bp = res->world->Position(res->entityToBody.at(e));
    CHECK(static_cast<float>(bp.x) == Approx(4.0f).margin(1e-4f));
    // If this FAILS (SetPosition no-ops on Static, per spec §5(1)), the reconcile
    // pass must remove+re-add the static body at the authored pose for Static bodies.
}
```

**Placeholder scan:** no TBD/TODO; all code steps carry full code. Fallback branches (LiveFixtureAabbs-empty, DropFixture-on-primary, FixtureHandle-fields) are concrete conditional instructions tied to a gating test, not deferred work.

**Type consistency:** `MakeScaledShape(const Fixture&, glm::vec2)`, `MakeFixtureDef(const Fixture&, glm::vec2)`, `RebuildScaledFixtures(PhysicsWorld&, BodyHandle, const Collider2D&, glm::vec2)`, `PhysicsBodyRef::appliedScale`, `AngleDelta(float,float)`, `kAuthorPosEps`/`kAuthorRotEps` — names identical across all tasks and the reconcile call sites.
