# Authored-Transform Ownership + Physics Sync (Arcane engine)

**Date:** 2026-07-18
**Status:** Design — approved to write; awaiting user spec review.
**Layer:** Arcane engine (`PhysicsSystem`). SPEC #1 of a two-spec, engine-first pair.
**Author of record:** brainstormed with the user (design fully presented pre-`/clear`, reconstructed from
`project_arcane_next_milestone.md` and re-verified against the current tree).

---

## 1. Why this exists (the two-spec structure)

Grimoire authoring's next step is **gizmos + undo/redo**. Before any editor UI, the underlying
engine tension has to be resolved "100% properly": for a physics entity, both the physics body and
the author want to own the entity's position/rotation, and there is currently no rule for who wins
when. So the work splits into two specs, **engine first**:

- **SPEC #1 (this doc) — engine:** `LocalTransform` becomes the authored source of truth for a
  physics entity, with ownership that flips deterministically between the running simulation and the
  paused author. Landed in Arcane's `PhysicsSystem`; no editor code.
- **SPEC #2 (later) — editor:** Grimoire move/rotate/scale gizmos + a unified reflection-based
  undo/redo command stack. It consumes SPEC #1's seam and edits only `LocalTransform` — it never
  touches physics. Written and implemented after SPEC #1 ships.

This ordering follows the modular-stack rule (`Mosaic/Astra/Manifold2D → Arcane → Grimoire`, strict
one-way): the sync capability is generic engine behavior, so it lives in Arcane as `ARCANE_API`
surface (here, plain `PhysicsSystem` behavior), and Grimoire stays physics-agnostic.

### The bug this fixes today

`PhysicsSystem`'s write-back pass (PASS 4) writes the body pose into `LocalTransform` **every frame,
including paused frames** (only the interp-capture and `world.Step` are gated on `m_stepWorld`; the
write-back is not). So any edit to a physics entity's `LocalTransform.position`/`rotation` — from the
Grimoire Inspector today, from a gizmo tomorrow — is overwritten by the body's stale pose on the very
next frame. This spec closes that stomp for pos/rot and adds first-class scale→collider authoring.

---

## 2. Verified code facts (grounding)

All line numbers against the current working tree (re-verified 2026-07-18):

- `LocalTransform` — `Arcane/Arcane/src/Arcane/Scene/Components.hpp:18`
  - `glm::vec2 position`, `float rotation` (radians), **`glm::vec2 scale{1,1}`** (per-axis).
- `Fixture` — `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp:91` (trivially-copyable POD):
  - Circle → `radius`; Capsule → `halfLen`, `radius`; Aabb → `halfW`, `halfH`; Polygon → unsupported
    (no authored verts; asserts/skips).
- `Collider2D` — `PhysicsComponents.hpp:127`: `std::vector<Fixture> fixtures`.
- `PhysicsBodyRef` — `PhysicsComponents.hpp:145`: `Phys::BodyHandle handle{}` (Serializable(false)).
- `PhysicsSystem` — `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp`. Pass order per `operator()`:
  1. DESTROY (`:196`), 2. CREATE/SYNC (`:221–315`), 2.5 CAPTURE-PREV (gated `m_stepWorld`, `:317`),
  3. STEP (gated `m_stepWorld`, `:353`), 4. WRITE-BACK (**always runs**, `:360–410`).
  - Create builds `def.shape` from `fx0.*` at the primary switch (`:255–270`) and additional fixtures
    via `MakeFixtureDef(const Fixture&)` (`:128–169`) — **both ignore `lt.scale`.**
  - Write-back writes `lt.position`/`lt.rotation` from the body (`:396–398`), **never** `lt.scale`
    (bodies carry no scale) — so scale is already un-stomped; only pos/rot are stomped today.
- `Manifold2D::Physics::PhysicsWorld` (`ThirdParty/Manifold2D/include/Manifold2D/Physics/PhysicsWorld.hpp`):
  - `SetPosition(h, Vec2)` — **teleport** (snaps interpolation prev); `:421`.
  - `SetAngle(h, Real)` `:481`; `SetVelocity(h, Vec2)` `:442`; `SetAngularVelocity(h, Real)` `:446`.
  - `AddFixture(bh, FixtureDef) -> FixtureHandle` `:359`; `DropFixture(FixtureHandle)` `:364`;
    `FixtureCount(bh)` `:370`. There is **no** public per-fixture shape-dims getter (only
    `GetFixtureWorldPos/Angle/Category/Mask`) — this is why scale detection needs a cached baseline.

---

## 3. Ownership model

`LocalTransform` is the authored source of truth. Ownership of **pos/rot** flips with sim state;
**scale** is always author-owned (physics never writes it).

| Sim state | pos / rot owner | scale owner |
|---|---|---|
| **Play** (`m_stepWorld == true`, stepping) | **body** → `LocalTransform` (PASS 4, unchanged) | author (applied at create; runtime resize out of scope) |
| **Edit** (`m_stepWorld == false`, paused) | **author** → body (new reconcile pass) | author → collider (new reconcile pass) |

