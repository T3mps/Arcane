# Arcane Epic 04.2 — Render Interpolation by RunLoop Alpha

Date: 2026-07-17
Status: Design (approved)
Epic: 04 (Grimoire shell + sim-time control). Prereqs 04.1 (sim-time control in
`RunLoop`) and 04.3 (Sandbox migrated onto it) are DONE + green.

## Goal

Make rendering smooth between fixed simulation steps by interpolating drawn poses
by the render alpha `RunLoop::Alpha()` already produces. The visible payoff is
smooth slow-motion: at a low time-scale a fixed step fires only every few frames,
so the current (snap-to-latest-step) rendering makes bodies jump discretely. With
interpolation the drawn pose slides from the previous step's pose to the current
step's pose across the intervening frames.

Time-scale, pause, and single-step are already correct in `RunLoop` (04.1). This
task changes ONLY how a frame is *drawn*; it does not touch the time model,
determinism, or the fixed step.

## Key finding (why the obvious approach is wrong)

The prior note assumed interpolation belongs on the ECS sprite path
(`RenderSubmissionSystem`, driven by `WorldTransform`). It does not — at least not
alone. The Sandbox's outline-unify pivot (`Scenes.cpp`, "Item A") attaches **no**
`SpriteRenderer` to physics bodies: every body is drawn by `DrawPhysicsDebug`,
which reads poses **directly from `PhysicsWorld` by body slot index**
(`world.PosSlot(i)`, `world.GetAngle(world.HandleOf(i))`), not from ECS
transforms. So interpolating only the sprite path would leave the Sandbox's
slow-mo exactly as steppy as it is today.

Therefore the debug-overlay path is the primary target. The ECS sprite path is
also interpolated for engine generality (the future `Game.dll` / Aphelyon client
renders sprites), even though no current interactive consumer exercises it — it is
unit-tested only.

## Confirmed data flow

- `SandboxApp::FixedUpdate` constructs and runs `PhysicsSystem(dt)` **once per
  fixed step**. `RunLoop` calls the fixed phase 0×/frame while paused and N×/frame
  under time-scale, at the canonical fixed dt.
- `PhysicsSystem::operator()`: DESTROY → CREATE/SYNC → `world.Step()` → WRITE-BACK
  (`world.Position/GetAngle` → `LocalTransform`, velocity → `RigidBody2D`).
- Update phase: `TransformPropagationSystem` derives `WorldTransform` from
  `LocalTransform` (runs even while paused).
- Render phase: `RenderSubmissionSystem` (sprites) then `PhysicsDebugRenderSystem`
  → `DrawPhysicsDebug` (overlay). Both read the `RenderContext2D` resource for the
  camera.
- `RunLoop::Alpha()` = `accumulator / fixedDt` ∈ [0,1). `Runtime` owns the
  `RunLoop` (`Runtime::Loop()`), and `Runtime::SetRenderContext(batcher)` is the
  once-per-frame seam that fills `RenderContext2D`. Surfacing alpha there needs no
  ABI change.
- `BodyHandle` is `{index, generation}`; `world.HandleOf(i)` returns the live
  handle for slot `i`. This makes a slot-indexed previous-pose buffer robust
  against slot reuse.

## Interpolation model (Gaffer "Fix Your Timestep")

For each interpolated pose we need the step-boundary poses:

- `current` = pose at the last committed fixed step (step N).
- `previous` = pose at the step before (step N-1).
- drawn pose = `lerp(previous, current, alpha)`.

Both only change when a fixed step runs, so capture happens at fixed-step cadence,
never per frame. We store only `previous` and read `current` live from the world /
ECS; the capture rule is: **snapshot the current pose into `previous` immediately
BEFORE `world.Step()`** (equivalently, before the write-back overwrites
`LocalTransform`). Consequences:

- One step this frame: `previous` = pre-step (N-1), `current` = post-step (N). ✔
- Multiple steps this frame: each step's capture overwrites `previous`, leaving
  `previous` = second-to-last and `current` = last. ✔
- Zero steps this frame (frame faster than the sim): `previous` and `current`
  unchanged, alpha grows toward 1 within the same interval. ✔
