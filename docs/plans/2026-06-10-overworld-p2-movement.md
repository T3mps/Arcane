# Overworld P2 — Movement Implementation Plan (P2 part 2 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **PREREQUISITE: `2026-06-10-physics-m1.md` must be fully executed first.**

**Goal:** Dual movement modes (WASD / Wakfu-style click-to-move) behind a runtime-switchable gameplay setting, driven by the physics M1 CharacterController; A* pathfinding with switchable 4/8 topology; path preview + click-marker overlays; the interim `WorldMoveSystem`/`InputSystem` pair retired. Closes overworld-spec acceptance #2 and #3.

**Architecture:** `MovementController` is the single owner (spec §Movement): polls the `movementMode` setting each tick, swaps the base input context (`world_wasd` ⇄ `world_click`), and drives the player's `CharacterController` from `Game:fixedUpdate` (reinstated — physics steps at 60 UPS again). Both modes emit the same intent shape into the `movement` component, so AnimationSystem stays mode-blind. `Pathfinder` is A* over `Map` flags with topology strategy objects and preallocated heap/arrays (zero steady-state allocation). Render boundary lerps body prev→pos via the existing `FrameCtx.alpha`, with the deferred camera-interpolation fix (2026-06-10 review M8) landing alongside.

**Tech Stack:** physics M1 (`PhysicsWorld`/`CharacterController`), `services/Input.lua` context stack, `services/Settings.lua`, WorldRenderer overlay hooks, world_harness.

**Spec:** `docs/superpowers/specs/2026-06-10-overworld-iso-map-movement-design.md` §Movement, §Pathfinding, P2 phasing, acceptance #2/#3.

**Input-map decisions (verified against `services/Input.lua` 2026-06-10):**
- The existing `gameplay` map IS the WASD world context in everything but name → **rename it `world_wasd`**, dropping the dead real-time-combat actions `attack` and `dodge` (no `Input.down/pressed` consumers — only their curated REBINDS rows, which are removed with them). `move` and `interact` stay.
- New map `world_click`: `move_to` (Button, `<Mouse>/rightButton`). Hover needs no action (pointer polled directly).
- `Input.setBaseContext` CLEARS the context stack — unusable for mode switches made from the settings screen (menu context is pushed). Add **`Input.swapBaseContext(mapName)`** which replaces `contextStack[1]` in place.
- `Input.load`'s base-context default `"gameplay"` becomes `"world_wasd"`; `MovementController` re-asserts the correct base per the setting on world load and on every mode change.
- REBINDS: `map = "gameplay"` entries move to `map = "world_wasd"`; add a `move_to` rebind row ("Move To", section "Movement", map `world_click`). Persisted `keybinds.json` entries for removed attack/dodge ids are simply ignored by `rebindDef` (verified: unknown ids are skipped on load).

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `Client/src/world/Pathfinder.lua` | Create | A* over cell flags; Topology4/Topology8; preallocated heap/arrays |
| `Client/src/world/PathFollower.lua` | Create | Cell-center-to-cell-center kinematic advance; re-path support |
| `Client/src/world/MovementController.lua` | Create | Mode owner: setting poll, context swap, intent → CharacterController |
| `Client/src/world/MoveOverlays.lua` | Create | Path preview + click marker + unreachable flash (world overlay hooks) |
| `Client/src/services/Input.lua` | Modify | `swapBaseContext`; REBINDS table; base default |
| `Client/data/input_actions.json` | Modify | `gameplay`→`world_wasd` (minus attack/dodge); add `world_click` |
| `Client/src/services/Settings.lua` | Modify | `movementMode`, `pathTopology` defaults + options |
| `Client/src/ui/screens/settings/init.lua` | Modify | GAMEPLAY "Movement Mode" radio; DEBUG "Path Topology" radio |
| `Client/src/game/Game.lua` | Modify | PhysicsWorld + player body + prop statics on load; fixedUpdate reinstated |
| `Client/src/systems/RenderSystem.lua` | Modify | drawEntity body lerp; M8 camera draw-position correction |
| `Client/src/world/ColliderDebug.lua` | Modify | Player collider drawn from the live capsule body |
| `Client/src/systems/WorldMoveSystem.lua` | Delete | Replaced by MovementController + CharacterController |
| `Client/src/systems/InputSystem.lua` | Delete | MovementController reads `Input.axis("move")` directly |
| `Client/src/tests/world_harness/main.lua` | Modify | New sections; retire WMS/InputSystem entries |

---

### Task 1: Settings + input maps + `Input.swapBaseContext`

**Files:** Modify `Settings.lua`, `input_actions.json`, `Input.lua`, world_harness (parse gate only — Input/Settings behavior is exercised in later tasks and by boot smoke).

- [ ] **Step 1.1** `Settings.lua` DEFAULTS — add after the debug-draw block:

```lua
    -- Movement (overworld P2)
    movementMode = "wasd",   -- "wasd" | "click" — exclusive, runtime-switchable
    pathTopology = "8",      -- "4" | "8" — pathfinder neighbor topology (dev knob)
```

and two option helpers next to the other `*Options()` functions:

```lua
--- Movement-mode options ({label,value}) for the gameplay selector.
function Settings.movementModeOptions()
    return {
        { label = "WASD",          value = "wasd"  },
        { label = "Click to Move", value = "click" },
    }
end

--- Pathfinder topology options (dev knob; spec wants both feel-testable).
function Settings.pathTopologyOptions()
    return {
        { label = "4-dir", value = "4" },
        { label = "8-dir", value = "8" },
    }
end
```

(No `apply()` branch needed — both are read live.)

- [ ] **Step 1.2** `input_actions.json`: in the `gameplay` map object: change `"name": "gameplay"` → `"name": "world_wasd"`, delete the entire `attack` and `dodge` action objects (keep `move`, `interact`). Append a new map object to `actionMaps`:

```json
    {
      "actions": [
        {
          "bindings": [
            {
              "groups": [
                "KeyboardMouse"
              ],
              "path": "<Mouse>/rightButton"
            }
          ],
          "name": "move_to",
          "type": "Button"
        }
      ],
      "blocking": false,
      "name": "world_click"
    }
```

- [ ] **Step 1.3** `Input.lua`:
- In `Input.load` (line ~471): `if maps.gameplay then contextStack[1] = "gameplay" end` → `if maps.world_wasd then contextStack[1] = "world_wasd" end`.
- Below `setBaseContext` add:

```lua
--- Replace the BOTTOM of the context stack without disturbing pushed
--- contexts (menus, combat). MovementController swaps the world base map on
--- mode change — which can happen from inside the settings screen, where
--- setBaseContext's stack reset would strand the menu context.
function Input.swapBaseContext(mapName)
    if not maps[mapName] then log.warn("swapBaseContext: unknown map '%s'", tostring(mapName)); return end
    contextStack[1] = mapName
end
```

- REBINDS table (line ~317): change the four movement part entries and `interact` from `map = "gameplay"` to `map = "world_wasd"`; delete the `attack` and `dodge` rows; add:

```lua
    { id = "move_to", section = "Movement", label = "Move To", map = "world_click", action = "move_to" },
```

(Check the actual REBINDS row shape at the file — movement rows may use `part = "up"` etc.; preserve their existing fields, change only `map`.)

- [ ] **Step 1.4** Run all harnesses + boot smoke (`lovec Client\.` for 6s — Input must log `Loaded 6 action map(s)` and no warnings). **Step 1.5** Commit: `feat(client): world_wasd/world_click input contexts, movement-mode + topology settings`

---

### Task 2: Pathfinder (A*, topology strategies, zero-alloc)

**Files:** Create `Client/src/world/Pathfinder.lua`. world_harness section + parse entry.

- [ ] **Step 2.1** Failing tests:

```lua
print("== Pathfinder ==")
do
    local Map = require "world.Map"
    local PF  = require "world.Pathfinder"

    -- 9x7: ring wall; vertical wall at cx=5 with a gap at cy=5
    local W, H = 9, 7
    local flags = {}
    for cy = 1, H do for cx = 1, W do
        local border = cx == 1 or cx == W or cy == 1 or cy == H
        local wall = cx == 5 and cy ~= 5
        flags[(cy-1)*W+cx] = (border or wall) and 4 or 1
    end end
    local m = Map.new{ id="pf", name="p", w=W, h=H, tileset="n",
        layers={{name="g",tiles=flags}}, flags=flags, entities={}, ambience={} }

    local pf4 = PF.new(m, PF.Topology4)
    local path = {}
    local n = pf4:find(2, 2, 8, 2, path)
    eq(n > 0, true, "4-dir path found")
    -- path is {cx,cy} pairs, excludes start, includes goal
    eq(path[n * 2 - 1], 8, "goal cx"); eq(path[n * 2], 2, "goal cy")
    -- must route through the gap (5,5)
    local throughGap = false
    for i = 1, n * 2, 2 do
        if path[i] == 5 and path[i + 1] == 5 then throughGap = true end
    end
    eq(throughGap, true, "routes through the wall gap")
    -- every step cell walkable + 4-adjacent to its predecessor
    local px, py = 2, 2
    for i = 1, n * 2, 2 do
        local cx, cy = path[i], path[i + 1]
        eq(m:isWalkable(cx, cy), true, "step walkable")
        eq(math.abs(cx - px) + math.abs(cy - py), 1, "4-adjacent steps")
        px, py = cx, cy
    end

    -- 8-dir is never longer than 4-dir
    local pf8 = PF.new(m, PF.Topology8)
    local path8 = {}
    local n8 = pf8:find(2, 2, 8, 2, path8)
    eq(n8 > 0 and n8 <= n, true, "8-dir path no longer than 4-dir")

    -- no corner cutting: a diagonal step requires both orthogonals passable.
    -- 4x4 fixture: walls at (3,2) and (2,3) — the diagonal (2,2)->(3,3) is
    -- geometrically open but must be refused.
    local f2 = {}
    for cy = 1, 4 do for cx = 1, 4 do f2[(cy-1)*4+cx] = 1 end end
    f2[(2-1)*4+3] = 4; f2[(3-1)*4+2] = 4
    local m2 = Map.new{ id="cc2", name="c", w=4, h=4, tileset="n",
        layers={{name="g",tiles=f2}}, flags=f2, entities={}, ambience={} }
    local pfc = PF.new(m2, PF.Topology8)
    local pc = {}
    local nc = pfc:find(2, 2, 3, 3, pc)
    eq(nc, 0, "corner-cut diagonal refused (no alternative route exists)")

    -- unreachable: walled-off goal returns 0
    eq(pf4:find(2, 2, 5, 2, path), 0, "wall cell unreachable")

    -- start == goal early-out
    eq(pf4:find(2, 2, 2, 2, path), 0, "start==goal -> empty path")

    -- zero steady-state allocation after warmup
    pf8:find(2, 2, 8, 6, path8)
    collectgarbage("collect")
    local before = collectgarbage("count")
    for _ = 1, 100 do pf8:find(2, 2, 8, 6, path8) end
    collectgarbage("collect")
    eq(collectgarbage("count") - before < 8, true, "no per-query garbage after warmup")
end
```

- [ ] **Step 2.2** Run → FAIL. **Step 2.3** Implement:

```lua
-- world/Pathfinder.lua
-- A* over Map cell flags (spec §Pathfinding). Reads Map.isWalkable ONLY —
-- the same truth physics derives from, so click mode and WASD can never
-- disagree about passability.
--
-- Topology is a strategy object (the requested clean switch point):
-- Topology4 / Topology8, selected via Settings "pathTopology" by the caller.
-- Topology8 enforces no-corner-cutting (a diagonal needs both orthogonals).
--
-- Zero steady-state allocation: binary-heap open list + flat g/cameFrom/
-- stamp arrays sized w*h at construction, reused per query (generation
-- stamps avoid clearing). Output path is written into a caller array as
-- flat {cx,cy} pairs, excluding start, including goal; returns step count.
local Pathfinder = {}
Pathfinder.__index = Pathfinder

local SQRT2 = 1.4142135623730951

Pathfinder.Topology4 = {
    dirs = { 1,0, -1,0, 0,1, 0,-1 },
    cost = function(dx, dy) return 1 end,
    heuristic = function(ax, ay, bx, by)
        return math.abs(ax - bx) + math.abs(ay - by)   -- Manhattan
    end,
    allowed = function(map, cx, cy, dx, dy) return true end,
}

Pathfinder.Topology8 = {
    dirs = { 1,0, -1,0, 0,1, 0,-1, 1,1, 1,-1, -1,1, -1,-1 },
    cost = function(dx, dy) return (dx ~= 0 and dy ~= 0) and SQRT2 or 1 end,
    heuristic = function(ax, ay, bx, by)               -- octile
        local dx, dy = math.abs(ax - bx), math.abs(ay - by)
        return (dx + dy) + (SQRT2 - 2) * math.min(dx, dy)
    end,
    allowed = function(map, cx, cy, dx, dy)            -- no corner cutting
        if dx ~= 0 and dy ~= 0 then
            return map:isWalkable(cx + dx, cy) and map:isWalkable(cx, cy + dy)
        end
        return true
    end,
}

function Pathfinder.new(map, topology)
    local n = map.w * map.h
    local self = setmetatable({
        map = map, topo = topology or Pathfinder.Topology8,
        g        = {},   -- best cost per cell index
        cameFrom = {},   -- predecessor cell index
        stamp    = {},   -- query generation per cell index
        inOpen   = {},   -- heap membership stamp
        heapIdx  = {},   -- binary heap of cell indices
        heapKey  = {},   -- parallel f-scores
        heapN    = 0,
        query    = 0,
    }, Pathfinder)
    for i = 1, n do self.g[i] = 0; self.cameFrom[i] = 0; self.stamp[i] = 0; self.inOpen[i] = 0 end
    return self
end

function Pathfinder:setTopology(t) self.topo = t end

-- binary min-heap on (heapIdx, heapKey)
local function heapPush(self, idx, key)
    local n = self.heapN + 1
    self.heapN = n
    self.heapIdx[n], self.heapKey[n] = idx, key
    while n > 1 do
        local p = math.floor(n / 2)
        if self.heapKey[p] <= self.heapKey[n] then break end
        self.heapIdx[p], self.heapIdx[n] = self.heapIdx[n], self.heapIdx[p]
        self.heapKey[p], self.heapKey[n] = self.heapKey[n], self.heapKey[p]
        n = p
    end
end

local function heapPop(self)
    local top = self.heapIdx[1]
    local n = self.heapN
    self.heapIdx[1], self.heapKey[1] = self.heapIdx[n], self.heapKey[n]
    self.heapN = n - 1
    n = self.heapN
    local i = 1
    while true do
        local l, r = i * 2, i * 2 + 1
        local s = i
        if l <= n and self.heapKey[l] < self.heapKey[s] then s = l end
        if r <= n and self.heapKey[r] < self.heapKey[s] then s = r end
        if s == i then break end
        self.heapIdx[s], self.heapIdx[i] = self.heapIdx[i], self.heapIdx[s]
        self.heapKey[s], self.heapKey[i] = self.heapKey[i], self.heapKey[s]
        i = s
    end
    return top
end

--- Find a path (sx,sy)->(ex,ey). Writes flat {cx,cy} pairs into outPath
--- (excluding start, including goal). Returns the step COUNT (0 = no path
--- or start==goal). Bounded expansion: at most w*h pops.
function Pathfinder:find(sx, sy, ex, ey, outPath)
    local map, topo = self.map, self.topo
    if sx == ex and sy == ey then return 0 end
    if not map:inBounds(ex, ey) or not map:isWalkable(ex, ey) then return 0 end

    local w = map.w
    local function idxOf(cx, cy) return (cy - 1) * w + cx end

    self.query = self.query + 1
    local q = self.query
    self.heapN = 0

    local si = idxOf(sx, sy)
    self.g[si] = 0; self.stamp[si] = q; self.cameFrom[si] = 0
    heapPush(self, si, topo.heuristic(sx, sy, ex, ey))
    self.inOpen[si] = q

    local goal = idxOf(ex, ey)
    local found = false
    local maxPops = map.w * map.h
    for _ = 1, maxPops do
        if self.heapN == 0 then break end
        local cur = heapPop(self)
        if cur == goal then found = true; break end
        local cx = (cur - 1) % w + 1
        local cy = math.floor((cur - 1) / w) + 1
        local dirs = topo.dirs
        for d = 1, #dirs, 2 do
            local dx, dy = dirs[d], dirs[d + 1]
            local nx, ny = cx + dx, cy + dy
            if map:isWalkable(nx, ny) and topo.allowed(map, cx, cy, dx, dy) then
                local ni = idxOf(nx, ny)
                local tentative = self.g[cur] + topo.cost(dx, dy)
                if self.stamp[ni] ~= q or tentative < self.g[ni] then
                    self.stamp[ni] = q
                    self.g[ni] = tentative
                    self.cameFrom[ni] = cur
                    heapPush(self, ni, tentative + topo.heuristic(nx, ny, ex, ey))
                end
            end
        end
    end
    if not found then return 0 end

    -- reconstruct backwards, then reverse in place into outPath
    local n = 0
    local cur = goal
    while cur ~= si do
        outPath[n * 2 + 1] = (cur - 1) % w + 1
        outPath[n * 2 + 2] = math.floor((cur - 1) / w) + 1
        n = n + 1
        cur = self.cameFrom[cur]
    end
    -- reverse pairs
    local i, j = 0, n - 1
    while i < j do
        outPath[i*2+1], outPath[j*2+1] = outPath[j*2+1], outPath[i*2+1]
        outPath[i*2+2], outPath[j*2+2] = outPath[j*2+2], outPath[i*2+2]
        i, j = i + 1, j - 1
    end
    return n
end

return Pathfinder
```