Grimoire only ever edits `LocalTransform`. The engine owns the sync in both directions.

---

## 4. Design — the paused author-reconcile pass

Add one new pass, **PASS 3.5: AUTHOR RECONCILE**, that runs **only when paused**
(`if (!m_stepWorld)`), positioned **after** CREATE/STEP and **before** WRITE-BACK (when paused, STEP
and CAPTURE-PREV are already skipped, so reconcile runs right after CREATE). It iterates the tracked
entities that have a live handle via a view of `PhysicsBodyRef, LocalTransform, Collider2D,
RigidBody2D` (PASS 4's set plus `Collider2D` for the rebuild descriptors) and, per entity:

### 4a. Pos/rot — stateless rule

```
read body pose  (world.Position(h), world.GetAngle(h))
if |lt.position - bodyPos| > kPosEps  OR  angleDelta(lt.rotation, bodyAngle) > kRotEps:
    world.SetPosition(h, lt.position)          # teleport: snaps interp prev, no smear
    world.SetAngle(h, lt.rotation)
    world.SetVelocity(h, {0,0})                # don't fling on resume
    world.SetAngularVelocity(h, 0)
# else: no-op
```

**Why stateless (no dirty flag / no new component for pos/rot):** a paused body cannot move itself
(no step), so on entry to a paused frame `LocalTransform` holds exactly the body-derived pose the
previous WRITE-BACK wrote. Therefore any divergence *is* an author edit — the live body pose is the
baseline, read for free. After reconcile, `body == lt`, so the unchanged PASS 4 write-back is a
harmless idempotent re-write. Untouched bodies reconcile to a no-op and PASS 4 re-writes the same
value. The `PhysicsSystem`'s reflect path (PASS 4) stays byte-for-byte unchanged.

`kPosEps` / `kRotEps` are small tolerances (order `1e-5` m / `1e-5` rad) guarding against float
round-trip noise in `SetAngle`→`GetAngle` normalization; sub-epsilon author nudges are intentionally
ignored (they would be swamped by the write-back anyway).

### 4b. Scale → collider (Unity/Unreal "baked scale")

Effective collider dims = **authored descriptor × `lt.scale`**. Scale is detected against a cached
baseline stored on `PhysicsBodyRef` (decided: no per-fixture geometry getter exists to read the
applied dims back, and per-frame rebuild is wasteful):

```cpp
struct PhysicsBodyRef {
    Phys::BodyHandle handle{};                 // runtime; Serializable(false)
    glm::vec2        appliedScale{1.0f, 1.0f}; // runtime baseline; Serializable(false)  <-- NEW
};
```

Paused reconcile, scale branch (runs before the pos/rot branch — rebuild does not move the body):

```
if (lt.scale != ref.appliedScale):             # exact compare: physics never writes scale, no drift
    rebuildFixtures(body, col.fixtures, lt.scale)   # DropFixture all -> AddFixture each at scaled dims
    ref.appliedScale = lt.scale
```

`rebuildFixtures` drops every current fixture of the body (`FixtureCount` + `DropFixture`) and
re-adds one per `Collider2D` descriptor via `AddFixture(MakeFixtureDef(f, lt.scale))`. The body is
not moved, so pose is preserved; `AddFixture` recomputes body mass (existing behavior). See §5 for
the two open API points this leans on.

**Per-shape scale semantics** (shared helper `ScaleShapeDims(Fixture, scale)`, used by both the
create pass and rebuild). Uniform scale (`sx == sy`) is exact for every shape; non-uniform:

Let `sx = abs(scale.x)`, `sy = abs(scale.y)`.

| Shape | Rule | Non-uniform note |
|---|---|---|
| **Aabb** | `halfW·sx`, `halfH·sy` | exact per-axis |
| **Circle** | `radius · max(sx, sy)` | representative axis (a circle has no distinguished axis; max never shrinks below the larger authored axis, matching Unity's circle/sphere handling) |
| **Capsule** | `halfLen·sx` (length axis = local X), `radius·sy` (perpendicular) | scalar-radius capsule under non-uniform scale is approximate — the round caps stay circular at `radius·sy` rather than becoming elliptical. Documented, matches pragmatic scalar-radius handling. |

*(Reviewable convention — see §7. If you'd rather a capsule use a single representative axis for both
dims, say so at review and I'll flip it before writing the plan.)*

### 4c. Create pass + mint-while-paused

The CREATE/SYNC pass (PASS 2) applies scale at body-birth so a body is never born unscaled:

- Multiply the primary-fixture dims (`:255–270`) and each additional fixture (via
  `MakeFixtureDef` gaining a `scale` param / overload) by `lt.scale` using `ScaleShapeDims`.
- Initialize `ref.appliedScale = lt.scale` at mint.

This covers mint-while-paused (Grimoire spawns an entity while frozen): the reconcile pass then sees
`lt.scale == appliedScale` and `body == lt`, so it no-ops — no spurious rebuild or teleport on the
first frame.

### 4d. Play resume — no snap-back

Because the last paused reconcile already pushed the authored pose into the body (`SetPosition`/
`SetAngle`), pressing Play continues the simulation from the authored pose. There is no one-frame
snap back to a pre-edit pose.

---

## 5. Open verification points (resolve at plan/impl time, not now)

1. **Static-body edits.** `MovePosition` no-ops on Static bodies (per the header comment).
   `SetPosition` is a teleport and is expected to apply to statics, but the editor must be able to
   move a static collider while paused. **Verify `SetPosition`/`SetAngle` take effect on
   `BodyType::Static`.** If they don't, statics reconcile via remove+re-add at the new pose (or an
   `AddBody`/`RemoveBody` re-mint) — a small branch in the reconcile pass.
2. **Fixture rebuild via drop-all + add-all.** Confirm a body can transiently hold zero fixtures and
   be re-populated purely through `AddFixture` (the primary fixture originally came from
   `BodyDef.shape` at `AddBody`). If a body must always retain ≥1 fixture, rebuild adds the new set
   before dropping the old, or re-mints the body. Confirm `DropFixture` + re-`AddFixture` correctly
   recomputes mass and refreshes broadphase proxies.

Both are physics-API confirmations only; neither changes the design shape above.

---

## 6. Tests (headless CPU, `[physics]`/new tag)

Driven through a fixture-owned `Registry` + `PhysicsSystem` with `stepWorld=false` for the paused
cases (mirroring the Sandbox test harness):

1. **Paused set position** → body teleports to `lt.position`, linear+angular velocity zeroed, and
   `lt.position` is **not** stomped back by the same frame's write-back.
2. **Paused set rotation** → body angle == `lt.rotation`; not stomped.
3. **Paused set scale (uniform)** → fixtures rebuilt at exact `descriptor·s`; `appliedScale` updated;
   body pose preserved.
4. **Paused set scale (non-uniform)** → Aabb per-axis exact; Circle/Capsule at the documented
   representative axis; single rebuild (no per-frame churn — set scale once, tick twice, assert one
   rebuild via fixture-count/dims invariants).
5. **Author-while-paused → Play** → stepping resumes from the authored pose (no snap-back).
6. **Play-mode reflect unchanged** → with `stepWorld=true`, PASS 4 still drives `lt` from the body
   (regression guard on the untouched reflect path).
7. **Mint-while-paused at scale ≠ 1** → new body born with scaled fixtures, `appliedScale` seeded,
   reconcile no-ops on the first frame.

---

## 7. Non-goals / decisions parked

- **SPEC #2 (Grimoire gizmos + unified undo/redo)** — separate spec, after #1 ships. Reference the
  old `Tools/Editor/src/UndoManager.hpp` (Begin/Track/Commit gesture + Push immediate) for the
  command model; build on a generic reflection-based "set component before→after" command so gizmo
  and inspector edits share one stack.
- **Runtime (Play-mode) scale-driven collider resize** — out of scope. Scale is honored at create and
  in edit-mode; game code mutating `lt.scale` mid-simulation does not resize the live collider.
- **Polygon fixtures** — already unsupported (no authored vertex array); unchanged here.
- **Per-fixture independent scale** — scale is whole-entity (`lt.scale`) applied uniformly to all of
  an entity's fixtures.
- **Capsule non-uniform exactness** — accepted approximation (§4b); flagged reviewable.
- **Astra re-vendor** — dropped (2026-07-18). The interim vendored `ComponentRegistry::Reserve` fix
  (`542a95b5`) stays as the crash guard; Astra is fixing the descriptor-dangling bug a different way
  upstream, so we do not re-vendor a moving target as part of this work.

## 8. Compatibility

The create pass now respects `lt.scale`, which changes collider sizing for **any** existing entity
authored at scale ≠ 1 (a correctness win under the engine-evolves rule, not a regression to avoid).
Verification: confirm the Sandbox scenes build their bodies at scale 1 (expected — they author px→m
dims directly), and flag any `[physics]` gate delta. No serialized data changes: `appliedScale` is
runtime-only (`Serializable(false)`), re-seeded on load exactly as `handle` and `WorldTransform`
are.

`PhysicsSystem`'s `SystemTraits` need no change: the reconcile pass reads `Collider2D` (already
`Reads<Collider2D>`), writes `PhysicsBodyRef.appliedScale` (already `Writes<PhysicsBodyRef>`), and
reads/writes `LocalTransform` (already `Writes<LocalTransform>`). The physics-world fixture/pose
mutations happen through the `PhysicsResource` handle, outside the component-dependency graph, as the
existing passes already do.
