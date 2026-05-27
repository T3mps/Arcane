# Rendering Phase 1 — Profiler & Instrumentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a scoped CPU profiler + draw-stats + on-screen overlay so every later rendering phase is optimized against real numbers (and regressions self-report), with zero per-frame allocation when disabled.

**Architecture:** Two render-agnostic primitives in `lib/` (`RingBuffer`, `ScopedTimer`) with an injectable clock for deterministic headless tests; a `systems/render/Profiler` instance that accumulates per-scope time per frame, keeps rolling history, enforces per-scope budgets, and detects frame spikes; a LÖVE overlay; wiring into `main.lua`'s `love.update`/`love.draw` and a Settings toggle. GPU timing is CPU-submission time + `love.graphics.getStats()` for now; a true FFI `GL_TIME_ELAPSED` backend is a later add (out of scope here).

**Tech Stack:** LÖVE 11.x / LuaJIT. Tests run headless via the bundled `ThirdParty/love2d/lovec.exe` against a `tests/render_harness` project (same pattern as the existing `tests/assets_harness`).

**Spec:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` (Phase 1).

---

## File structure

| File | Responsibility |
|---|---|
| `GachaClient/lib/RingBuffer.lua` | Fixed-capacity circular buffer over a preallocated array (rolling windows). Render-agnostic. |
| `GachaClient/lib/ScopedTimer.lua` | Nestable time-span measurement with an injectable clock. Render-agnostic. |
| `GachaClient/systems/render/Profiler.lua` | Per-scope frame accumulation + history + budgets + spike detection (instance + `.default` singleton); LÖVE overlay + `getStats` capture. |
| `GachaClient/tests/render_harness/conf.lua` | Headless LÖVE config for the unit harness. |
| `GachaClient/tests/render_harness/main.lua` | Runs the pure-logic unit tests for the above and exits non-zero on failure. |
| `GachaClient/main.lua` | Modify: wrap `love.update` sub-steps + `love.draw` frame in profiler scopes; draw the overlay; toggle key. |
| `GachaClient/services/Settings.lua` | Modify: add `showProfiler` setting (+ getter); Settings UI row optional, not in this plan. |

Conventions: harness uses an absolute client path (dev-only tool, like `tests/assets_harness`). Run command throughout:
`ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` (from repo root `D:/dev/starworks/Gacha`).

---

### Task 1: `lib/RingBuffer.lua` + harness scaffold

**Files:**
- Create: `GachaClient/tests/render_harness/conf.lua`
- Create: `GachaClient/tests/render_harness/main.lua`
- Create: `GachaClient/lib/RingBuffer.lua`

- [ ] **Step 1: Create the harness config**

`GachaClient/tests/render_harness/conf.lua`:
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

- [ ] **Step 2: Write the failing harness with RingBuffer tests**

`GachaClient/tests/render_harness/main.lua`:
```lua
-- Headless unit harness for the rendering primitives + profiler core.
-- Run: ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness
local CLIENT = "D:/dev/starworks/Gacha/GachaClient/"
package.path = CLIENT .. "?.lua;" .. CLIENT .. "?/init.lua;" .. package.path

local fails = 0
local function fail(m) fails = fails + 1; print("  FAIL: " .. m) end
local function eq(a, b, what)
    if a ~= b then fail((what or "eq") .. " expected " .. tostring(b) .. " got " .. tostring(a)) end
end
local function approx(a, b, what)
    if math.abs(a - b) > 1e-9 then fail((what or "approx") .. " expected " .. tostring(b) .. " got " .. tostring(a)) end
end

