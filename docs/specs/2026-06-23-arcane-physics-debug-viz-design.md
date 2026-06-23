# Arcane Physics — Debug Visualization Suite (Design)

- **Date:** 2026-06-23
- **Status:** Design approved (brainstorm); implementation plan pending.
- **Scope:** New interactive debug rendering for the physics broadphases + narrowphase, in the Sandbox (`Loom`-hosted `Sandbox.dll`), backed by read-only + opt-in instrumentation in presentation-free Core.
- **Relates to:** the collision-module rebuild ([[project_arcane_collision_rebuild_phase2]] — per-fixture `DynamicTree`, `SpatialGrid`, the `Collide` narrowphase + EPA/MPR), `feedback_homogenized_rendering` (one canonical render path), `feedback_engine_evolves_not_frozen` (determinism is the contract).

---

## 1. Context & Motivation

The physics engine now has a per-fixture `DynamicTree` broadphase (movers), a `SpatialGrid` for statics + residency, and a multi-path narrowphase (`Collide`: SAT poly-poly, GJK/EPA deep penetration, MPR fallback, analytic circle/capsule). None of it is visible. The existing Sandbox overlay (`DrawPhysicsDebug`) draws collider outlines, body AABBs, velocities, COM, and center-to-center contact lines — but nothing of the spatial structures or the narrowphase reasoning.

This adds an interactive debug-visualization suite so a developer can *see* the broadphases working and *watch* the narrowphase resolve an individual contact (separating axes, GJK/EPA simplex/polytope in Minkowski space) — a debugging and teaching tool for the engine's hottest, least-observable code.

## 2. Goals / Non-Goals

**Goals**
- Toggle-able visualization of **both broadphases**: the per-fixture `DynamicTree` (mover proxies, fat boxes, candidate pairs) and the `SpatialGrid` (static + residency occupied cells).
- A **contact-manifold overview**: real manifold points + normals + separation for every active contact, color-coded by which narrowphase produced it.
- A **narrowphase inspector**: click-to-pin one contact pair and watch its narrowphase internals — separating axes / support points / normal in world space, and the GJK simplex / EPA polytope in a **separate Minkowski-space canvas**, with a step control to scrub the algorithm's iterations.
- All Core additions are **read-only or opt-in**, **presentation-free**, **static-CRT clean**, and **determinism-neutral** (no effect on the Step path).

**Non-Goals**
- No change to broadphase/narrowphase/solver behavior or results.
- No persistent UI state across sessions; no serialization of debug config.
- Not a profiler (the `--perf` harness already covers timing).
- The inspector renders ONE pinned pair at a time (visualizing all narrowphase internals simultaneously is illegible by construction).

## 3. Decisions Log (from brainstorm)

1. **Two slices, one spec.** Slice A (broadphase + manifold overview) ships first and is independently useful; Slice B (narrowphase inspector) layers on the shared Core seam.
2. **Broadphase detail = structure + candidate pairs** (chosen over structure-only or tree-internals): proxy tight + fat AABBs + candidate-pair lines; grid occupied cells.
3. **Narrowphase inspector = the deep version:** render the actual separating axes and GJK/EPA simplex/polytope (chosen over manifold-only or algorithm-tag-only).
4. **Pair selection = click-to-pin** (chosen over follow-cursor / HUD dropdown): stable to study while reading the Minkowski panel; pinned by fixture id across steps; click empty to unpin.
5. **Depth = stepped/animated** (chosen over final-state-only): record every GJK/EPA iteration; a step slider + auto-play scrubs the simplex/polytope evolution.
6. **Minkowski panel = render-target texture via a reusable `OffscreenCanvas`** (chosen over ImGui-DrawList): nicer output (SDF circles, AA, engine-consistent) AND keeps ONE render path — the inset reuses the Batcher2D draw helpers instead of a parallel `ImDrawList` path (honors `feedback_homogenized_rendering`). Cost: a new reusable engine helper (the plugin cannot render to its own texture today). Confirmed feasible: the first-party `ImGuiNvrhi` backend already binds user textures (`ImGuiNvrhi.cpp` `SetTexID` from an nvrhi handle; per-`ITexture*` binding-set cache).
7. **Recorder = opt-in nullptr-default pointer** threaded through the real `Collide` + sub-routines (chosen over a re-implemented debug narrowphase): the inspector shows the ACTUAL algorithm, and the Step path passes `nullptr` → zero cost.

