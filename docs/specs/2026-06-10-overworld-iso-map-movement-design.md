# Overworld Isometric Map + Movement Design

**Date:** 2026-06-10
**Status:** DRAFT — pending review
**Scope:** Client-first. Own map format replacing Tiled/STI, isometric world renderer matching combat's projection, dual movement modes (WASD / Wakfu-style click-to-move) behind a gameplay setting, A* pathfinding with switchable topology, removal of `--export-arenas` and the portal system. Editor (LevelEditor + engine-ification overhaul) is the explicit second phase and gets its own spec.
**Companion:** `2026-06-10-client-physics-engine-design.md` (cells-authoritative collision; Box2D removal)

---

## Goal

Make the overworld and combat one visual and structural space. The world becomes an isometric cell grid rendered through the canonical pipeline with the exact projection combat already uses, authored in our own JSON format (Tiled/STI leave the codebase), navigated either freely (WASD) or cell-to-cell (right-click pathing) — exclusively, by player setting — with per-cell data that already carries the combat semantics (walkability, obstacle class) that the future Wakfu-style in-place Combat Sphere will consume.

**In scope (this phase):** map schema + loader, iso world renderer with placeholder art, both movement modes + setting + input integration, pathfinding, removals.
**Out of scope (later phases):** Combat Sphere / in-place combat initiation (consumes this schema, separate phase), LevelEditor + editor engine-ification (separate spec, before or in lock-step with LevelEditor), server-authoritative position, multi-map/warps, enemy roaming AI.

---

## Context

- Combat renders an isometric diamond grid via `combat/Grid.lua` (`toScreen`, `TILE_W`/`TILE_H` 2:1) — this projection is the definition of "isometric" for the game and the overworld must share it exactly.
- The overworld today is an orthogonal Tiled map (`data/map/lobby.lua`, 2.9k-line STI export) + Box2D free movement + an `arenaPortal` entity for combat transition. All three are being removed.
- Research (2026-06-10) confirmed the Wakfu target model: combat is initiated in place on the world's own cells — a bounded Combat Sphere with blue/red placement cells, border penalty tiles, and terrain-driven LOS (tall obstacles block LOS+movement, low obstacles block movement only). The map schema must carry those distinctions from day one even though combat-on-map ships later.
- The tools suite (ImGui editor) will author maps in phase 2. The UIEditor pattern (WYSIWYG canvas → JSON on disk → UDP live preview → client hot reload) is the proven template; this spec's loader must leave that seam open.
- Standing mandates: homogenized rendering (one canonical scene-producer path), FFI-friendly design (typed buffers, low GC churn), action-model input (Unity-style contexts in `services/Input.lua`).

---

## Removals

1. **`--export-arenas`** (Combat server pre-build via lovec) and everything that consumes `data/arenas/`. Arenas become map regions in a later phase; the Combat server stub will consume map JSON when that lands. Remove the export handler, the pre-build step in the server premake, and the export-time code paths in the client.
2. **Portal system**: the `arenaPortal` entity convention, the off-screen portal indicator in `RenderSystem` (~150 lines, already flagged as residue), and any portal-driven combat transition. Combat entry remains the F4 test encounter until the Combat Sphere phase.
3. **Tiled/STI**: `thirdparty.sti` requires, `data/map/lobby.lua`, STI-specific code in `Game:loadGameWorld`. `InterpolationSystem` (network-entity lerp with no producer) is removed in the same sweep unless a networked-overworld plan claims it.
4. **Box2D / PhysicsSystem**: per the companion physics spec. Until physics M1 lands, WASD may temporarily keep Box2D *on the old map only*; the new map path never links Box2D.

---

## Map schema — `data/maps/<id>.json`

One file per map, loaded via `services.Assets.data` like every other JSON. Versioned (`"version": 1`); the loader rejects mismatches loudly (same convention as `assets/data.lua` opts.version).