print("== RingBuffer ==")
do
    local RB = require "lib.RingBuffer"
    local r = RB.new(3)
    eq(r:len(), 0, "empty len")
    eq(r:last(), nil, "empty last")
    eq(r:avg(), 0, "empty avg")
    r:push(1); r:push(2)
    eq(r:len(), 2, "len after 2")
    eq(r:last(), 2, "last after 2")
    approx(r:avg(), 1.5, "avg after 2")
    eq(r:at(1), 1, "oldest")
    r:push(3); r:push(4)   -- overwrites the 1
    eq(r:len(), 3, "len capped")
    eq(r:last(), 4, "last after wrap")
    eq(r:at(1), 2, "oldest after wrap")
    eq(r:at(3), 4, "newest via at")
    approx(r:avg(), 3, "avg after wrap")
    eq(r:max(), 4, "max")
    r:clear(); eq(r:len(), 0, "len after clear")
end

if fails == 0 then print("\nALL RENDER HARNESS CHECKS PASSED") else print(("\n%d FAILED"):format(fails)) end
os.exit(fails == 0 and 0 or 1)
```

- [ ] **Step 3: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — error like `module 'lib.RingBuffer' not found` (non-zero exit).

- [ ] **Step 4: Implement `lib/RingBuffer.lua`**

`GachaClient/lib/RingBuffer.lua`:
```lua
-- lib/RingBuffer.lua
-- Fixed-capacity circular buffer over a preallocated array; overwrites the oldest when full.
-- Render-agnostic (profiler history, fps smoothing). Dense numeric array + head/count, so it
-- can move to ffi.new during the planned FFI pass without an API change.
local RingBuffer = {}
RingBuffer.__index = RingBuffer

function RingBuffer.new(capacity)
    assert(type(capacity) == "number" and capacity >= 1, "capacity must be >= 1")
    local self = setmetatable({}, RingBuffer)
    self.capacity = math.floor(capacity)
    self.buf = {}
    for i = 1, self.capacity do self.buf[i] = 0 end
    self.head = 0      -- index of the most recently pushed element (0 = empty)
    self.count = 0
    return self
end

function RingBuffer:push(v)
    self.head = (self.head % self.capacity) + 1
    self.buf[self.head] = v
    if self.count < self.capacity then self.count = self.count + 1 end
    return v
end

function RingBuffer:len() return self.count end

function RingBuffer:last()
    if self.count == 0 then return nil end
    return self.buf[self.head]
end

-- i = 1 oldest retained, i = count newest.
function RingBuffer:at(i)
    if i < 1 or i > self.count then return nil end
    local oldest = (self.head - self.count + self.capacity) % self.capacity + 1
    local idx = (oldest - 1 + (i - 1)) % self.capacity + 1
    return self.buf[idx]
end

-- Order-independent aggregates over the retained elements (before wrap they occupy buf[1..count]).
function RingBuffer:avg()
    if self.count == 0 then return 0 end
    local n = (self.count == self.capacity) and self.capacity or self.count
    local s = 0
    for i = 1, n do s = s + self.buf[i] end
    return s / n
end

function RingBuffer:max()
    if self.count == 0 then return 0 end
    local n = (self.count == self.capacity) and self.capacity or self.count
    local m = self.buf[1]
    for i = 2, n do if self.buf[i] > m then m = self.buf[i] end end
    return m
end

function RingBuffer:clear() self.head = 0; self.count = 0 end

return RingBuffer
```

- [ ] **Step 5: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS — prints `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

- [ ] **Step 6: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/lib/RingBuffer.lua GachaClient/tests/render_harness
git -C D:/dev/starworks/Gacha commit -m "feat(render): RingBuffer primitive + render unit harness"
```

---

### Task 2: `lib/ScopedTimer.lua`

**Files:**
- Create: `GachaClient/lib/ScopedTimer.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (add a test section)

- [ ] **Step 1: Add the failing test section**