## 4. Architecture Overview

Three layers; Core stays presentation-free, the Sandbox owns all drawing/UI, a new engine helper bridges offscreen rendering.

```
CORE (Arcane/Core, presentation-free, read-only + opt-in)
  - DynamicTree::ForEachLeaf(fn)                 [proxy tight+fat AABBs]
  - SpatialGrid::ForEachCell(fn) + PhysicsWorld::StaticGrid()/ResidencyGrid()
  - PhysicsWorld::ForEachContactConstraint(fn)   [manifold pool: points/normal/sep/kind]
  - Manifold/ContactConstraint gains NarrowphaseKind kind
  - NarrowphaseTrace recorder + Collide(..., NarrowphaseTrace* = nullptr)
  - PhysicsWorld::DebugCollide(fixA, fixB, &trace)  [re-run real narrowphase, recording]

ENGINE (Arcane/Arcane, the DLL)
  - OffscreenCanvas helper: owns RGBA16F target + Batcher2D + TonemapPass + fit camera;
    Draw(fn) renders a 2D pass into its texture -> ImTextureID for ImGui::Image
  - DrawPhysicsDebug gains broadphase + manifold + inspector-world-overlay draw blocks

SANDBOX (Sandbox.dll, all UI + draw orchestration)
  - SandboxDebugDraw gains the new toggles; Hud.cpp adds checkboxes
  - Interaction: click-to-pin a contact pair
  - Narrowphase inspector: per-frame DebugCollide(pinned) -> trace;
    world overlay via batcher; Minkowski inset via OffscreenCanvas + ImGui::Image + step UI
```

## 5. Component — Core read accessors (Slice A)

All read-only; no behavior change; presentation-free (data + std + glm only).

- **`DynamicTree::ForEachLeaf(const std::function<void(std::uint32_t id, const Aabb2& tight, const Aabb2& fat)>&) const`** — walks `m_leafOfId` (or `m_nodes` filtered by `IsLeaf()`), yielding each live proxy's id + tight + fat AABB. (Fat boxes are otherwise unreachable; tight is also reachable via `LiveFixtureAabbs`, but `ForEachLeaf` is the single clean source for both.)
- **`SpatialGrid::ForEachCell(const std::function<void(int cx, int cy, const std::vector<std::uint32_t>& ids)>&) const`** — iterates occupied cells (`m_cells`), splitting the packed key into `(cx, cy)`. The caller converts to a world AABB via `TileSize()`/`Origin()`.
- **`PhysicsWorld::StaticGrid() const -> const SpatialGrid&`** and **`ResidencyGrid() const -> const SpatialGrid&`** — expose the two grids for `ForEachCell` (statics never move; residency tracks movers).
- **`PhysicsWorld::ForEachContactConstraint(const std::function<void(const ContactConstraint&)>&) const`** — exposes the live `m_contactConstraints` pool (filled by `GenerateContacts`, valid until the next `Step`). Each `ContactConstraint` carries the manifold: bodyA/bodyB, `normal`, `pointCount`, `points[2]` (`anchorA`, `anchorB`, `baseSeparation`), and (per §6) `NarrowphaseKind kind`. World-space contact points are recovered from `anchor + body-COM`. (`world.ForEachContact` stays — it yields only body-slot pairs; the new accessor is the manifold-grade source.)

**Determinism note:** iteration order of these accessors is for *display only* and never feeds the simulation. Where order matters for a stable visual (e.g. cell draw order), the caller sorts; the underlying maps' iteration order is not exposed as semantics.

## 6. Component — NarrowphaseKind + NarrowphaseTrace + DebugCollide (Slice A tag + Slice B trace)

