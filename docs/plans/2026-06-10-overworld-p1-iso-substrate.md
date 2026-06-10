# Overworld P1 — Iso Map Substrate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Tiled/STI orthogonal overworld with the new isometric cell-grid substrate — `IsoProjection` (shared with combat), `Map`/`MapLoader`, a placeholder-art world renderer on the canonical pipeline, a hand-authored `devtest` map — and complete the removal sweep (STI/Tiled, portal indicator, `--export-arenas` residue, `InterpolationSystem`, Box2D off the new path).

**Architecture:** New `Client/src/world/` package: `IsoProjection.lua` is the single source of projection constants (combat's `Grid.lua` delegates to it); `MapLoader.lua` validates versioned JSON from `data/maps/` into immutable-ish `Map` objects with bitfield cell flags; `WorldRenderer.lua` draws culled placeholder diamond tiles (MeshBatch) + Y-sorted TALL obstacles/entities + overlay hooks, invoked from the slimmed `RenderSystem` (still the canonical world scene-producer publishing `ctx.sceneRT`). A small interim `WorldMoveSystem` (explicitly marked for replacement by P2's MovementController + physics M1) keeps WASD usable against `Map.isWalkable` without Box2D.

**Tech Stack:** Love2D 11.5 / LuaJIT (Lua 5.1 — use `bit.band/bor/rshift`, no `//`), tiny-ECS, existing render pipeline (`Pipeline`/`FrameCtx`/`MeshBatch`), headless lovec harness pattern.

**Spec:** `docs/superpowers/specs/2026-06-10-overworld-iso-map-movement-design.md` (P1 scope). Companion physics spec is P2 — **no physics engine work in this plan**.

**Consciously narrowed (documented deviations):**
- The textured-tileset SpriteBatch pass: only the pure quad-slicing math (`WorldRenderer.tilesetQuadRect`) is implemented + unit-tested; the SpriteBatch wiring lands with the first real tileset art. No art exists, so a full textured path would be dead, untestable code.
- Camera-interpolation fix (review M8) is deferred to P2: the spec ties it to "the follow target moves to cell-aware space", which is the P2 MovementController. P1's interim mover integrates in variable-rate `update`, so there is no fixed-step aliasing to fix yet.
- The `world_harness` covers IsoProjection round-trips and Map flag accessors per acceptance #5; the Pathfinder-topology tests named there are P2 (no Pathfinder exists in P1).

**Cell-coordinate convention (used everywhere):** cells are **1-based**, `cx ∈ [1..w]`, `cy ∈ [1..h]`, matching combat's `Grid`. Flat arrays are row-major: `index = (cy-1)*w + cx`. JSON `entities[].cell` values are 1-based.

**Cell flag bitfield (spec §cells.flags):** bit 0 = WALKABLE; bits 1–2 = obstacle class (0 none, 1 LOW, 2 TALL); bit 3 = COMBAT_ALLOWED; bits 4–7 reserved. So: plain floor = `1`, combat floor = `9`, LOW obstacle = `2`, TALL obstacle/wall = `4`.

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `Client/src/world/IsoProjection.lua` | Create | Projection constants + toScreen/toCell/cellCenter (pure math) |
| `Client/src/world/Map.lua` | Create | Runtime map: dims, layers, flags, entities; flag accessors |
| `Client/src/world/MapLoader.lua` | Create | `load(id)` via Assets.data + `loadFromTable(tbl)` validation (hot-reload seam) |
| `Client/src/world/WorldRenderer.lua` | Create | Tile pass (culled MeshBatch diamonds), Y-sort pass, overlay hooks |
| `Client/src/systems/WorldMoveSystem.lua` | Create | INTERIM WASD mover vs cell flags (replaced in P2) |
| `Client/src/tests/world_harness/{conf,main}.lua` | Create | Headless tests: projection round-trip, flags, loader validation, slicing, mover |
| `Client/scripts/gen_devtest_map.ps1` | Create | Deterministic generator for devtest.json |
| `Client/data/maps/devtest.json` | Create (generated) | 24×18 dev map: border walls, TALL/LOW clusters, combat region, spawn, props |
| `Client/src/combat/Grid.lua` | Modify | TILE_W/TILE_H/toScreen/fromScreen delegate to IsoProjection |
| `Client/src/game/Game.lua` | Modify | loadGameWorld → Map path; drop sti/Box2D/fixedUpdate physics |
| `Client/src/systems/RenderSystem.lua` | Modify | Drop STI/map-fade/front-layers/portal/physics-lerp; call WorldRenderer |
| `Client/src/systems/InputSystem.lua` | Modify | Comment fix (MovementSystem → WorldMoveSystem) |
| `Client/src/systems/Camera.lua` | Modify | Comment fix (portal-distort mention) |
| `Client/src/tests/assets_harness/main.lua` | Modify | Parse list: drop deleted PhysicsSystem.lua |
| `Client/conf.lua` | Modify | `t.modules.physics = false`; comment no longer cites sti |
| `Client/src/systems/PhysicsSystem.lua` | Delete | Box2D wrapper — no consumers after Game rewrite |
| `Client/src/systems/MovementSystem.lua` | Delete | Box2D velocity mover — replaced by interim WorldMoveSystem |
| `Client/src/systems/InterpolationSystem.lua` | Delete | network-entity lerp with no producer (confirmed residue) |
| `Client/data/map/lobby.lua` (+ dir) | Delete | STI export |
| `ThirdParty/sti/` | Delete | Vendored STI library |
| `.gitignore` | Modify | Drop `**/data/arenas/` block |
| `Server/Combat/src/CombatServer.hpp` | Modify | Comments: arenas → map regions (later phase) |
| `CLAUDE.md` | Modify | Combat arena-export bullet rewritten |

Execution order matters: new modules + harness first (Tasks 1–6), then the Game/RenderSystem cutover (Task 7), then deletions (Task 8) — files can only be deleted after their last consumer is rewritten.

---

### Task 1: world_harness scaffold + IsoProjection (combat/Grid delegating)

**Files:**
- Create: `Client/src/tests/world_harness/conf.lua`
- Create: `Client/src/tests/world_harness/main.lua`
- Create: `Client/src/world/IsoProjection.lua`
- Modify: `Client/src/combat/Grid.lua:8-9,114-138`

- [ ] **Step 1.1: Create the harness conf (headless, same as render_harness)**

`Client/src/tests/world_harness/conf.lua`:

```lua
function love.conf(t)
    t.console = false
    t.window = false
    t.modules.graphics = false
    t.modules.window   = false
    t.modules.audio    = false
    t.modules.sound    = false
    t.modules.joystick = false
    t.modules.physics  = false
end
```

- [ ] **Step 1.2: Write the failing tests**

`Client/src/tests/world_harness/main.lua` (initial version — grows in later tasks):

```lua
-- Headless unit harness for the overworld substrate (P1):
-- IsoProjection round-trips, Map flag accessors, MapLoader validation,
-- tileset quad slicing, interim mover collision.
-- Run: ThirdParty/love2d/lovec.exe Client/src/tests/world_harness
local CLIENT = love.filesystem.getSource() .. "/../../"
package.path = CLIENT .. "?.lua;" .. CLIENT .. "?/init.lua;" .. package.path

local fails = 0
local function fail(m) fails = fails + 1; print("  FAIL: " .. m) end
local function eq(a, b, what)
    if a ~= b then fail((what or "eq") .. " expected " .. tostring(b) .. " got " .. tostring(a)) end
end
local function approx(a, b, what)
    if math.abs(a - b) > 1e-9 then fail((what or "approx") .. " expected " .. tostring(b) .. " got " .. tostring(a)) end
end

print("== parse-check world files ==")
do
    local touched = {
        "world/IsoProjection.lua",
        "combat/Grid.lua",
    }
    for _, rel in ipairs(touched) do
        local chunk, err = loadfile(CLIENT .. rel)
        if not chunk then fail(rel .. " :: " .. tostring(err)) end
    end
    print(("  %d files checked"):format(#touched))
end

print("== IsoProjection ==")
do
    local Iso = require "world.IsoProjection"
    eq(Iso.TILE_W, 64, "TILE_W")
    eq(Iso.TILE_H, 32, "TILE_H")

    -- toScreen formula (matches combat/Grid historic values)
    local sx, sy = Iso.toScreen(1, 1)
    approx(sx, 0, "toScreen(1,1).x"); approx(sy, 32, "toScreen(1,1).y")
    sx, sy = Iso.toScreen(3, 1)
    approx(sx, 64, "toScreen(3,1).x"); approx(sy, 64, "toScreen(3,1).y")

    -- cellCenter is toScreen + half-tile
    local cxp, cyp = Iso.cellCenter(2, 2)
    approx(cxp, 32, "cellCenter(2,2).x"); approx(cyp, 80, "cellCenter(2,2).y")

    -- Round-trip: every cell's center maps back to that cell
    for cy = -3, 12 do
        for cx = -3, 12 do
            local px, py = Iso.cellCenter(cx, cy)
            local rx, ry = Iso.toCell(px, py)
            if rx ~= cx or ry ~= cy then
                fail(("round-trip (%d,%d) -> (%d,%d)"):format(cx, cy, rx, ry))
            end
        end
    end

    -- Points NEAR a center (within the diamond) still pick that cell
    local px, py = Iso.cellCenter(5, 7)
    local rx, ry = Iso.toCell(px + 10, py + 4)
    eq(rx, 5, "near-center pick x"); eq(ry, 7, "near-center pick y")
end

print("== combat/Grid delegates to IsoProjection ==")
do
    local Iso  = require "world.IsoProjection"
    local Grid = require "combat.Grid"
    eq(Grid.TILE_W, Iso.TILE_W, "Grid.TILE_W delegated")
    eq(Grid.TILE_H, Iso.TILE_H, "Grid.TILE_H delegated")
    -- Same numbers Grid produced before the delegation, with offset support intact
    local gx, gy = Grid.toScreen(4, 2, 100, 200)
    local ix, iy = Iso.toScreen(4, 2)
    approx(gx, ix + 100, "Grid.toScreen ox")
    approx(gy, iy + 200, "Grid.toScreen oy")
    -- fromScreen inverse with offsets
    local cxp, cyp = Iso.cellCenter(6, 3)
    local rx, ry = Grid.fromScreen(cxp + 100, cyp + 200, 100, 200)
    eq(rx, 6, "Grid.fromScreen x"); eq(ry, 3, "Grid.fromScreen y")
end

if fails == 0 then print("\nALL WORLD HARNESS CHECKS PASSED") else print(("\n%d FAILED"):format(fails)) end
os.exit(fails == 0 and 0 or 1)
```

