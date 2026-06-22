# Arcane Sandbox — "Brutal Churn" Stress Scene

Date: 2026-06-22
Status: Design approved (brainstorming), pending spec review -> implementation.
Branch: feature/arcane-sandbox-spawn-selector

## 1. Motivation

The Sandbox "Stress test" (scene 8, `BuildStressTest` in `Scenes.cpp`) is not a real
stress test. It drops a 16x8 = 128-body grid of *alternating boxes and circles* into
a pen; the pile collapses and **settles**. It exercises broadphase + island count and
nothing else: no complex shapes (no polygons, capsules, or compounds) and no sustained
interaction. The engine's rich narrowphase paths (SAT poly-poly, capsule-vs-poly
2-point, EPA/MPR deep round-in-polygon, compound multi-manifold) are never touched, and
once the pile settles the solver goes quiet.

Replace it with a **brutal, never-settling churn**: a large, procedurally-generated mass
of mixed complex shapes continuously stirred by kinematic spinners, with the body count
behind a single configurable constant.

## 2. Goals / Non-goals

Goals:
- A single `kStressBodyCount` constant is THE knob; changing it scales the scene.
- Bodies are generated PROCEDURALLY at random per loop iteration (random shape / size /
  tint / orientation), from a SEEDED RNG so the scene is deterministic + testable.
- Full shape mix: boxes, circles, capsules, regular convex polygons (triangles /
  pentagons / hexagons), AND compound multi-fixture bodies.
- Continuous agitation: kinematic cross-spinners that churn the pile forever (sustained
  solver stress), never letting it settle.
- Brutal volume (default 320) -- a benchmark/limit-finder that visibly taxes the
  solver/broadphase; tuned to be heavy but stable (no explosion / tunneling).

Non-goals (YAGNI):
- No engine/physics/render changes. Renders through the existing outline path
  (`DrawPhysicsDebug`); no `PhysicsDebugDraw.*` edits.
- No runtime HUD control of the count (it is a code constant, per the request).
- No continuous re-spawn/recycling of bodies that escape -- the initial fill + the
  spinners are enough; walls contain the mass.
- No per-frame allocation / scene rebuild churn (the scene is built once on switch).

## 3. The single knob + procedural generation

In `Scenes.cpp` (constant exposed via `Scenes.hpp` so the test auto-scales with it):

```cpp
inline constexpr int kStressBodyCount = 320;   // THE knob: scale the brutality here
```

Plus internal tuning constants (named, in the anonymous namespace):
`kStressSeed` (RNG seed, deterministic scene), `kStressSpinnerCount = 3`,
`kSpinnerOmega`, body size range `[kMinBodyHalf, kMaxBodyHalf]`, grid `kPitch`,
arena bounds. Increasing `kStressBodyCount` adds rows to the spawn grid (Section 6),
so the only thing that grows is the falling stack -> a bigger churned pile.

A single loop generates the bodies:

```cpp
std::mt19937 rng(kStressSeed);
for (int i = 0; i < kStressBodyCount; ++i)
{
    // 1. position: grid cell (i) + small random jitter (Section 6)
    // 2. shape:    weighted random pick (Section 6)
    // 3. size/tint/angle: random within ranges
    // ... spawn via the matching maker ...
}
```

Seeded (not wall-clock) RNG: the scene is the SAME every build (reproducible bug repros
+ deterministic tests), while each body is independently random. Changing `kStressSeed`
re-rolls the whole layout.

## 4. Arena

A wide, tall walled pen sized to contain a churning mass (independent of body count):
- Floor: a wide static box near the bottom.
- Two thick, TALL static side walls (tall enough that spinners cannot fling bodies over
  them at the tuned tip speed).
- Bounds roughly `x in [~ -120, ~1600]`, floor `y ~ 900`, walls ~700 tall. The view is
  larger than the default camera; the user zooms out (wheel zoom is smooth) to see it.

The arena is FIXED; only the falling stack scales with `kStressBodyCount`.

## 5. Kinematic cross-spinner agitators

`kStressSpinnerCount` (default 3) kinematic spinners, evenly spaced along the floor at a
y just above it (so falling bodies land on them and get churned):

- Each spinner is a WORLD-DIRECT kinematic body (so the handle is available immediately
  to set spin): `AddBody` with one elongated box fixture (a blade), then `AddFixture`
  with a perpendicular blade -> a CROSS. `BodyType::Kinematic` (never pushed, never
  damped -> spins forever, pushes dynamics).
- Spin: `world.SetAngVelSlot(handle.index, sign * kSpinnerOmega)`, sign ALTERNATING per
  spinner (clockwise / counter-clockwise) for more chaotic churn.
- Tuning: blade half-length ~110-130, half-width ~14-18; `kSpinnerOmega` ~2.0 rad/s so
  tip speed (`omega * bladeLen`) stays well under the wall thickness / dt budget (no
  tunneling). Kinematic mass is moot (invMass/invInertia = 0).

The spinners are the "complex interactions": they keep dozens of bodies in deep,
shifting contact every step -> constant warm-start invalidation + many simultaneous
manifolds, the real solver stress.

## 6. The procedural body generator

