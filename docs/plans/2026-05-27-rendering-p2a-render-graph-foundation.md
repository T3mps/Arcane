# Rendering Phase 2a — Render-Graph Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the render-graph skeleton (`lib/Pool` → `RenderTargets` → `Pass` → `Renderer`) and route `love.draw` through it via a single output-identical `legacy` pass that is now profiled — the safe, parity-gated bedrock the rest of the rendering work peels off of.

**Architecture:** A generic keyed free-list (`lib/Pool`) backs a pooled render-target manager (`RenderTargets`, injectable canvas factory so its bookkeeping is headless-testable). A `Pass` is plain data + a `draw(ctx)` fn. `Renderer` holds an ordered pass list and executes each inside a profiler scope, passing a shared frame `ctx`. `love.draw` becomes `Renderer.frame()` with one `legacy` pass that runs today's exact draw sequence — identical pixels, now a graph.

**Tech Stack:** LÖVE 11.x / LuaJIT. Headless unit tests via `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` (created in Phase 1). Depends on Phase 1's `systems/render/Profiler` (committed).

**Spec:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` (Phase 2). **Scope of THIS plan (2a):** Pool, RenderTargets, Pass, Renderer, legacy-pass migration. **Deferred to Phase 2b:** linear/HDR/tonemap color pipeline; peeling `present`/`post`/`scene` out of the legacy pass.

---

## File structure

| File | Responsibility |
|---|---|
| `GachaClient/lib/Pool.lua` | Generic keyed acquire/release free-list with per-frame reset. Render-agnostic. |
| `GachaClient/systems/render/RenderTargets.lua` | Pooled canvas manager over `lib/Pool` (key = `w,h,format,msaa`); transient (frame-released) vs persistent; injectable canvas factory for tests. |
| `GachaClient/systems/render/Pass.lua` | Pass descriptor constructor (`name, inputs, outputs, enabled, draw, budgetMs`). |
| `GachaClient/systems/render/Renderer.lua` | Ordered pass list; `frame(ctx)` executes enabled passes inside profiler scopes; `add/get/setEnabled`. |
| `GachaClient/main.lua` | Modify: build the Renderer with one `legacy` pass wrapping the current `love.draw` body; call `Renderer:frame(ctx)` from `love.draw`. |
| `GachaClient/tests/render_harness/main.lua` | Modify: add unit sections for Pool, RenderTargets (fake factory), Renderer (fake passes/profiler). |

Run command (from repo root `D:/dev/starworks/Gacha`): `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` → expect `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

---

### Task 1: `lib/Pool.lua`

**Files:** Create `GachaClient/lib/Pool.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line of `main.lua`:
```lua
print("== Pool ==")
do
    local Pool = require "lib.Pool"
    local made = 0
    local p = Pool.new(function(key) made = made + 1; return { key = key, id = made } end)
    local a = p:acquire("k1"); eq(a.id, 1, "first make")
    local b = p:acquire("k1"); eq(b.id, 2, "second distinct (a still out)")
    p:release(a)                                   -- a back to the k1 free list
    local c = p:acquire("k1"); eq(c.id, 1, "reused released a")
    eq(made, 2, "no extra creation on reuse")
    local d = p:acquire("k2"); eq(d.key, "k2", "different key new bucket"); eq(made, 3, "k2 created")
    -- frameReset() releases everything acquired-and-not-released this frame.
    p:frameReset()
    local e = p:acquire("k1"); eq(made, 3, "frameReset returned live ones to pool (reused)")
end
```

- [ ] **Step 2: Run → FAIL** (`module 'lib.Pool' not found`).
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 3: Implement `GachaClient/lib/Pool.lua`:**
```lua
-- lib/Pool.lua
-- Generic keyed object pool / free-list. acquire(key) reuses a free object for that key or
-- creates one via the factory; release(obj) returns it to its key's free list; frameReset()
-- returns everything still checked out (handy for per-frame transient resources). Render-
-- agnostic; RenderTargets specializes it for canvases. FFI-neutral (plain tables/arrays).
local Pool = {}
Pool.__index = Pool

-- factory(key) -> object. Objects carry a private _poolKey for release/reset bookkeeping.
function Pool.new(factory)
    assert(type(factory) == "function", "Pool.new requires a factory function")
    return setmetatable({ factory = factory, free = {}, live = {} }, Pool)
end

