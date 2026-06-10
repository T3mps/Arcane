# Client Physics Engine Design

**Date:** 2026-06-10
**Status:** DRAFT — pending review
**Scope:** Hand-rolled 2D physics engine replacing Box2D in the Love2D client, tailored to a tile-based isometric world with off-grid objects and cells-authoritative passability. Collider→Fixture→Rigidbody model, GJK (distance/CCD) + SAT (polygons) + specialized circle/capsule tests, constraints/joints, PhysicsWorld with contact management, constraint graph + islanding, solver abstraction, pluggable broadphase (spatial hash / SAP / dynamic AABB tree).
**Companion:** `2026-06-10-overworld-iso-map-movement-design.md` (the first consumer: WASD character controller + kinematic path follower against cell statics)

---

## Goal

One physics engine, exactly fitted to this game: deterministic fixed-step (the existing 60 UPS), cells as the single source of passability (world statics are *derived* from map cell flags, never authored separately), kinematic-friendly (click-to-move path followers must glide without solver fighting), FFI-first data layout (this is the flagship system for the engine-wide LuaJIT FFI mandate), and abstractions that let the future editor/engine phase introspect bodies, contacts, and joints generically.

Box2D leaves entirely. We do not need its feature breadth (no stacking-heavy dynamics, no continuous rotation-critical gameplay today); we do need things it makes awkward: deriving statics from tile data with zero duplication, custom character controllers, deterministic Lua-side replay, and editor-grade introspection.

---

## Why hand-rolled (and the honest cost)

- **Single passability truth:** Box2D forces a parallel static-fixture world that must mirror cell flags. Hand-rolled, the tile broadphase *reads* cell flags (via merged rectangles) — passability cannot desync between pathfinding, WASD, and rendering.
- **Tailoring:** kinematic path followers, slide-style character controllers, trigger volumes, and Combat-Sphere-era queries (cells in radius, LOS vs TALL obstacles) are first-class, not bolted on.
- **FFI/perf:** SoA body/contact storage in `ffi` buffers is the design center, not a retrofit.
- **Cost acknowledged:** a correct solver + CCD is real engineering. The phasing below front-loads the small subset the game needs *now* (M1) and defers the hard generality (joints, islands, CCD) to milestones that can each be validated by harness before anything depends on them.

---

## Architecture

```
src/physics/
  PhysicsWorld.lua        -- owns bodies, fixtures, joints, contacts; step() orchestration
  Body.lua                -- Rigidbody: static | kinematic | dynamic
  Fixture.lua             -- shape instance on a body: collider + material + filter
  shapes/                 -- Collider definitions (pure geometry, shareable)
    Circle.lua  Capsule.lua  Polygon.lua  AABB.lua
  broadphase/
    Broadphase.lua        -- interface: insert/remove/update(proxy), queryAABB, pairs()
    SpatialHash.lua       -- default for dynamics/kinematics (uniform world scale)
    TileGrid.lua          -- specialized static broadphase: reads Map cell flags directly
    SAP.lua               -- sweep-and-prune alternative          (M4)
    AABBTree.lua          -- dynamic AABB tree alternative        (M4)
  narrowphase/
    Dispatch.lua          -- shape-pair function table
    CircleTests.lua       -- circle/circle, circle/AABB, circle/polygon, capsule specials
    SAT.lua               -- polygon/polygon, polygon/AABB overlap + manifold
    GJK.lua               -- distance queries, closest points     (M2)
    CCD.lua               -- conservative advancement on GJK distance (M2)
  solver/
    Solver.lua            -- interface: solveVelocity(contacts, joints, dt), solvePosition(...)
    SequentialImpulse.lua -- default implementation (Baumgarte/positional split)
  joints/                 -- (M3) Joint base + DistanceJoint, RevoluteJoint, PrismaticJoint,
                          --       MouseJoint/target joint (editor manipulation), WeldJoint
  ContactManager.lua      -- persistent contact pairs, begin/end/stay events, warm-start cache
  Island.lua              -- constraint graph + island extraction, sleep management (M3)
  CharacterController.lua -- game-facing: slide-move (WASD) + kinematic follower (click mode)
  DebugDraw.lua           -- pipeline overlay: shapes, contacts, AABBs, islands by color
```

### Core model

- **Collider** (shape): pure geometry, immutable, shareable across fixtures. Circle, Capsule, AABB, convex Polygon (CCW, ≤8 verts).
- **Fixture**: a collider instance on a body — local transform, material (friction/restitution), collision filter (category/mask bits), `isSensor` (trigger volumes — overlap events, no response).
- **Rigidbody** (`Body`): `static` (never moves; tile statics + props), `kinematic` (script-driven velocity, pushes dynamics, unaffected by forces — the path follower), `dynamic` (integrated, solved). Position/velocity/mass live in the world's SoA arrays; `Body` is a lightweight handle (index) with accessor methods — handles stay stable via a free-list.
- **PhysicsWorld**: `new(opts)`, `addBody/removeBody`, `addJoint/removeJoint`, `step(dt)` called once per `fixedUpdate` (fixed 60 Hz — determinism contract: identical input sequence ⇒ identical state; no wall-clock reads inside step). Query API: `queryAABB`, `raycast`, `shapeCast` (M2, GJK-based), `overlapShape`.

### step() pipeline