### `NarrowphaseKind` (the algorithm tag)
A small enum on the `Manifold` (and copied to `ContactConstraint`), set by `Collide` at the point each dispatch branch resolves:
`Separated`, `CircleCircle`, `CircleVsPolygon`, `Capsule`, `SatPolygon`, `Epa`, `Mpr`. The exact set mirrors `Collide`'s dispatcher branches; one field write per contact, zero measurable cost. Powers Slice A's manifold color-coding and tells Slice B which trace shape to draw.

### `NarrowphaseTrace` (the recorder, Slice B)
A pure-data struct (in `Arcane/Core/src/Arcane/Physics/Narrowphase/`), holding the recorded geometry for ONE `Collide` invocation. Conceptually:
- `kind` + the two world-space shapes/transforms (so the inset can draw the shapes too if useful) + the final world-space manifold (points + normal + separation).
- **SAT path:** an ordered list of candidate axes, each with `{ axisDir (world), the two projection intervals, isChosen }`; the chosen min-penetration axis + depth.
- **GJK path:** the ordered support points queried (Minkowski-space), and a per-iteration `SimplexSnapshot { verts[1..3] (Minkowski), searchDir, containsOrigin }`.
- **EPA path:** the seed simplex + a per-iteration `PolytopeSnapshot { verts[] (Minkowski, ordered), closestEdge {a,b}, edgeNormal, edgeDistance }`; the final normal + depth.
- **MPR path:** per-iteration portal `{ v0,v1,v2 (Minkowski), rayDir }` refinement snapshots.
- Analytic (circle/capsule): closest points + normal (no axes/simplex — a thin trace, drawn but flagged as "analytic, no iterations").

The per-iteration snapshot vectors are what the **step slider** scrubs. The struct is reset/reused per inspect call (caller owns it; no per-call heap churn after warmup).

### Recorder threading + `DebugCollide`
`Collide` and its sub-routines (SAT / GJK / EPA / MPR / circle / capsule) gain an optional trailing parameter `NarrowphaseTrace* trace = nullptr`. When `nullptr` (every Step-path call) nothing is recorded — no branches of consequence, zero overhead, determinism untouched. When non-null, each sub-routine appends its snapshots at the natural points (after computing each candidate axis; at the end of each GJK/EPA/MPR iteration).

`PhysicsWorld::DebugCollide(FixtureHandle a, FixtureHandle b, NarrowphaseTrace& out) const` re-runs the real `Collide` for that fixture pair with `&out`, using the same `ComposeFixtureXf` world transforms the Step uses. Because `Collide` is pure, the re-run reproduces the contact exactly (verified by a test: `DebugCollide`'s final manifold == the Step's manifold for the same pair/state). The inspector calls this each frame for the pinned pair.

## 7. Component — `OffscreenCanvas` (engine helper, Slice B)

A reusable engine-side (`Arcane.dll`) helper that lets a caller render a 2D `Batcher2D` pass into an offscreen texture and display it in ImGui. Mirrors the main render path (one canonical pipeline):

- **Owns:** an RGBA16F linear `Canvas` (target + framebuffer) at a caller-set size, a `Batcher2D`, a `TonemapPass`, and an sRGB8 output texture (the ImGui-displayable result).
- **`Draw(const std::function<void(Batcher2D&)>& fn, glm::vec4 clearColor)`** — clears the canvas, `batcher.Begin(...)`, calls `fn(batcher)` (the caller emits world→inset-space primitives), `batcher.End()`, then `TonemapPass` → the sRGB8 output. Runs on the frame's command list during the render phase, BEFORE ImGui renders, so the texture is ready for the same frame's `ImGui::Image`.
- **`TextureId() -> ImTextureID`** — the output texture as an `ImTextureID` (per the confirmed `ImGuiNvrhi` user-texture binding).
- **`Resize(w, h)`** — recreate the target when the inset panel size changes.

The caller (the inspector) supplies a **fit-to-bounds camera** (a simple world→pixel transform) so the Minkowski difference + origin + simplex fit the inset with padding. The helper is game-agnostic and reusable by any future tool inset.

*Command-list/timing detail* (for the plan): the offscreen pass executes as an early render-phase step on the host's frame command list (before the main canvas pass / tonemap / ImGui), so the texture is valid when ImGui samples it. NVRHI manages the framebuffer transitions.