Insert into `GachaClient/tests/render_harness/main.lua` immediately before the final `if fails == 0` line:
```lua
print("== ScopedTimer ==")
do
    local ST = require "lib.ScopedTimer"
    local t = 0
    local st = ST.new(function() return t end)   -- injected fake clock
    st:begin("outer"); t = t + 0.010
    st:begin("inner"); t = t + 0.003
    local n1, d1, depth1 = st:finish()            -- close inner
    eq(n1, "inner", "inner name")
    approx(d1, 0.003, "inner dur")
    eq(depth1, 1, "depth after inner pop")
    local n2, d2, depth2 = st:finish()            -- close outer
    eq(n2, "outer", "outer name")
    approx(d2, 0.013, "outer dur")
    eq(depth2, 0, "depth after outer pop")
    eq(st:finish(), nil, "finish on empty returns nil")
end
```

- [ ] **Step 2: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — `module 'lib.ScopedTimer' not found`.

- [ ] **Step 3: Implement `lib/ScopedTimer.lua`**

`GachaClient/lib/ScopedTimer.lua`:
```lua
-- lib/ScopedTimer.lua
-- Nestable time-span measurement. Inject a clock fn for deterministic tests; defaults to
-- love.timer.getTime (referenced lazily, so requiring this module never needs LÖVE).
-- Render-agnostic; the Profiler builds per-scope aggregation on top.
local ScopedTimer = {}
ScopedTimer.__index = ScopedTimer

function ScopedTimer.new(clock)
    local self = setmetatable({}, ScopedTimer)
    self.clock = clock or function() return love.timer.getTime() end
    self.stack = {}   -- array of { name, start }
    return self
end

function ScopedTimer:begin(name)
    self.stack[#self.stack + 1] = { name = name, start = self.clock() }
end

-- Returns name, duration(seconds), depth (nesting level remaining; 0 = back at top level), or nil if empty.
function ScopedTimer:finish()
    local top = self.stack[#self.stack]
    if not top then return nil end
    self.stack[#self.stack] = nil
    return top.name, self.clock() - top.start, #self.stack
end

function ScopedTimer:depth() return #self.stack end
function ScopedTimer:reset() self.stack = {} end

return ScopedTimer
```

- [ ] **Step 4: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS, exit 0.

- [ ] **Step 5: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/lib/ScopedTimer.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): ScopedTimer primitive (injectable clock)"
```

---

### Task 3: `systems/render/Profiler.lua` core (frame accumulation, history, budgets, spikes)

**Files:**
- Create: `GachaClient/systems/render/Profiler.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (add a test section)

- [ ] **Step 1: Add the failing test section**

Insert into `GachaClient/tests/render_harness/main.lua` before the final `if fails == 0` line:
```lua
print("== Profiler core ==")
do
    local Profiler = require "systems.render.Profiler"
    local t = 0
    local p = Profiler.new({ clock = function() return t end, history = 8, enabled = true, spikeFactor = 2.0 })
    p:setBudget("scene", 5.0)   -- ms

    -- Frame 1: scene takes 3ms, ui 1ms.
    p:beginFrame()
    local d = p:scope("scene"); t = t + 0.003; d()
    d = p:scope("ui"); t = t + 0.001; d()
    t = t + 0.0005   -- a little untimed frame overhead
    p:endFrame()

    local r = p:report()
    approx(r.scopes[1].lastMs, 3.0, "scene lastMs")
    eq(r.scopes[1].name, "scene", "scope order stable")
    eq(r.scopes[1].over, false, "scene under budget")
    eq(r.spike, false, "no spike with <10 frames history")

    -- Disabled profiler: scope() is a no-op closure, nothing recorded.
    local p2 = Profiler.new({ clock = function() return t end, enabled = false })
    p2:beginFrame()
    local d2 = p2:scope("x"); d2()
    p2:endFrame()
    eq(#p2:report().scopes, 0, "disabled records nothing")

    -- Budget breach flags `over`.
    local t3 = 0
    local p3 = Profiler.new({ clock = function() return t3 end, enabled = true })
    p3:setBudget("heavy", 2.0)
    p3:beginFrame()
    local d3 = p3:scope("heavy"); t3 = t3 + 0.005; d3()   -- 5ms > 2ms
    p3:endFrame()
    eq(p3:report().scopes[1].over, true, "budget breach flagged")
end
```

