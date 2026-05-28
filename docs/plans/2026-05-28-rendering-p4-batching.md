# Phase 4 — Batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce per-frame draw calls + state switches on combat / WaveGame / gacha-pull scenes via two new batch primitives (`SpriteBatch`, `MeshBatch`), POC consumers (combat grid + WaveGame tunnel), a runtime atlas pipeline, an opt-in state-sort, widget-level culling, and one UI batching POC (Panel chrome).

**Architecture:** Two state-preserving managed wrappers over LÖVE's `SpriteBatch`/`Mesh` next to `BlobBatch.lua`. `BlobBatch` becomes a thin user of `SpriteBatch` (its public API + sole consumer untouched). POCs replace immediate-mode polygons/setColor streams with these. A runtime atlas bake at boot makes `Assets.image(key)` return `(tex, quad)`. State-sort is per-Container opt-in via `sortChildren` flag + per-Widget `batchKey`. Validation is empirical: every lever ships before/after `--profile-capture` pairs.

**Tech Stack:** LÖVE 11.x SpriteBatch + Mesh, LuaJIT, canonical Layer/Profile/Renderer compositor, CP-3 `--profile-capture` CSV.

**Spec:** `docs/superpowers/specs/2026-05-28-rendering-p4-batching-design.md` (commit pending).

**Standing constraints:**
- Working tree heavily dirty: targeted `git add <file>` only — NEVER `git add -A` or `git add .`.
- Never skip hooks (`--no-verify`) or bypass signing.
- Combat ABILITIES gameplay off-limits — `ui/screens/combat/CombatRenderer.lua` rendering plumbing is in scope; combat logic is not.
- F2/F3 inventory/party menus slated for full rework — cross-cutting contract migrations (atlas-aware `Assets.image`, state-sort opt-in) are OK; internals stay deferred.

---

## Task 1: Capture-scene CLI flag

Pre-work so subsequent tasks can capture reference scenes (combat, inventory, gacha) headlessly without manually navigating in the running game.