- [ ] **Step 2.4** Run green. **Step 2.5** Commit: `feat(world): A* Pathfinder — topology strategies, binary heap, zero steady-state alloc`

---

### Task 3: PathFollower

**Files:** Create `Client/src/world/PathFollower.lua`. world_harness section + parse entry.

- [ ] **Step 3.1** Failing tests:

```lua
print("== PathFollower ==")
do
    local Iso = require "world.IsoProjection"
    local PFol = require "world.PathFollower"

    local f = PFol.new(120)   -- speed px/s
    eq(f:active(), false, "idle initially")

    -- straight 3-cell path; fake "body" = table with position
    local pos = { x = 0, y = 0 }
    pos.x, pos.y = Iso.cellCenter(2, 2)
    f:setPath({ 3,2, 4,2, 5,2 }, 3)
    eq(f:active(), true, "active after setPath")
    eq(f:targetCell(), 3, "first target cx")   -- returns cx, cy

    -- advance until done; velocity always points at the current target
    local guard = 0
    while f:active() and guard < 600 do
        guard = guard + 1
        local vx, vy = f:desiredVelocity(pos.x, pos.y, 1 / 60)
        pos.x = pos.x + vx * (1 / 60)
        pos.y = pos.y + vy * (1 / 60)
    end
    eq(f:active(), false, "path completes")
    local gx, gy = Iso.cellCenter(5, 2)
    eq(math.abs(pos.x - gx) < 2 and math.abs(pos.y - gy) < 2, true, "arrived at goal cell center")

    -- re-path mid-walk replaces the route (Wakfu behavior)
    pos.x, pos.y = Iso.cellCenter(2, 2)
    f:setPath({ 3,2, 4,2 }, 2)
    f:desiredVelocity(pos.x, pos.y, 1 / 60)
    f:setPath({ 2,3, 2,4 }, 2)
    local tcx, tcy = f:targetCell()
    eq(tcx, 2, "re-path retargets cx"); eq(tcy, 3, "re-path retargets cy")

    -- cancel stops cleanly
    f:cancel()
    eq(f:active(), false, "cancel deactivates")
    local vx, vy = f:desiredVelocity(pos.x, pos.y, 1 / 60)
    approx(vx, 0, "idle vx"); approx(vy, 0, "idle vy")
end
```

- [ ] **Step 3.2** Run → FAIL. **Step 3.3** Implement:

```lua
-- world/PathFollower.lua
-- Advances cell-center to cell-center at move speed with continuous motion
-- (spec §Movement, click mode). Pure velocity math — the caller feeds the
-- result to CharacterController:followVelocity (kinematic body; physics
-- never deflects it: paths are cell-safe by construction).
local Iso = require "world.IsoProjection"

local PathFollower = {}
PathFollower.__index = PathFollower

local ARRIVE = 2   -- px: close enough to snap to the next waypoint

function PathFollower.new(speed)
    return setmetatable({
        speed = speed or 120,
        path  = {},    -- flat {cx,cy} pairs (reused storage)
        n     = 0,     -- step count
        i     = 0,     -- current target step (1-based), 0 = inactive
    }, PathFollower)
end

function PathFollower:active() return self.i > 0 and self.i <= self.n end

function PathFollower:targetCell()
    if not self:active() then return nil end
    return self.path[self.i * 2 - 1], self.path[self.i * 2]
end

--- Adopt a new route (flat pairs + count). Copies into owned storage so the
--- caller's scratch array can be reused. Re-pathing mid-walk just calls this
--- again (Wakfu behavior: new click re-paths from the current position).
function PathFollower:setPath(pairsFlat, count)
    for k = 1, count * 2 do self.path[k] = pairsFlat[k] end
    self.n = count
    self.i = count > 0 and 1 or 0
end

function PathFollower:cancel() self.i = 0; self.n = 0 end

--- Velocity toward the current waypoint; advances waypoints on arrival.
--- Returns vx, vy (0,0 when inactive/finished).
function PathFollower:desiredVelocity(x, y, dt)
    while self:active() do
        local tcx, tcy = self:targetCell()
        local tx, ty = Iso.cellCenter(tcx, tcy)
        local dx, dy = tx - x, ty - y
        local dist = math.sqrt(dx * dx + dy * dy)
        if dist > math.max(ARRIVE, self.speed * dt) then
            return dx / dist * self.speed, dy / dist * self.speed
        end
        self.i = self.i + 1   -- reached this waypoint; take the next
    end
    return 0, 0
end

return PathFollower
```

- [ ] **Step 3.4** Run green. **Step 3.5** Commit: `feat(world): PathFollower — continuous cell-to-cell advance with re-path`

---

### Task 4: MovementController (the single owner)

**Files:** Create `Client/src/world/MovementController.lua`. world_harness parse entry (+ mode-logic unit test below).

The controller has two ticks: `update(dt)` (variable rate — click/hover edge capture, since `Input.pressed` edges are per-render-frame and a frame can run zero fixed steps) and `fixedUpdate(fixedDt)` (motion). Pure mode-resolution logic is extracted as `MovementController.resolveMode` for the harness.

