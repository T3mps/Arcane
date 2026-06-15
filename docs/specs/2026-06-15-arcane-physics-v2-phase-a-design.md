# Arcane Physics v2 — Phase A: Rotation-Aware Rigid-Body Core — Design

> **Program context.** This is **Phase A** of the **Physics v2** program — a first-party,
> state-of-the-art 2D physics engine that supersedes the M6 oracle-faithful port. The program
> decomposes into: **A** rotation-aware rigid-body core (this doc), **B** swept
> CharacterController + overworld feel-parity, **C** gameplay materials & interaction layer,
> **D** soft bodies & particles (XPBD), **E** destruction. Each phase gets its own spec → plan →
> subagent-driven execution. Phase A is the keystone every later phase builds on.

## Goal

Make the Arcane physics engine **rotation-aware** end to end, on a **unified rounded-polygon
shape model** with **stable feature-ID contact manifolds**, retiring the Lua oracle as a
correctness gate and **re-baselining** the test suite on physics invariants + fresh golden
hashes. The keystone **TileGrid** single-source passability is retained unchanged. After Phase A,
a dynamic body that the solver spins actually *collides* as a rotated body, and per-body rotation
can be locked with one flag.

## The three locked foundational decisions (from brainstorming)

1. **Determinism = per-platform self-consistent, `f32`.** Same as M6: identical inputs ⇒
   identical results on the same binary/platform. No fixed-point / soft-float rewrite. (The
   Combat service is turn-based / server-authoritative, so cross-machine bit-determinism is not
   required.) **One addition:** because we re-baseline goldens anyway, Phase A also closes the
   deferred M6 item by enforcing **FP contraction off** in the physics TUs (`floatingpoint
   "Strict"` / `fp_contract(off)`) so the determinism contract the M6 spec stated is finally
   explicit — and the freshly-captured goldens bake that in.
2. **Correctness = replace + re-baseline.** The oracle was a **porting scaffold**, not a live
   dependency — the shipping client still runs the **Lua** physics; nothing live depends on the
   C++ core yet (the client ports onto Arcane at the later Game.dll milestone). So we retire
   oracle bit-match parity, define a fresh **property/invariant** suite + **new golden hashes**
   captured from the v2 engine, and **archive the Lua engine's overworld behavior as golden
   "feel-reference" traces** for the eventual client port (Phase B validates against them).
3. **Scope = maximal**, sequenced. Phase A is *only* the rigid-body core (below). Materials,
   soft/particles, destruction are Phases C–E.

## Key grounding finding (scopes Phase A accurately)

The **SoftStep solver is already fully rotation-complete.** It computes effective normal/tangent
mass with the inertia terms `1/(iMa + iMb + iIa·(rA×n)² + iIb·(rB×n)²)`, applies angular impulse
`wA += invInertiaA·(rA×P)`, and tracks per-body `deltaRot` for TGS separation re-evaluation
(`Solver/SoftStep.cpp`). `ContactConstraint`/`ContactConstraintPoint` already carry
`anchorA/anchorB`, `invInertiaA/invInertiaB`, and a stable warm-start `id` slot.

**Therefore the `fixedRotation` limitation lives entirely in the narrowphase + contact
generation, not the solver.** Concretely, in `PhysicsWorld::GenerateContacts` every transform is
built with rotation `Real(0)` (lines ~137, 827, 845, 860, 942–943), and `Manifold`/`Sat`/
`Specialized`/`GeometryKernel`/`Gjk` all document "rotation identity this phase, translate-only."
The manifold feature `id` is **rank-based** (`keyBase + 1` deepest, `+2` second), so warm-start
matching is fragile under contact-point reordering. **Phase A fixes exactly these.**

## Architecture

### 1. Unified rounded-polygon shape model

Collapse the four-kind `Shape` (circle / capsule / aabb / polygon) and the SAT-poly /
specialized-round / GJK-distance split into **one representation**: a convex polygon **core**
(CCW vertices + outward edge normals + centroid) plus a **radius** `r`.

| Old kind | v2 core |
|---|---|
| Circle(r) | 1 vertex (the center), radius `r` |
| Capsule(halfLen, r) | 2 vertices (segment endpoints), radius `r` |
| Aabb(hw, hh) | 4 vertices, radius 0 |
| Polygon(verts) | N vertices, radius 0 (optional small skin) |