- [ ] **Step 1.3: Run to verify it fails**

Run (from repo root): `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: FAIL — `world/IsoProjection.lua` parse-check fails (file missing) and `module 'world.IsoProjection' not found`.

- [ ] **Step 1.4: Implement IsoProjection**

`Client/src/world/IsoProjection.lua`:

```lua
-- world/IsoProjection.lua
-- THE single source of the game's isometric projection. Combat (combat/Grid)
-- and the overworld both delegate here so the two views are pixel-identical
-- (spec 2026-06-10-overworld-iso-map-movement-design.md §IsoProjection).
--
-- Conventions:
--   * Cells are 1-based, matching combat's Grid.
--   * toScreen(cx, cy) returns the TOP-LEFT of the tile diamond's bounding
--     box (the diamond's top vertex is at x + TILE_W/2, y).
--   * cellCenter is the diamond's center; toCell is its exact inverse
--     (nearest-cell picking, identical to the historic Grid.fromScreen).
-- Pure math, no LOVE dependencies — headless-harness testable.

local IsoProjection = {}

IsoProjection.TILE_W = 64   -- full diamond width
IsoProjection.TILE_H = 32   -- full diamond height (2:1)

local HALF_W = IsoProjection.TILE_W / 2
local HALF_H = IsoProjection.TILE_H / 2

--- Cell -> screen-space top-left of the tile's bounding box.
function IsoProjection.toScreen(cx, cy)
    return (cx - cy) * HALF_W, (cx + cy) * HALF_H
end

--- Center of the cell's diamond (where a unit standing on the cell sits).
function IsoProjection.cellCenter(cx, cy)
    local sx, sy = IsoProjection.toScreen(cx, cy)
    return sx + HALF_W, sy + HALF_H
end

--- Screen point -> nearest cell (exact inverse of cellCenter picking).
function IsoProjection.toCell(sx, sy)
    local rx = sx - HALF_W
    local ry = sy - HALF_H
    local fx = rx / HALF_W
    local fy = ry / HALF_H
    return math.floor((fx + fy) / 2 + 0.5), math.floor((fy - fx) / 2 + 0.5)
end

return IsoProjection
```

- [ ] **Step 1.5: Delegate combat/Grid to IsoProjection**

In `Client/src/combat/Grid.lua` replace lines 8-9:

```lua
-- Tile dimensions for isometric rendering
Grid.TILE_W = 64   -- full tile width  (diamond width)
Grid.TILE_H = 32   -- full tile height (diamond height)
```

with:

```lua
-- Projection is owned by world/IsoProjection (P1, 2026-06-10): combat and the
-- overworld must be pixel-identical, so the constants + conversions delegate.
local Iso = require "world.IsoProjection"
Grid.TILE_W = Iso.TILE_W   -- full tile width  (diamond width)
Grid.TILE_H = Iso.TILE_H   -- full tile height (diamond height)
```

and replace the bodies of `Grid.toScreen` / `Grid.fromScreen` (lines 114-138, keeping their doc comments and offset parameters):

```lua
function Grid.toScreen(gx, gy, ox, oy)
    local sx, sy = Iso.toScreen(gx, gy)
    return sx + (ox or 0), sy + (oy or 0)
end
```

```lua
function Grid.fromScreen(sx, sy, ox, oy)
    return Iso.toCell(sx - (ox or 0), sy - (oy or 0))
end
```

(Delete the now-inlined math/comment lines inside the old bodies; the function doc comments above each stay.)

- [ ] **Step 1.6: Run tests to verify they pass**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: `ALL WORLD HARNESS CHECKS PASSED`, exit 0.

- [ ] **Step 1.7: Commit**

```bash
git add Client/src/world/IsoProjection.lua Client/src/combat/Grid.lua Client/src/tests/world_harness
git commit -m "feat(client): IsoProjection shared module; combat Grid delegates to it"
```

---

### Task 2: Map runtime model

**Files:**
- Create: `Client/src/world/Map.lua`
- Modify: `Client/src/tests/world_harness/main.lua` (append section, extend parse list)

- [ ] **Step 2.1: Write the failing tests**

In `world_harness/main.lua`, add `"world/Map.lua"` to the `touched` parse list, and insert before the final summary block:

```lua
print("== Map flag accessors ==")
do
    local Map = require "world.Map"
    eq(Map.FLAG_WALKABLE, 1, "FLAG_WALKABLE")
    eq(Map.OBSTACLE_NONE, 0, "OBSTACLE_NONE")
    eq(Map.OBSTACLE_LOW,  1, "OBSTACLE_LOW")
    eq(Map.OBSTACLE_TALL, 2, "OBSTACLE_TALL")

    -- 3x2 fixture: row-major flags
    --   (1,1)=floor (2,1)=combat floor (3,1)=LOW
    --   (1,2)=TALL  (2,2)=floor        (3,2)=unwalkable bare
    local m = Map.new{
        id = "fixture", name = "Fixture",
        w = 3, h = 2,
        tileset = "none",
        layers = { { name = "ground", tiles = { 1, 1, 1, 1, 1, 1 } } },
        flags = { 1, 9, 2, 4, 1, 0 },
        entities = {},
        ambience = {},
    }
    eq(m.w, 3, "w"); eq(m.h, 2, "h")
    eq(m:inBounds(1, 1), true,  "inBounds 1,1")
    eq(m:inBounds(3, 2), true,  "inBounds 3,2")
    eq(m:inBounds(0, 1), false, "inBounds 0,1")
    eq(m:inBounds(4, 1), false, "inBounds 4,1")
    eq(m:inBounds(1, 3), false, "inBounds 1,3")

    eq(m:flagAt(2, 1), 9, "flagAt 2,1")
    eq(m:isWalkable(1, 1), true,  "floor walkable")
    eq(m:isWalkable(2, 1), true,  "combat floor walkable")
    eq(m:isWalkable(3, 1), false, "LOW not walkable")
    eq(m:isWalkable(1, 2), false, "TALL not walkable")
    eq(m:isWalkable(3, 2), false, "bare 0 not walkable")
    eq(m:isWalkable(0, 0), false, "out of bounds not walkable")

    eq(m:obstacleClass(1, 1), Map.OBSTACLE_NONE, "floor class")
    eq(m:obstacleClass(3, 1), Map.OBSTACLE_LOW,  "LOW class")
    eq(m:obstacleClass(1, 2), Map.OBSTACLE_TALL, "TALL class")

    eq(m:combatAllowed(2, 1), true,  "combat bit set")
    eq(m:combatAllowed(1, 1), false, "combat bit clear")

    eq(m:layerTile(1, 2, 1), 1, "layerTile")
    eq(m:layerTile(1, 0, 1), 0, "layerTile OOB returns 0")

    -- pixelBounds: 3x2 map. minX = toScreen(1,2).x = -32; maxX = toScreen(3,1).x + 64 = 128
    -- minY = toScreen(1,1).y = 32; maxY = toScreen(3,2).y + 32 = 112
    local x, y, w, h = m:pixelBounds()
    approx(x, -32, "bounds x"); approx(y, 32, "bounds y")
    approx(w, 160, "bounds w"); approx(h, 80, "bounds h")
end
```

- [ ] **Step 2.2: Run to verify it fails**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: FAIL — parse-check `world/Map.lua` missing, `module 'world.Map' not found`.

- [ ] **Step 2.3: Implement Map**

`Client/src/world/Map.lua`:

```lua
-- world/Map.lua
-- Immutable-ish runtime map: dimensions, layer tile arrays, per-cell flag
-- bitfield, entity placements. Cell flags are the SINGLE SOURCE OF TRUTH for
-- passability (spec §Map schema): the future physics engine derives statics
-- from them and the P2 pathfinder reads them — nothing else encodes "where
-- can you stand".
--
-- Storage is flat row-major Lua arrays today, accessed ONLY through the
-- methods below (index = (cy-1)*w + cx, cells 1-based) so a later FFI pass
-- can swap in ffi.new("uint8_t[?]") buffers without touching callers.

local bit = require "bit"
local band, rshift = bit.band, bit.rshift

local Iso = require "world.IsoProjection"

local Map = {}
Map.__index = Map

-- cells.flags bitfield (spec §cells.flags)
Map.FLAG_WALKABLE       = 1     -- bit 0
Map.FLAG_COMBAT_ALLOWED = 8     -- bit 3
-- bits 1-2: obstacle class
Map.OBSTACLE_NONE = 0
Map.OBSTACLE_LOW  = 1   -- blocks movement only
Map.OBSTACLE_TALL = 2   -- blocks movement + line of sight

--- def: { id, name, w, h, tileset, layers = {{name, tiles}, ...}, flags,
---        entities = {...}, ambience = {...} }. MapLoader validates; Map trusts.
function Map.new(def)
    return setmetatable({
        id       = def.id,
        name     = def.name,
        w        = def.w,
        h        = def.h,
        tileset  = def.tileset,
        layers   = def.layers,
        flags    = def.flags,
        entities = def.entities,
        ambience = def.ambience or {},
    }, Map)
end

function Map:inBounds(cx, cy)
    return cx >= 1 and cx <= self.w and cy >= 1 and cy <= self.h
end

--- Raw flag byte at a cell (0 outside bounds).
function Map:flagAt(cx, cy)
    if not self:inBounds(cx, cy) then return 0 end
    return self.flags[(cy - 1) * self.w + cx]
end

function Map:isWalkable(cx, cy)
    return band(self:flagAt(cx, cy), Map.FLAG_WALKABLE) ~= 0
end

--- OBSTACLE_NONE / OBSTACLE_LOW / OBSTACLE_TALL.
function Map:obstacleClass(cx, cy)
    return band(rshift(self:flagAt(cx, cy), 1), 3)
end

function Map:combatAllowed(cx, cy)
    return band(self:flagAt(cx, cy), Map.FLAG_COMBAT_ALLOWED) ~= 0
end

--- Tileset index on layer `li` at a cell; 0 (empty) outside bounds.
function Map:layerTile(li, cx, cy)
    if not self:inBounds(cx, cy) then return 0 end
    local layer = self.layers[li]
    if not layer then return 0 end
    return layer.tiles[(cy - 1) * self.w + cx]
end

--- World-pixel AABB of the rendered diamond map: x, y, w, h.
function Map:pixelBounds()
    local minX = Iso.toScreen(1, self.h)
    local maxXr = Iso.toScreen(self.w, 1) + Iso.TILE_W
    local _, minY = Iso.toScreen(1, 1)
    local _, maxYb = Iso.toScreen(self.w, self.h)
    maxYb = maxYb + Iso.TILE_H
    return minX, minY, maxXr - minX, maxYb - minY
end

return Map
```

- [ ] **Step 2.4: Run tests to verify they pass**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: `ALL WORLD HARNESS CHECKS PASSED`.

- [ ] **Step 2.5: Commit**

```bash
git add Client/src/world/Map.lua Client/src/tests/world_harness/main.lua
git commit -m "feat(client): Map runtime model with bitfield cell-flag accessors"
```

---

### Task 3: MapLoader (validation + hot-reload seam)

**Files:**
- Create: `Client/src/world/MapLoader.lua`
- Modify: `Client/src/tests/world_harness/main.lua` (append section, extend parse list)

- [ ] **Step 3.1: Write the failing tests**

Add `"world/MapLoader.lua"` to the harness parse list, and insert before the summary:

```lua
print("== MapLoader.loadFromTable ==")
do
    local MapLoader = require "world.MapLoader"

    local function goodTbl()
        return {
            version = 1, id = "t", name = "T",
            grid = { w = 2, h = 2 },
            tileset = "t_tiles",
            layers = { { name = "ground", tiles = { 1, 2, 1, 2 } } },
            cells = { flags = { 1, 1, 4, 9 } },
            entities = {
                { type = "player_spawn", cell = { 1, 1 } },
                { type = "prop", key = "crate", pos = { 1.5, 1.25 }, collider = "aabb" },
            },
            ambience = { background = "void_background" },
        }
    end

    -- Happy path
    local m = MapLoader.loadFromTable(goodTbl())
    eq(m.id, "t", "id"); eq(m.w, 2, "w"); eq(m.h, 2, "h")
    eq(m:isWalkable(2, 2), true, "flags wired")
    eq(m:obstacleClass(1, 2), 2, "TALL wired")
    eq(#m.entities, 2, "entities kept")
    eq(m.ambience.background, "void_background", "ambience kept")

    -- Version mismatch rejects loudly
    local bad = goodTbl(); bad.version = 2
    local ok, err = pcall(MapLoader.loadFromTable, bad)
    eq(ok, false, "version mismatch raises")
    eq(err:find("version", 1, true) ~= nil, true, "version in message")

    -- Wrong layer length rejects
    bad = goodTbl(); table.remove(bad.layers[1].tiles)
    ok, err = pcall(MapLoader.loadFromTable, bad)
    eq(ok, false, "short layer raises")
    eq(err:find("ground", 1, true) ~= nil, true, "layer name in message")

    -- Wrong flags length rejects
    bad = goodTbl(); bad.cells.flags[5] = 1
    ok, err = pcall(MapLoader.loadFromTable, bad)
    eq(ok, false, "long flags raises")

    -- Missing grid rejects
    bad = goodTbl(); bad.grid = nil
    ok = pcall(MapLoader.loadFromTable, bad)
    eq(ok, false, "missing grid raises")

    -- Unknown entity type warns and skips (doesn't raise, doesn't keep)
    bad = goodTbl()
    bad.entities[3] = { type = "mystery_widget", cell = { 2, 2 } }
    m = MapLoader.loadFromTable(bad)
    eq(#m.entities, 2, "unknown entity skipped")
end
```

- [ ] **Step 3.2: Run to verify it fails**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: FAIL — `module 'world.MapLoader' not found`.

- [ ] **Step 3.3: Implement MapLoader**

`Client/src/world/MapLoader.lua`:

```lua
-- world/MapLoader.lua
-- Loads + validates data/maps/<id>.json into a world.Map.
--
-- Two entry points (spec §Runtime model + loader):
--   * MapLoader.load(id)           — disk path via services.Assets ("maps/<id>")
--   * MapLoader.loadFromTable(tbl) — pure validation seam; the phase-2 editor's
--     UDP live preview pushes decoded tables here without touching disk
--     (mirrors ui_loader.loadFromJSON).
--
-- Validation is loud: any structural problem raises (same convention as the
-- Assets data loader's version check). Unknown entity types warn-and-skip.

local Logger = require "services.Logger"
local Map    = require "world.Map"

local log = Logger.create("MapLoader")

local SCHEMA_VERSION = 1

-- Entity types this client knows how to spawn. Unknown types are skipped with
-- a warning so newer maps degrade instead of crashing older clients.
local KNOWN_ENTITY_TYPES = {
    player_spawn = true,
    prop         = true,
}

local MapLoader = {}

function MapLoader.loadFromTable(tbl)
    assert(type(tbl) == "table", "map: not a table")
    if tbl.version ~= SCHEMA_VERSION then
        error(("map '%s': version mismatch (want %d, got %s)")
            :format(tostring(tbl.id), SCHEMA_VERSION, tostring(tbl.version)))
    end
    assert(type(tbl.id) == "string" and #tbl.id > 0, "map: missing id")
    assert(type(tbl.grid) == "table", ("map '%s': missing grid"):format(tbl.id))
    local w, h = tbl.grid.w, tbl.grid.h
    assert(type(w) == "number" and w >= 1 and w == math.floor(w),
        ("map '%s': bad grid.w"):format(tbl.id))
    assert(type(h) == "number" and h >= 1 and h == math.floor(h),
        ("map '%s': bad grid.h"):format(tbl.id))
    local n = w * h

    assert(type(tbl.layers) == "table" and #tbl.layers >= 1,
        ("map '%s': needs at least one layer"):format(tbl.id))
    for li, layer in ipairs(tbl.layers) do
        local name = tostring(layer.name or li)
        assert(type(layer.tiles) == "table",
            ("map '%s': layer '%s' missing tiles"):format(tbl.id, name))
        if #layer.tiles ~= n then
            error(("map '%s': layer '%s' has %d tiles, expected w*h=%d")
                :format(tbl.id, name, #layer.tiles, n))
        end
    end

    assert(type(tbl.cells) == "table" and type(tbl.cells.flags) == "table",
        ("map '%s': missing cells.flags"):format(tbl.id))
    if #tbl.cells.flags ~= n then
        error(("map '%s': cells.flags has %d entries, expected w*h=%d")
            :format(tbl.id, #tbl.cells.flags, n))
    end

    local entities = {}
    for _, ent in ipairs(tbl.entities or {}) do
        if KNOWN_ENTITY_TYPES[ent.type] then
            entities[#entities + 1] = ent
        else
            log.warn("map '%s': skipping unknown entity type '%s'",
                tbl.id, tostring(ent.type))
        end
    end

    return Map.new{
        id       = tbl.id,
        name     = tbl.name or tbl.id,
        w        = w,
        h        = h,
        tileset  = tbl.tileset,
        layers   = tbl.layers,
        flags    = tbl.cells.flags,
        entities = entities,
        ambience = tbl.ambience or {},
    }
end

--- Load data/maps/<id>.json. Raises on missing file or validation failure.
function MapLoader.load(id)
    -- Lazy require keeps loadFromTable usable in the headless harness without
    -- dragging the Assets manifest in.
    local Assets = require "services.Assets"
    local tbl = Assets.data("maps/" .. id)
    if not tbl then
        error(("map '%s': data/maps/%s.json missing or invalid JSON"):format(id, id))
    end
    return MapLoader.loadFromTable(tbl)
end

return MapLoader
```

**Note:** `services.Logger` must load headless. If the harness errors on the Logger require chain, stub it in the harness BEFORE the MapLoader require:
`package.loaded["services.Logger"] = { create = function() return setmetatable({}, { __index = function() return function() end end }) end }`
(check first — Logger may already be love-free).

- [ ] **Step 3.4: Run tests to verify they pass**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: `ALL WORLD HARNESS CHECKS PASSED`.

- [ ] **Step 3.5: Commit**

```bash
git add Client/src/world/MapLoader.lua Client/src/tests/world_harness/main.lua
git commit -m "feat(client): MapLoader with loud validation + loadFromTable hot-reload seam"
```

---

### Task 4: devtest map (generator + JSON)

**Files:**
- Create: `Client/scripts/gen_devtest_map.ps1`
- Create: `Client/data/maps/devtest.json` (generated)
- Modify: `Client/src/tests/world_harness/main.lua` (append devtest sanity section)

Map design (24×18, all rules deterministic):
- Border ring → TALL wall: flag `4`, deco tile `5`.
- Interior default → floor: flag `1`, deco tile `0`.
- Combat region `cx 8..17, cy 6..13` → flag `9` (walkable + combat).
- TALL block `cx 11..12, cy 9..11` → flag `4`, deco `5` (overrides combat region).
- LOW strip `cx 15..18, cy 14` → flag `2`, deco `4`.
- Ground layer: checker — `(cx+cy) % 2 == 0` → tile `1`, else `2`.
- Entities: `player_spawn` at `[5, 9]` (walkable, outside combat region); two props at fractional positions.
- Ambience: `void_background`.

- [ ] **Step 4.1: Write the generator**

`Client/scripts/gen_devtest_map.ps1`:

```powershell
# One-off generator for Client/data/maps/devtest.json (P1 dev fixture).
# Deterministic: re-running always produces the same file. Rules documented in
# docs/superpowers/plans/2026-06-10-overworld-p1-iso-substrate.md Task 4.
$W = 24; $H = 18
$ground = New-Object System.Collections.Generic.List[int]
$deco   = New-Object System.Collections.Generic.List[int]
$flags  = New-Object System.Collections.Generic.List[int]

for ($cy = 1; $cy -le $H; $cy++) {
  for ($cx = 1; $cx -le $W; $cx++) {
    # ground: checkerboard
    if ((($cx + $cy) % 2) -eq 0) { $ground.Add(1) } else { $ground.Add(2) }

    $isBorder = ($cx -eq 1) -or ($cx -eq $W) -or ($cy -eq 1) -or ($cy -eq $H)
    $isTall   = ($cx -ge 11) -and ($cx -le 12) -and ($cy -ge 9) -and ($cy -le 11)
    $isLow    = ($cx -ge 15) -and ($cx -le 18) -and ($cy -eq 14)
    $isCombat = ($cx -ge 8)  -and ($cx -le 17) -and ($cy -ge 6) -and ($cy -le 13)

    if ($isBorder -or $isTall) { $flags.Add(4); $deco.Add(5) }
    elseif ($isLow)            { $flags.Add(2); $deco.Add(4) }
    elseif ($isCombat)         { $flags.Add(9); $deco.Add(0) }
    else                       { $flags.Add(1); $deco.Add(0) }
  }
}

function RowsOf([System.Collections.Generic.List[int]]$list) {
  $rows = @()
  for ($r = 0; $r -lt $H; $r++) {
    $rows += "      " + (($list.GetRange($r * $W, $W) | ForEach-Object { $_ }) -join ", ")
  }
  return $rows -join ",`n"
}

$json = @"
{
  "version": 1,
  "id": "devtest",
  "name": "Dev Test",
  "grid": { "w": $W, "h": $H },
  "tileset": "devtest_tiles",
  "layers": [
    { "name": "ground", "tiles": [
$(RowsOf $ground)
    ] },
    { "name": "deco", "tiles": [
$(RowsOf $deco)
    ] }
  ],
  "cells": {
    "flags": [
$(RowsOf $flags)
    ]
  },
  "entities": [
    { "type": "player_spawn", "cell": [5, 9] },
    { "type": "prop", "key": "crate_big", "pos": [6.5, 13.25], "collider": "aabb" },
    { "type": "prop", "key": "crate_small", "pos": [19.25, 5.5], "collider": "aabb" }
  ],
  "ambience": { "background": "void_background" }
}
"@

$out = Join-Path $PSScriptRoot "..\data\maps\devtest.json"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
[System.IO.File]::WriteAllText((Join-Path (Resolve-Path (Split-Path $out)) "devtest.json"), $json + "`n")
Write-Host "wrote $out ($($flags.Count) cells)"
```

- [ ] **Step 4.2: Run the generator**

Run (PowerShell, from repo root): `powershell -ExecutionPolicy Bypass -File Client\scripts\gen_devtest_map.ps1`
Expected: `wrote ...devtest.json (432 cells)`.

- [ ] **Step 4.3: Add a devtest sanity check to the harness**

The harness is headless but `love.filesystem` is alive — decode the real file through the real loader. Insert before the summary block in `world_harness/main.lua`:

```lua
print("== devtest.json loads through MapLoader ==")
do
    local MapLoader = require "world.MapLoader"
    local json = require "thirdparty.json"   -- resolved by the harness loader below

    local f = io.open(CLIENT .. "../data/maps/devtest.json", "rb")
    eq(f ~= nil, true, "devtest.json exists")
    if f then
        local raw = f:read("*a"); f:close()
        local m = MapLoader.loadFromTable(json.decode(raw))
        eq(m.id, "devtest", "id")
        eq(m.w, 24, "w"); eq(m.h, 18, "h")
        eq(m:isWalkable(1, 1), false, "border wall unwalkable")
        eq(m:obstacleClass(1, 1), 2, "border wall TALL")
        eq(m:isWalkable(5, 9), true, "spawn cell walkable")
        eq(m:isWalkable(11, 9), false, "tall block unwalkable")
        eq(m:obstacleClass(11, 9), 2, "tall block class")
        eq(m:obstacleClass(15, 14), 1, "low strip class")
        eq(m:combatAllowed(10, 10), true, "combat region")
        eq(m:combatAllowed(3, 3), false, "outside combat region")
        local spawn
        for _, e in ipairs(m.entities) do
            if e.type == "player_spawn" then spawn = e end
        end
        eq(spawn ~= nil, true, "player_spawn present")
    end
end
```

`require "thirdparty.json"` needs the ThirdParty loader, which the game's `Client/conf.lua` provides but the harness conf doesn't. Add this once near the top of `world_harness/main.lua`, right after the `package.path` line:

```lua
-- Minimal mirror of Client/conf.lua's thirdparty loader (single-file libs only).
table.insert(package.loaders or package.searchers, 1, function(name)
    local lib = name:match("^thirdparty%.([%w_]+)$")
    if not lib then return end
    local root = CLIENT .. "../../ThirdParty/"
    return loadfile(root .. lib .. "/" .. lib .. ".lua")
        or loadfile(root .. lib .. "/init.lua")
end)
```

(`CLIENT` is `Client/src/`, so `CLIENT .. "../../ThirdParty/"` is the repo-root `ThirdParty/` and `CLIENT .. "../data/..."` is `Client/data/...`.)

- [ ] **Step 4.4: Run tests to verify they pass**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: `ALL WORLD HARNESS CHECKS PASSED`.

- [ ] **Step 4.5: Commit**

```bash
git add Client/scripts/gen_devtest_map.ps1 Client/data/maps/devtest.json Client/src/tests/world_harness/main.lua
git commit -m "feat(client): devtest map fixture + deterministic generator"
```

---

### Task 5: WorldRenderer (placeholder tile pass, Y-sort, overlay hooks)

**Files:**
- Create: `Client/src/world/WorldRenderer.lua`
- Modify: `Client/src/tests/world_harness/main.lua` (pure-math tests, extend parse list)

The renderer has two testable pure functions (`visibleCellBounds`, `tilesetQuadRect`) and a draw path that only runs in-game. TDD covers the pure parts; the draw path is verified by parse-check + the Task 7 in-game cutover.

- [ ] **Step 5.1: Write the failing tests**

Add `"world/WorldRenderer.lua"` to the harness parse list, and insert before the summary:

```lua
print("== WorldRenderer pure math ==")
do
    local WR = require "world.WorldRenderer"

    -- visibleCellBounds: world-space view rect -> conservative cell range.
    -- View centered on cellCenter(5,5)=(32,160) with ±64px extents must
    -- include cell (5,5) and stay clamped to the map.
    local minCx, minCy, maxCx, maxCy = WR.visibleCellBounds(32 - 64, 160 - 64, 32 + 64, 160 + 64, 24, 18)
    eq(minCx >= 1 and minCx <= 5, true, "minCx sane: " .. minCx)
    eq(maxCx >= 5 and maxCx <= 24, true, "maxCx sane: " .. maxCx)
    eq(minCy >= 1 and minCy <= 5, true, "minCy sane: " .. minCy)
    eq(maxCy >= 5 and maxCy <= 18, true, "maxCy sane: " .. maxCy)

    -- Degenerate/offscreen view clamps to valid empty-or-clamped range
    local a, b, c, d = WR.visibleCellBounds(-10000, -10000, -9000, -9000, 24, 18)
    eq(a >= 1 and c <= 24 and b >= 1 and d <= 18, true, "clamped to map")

    -- tilesetQuadRect: uniform 64x32 slicing, row-major from index 1
    local x, y, w, h = WR.tilesetQuadRect(1, 256, 64)
    eq(x, 0, "q1 x"); eq(y, 0, "q1 y"); eq(w, 64, "q w"); eq(h, 32, "q h")
    x, y = WR.tilesetQuadRect(4, 256, 64)
    eq(x, 192, "q4 x"); eq(y, 0, "q4 y")
    x, y = WR.tilesetQuadRect(5, 256, 64)   -- wraps to second row
    eq(x, 0, "q5 x"); eq(y, 32, "q5 y")

    -- placeholderColor: deterministic, distinct for distinct indices
    local r1, g1, b1 = WR.placeholderColor(1)
    local r2, g2, b2 = WR.placeholderColor(2)
    eq(type(r1), "number", "color r")
    eq(r1 == r2 and g1 == g2 and b1 == b2, false, "distinct colors for 1,2")
    local r1b = WR.placeholderColor(1)
    eq(r1, r1b, "deterministic")
end
```

- [ ] **Step 5.2: Run to verify it fails**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: FAIL — `module 'world.WorldRenderer' not found`.

- [ ] **Step 5.3: Implement WorldRenderer**

`Client/src/world/WorldRenderer.lua`:

```lua
-- world/WorldRenderer.lua
-- Draws a world.Map through the canonical pipeline (called from RenderSystem,
-- the overworld scene producer — homogenization mandate: no bespoke chain).
--
-- Passes (spec §World renderer):
--   1. Tile pass — ground + non-TALL deco, frustum-culled to the visible cell
--      range, batched as colored diamonds via MeshBatch (placeholder art).
--      When real tileset art lands, this switches to one SpriteBatch per
--      layer using tilesetQuadRect() for the uniform diamond slicing — the
--      slicing math is implemented + tested now, the SpriteBatch wiring is not
--      (no art exists to exercise it).
--   2. Y-sort pass — entities and TALL deco tiles interleaved, sorted by
--      world-space screen Y (equivalent to (cx+cy) iso depth for cells).
--      Preallocated arrays, re-sorted in place (FFI mandate: no per-frame
--      table churn beyond MeshBatch's own vertex rows).
--   3. Overlay hooks — registered draw functions for cell highlights (hover
--      now; path preview / Combat Sphere placement cells later).
--
-- All drawing happens INSIDE the caller's camera transform (world coords).

local Iso = require "world.IsoProjection"
local Map = require "world.Map"

local WorldRenderer = {}
WorldRenderer.__index = WorldRenderer

local HALF_W = Iso.TILE_W / 2
local HALF_H = Iso.TILE_H / 2

-- Raised-diamond heights for placeholder obstacle art
local TALL_RAISE = 24
local LOW_RAISE  = 8

local CULL_PAD = 2   -- cells of slack around the computed visible range

-- ============================================================================
-- Pure helpers (headless-tested in world_harness)
-- ============================================================================

--- Conservative cell range covering the world-space rect (x0,y0)-(x1,y1).
--- Iso cells form a diamond lattice, so we invert the projection (without
--- rounding) at all 4 corners and take the padded min/max box.
function WorldRenderer.visibleCellBounds(x0, y0, x1, y1, mapW, mapH)
    local minCx, minCy = math.huge, math.huge
    local maxCx, maxCy = -math.huge, -math.huge
    local function corner(sx, sy)
        local fx = (sx - HALF_W) / HALF_W
        local fy = (sy - HALF_H) / HALF_H
        local cx = (fx + fy) / 2
        local cy = (fy - fx) / 2
        if cx < minCx then minCx = cx end
        if cx > maxCx then maxCx = cx end
        if cy < minCy then minCy = cy end
        if cy > maxCy then maxCy = cy end
    end
    corner(x0, y0); corner(x1, y0); corner(x0, y1); corner(x1, y1)
    local a = math.max(1, math.floor(minCx) - CULL_PAD)
    local b = math.max(1, math.floor(minCy) - CULL_PAD)
    local c = math.min(mapW, math.ceil(maxCx) + CULL_PAD)
    local d = math.min(mapH, math.ceil(maxCy) + CULL_PAD)
    -- Fully offscreen: collapse to an empty-but-valid range
    if c < a then c = a end
    if d < b then d = b end
    return a, b, c, d
end

--- Uniform diamond slicing for a tileset image: index (1-based) -> quad rect.
function WorldRenderer.tilesetQuadRect(index, imgW, _imgH)
    local cols = math.max(1, math.floor(imgW / Iso.TILE_W))
    local i = index - 1
    local col = i % cols
    local row = math.floor(i / cols)
    return col * Iso.TILE_W, row * Iso.TILE_H, Iso.TILE_W, Iso.TILE_H
end

--- Deterministic programmer-art color for a tileset index (r,g,b in 0-1).
function WorldRenderer.placeholderColor(index)
    -- Small hash -> muted hue family, combat TILE_COL-adjacent brightness
    local h = (index * 2654435761) % 360
    local base = 0.18 + 0.10 * (((index * 97) % 7) / 7)
    local r = base + 0.10 * math.abs(math.cos(math.rad(h)))
    local g = base + 0.10 * math.abs(math.cos(math.rad(h + 120)))
    local b = base + 0.12 * math.abs(math.cos(math.rad(h + 240)))
    return r, g, b
end

-- ============================================================================
-- Instance
-- ============================================================================

function WorldRenderer.new(map)
    local self = setmetatable({}, WorldRenderer)
    self.map = map

    -- One streaming mesh batch for ALL flat diamonds (placeholder mode draws
    -- untextured, so layers don't need separate batches).
    local MeshBatch = require "systems.render.MeshBatch"
    local format = {
        { "VertexPosition", "float", 2 },
        { "VertexColor",    "byte",  4 },
    }
    self.batch = MeshBatch.new(format, map.w * map.h * 6, { mode = "triangles" })

    -- Static list of Y-sorted cells: TALL-obstacle deco tiles (layers 2+).
    -- {cx, cy, tileIdx} triples in flat arrays; they never move.
    self.tallCx, self.tallCy, self.tallTile = {}, {}, {}
    for li = 2, #map.layers do
        for cy = 1, map.h do
            for cx = 1, map.w do
                local t = map:layerTile(li, cx, cy)
                if t ~= 0 and map:obstacleClass(cx, cy) == Map.OBSTACLE_TALL then
                    local n = #self.tallCx + 1
                    self.tallCx[n], self.tallCy[n], self.tallTile[n] = cx, cy, t
                end
            end
        end
    end

    -- Preallocated y-sort scratch (reused every frame; grown on demand).
    self.sortItems = {}   -- array of {kind, idx, key} rows, reused in place
    self.sortCount = 0

    self.overlays = {}    -- ordered draw hooks: fn(map, camera)
    return self
end

--- Register an overlay draw hook (runs inside the camera transform, after the
--- y-sort pass). Returns fn for symmetry with removal-by-identity later.
function WorldRenderer:addOverlay(fn)
    self.overlays[#self.overlays + 1] = fn
    return fn
end

local function diamond(batch, px, py, r, g, b, a)
    -- px,py = top-left of the cell's bounding box (Iso.toScreen)
    local cx, cy = px + HALF_W, py + HALF_H
    batch:addTriangle(cx, py, px + Iso.TILE_W, cy, cx, py + Iso.TILE_H, { r, g, b, a })
    batch:addTriangle(cx, py, cx, py + Iso.TILE_H, px, cy, { r, g, b, a })
end

local function drawRaisedDiamond(px, py, raise, r, g, b)
    -- Immediate-mode "pillar": darker base diamond + raised top + side facets.
    local cx = px + HALF_W
    local topY = py - raise
    -- left facet
    love.graphics.setColor(r * 0.55, g * 0.55, b * 0.55, 1)
    love.graphics.polygon("fill",
        px, py + HALF_H, cx, py + Iso.TILE_H,
        cx, py + Iso.TILE_H - raise, px, py + HALF_H - raise)
    -- right facet
    love.graphics.setColor(r * 0.7, g * 0.7, b * 0.7, 1)
    love.graphics.polygon("fill",
        cx, py + Iso.TILE_H, px + Iso.TILE_W, py + HALF_H,
        px + Iso.TILE_W, py + HALF_H - raise, cx, py + Iso.TILE_H - raise)
    -- top diamond
    love.graphics.setColor(r, g, b, 1)
    love.graphics.polygon("fill",
        cx, topY, px + Iso.TILE_W, topY + HALF_H,
        cx, topY + Iso.TILE_H, px, topY + HALF_H)
end

local function sortComparator(a, b) return a.key < b.key end

--- Draw the map + entities. Must be called inside camera:attach().
--- @param camera   systems.Camera (for the visible world rect)
--- @param renderW  number internal render width
--- @param renderH  number internal render height
--- @param entities array of ECS entities with .position (RenderSystem.entities)
--- @param drawEntityFn function(e) — draws one entity (RenderSystem.drawEntity)
function WorldRenderer:draw(camera, renderW, renderH, entities, drawEntityFn)
    local map = self.map
    local x0, y0 = camera:screenToWorld(0, 0)
    local x1, y1 = camera:screenToWorld(renderW, renderH)
    local minCx, minCy, maxCx, maxCy =
        WorldRenderer.visibleCellBounds(x0, y0, x1, y1, map.w, map.h)

    -- ---- Pass 1: flat tiles (ground layers + non-TALL deco), batched ----
    self.batch:begin()
    for li = 1, #map.layers do
        for cy = minCy, maxCy do
            for cx = minCx, maxCx do
                local t = map:layerTile(li, cx, cy)
                if t ~= 0 then
                    local cls = map:obstacleClass(cx, cy)
                    local isTallDeco = li >= 2 and cls == Map.OBSTACLE_TALL
                    if not isTallDeco then
                        local px, py = Iso.toScreen(cx, cy)
                        if li >= 2 and cls == Map.OBSTACLE_LOW then
                            -- LOW obstacles draw raised but never occlude:
                            -- still part of the flat pass (immediate mode,
                            -- they're sparse).
                            self.batch:flush()
                            self.batch:begin()
                            local r, g, b = WorldRenderer.placeholderColor(t)
                            drawRaisedDiamond(px, py, LOW_RAISE, r, g, b)
                        else
                            local r, g, b = WorldRenderer.placeholderColor(t)
                            diamond(self.batch, px, py,
                                math.floor(r * 255), math.floor(g * 255),
                                math.floor(b * 255), 255)
                        end
                    end
                end
            end
        end
    end
    self.batch:flush()

    -- ---- Pass 2: Y-sorted TALL deco tiles + entities, interleaved ----
    local items, n = self.sortItems, 0
    for i = 1, #self.tallCx do
        local cx, cy = self.tallCx[i], self.tallCy[i]
        if cx >= minCx and cx <= maxCx and cy >= minCy and cy <= maxCy then
            n = n + 1
            local it = items[n]
            if not it then it = {}; items[n] = it end
            local _, py = Iso.toScreen(cx, cy)
            it.kind, it.idx, it.key = 1, i, py + Iso.TILE_H   -- base of the diamond
        end
    end
    for i = 1, #entities do
        local e = entities[i]
        n = n + 1
        local it = items[n]
        if not it then it = {}; items[n] = it end
        it.kind, it.idx, it.key = 2, i, e.position.y
    end
    -- table.sort needs an exact-length array; trim stale rows from last frame.
    for i = #items, n + 1, -1 do items[i] = nil end
    self.sortCount = n
    table.sort(items, sortComparator)

    for i = 1, n do
        local it = items[i]
        if it.kind == 1 then
            local idx = it.idx
            local px, py = Iso.toScreen(self.tallCx[idx], self.tallCy[idx])
            local r, g, b = WorldRenderer.placeholderColor(self.tallTile[idx])
            drawRaisedDiamond(px, py, TALL_RAISE, r, g, b)
        else
            drawEntityFn(entities[it.idx])
        end
    end
    love.graphics.setColor(1, 1, 1, 1)

    -- ---- Pass 3: overlays (cell highlights etc.) ----
    for i = 1, #self.overlays do
        self.overlays[i](map, camera)
    end
    love.graphics.setColor(1, 1, 1, 1)
end

return WorldRenderer
```

- [ ] **Step 5.4: Run tests to verify they pass**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: `ALL WORLD HARNESS CHECKS PASSED`. (The require pulls `systems.render.MeshBatch` only inside `new()`, so the headless harness — which never constructs an instance — is unaffected.)

- [ ] **Step 5.5: Commit**

```bash
git add Client/src/world/WorldRenderer.lua Client/src/tests/world_harness/main.lua
git commit -m "feat(client): WorldRenderer — culled placeholder tile pass, y-sort, overlay hooks"
```

---

### Task 6: WorldMoveSystem (interim WASD vs cell flags)

**Files:**
- Create: `Client/src/systems/WorldMoveSystem.lua`
- Modify: `Client/src/tests/world_harness/main.lua` (append section, extend parse list)
- Modify: `Client/src/systems/InputSystem.lua:5` (comment)

- [ ] **Step 6.1: Write the failing tests**

Add `"systems/WorldMoveSystem.lua"` to the harness parse list, and insert before the summary:

```lua
print("== WorldMoveSystem.step (interim mover) ==")
do
    local Map = require "world.Map"
    local Iso = require "world.IsoProjection"
    local WMS = require "systems.WorldMoveSystem"

    -- 4x4 fixture: outer ring TALL walls, inner 2x2 floor
    local flags = {}
    for cy = 1, 4 do
        for cx = 1, 4 do
            local border = cx == 1 or cx == 4 or cy == 1 or cy == 4
            flags[(cy - 1) * 4 + cx] = border and 4 or 1
        end
    end
    local m = Map.new{
        id = "move-fix", name = "F", w = 4, h = 4, tileset = "none",
        layers = { { name = "g", tiles = flags } },  -- contents irrelevant
        flags = flags, entities = {}, ambience = {},
    }

    local sx, sy = Iso.cellCenter(2, 2)

    -- No input: no movement, zero speedRatio
    local nx, ny, vx, vy = WMS.step(m, sx, sy, 0, 0, 120, 1 / 60)
    approx(nx, sx, "idle x"); approx(ny, sy, "idle y")
    approx(vx, 0, "idle vx"); approx(vy, 0, "idle vy")

    -- Move right within open floor: advances
    nx, ny = WMS.step(m, sx, sy, 1, 0, 120, 1 / 60)
    eq(nx > sx, true, "moved right")
    approx(ny, sy, "y unchanged on x move")

    -- Walking into a wall is blocked: hammer right for 5 simulated seconds;
    -- position must remain inside a walkable cell the whole time.
    local px, py = sx, sy
    for _ = 1, 300 do
        px, py = WMS.step(m, px, py, 1, 0, 120, 1 / 60)
        local cx, cy = Iso.toCell(px, py)
        if not m:isWalkable(cx, cy) then
            fail(("mover escaped into unwalkable cell (%d,%d)"):format(cx, cy))
            break
        end
    end

    -- Diagonal input is normalized: |v| == speed
    local _, _, dvx, dvy = WMS.step(m, sx, sy, 1, 1, 120, 1 / 60)
    approx(math.sqrt(dvx * dvx + dvy * dvy), 120, "diagonal normalized")
end
```

- [ ] **Step 6.2: Run to verify it fails**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: FAIL — `module 'systems.WorldMoveSystem' not found`.

- [ ] **Step 6.3: Implement WorldMoveSystem**

`Client/src/systems/WorldMoveSystem.lua`:

```lua
-- systems/WorldMoveSystem.lua
-- P1 INTERIM overworld mover. Reads the boolean input component (InputSystem),
-- integrates position directly at a flat speed, and blocks entry into
-- unwalkable cells (Map.isWalkable — the same passability truth the P2
-- physics engine and pathfinder will read).
--
-- REPLACED IN P2 by world/MovementController + the hand-rolled physics
-- CharacterController (specs 2026-06-10-overworld-iso-map-movement-design /
-- 2026-06-10-client-physics-engine-design). Deliberately dumb: no
-- accel/decel curves, no buffs, no dash — just enough to walk the devtest
-- map while the substrate is verified. Writes the movement component fields
-- AnimationSystem reads (velocityX/Y, facingX/Y, speedRatio).

local tiny = require "thirdparty.tiny"
local Iso  = require "world.IsoProjection"

local WorldMoveSystem = tiny.processingSystem()
WorldMoveSystem.filter = tiny.requireAll("input", "movement", "position")

--- Pure step (headless-tested): one tick of movement with per-axis cell
--- blocking. Returns newX, newY, velX, velY.
function WorldMoveSystem.step(map, x, y, ix, iy, speed, dt)
    local len = math.sqrt(ix * ix + iy * iy)
    if len < 1e-6 then return x, y, 0, 0 end
    ix, iy = ix / len, iy / len
    local vx, vy = ix * speed, iy * speed

    -- Per-axis stepping gives a crude slide along blocked cells.
    local nx = x + vx * dt
    if not map:isWalkable(Iso.toCell(nx, y)) then nx = x; vx = 0 end
    local ny = y + vy * dt
    if not map:isWalkable(Iso.toCell(nx, ny)) then ny = y; vy = 0 end
    return nx, ny, vx, vy
end

function WorldMoveSystem:process(e, dt)
    local map = state.worldMap
    if not map then return end

    local input = e.input
    local ix = (input.right and 1 or 0) - (input.left and 1 or 0)
    local iy = (input.down and 1 or 0) - (input.up and 1 or 0)

    local mov = e.movement
    local speed = mov.speed or 120
    local nx, ny, vx, vy = WorldMoveSystem.step(map, e.position.x, e.position.y, ix, iy, speed, dt)
    e.position.x, e.position.y = nx, ny

    mov.velocityX, mov.velocityY = vx, vy
    local v = math.sqrt(vx * vx + vy * vy)
    if v > 1 then
        mov.facingX, mov.facingY = vx / v, vy / v
    end
    mov.speedRatio = v / speed
end

return WorldMoveSystem
```

Note: `Iso.toCell` returns two values used as two args to `isWalkable` — that is valid Lua (both propagate). Keep it exactly as written.

- [ ] **Step 6.4: Fix the stale comment in InputSystem**

`Client/src/systems/InputSystem.lua` line 5: change
`-- component. MovementSystem reads these booleans and does its own normalization,`
to
`-- component. WorldMoveSystem reads these booleans and does its own normalization,`

- [ ] **Step 6.5: Run tests to verify they pass**

Run: `ThirdParty\love2d\lovec.exe Client\src\tests\world_harness`
Expected: `ALL WORLD HARNESS CHECKS PASSED`. (The `state.worldMap` global is only touched in `process`, which the harness never calls.)

- [ ] **Step 6.6: Commit**

```bash
git add Client/src/systems/WorldMoveSystem.lua Client/src/systems/InputSystem.lua Client/src/tests/world_harness/main.lua
git commit -m "feat(client): interim WorldMoveSystem — WASD vs cell flags, no Box2D"
```

---

### Task 7: Cutover — Game.lua + RenderSystem on the new map path

**Files:**
- Modify: `Client/src/game/Game.lua` (loadGameWorld, fixedUpdate, doLogout, requires)
- Modify: `Client/src/systems/RenderSystem.lua` (world drawing rewritten)
- Modify: `Client/src/tests/world_harness/main.lua` (parse list additions)

This is the big swap. No new unit-testable logic — correctness is held by the harness parse-checks, the existing render/assets harnesses, and manual UAT (run the game).

- [ ] **Step 7.1: Add cutover files to the world_harness parse list**

Append to the `touched` list in `world_harness/main.lua`:

```lua
        "game/Game.lua",
        "systems/RenderSystem.lua",
        "systems/WorldMoveSystem.lua",
        "systems/InputSystem.lua",
        "systems/CameraSystem.lua",
```

(Keep prior entries. Duplicates with assets_harness are fine — both gates run.)

- [ ] **Step 7.2: Rewrite Game.lua world lifecycle**

In `Client/src/game/Game.lua`:

**(a)** Delete line 28 (`local sti  = require "thirdparty.sti"`).

**(b)** Delete the whole `Game:fixedUpdate` method (lines 119-144) and its banner comment — the interim mover integrates in the variable-rate `update`; the fixed-step hook returns with physics M1 in P2. (`Application` duck-types its hooks, so removing the method is safe.)

**(c)** Replace `Game:loadGameWorld` (lines 169-315) with:

```lua
-- ============================================================================
-- World load. Builds the tiny.world() from the new iso map substrate
-- (world.MapLoader / world.Map / world.WorldRenderer — P1 of the overworld
-- spec). No Box2D on this path: the interim WorldMoveSystem walks cell flags
-- until physics M1 + MovementController land in P2.
-- ============================================================================

function Game:loadGameWorld()
    state.gameState = GAME_STATE.LOADING
    self.gameState  = GAME_STATE.LOADING

    if state.world then
        state.world:clearSystems()
        state.world:clearEntities()
        state.world:refresh()
    end

    state.world = tiny.world()
    self.world  = state.world

    local MapLoader     = require "world.MapLoader"
    local WorldRenderer = require "world.WorldRenderer"
    local Iso           = require "world.IsoProjection"

    local map = MapLoader.load("devtest")
    state.worldMap      = map
    state.worldRenderer = WorldRenderer.new(map)

    -- Hover-cell highlight overlay: proves toCell picking against the live
    -- camera; the P2 click-to-move marker and Combat Sphere placement cells
    -- reuse this exact hook.
    state.worldRenderer:addOverlay(function(m, camera)
        local Settings = require "services.Settings"
        local mx, my = love.mouse.getPosition()
        local s = Settings.renderScale()
        local wx, wy = camera:screenToWorld(mx * s, my * s)
        local cx, cy = Iso.toCell(wx, wy)
        if m:inBounds(cx, cy) then
            local px, py = Iso.toScreen(cx, cy)
            love.graphics.setColor(1, 1, 1, 0.25)
            love.graphics.polygon("line",
                px + Iso.TILE_W / 2, py,
                px + Iso.TILE_W,     py + Iso.TILE_H / 2,
                px + Iso.TILE_W / 2, py + Iso.TILE_H,
                px,                  py + Iso.TILE_H / 2)
        end
    end)

    -- Spawn + props from map entities
    local spawnX, spawnY = Iso.cellCenter(2, 2)   -- fallback if no spawn entity
    local props = {}
    for _, ent in ipairs(map.entities) do
        if ent.type == "player_spawn" and ent.cell then
            spawnX, spawnY = Iso.cellCenter(ent.cell[1], ent.cell[2])
        elseif ent.type == "prop" and ent.pos then
            -- Fractional cell coords -> world px. Colliders are declared in
            -- the schema but ignored until physics M1 registers them as
            -- off-grid statics (companion physics spec).
            local px, py = Iso.cellCenter(ent.pos[1], ent.pos[2])
            props[#props + 1] = {
                position = { x = px, y = py },
                drawable = { color = { 0.55, 0.42, 0.25, 1 }, width = 20, height = 28 },
            }
        end
    end

    -- Build spritesheet animation data
    local function makeAnim(image, fw, fh, count, duration)
        local quads = {}
        local iw, ih = image:getDimensions()
        for i = 0, count - 1 do
            quads[i + 1] = love.graphics.newQuad(i * fw, 0, fw, fh, iw, ih)
        end
        return { image = image, quads = quads, frameWidth = fw, frameHeight = fh,
                 frameCount = count, frameDuration = duration, currentFrame = 1, frameTimer = 0 }
    end
    local Assets = require "services.Assets"
    local imgMove = Assets.image("player_move")
    local imgIdle = Assets.image("player_idle")

    local player = {
        player_tag = true,
        position   = { x = spawnX, y = spawnY },
        input      = { left = false, right = false, up = false, down = false },
        movement   = {
            speed      = 120,
            velocityX  = 0,
            velocityY  = 0,
            facingX    = 1,
            facingY    = 0,
            speedRatio = 0,
        },
        sprite_animation = {
            animations       = {
                move = makeAnim(imgMove, 32, 32, 8, 0.1),
                idle = makeAnim(imgIdle, 32, 32, 2, 0.07),
            },
            currentAnimation = "idle",
            flipX            = false,
            scale            = 1,
        },
    }
    state.player = player

    -- ECS systems (open-world exploration; turn-based combat lives in its own screens)
    local InputSystem     = require "systems.InputSystem"
    local WorldMoveSystem = require "systems.WorldMoveSystem"
    local AnimationSystem = require "systems.AnimationSystem"
    local CameraSystem    = require "systems.CameraSystem"
    local RenderSystem    = require "systems.RenderSystem"

    state.world:add(
        player,
        InputSystem,
        WorldMoveSystem,
        AnimationSystem,
        CameraSystem,
        RenderSystem
    )
    for _, p in ipairs(props) do state.world:add(p) end

    -- Quest service (global; WaveGame and DailiesScreen both read it)
    local QuestService = require "services.QuestService"
    state.quests = QuestService.new()
    state.quests:load()

    state.gameLoaded = true
    state.gameState  = GAME_STATE.PLAYING
    self.gameLoaded  = true
    self.gameState   = GAME_STATE.PLAYING

    self.UI.clear()

    if self.bus then self.bus:publish("game.world_loaded", {}) end
end
```

**(d)** In `Game:doLogout`, replace the physics-world teardown block

```lua
        if state.physicsWorld then
            state.physicsWorld:destroy()
            state.physicsWorld = nil
        end
```

with

```lua
        state.worldMap      = nil
        state.worldRenderer = nil
```

**(e)** Update the file-top comment block that mentions "sti map + physics + walls" (line ~165 banner is replaced by (c); also scan the header comment lines 1-18 — no sti mention there, leave).

- [ ] **Step 7.3: Rewrite RenderSystem world drawing**

`Client/src/systems/RenderSystem.lua` — surgical removals + one insertion. The file keeps: module-level `postProcessSettings`, `getVoidShader`, the filter, `setPostProcessEnabled`/`getPostProcessSettings`, `getVoidBlendFactor`, `drawEntity` (simplified), and in `:draw()` the canvas setup, void background, camera attach, particle/flash, sceneRT publish, chromatic decay.

**(a)** Delete these module-level blocks:
- `mapFadeShader` / `mapFadeShaderError` locals (lines 18-20) and `getMapFadeShader` (77-81)
- `mapCanvas` local (22-23) and `getMapCanvas` (83-89)
- the off-screen-indicator constants block (lines 63-69: `portalIndicatorPos` through `INDICATOR_HALF_HEIGHT`)
- `drawBaseLayers` (lines 91-134)
- `frontLayerDebugLogged` / `lastMapForDebug` (lines 136-138)
- `HUD_MARGIN_BASE` / `HUD_ELEMENT_PADDING` (lines 160-162)

**(b)** Replace `entityDrawPos` (lines 163-182) and the first lines of `drawEntity` with a direct read — no physics bodies exist on this path anymore:

```lua
function RenderSystem.drawEntity(e)
    local px, py = e.position.x, e.position.y
```

(Delete the `entityDrawPos` function and its comment block; the rest of `drawEntity`'s body is unchanged.)

**(c)** Replace the body of `RenderSystem:draw()` from the void-shader step through the portal-indicator block (lines 254-737, i.e. everything between the `ppCanvas` clear and the `state.screenFlash` block) with:

```lua
    local map = state.worldMap
    local wr  = state.worldRenderer

    -- Step 1: ambience background shader (map-driven; defaults to void)
    local bgKey = (map and map.ambience and map.ambience.background) or "void_background"
    local vShader = Assets.shader(bgKey)
    if vShader and map then
        local _, _, mapW, mapH = map:pixelBounds()
        love.graphics.setShader(vShader)
        if vShader:hasUniform("time") then vShader:send("time", love.timer.getTime()) end
        if vShader:hasUniform("camera_pos") then vShader:send("camera_pos", camUniforms.camera_pos) end
        if vShader:hasUniform("camera_scale") then vShader:send("camera_scale", camUniforms.camera_scale) end
        if vShader:hasUniform("screen_size") then vShader:send("screen_size", camUniforms.screen_size) end
        if vShader:hasUniform("map_size") then vShader:send("map_size", { mapW, mapH }) end
        if vShader:hasUniform("blend_factor") then vShader:send("blend_factor", getVoidBlendFactor()) end
        if vShader:hasUniform("fade_width") then vShader:send("fade_width", 80.0) end
        if vShader:hasUniform("pixel_size") then vShader:send("pixel_size", WORLD_PIXEL_SIZE) end
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.rectangle("fill", 0, 0, renderW, renderH)
        love.graphics.setShader()
    end

    -- Step 2: world (tiles + y-sorted entities + overlays) inside the camera
    state.camera:attach()
    if wr then
        wr:draw(state.camera, renderW, renderH, self.entities, RenderSystem.drawEntity)
    else
        for _, e in ipairs(self.entities) do RenderSystem.drawEntity(e) end
    end
    state.camera:detach()

    -- Draw particle effects (with pixelation)
    ParticleSystem:draw()

    -- Anchored draw-clock delta for the chromatic-pulse decay below (the
    -- compositor can invoke this producer twice per frame; wall-clock
    -- anchoring keeps the second invocation's dt at ~0).
    local _drawNow = love.timer.getTime()
    local dt = math.min(_drawNow - (RenderSystem._drawClock or _drawNow), 0.1)
    RenderSystem._drawClock = _drawNow
```

Everything after (the `state.screenFlash` block, the sceneRT/sceneSettings/sceneScaled publish, and the chromatic-pulse decay) stays byte-identical. Note `getVoidShader`/`voidShader` locals become unused once `Assets.shader(bgKey)` is called directly — delete `voidShader`/`voidShaderError`/`getVoidShader` too (Assets memoizes shader loads itself).

Also delete the now-stale `theme` require at the top (line 2) **only if** nothing else in the file references `theme` after the portal block is gone — verify with a search in the file before removing. Same check for `GameMode` (still used by `getVoidBlendFactor` — keep).

- [ ] **Step 7.4: Parse + harness gate**

Run all four:
```
ThirdParty\love2d\lovec.exe Client\src\tests\world_harness
ThirdParty\love2d\lovec.exe Client\src\tests\render_harness
ThirdParty\love2d\lovec.exe Client\src\tests\assets_harness
ThirdParty\love2d\lovec.exe Client\src\tests\threading_harness
```
Expected: all pass. (assets_harness still lists `systems/PhysicsSystem.lua` — that file still exists until Task 8, so it passes here.)

- [ ] **Step 7.5: Manual smoke (best-effort)**

If the Auth/Account services are up: `Client\run.bat`, log in, click to play. Expected: iso diamond checkerboard with raised border walls, TALL block, LOW strip; player spawns at cell (5,9) and walks with WASD, blocked by walls; hover highlights the cell under the mouse; props y-sort against the player. If services are down, defer to user UAT and say so in the summary.

- [ ] **Step 7.6: Commit**

```bash
git add Client/src/game/Game.lua Client/src/systems/RenderSystem.lua Client/src/tests/world_harness/main.lua
git commit -m "feat(client): overworld cutover to iso map substrate (Map/WorldRenderer scene path)"
```

---

### Task 8: Removal sweep

**Files:**
- Delete: `Client/src/systems/PhysicsSystem.lua`, `Client/src/systems/MovementSystem.lua`, `Client/src/systems/InterpolationSystem.lua`
- Delete: `Client/data/map/lobby.lua` (and the now-empty `Client/data/map/`)
- Delete: `ThirdParty/sti/` (entire directory)
- Modify: `Client/src/tests/assets_harness/main.lua:52`, `Client/conf.lua`, `Client/src/systems/Camera.lua:132`, `.gitignore:73-74`, `Server/Combat/src/CombatServer.hpp:13,18,113`, `CLAUDE.md`

- [ ] **Step 8.1: Delete the dead files**

```bash
git rm Client/src/systems/PhysicsSystem.lua Client/src/systems/MovementSystem.lua Client/src/systems/InterpolationSystem.lua
git rm Client/data/map/lobby.lua
git rm -r ThirdParty/sti
```

- [ ] **Step 8.2: assets_harness parse list**

In `Client/src/tests/assets_harness/main.lua`, replace line 52 (`"systems/PhysicsSystem.lua",`) with:

```lua
    "world/IsoProjection.lua", "world/Map.lua",
    "world/MapLoader.lua", "world/WorldRenderer.lua",
    "systems/WorldMoveSystem.lua",
```

- [ ] **Step 8.3: conf.lua — physics module off, comment updated**

In `Client/conf.lua`:
- In the main `love.conf(t)` body (after `t.window.msaa = 0` block) add:

```lua
    t.modules.physics = false   -- Box2D is off the build: the overworld walks
                                -- cell flags (P1) and the hand-rolled physics
                                -- engine arrives in P2 (2026-06-10 specs).
```

- In the header comment, replace the two mentions of sti (`ThirdParty/sti/init.lua, etc.)` and `(catches sti's internal "sti.utils" -> sti/utils.lua)` and `e.g. sti/init.lua`) with non-sti examples, e.g. `ThirdParty/tiny/tiny.lua` and `(catches multi-file packages' internal "lib.sub" -> lib/sub.lua)`. Keep the loader logic itself — `tiny`, `json`, `fun`, `inspect`, `strict` still resolve through it.

- [ ] **Step 8.4: Comment fixes**

- `Client/src/systems/Camera.lua:132`: change `(void / map fade / portal distort)` to `(void / ambience backgrounds)`.
- `Client/src/systems/render/Pipeline.lua:368-369`: the `Pipeline:draw` comment says "RenderSystem.drawEntity can interpolate Box2D body positions" — change to "alpha is threaded through FrameCtx for draw-time interpolation (consumer returns with physics M1 in P2)".

- [ ] **Step 8.5: .gitignore**

Delete lines 73-74:

```
# Arena exports written at build time by lovec.exe --export-arenas.
**/data/arenas/
```

- [ ] **Step 8.6: Server comment cleanup (CombatServer.hpp)**

In `Server/Combat/src/CombatServer.hpp`:
- Line 13: `// Combat: Arena loading, match session management, pre/post combat handling.` → `// Combat: match session management, pre/post combat handling.`
- Line 18: `//   - Arena data loading from data/arenas/` → `//   - Map-region data loading (consumes the client map JSON; Combat Sphere phase)`
- Line 113: `LOG_SERVER_WARN("Combat is a stub — match/arena systems not yet implemented");` → `LOG_SERVER_WARN("Combat is a stub — match systems not yet implemented");`

(Comment-only changes; no server rebuild needed for correctness, but the file must stay valid C++ — don't touch anything else.)

- [ ] **Step 8.7: CLAUDE.md**

- In **Combat internals**: replace `- **Arena data**: Exported from the Love2D client at build time via a `lovec.exe --export-arenas` pre-build command. Arena JSON lands in `data/arenas/`.` with `- **Arena data**: removed 2026-06-10 (the legacy --export-arenas pipeline is gone). Combat is a stub; it will consume client map JSON regions in the Combat Sphere phase.`
- Search CLAUDE.md for other `sti`/`Tiled`/`arenas` mentions and update any found.

- [ ] **Step 8.8: Repo-wide residue grep (the proof)**

```bash
grep -rn --include=*.lua -e "thirdparty.sti" -e "arenaPortal" -e "love.physics" -e "physicsWorld" -e "InterpolationSystem" -e "data/map/" -e "systems.MovementSystem" -e "systems.PhysicsSystem" Client/src Client/main.lua Client/conf.lua
```

Expected: **zero hits** (the conf.lua `t.modules.physics = false` line doesn't match any pattern; if a hit appears, fix it and re-run).

Also confirm the old map data and STI are gone: `ls Client/data/map` → no such directory; `ls ThirdParty/sti` → no such directory.

- [ ] **Step 8.9: Full harness gate**

Run all four harnesses again (same commands as Step 7.4). Expected: all pass — assets_harness now parse-checks the world files instead of the deleted PhysicsSystem.

- [ ] **Step 8.10: Commit**

```bash
git add -A
git commit -m "refactor(client)!: remove STI/Tiled, Box2D path, portal indicator, InterpolationSystem, arena-export residue"
```

---

### Task 9: Final verification + docs

- [ ] **Step 9.1: Run the complete gate one last time**

All four harnesses (Step 7.4 commands) + the residue grep (Step 8.8). Expected: green across the board.

- [ ] **Step 9.2: Check acceptance criteria against the result**

- #1 (iso map renders through canonical pipeline, projection-identical to combat): combat/Grid delegates to IsoProjection — identical by construction; harness round-trips prove the math. Manual UAT confirms visuals.
- #4 (STI, Tiled data, Box2D (new path), portal code, --export-arenas gone): Step 8.8 grep is the proof.
- #5 (harness coverage): world_harness covers IsoProjection round-trips + Map flag accessors (+ loader/mover/renderer math). Pathfinder topology tests arrive with the P2 Pathfinder.
- #2/#3 are P2 (movement modes + pathfinding).

- [ ] **Step 9.3: Update CLAUDE.md repository-layout/client notes if stale** (mention `Client/src/world/` substrate + `data/maps/` in the client layout table if one exists; skip if nothing fits naturally).

- [ ] **Step 9.4: Final commit (if 9.3 changed anything)**

```bash
git add CLAUDE.md
git commit -m "docs: note iso map substrate in CLAUDE.md"
```