## 8. Slice A — broadphase + manifold overlay

Follows the existing toggle pattern exactly: a `bool` on `SandboxDebugDraw` → mirrored to the registry resource by `PublishDebug` → copied into `PhysicsDebugDrawOptions` by `PhysicsDebugRenderSystem` → a gated draw block in `DrawPhysicsDebug` (`PhysicsDebugDraw.cpp`). New checkboxes in Hud.cpp's "Debug draw" header:

- **Fixture broadphase** — for each `world.FixtureBroadphase()` proxy via `DynamicTree::ForEachLeaf`: draw the tight AABB (thin) + the fat/margin box (dashed/dimmer); then draw lines between the centers of each candidate **pair** from `world.FixtureBroadphase().Pairs(...)` (mapping fixture slot → world AABB center). Color: a distinct broadphase hue.
- **Static grid** — `world.StaticGrid().ForEachCell(...)`: a tinted filled/outlined rect per occupied cell (cell→world AABB via `TileSize()`/`Origin()`).
- **Residency grid** — `world.ResidencyGrid().ForEachCell(...)`: same, distinct tint (so static vs residency read differently).
- **Contact manifolds** — `world.ForEachContactConstraint(...)`: a small disc at each world-space contact point, a normal arrow (point → point + normal·len), and a separation read (color or length), **color-coded by `NarrowphaseKind`** (a fixed palette: SAT, EPA, MPR, circle, capsule). Added as a NEW "Contact manifolds" toggle **alongside** the existing center-to-center "Contacts" line toggle (non-destructive — both can be on; the legacy line is left untouched).

All drawing uses the existing `Batcher2D::Line/Circle/Rect` + `ToScreen(world*zoom+offset)` in `DrawPhysicsDebug`. Optional thickness/length sliders mirror the existing velocity/COM slider pattern.

## 9. Slice B — narrowphase inspector