### 6.1 Layout (grid-from-count + jitter)
A grid sized to the arena width; extra bodies stack UPWARD (taller drop), so the single
knob scales cleanly:
```
pitch = 2*kMaxBodyHalf + margin                 (~78)
cols  = max(1, floor(arenaInnerWidth / pitch))
row   = i / cols ; col = i % cols
x = spawnLeft + col*pitch + pitch/2 + jitterX   (jitter in +-0.25*pitch)
y = spawnBottom - row*pitch + jitterY           (rows climb above the floor)
```
Pitch >= max body diameter + margin guarantees no DEEP initial overlap (which would
explode); the small jitter + mixed shapes break grid symmetry so the mass tumbles.

### 6.2 Shape pick (weighted random per iteration)
Approximate weights (sum 100): Box 25, Circle 20, Capsule 20, Polygon 25 (n-gon, sides
uniformly {3,5,6}), Compound 10. Per body also randomize size in
`[kMinBodyHalf, kMaxBodyHalf]`, a tint from the existing palette, and (boxes/polygons)
a random initial angle. Low density (~0.05-0.1) + low friction (~0.3-0.5) so the mass
flows and the spinners can churn it.

Makers (mirrors the proven "Mixed shapes" scene pattern):
- Box -> `MakeBox` (path-A), random half-extents (some long/thin, some square).
- Circle -> `MakeCircle` (path-A), random radius.
- Capsule -> `MakeCapsule` (path-A), random length + radius.
- Polygon -> `WorldNgon(world, pos, radius, sides, angle)` (NEW helper, world-direct
  `MakePolygon` of a regular convex n-gon).
- Compound -> a world-direct 2-fixture body (e.g. an L / peanut: `AddBody` + `AddFixture`
  with an offset lobe) so the aggregate COM is off-origin (tips + tumbles).

Path-A bodies are minted by `PhysicsSystem` on the first `FixedUpdate`; world-direct
bodies exist immediately. Both coexist in the one `PhysicsWorld` and render as outlines.

### 6.3 New helper: `WorldNgon`
```cpp
Physics::BodyHandle WorldNgon(Physics::PhysicsWorld& w, glm::vec2 pos,
                              float radius, int sides, float angle,
                              float density = 0.08f);
```
Builds a regular convex polygon (verts on a circle of `radius`, `sides` in 3..8) via
`Physics::MakePolygon` + `AddBody` at `pos`/`angle`, dynamic. Sibling of the existing
`WorldPolygonBox` / `WorldTriangle`.

## 7. Tuning & stability

This is intentionally heavy, but it must not explode or tunnel:
- Pitch >= max body diameter + margin (no deep spawn overlap).
- Spinner tip speed bounded (`kSpinnerOmega` * bladeLen) under the wall thickness;
  thick, tall walls.
- Moderate body sizes (not tiny) so fast bodies don't slip through blades/walls; the
  engine's speculative CCD (M6) covers the rest.
- Low density + low friction so the mass flows (a high-friction pile would jam and
  defeat the churn).
- Headless `Loom --frames N` + the unit test (Section 8) verify it builds and steps
  without NaNs / unbounded positions before the user's visual gate.

If the default 320 proves unstable, the fix is tuning these constants (down spinner
omega / up body size / down count) -- NOT a structural change.

## 8. Testing

Extend the sandbox scene tests (`SandboxVisualsTest.cpp` or a sibling) with a
`[sandbox]` case that builds `BuildStressTest`, steps it ~30 fixed steps, and asserts:
- **Volume scales with the knob:** `world.Count() >= kStressBodyCount` (Count includes
  dynamics + spinners + walls; referencing the exported constant auto-scales the test).
- **Agitators present:** at least `kStressSpinnerCount` kinematic bodies exist (via the
  body-type query; if none is exposed, assert via the count of world-direct kinematic
  handles the builder returns -- the builder can stash them for the test, or a minimal
  `world` type accessor is used).
- **Stable (no explosion):** after stepping, every body position is finite and within a
  generous bound of the arena (no NaN / runaway) -- the brutal scene must still be a
  valid sim.
- **Shape variety:** the build path exercised polygons + capsules + compounds (assert at
  least one world-direct polygon body exists, i.e. `Count` exceeds the path-A entity
  count by the world-direct bodies the generator made).

Determinism: the seeded RNG makes these assertions reproducible. MSVC is the source of
truth (clangd shows false positives in this repo).

## 9. File manifest / scope

Edited:
- `Arcane/Sandbox/src/Scenes.cpp` — rewrite `BuildStressTest`; add `WorldNgon` + a
  kinematic cross-`Spinner` builder; the procedural generator loop + tuning constants;
  update the scene-roster header comment (scene 8 description).
- `Arcane/Sandbox/src/Scenes.hpp` — export `kStressBodyCount` (+ `kStressSpinnerCount`)
  so the test auto-scales with the knob.
- `Arcane/Tests/src/SandboxVisualsTest.cpp` (or a sibling) — the stress-scene test.

Untouched: the other scene builders, `PhysicsDebugDraw.*`, `RenderSystems.hpp`, the
physics core, `Arcane/Geometry/`, the Interaction/HUD layer.

## 10. Scope guardrails

A single-scene content rewrite + two small scene helpers + one test. No engine,
physics, render, or input changes. Honors the outline-render mandate (every body draws
through the same `DrawPhysicsDebug` path) and the determinism rule (seeded RNG, no
`/fp:fast`).