function Pool:acquire(key)
    local bucket = self.free[key]
    local obj
    if bucket and #bucket > 0 then
        obj = bucket[#bucket]; bucket[#bucket] = nil
    else
        obj = self.factory(key)
    end
    obj._poolKey = key
    self.live[obj] = key
    return obj
end

function Pool:release(obj)
    local key = self.live[obj]
    if not key then return end
    self.live[obj] = nil
    local bucket = self.free[key]; if not bucket then bucket = {}; self.free[key] = bucket end
    bucket[#bucket + 1] = obj
end

-- Return every still-checked-out object to its free list (call at end of frame for transients).
function Pool:frameReset()
    for obj in pairs(self.live) do
        local key = self.live[obj]
        local bucket = self.free[key]; if not bucket then bucket = {}; self.free[key] = bucket end
        bucket[#bucket + 1] = obj
    end
    self.live = {}
end

function Pool:liveCount() local n = 0; for _ in pairs(self.live) do n = n + 1 end; return n end

return Pool
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0).

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/lib/Pool.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): generic keyed object Pool primitive"
```

---

### Task 2: `systems/render/RenderTargets.lua`

The canvas pool. Its *bookkeeping* is unit-tested with an **injected fake canvas factory** (no GL); real canvas creation only happens in-game.

**Files:** Create `GachaClient/systems/render/RenderTargets.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== RenderTargets ==")
do
    local RT = require "systems.render.RenderTargets"
    local created = {}
    -- Inject a fake canvas factory so we don't need love.graphics.
    local rt = RT.new({ factory = function(w, h, fmt, msaa)
        local c = { w = w, h = h, fmt = fmt, msaa = msaa }
        created[#created + 1] = c
        return c
    end })
    local a = rt:acquire(100, 50)                 -- default format/msaa
    eq(a.w, 100, "w"); eq(a.h, 50, "h")
    eq(#created, 1, "one created")
    rt:frameReset()                               -- transient release
    local b = rt:acquire(100, 50)
    eq(#created, 1, "reused same-key canvas after frameReset")
    local c = rt:acquire(100, 50, "rgba16f")      -- different format -> new bucket
    eq(#created, 2, "format change creates a new canvas")
    -- msaa=0 is normalized to nil-equivalent key (shader-sampled targets are single-sample).
    local d = rt:acquire(100, 50, nil, 0)
    eq(d.w, 100, "msaa0 ok")
end
```

- [ ] **Step 2: Run → FAIL** (`module 'systems.render.RenderTargets' not found`).

- [ ] **Step 3: Implement `GachaClient/systems/render/RenderTargets.lua`:**
```lua
-- systems/render/RenderTargets.lua
-- Pooled canvas manager over lib/Pool. acquire(w,h,format,msaa) returns a reused canvas keyed
-- by its dimensions/format/msaa; frameReset() returns transient targets to the pool each frame.
-- Policy lives here: shader-sampled targets are single-sample (msaa<2 -> none); the only
-- multisampled target is the dedicated full-frame AA canvas (acquired with msaa>=2 explicitly).
-- A canvas factory is injectable so the keying/reuse bookkeeping is unit-testable headless;
-- in-game it defaults to love.graphics.newCanvas.
local Pool = require "lib.Pool"

local RenderTargets = {}
RenderTargets.__index = RenderTargets

local function defaultFactory(w, h, fmt, msaa)
    local opts = {}
    if fmt then opts.format = fmt end
    if msaa and msaa >= 2 then opts.msaa = msaa end
    return love.graphics.newCanvas(w, h, opts)
end

function RenderTargets.new(opts)
    opts = opts or {}
    local self = setmetatable({}, RenderTargets)
    self.makeCanvas = opts.factory or defaultFactory
    -- One Pool keyed by a string "WxH|fmt|msaa"; the factory closes over the parsed parts.
    self.pool = Pool.new(function(key)
        local w, h, fmt, msaa = self._pending.w, self._pending.h, self._pending.fmt, self._pending.msaa
        return self.makeCanvas(w, h, fmt, msaa)
    end)
    self._pending = {}
    return self
end

local function keyFor(w, h, fmt, msaa)
    msaa = (msaa and msaa >= 2) and msaa or 0
    return string.format("%dx%d|%s|%d", w, h, fmt or "rgba8", msaa)
end

-- Acquire a transient canvas (auto-released by frameReset()). fmt defaults to the backend's
-- normal 8-bit; pass "rgba16f" for HDR. msaa<2 means single-sample.
function RenderTargets:acquire(w, h, fmt, msaa)
    self._pending = { w = w, h = h, fmt = fmt, msaa = (msaa and msaa >= 2) and msaa or nil }
    return self.pool:acquire(keyFor(w, h, fmt, msaa))
end

-- Return all transient canvases to the pool (call once at end of frame).
function RenderTargets:frameReset() self.pool:frameReset() end

return RenderTargets
```

- [ ] **Step 4: Run → PASS.**

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/RenderTargets.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): pooled RenderTargets (injectable factory, single-sample policy)"
```

---

### Task 3: `Pass.lua` + `Renderer.lua` core

**Files:** Create `GachaClient/systems/render/Pass.lua`, `GachaClient/systems/render/Renderer.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Renderer ==")
do
    local Renderer = require "systems.render.Renderer"
    local Pass     = require "systems.render.Pass"
    -- Fake profiler that records scope names in order (matches Profiler:scope -> finish-closure).
    local seen = {}
    local fakeProf = { scope = function(_, name) seen[#seen + 1] = name; return function() end end }

    local log = {}
    local r = Renderer.new({ profiler = fakeProf })
    r:add(Pass.new{ name = "a", draw = function(ctx) log[#log+1] = "a:" .. ctx.dt end })
    r:add(Pass.new{ name = "b", enabled = false, draw = function() log[#log+1] = "b" end })
    r:add(Pass.new{ name = "c", draw = function() log[#log+1] = "c" end })

    r:frame({ dt = 1 })
    eq(table.concat(log, ","), "a:1,c", "runs enabled passes in order, skips disabled")
    eq(table.concat(seen, ","), "a,c", "profiler scoped each run pass by name")

    r:setEnabled("b", true)
    log = {}
    r:frame({ dt = 2 })
    eq(table.concat(log, ","), "a:2,b,c", "re-enabled pass runs")
end
```

- [ ] **Step 2: Run → FAIL** (`module 'systems.render.Renderer' not found`).

- [ ] **Step 3: Implement `GachaClient/systems/render/Pass.lua`:**
```lua
-- systems/render/Pass.lua
-- A render pass descriptor: plain data + one draw(ctx) function. No inheritance.
-- inputs/outputs are render-target names a later phase uses to bind targets; for the
-- foundation they're informational. enabled may be a boolean or a function(ctx)->boolean.
local Pass = {}

function Pass.new(opts)
    assert(type(opts) == "table" and opts.name and type(opts.draw) == "function",
        "Pass.new requires { name, draw, ... }")
    return {
        name    = opts.name,
        draw    = opts.draw,
        inputs  = opts.inputs or {},
        outputs = opts.outputs or {},
        enabled = (opts.enabled == nil) and true or opts.enabled,
        budgetMs = opts.budgetMs,
    }
end

return Pass
```

- [ ] **Step 4: Implement `GachaClient/systems/render/Renderer.lua`:**
```lua
-- systems/render/Renderer.lua
-- The frame graph: an ordered list of passes executed each frame inside profiler scopes.
-- Pragmatic 2D graph (ordered execution, not an async dependency compiler). The shared frame
-- ctx is passed to each pass; passes read inputs / write outputs via ctx.targets in later phases.
local Renderer = {}
Renderer.__index = Renderer

function Renderer.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Renderer)
    self.passes   = {}     -- ordered array
    self.byName   = {}     -- name -> pass
    self.profiler = opts.profiler   -- expects :scope(name) -> finish closure; optional
    return self
end

function Renderer:add(pass)
    self.passes[#self.passes + 1] = pass
    self.byName[pass.name] = pass
    return pass
end

function Renderer:get(name) return self.byName[name] end

function Renderer:setEnabled(name, v)
    local p = self.byName[name]; if p then p.enabled = v and true or false end
end

local function isEnabled(pass, ctx)
    local e = pass.enabled
    if type(e) == "function" then return e(ctx) end
    return e ~= false
end

-- Execute all enabled passes in order. Each pass runs inside a profiler scope named after it
-- (if a profiler was provided). Pass budgets are registered lazily with the profiler.
function Renderer:frame(ctx)
    for i = 1, #self.passes do
        local pass = self.passes[i]
        if isEnabled(pass, ctx) then
            local done
            if self.profiler then done = self.profiler:scope(pass.name) end
            pass.draw(ctx)
            if done then done() end
        end
    end
end

return Renderer
```

- [ ] **Step 5: Run → PASS.**

- [ ] **Step 6: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Pass.lua GachaClient/systems/render/Renderer.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Pass descriptor + Renderer ordered-pass executor"
```

---

### Task 4: Route `love.draw` through the Renderer via a single `legacy` pass

Output-identical migration: the current `love.draw` body becomes the `draw` fn of one `legacy` pass; `love.draw` builds the Renderer once and calls `Renderer:frame(ctx)`. Now the whole frame is one profiler scope (`legacy`) inside the graph, ready to peel apart in Phase 2b. **Verification is visual + profiler parity** (can't be headless).

**Files:** Modify `GachaClient/main.lua`.

- [ ] **Step 1: Read `GachaClient/main.lua`** — locate the `love.draw()` function (it currently opens with `local _drawScope = Profiler:scope("draw")` from Phase 1 and ends with `_drawScope(); Profiler:captureStats(); Profiler:endFrame(); Profiler:drawOverlay(8, 30)`). Identify the body BETWEEN those two (the FrameAA begin → world draw → UI.draw → transition → FrameAA.present → FPS overlay → flash sequence).

- [ ] **Step 2: Add requires + build the Renderer once** — near the top requires of `main.lua`, after the `local Profiler = require("systems.render.Profiler").default` line, add:
```lua
local Renderer = require "systems.render.Renderer"
local Pass     = require "systems.render.Pass"
local _renderer    -- built lazily on first draw (after love.load)
```

- [ ] **Step 3: Wrap the existing draw body in a `legacy` pass.** Restructure `love.draw()` so the existing body (everything that was between the Phase-1 `_drawScope` open and the `_drawScope()` close) moves verbatim into a local function `drawLegacy()`, and `love.draw` becomes:
```lua
local function drawLegacy()
    -- <<< the EXACT existing love.draw body from Phase 1 goes here UNCHANGED:
    -- FrameAA begin, world draw, UI.draw (+shake), UI.drawTransition, FrameAA.present,
    -- FPS overlay, warp flash. Do not alter its contents. >>>
end

function love.draw()
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler })
        _renderer:add(Pass.new{ name = "legacy", draw = function() drawLegacy() end })
    end
    Profiler:beginFrame()   -- NOTE: see Step 4 — beginFrame moves out of update into here? No.
    _renderer:frame({ dt = love.timer.getDelta() })
    Profiler:captureStats()
    Profiler:endFrame()
    Profiler:drawOverlay(8, 30)
end
```
**IMPORTANT do not break Phase-1 framing:** Phase 1 calls `Profiler:beginFrame()` in `love.update` and `Profiler:endFrame()` at the end of `love.draw`. Keep `beginFrame` in `love.update` exactly as it is — do **not** add a second `beginFrame` in `love.draw`. So the `love.draw` above must **omit** the `Profiler:beginFrame()` line (it's already called in update). The correct `love.draw`:
```lua
function love.draw()
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler })
        _renderer:add(Pass.new{ name = "legacy", draw = function() drawLegacy() end })
    end
    _renderer:frame({ dt = love.timer.getDelta() })
    Profiler:captureStats()
    Profiler:endFrame()
    Profiler:drawOverlay(8, 30)
end
```
The previous `local _drawScope = Profiler:scope("draw")` / `_drawScope()` pair is **removed** — the Renderer now creates the scope (named `legacy`) around the pass. (Update the Phase-1 budget key if you want: `Profiler:setBudget("draw", 8.0)` in `love.load` may be renamed to `"legacy"`, or left — a budget for a non-existent scope is simply never flagged. Renaming it to `"legacy"` is cleaner; do that.)

- [ ] **Step 4: Update the budget key** — in `love.load`, change `Profiler:setBudget("draw", 8.0)` to `Profiler:setBudget("legacy", 8.0)`.

- [ ] **Step 5: Verify in-game (visual + profiler parity).**
Run: `GachaClient/run.bat`.
Expected: the game looks **identical** to before (same menus, overworld, combat, pull reveal). Press **F9**: the profiler now shows a `legacy` row (replacing the old `draw` row) whose ms ≈ the previous `draw` ms — i.e. no regression. No crash. Check a menu, the overworld, and the pull reveal still render correctly.

- [ ] **Step 6: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): route love.draw through Renderer via a single legacy pass"
```

---

## Self-review (completed)

- **Spec coverage (2a):** `lib/Pool` → T1; `RenderTargets` pool + single-sample policy + injectable factory → T2 (full HDR-format *use* is wired in 2b but the `rgba16f` key path is implemented/tested here); `Pass` + `Renderer` ordered execution + profiler scoping → T3; `love.draw → Renderer.frame` legacy-pass migration → T4. Deferred-to-2b items (linear/HDR/tonemap pipeline; peeling present/post/scene) are explicitly out of scope and labeled. Covered.
- **Placeholders:** none — every code step is complete; the one verbatim-move (T4 `drawLegacy`) explicitly says move the existing body unchanged rather than re-pasting the whole Phase-1 draw body (which the engineer has in-file).
- **Type/name consistency:** `Pool.new/:acquire/:release/:frameReset/:liveCount`; `RenderTargets.new{factory}/:acquire(w,h,fmt,msaa)/:frameReset`; `Pass.new{name,draw,inputs,outputs,enabled,budgetMs}`; `Renderer.new{profiler}/:add/:get/:setEnabled/:frame(ctx)` — used identically across tasks. Profiler `:scope(name)->finish-closure` matches the Phase-1 API. The `draw`→`legacy` budget rename is applied in both T4 step 3 and step 4.

## Next: Phase 2b

`docs/superpowers/plans/2026-05-27-rendering-p2b-*.md`: introduce the linear/HDR `rgba16f` scene target + `tonemap` pass, then peel `present` (FrameAA resolve + RenderScale upscale + FXAA), `post` (PostFx), and `scene` out of the `legacy` pass one at a time, each gated on visual + profiler parity. Then Phase 3 (caching) becomes trivial because the scene target already exists.