- [ ] **Step 2: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — `module 'systems.render.Profiler' not found`.

- [ ] **Step 3: Implement `systems/render/Profiler.lua`**

`GachaClient/systems/render/Profiler.lua`:
```lua
-- systems/render/Profiler.lua
-- Scoped CPU profiler: per-frame per-scope accumulation, rolling history, per-scope budgets,
-- and frame-spike detection. Returns the class (with .new) plus a shared `.default` instance.
-- Zero per-frame allocation in the hot path when disabled (scope() returns a shared no-op).
-- GPU time here is CPU-submission time; a true FFI GL_TIME_ELAPSED backend is a later add.
local RingBuffer  = require "lib.RingBuffer"
local ScopedTimer = require "lib.ScopedTimer"

local Profiler = {}
Profiler.__index = Profiler

local NOOP = function() end

function Profiler.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Profiler)
    self.clock       = opts.clock or function() return love.timer.getTime() end
    self.timer       = ScopedTimer.new(self.clock)
    self.enabled     = opts.enabled or false
    self.history     = opts.history or 120
    self.spikeFactor = opts.spikeFactor or 2.0
    self.scopes      = {}     -- name -> { buf, accum, calls, budgetMs, lastMs, over }
    self.order       = {}     -- stable display order of scope names
    self.frameBuf    = RingBuffer.new(self.history)
    self.frameStart  = 0
    self.frameMs     = 0
    self.spikeFrame  = false
    self.stats       = nil
    return self
end

local function ensureScope(self, name)
    local s = self.scopes[name]
    if not s then
        s = { buf = RingBuffer.new(self.history), accum = 0, calls = 0,
              budgetMs = nil, lastMs = 0, over = false }
        self.scopes[name] = s
        self.order[#self.order + 1] = name
    end
    return s
end

function Profiler:setBudget(name, ms) ensureScope(self, name).budgetMs = ms end
function Profiler:setEnabled(v) self.enabled = v and true or false end
function Profiler:toggle() self.enabled = not self.enabled end
function Profiler:isEnabled() return self.enabled end

function Profiler:beginFrame()
    if not self.enabled then return end
    self.frameStart = self.clock()
    for i = 1, #self.order do
        local s = self.scopes[self.order[i]]
        s.accum = 0; s.calls = 0
    end
    self.spikeFrame = false
end

-- Returns a finish closure: local done = prof:scope("name"); ...work...; done()
function Profiler:scope(name)
    if not self.enabled then return NOOP end
    self.timer:begin(name)
    local s = ensureScope(self, name)
    return function()
        local _, dur = self.timer:finish()
        if dur then s.accum = s.accum + dur * 1000; s.calls = s.calls + 1 end
    end
end

function Profiler:endFrame()
    if not self.enabled then return end
    self.frameMs = (self.clock() - self.frameStart) * 1000
    self.frameBuf:push(self.frameMs)
    for i = 1, #self.order do
        local s = self.scopes[self.order[i]]
        s.lastMs = s.accum
        s.buf:push(s.accum)
        s.over = (s.budgetMs ~= nil) and (s.accum > s.budgetMs)
    end
    local avg = self.frameBuf:avg()
    self.spikeFrame = (self.frameBuf:len() > 10) and avg > 0 and (self.frameMs > avg * self.spikeFactor)
end

-- Capture draw stats (call once per frame in LÖVE context). No-op headless.
function Profiler:captureStats()
    if not self.enabled then return end
    if love and love.graphics and love.graphics.getStats then self.stats = love.graphics.getStats() end
end

-- Snapshot for the overlay / tests.
function Profiler:report()
    local rows = {}
    for i = 1, #self.order do
        local name = self.order[i]
        local s = self.scopes[name]
        rows[i] = { name = name, lastMs = s.lastMs, avgMs = s.buf:avg(), maxMs = s.buf:max(),
                    budgetMs = s.budgetMs, over = s.over, calls = s.calls }
    end
    return { frameMs = self.frameMs, frameAvg = self.frameBuf:avg(), frameMax = self.frameBuf:max(),
             spike = self.spikeFrame, scopes = rows, stats = self.stats }
end

Profiler.default = Profiler.new()
return Profiler
```