- Paused: no step → no capture; alpha frozen → the drawn pose is frozen exactly
  where it was when paused (no jump on pausing). ✔
- Single-step: one capture + one step; alpha frozen. Each press advances the drawn
  pose by exactly one step of motion (a constant sub-step offset is invisible). ✔

Rotation is interpolated as a decomposed angle via shortest-arc `AngleLerp`, NOT
by lerping matrix components (which would distort a rotating body).

## Design

### Path A — physics-debug overlay (primary; makes the Sandbox smooth)

1. **`RenderContext2D.alpha`** — new `float alpha = 0.0f` field.
   `Runtime::SetRenderContext` writes `Loop().Alpha()` into it alongside the
   camera. No ABI change (Runtime owns the loop). Default 0 keeps every existing
   headless caller unchanged.

2. **`PhysicsInterpBuffer`** — new resource in `SceneResources.hpp` (POD,
   Manifold2D-free so it stays game-plugin-includable):

   ```cpp
   struct InterpPose { glm::vec2 position{}; float angle = 0.0f; std::uint32_t generation = 0; };
   struct PhysicsInterpBuffer {
       std::vector<InterpPose> prev;      // indexed by body SLOT index
       bool                    captured = false;   // false until the first capture
       template<class Ar> void Serialize(Ar&) {}   // transient per-frame runtime state
   };
   ```

   Indexed by the same body-slot space `DrawPhysicsDebug` iterates. Each entry
   records the slot's `generation` so a recycled slot is detected.

3. **Capture in `PhysicsSystem`**, immediately before `world.Step()` (PASS 3),
   gated on `m_stepWorld` and the resource being present: resize `prev` to
   `world.Count()`; for each slot `i`, if `world.Alive(i)` write
   `prev[i] = { world.PosSlot(i), world.GetAngle(world.HandleOf(i)),
   world.HandleOf(i).generation }`, else `prev[i].generation = 0`. Set
   `captured = true`. Iterating all world bodies (not ECS entities) covers
   world-direct joint/polygon bodies (scenes 3/4/7/8) that have no ECS entity.
   Cost: one `vec2 + float + uint` per body per step — negligible next to the
   solve.

4. **Consume in `DrawPhysicsDebug`** — add to `PhysicsDebugDrawOptions`:
   `const PhysicsInterpBuffer* interp = nullptr; float alpha = 0.0f;`. At the top
   of the per-body loop compute the render pose ONCE:

   ```cpp
   Vec2  rpos = world.PosSlot(i);
   float rang = static_cast<float>(world.GetAngle(world.HandleOf(i)));
   if (interp && interp->captured && i < interp->prev.size()
       && interp->prev[i].generation == world.HandleOf(i).generation) {
       rpos = { Lerp(interp->prev[i].position.x, rpos.x, alpha),
                Lerp(interp->prev[i].position.y, rpos.y, alpha) };
       rang = AngleLerp(interp->prev[i].angle, rang, alpha);
   }
   ```

   Drive the outline, AABB outline, COM marker, orientation tick, and velocity-ray
   ORIGIN from `rpos`/`rang`. This also collapses today's duplicate
   `world.GetAngle(...)` calls (currently up to 2× per body) into one.

   NOT interpolated (stay at current-step): contact markers / manifolds and the
   broadphase tree/grid overlays. They are per-contact / per-broadphase
   diagnostics, not per-body poses; interpolating them is ill-defined and they are
   debug-only. The velocity ray uses the current velocity (direction unchanged).

5. **Wire `PhysicsDebugRenderSystem`** (in `SandboxApp.hpp`) to read the
   `PhysicsInterpBuffer` resource + `ctx->alpha` and set `opts.interp` /
   `opts.alpha`. The Sandbox creates the buffer resource in
   `SandboxApp::RebuildScene` (mirrors the existing `SandboxDebugDraw` publish
   pattern), so it is present for both capture and consume.

### Path B — ECS sprite path (engine generality)

6. **`PreviousTransform { glm::vec2 position; float rotation; }`** — new reflected
   component in `Components.hpp` (decomposed local pose). A renderable entity opts
   into interpolation by carrying it. Reflected with the derived-state marker
   (`Serializable(false)` on the fields, like `WorldTransform::matrix`) since it is
   re-derived each step.

