# Arcane Physics v2 — Fixture & Material Model — Design

> **Program context.** Inserts the **Body → Fixtures → Shape** model into Physics v2 **Phase A**
> (rotation-aware rigid-body core) *before* T5 wires contact generation — because Fixtures reshape
> exactly that layer, and wiring it single-shape-per-body then retrofitting would be rework. The
> rotation-aware narrowphase (T1–T3: `Collide(shapeA, xfA, shapeB, xfB)`) is **fixture-agnostic and
> stands unchanged**. EPA is **deferred** (SAT is exact for our convex polygon cores); a future
> "narrowphase specialization" workstream implements **EPA + MPR** for curved/high-vertex/per-
> circumstance routing.

## Goal

Give a body **one or more convex colliders (fixtures)**, each carrying its own **material**
(density, friction, restitution), **filter**, and **isSensor** flag, positioned by a **local
transform** in the body frame. This unlocks compound bodies, per-shape materials, per-fixture
sensors/filters, and **concave/irregular shapes via convex decomposition** (a concave outline is
authored as several convex fixtures) — all while keeping every collision pair convex, so SAT+GJK
stays the efficient, exact narrowphase.

## The model

- **Body** — the dynamic unit: transform (position + angle), velocity (linear + angular),
  body-type (Static/Kinematic/Dynamic), and **aggregated** mass / inertia / center-of-mass / the
  motion flags (`fixedRotation`, `bullet`, `linearDamping`, sleep state). A body owns 1..N fixtures.
- **Fixture** — one convex collider attached to a body: a **Shape** (the unified core+radius from
  T1), a **local transform** (`Vec2 localPos`, `Real localAngle`) in the body frame, **material**
  (`density`, `friction`, `restitution`), a collision **filter** (`categoryBits`, `maskBits`,
  optional group), and **isSensor**. A stable `FixtureHandle{index, generation}`.
- **Shape** — unchanged from T1: convex core (CCW verts + normals + centroid) + radius. Circle =
  1 vert, capsule = 2, polygon = N. **Convex-only**; concave shapes are multiple fixtures.

A fixture's shape lives in **body-local space** offset by its local transform; the world shape
transform fed to `Collide` is `worldXf = compose(bodyXf, fixtureLocalXf)` (rotation composes:
`worldAngle = bodyAngle + localAngle`, `worldPos = bodyPos + R(bodyAngle)·localPos`).

## Storage (Core, SoA)

- **Fixture SoA** on `PhysicsWorld`: parallel arrays `m_fxShape`, `m_fxLocalPos`, `m_fxLocalAngle`,
  `m_fxDensity`, `m_fxFriction`, `m_fxRestitution`, `m_fxFilterCat`, `m_fxFilterMask`, `m_fxSensor`,
  `m_fxBody` (owning body slot), `m_fxProxy` (broadphase proxy id), `m_fxGen` (generation), over a
  free-list — mirroring the body SoA discipline (zero steady-state alloc, stable index iteration).
- **Body → fixtures link:** each body slot stores a fixture index list (a small inline range or a
  per-body `std::vector<uint32_t>` of fixture slots; a body typically has 1–4 fixtures).
- **Material moves off the body:** the M6 per-body `m_rest`/`m_fric`/`density` become per-fixture.
  The body keeps only the **aggregated** mass/invMass/invInertia/COM.

## Mass aggregation

`AddFixture` (and body finalization) computes body mass by summing each fixture's
`Shape::ComputeMass(density)` (density-weighted), composing centroids into the body **center of
mass**, and parallel-axis-summing each fixture's inertia about that COM. `invMass`/`invInertia`
derive from the totals; `fixedRotation` forces `invInertia = 0`; Static/Kinematic keep 0/0.
(Closes the T1 carry-forward: `ComputeMass` for rounded polygons is implemented here if a fixture
has radius > 0.) Body anchors (`rA`/`rB` in contacts) are measured from the **body COM**, not the
body origin — the solver already uses center-relative anchors, so contact anchors are computed
from the world contact point minus the body world COM.

## Contacts (per fixture-pair)

- **Broadphase = fixture-level proxies.** Each fixture gets a proxy AABB in the dynamic tree
  (rotation-aware AABB from T1 over the world shape transform). The broadphase emits **fixture
  pairs** (sorted, strategy-independent). Same-body fixture pairs are filtered out; collision-filter
  (`category`/`mask`) rejects non-matching pairs early. *(Chosen over a body fat-AABB + sub-test:
  fixture-level proxies are simpler, uniform with the existing DynamicTree, and the per-fixture AABB
  is what the solver needs anyway.)*
- **`GenerateContacts` iterates fixture pairs:** for each (fixtureA, fixtureB) candidate, call
  `Collide(shapeA, worldXfA, shapeB, worldXfB, specMargin)`; material for the constraint = combined
  fixture materials (friction = `sqrt(fA·fB)`, restitution = `max(rA,rB)` — the existing rule, now
  sourced from fixtures). The `ContactConstraint` still references the two **body** slots for the
  solve (the solver is unchanged), plus the contact anchors from the fixture-pair manifold.
- **`ContactManager` keys on fixture-pair ids** (a 64-bit `(minFixture<<32)|maxFixture`), not body
  pairs. One body-pair can have several fixture-pair contacts. Begin/Stay/End gating + the
  two-granularity event semantics carry forward at **fixture-pair** granularity (matches Box2D;
  events can report the fixtures + their bodies). `DropBody` drops all its fixtures' pairs;
  `DropFixture` drops that fixture's pairs.