- [ ] **Step 4: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS, exit 0.

- [ ] **Step 5: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profiler.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Profiler core (scopes, history, budgets, spike detection)"
```

---

### Task 4: Profiler overlay (LÖVE draw)

The overlay needs `love.graphics`, so it's verified by a boot smoke + visual check, not the headless harness.

**Files:**
- Modify: `GachaClient/systems/render/Profiler.lua` (add `drawOverlay`)

- [ ] **Step 1: Add `drawOverlay` to `systems/render/Profiler.lua`**

Insert this method just before the `Profiler.default = Profiler.new()` line:
```lua
-- Draw the profiler HUD at (x,y). Safe to call every frame; draws nothing when disabled.
function Profiler:drawOverlay(x, y)
    if not self.enabled or not (love and love.graphics) then return end
    x = x or 8; y = y or 30
    local lg = love.graphics
    local pad, lineH, w = 6, 14, 240
    local rows = self:report()
    local h = lineH * (#rows.scopes + 3) + pad * 2

    lg.push("all")
    lg.setColor(0, 0, 0, 0.6); lg.rectangle("fill", x, y, w, h, 4)
    lg.setColor(rows.spike and {1, 0.4, 0.4, 1} or {0.6, 0.95, 1.0, 1})
    lg.print(("frame %.2f ms (avg %.2f, max %.2f)"):format(rows.frameMs, rows.frameAvg, rows.frameMax), x + pad, y + pad)
    local ty = y + pad + lineH
    if rows.stats then
        lg.setColor(0.7, 0.7, 0.75, 1)
        lg.print(("draws %d  switches %d  vram %.1fMB"):format(
            rows.stats.drawcalls or 0, rows.stats.canvasswitches or 0,
            (rows.stats.texturememory or 0) / 1048576), x + pad, ty)
        ty = ty + lineH
    end
    for i = 1, #rows.scopes do
        local s = rows.scopes[i]
        lg.setColor(s.over and {1, 0.4, 0.4, 1} or {0.85, 0.85, 0.9, 1})
        local label = s.budgetMs and ("%s %.2f/%.1f ms"):format(s.name, s.lastMs, s.budgetMs)
                                  or  ("%s %.2f ms"):format(s.name, s.lastMs)
        lg.print(label, x + pad, ty); ty = ty + lineH
    end
    lg.pop()
end
```

- [ ] **Step 2: Boot smoke — verify the file still loads and the overlay is callable**

Add a temporary smoke line to the END of `GachaClient/tests/render_harness/main.lua` (before `os.exit`) — note `drawOverlay` early-returns headless (no `love.graphics`), so this only proves it's defined and safe:
```lua
print("== Profiler overlay (headless safety) ==")
do
    local Profiler = require "systems.render.Profiler"
    local p = Profiler.new({ enabled = true })
    local ok, err = pcall(function() p:drawOverlay(0, 0) end)   -- headless: graphics module off -> early return
    eq(ok, true, "drawOverlay safe when graphics unavailable: " .. tostring(err))
end
```
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS, exit 0.

- [ ] **Step 3: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profiler.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Profiler overlay HUD"
```

---

### Task 5: `showProfiler` setting

**Files:**
- Modify: `GachaClient/services/Settings.lua` (DEFAULTS + getter + apply branch)

- [ ] **Step 1: Add the default**

In `GachaClient/services/Settings.lua`, in the `DEFAULTS` table, add after the `showFps = false,` line:
```lua
    showProfiler   = false,        -- read by love.draw to draw the profiler overlay (F9)
```

- [ ] **Step 2: Add the getter**

In `GachaClient/services/Settings.lua`, immediately after the `Settings.fpsCap()` getter, add:
```lua
--- Whether the profiler overlay should be drawn (read live by love.draw).
function Settings.showProfiler() ensure(); return _values.showProfiler == true end
```

- [ ] **Step 3: Add the apply branch (no engine side effect; just persists)**

In `GachaClient/services/Settings.lua`, in the `apply(key)` function, add before the closing `end` of the `if/elseif` chain (next to the `fpsCap` branch):
```lua
    elseif key == "showProfiler" then
        -- read live by love.draw; nothing to apply here
```

- [ ] **Step 4: Verify Settings still loads (headless)**

Add to `GachaClient/tests/render_harness/main.lua` before `os.exit` — `Settings` reads files via `love.filesystem`, available under lovec:
```lua
print("== Settings.showProfiler ==")
do
    local chunk, err = loadfile(CLIENT .. "services/Settings.lua")
    eq(chunk ~= nil, true, "Settings parses: " .. tostring(err))
end
```
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS, exit 0.

- [ ] **Step 5: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/Settings.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(settings): showProfiler toggle"
```

---

### Task 6: Wire the profiler into the frame (`main.lua`)

Instruments the real frame: `update` sub-steps + the whole `draw`, draws the overlay, and toggles with **F9**. This is verified by launching the game (`GachaClient/run.bat`) and pressing F9 — visual, not headless.

**Files:**
- Modify: `GachaClient/main.lua` (require Profiler; scope `love.update` steps; frame-scope + overlay in `love.draw`; F9 in `love.keypressed`)

- [ ] **Step 1: Require the profiler at the top of `main.lua`**

Add alongside the other `require` lines near the top of `GachaClient/main.lua`:
```lua
local Profiler = require("systems.render.Profiler").default
```

- [ ] **Step 2: Enable the profiler from the saved setting at boot**

In `GachaClient/main.lua` `love.load()`, after `require("services.Settings").load()`, add:
```lua
Profiler.setEnabled(Profiler, require("services.Settings").showProfiler())
```
(Equivalently `Profiler:setEnabled(...)`.)

- [ ] **Step 3: Scope the update sub-steps**

In `GachaClient/main.lua` `love.update(dt)`, wrap the body in profiler frame + scopes. Replace the existing `guarded(...)` block (the `input.update` / `ui.update` / `uipreview.update` / world / network calls) with:
```lua
    Profiler:beginFrame()
    do local s = Profiler:scope("update.input");     guarded("input.update",     function() Input.update(dt) end);     s() end
    do local s = Profiler:scope("update.ui");        guarded("ui.update",        function() UI.update(dt) end);        s() end
    do local s = Profiler:scope("update.uipreview"); guarded("uipreview.update", function() UIPreview.update(dt) end); s() end

    if state.gameState == GAME_STATE.PLAYING and state.world and not UI.hasScreens() then
        local s = Profiler:scope("update.world"); guarded("world.update", function() state.world:update(dt) end); s()
    end

    if state.gameState == GAME_STATE.PLAYING then
        local s = Profiler:scope("update.network"); guarded("network.update", function() Network.update(dt) end); s()
    end
```
(Keep the existing frame-limiter block above this untouched.)

- [ ] **Step 4: Scope the draw + draw the overlay + capture stats**

In `GachaClient/main.lua` `love.draw()`, wrap the existing body in a `draw` scope and finish the frame at the end. At the very top of `love.draw()` add:
```lua
    local _drawScope = Profiler:scope("draw")
```
Then at the very end of `love.draw()` (after the warp-flash block), add:
```lua
    _drawScope()
    Profiler:captureStats()
    Profiler:endFrame()
    Profiler:drawOverlay(8, 30)
```

- [ ] **Step 5: Add the F9 toggle**

Find `love.keypressed` in `GachaClient/main.lua`. Add at the top of its body (after any existing early-outs but before other handling):
```lua
    if key == "f9" then
        Profiler:toggle()
        require("services.Settings").set("showProfiler", Profiler:isEnabled())
        return
    end
```
If `main.lua` has no `love.keypressed`, add one:
```lua
function love.keypressed(key)
    if key == "f9" then
        Profiler:toggle()
        require("services.Settings").set("showProfiler", Profiler:isEnabled())
        return
    end
    guarded("input.keypressed", function() Input.keypressed(key) end)
end
```
(If one exists, do NOT duplicate it — only insert the F9 block.)

- [ ] **Step 6: Verify in-game (visual)**

Run: `GachaClient/run.bat`
Expected: game launches; press **F9** → profiler HUD appears top-left showing `frame X.XX ms`, draw/switch/vram counts, and per-scope rows (`update.input`, `update.ui`, `draw`, …). Press F9 again → it hides. No crash, no fps regression when hidden.

- [ ] **Step 7: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): wire profiler into frame + F9 overlay toggle"
```

---

### Task 7: Seed initial per-scope budgets (optional, fast)

Gives the spike/over-budget flags something to fire against in later phases.

**Files:**
- Modify: `GachaClient/main.lua` (after `Profiler.setEnabled` in `love.load`)

- [ ] **Step 1: Set starter budgets**

In `GachaClient/main.lua` `love.load()`, after the `Profiler:setEnabled(...)` line:
```lua
-- Starter budgets (ms) — refined empirically as phases land. Hitch-free goal: sum well under
-- the frame budget; these just make over-budget/spike flags meaningful from day one.
Profiler:setBudget("draw", 8.0)
Profiler:setBudget("update.network", 1.0)
Profiler:setBudget("update.world", 3.0)
```

- [ ] **Step 2: Verify in-game**

Run: `GachaClient/run.bat`, press F9. Expected: budgets show as `draw X.XX/8.0 ms`; a row turns red if it exceeds its budget (e.g. `update.network` if a regression returns).

- [ ] **Step 3: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "chore(render): seed initial profiler budgets"
```

---

## Self-review (completed)

- **Spec coverage (Phase 1):** scoped CPU timers → Tasks 2–3,6; RingBuffer/ScopedTimer primitives → Tasks 1–2; draw stats (`getStats`) → Task 3 (`captureStats`) + Task 4 (overlay); budgets + spike detection → Task 3; overlay + toggle → Tasks 4–6; zero-cost-when-disabled (`NOOP` scope, no per-frame alloc) → Task 3 test; FFI GPU backend explicitly out of scope (stated in Goal). Covered.
- **Placeholders:** none — every code step has complete code; commands have expected output.
- **Type/name consistency:** `Profiler.new`, `:setBudget`, `:setEnabled`, `:toggle`, `:isEnabled`, `:beginFrame`, `:scope`→finish-closure, `:endFrame`, `:captureStats`, `:report`, `:drawOverlay`, `.default` are used identically across Tasks 3–7. `RingBuffer.new/:push/:len/:last/:at/:avg/:max/:clear` and `ScopedTimer.new/:begin/:finish/:depth/:reset` consistent across Tasks 1–3. `Settings.showProfiler()` matches Task 5 + Task 6.
- **Note:** the `tests/render_harness` uses an absolute client path (dev-only, matching `tests/assets_harness`) — flagged for later cleanup, not committed to the shipped game path resolution.

## Subsequent phases

P2–P6 each get their own plan (`docs/superpowers/plans/2026-05-27-rendering-p2-*.md`, etc.), written once their dependencies land — exact TDD steps for the render-graph/lighting can't be written against code that doesn't exist yet. P2 (render-graph + RT pool + color pipeline) is next after P1 ships.