```json
{
  "version": 1,
  "id": "lobby",
  "name": "Lobby",
  "grid": { "w": 32, "h": 24 },
  "tileset": "lobby_tiles",
  "layers": [
    { "name": "ground",  "tiles": [0, 1, 1, 2, "..."] },
    { "name": "deco",    "tiles": [0, 0, 5, 0, "..."] }
  ],
  "cells": {
    "flags": [0, 0, 1, 3, "..."]
  },
  "entities": [
    { "type": "player_spawn", "cell": [4, 5] },
    { "type": "prop", "key": "crate_big", "pos": [7.5, 3.25], "collider": "aabb" }
  ],
  "ambience": { "background": "void_background" }
}
```

Schema facts:
- **Projection constants are NOT in the file.** Tile pixel dimensions live in one shared code module (see IsoProjection below) so combat and overworld can never disagree. The file stores logical cells only.
- **`layers[].tiles`**: row-major `w*h` arrays of tileset indices, `0` = empty. Flat numeric arrays — directly consumable as FFI buffers later; no nesting, no per-tile objects.
- **`cells.flags`**: one flat `w*h` array, bitfield per cell:
  - bit 0 — `WALKABLE`
  - bits 1-2 — obstacle class: `0=none, 1=LOW` (blocks movement only), `2=TALL` (blocks movement + line of sight) — the Wakfu distinction, stored now, consumed by the Combat Sphere phase
  - bit 3 — `COMBAT_ALLOWED` (cell may be part of a Combat Sphere)
  - bits 4-7 reserved (height tiers, water, etc.)
  Cell flags are the **single source of truth for passability** — the physics engine derives world statics from them (companion spec), the pathfinder reads them directly, and the future LevelEditor authors them directly. Nothing else encodes "where can you stand."
- **`entities`**: typed placements. `cell` for grid-aligned, `pos` (fractional cell coords) for off-grid props — supporting the by-design case of objects not aligned to the grid. Prop colliders are declared here and registered with the physics world as off-grid statics.
- **Tileset**: an Assets image key + an implied uniform diamond slicing. Until art exists, the renderer falls back to programmer-art flat diamonds (per-index color hash), exactly like combat's `TILE_COL` look — the schema and code paths are real, only the texture is absent.

## IsoProjection — shared module

`src/world/IsoProjection.lua`: `TILE_W`, `TILE_H`, `toScreen(cx, cy)`, `toCell(sx, sy)` (exact inverse for picking), `cellCenter(cx, cy)`. Constants are lifted from `combat/Grid.lua` so the two views are pixel-identical. `combat/Grid` delegates to this module **in P1** — the combat-code ownership boundary was lifted 2026-06-10, so no mirroring/lockstep-comment interim is needed.

## Runtime model + loader

- `src/world/Map.lua` — immutable-ish runtime map: dimensions, layer arrays, cell flags, entity defs. Accessors: `isWalkable(cx, cy)`, `obstacleClass(cx, cy)`, `inBounds`, `flagAt`. Internally plain flat arrays now, `ffi.new("uint8_t[?]")`-ready (accessor seam, no exposed table iteration).
- `src/world/MapLoader.lua` — `MapLoader.load(id)` via `Assets.data("maps/<id>")`, validation (array lengths == w*h, version, unknown entity types warn-and-skip), returns `Map`. **Hot-reload seam:** `MapLoader.loadFromTable(tbl)` mirrors `ui_loader.loadFromJSON` so the phase-2 editor's UDP preview can push maps without touching disk.
- `Game:loadGameWorld` builds the world from `Map` instead of STI: spawns from `player_spawn`, physics statics from cell flags + prop entities, world renderer wired as the scene producer.

## World renderer

A scene producer on the canonical pipeline (no bespoke chain — homogenization mandate):
- **Tile pass:** one `SpriteBatch` per layer per tileset texture (P4 primitives, finally adopted). Placeholder mode batches colored diamond meshes via `MeshBatch`. Frustum-culled by visible cell range (cheap on iso: screen rect → cell bounds via `toCell` of corners).
- **Y-sort pass:** entities and TALL-obstacle deco tiles sorted by cell `(cx + cy)` (iso depth) and drawn interleaved; LOW obstacles and ground never occlude. The sort uses a preallocated index array re-sorted in place (FFI mandate: no per-frame table churn).
- **Overlay hooks:** a thin overlay layer for cell highlights (hover cell, path preview, click marker now; blue/red placement cells and sphere border later). Combat's tile highlighting migrates onto this in the Combat Sphere phase, collapsing `CombatRenderer`'s grid drawing into the world renderer.
- Camera unchanged; the deferred camera-interpolation fix (2026-06-10 review M8) should land as part of this work since the follow target moves to cell-aware space anyway.