All collision reduces to **"distance/overlap between two convex polygon cores, inflated by
`rA + rB`."** This is the Box2D-v3 model and it is what makes one narrowphase function cover every
pair. `MakeCircle/MakeCapsule/MakeAabb/MakePolygon` are retained as ergonomic constructors that
build the unified shape; existing call sites keep working. `ComputeMass` stays (mass/inertia per
kind, now derived from core + radius).

### 2. Rotation-aware narrowphase (one pipeline)

`Transform{ position, angle }` becomes load-bearing. A `RotateInto(worldVerts, shape, xf)` helper
produces world-space rotated+translated core vertices; **every** narrowphase routine operates on
those world cores + radii. The pipeline per pair:

1. **GJK on the cores** → closest features + core separation (handles separated and shallow
   cases; also the speculative-margin path — a contact is reported when core distance < `rA + rB +
   speculativeMargin`).
2. **Manifold from the closest features** by **reference-face clipping** (find the reference edge
   via the deepest SAT axis / closest GJK feature, clip the incident edge against the reference
   side planes, keep ≤2 points, offset each by the radii). Produces up to **2 contact points**.
3. **Deep-overlap fallback:** when the cores interpenetrate (GJK degenerate), **SAT** over the
   polygon axes gives the min-penetration axis + reference face; **EPA** is the general fallback
   for arbitrary deep convex overlap where the SAT axis set is insufficient (rare for the shapes
   we author, but it makes the core fully general). Circle/capsule cores (1–2 verts) take the
   segment-distance fast path.

`Dispatch` collapses from a kind-pair table to a single `Collide(shapeA, xfA, shapeB, xfB,
speculativeMargin)`; the specialized circle/capsule files fold into the unified segment-distance
core path.

### 3. Stable feature-ID manifolds (the warm-start fix)

Each `ManifoldPoint.id` becomes a **stable feature key** encoding the contacting features
(reference-edge index, incident vertex/edge index, and which shape is reference), packed into the
`uint32` — **not** a depth rank. The solver's warm-start cache (already `id`-keyed) then matches a
contact to the *same physical feature* across frames, so accumulated impulses persist correctly →
**stable stacks with far fewer iterations**. This is the single biggest solver-quality win and it
requires **no solver change** — only a manifold that emits stable ids.

### 4. Rotation-aware AABBs + broadphase + per-body lock

- `Shape::ComputeAABB(xf)` rotates the core vertices by `xf.angle`, bounds them, and adds the
  radius. `PhysicsWorld::SlotAabb` and `GenerateContacts` feed the body's **actual `m_angle`**
  (not `0`). Fast movers' velocity-scaled speculative margin (M6 P3.1) still applies.
- **DynamicTree / SpatialHash / SAP** broadphases are unchanged — they consume AABBs and don't
  care that the AABBs are now rotation-aware. **TileGrid is unchanged** (static passability source;
  tiles are axis-aligned by definition).
- **Per-body rotation lock:** the existing `BodyDef::fixedRotation` flag (zeros `invInertia`)
  already prevents the solver from spinning a body; with the rotation-aware narrowphase its angle
  simply stays constant and the collider stays at that fixed orientation. "Disable rotation" is
  one flag, for free.

### 5. Solver / ContactManager / Island

**Unchanged in behavior.** The solver is already rotation-complete (above). `GenerateContacts`
now produces geometrically-correct rotated manifolds with stable ids; the solver consumes them
exactly as today. ContactManager event gating and Island sleep are orientation-agnostic (they key
on slots/overlap) and carry forward as-is.

## What changes, module by module

| Module | Change |
|---|---|
| `Shapes.{hpp,cpp}` | Unified core+radius representation; `ComputeAABB` rotation-aware; `ComputeMass` per core+radius. Constructors retained. |
| `Narrowphase/Gjk` | Operate on world cores; drive separation + closest features + speculative gap. |
| `Narrowphase/Sat` + new EPA | Penetration axis / reference face for deep overlap; EPA fallback. |
| `Narrowphase/Manifold` | Reference-face **clipping** (replaces contained-vertex); **stable feature-id** emission; ≤2 points + radii. |
| `Narrowphase/Specialized`, `Dispatch`, `GeometryKernel` | Fold into the unified core path; `Dispatch` → single `Collide(...)`. |
| `PhysicsWorld::GenerateContacts`, `SlotAabb`, `ComputeAABB` callsites | Feed real `m_angle` everywhere (kill the `Real(0)` angle). |
| `Solver/*`, `ContactManager`, `Island` | **No behavioral change** (consume the better manifolds). |
| `premake5.lua` (Core, both flavors) | Add `floatingpoint "Strict"` / `fp_contract(off)` to the physics build. |
| `Tests/` | Retire oracle bit-match parity; add invariant suite + new goldens; archive Lua feel-reference traces. |

