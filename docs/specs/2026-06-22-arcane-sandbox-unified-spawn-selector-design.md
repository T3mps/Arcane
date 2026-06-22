# Arcane Sandbox — Unified Spawn-Shape Selector

Date: 2026-06-22
Status: Design approved (brainstorming), pending spec review -> implementation plan.
Branch: feature/arcane-sandbox-spawn-selector (off main @ 22db259)

## 1. Motivation

The Sandbox HUD currently exposes two parallel spawn mechanisms:
- A **Spawn** section: a Box/Circle combo + Size/Density sliders; left-click empty
  space spawns the chosen shape immediately.
- A separate **Polygon** section: a `Polygon mode` checkbox (backed by an
  independent `m_polygonMode` bool in `Interaction`); while on, left-clicks collect
  vertices and a `Spawn polygon` button commits the convex hull.

Two sections, two interaction modes, and two pieces of state (`SpawnConfig.shape`
and `m_polygonMode`) that can disagree. Unify them into ONE spawn-shape selector
where Polygon is simply another shape, with adaptive controls and nicer UX.

## 2. Goals / Non-goals

Goals:
- One shape selector (segmented buttons) offering Box / Circle / Capsule / Polygon.
- Shape is the single source of truth; the `m_polygonMode` bool is removed
  (polygon mode is derived: `shape == Polygon`).
- Per-shape adaptive controls in a single Spawn section.
- Polygon authoring gains fluid in-world shortcuts (Enter spawn / Backspace undo /
  Esc clear) alongside the discoverable HUD buttons.
- Capsule joins the instant-spawn shapes (a new `SpawnCapsule` builder).

Non-goals (YAGNI):
- No new physics, no convex-hull change (the polygon path still uses the existing
  `ConvexHull<MonotoneChain>` + world-direct `MakePolygon`/`AddBody` flow).
- No per-shape density/size presets, no spawn-on-drag sizing, no rotation control
  on spawn (capsule spawns upright).
- No keyboard-capture gating on the polygon shortcuts (the sandbox HUD has no text
  inputs; this matches how the existing `=`/`-` zoom keys already work).

## 3. Shape is the single source of truth

`Interaction.hpp`:
- Extend `enum class SpawnShape : std::uint8_t { Box=0, Circle=1, Capsule=2, Polygon=3 }`.
- `SpawnConfig.size` doc updated: box half-extent | circle radius | capsule radius
  (ignored for Polygon, whose geometry comes from the clicked points). `density`
  applies to every shape including the polygon hull body.
- **Remove** the `m_polygonMode` member. Polygon mode is derived everywhere from
  `m_spawn.shape == SpawnShape::Polygon`.
- `SandboxApp` accessors: `IsPolygonMode()` returns `SpawnCfg().shape == Polygon`;
  `SetPolygonMode(bool)` is REPLACED by shape get/set on `SpawnConfigMut()` (the HUD
  and tests set `shape` directly). If a thin `SetPolygonMode` shim is retained for
  test churn it must set `shape = on ? Polygon : Box` and nothing else -- but prefer
  migrating callers to shape get/set so there is exactly one knob.

## 4. Interaction (`Tick` branches on shape)

`Interaction::Tick` (`Interaction.cpp`), after the existing camera nav + capture
guard, branches on `m_spawn.shape`:

- **Polygon:** an LMB press-edge over the world adds a vertex (today's collect
  behavior, unchanged). PLUS edge-detected keyboard shortcuts (one action per
  press, NOT per held frame):
  - `Enter` (SDLK_RETURN, and KP_ENTER) -> request a polygon spawn (same deferred
    `RequestPolygonSpawn` path the HUD button uses; a < 3-point list is a no-op).
  - `Backspace` (SDLK_BACKSPACE) -> pop the last collected point (no-op if empty).
  - `Esc` (SDLK_ESCAPE) -> clear all collected points.
  LMB still belongs to the polygon (no grab/spawn/drive), as today.
- **Box / Circle / Capsule:** an LMB press-edge on EMPTY space spawns that shape at
  the cursor world point with `size` + `density` + the existing `kSpawnTint`:
  - Box -> `SpawnBox(... glm::vec2(size,size) ...)` (unchanged).
  - Circle -> `SpawnCircle(... size ...)` (unchanged).
  - Capsule -> `SpawnCapsule(... radius=size ...)` (new builder, Section 5).
  Picking/drag/throw on an occupied body is unchanged.

Edge detection: add per-key previous-state tracking for the three shortcut keys
(mirroring the existing LMB `m_prevButtons` press-edge pattern) so a held key fires
once. Recorded at the end of `Tick` like the mouse-button state. The shortcuts are
only consulted when `shape == Polygon`.

Draft-marker publish gating: the `PolygonDraftResource` is populated with
`PolygonPoints()` ONLY when `shape == Polygon`; otherwise it publishes an empty
list (so switching to Box/Circle/Capsule hides stray markers). In-progress points
are RETAINED across a shape switch (switching back to Polygon resumes them); Esc /
Clear discard them.

## 5. New `SpawnCapsule` builder

Add to `Scenes.hpp`/`Scenes.cpp`, mirroring `SpawnBox`/`SpawnCircle`:

```cpp
Astra::Entity SpawnCapsule(Astra::Registry& reg, Astra::Entity root,
                           glm::vec2 pos, float radius,
                           Arcane::Physics::BodyType type,
                           glm::vec4 tint, float density);
```

Builds a body with `Physics::MakeCapsule` and the existing capsule sprite
(`SpriteShape::Capsule`, from the shape-aware-sprite work). Spawned UPRIGHT as a
pill: radius = `radius`, segment half-length = `radius` (so total height = 4*radius,
a clean 2:1 pill). Density passed through like the box/circle builders.

## 6. HUD — one unified "Spawn" section (`Hud.cpp`)

Remove the separate `Polygon` `CollapsingHeader`. The `Spawn` header becomes:

1. **Segmented shape buttons:** a single row `[ Box ][ Circle ][ Capsule ][ Polygon ]`
   built from a `kShapes[]` name table; the ACTIVE shape's button is highlighted by
   pushing `ImGuiCol_Button` (and `*Hovered`/`*Active`) to the accent color for that
   one button. Clicking a button sets `cfg.shape`. `ImGui::SameLine()` between them.
2. **Density** slider (always shown; applies to all shapes).
3. **Adaptive controls:**
   - Box / Circle / Capsule: a **Size** slider + `TextDisabled("Left-click empty space to spawn")`.
   - Polygon: `Points: N` + `[ Clear ]` (disabled when 0) `[ Spawn ]` (disabled when
     < 3) on one line, then
     `TextDisabled("Click to add a vertex - Enter spawn / Backspace undo / Esc clear")`.
     Clear -> `app.ClearPolygonPoints()`; Spawn -> `app.RequestPolygonSpawn()`
     (same calls as today, relocated).

No other HUD sections change.

## 7. Testing (TDD)

Update `SandboxInteractionTest.cpp` + `SandboxHudTest.cpp` (`[sandbox]`):
- Existing polygon cases migrate from `SetPolygonMode(true)` to
  `SpawnConfigMut().shape = SpawnShape::Polygon` (drive clicks via fabricated
  `InputSnapshot`s through `Tick`, as today).
- Box / Circle / **Capsule** each: an LMB press on empty space with that shape
  selected spawns exactly one body (count +1); Capsule exercises the new builder.
- Polygon shortcuts (fabricated `InputSnapshot`s with the keycodes, asserting edge
  behavior): Enter on >= 3 points -> a body spawns + points clear; Enter on < 3 ->
  no-op; Backspace -> point count drops by one; Esc -> points cleared; a HELD key
  fires once (edge), not every frame.
- Draft-marker gating: with shape != Polygon, the published `PolygonDraftResource`
  is empty even if points were collected; switching back to Polygon re-publishes
  them (points retained).
- HUD: selecting a segment sets `cfg.shape`; the Polygon controls appear only for
  the Polygon shape (assert via the app state the HUD drives, using the existing
  HUD-test harness pattern).

MSVC is the source of truth (clangd shows false positives in this repo).

## 8. Build / test notes

- No NEW files (SpawnCapsule goes in existing Scenes.cpp/.hpp; no new test file
  unless one is added -> then regen). If a new test file IS added, regen with
  `cd Arcane && ../ThirdParty/premake5/premake5.exe vs2026` (NOT GenerateProjects.bat,
  which hangs on a `pause`).
- HUD + plugin changes touch Sandbox.dll: build the FULL Debug `Arcane.slnx`
  (not just `-t:ArcaneTests`) so the plugin + tests rebuild.
- Run from the exe dir; `"[sandbox]"` for fast iteration. If SandboxSmoke fails
  "cannot copy source DLL", kill a stray Loom.exe first.
- FINAL GATE: full `ArcaneTests` Debug AND Release (no filter, incl `[gpu]` both
  backends + SandboxSmoke) green + ArcaneCore static-CRT Debug+Release clean.

## 9. File manifest

Edited:
- `Arcane/Sandbox/src/Interaction.hpp` — SpawnShape += Capsule/Polygon; remove
  m_polygonMode; shortcut-key edge-state members; doc updates.
- `Arcane/Sandbox/src/Interaction.cpp` — Tick shape branch (Box/Circle/Capsule/
  Polygon); Enter/Backspace/Esc edge shortcuts.
- `Arcane/Sandbox/src/Scenes.hpp` / `Scenes.cpp` — new `SpawnCapsule` builder.
- `Arcane/Sandbox/src/Hud.cpp` — unified Spawn section (segmented buttons +
  adaptive controls); remove the Polygon header.
- `Arcane/Sandbox/src/SandboxApp.hpp` (+ `.cpp` if the draft-publish gating moves) —
  IsPolygonMode/SetPolygonMode reframed on shape; draft-publish gated on
  shape == Polygon.
- `Arcane/Tests/src/SandboxInteractionTest.cpp`, `Arcane/Tests/src/SandboxHudTest.cpp`.

Untouched: physics solver/narrowphase, `Arcane/Geometry/`, `PhysicsDebugDraw.*`,
engine render internals, the scene roster in `Scenes.cpp` (only the new builder is
added).

## 10. Scope guardrails

A focused Sandbox interaction/HUD refactor + one new spawn builder. One piece of
state (the shape) instead of two. No engine-core behavior change. Honors the
homogenized-render mandate (draft markers + capsule sprites go through the same
Batcher2D paths already established).