1. Integrate velocities (gravity is configurable and **off** for this game's top-down world; the abstraction keeps it).
2. Broadphase update → candidate pairs (per-strategy; static tile pairs come from `TileGrid`).
3. Narrowphase dispatch → contact manifolds; `ContactManager` persists pairs (warm-start impulses cached per pair), emits begin/end/stay + sensor events (queued, delivered after step — no user code mutates mid-solve).
4. (M3) Build constraint graph (bodies = nodes; contacts + joints = edges), extract islands, sleep idle islands.
5. Solve: `Solver` interface — default `SequentialImpulse` (velocity iterations then position correction; iteration counts configurable per world). Solver is swappable for the editor-era "anything that would be expected here" cases (e.g., a soft/XPBD experiment) without touching the world.
6. (M2) CCD pass for flagged fast movers: GJK conservative advancement against statics, clamp to time-of-impact.
7. Integrate positions; publish transforms (interpolation alpha already exists at draw — physics stores prev/curr, the render boundary lerps).

### Narrowphase strategy (as specified)

- **SAT** for polygon↔polygon / polygon↔AABB overlap + contact manifold (clip incident face).
- **Specialized tests** for circle↔circle, circle↔AABB, circle↔polygon, capsule↔* (capsule = segment + radius; reuse closest-point-on-segment).
- **GJK** for distance queries (`distance(shapeA, xfA, shapeB, xfB)`), closest points, `shapeCast`, and as the CCD primitive. GJK is *not* used for the common overlap manifolds (SAT/specials are simpler and branch-predictable for our shape set); it earns its place on distance/CCD where SAT can't serve.

### Broadphase strategy

`Broadphase` is an interface; the world composes **two** instances: one for statics, one for movers — pairs are (mover↔mover) ∪ (mover↔static).
- **`TileGrid` (statics, default):** does not store fixtures for walls at all — it answers queries straight from `Map` cell flags, materializing transient AABB "virtual fixtures" for solid cells near a query/mover, with **greedy rectangle merging** per row-span to kill internal-edge catching (the classic tile-seam bug). This is the single-source-of-truth mechanism: change a cell flag, physics follows instantly (editor live-editing inherits this for free). Off-grid props are ordinary static bodies registered alongside.
- **`SpatialHash` (movers, default):** uniform grid keyed at ~2× max mover extent; ideal for our flat, evenly-scaled world.
- **`SAP` / `AABBTree` (M4):** drop-in alternates behind the same interface for profiling against real scenes (AABBTree likely wins if the editor era brings big sparse maps; SAP if mover counts grow with mostly-coherent motion). Selection is a world option; harness runs the same scenarios across all strategies and asserts identical pair sets.

### Character control (the actual consumers)

`CharacterController` wraps a body per mode (the overworld spec's `MovementController` drives it):
- **WASD:** kinematic-style slide-move — desired velocity in, swept AABB/capsule vs statics, slide along contact normals (two-pass), step-free (no stairs yet). Deterministic, no solver involvement; dynamics can still push/be blocked via standard contacts if we later make the player dynamic.
- **Click-to-move:** plain kinematic body; the PathFollower sets velocity along the path; physics only reports overlaps (sensors, future combat-contact initiation) and never deflects it — paths are cell-safe by construction (pathfinder reads the same flags).

### FFI layout (mandate)

SoA `ffi` arrays on the world: `posX/posY/prevX/prevY/velX/velY/invMass/...` (`double[capacity]`), contact buffer as a flat struct array, body handles = int indices + generation counter. Lua-table fallback behind the same accessors for the harnesses' no-FFI context if needed (decide in M1 — if `love.thread`-less harness can load `ffi`, no fallback required; LuaJIT always has ffi, so default is FFI-only). Zero steady-state allocation in `step()` after warmup; scratch buffers pooled on the world.

---

## Milestones

- **M1 — what the overworld needs (with P2 of the companion spec):** shapes (Circle/AABB/Polygon), `TileGrid` + `SpatialHash` broadphases, specialized circle/AABB tests + SAT, ContactManager with events/sensors, static/kinematic bodies, `CharacterController` (both modes), `queryAABB` + `raycast`, DebugDraw, SoA core. *No dynamics solving yet* — nothing in the overworld needs impulse response on day one.
- **M2 — queries + CCD:** GJK distance/closest-points, `shapeCast`, conservative-advancement CCD, Capsule shape. (Raycast vs TALL cells here doubles as the LOS primitive the Combat Sphere phase will want.)
- **M3 — dynamics:** dynamic bodies, `SequentialImpulse` solver, constraint graph + islanding + sleeping, joints (Distance, Revolute, Prismatic, Weld, Mouse/target for editor manipulation).
- **M4 — alternates + tuning:** SAP + AABBTree broadphases, profiling harness, FFI pass audit, determinism replay test (record input → identical state hash).

Each milestone lands with a `physics_harness` (existing harness pattern): geometric unit tests (manifold normals/depths from known configurations), tile-seam slide tests, broadphase pair-set equivalence, and (M4) determinism replay.

## Integration & removal

- `PhysicsSystem` (tiny-ECS Box2D wrapper) is replaced by a thin system calling `PhysicsWorld:step(fixedDt)`; `MovementSystem` routes intent through `CharacterController`. Box2D usage and STI's collision import are deleted with the Tiled removal (companion spec).
- DebugDraw rides the canonical pipeline as an overlay (homogenization-compliant), toggled by a dev key.

## Open questions (for review)

1. Player body shape: circle vs capsule for slide feel (M1 starts circle; capsule arrives M2).
2. Polygon vertex cap (8 proposed) — enough for props? Editor era may want more; cap is a constant.
3. Should sensors fire during placement-frozen states (combat initiation later)? Proposed: world-level event gate flag.