## Movement

**Setting:** `movement_mode = "wasd" | "click"` in `services/Settings.lua` (gameplay section) + a settings-screen toggle. Exclusive — switching modes cancels any active path and swaps input context. Runtime-switchable.

**One owner:** `src/world/MovementController.lua` is the single switch point. It owns the active mode, subscribes the right input context, and drives the entity's motion intent each `fixedUpdate`. Both modes emit the same intent shape — `{ vx, vy }` desired velocity + facing — consumed identically by the physics character controller, so animation/facing/footstep logic is mode-blind.

- **WASD mode:** Input context `world_wasd` — the existing action-map pattern; the move action's vector → normalized desired velocity. Collision resolved by the physics engine against cell statics (cells authoritative; identical passability to click mode by construction).
- **Click mode:** Input context `world_click` — `move_to` action bound to right-click. Pointer → `IsoProjection.toCell` → if walkable and reachable, request a path; the **PathFollower** advances cell-center to cell-center at move speed with continuous tweened motion (the entity is kinematic — physics doesn't fight the follower; see companion spec). New click re-paths from the current position (Wakfu behavior). Unreachable/unwalkable click: feedback marker, no movement. Hover shows the cell highlight.

**Input integration:** both contexts live in `data/input_actions.json` alongside existing maps; `MovementController` pushes/pops the context on mode switch — one spot, matching the established `Input.activeContext` discipline.

## Pathfinding

`src/world/Pathfinder.lua` — A* over cell flags.
- **Topology is a strategy object** — the clean switch point requested: `Topology4` and `Topology8`, each providing `neighbors(cx, cy, visitFn)`, `cost(dx, dy)`, `heuristic(ax, ay, bx, by)` (Manhattan / octile). Selected in one place (a dev setting / debug toggle to feel both out; ship decision later). `Topology8` enforces no-corner-cutting: a diagonal step requires both adjacent orthogonal cells passable.
- **Implementation:** binary-heap open list and flat `g`/`came_from` arrays preallocated at map-load size and reused per query (zero steady-state allocation; FFI-ready). Early-out when start==goal; bounded expansion (map size) so a degenerate query can't spike a frame. Path returned as a reusable array of cell indices.
- Pathfinder reads `Map.isWalkable` only — the same truth physics derives from, so click mode and WASD can never disagree about passability.

---

## Phasing

- **P1 — substrate:** schema + `IsoProjection` + `MapLoader`/`Map` + world renderer (placeholder art), hand-authored `data/maps/devtest.json`, loaded behind the existing world entry (replaces STI path). Removals land here (STI/Tiled, portal, `--export-arenas`).
- **P2 — movement:** physics M1 (companion spec) + `MovementController` + both input contexts + setting + Pathfinder + PathFollower + cell highlight overlays.
- **P3 (separate specs):** editor engine-ification + LevelEditor (authoring + UDP map preview against `MapLoader.loadFromTable`); Combat Sphere phase (consumes `cells.flags`, collapses CombatRenderer tile drawing into the world renderer).

## Acceptance (client-first, no assets)

1. `devtest.json` renders as an iso diamond map through the canonical pipeline, projection-identical to combat (overlaying combat's grid on the same cells aligns exactly).
2. Toggling the gameplay setting switches WASD ↔ click-to-move at runtime; only one is ever active; both respect identical passability.
3. Right-click paths around LOW/TALL obstacle cells; 4-dir/8-dir switchable in one place; re-path on new click; no per-query allocation after warmup.
4. STI, Tiled data, Box2D (new path), portal code, and `--export-arenas` are gone from the build.
5. Harness coverage: a `world_harness` (pattern: existing harnesses) unit-tests `IsoProjection` round-trips, `Map` flag accessors, and Pathfinder topologies against fixture grids.