- [ ] **Step 4.1** Failing test:

```lua
print("== MovementController mode logic ==")
do
    local MC = require "world.MovementController"
    -- resolveMode(settingValue, currentMode) -> newMode, changed
    local mode, changed = MC.resolveMode("wasd", nil)
    eq(mode, "wasd", "initial wasd"); eq(changed, true, "initial counts as change")
    mode, changed = MC.resolveMode("wasd", "wasd")
    eq(changed, false, "no change")
    mode, changed = MC.resolveMode("click", "wasd")
    eq(mode, "click", "switches"); eq(changed, true, "switch flagged")
    mode = MC.resolveMode("garbage", "click")
    eq(mode, "wasd", "unknown value falls back to wasd")
end
```

- [ ] **Step 4.2** Run → FAIL. **Step 4.3** Implement:

```lua
-- world/MovementController.lua
-- THE single owner of overworld movement (spec §Movement). Owns the active
-- mode, swaps the base input context, and drives the player's
-- CharacterController each fixedUpdate. Both modes emit the same intent —
-- desired velocity + facing written to the movement component — so
-- AnimationSystem (and any future footstep/facing consumer) is mode-blind.
--
--   WASD : Input.axis("move") -> normalized velocity -> slideMove (collision
--          resolved vs cell statics; identical passability to click mode by
--          construction — both read Map.isWalkable truth).
--   click: right-click -> toCell -> Pathfinder -> PathFollower velocity ->
--          followVelocity (kinematic; physics integrates, never deflects).
--
-- Mode is the "movementMode" setting, polled every tick: runtime-switchable
-- from the settings screen with no listener plumbing. Switching cancels any
-- active path and swaps the base input context (world_wasd / world_click).
local Settings = require "services.Settings"
local Input    = require "services.Input"
local Iso      = require "world.IsoProjection"
local Pathfinder   = require "world.Pathfinder"
local PathFollower = require "world.PathFollower"

local MovementController = {}
MovementController.__index = MovementController

local SPEED = 120

--- Pure: sanitize the setting against the current mode.
function MovementController.resolveMode(settingValue, currentMode)
    local mode = (settingValue == "click") and "click" or "wasd"
    return mode, mode ~= currentMode
end

--- deps: { map, controller (physics CharacterController), player (entity) }
function MovementController.new(deps)
    local topo = Settings.get("pathTopology") == "4"
        and Pathfinder.Topology4 or Pathfinder.Topology8
    local self = setmetatable({
        map        = deps.map,
        controller = deps.controller,
        player     = deps.player,
        pathfinder = Pathfinder.new(deps.map, topo),
        follower   = PathFollower.new(SPEED),
        mode       = nil,
        _pathScratch = {},
        -- overlay-facing state (read by MoveOverlays)
        clickMarker  = nil,    -- { cx, cy, t }  (t counts down)
        unreachable  = nil,    -- { cx, cy, t }
        _pendingClick = nil,   -- { cx, cy } captured in update, consumed in fixedUpdate
    }, MovementController)
    self:_applyMode(MovementController.resolveMode(Settings.get("movementMode"), nil))
    return self
end

function MovementController:_applyMode(mode)
    self.mode = mode
    self.follower:cancel()
    self._pendingClick = nil
    Input.swapBaseContext(mode == "click" and "world_click" or "world_wasd")
    local body = self.controller.body
    body:setVelocity(0, 0)
end

--- Variable-rate tick: edge capture + marker timers. Call from Game:update.
function MovementController:update(dt)
    local mode, changed = MovementController.resolveMode(Settings.get("movementMode"), self.mode)
    if changed then self:_applyMode(mode) end

    if self.mode == "click" and Input.pressed("move_to") then
        local mx, my = love.mouse.getPosition()
        local s = Settings.renderScale()
        local wx, wy = state.camera:screenToWorld(mx * s, my * s)
        local cx, cy = Iso.toCell(wx, wy)
        self._pendingClick = { cx = cx, cy = cy }
    end

    if self.clickMarker then
        self.clickMarker.t = self.clickMarker.t - dt
        if self.clickMarker.t <= 0 then self.clickMarker = nil end
    end
    if self.unreachable then
        self.unreachable.t = self.unreachable.t - dt
        if self.unreachable.t <= 0 then self.unreachable = nil end
    end
end

function MovementController:_requestPath(cx, cy)
    local body = self.controller.body
    local px, py = body:getPosition()
    local scx, scy = Iso.toCell(px, py)
    if not self.map:isWalkable(cx, cy) then
        self.unreachable = { cx = cx, cy = cy, t = 0.6 }
        return
    end
    local n = self.pathfinder:find(scx, scy, cx, cy, self._pathScratch)
    if n == 0 and not (scx == cx and scy == cy) then
        self.unreachable = { cx = cx, cy = cy, t = 0.6 }
        return
    end
    self.follower:setPath(self._pathScratch, n)
    self.clickMarker = { cx = cx, cy = cy, t = 0.8 }
end

--- Fixed-step tick: drive motion. Call from Game:fixedUpdate BEFORE
--- physicsWorld:step (slideMove moves the body directly; follower velocity
--- is integrated by the step).
function MovementController:fixedUpdate(fixedDt)
    local mov  = self.player.movement
    local body = self.controller.body

    if self.mode == "wasd" then
        local ax, ay = Input.axis("move")
        local len = math.sqrt(ax * ax + ay * ay)
        if len > 1 then ax, ay = ax / len, ay / len end
        local dx, dy = self.controller:slideMove(ax * SPEED * fixedDt, ay * SPEED * fixedDt)
        local vx, vy = dx / fixedDt, dy / fixedDt
        mov.velocityX, mov.velocityY = vx, vy
        local v = math.sqrt(vx * vx + vy * vy)
        if v > 1 then mov.facingX, mov.facingY = vx / v, vy / v end
        mov.speedRatio = math.min(1, v / SPEED)
    else
        if self._pendingClick then
            self:_requestPath(self._pendingClick.cx, self._pendingClick.cy)
            self._pendingClick = nil
        end
        local px, py = body:getPosition()
        local vx, vy = self.follower:desiredVelocity(px, py, fixedDt)
        self.controller:followVelocity(vx, vy)
        mov.velocityX, mov.velocityY = vx, vy
        local v = math.sqrt(vx * vx + vy * vy)
        if v > 1 then mov.facingX, mov.facingY = vx / v, vy / v end
        mov.speedRatio = math.min(1, v / SPEED)
    end
end

--- Hover cell for the click-mode highlight (nil in WASD mode).
function MovementController:hoverCell()
    if self.mode ~= "click" then return nil end
    local mx, my = love.mouse.getPosition()
    local s = Settings.renderScale()
    local wx, wy = state.camera:screenToWorld(mx * s, my * s)
    local cx, cy = Iso.toCell(wx, wy)
    if self.map:inBounds(cx, cy) then return cx, cy end
    return nil
end

--- Topology dev knob (read live so the DEBUG setting applies immediately).
function MovementController:refreshTopology()
    self.pathfinder:setTopology(Settings.get("pathTopology") == "4"
        and Pathfinder.Topology4 or Pathfinder.Topology8)
end

return MovementController
```