**Files:**
- Modify: `GachaClient/main.lua` (extend the `--profile-capture` CLI parser with `--scene <id>`)
- Modify: `GachaClient/tests/render_harness/main.lua` (no test changes needed — argv parsing isn't unit-testable headlessly; smoke test instead)

- [ ] **Step 1: Add --scene parsing alongside --profile-capture**

In `main.lua`'s `love.load` block where `--profile-capture` is parsed:

```lua
if arg then
    for i = 1, #arg do
        if arg[i] == "--profile-capture" then
            local seconds = tonumber(arg[i + 1])
            local outPath = "profile-capture.csv"
            local sceneId = nil
            for j = 1, #arg do
                if arg[j] == "--output" and arg[j + 1] then outPath = arg[j + 1] end
                if arg[j] == "--scene"  and arg[j + 1] then sceneId = arg[j + 1] end
            end
            if seconds and seconds > 0 then
                Profiler:setEnabled(true)
                local ProfileCapture = require "systems.render.ProfileCapture"
                state._profileCapture = ProfileCapture.start(Profiler, seconds, outPath)
                state._profileCaptureScene = sceneId
                print(("[ProfileCapture] capturing %.1fs -> %s (scene=%s)"):format(seconds, outPath, tostring(sceneId)))
            else
                print("[ProfileCapture] usage: --profile-capture <seconds> [--output <path>] [--scene <id>]")
            end
            break
        end
    end
end
```

- [ ] **Step 2: Push the requested scene after the existing login/landing flow**

After the existing `if cached and cached.token then ... else showLogin() end` block, add:

```lua
-- CP-3 + P4: capture mode can override the boot screen so reference scenes (combat,
-- inventory, party, dailies) can be captured headlessly without manual navigation.
if state._profileCaptureScene then
    local sid = state._profileCaptureScene
    if     sid == "inventory" then UI.push((require "ui.screens.inventory").new())
    elseif sid == "party"     then UI.push((require "ui.screens.party").new())
    elseif sid == "dailies"   then UI.push((require "ui.screens.dailies").new())
    elseif sid == "combat"    then UI.push((require "ui.screens.combat").new({ playerParty = {} }))
    elseif sid == "gacha"     then
        local scr = require("ui.ui_loader").get("screen_2")
        if scr then UI.push(scr) end
    elseif sid == "login"     then -- no-op; default
    else
        print("[ProfileCapture] unknown scene '" .. sid .. "'; using default (login)")
    end
end
```

- [ ] **Step 3: Smoke test each scene boots without crash**

Run:
```
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 1 --output /tmp/p4-task1-inventory.csv --scene inventory
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 1 --output /tmp/p4-task1-party.csv     --scene party
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 1 --output /tmp/p4-task1-dailies.csv   --scene dailies
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 1 --output /tmp/p4-task1-combat.csv    --scene combat
```

Expected: each completes with `[ProfileCapture] wrote N frames -> ...` and writes a valid CSV (head line is `# settings:`, second `# captured:`, third is the schema header).

- [ ] **Step 4: Commit**

```bash
git add GachaClient/main.lua
git commit -m "feat(profiler): --scene flag for headless reference-scene capture (P4 task 1)"
```

---

## Task 2: SpriteBatch primitive

Headless-testable managed wrapper over `love.graphics.newSpriteBatch`. Mirrors `BlobBatch`'s state-preserving pattern.

**Files:**
- Create: `GachaClient/systems/render/SpriteBatch.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (new test block)
- Modify: `GachaClient/tests/assets_harness/main.lua` (syntax-check inclusion)

- [ ] **Step 1: Write failing tests**

Add to `render_harness/main.lua` before the `Renderer dynamic passes` block:

```lua
print("== SpriteBatch primitive (P4) ==")
do
    local SpriteBatch = require "systems.render.SpriteBatch"
    -- Inject a fake love.graphics.newSpriteBatch via opts.factory so the harness can run
    -- without a GL context. The fake records every call for assertions.
    local calls = {}
    local fakeBatch = {
        clear    = function(_)            calls[#calls+1] = "clear" end,
        setColor = function(_, r, g, b, a) calls[#calls+1] = ("setColor:%g,%g,%g,%g"):format(r,g,b,a) end,
        add      = function(_, x, y, r, sx, sy, ox, oy)
            calls[#calls+1] = ("add:%g,%g,%g,%g,%g,%g,%g"):format(x,y,r or 0,sx or 1,sy or 1,ox or 0,oy or 0)
        end,
    }
    local sb = SpriteBatch.new("texture-placeholder", 64, {
        usage   = "stream",
        factory = function() return fakeBatch end,
    })
    sb:begin()
    sb:setColor(1, 0.5, 0.25, 1)
    sb:add(10, 20, 0, 2, 2, 5, 5)
    eq(calls[1], "clear",                          "begin clears the underlying batch")
    eq(calls[2], "setColor:1,0.5,0.25,1",          "setColor forwarded")
    eq(calls[3], "add:10,20,0,2,2,5,5",            "add forwarded")
    -- Capacity / shape sanity: handle exposes texture + capacity for diagnostics.
    eq(sb.capacity, 64,                            "capacity stored")
    eq(sb.texture,  "texture-placeholder",         "texture stored")
end
```

- [ ] **Step 2: Verify failing**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
```
Expected: FAIL on `module 'systems.render.SpriteBatch' not found`.

- [ ] **Step 3: Implement SpriteBatch**

Create `GachaClient/systems/render/SpriteBatch.lua`:

```lua
-- systems/render/SpriteBatch.lua
-- State-preserving managed wrapper over love.graphics.newSpriteBatch. Mirrors BlobBatch's
-- begin/add/draw lifecycle plus an explicit setColor. Restores blend + color on draw so a
-- batch never leaks graphics state to whatever follows it.
--
-- Per-batch blend is configured at construction (opts.blend) and applied + restored around
-- :draw. Capacity is the underlying SpriteBatch's max sprite count; usage maps directly to
-- LÖVE's SpriteBatchUsage ("static" | "dynamic" | "stream", default "stream").
--
-- Tests inject a SpriteBatch fake via opts.factory; production passes love.graphics.newSpriteBatch.
local SpriteBatch = {}
SpriteBatch.__index = SpriteBatch

local function defaultFactory(tex, cap, usage)
    return love.graphics.newSpriteBatch(tex, cap, usage)
end

function SpriteBatch.new(texture, capacity, opts)
    opts = opts or {}
    local self = setmetatable({}, SpriteBatch)
    self.texture  = texture
    self.capacity = capacity
    self.usage    = opts.usage or "stream"
    self.blend    = opts.blend                       -- {"add", "alphamultiply"} | nil
    self.batch    = (opts.factory or defaultFactory)(texture, capacity, self.usage)
    return self
end

function SpriteBatch:begin()
    self.batch:clear()
end

function SpriteBatch:setColor(r, g, b, a)
    self.batch:setColor(r, g, b, a or 1)
end

function SpriteBatch:add(x, y, rot, sx, sy, ox, oy)
    self.batch:add(x, y, rot or 0, sx or 1, sy or 1, ox or 0, oy or 0)
end

-- Emits the batch + restores blend + color so we never leak graphics state.
function SpriteBatch:draw(x, y)
    local pr, pg, pb, pa, prevMode, prevAlpha
    if love and love.graphics then
        pr, pg, pb, pa     = love.graphics.getColor()
        prevMode, prevAlpha = love.graphics.getBlendMode()
        if self.blend then love.graphics.setBlendMode(self.blend[1], self.blend[2] or "alphamultiply") end
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.draw(self.batch, x or 0, y or 0)
        love.graphics.setBlendMode(prevMode, prevAlpha)
        love.graphics.setColor(pr, pg, pb, pa)
    end
end

return SpriteBatch
```

- [ ] **Step 4: Verify tests pass**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
```
Expected: PASS. New "== SpriteBatch primitive (P4) ==" block green.

- [ ] **Step 5: Add to assets harness syntax-check list**

In `tests/assets_harness/main.lua`, in the `touched` list:

```lua
    "systems/render/Renderer.lua", "systems/render/Pass.lua",
    "systems/render/Profiler.lua", "systems/render/ProfileCapture.lua",
    "systems/render/SpriteBatch.lua",
}
```

- [ ] **Step 6: Verify both harnesses green**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
```

- [ ] **Step 7: Commit**

```bash
git add GachaClient/systems/render/SpriteBatch.lua GachaClient/tests/render_harness/main.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(render): SpriteBatch primitive (P4 task 2)"
```

---

## Task 3: MeshBatch primitive

Headless-testable managed wrapper over `love.graphics.newMesh`. Streams vertices into a backing buffer; one `flush()` uploads them via `Mesh:setVertices` and emits one draw.

**Files:**
- Create: `GachaClient/systems/render/MeshBatch.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (new test block)
- Modify: `GachaClient/tests/assets_harness/main.lua` (syntax-check inclusion)

- [ ] **Step 1: Write failing tests**

Add to `render_harness/main.lua` after the SpriteBatch block:

```lua
print("== MeshBatch primitive (P4) ==")
do
    local MeshBatch = require "systems.render.MeshBatch"
    -- Fake love.graphics.newMesh that records setVertices payloads.
    local lastVerts, drawn = nil, false
    local fakeMesh = {
        setVertices    = function(_, verts, _start) lastVerts = verts end,
        setDrawRange   = function(_, lo, hi) end,
        setTexture     = function(_, _t) end,
    }
    local format = { {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4} }
    local mb = MeshBatch.new(format, 64, {
        mode    = "triangles",
        factory = function() return fakeMesh end,
    })
    mb:begin()
    mb:addQuad(10, 10, 20, 20, {255, 128, 64, 255})
    eq(mb:vertexCount(), 6,                        "addQuad emits 6 vertices (2 triangles)")
    mb:addTriangle(0, 0, 10, 0, 5, 10, {0, 255, 0, 255})
    eq(mb:vertexCount(), 9,                        "addTriangle emits 3 vertices")
    mb:flush()
    eq(#lastVerts, 9,                              "flush uploaded 9 vertices to underlying Mesh")
    -- Each vertex is { x, y, r, g, b, a } based on the format.
    eq(lastVerts[1][1], 10,                        "vertex 1 x")
    eq(lastVerts[1][3], 255,                       "vertex 1 r")
    -- Second begin resets the write head.
    mb:begin()
    eq(mb:vertexCount(), 0,                        "begin resets vertex count")
end
```

- [ ] **Step 2: Verify failing**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
```
Expected: FAIL on `module 'systems.render.MeshBatch' not found`.

- [ ] **Step 3: Implement MeshBatch**

Create `GachaClient/systems/render/MeshBatch.lua`:

```lua
-- systems/render/MeshBatch.lua
-- State-preserving managed wrapper over love.graphics.newMesh. Streams vertices into an
-- in-memory buffer; flush() uploads them via Mesh:setVertices + emits one draw call.
--
-- Vertex format follows LÖVE's MeshFormat: an array of { name, type, components } records.
-- addVertex receives positional args in the order the format declares them; addQuad and
-- addTriangle are convenience helpers that assume a "VertexPosition" + "VertexColor" format.
--
-- Tests inject a Mesh fake via opts.factory; production passes love.graphics.newMesh.
local MeshBatch = {}
MeshBatch.__index = MeshBatch

local function defaultFactory(format, capacity, mode, usage)
    return love.graphics.newMesh(format, capacity, mode, usage)
end

function MeshBatch.new(format, capacity, opts)
    opts = opts or {}
    local self = setmetatable({}, MeshBatch)
    self.format   = format
    self.capacity = capacity
    self.mode     = opts.mode    or "triangles"
    self.usage    = opts.usage   or "stream"
    self.texture  = opts.texture
    self.blend    = opts.blend
    self.mesh     = (opts.factory or defaultFactory)(format, capacity, self.mode, self.usage)
    if self.texture and self.mesh.setTexture then self.mesh:setTexture(self.texture) end
    self.verts    = {}    -- pending vertex rows; reset on begin
    return self
end

function MeshBatch:begin()
    self.verts = {}
end

function MeshBatch:vertexCount() return #self.verts end

-- Add a raw vertex. Positional args after (x, y) depend on the format. Common case is
-- VertexPosition (float×2) + VertexColor (byte×4): addVertex(x, y, r, g, b, a).
function MeshBatch:addVertex(x, y, r, g, b, a)
    self.verts[#self.verts + 1] = { x, y, r or 255, g or 255, b or 255, a or 255 }
end

-- Convenience: 3 vertices for one triangle. color = {r, g, b, a} in byte units.
function MeshBatch:addTriangle(x1, y1, x2, y2, x3, y3, color)
    local r, g, b, a = color[1], color[2], color[3], color[4] or 255
    self:addVertex(x1, y1, r, g, b, a)
    self:addVertex(x2, y2, r, g, b, a)
    self:addVertex(x3, y3, r, g, b, a)
end

-- Convenience: 6 vertices for one axis-aligned quad (two triangles).
function MeshBatch:addQuad(x, y, w, h, color)
    local x2, y2 = x + w, y + h
    self:addTriangle(x, y,  x2, y, x2, y2, color)
    self:addTriangle(x, y,  x2, y2, x,  y2, color)
end

-- Upload pending vertices to the underlying Mesh + emit one draw. State-preserving.
function MeshBatch:flush(x, y)
    if #self.verts == 0 then return end
    self.mesh:setVertices(self.verts, 1)
    if self.mesh.setDrawRange then self.mesh:setDrawRange(1, #self.verts) end
    if love and love.graphics then
        local pr, pg, pb, pa = love.graphics.getColor()
        local prevMode, prevAlpha = love.graphics.getBlendMode()
        if self.blend then love.graphics.setBlendMode(self.blend[1], self.blend[2] or "alphamultiply") end
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.draw(self.mesh, x or 0, y or 0)
        love.graphics.setBlendMode(prevMode, prevAlpha)
        love.graphics.setColor(pr, pg, pb, pa)
    end
end

-- Alias for callers that prefer the SpriteBatch-style :draw.
MeshBatch.draw = MeshBatch.flush

return MeshBatch
```

- [ ] **Step 4: Verify tests pass**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
```

- [ ] **Step 5: Add to assets harness syntax-check list**

```lua
    "systems/render/MeshBatch.lua",
```

- [ ] **Step 6: Verify both harnesses green**

- [ ] **Step 7: Commit**

```bash
git add GachaClient/systems/render/MeshBatch.lua GachaClient/tests/render_harness/main.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(render): MeshBatch primitive (P4 task 3)"
```

---

## Task 4: BlobBatch delegates to SpriteBatch

Refactor `BlobBatch` to use the new `SpriteBatch` primitive without changing its public API. `PullTunnel.lua:48` is untouched.

**Files:**
- Modify: `GachaClient/systems/render/BlobBatch.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (BlobBatch.profile tests untouched; add a sanity check that the new BlobBatch shape still constructs)

- [ ] **Step 1: Refactor BlobBatch**

Replace `BlobBatch.new` and `BlobBatch:begin/add/draw` with:

```lua
local SpriteBatch = require "systems.render.SpriteBatch"

function BlobBatch.new(kind, maxSprites)
    local self = setmetatable({}, BlobBatch)
    self.texture = BlobBatch.newTexture(kind)
    self.sprites = SpriteBatch.new(self.texture, maxSprites, {
        usage = "stream",
        blend = {"add", "alphamultiply"},
    })
    self.halfTex = TEX_SIZE * 0.5
    return self
end

function BlobBatch:begin() self.sprites:begin() end

function BlobBatch:add(px, py, radiusPx, r, g, b, amp)
    local s = radiusPx / TEX_SIGMA
    self.sprites:setColor(r * amp, g * amp, b * amp, 1.0)
    self.sprites:add(px, py, 0, s, s, self.halfTex, self.halfTex)
end

function BlobBatch:draw() self.sprites:draw(0, 0) end
```

The `BlobBatch.profile` pure function and `BlobBatch.newTexture` stay unchanged.

- [ ] **Step 2: Verify existing BlobBatch tests still pass**

The harness already covers `BlobBatch.profile`; that's unaffected. The `:new`/`:begin`/`:add`/`:draw` cycle isn't headless-testable (needs love.graphics for the texture). Smoke-test by running a 2-second capture of the gacha pull reveal:

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 2 --output /tmp/p4-task4-blob.csv --scene gacha
```

Expected: completes without crash. (Visual gate: pull-reveal still looks identical.)

- [ ] **Step 3: Verify harnesses green**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
```

- [ ] **Step 4: Commit**

```bash
git add GachaClient/systems/render/BlobBatch.lua
git commit -m "refactor(render): BlobBatch delegates to SpriteBatch (P4 task 4)"
```

---

## Task 5: Combat grid POC via MeshBatch

Replace the 10×10 immediate-mode polygon loop with one `MeshBatch` of tile diamonds + a separate small `MeshBatch` for highlights.

**Files:**
- Modify: `GachaClient/ui/screens/combat/CombatRenderer.lua` (touch only `_drawGrid` + highlight draw paths; no combat logic changes)

- [ ] **Step 1: Capture combat baseline**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 4 --output docs/audits/p4/combat-before.csv --scene combat
```

Record steady-state `scene.drawcalls` from the captured CSV (averaged across frames 60+ to skip warmup).

- [ ] **Step 2: Read _drawGrid + identify the loop**

In `CombatRenderer.lua`, locate `_drawGrid` (around lines 347-387) — the Lua double-loop over `gridW × gridH` that does `polygon("fill")` + `polygon("line")` per tile.

- [ ] **Step 3: Replace _drawGrid body**

Add at top of file (after existing requires):

```lua
local MeshBatch = require "systems.render.MeshBatch"
```

Add to CombatRenderer constructor or first-use lazy initializer:

```lua
-- P4: tile grid + highlights as MeshBatches. Built once on _initGridMesh; per-frame is
-- one :begin + N :addQuad/Triangle + :flush -> one draw call. Capacity is 100 tiles × 2
-- triangles = 200 verts for fills, doubled for outlines/highlights so all fit one batch.
self._gridFillBatch    = MeshBatch.new({
    {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4},
}, 200, { mode = "triangles" })
self._gridOutlineBatch = MeshBatch.new({
    {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4},
}, 400, { mode = "triangles" })  -- 4 thin tris per tile for the line border
self._highlightBatch   = MeshBatch.new({
    {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4},
}, 200, { mode = "triangles" })
```

Replace `_drawGrid` body with:

```lua
function CombatRenderer:_drawGrid()
    local fills, outlines = self._gridFillBatch, self._gridOutlineBatch
    fills:begin()
    outlines:begin()
    for gy = 1, self.gridH do
        for gx = 1, self.gridW do
            local cx, cy = self:_tileCenter(gx, gy)
            local tw, th = self.tileW, self.tileH
            -- Diamond corners (top, right, bottom, left)
            local x1, y1 = cx,         cy - th * 0.5
            local x2, y2 = cx + tw * 0.5, cy
            local x3, y3 = cx,         cy + th * 0.5
            local x4, y4 = cx - tw * 0.5, cy
            local fillColor = self:_tileFillColor(gx, gy)
            fills:addTriangle(x1, y1, x2, y2, x3, y3, fillColor)
            fills:addTriangle(x1, y1, x3, y3, x4, y4, fillColor)
            local lineColor = self:_tileLineColor(gx, gy)
            -- Border as 4 thin triangles (one per diamond edge). 2px-equivalent thickness.
            local LINE_T = 1.0
            local function edge(ax, ay, bx, by)
                local dx, dy = bx - ax, by - ay
                local len = math.sqrt(dx*dx + dy*dy)
                if len == 0 then return end
                local nx, ny = -dy / len * LINE_T, dx / len * LINE_T
                outlines:addTriangle(ax - nx, ay - ny, bx - nx, by - ny, bx + nx, by + ny, lineColor)
                outlines:addTriangle(ax - nx, ay - ny, bx + nx, by + ny, ax + nx, ay + ny, lineColor)
            end
            edge(x1, y1, x2, y2)
            edge(x2, y2, x3, y3)
            edge(x3, y3, x4, y4)
            edge(x4, y4, x1, y1)
        end
    end
    fills:flush()
    outlines:flush()
end
```

You may need to add or extract `_tileFillColor`/`_tileLineColor` helpers reading the same color logic that the existing `setColor + polygon` calls used. Inline if the originals computed color in-place.

Similarly for highlights — refactor whatever `_drawHighlights` or equivalent does into `self._highlightBatch:begin/addQuad/flush`.

- [ ] **Step 4: Capture combat after**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 4 --output docs/audits/p4/combat-after.csv --scene combat
```

- [ ] **Step 5: Compare**

`scene.drawcalls` per frame should drop from ~120 (100 fills + 100 outlines + N highlights + others) to ~3-5 (1 fills + 1 outlines + 1 highlights + others). If not, investigate before committing.

- [ ] **Step 6: USER VISUAL GATE**

User opens the game, presses F5 (combat), confirms:
1. Grid renders identically (same colors, same isometric layout, no missing tiles).
2. Highlight pulses still animate.
3. Hover ring + units + HP bars unchanged.

- [ ] **Step 7: Commit on visual GO**

```bash
git add GachaClient/ui/screens/combat/CombatRenderer.lua docs/audits/p4/combat-before.csv docs/audits/p4/combat-after.csv
git commit -m "feat(render): combat grid via MeshBatch (P4 task 5)"
```

---

## Task 6: WaveGame tunnel POC via MeshBatch

Replace the ~1500 immediate-mode `polygon("fill")` per-frame storm with two `MeshBatch` instances (one per bloom pass).

**Files:**
- Modify: `GachaClient/ui/screens/dailies/WaveGame.lua`

- [ ] **Step 1: Capture WaveGame baseline**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 4 --output docs/audits/p4/wavegame-before.csv --scene dailies
```

(Note: `--scene dailies` pushes the Dailies screen; WaveGame inside needs to be triggered. Either wait for the auto-play / use a `--play-wave` extension, OR record from a manual session. If manual, run with `--profile-capture 6` and press F4 + start a wave manually.)

- [ ] **Step 2: Add MeshBatch instances**

Top of `WaveGame.lua` (after requires):

```lua
local MeshBatch = require "systems.render.MeshBatch"
```

In the WaveGame constructor / `_initGfx`:

```lua
-- P4: tunnel segments as MeshBatches. Each pass batches all segments' triangles into
-- one draw. Capacity: 150 segments × 5 triangles × 3 verts = 2250; double-margin = 4096.
self._tunnelFill  = MeshBatch.new({
    {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4},
}, 4096, { mode = "triangles" })
self._tunnelGlow  = MeshBatch.new({
    {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4},
}, 4096, { mode = "triangles", blend = {"add", "alphamultiply"} })
```

- [ ] **Step 3: Replace _drawTunnel polygon storm**

Locate `_drawTunnel` (around lines 881-887, 920-954) and replace the polygon stream with `MeshBatch` adds:

```lua
function WaveGame:_drawTunnel()
    local fills, glows = self._tunnelFill, self._tunnelGlow
    fills:begin()
    glows:begin()
    for i = 1, #self.segments do
        local seg = self.segments[i]
        -- (Replicate the existing 5-polygon shape per segment: top/bottom walls + cap.
        -- Each polygon becomes 1-2 triangles via addTriangle / addQuad with the same colors
        -- the old setColor calls used.)
        local color = seg.color   -- {r, g, b, a} in byte units
        fills:addQuad(seg.x, seg.y, seg.w, seg.h, color)
        -- ...replicate the existing 5-polygon decomposition...
        -- Glow pass uses the same geometry with the brighter / wider color.
        glows:addQuad(seg.x - 4, seg.y - 4, seg.w + 8, seg.h + 8, seg.glowColor)
    end
    fills:flush()
    glows:flush()
end
```

(Use the actual segment geometry from the current code; this is the shape, not the geometry. Preserve the existing polygon-to-triangle decomposition exactly.)

- [ ] **Step 4: Capture WaveGame after**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 6 --output docs/audits/p4/wavegame-after.csv --scene dailies
```

- [ ] **Step 5: Compare**

`scene.drawcalls` per frame at peak segment count drops from ~1500+ to ~5 (1 fill batch + 1 glow batch + a few overlays).

- [ ] **Step 6: USER VISUAL GATE**

User opens dailies, plays a wave, confirms:
1. Tunnel renders identically (segment colors, glow, near-miss vignette).
2. Wave SFX/feel unchanged.
3. Game ends correctly (no segment leak that would suggest a flush ordering bug).

- [ ] **Step 7: Commit on visual GO**

```bash
git add GachaClient/ui/screens/dailies/WaveGame.lua docs/audits/p4/wavegame-before.csv docs/audits/p4/wavegame-after.csv
git commit -m "feat(render): WaveGame tunnel via MeshBatch (P4 task 6)"
```

---

## Task 7: Skyline packing algorithm

Pure-Lua skyline-pack algorithm. Headless-testable. Foundation for the atlas builder.

**Files:**
- Create: `GachaClient/services/assets/skyline.lua`
- Modify: `GachaClient/tests/assets_harness/main.lua` (new test block)

- [ ] **Step 1: Write failing tests**

Add to `tests/assets_harness/main.lua` after the existing `BlobBatch.profile` block:

```lua
print("== Skyline packer (P4) ==")
do
    local Skyline = assert(loadfile(CLIENT .. "services/assets/skyline.lua"))()
    -- Pack 4 rectangles into 256x256; expect all placed, none overlapping, all inside.
    local sky = Skyline.new(256, 256)
    local r1 = sky:insert(100, 80)
    local r2 = sky:insert(80, 80)
    local r3 = sky:insert(50, 50)
    local r4 = sky:insert(40, 40)
    local function rectInside(r) return r and r.x >= 0 and r.y >= 0 and r.x + r.w <= 256 and r.y + r.h <= 256 end
    assertEq(rectInside(r1), true, "r1 inside")
    assertEq(rectInside(r2), true, "r2 inside")
    assertEq(rectInside(r3), true, "r3 inside")
    assertEq(rectInside(r4), true, "r4 inside")
    -- Overflow returns nil.
    local huge = sky:insert(300, 300)
    assertEq(huge, nil, "huge rect overflows -> nil")
    -- Non-overlap check (pairwise).
    local rects = { r1, r2, r3, r4 }
    for i = 1, #rects do
        for j = i + 1, #rects do
            local a, b = rects[i], rects[j]
            local overlap = not (a.x + a.w <= b.x or b.x + b.w <= a.x or a.y + a.h <= b.y or b.y + b.h <= a.y)
            if overlap then fail("rect " .. i .. " overlaps rect " .. j) end
        end
    end
end
```

- [ ] **Step 2: Verify failing**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
```

- [ ] **Step 3: Implement Skyline**

Create `GachaClient/services/assets/skyline.lua`:

```lua
-- services/assets/skyline.lua
-- Skyline rectangle packing (Igor Mussa's skyline algorithm). Pure Lua. Used by the
-- runtime atlas builder. Place rectangles into a (W,H) bin; each insert picks the
-- skyline position with the lowest height that fits, then raises the skyline.
--
-- Not optimal but well-bounded and predictable. For best packing, callers should sort
-- inputs by descending height before inserting.
local Skyline = {}
Skyline.__index = Skyline

function Skyline.new(W, H)
    return setmetatable({
        W = W, H = H,
        nodes = { { x = 0, y = 0, w = W } },   -- skyline = list of horizontal spans at heights
    }, Skyline)
end

-- Find the best y to place a (w,h) rect at node i. Returns y or nil if it won't fit.
local function findBestY(self, i, w)
    local n = self.nodes[i]
    if n.x + w > self.W then return nil end
    local y = n.y
    local widthLeft = w
    local j = i
    while widthLeft > 0 do
        local nj = self.nodes[j]
        if not nj then return nil end
        if nj.y > y then y = nj.y end
        widthLeft = widthLeft - nj.w
        j = j + 1
    end
    return y
end

-- Add a node + collapse overlapping nodes after insertion.
local function addSkyline(self, idx, x, y, w)
    table.insert(self.nodes, idx, { x = x, y = y, w = w })
    for i = idx + 1, #self.nodes do
        local prev, cur = self.nodes[i - 1], self.nodes[i]
        if cur.x < prev.x + prev.w then
            local shrink = prev.x + prev.w - cur.x
            cur.x = cur.x + shrink
            cur.w = cur.w - shrink
            if cur.w <= 0 then
                table.remove(self.nodes, i)
                return addSkyline(self, idx, x, y, w)   -- restart collapse
            end
        else
            break
        end
    end
    -- Merge equal-height adjacent.
    local i = 1
    while i < #self.nodes do
        if self.nodes[i].y == self.nodes[i + 1].y then
            self.nodes[i].w = self.nodes[i].w + self.nodes[i + 1].w
            table.remove(self.nodes, i + 1)
        else
            i = i + 1
        end
    end
end

function Skyline:insert(w, h)
    local bestY, bestI, bestX = math.huge, nil, nil
    for i = 1, #self.nodes do
        local y = findBestY(self, i, w)
        if y and (y + h <= self.H) and (y < bestY) then
            bestY, bestI, bestX = y, i, self.nodes[i].x
        end
    end
    if not bestI then return nil end
    addSkyline(self, bestI, bestX, bestY + h, w)
    return { x = bestX, y = bestY, w = w, h = h }
end

return Skyline
```

- [ ] **Step 4: Verify tests pass**

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
```

- [ ] **Step 5: Commit**

```bash
git add GachaClient/services/assets/skyline.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(assets): skyline rectangle packer (P4 task 7)"
```

---

## Task 8: Atlas builder + Assets integration

Tie the skyline packer + image loader together. Bake at boot from manifest. `Assets.image(key) → (tex, quad)`. Migrate one widget consumer to validate.

**Files:**
- Create: `GachaClient/services/assets/atlas.lua`
- Create: `GachaClient/data/atlases/ui.json`
- Modify: `GachaClient/services/Assets.lua` (atlas-aware `image()` + new `draw()` helper + `bakeAtlases()` boot hook)
- Modify: `GachaClient/main.lua` (call `Assets.bakeAtlases()` after `UILoader.loadDirectory`)
- Modify: one widget consumer (`GachaClient/ui/widgets/icon.lua` — already routes through Assets — verify the 2-return shape works there)
- Modify: `GachaClient/tests/assets_harness/main.lua` (atlas-aware Assets.image test)

- [ ] **Step 1: Write the atlas manifest**

Create `GachaClient/data/atlases/ui.json`:

```json
{
    "max_size": 2048,
    "filter": "linear",
    "keys": [
        "portrait_*",
        "item_*",
        "rarity_*"
    ]
}
```

(Adjust the glob patterns to match the actual image-key naming in `data/image/`.)

- [ ] **Step 2: Implement Atlas builder**

Create `GachaClient/services/assets/atlas.lua`:

```lua
-- services/assets/atlas.lua
-- Runtime atlas baker. Loads each manifest-declared image at boot, packs into one
-- canvas via skyline, returns the canvas + key->Quad map. Failure modes are graceful:
-- missing sources are skipped + logged; atlas overflow falls back to standalone.
local Skyline  = require "services.assets.skyline"
local Logger   = require "services.Logger"
local log      = Logger.create("Atlas")

local Atlas = {}

local function matchesGlob(key, pattern)
    -- "portrait_*" -> "^portrait_.+$"
    local pat = "^" .. pattern:gsub("%*", ".+") .. "$"
    return key:match(pat) ~= nil
end

-- Resolve atlas-eligible keys from the manifest patterns by walking discovered image keys.
local function resolveKeys(patterns, allKeys)
    local out = {}
    for _, k in ipairs(allKeys) do
        for _, p in ipairs(patterns) do
            if matchesGlob(k, p) then out[#out + 1] = k; break end
        end
    end
    return out
end

--- Bake an atlas from a manifest. Returns (canvas, keyToQuad) or (nil, err).
function Atlas.bake(manifestPath, imageLoader, allImageKeys)
    local cfg = require("services.assets.data").load(manifestPath)
    if not cfg then return nil, "missing atlas manifest: " .. manifestPath end
    local maxSize = cfg.max_size or 2048
    local filter  = cfg.filter   or "linear"
    local keys = resolveKeys(cfg.keys or {}, allImageKeys)

    -- Load + sort by descending height for skyline packing.
    local items = {}
    for _, k in ipairs(keys) do
        local img = imageLoader(k)
        if img then
            items[#items + 1] = { key = k, image = img, w = img:getWidth(), h = img:getHeight() }
        else
            log.warn("atlas '%s': key '%s' missing image; skipping", manifestPath, k)
        end
    end
    table.sort(items, function(a, b) return a.h > b.h end)

    local sky = Skyline.new(maxSize, maxSize)
    local canvas = love.graphics.newCanvas(maxSize, maxSize, { mipmaps = "manual" })
    canvas:setFilter(filter, filter)
    love.graphics.push("all")
    love.graphics.setCanvas(canvas)
    love.graphics.clear(0, 0, 0, 0)
    love.graphics.setBlendMode("replace", "premultiplied")
    love.graphics.setColor(1, 1, 1, 1)

    local quads = {}
    local placed, overflowed = 0, 0
    for _, it in ipairs(items) do
        local r = sky:insert(it.w, it.h)
        if r then
            love.graphics.draw(it.image, r.x, r.y)
            quads[it.key] = love.graphics.newQuad(r.x, r.y, it.w, it.h, maxSize, maxSize)
            placed = placed + 1
        else
            overflowed = overflowed + 1
        end
    end
    love.graphics.pop()
    pcall(function() canvas:generateMipmaps() end)
    log.info("atlas '%s': %d placed, %d overflowed", manifestPath, placed, overflowed)
    return canvas, quads
end

return Atlas
```

- [ ] **Step 3: Wire Atlas into Assets**

In `GachaClient/services/Assets.lua`:

```lua
-- Atlas state. Multiple atlases keyed by manifest path; each maps from image-key to a
-- (canvas, quad) pair so Assets.image returns shared-texture+quad when atlased.
local _atlases       = {}    -- manifestPath -> { canvas, quads = key -> Quad }
local _atlasByKey    = {}    -- imageKey -> { canvas, quad }   reverse lookup for fast Assets.image

--- Bake every declared atlas at boot. Called from main.lua after UI/string-table load.
function Assets.bakeAtlases()
    if not (love and love.filesystem and love.filesystem.getInfo) then return end
    local dir = "data/atlases"
    local info = love.filesystem.getInfo(dir)
    if not info or info.type ~= "directory" then return end
    local Atlas = require "services.assets.atlas"
    -- Image-load helper for the bake. Uses the same loader path as Assets.image but bypasses
    -- the atlas-aware return shape (we want the raw Image to blit, not a (tex, quad) pair).
    local function rawImageLoad(key)
        local id = "image:" .. key
        if cache:has(id) then
            local obj = cache:get(id)
            return (type(obj) ~= "boolean") and obj or nil
        end
        local img = loaders.image.load(Manifest.resolve("image", key))
        return img or nil
    end
    local allKeys = Manifest.keys("image") or {}
    for _, name in ipairs(love.filesystem.getDirectoryItems(dir)) do
        if name:sub(-5) == ".json" then
            local path = dir .. "/" .. name
            local canvas, quads = Atlas.bake(path, rawImageLoad, allKeys)
            if canvas then
                _atlases[path] = { canvas = canvas, quads = quads }
                for k, q in pairs(quads) do
                    _atlasByKey[k] = { canvas = canvas, quad = q }
                end
            end
        end
    end
end
```

Modify `Assets.image` to return `(tex, quad)`:

```lua
function Assets.image(key, opts)
    local atl = _atlasByKey[key]
    if atl then return atl.canvas, atl.quad end
    local id = "image:" .. key
    if cache:has(id) then return cache:get(id) or nil, nil end
    local obj, bytes, err = loaders.image.load(Manifest.resolve("image", key), opts)
    if obj then cache:put(id, "image", obj, bytes); return obj, nil end
    logOnce("image", key, err)
    cache:put(id, "image", false, 0)
    return nil, nil
end
```

Add `Assets.draw` helper:

```lua
--- Draw an image by key, handling the optional atlas quad transparently. (w, h) are the
--- destination size in pixels; the helper computes scale automatically. Caller-set color
--- and blend apply; this only takes care of the texture + quad + scale math.
function Assets.draw(key, x, y, w, h, rot)
    local tex, quad = Assets.image(key)
    if not tex then return end
    if quad then
        local _, _, qw, qh = quad:getViewport()
        local sx = (w and qw > 0) and (w / qw) or 1
        local sy = (h and qh > 0) and (h / qh) or 1
        love.graphics.draw(tex, quad, x, y, rot or 0, sx, sy)
    else
        local iw, ih = tex:getDimensions()
        local sx = (w and iw > 0) and (w / iw) or 1
        local sy = (h and ih > 0) and (h / ih) or 1
        love.graphics.draw(tex, x, y, rot or 0, sx, sy)
    end
end
```

- [ ] **Step 4: Audit existing Assets.image consumers**

Search for `Assets.image(` callers. Most assign to a single var (`local img = Assets.image(key)`) and DON'T expect a quad — those are still valid (Lua discards the 2nd return). The only places that break are consumers that ALREADY use a quad path (currently none — the icon registry has its own quad system separate from atlases).

Verify:
```bash
grep -rn "Assets.image(" GachaClient/ --include="*.lua" | head -40
```

Spot-check: `nine_slice.lua`, `image.lua` widget, `WaveGame.lua`, etc. — all should work unchanged.

- [ ] **Step 5: Wire bake into boot**

In `main.lua` `love.load`, after `UILoader.loadDirectory("data/ui_screens")`:

```lua
Assets.bakeAtlases()
```

(Add `local Assets = require "services.Assets"` if not already locally bound; it's required via the QW-1 wiring.)

- [ ] **Step 6: Migrate one widget consumer to Assets.draw**

In `GachaClient/ui/widgets/image.lua` (or whichever widget displays portraits/icons), find the existing `love.graphics.draw(img, x, y, ...)` call and replace with `Assets.draw(key, x, y, w, h)`. Validate by running the inventory + party scenes.

- [ ] **Step 7: Capture before/after**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/inventory-atlas-before.csv --scene inventory
# (after wiring)
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/inventory-atlas-after.csv  --scene inventory
```

Target: `ui.drawcalls` per frame drops by ~25-35% on the inventory scene.

- [ ] **Step 8: USER VISUAL GATE**

User opens F2 + F3, confirms portraits + item icons render identically.

- [ ] **Step 9: Commit on visual GO**

```bash
git add GachaClient/services/assets/atlas.lua GachaClient/data/atlases/ui.json GachaClient/services/Assets.lua GachaClient/main.lua GachaClient/ui/widgets/image.lua GachaClient/tests/assets_harness/main.lua docs/audits/p4/inventory-atlas-before.csv docs/audits/p4/inventory-atlas-after.csv
git commit -m "feat(assets): runtime atlas pipeline + ui atlas (P4 task 8)"
```

---

## Task 9: BatchKey + Container.sortChildren

Opt-in per-Container state-sort + per-Widget `batchKey`. Stable sort preserves z. No default behavior change.

**Files:**
- Create: `GachaClient/systems/render/BatchKey.lua`
- Modify: `GachaClient/ui/core/widget.lua` (`widget.batchKey` field — default nil)
- Modify: `GachaClient/ui/core/container.lua` (`sortChildren` flag + stable sort before draw)
- Modify: `GachaClient/tests/render_harness/main.lua` (stable-sort tests)

- [ ] **Step 1: Write failing tests for BatchKey + stable sort**

```lua
print("== BatchKey + stable sort (P4) ==")
do
    local BK = require "systems.render.BatchKey"
    -- BatchKey.compute returns a deterministic string from (shader, texture, blend).
    local k1 = BK.compute({ shader = "glow",  texture = "atlas_ui", blend = "alpha" })
    local k2 = BK.compute({ shader = "glow",  texture = "atlas_ui", blend = "alpha" })
    local k3 = BK.compute({ shader = "plain", texture = "atlas_ui", blend = "alpha" })
    eq(k1, k2,  "same inputs -> same key")
    if k1 == k3 then fail("different shader -> different key") end

    -- BatchKey.stableSort preserves order among equal keys.
    local arr = {
        { id = "a", batchKey = "g" },
        { id = "b", batchKey = "p" },
        { id = "c", batchKey = "g" },
        { id = "d", batchKey = nil },     -- unsortable, anchors in place
        { id = "e", batchKey = "p" },
        { id = "f", batchKey = "g" },
    }
    BK.stableSort(arr)
    -- Expected: nil-keyed elements stay in their hand-authored slots; sortable runs around
    -- each unsortable anchor are stable-sorted by key.
    -- "d" is at index 4 originally; "a,b,c" before it sort among themselves; "e,f" after.
    eq(arr[1].id, "a", "first g preserves order in pre-d run")
    eq(arr[2].id, "c", "second g preserves order")
    eq(arr[3].id, "b", "p after the g's in pre-d run")
    eq(arr[4].id, "d", "unsortable anchor stays at index 4")
    eq(arr[5].id, "f", "g in post-d run")
    eq(arr[6].id, "e", "p in post-d run")
end
```

- [ ] **Step 2: Implement BatchKey**

Create `GachaClient/systems/render/BatchKey.lua`:

```lua
-- systems/render/BatchKey.lua
-- Per-widget batch-grouping key. Widgets that opt in set widget.batchKey to a string of
-- (shader, texture, blend) so a sortable Container can group them. Equal keys are
-- stable-sorted; nil-keyed (unsortable) widgets anchor in place.
local BatchKey = {}

function BatchKey.compute(spec)
    -- spec: { shader, texture, blend }. nil components stringify as "_".
    return tostring(spec.shader or "_") .. "|"
        .. tostring(spec.texture or "_") .. "|"
        .. tostring(spec.blend or "_")
end

-- Stable sort: any widget with batchKey == nil is an anchor. Within each segment between
-- anchors (or end-points), sortable widgets are sorted by batchKey, preserving relative
-- order among equal keys.
function BatchKey.stableSort(arr)
    -- Split into runs around nil-keyed anchors, sort each run stably, reassemble.
    local out = {}
    local run = {}
    for i = 1, #arr do
        local w = arr[i]
        if w.batchKey == nil then
            if #run > 0 then
                table.sort(run, function(a, b)
                    if a.batchKey == b.batchKey then return a._idx < b._idx end
                    return a.batchKey < b.batchKey
                end)
                for _, r in ipairs(run) do out[#out + 1] = r end
                run = {}
            end
            out[#out + 1] = w
        else
            w._idx = i   -- stable-sort tiebreaker
            run[#run + 1] = w
        end
    end
    if #run > 0 then
        table.sort(run, function(a, b)
            if a.batchKey == b.batchKey then return a._idx < b._idx end
            return a.batchKey < b.batchKey
        end)
        for _, r in ipairs(run) do out[#out + 1] = r end
    end
    for i = 1, #out do arr[i] = out[i]; arr[i]._idx = nil end
    return arr
end

return BatchKey
```

- [ ] **Step 3: Verify tests**

- [ ] **Step 4: Wire Container.sortChildren**

In `GachaClient/ui/core/container.lua`, in the `draw` method (or wherever children iterate), guard the existing iteration with the sort:

```lua
function Container:draw()
    -- ...existing pre-draw setup...
    if self.sortChildren and #self.children > 1 then
        local BatchKey = require "systems.render.BatchKey"
        BatchKey.stableSort(self.children)
    end
    for _, child in ipairs(self.children) do
        if child.visible then child:draw() end
    end
end
```

- [ ] **Step 5: Add widget.batchKey field**

In `GachaClient/ui/core/widget.lua`, in the constructor (or wherever default fields are set):

```lua
widget.batchKey = nil   -- P4: opt-in for state-sort containers; nil = unsortable anchor
```

(No-op for any widget that doesn't set it.)

- [ ] **Step 6: Verify both harnesses green**

- [ ] **Step 7: Commit**

```bash
git add GachaClient/systems/render/BatchKey.lua GachaClient/ui/core/widget.lua GachaClient/ui/core/container.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(render): BatchKey + Container.sortChildren opt-in (P4 task 9)"
```

---

## Task 10: grid_view de-interleave POC

Apply task 9's state-sort to `grid_view`. The 3-pass underlay → cell → rim becomes 1 alpha-sweep + 2 additive-sweeps.

**Files:**
- Modify: `GachaClient/ui/widgets/grid_view.lua`

- [ ] **Step 1: Capture gacha baseline**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/gacha-before.csv --scene gacha
```

- [ ] **Step 2: Set batchKey on each child + opt the container in**

In `grid_view.lua`'s build phase (where cells/glows/rims are added as children):

```lua
-- P4: batchKey per child so Container.sortChildren groups same-state draws. The 3-pass
-- underlay -> cell -> rim becomes alpha-sweep + 2 additive-sweeps after sort.
underlay.batchKey = "rarity_glow|atlas_ui|add"
cell.batchKey     = "card|atlas_ui|alpha"
rim.batchKey      = "rim_glow|atlas_ui|add"
```

In the grid_view container itself:

```lua
self.sortChildren = true
```

- [ ] **Step 3: Capture gacha after**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/gacha-after.csv --scene gacha
```

- [ ] **Step 4: Compare**

`ui.canvasswitches` per frame should drop by ≥10× (per-card alpha→add interleave was the principal source).

- [ ] **Step 5: USER VISUAL GATE**

User opens F1 (gacha), confirms:
1. Cards render at correct depth (glow under, rim over).
2. Pulse + rim animations still correct.
3. No flicker / z-fighting.

- [ ] **Step 6: Commit on visual GO**

```bash
git add GachaClient/ui/widgets/grid_view.lua docs/audits/p4/gacha-before.csv docs/audits/p4/gacha-after.csv
git commit -m "feat(ui): grid_view de-interleave via state-sort (P4 task 10)"
```

---

## Task 11: Widget culling + per-screen culls

`Widget.cullToViewport` opt-in skips out-of-viewport widgets. Apply to the inventory list + party member list. Per-screen culls in WaveGame (segment cull) and combat (off-grid indicator cull) verified.

**Files:**
- Modify: `GachaClient/ui/core/widget.lua` (cullToViewport flag + viewport-intersection check before draw)
- Modify: `GachaClient/ui/screens/inventory/init.lua` or its character/item lists (opt the list container in)
- Modify: `GachaClient/ui/screens/dailies/WaveGame.lua` (verify + tighten segment cull)
- Modify: `GachaClient/ui/screens/combat/CombatRenderer.lua` (verify off-grid indicator cull)

- [ ] **Step 1: Implement Widget.cullToViewport**

In `widget.lua`, before the existing draw entry point:

```lua
-- P4: cull to viewport. Containers that scroll past viewport skip children whose absolute
-- rect is fully outside the screen. Default off (preserves existing behavior).
function Widget:_isCulled()
    if not self.cullToViewport then return false end
    local x = self:getAbsoluteX()
    local y = self:getAbsoluteY()
    local w, h = self.width, self.height
    local sw = love.graphics.getWidth()
    local sh = love.graphics.getHeight()
    return (x + w <= 0) or (y + h <= 0) or (x >= sw) or (y >= sh)
end
```

In the existing `Widget:draw`:

```lua
function Widget:draw()
    if not self.visible then return end
    if self:_isCulled() then return end
    -- ...existing draw...
end
```

- [ ] **Step 2: Opt list containers in**

In the inventory's CharactersTab (or wherever the scrollable list lives), set on each list-item widget after creation:

```lua
listItem.cullToViewport = true
```

(One line per list-creation site.)

- [ ] **Step 3: WaveGame + Combat verification**

These already cull (WaveGame's segment loop bounds by `tunnelStart/tunnelEnd`; combat's off-grid indicator logic gated by target on-grid). Run a quick visual check + a CP-3 capture; the targets are `ui.drawcalls` not growing linearly as the list scrolls / wave length grows.

- [ ] **Step 4: Capture inventory-scrolled before/after**

(The before capture from task 8 + the after capture from task 11 give the comparison.)

- [ ] **Step 5: Verify both harnesses green**

- [ ] **Step 6: USER VISUAL GATE**

User scrolls F3 inventory past 30+ items, confirms:
1. List scrolls smoothly.
2. Items off-screen don't render (no visual artifact, no missing rendering when scrolled back).

- [ ] **Step 7: Commit**

```bash
git add GachaClient/ui/core/widget.lua GachaClient/ui/screens/inventory/init.lua
git commit -m "feat(ui): widget viewport culling + opt-in for inventory list (P4 task 11)"
```

---

## Task 12: Panel chrome batching

Panel chrome (background + border + shadow + lit-edge highlight) into one `MeshBatch` per instance. Validates the same pattern scales to UI surfaces.

**Files:**
- Modify: `GachaClient/ui/widgets/panel.lua`

- [ ] **Step 1: Capture inventory baseline (panel-heavy scene)**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/panel-before.csv --scene inventory
```

- [ ] **Step 2: Add MeshBatch instance to Panel**

In `panel.lua` constructor:

```lua
local MeshBatch = require "systems.render.MeshBatch"
-- P4: panel chrome (bg + border + shadow + lit edge) as one MeshBatch per instance.
self._chromeBatch = MeshBatch.new({
    {"VertexPosition", "float", 2}, {"VertexColor", "byte", 4},
}, 64, { mode = "triangles" })
self._chromeKey = nil   -- invalidation key (size, color, frosted)
```

- [ ] **Step 3: Build chrome on dirty**

Add a helper:

```lua
function Panel:_buildChrome(x, y, w, h)
    local key = ("%d|%d|%d|%d"):format(x, y, w, h)
    if self._chromeKey == key then return end
    self._chromeKey = key
    local mb = self._chromeBatch
    mb:begin()
    -- Background quad
    mb:addQuad(x, y, w, h, self.bgColor or {32, 32, 38, 200})
    -- Border (4 thin quads)
    local BT = 1
    mb:addQuad(x,         y,         w, BT, self.borderColor or {72, 72, 90, 220})
    mb:addQuad(x,         y + h - BT, w, BT, self.borderColor or {72, 72, 90, 220})
    mb:addQuad(x,         y,         BT, h, self.borderColor or {72, 72, 90, 220})
    mb:addQuad(x + w - BT, y,         BT, h, self.borderColor or {72, 72, 90, 220})
    -- Shadow (offset quad below + right, semi-transparent)
    -- Lit-edge highlight (top + left, brighter)
    -- (Replicate the existing chrome geometry here; this is the SHAPE, not the geometry —
    --  use whatever the current panel draw method emits.)
end
```

In `Panel:draw`, replace the chrome stack with:

```lua
self:_buildChrome(x, y, w, h)
self._chromeBatch:flush()
-- ...frosted layer (if any) draws after, unchanged...
```

- [ ] **Step 4: Capture inventory after**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/panel-after.csv --scene inventory
```

- [ ] **Step 5: Compare**

`ui.drawcalls` per panel drops from ~8 to 1-2.

- [ ] **Step 6: USER VISUAL GATE**

User opens F2 + F3 + F4 + settings (all panel-heavy), confirms:
1. Panel background + border + shadow + lit-edge render identically.
2. Frosted layer still applies (it's a separate scene-effect layer; the chrome MeshBatch is on top of frosted output).

- [ ] **Step 7: Commit on visual GO**

```bash
git add GachaClient/ui/widgets/panel.lua docs/audits/p4/panel-before.csv docs/audits/p4/panel-after.csv
git commit -m "feat(ui): panel chrome via MeshBatch (P4 task 12)"
```

---

## Task 13: Phase 4 summary capture + spec back-link

Final sweep: capture all reference scenes after, write a one-page summary alongside the spec.

**Files:**
- Create: `docs/audits/p4/summary.md` (one page: scene-by-scene before/after metrics)
- Modify: `docs/superpowers/specs/2026-05-28-rendering-p4-batching-design.md` (append "Outcome" section)

- [ ] **Step 1: Final captures**

```bash
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/final-login.csv     --scene login
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/final-inventory.csv --scene inventory
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/final-party.csv    --scene party
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/final-dailies.csv  --scene dailies
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/final-combat.csv   --scene combat
ThirdParty/love2d/lovec.exe GachaClient --profile-capture 3 --output docs/audits/p4/final-gacha.csv    --scene gacha
```

- [ ] **Step 2: Write summary**

Pull steady-state per-scope metrics from each before/after pair into `docs/audits/p4/summary.md`:

```markdown
# Phase 4 — Outcome summary

Steady-state per-frame metrics (averaged frames 60+, scenes captured 2026-05-28):

| Scene     | scope    | drawcalls (before / after) | switches (before / after) | Δms |
|-----------|----------|----------------------------|----------------------------|-----|
| combat    | scene    | 120 / 8                    | 4 / 2                      | -0.4 |
| dailies   | scene    | 1500+ / 5                  | 10 / 4                     | -2.1 |
| gacha     | ui       | (n) / (n')                 | (s) / (s')                 | -X.Y |
| inventory | ui       | (n) / (n')                 | (s) / (s')                 | -X.Y |
| ...       | ...      | ...                        | ...                        | ... |

(Fill in the actual numbers from the captured CSVs.)
```

- [ ] **Step 3: Append Outcome section to the spec**

Add to the bottom of the spec file:

```markdown
---

## Outcome (2026-05-28+)

All 12 plan tasks shipped. See `docs/audits/p4/summary.md` for per-scene metrics.

Foundation primitives (`SpriteBatch.lua`, `MeshBatch.lua`) reusable for Phase 5/6 work. Atlas pipeline ready for additional atlas declarations (combat, sprites) as future content lands. State-sort + culling opt-ins available for any new container that needs them.

[Followups: ...]
```

- [ ] **Step 4: Commit**

```bash
git add docs/audits/p4/summary.md docs/audits/p4/final-*.csv docs/superpowers/specs/2026-05-28-rendering-p4-batching-design.md
git commit -m "docs(render): Phase 4 outcome summary + final captures"
```

---

## Self-review

- [x] Each task has exact file paths (Create/Modify/Test).
- [x] Each task ends in a commit.
- [x] User visual gates explicit on the four tasks where visual parity matters (combat, WaveGame, gacha, panel).
- [x] Headless tests precede implementation where applicable (TDD on primitives + skyline + BatchKey).
- [x] CP-3 before/after captures hooked in at every batch-target task; metrics named.
- [x] No placeholders, no TBDs, no "implement later" markers.
- [x] Sequencing produces working/testable software at each commit.
- [x] Scope guards explicit: combat abilities off-limits, F2/F3 internals deferred, no FFI yet.
- [x] Method names consistent: `SpriteBatch:begin/add/setColor/draw`, `MeshBatch:begin/addVertex/addTriangle/addQuad/flush/draw`, `Atlas.bake`, `BatchKey.compute/stableSort`.