## Re-baseline test strategy

- **Retire** the Lua-oracle bit-match assertions (the `physics_oracle/*.json`-pinned tests). The
  capture program stays in the repo but is repurposed to produce the **archived Lua feel-reference
  traces** (overworld slide paths, query results) used by Phase B, not as a C++ gate.
- **Property / invariant suite** (the new correctness gate): no-tunnel (incl. fast + rotating
  bodies), penetration < `kLinearSlop` at rest, energy bounded (no blow-up), run-twice
  determinism (the P3.4 replay, extended with rotating bodies), broadphase-strategy invariance
  (Tree/Hash/SAP identical), slide-no-catch over merged TileGrid spans, **rotation-specific**:
  a box dropped at an angle settles flat, a spinning body's manifold normal is correct, warm-start
  cache stays bounded as contacts persist by feature id.
- **New golden hashes** captured from the v2 engine for representative scenes (stacks, joints,
  CCD, rotation) — the per-platform self-consistency anchors going forward.
- Shape mass/AABB/centroid analytic tests survive (they're math, not oracle).

## Determinism

`f32`, fixed 60 Hz, stable index iteration, sorted broadphase pairs, no wall-clock — all retained.
**New:** FP contraction off in the physics TUs (closes the M6 deferred item; safe to add now
because goldens are re-captured). Dual-flavor (Arcane /MD + server static-CRT ArcaneCore) preserved.

## Non-goals (explicitly deferred)

- **Swept CharacterController** + overworld migration/feel-parity → **Phase B**.
- **Gameplay materials** (one-way platforms, surface velocity, friction/restitution tables,
  material-aware contacts, richer events) → **Phase C**.
- **Soft bodies / particles (XPBD)** → **Phase D**. **Destruction** → **Phase E**.
- Cross-platform bit-determinism (fixed-point) — out of scope by decision #1.

## Risks & mitigations

- **Live-overworld feel divergence.** Deferred, not present: the live client runs Lua; the C++
  core is not load-bearing yet. Mitigated by archiving Lua feel-reference traces now and gating
  the eventual port on them in Phase B.
- **Manifold robustness regressions vs the well-tested oracle manifold.** Mitigated by the
  property/invariant suite (stacking, rest penetration, no-jitter) + the A/B Baumgarte cross-check
  (retained) catching solver-agnostic geometry bugs.
- **EPA correctness/termination.** Mitigated by SAT covering the polygon-core common case; EPA is
  a bounded-iteration fallback with explicit termination + tests; circles/capsules use the exact
  segment-distance path (no EPA).
- **Branch base.** This work builds on the M6 engine (branch `feature/arcane-m6-physics`, pushed,
  awaiting CI+merge); the v2 branch is cut from M6's HEAD and folds in once M6 merges to main.

## Success criteria

- A dynamic body the solver rotates **collides as a rotated body** (rotation-aware manifold/AABB);
  `fixedRotation` locks a body's orientation with one flag.
- One unified shape model + one `Collide(...)` narrowphase entry; specialized/dispatch split gone.
- Manifolds carry **stable feature ids**; warm-start cache matches by feature across frames
  (verified by a persistence test).
- Property/invariant suite + new goldens green, Debug + Release, both backends; server-flavor
  ArcaneCore builds; FP-contraction-off in effect.
- TileGrid behavior unchanged; no presentation/dialect leak into Core.

## Next step

On approval: `writing-plans` → a phased Phase-A implementation plan (unified shapes → rotation-aware
GJK/SAT/EPA → clip manifolds + stable ids → feed real angle + rotation-aware AABB → re-baseline
tests → fp_contract + dual-flavor gate), executed via subagent-driven-development.