(`refreshTopology` is called from `update()` — add `self:refreshTopology()` right after the mode poll; it's two compares, cheap.)

- [ ] **Step 4.4** Run green (only `resolveMode` is unit-tested; the rest is parse-gated + integration-verified in Task 6). **Step 4.5** Commit: `feat(world): MovementController — mode owner, context swap, intent emitter`

---

### Task 5: MoveOverlays (path preview, click marker, unreachable flash)

**Files:** Create `Client/src/world/MoveOverlays.lua`. world_harness parse entry.

- [ ] **Step 5.1** Implement (graphics-only; parse-gated):

```lua
-- world/MoveOverlays.lua
-- Click-mode gameplay overlays on WorldRenderer's overlay seam (NOT debug
-- draw — these are player-facing): remaining path preview, click marker
-- pulse, unreachable flash. The Combat Sphere phase's blue/red placement
-- cells will be this module's siblings on the same seam.
local Iso = require "world.IsoProjection"

local MoveOverlays = {}

local COL_PATH        = { 1.0, 1.0, 1.0, 0.18 }
local COL_MARKER      = { 0.4, 0.9, 1.0, 0.9 }
local COL_UNREACHABLE = { 1.0, 0.3, 0.3, 0.9 }

local function diamondOutline(cx, cy, inset)
    inset = inset or 0
    local px, py = Iso.toScreen(cx, cy)
    love.graphics.polygon("line",
        px + Iso.TILE_W / 2,         py + inset,
        px + Iso.TILE_W - inset * 2, py + Iso.TILE_H / 2,
        px + Iso.TILE_W / 2,         py + Iso.TILE_H - inset,
        px + inset * 2,              py + Iso.TILE_H / 2)
end

--- Returns the overlay fn to register: WorldRenderer:addOverlay(MoveOverlays.make(mc))
function MoveOverlays.make(mc)
    return function(map, camera)
        if mc.mode ~= "click" then return end
        local lg = love.graphics
        local prevWidth = lg.getLineWidth()
        lg.setLineWidth(1 / camera:renderZoom())

        -- remaining path
        local f = mc.follower
        if f:active() then
            lg.setColor(COL_PATH)
            for i = f.i, f.n do
                diamondOutline(f.path[i * 2 - 1], f.path[i * 2], 2)
            end
        end

        -- click marker pulse (shrinking inset)
        if mc.clickMarker then
            lg.setColor(COL_MARKER)
            local k = 1 - (mc.clickMarker.t / 0.8)
            diamondOutline(mc.clickMarker.cx, mc.clickMarker.cy, 2 + k * 6)
        end

        -- unreachable flash (blink)
        if mc.unreachable then
            local blink = math.floor(mc.unreachable.t * 10) % 2 == 0
            if blink then
                lg.setColor(COL_UNREACHABLE)
                diamondOutline(mc.unreachable.cx, mc.unreachable.cy, 2)
                local wx, wy = Iso.cellCenter(mc.unreachable.cx, mc.unreachable.cy)
                lg.line(wx - 6, wy - 6, wx + 6, wy + 6)
                lg.line(wx - 6, wy + 6, wx + 6, wy - 6)
            end
        end

        lg.setLineWidth(prevWidth)
        lg.setColor(1, 1, 1, 1)
    end
end

return MoveOverlays
```

- [ ] **Step 5.2** Parse gate green. **Step 5.3** Commit: `feat(world): click-mode overlays — path preview, click marker, unreachable flash`

---

### Task 6: Game integration (the cutover)

**Files:** Modify `Game.lua`, `RenderSystem.lua`, `ColliderDebug.lua`. Delete `WorldMoveSystem.lua`, `InputSystem.lua`. Modify world_harness (retire WMS/InputSystem sections + parse entries; the slide/tunnel/body-width invariants now live in physics_harness Task 9).

- [ ] **Step 6.1** `Game.lua` — in `loadGameWorld`, after `state.worldRenderer = WorldRenderer.new(map)`:

```lua
    -- Physics world (M1): cells are the statics via TileGrid; props register
    -- as off-grid static bodies (their schema-declared colliders go live).
    local PhysicsWorld = require "physics.PhysicsWorld"
    local CharacterController = require "physics.CharacterController"
    local S = require "physics.shapes"
    state.physicsWorld = PhysicsWorld.new{ map = map }
```

Replace the prop-entity loop body's collider comment: props now ALSO add a static body (footprint placeholder until the editor authors real sizes):

```lua
        elseif ent.type == "prop" and ent.pos then
            local px, py = Iso.cellCenter(ent.pos[1], ent.pos[2])
            props[#props + 1] = {
                position = { x = px, y = py },
                drawable = { color = { 0.55, 0.42, 0.25, 1 }, width = 20, height = 28 },
            }
            if ent.collider == "aabb" then
                state.physicsWorld:addBody{ type = "static", x = px, y = py - 4,
                                            shape = S.aabb(10, 6) }
            end
```

After the player entity is built (before systems are added):

```lua
    -- Player body: capsule from M1 (spec resolved decision #1 — slide feel
    -- on tile corners). e.position stays the logical ground point; the body
    -- is authoritative, synced back each fixed step.
    local playerBody = state.physicsWorld:addBody{
        type = "kinematic", x = spawnX, y = spawnY, shape = S.capsule(6, 9),
    }
    player.phys_body = playerBody

    state.movementController = require("world.MovementController").new{
        map        = map,
        controller = CharacterController.new(state.physicsWorld, playerBody),
        player     = player,
    }
    state.worldRenderer:addOverlay(require("world.MoveOverlays").make(state.movementController))
```

Replace the P1 hover overlay's body: in click mode the hover comes from the controller; in WASD mode no hover. Replace the existing hover overlay registration with:

```lua
    -- Hover-cell highlight (click mode): the P2 path-preview/marker overlays
    -- live in world.MoveOverlays; this is just the cursor cell.
    state.worldRenderer:addOverlay(function(m, camera)
        local mc = state.movementController
        if not mc then return end
        local cx, cy = mc:hoverCell()
        if cx then
            local px, py = Iso.toScreen(cx, cy)
            love.graphics.setColor(1, 1, 1, 0.25)
            love.graphics.polygon("line",
                px + Iso.TILE_W / 2, py,
                px + Iso.TILE_W,     py + Iso.TILE_H / 2,
                px + Iso.TILE_W / 2, py + Iso.TILE_H,
                px,                  py + Iso.TILE_H / 2)
        end
    end)
```

(Note ordering: register MoveOverlays BEFORE the hover overlay so the hover ring draws on top; both register AFTER the DebugDraw overlay registration or before — debug draws last either way is fine, pick: DebugDraw LAST so wireframes sit on top of gameplay overlays. Move the existing DebugDraw `addOverlay` call below these two.)

In the ECS systems block: remove `InputSystem` and `WorldMoveSystem` requires + `state.world:add` entries (keep AnimationSystem, CameraSystem, RenderSystem).

Reinstate `Game:fixedUpdate` (replacing the P1 "no fixedUpdate" comment block):

```lua
-- ============================================================================
-- Fixed-step tick: movement intent + physics. 60 UPS via Application's
-- accumulator. Same PLAYING + !hasScreens gate as :update — the world
-- freezes under menus.
-- ============================================================================

function Game:fixedUpdate(fixedDt)
    if self.gameState == GAME_STATE.PLAYING and state.physicsWorld and not self.UI.hasScreens() then
        local s = Profiler:scope("fixed.physics")
        self:guarded("physics.step", function()
            -- ORDER MATTERS (physics M1 integration contract): step FIRST so
            -- its prev-snapshot predates this tick's slide — the render lerp
            -- then spans exactly the slide. Controller AFTER: WASD slideMove
            -- moves the body directly; click velocity integrates next step.
            state.physicsWorld:step(fixedDt)
            state.movementController:fixedUpdate(fixedDt)
            -- body is authoritative; sync the logical position component
            local p = state.player
            if p and p.phys_body then
                p.position.x, p.position.y = p.phys_body:getPosition()
            end
        end)
        s()
    end
end
```

In `Game:update`, inside the PLAYING gate before `state.world:update(dt)`:

```lua
        if state.movementController then
            self:guarded("movement.update", function() state.movementController:update(dt) end)
        end
```

In `doLogout`, extend the teardown:

```lua
        state.worldMap           = nil
        state.worldRenderer      = nil
        state.physicsWorld       = nil
        state.movementController = nil
```

- [ ] **Step 6.2** `RenderSystem.lua` — `drawEntity` reads the body's interpolated position when present, and `:draw()` applies the M8 camera correction:

In `drawEntity`, replace `local px, py = e.position.x, e.position.y` with:

```lua
    local px, py = e.position.x, e.position.y
    if e.phys_body then
        local ctx = FrameCtx.get()
        px, py = e.phys_body:drawPosition(ctx and ctx.alpha or 1)
    end
```

In `:draw()`, replace `state.camera:lookAt(state.camera.cx or 0, state.camera.cy or 0)` with:

```lua
    -- M8 (2026-06-10 review): follow the player's DRAW position. CameraSystem
    -- smooths toward the fixed-step position in update; at display rates
    -- above 60 Hz the sprite is alpha-lerped between steps, so the camera
    -- adds the same lerp delta — otherwise the player visibly judders
    -- against the camera.
    local fx, fy = state.camera.cx or 0, state.camera.cy or 0
    local pl = state.player
    if pl and pl.phys_body then
        local dx, dy = pl.phys_body:drawPosition(ctx.alpha or 1)
        fx, fy = fx + (dx - pl.position.x), fy + (dy - pl.position.y)
    end
    state.camera:lookAt(fx, fy)
```

- [ ] **Step 6.3** `ColliderDebug.lua` — replace the player ground-segment block (which read `WorldMoveSystem.BODY_RADIUS`, now gone) with the live capsule:

```lua
    -- Player capsule (the live physics collider)
    local p = state.player
    if p and p.phys_body and p.phys_body.world then
        local s = p.phys_body:shape()
        local px, py = p.phys_body:getPosition()
        lg.setColor(COL_BODY)
        lg.circle("line", px - s.halfLen, py, s.r)
        lg.circle("line", px + s.halfLen, py, s.r)
        lg.line(px - s.halfLen, py - s.r, px + s.halfLen, py - s.r)
        lg.line(px - s.halfLen, py + s.r, px + s.halfLen, py + s.r)
    end
```

(Drop the `WorldMoveSystem` require at the top.) Also register the physics channel in `Game:loadGameWorld` next to the colliders channel:

```lua
    DebugDraw.channel("physics", function(_, cam)
        require("physics.PhysicsDebug").draw(state.physicsWorld, cam)
    end, "debugDrawPhysics")
```

and add `debugDrawPhysics = true` to Settings DEFAULTS (debug block) + a "Physics Shapes" sub-toggle row in the settings DEBUG section (same pattern as "Collider Wireframes").

- [ ] **Step 6.4** Delete the dead files and update gates:

```bash
git rm Client/src/systems/WorldMoveSystem.lua Client/src/systems/InputSystem.lua
```

world_harness: remove the `WorldMoveSystem.step` test section entirely (its three invariants — never-in-solid, no-tunnel, body-width — are physics_harness Task 9's tests now) and remove `"systems/WorldMoveSystem.lua"` / `"systems/InputSystem.lua"` from the parse list; add `"world/Pathfinder.lua"`, `"world/PathFollower.lua"`, `"world/MovementController.lua"`, `"world/MoveOverlays.lua"`. assets_harness: swap `"systems/WorldMoveSystem.lua"`-era entries similarly if present.

Residue grep (expect zero hits):

```bash
git grep -n -e "WorldMoveSystem" -e "systems.InputSystem" -- Client
```

- [ ] **Step 6.5** Run ALL FIVE harnesses + boot smoke. **Step 6.6** Commit: `feat(client): movement cutover — physics-driven WASD + click-to-move, interim mover retired`

---

### Task 7: Settings screen rows

**Files:** Modify `Client/src/ui/screens/settings/init.lua`.

- [ ] **Step 7.1** GAMEPLAY section — after the Screen Shake row:

```lua
        y = y + self:_radioRow(y, "Movement Mode", Settings.movementModeOptions(),
            Settings.get("movementMode"), function(v) Settings.set("movementMode", v) end)
```

DEBUG section — after the Physics Shapes row (NO disabled arg: the topology
knob is independent of the debug-DRAW master, it only lives in this section):

```lua
        y = y + self:_radioRow(y, "Path Topology", Settings.pathTopologyOptions(),
            Settings.get("pathTopology"), function(v) Settings.set("pathTopology", v) end)
```

Update `SECTION_LABELS`: gameplay += `"Movement Mode"`, debug += `"Physics Shapes", "Path Topology"`.

(Check `_radioRow`'s exact signature against the Anti-Aliasing usage at line ~436 before transcribing.)

- [ ] **Step 7.2** Boot smoke; open settings (manual UAT note). **Step 7.3** Commit: `feat(client): movement-mode + path-topology settings rows`

---

### Task 8: Final gate + acceptance audit

- [ ] **Step 8.1** All five harnesses green; boot smoke clean; residue grep clean.
- [ ] **Step 8.2** Acceptance audit against the overworld spec:
  - **#2** Toggling the gameplay setting switches WASD ⇄ click at runtime (controller polls per tick; context swap preserves menu stack); only one mode active (exclusive `_applyMode`, path cancelled on switch); identical passability (both modes resolve against `Map.isWalkable` — WASD through TileGrid-derived statics, click through the Pathfinder).
  - **#3** Right-click paths around LOW/TALL cells (both are unwalkable flags); 4/8-dir switchable in one place (Settings `pathTopology` → topology strategy); re-path on new click (`_requestPath` from current body cell); no per-query allocation after warmup (harness-proven).
- [ ] **Step 8.3** Manual UAT list for the user: walk WASD around the devtest block (slide feel on corners — the capsule's whole point); switch to Click in settings mid-session; right-click across the map and behind the TALL block; click an unwalkable cell (red flash); click outside the map (no-op); toggle Path Topology 4-dir and watch staircase paths; F9 shows capsule + tile spans + contacts.
- [ ] **Step 8.4** Update CLAUDE.md if a natural slot exists (client layout mention of `src/physics/`); update the overworld-arc memory: P2 landed, interim mover gone, invariants now physics-owned.
- [ ] **Step 8.5** Commit: `docs: P2 movement complete — acceptance #2/#3 closed`

---

## Self-review checklist (run at execution end)

1. Spec P2 coverage: movement_mode setting + toggle ✓(T1,T7) · MovementController single owner, same intent shape ✓(T4) · WASD context + vector→velocity ✓(T1,T4) · click context, right-click pathing, re-path, unreachable feedback, hover ✓(T1,T4,T5) · Pathfinder topology strategy + binary heap + no-corner-cut + zero alloc ✓(T2) · PathFollower continuous tween ✓(T3) · overlays ✓(T5) · physics integration + interim-mover removal ✓(T6) · M8 camera fix ✓(T6.2).
2. Type consistency: `Pathfinder:find` returns count with flat-pair out array — `PathFollower:setPath(pairsFlat, count)` consumes exactly that; `MovementController` passes `self._pathScratch, n`. `CharacterController:slideMove` returns actual displacement (dx,dy) — controller converts to velocity for the animation component. `hoverCell` returns `cx, cy` or nil.
3. The interim mover's three hard-won invariants (never-in-solid, no-tunnel, body-width) MUST exist in physics_harness before its deletion lands (they do — physics plan Task 9). Verify before executing Task 6.