- **TileGrid** (static passability keystone) is unchanged — tile spans are virtual static fixtures
  exactly as today; a mover's fixtures test against tile spans per the existing path.

## Astra / Arcane.dll scene layer

- **`RigidBody2D`** (M6 P3.2) — unchanged role: body-type, velocity, `fixedRotation`, `bullet`,
  `linearDamping`. Material fields move OUT (to fixtures).
- **`Collider2D` becomes a fixture list.** A single `Collider2D` component carries a small
  **vector of fixtures**, each = `{ shape descriptor (ShapeKind + scalars), localPos, localAngle,
  density, friction, restitution, categoryBits, maskBits, isSensor }`. *(Chosen over N separate
  collider components per entity: one entity = one body with its fixtures is cleaner for the editor
  + the PhysicsSystem sync, and matches the SoA body→fixtures link.)* Still `ASTRA_REFLECT`
  (the reflected vector-of-struct path; verify Astra reflects a `std::vector<Fixture>` field, else
  reflect a fixed small array + count).
- **`PhysicsSystem`** sync (M6 P3.3): on body create, create the body + each fixture (build the
  Core `Shape` from each descriptor, place by local transform); on remove, drop the body (which
  drops its fixtures). `PhysicsBodyRef` keeps the `BodyHandle`; optionally a parallel fixture-handle
  list if entities need per-fixture refs (defer until a consumer needs it — YAGNI).

## Back-compat / migration

- `AddBody(BodyDef{ shape, material… })` stays as a **convenience** that creates a body with **one
  fixture** carrying that shape + material (so all existing single-shape call sites + tests keep
  working). New `AddFixture(BodyHandle, FixtureDef)` adds more.
- The M6 single-`Shape` per body becomes the 1-fixture special case — no behavior change for
  single-shape bodies.

## Determinism

Fixtures iterated by stable index; broadphase emits sorted fixture pairs; ContactManager sorts by
fixture-pair key. f32, fixed dt, no wall-clock — unchanged. Mass aggregation is order-independent
(summation in stable fixture order).

## Non-goals (deferred)

- **Geometry utilities (future workstream)** → the algorithms that *generate* collider/fixture
  data: **convex hull** (point cloud / sprite-alpha outline → convex polygon, e.g. Andrew's
  monotone chain), **convex decomposition** (simple concave polygon → convex fixtures, e.g.
  Bayazit / Hertel–Mehlhorn), plus polygon **simplification** (Douglas–Peucker), **triangulation**,
  offset/inset (skin), and sprite-outline extraction (alpha → contour). These FEED the Fixture
  model (decomposition output = the convex fixtures), the editor (author colliders from sprites),
  and Destruction (Phase E: runtime decomposition of fracture fragments). Authoring/offline-first
  (a runtime decompose is opt-in). Out of scope for this model; lands as its own utility workstream
  (likely with Grimoire/LevelEditor). For now, authors supply convex fixtures directly.
- **EPA / MPR** → future narrowphase-specialization workstream (curved/high-vertex/per-circumstance).
- **Per-fixture restitution/friction mixing callbacks, surface velocity, one-way platforms** →
  Phase C (gameplay materials).

## Impact on the Phase A plan (revision)

- **T4 (EPA): removed/deferred.**
- **New T4 (Fixture data model):** Fixture SoA + `FixtureHandle` + `AddFixture`/`DropFixture` +
  mass aggregation (incl. rounded-polygon `ComputeMass`) + `AddBody` back-compat (1-fixture).
  Test: a compound body's aggregated mass/COM/inertia matches a hand-derived two-shape body; a
  fixture's world shape transform composes body∘local correctly; stale-handle invariants.
- **T5 (wire, fixture-aware):** broadphase indexes fixtures; `GenerateContacts` iterates fixture
  pairs through `Collide` with **real body+fixture angle**; ContactManager keys on fixture pairs.
  Headline tests retained: box dropped at 45° settles flat; `fixedRotation` lock; warm-start
  persists by feature id; a **compound** body (e.g. box + offset circle) collides on both fixtures;
  determinism run-twice.
- **T6 (strangle + retire oracle):** as before, now fixture-aware; archive Lua feel-reference
  traces.
- **T7 (finish):** as before — fp_contract(off), dual-flavor, full green, new goldens, final review.
- **Astra layer (T5/T6):** update `Collider2D` → fixture list + `PhysicsSystem` sync.

## Success criteria

- A body can carry multiple convex fixtures, each with its own material + local transform; a
  concave shape works as several convex fixtures; per-fixture sensors/filters work.
- Body mass/inertia/COM aggregate correctly over fixtures; contacts + events are per-fixture-pair;
  broadphase emits sorted fixture pairs; TileGrid unchanged.
- Single-shape `AddBody` back-compat holds (existing tests green); rotation-aware contact gen
  (T5 headlines) holds on fixtures.

## Next step

On approval: revise `docs/superpowers/plans/2026-06-15-arcane-physics-v2-phase-a.md` (drop EPA T4,
insert the fixture data-model task, make T5+ fixture-aware) via writing-plans, then resume
subagent-driven execution.