7. **Capture in `PhysicsSystem` WRITE-BACK (PASS 4)** — before overwriting
   `LocalTransform`, if the entity has `PreviousTransform`, copy the OLD
   `{ position, rotation }` into it (`reg.GetComponent<PreviousTransform>(entity)`;
   the entity is already in scope in the write-back lambda). Same pre-overwrite =
   "previous step" semantics as Path A; multiple-steps-per-frame handled
   identically.

8. **Consume in `RenderSubmissionSystem`** — inside the `ForEach`, if the entity
   has `PreviousTransform`, lerp the decomposed current world pose (`worldPos`,
   `worldRot`) against it by `ctx->alpha` (`Lerp` position, `AngleLerp` rotation)
   and draw from the blended pose. No `PreviousTransform` → unchanged path
   (byte-identical for every existing consumer).

### Shared piece

- **`Lerp` / `AngleLerp`** — small inline helpers placed in `SceneResources.hpp`
  next to `PhysicsInterpBuffer` (both `DrawPhysicsDebug` and `RenderSubmissionSystem`
  include, or will include, that header). `AngleLerp(a, b, t)` interpolates the
  shortest arc: e.g. `350°→10°` moves `+20°` through 0, not `-340°`.

## Assumptions and limitations (documented, not fixed here)

- **Flat hierarchy assumption.** Both consume-sides treat a physics entity's local
  pose as its world pose (Path A reads world poses directly; Path B compares a
  previous LOCAL pose against a current WORLD pose). This is exact for the Sandbox
  — physics bodies are direct children of an identity `SceneRoot` — and for any
  scene whose physics parents are static. Under a *moving* nested parent it is
  approximate. No current scene has one. Generalization (propagate a previous-world
  pose) is future work, called out here so a later hierarchy change knows to
  revisit.
- **Capture site.** Previous poses are captured in `PhysicsSystem` (Path A: all
  world bodies; Path B: entities carrying `PreviousTransform`). A purely
  gameplay-animated, non-physics entity that moves its own `LocalTransform` would
  need a general capture system to interpolate. Out of scope; the physics-driven
  case is the near-term consumer.
- **Slot reuse.** Handled by the generation gate (Path A). On the single frame a
  slot is recycled the generation mismatches and that body draws at its current
  pose (one non-smooth frame on spawn — invisible).

## Testing

CPU-only (no `[gpu]`), landing in `[sandbox]` / `[render]` / `[runloop]`:

- `AngleLerp` shortest-arc: wrap-around cases (`350→10`, `10→350`, `-170→170`),
  endpoints at `t=0/1`, and a plain non-wrapping case.
- `PhysicsInterpBuffer` capture semantics: drive a headless `PhysicsSystem` on a
  falling body for two steps; assert `prev` holds the second-to-last pose and the
  live world holds the last. Assert the generation gate rejects a recycled slot.
- `DrawPhysicsDebug` interpolation: a recording `Batcher2D` (as used by
  `SpriteRotationTest`), one body, a synthesized `prev` buffer, `alpha = 0.5` →
  assert the outline center is the midpoint and a rotated shape's angle is the
  mid-angle. `captured = false` / generation mismatch → drawn at current.
- `RenderSubmissionSystem` interpolation: recording `Batcher2D`, a sprite entity
  with `PreviousTransform`, `alpha = 0.5` → asserted midpoint; no
  `PreviousTransform` → unchanged.
- Paused: no capture, alpha frozen → render frozen.

Gate: full `~[gpu]` stays green (existing count + the new cases).

## Desk-verify (headless cannot judge smoothness)

Loom interactive (GPU-driver crash hazard under Parsec — at the desk only):
time-scale ~0.1 → outlines glide between steps rather than snapping; pause freezes
the drawn pose in place; single-step advances one step of motion per press.

## Out of scope

- Interpolating contacts / manifolds / broadphase overlays.
- A general (non-physics) previous-pose capture system.
- Moving-nested-parent correctness (previous-world propagation).
- Any change to `RunLoop` time semantics, the fixed step, or determinism.