### Selection (click-to-pin) — `Interaction`
On a left-click that is NOT consumed by ImGui (the existing `WantCaptureMouse` gate) and NOT a spawn/drag, find the active contact whose world-space contact point (or body-pair midpoint) is nearest the click within a pixel threshold; pin its **fixture pair** by `(FixtureHandle a, FixtureHandle b)` (generation-stamped, so a recycled slot can't alias). Clicking empty space (or a "Clear" button) unpins. The pin persists across steps as long as both fixtures stay valid AND the pair stays in contact; if it separates, keep the pin but show "separated" (the trace still renders the no-overlap GJK result). A small "inspect mode" toggle gates click-to-pin so it doesn't fight spawn/drag.

### World-space overlay (via `DrawPhysicsDebug`, batcher)
For the pinned pair's `NarrowphaseTrace`: draw the two shapes highlighted; the separating axes (SAT) as lines through the shapes with the chosen axis emphasized; support points as markers; the final normal as a bold arrow at the contact; the contact point(s). Drawn at the current step slider position where applicable (e.g. SAT shows all axes; the chosen one highlights).

### Minkowski inset (via `OffscreenCanvas` + ImGui)
An ImGui window ("Narrowphase Inspector") containing:
- A header: the pinned pair (body/fixture ids), `NarrowphaseKind`, final normal + depth, point count.
- The **step control**: a slider over the recorded iteration count + a play/pause auto-advance + step ±1 buttons. Index 0..N selects which `SimplexSnapshot`/`PolytopeSnapshot` to render.
- The **inset image**: `ImGui::Image(offscreen.TextureId(), size)`. Each frame, before ImGui renders, the inspector calls `offscreen.Draw(fn)` where `fn` draws (in Minkowski space, via the fit-to-bounds camera): the Minkowski-difference reference (the support-point cloud / hull), the **origin** (a crosshair), and the current iteration's GJK simplex (point/segment/triangle) or EPA polytope (closed polygon) with the closest edge + edge-normal highlighted, or the MPR portal. Analytic kinds render the closest-point construction instead (and disable the step slider with an "analytic — no iterations" note).
- Resize: when the ImGui content region changes, `offscreen.Resize(...)`.

The inspector lives Sandbox-side (`SandboxApp` owns the pinned pair + the `OffscreenCanvas` + the `NarrowphaseTrace`); `Hud.cpp` draws the window when a pair is pinned.

## 10. Determinism, safety, presentation-free

- **Determinism-neutral:** the recorder is `nullptr` on every Step-path `Collide`; `DebugCollide` is a side-effect-free re-run (reads world state, writes only the trace). No accessor feeds the simulation. Run-twice-identical is unaffected.
- **Presentation-free Core:** all Core additions are data + `std` + `glm` + sibling Physics headers (the recorder holds `Vec2` arrays; the accessors take `std::function`). No render/SDL/ImGui in Core. Compiles `/MD` and static-CRT (`ArcaneCore`).
- **Lifecycle safety:** the pinned pair is generation-stamped; `ForEachContactConstraint` reads a pool valid only between `Step`s (documented; the inspector reads it post-step in the render phase, the correct window).
- **`OffscreenCanvas` lifetime:** owned by `SandboxApp`, created lazily on first pin, torn down with the app; its NVRHI resources follow the engine's existing canvas/device lifetime rules.

## 11. Testing

- **Core accessor unit tests** (`[physics]`): `ForEachLeaf` enumerates exactly the live mover fixtures with tight ⊆ fat; `ForEachCell` enumerates exactly the occupied cells (cross-checked against `QueryAABB` over the world bounds); `ForEachContactConstraint` count == `ActiveContactCount()`; `NarrowphaseKind` is the expected kind for representative shape-pairs (circle/circle → CircleCircle, box/box overlapping → SatPolygon or Epa, etc.).
- **`DebugCollide` reproduction test** (`[physics]`): for several pinned pairs, `DebugCollide`'s final manifold (points/normal/separation) equals the Step's manifold for the same world state — proving the inspector shows the real result.
- **Recorder sanity** (`[physics]`): for a known overlapping box pair, the trace's SAT axes include the expected face normals and the chosen axis matches the manifold normal; for a deep overlap the EPA path records ≥1 polytope snapshot ending at the manifold normal/depth.
- **`OffscreenCanvas`** (`[gpu]`): create → `Draw` a couple of primitives → `RenderErrorCount()==0` and a valid `TextureId()`; resize round-trip.
- **The drawing/UI itself** is a **visual gate in Loom** (Dist build): toggles render the structures; click-to-pin + the step slider animate the simplex/polytope. Sandbox-side draw code is smoke-tested (builds, the systems run) but not pixel-asserted.

## 12. Implementation Sequencing

Gate-green increments; each ships something runnable.

1. **Core accessors + `NarrowphaseKind` tag** — `ForEachLeaf`, `ForEachCell` + grid getters, `ForEachContactConstraint`, the `kind` field set in `Collide`. Unit-tested. (No drawing yet.)
2. **Slice A overlay** — the four checkboxes + `DrawPhysicsDebug` blocks. **Ships the broadphase + manifold viz.** Visual gate.
3. **`NarrowphaseTrace` + recorder threading + `DebugCollide`** — instrument `Collide`/SAT/GJK/EPA/MPR; the reproduction + recorder-sanity tests. (No UI yet.)
4. **`OffscreenCanvas` engine helper** — RGBA16F + batcher + tonemap → ImGui texture; `[gpu]` test.
5. **Slice B inspector** — click-to-pin (`Interaction`), the world overlay, the Minkowski inset + step control. Visual gate.

Slice A = steps 1–2 (independently useful). Slice B = steps 3–5.

## 13. Tunables / Open Items (documented at the definition site)

| Item | Default intent |
|---|---|
| broadphase / grid / manifold colors + line thickness | a fixed debug palette; thickness reuses the existing `lineThickness` slider |
| pinned-pair click pixel threshold | a small radius (e.g. ~12 px) |
| inset size + fit-to-bounds padding | a default panel size; ~10% padding around the Minkowski content |
| auto-play step rate | a slow default (e.g. ~4 iterations/sec), adjustable |

Open (decide at implementation, not blockers): whether the world overlay draws the Minkowski-difference reference too (likely inset-only to avoid world clutter).
