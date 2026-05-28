# Application + Bus + Service Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the 1038-line `main.lua` into a clean entry point + an `Application` class that owns LÖVE callback hosting, service lifecycle dispatch, and an event bus. Five duck-typed service hooks (init / fixedUpdate / update / draw / shutdown). Fixed-step at 60 UPS with accumulator + alpha + spiral guard. `main.lua` reaches ~40 lines.

**Architecture:** Three orthogonal types — `ServiceLocator` (existing, unchanged, name lookup), `Application` (new, lifecycle dispatcher), `Bus` (new, sync pub/sub). New service modules: `Game` (session lifecycle), `Pipeline` (render graph composition), `InputDispatch` (LÖVE input adapters). New `bootstrap` module for stdout/CrashLog/Trace setup. Existing modules (UI, Network, Jobs, Assets) slot into the App via duck-typed dispatch with zero changes.

**Tech Stack:** LÖVE 11.x, LuaJIT, existing services (Logger, CrashLog, Trace, ServiceLocator, Settings, Profiler), existing tests (assets_harness, render_harness, threading_harness).

**Spec:** `docs/superpowers/specs/2026-05-28-application-engine-design.md` (commit `50bd2aa`)

**Standing constraints:**
- Working tree dirty: targeted `git add` only — NEVER `git add -A` or `git add .`
- Never skip hooks (`--no-verify`) or bypass signing
- Combat ABILITIES gameplay off-limits
- F2/F3 inventory + party menus deferred per memory; cross-cutting App contract changes OK
- No protocol changes; no JSON schema changes
- ServiceLocator.lua is untouched (existing primitive that the App composes with)
- `state` proxy backward compat preserved through migration

**File structure (all new files except main.lua refactor):**

| File | Status | Lines (target) | Responsibility |
|---|---|---|---|
| `GachaClient/engine/bootstrap.lua` | new | ~50 | stdout/runtime.log/CrashLog/Trace before everything |
| `GachaClient/engine/Bus.lua` | new | ~30 | sync pub/sub primitive |
| `GachaClient/engine/Application.lua` | new | ~150 | lifecycle dispatcher + bindLove + accumulator |
| `GachaClient/engine/InputDispatch.lua` | new | ~180 | LÖVE input callback adapters |
| `GachaClient/systems/render/Pipeline.lua` | new | ~280 | render-graph composition |
| `GachaClient/game/Game.lua` | new | ~280 | session lifecycle |
| `GachaClient/main.lua` | refactor | ~40 (was 1038) | entry point + service registration |
| `GachaClient/tests/render_harness/main.lua` | extend | +~120 | Bus + Application test blocks |
| `GachaClient/tests/assets_harness/main.lua` | extend | +5 | syntax-check new files |
| `docs/architecture/application.md` | new | ~80 | one-pager: ServiceLocator vs App vs Bus boundaries |
| `docs/events.md` | new | ~50 | event registry (free-form bus events, names + payloads) |
| `docs/audits/app-refactor/summary.md` | new (M8) | ~100 | before/after metrics + commit SHAs |

---

## Task 1: M0a — `Bus.lua` + tests

**Files:**
- Create: `GachaClient/engine/Bus.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (new "Bus (M0)" test block)

- [ ] **Step 1: Write the failing tests**

In `tests/render_harness/main.lua`, after the existing "JobSystem (P5)" block, add:

```lua
print("== Bus (M0) ==")
do
    local Bus = require "engine.Bus"
    local bus = Bus.new()

    -- subscribe returns numeric tokens; tokens are unique
    local t1 = bus:subscribe("evt", function() end)
    local t2 = bus:subscribe("evt", function() end)
    eq(type(t1), "number", "subscribe returns numeric token")
    eq(t1 ~= t2, true, "tokens are unique")

    -- publish fires subscribers with the payload
    local got = nil
    bus:subscribe("payload_test", function(e) got = e end)
    bus:publish("payload_test", { v = 42 })
    eq(type(got), "table", "subscriber received table payload")
    eq(got.v, 42, "payload v passed through")

    -- multiple subscribers fire in subscription order
    local order = {}
    bus:subscribe("ordered", function() order[#order+1] = "a" end)
    bus:subscribe("ordered", function() order[#order+1] = "b" end)
    bus:subscribe("ordered", function() order[#order+1] = "c" end)
    bus:publish("ordered", {})
    eq(table.concat(order), "abc", "subscribers fire in subscription order")

    -- unsubscribe stops the subscriber
    local fires = 0
    local tok = bus:subscribe("unsub_test", function() fires = fires + 1 end)
    bus:publish("unsub_test", {})
    eq(fires, 1, "subscriber fired once before unsubscribe")
    bus:unsubscribe(tok)
    bus:publish("unsub_test", {})
    eq(fires, 1, "subscriber did not fire after unsubscribe")

    -- publish to unknown event is a no-op (no error)
    bus:publish("unknown_event", {})

    -- subscriber errors don't propagate; other subscribers still fire
    local goodA, goodB = false, false
    bus:subscribe("error_test", function() goodA = true end)
    bus:subscribe("error_test", function() error("intentional") end)
    bus:subscribe("error_test", function() goodB = true end)
    bus:publish("error_test", {})
    eq(goodA, true, "subscriber before erroring one still fired")
    eq(goodB, true, "subscriber after erroring one still fired")

    -- publish without payload defaults to empty table (subscriber gets a table, not nil)
    local nilCheck = "untouched"
    bus:subscribe("no_payload", function(e) nilCheck = type(e) end)
    bus:publish("no_payload")
    eq(nilCheck, "table", "publish without payload delivers empty table")

    -- unsubscribe of unknown token is a no-op
    bus:unsubscribe(99999)   -- should not error
end
```

- [ ] **Step 2: Verify failing**

Run:
```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```

Expected: FAIL with `module 'engine.Bus' not found`.

- [ ] **Step 3: Implement `Bus.lua`**

Create `GachaClient/engine/Bus.lua`:

```lua
-- engine/Bus.lua
-- Sync pub/sub for app-level events. Owned by Application; one instance per process.
-- Free-form string event names (namespaced by convention: "category.action"), single
-- table payload, synchronous dispatch in subscription order, pcall-guarded subscribers
-- (one bad subscriber does not break others), token-based unsubscribe.
--
-- NOT in scope: queued publish, priority/ordering beyond subscription order, wildcards,
-- replay/buffering, async dispatch, declared event schemas. Add only when a concrete
-- use case demands them.

local Logger = require "services.Logger"
local log = Logger.create("Bus")

local Bus = {}
Bus.__index = Bus

function Bus.new()
    return setmetatable({
        _subs      = {},     -- name -> array of {fn, token}
        _nextToken = 0,
    }, Bus)
end

function Bus:subscribe(name, fn)
    self._nextToken = self._nextToken + 1
    local token = self._nextToken
    self._subs[name] = self._subs[name] or {}
    table.insert(self._subs[name], { fn = fn, token = token })
    return token
end

function Bus:unsubscribe(token)
    for _, list in pairs(self._subs) do
        for i, sub in ipairs(list) do
            if sub.token == token then
                table.remove(list, i)
                return
            end
        end
    end
end

function Bus:publish(name, payload)
    local list = self._subs[name]
    if not list then return end
    payload = payload or {}
    for _, sub in ipairs(list) do
        local ok, err = pcall(sub.fn, payload)
        if not ok then
            log.error("Bus subscriber for '%s' errored: %s", name, tostring(err))
        end
    end
end

return Bus
```

- [ ] **Step 4: Verify tests pass**

Run:
```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```

Expected: ALL RENDER HARNESS CHECKS PASSED, new "Bus (M0)" block green.

- [ ] **Step 5: Add to assets harness syntax-check**

In `tests/assets_harness/main.lua`, find the `touched` list and add:

```lua
    "engine/Bus.lua",
```

Run:
```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/assets_harness
```

Expected: ALL ASSET HARNESS CHECKS PASSED, file count +1.

- [ ] **Step 6: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/engine/Bus.lua GachaClient/tests/render_harness/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(engine): Bus pub/sub primitive + headless tests (M0a)"
```

---

## Task 2: M0b — `Application.lua` + tests

**Files:**
- Create: `GachaClient/engine/Application.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (new "Application (M0)" test block)

- [ ] **Step 1: Write the failing tests**

In `tests/render_harness/main.lua`, after the "Bus (M0)" block, add:

```lua
print("== Application (M0) ==")
do
    local Application = require "engine.Application"

    -- Construction with defaults: fixedDt = 1/60.
    local app = Application.new()
    eq(type(app.bus), "table", "Application owns a bus")
    eq(#app.services, 0, "services list starts empty")
    approx(app._fixedDt, 1/60, "default fixedDt is 1/60")

    -- Custom fixedRate.
    local app30 = Application.new({ fixedRate = 30 })
    approx(app30._fixedDt, 1/30, "custom fixedRate sets fixedDt")

    -- registerService appends in order and writes to ServiceLocator.
    local ServiceLocator = require "services.ServiceLocator"
    local svcA = { name = "a" }
    local svcB = { name = "b" }
    app:registerService("alpha", svcA)
    app:registerService("beta",  svcB)
    eq(#app.services, 2, "two services registered")
    eq(app.services[1].name, "alpha", "first registration first in list")
    eq(app.services[2].name, "beta", "second registration second in list")
    eq(ServiceLocator.get("alpha"), svcA, "ServiceLocator has alpha")
    eq(ServiceLocator.get("beta"),  svcB, "ServiceLocator has beta")

    -- Duck-typed dispatch: services without :update skipped.
    local fires = { update = {}, draw = {}, fixed = {}, init = {}, shut = {} }
    local svc1 = {
        init   = function(self, a) fires.init[#fires.init+1] = "1" end,
        update = function(self, dt) fires.update[#fires.update+1] = "1" end,
        draw   = function(self) fires.draw[#fires.draw+1] = "1" end,
        fixedUpdate = function(self, fdt) fires.fixed[#fires.fixed+1] = "1" end,
        shutdown    = function(self) fires.shut[#fires.shut+1] = "1" end,
    }
    local svc2 = {
        init = function(self, a) fires.init[#fires.init+1] = "2" end,
        -- no update, no draw, no fixedUpdate
        shutdown = function(self) fires.shut[#fires.shut+1] = "2" end,
    }
    local svc3 = {
        update = function(self, dt) fires.update[#fires.update+1] = "3" end,
        draw   = function(self) fires.draw[#fires.draw+1] = "3" end,
    }
    local app2 = Application.new()
    app2:registerService("s1", svc1)
    app2:registerService("s2", svc2)
    app2:registerService("s3", svc3)

    -- init: registration order, all services that implement it.
    app2:load()
    eq(table.concat(fires.init), "12", "init runs in registration order, only on services that have it")

    -- update with dt < fixedDt: no fixedUpdate, single update.
    app2:update(1/120)
    eq(table.concat(fires.fixed),  "",   "no fixedUpdate when accum < fixedDt")
    eq(table.concat(fires.update), "13", "update fires on s1 and s3 only, in order")
    fires.update = {}

    -- update with dt = 2*fixedDt: 2 fixedUpdates, 1 update.
    app2:update(2/60)
    eq(table.concat(fires.fixed),  "11",  "fixedUpdate fires 2x for one service")
    eq(table.concat(fires.update), "13",  "update fires once after fixedUpdate batch")
    fires.fixed = {}; fires.update = {}

    -- Spiral guard: dt = 6*fixedDt should cap at 5 fixedUpdates.
    app2:update(6/60)
    eq(#fires.fixed, 5, "spiral guard caps fixedUpdate at 5 per real frame")
    fires.fixed = {}; fires.update = {}

    -- draw: registration order.
    app2:draw()
    eq(table.concat(fires.draw), "13", "draw fires on s1 and s3 only, in order")

    -- shutdown: reverse registration order.
    app2:shutdown()
    eq(table.concat(fires.shut), "21", "shutdown fires in REVERSE registration order")
end
```

- [ ] **Step 2: Verify failing**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```

Expected: FAIL with `module 'engine.Application' not found`.

- [ ] **Step 3: Implement `Application.lua`**

Create `GachaClient/engine/Application.lua`:

```lua
-- engine/Application.lua
-- Lifecycle dispatcher. Owns the bus + an ORDERED list of registered services.
-- Dispatches five duck-typed hooks via LOVE callbacks: init / fixedUpdate / update /
-- draw / shutdown. Registration order = dispatch order. fixedUpdate runs N times per
-- real frame via an accumulator (60 UPS by default), with a spiral-of-death guard
-- (max 5 steps/frame) and an alpha residual passed to :update(dt, alpha) for interp.
--
-- Construction is split from start:
--   Application.new(opts) -- creates bus + empty service list; nothing else runs
--   app:registerService(name, svc) -- appends to list + ServiceLocator.register
--   app:bindLove() -- installs love.<callback> = function(...) app:<callback>(...) end
--   (love.load fires -> app:load() runs each service's :init(app))
--
-- See spec: docs/superpowers/specs/2026-05-28-application-engine-design.md
-- See architecture one-pager: docs/architecture/application.md

local Bus            = require "engine.Bus"
local ServiceLocator = require "services.ServiceLocator"
local Logger         = require "services.Logger"
local Profiler       = require("systems.render.Profiler").default
local Settings       = require "services.Settings"

local log = Logger.create("App")

local Application = {}
Application.__index = Application

local MAX_FIXED_STEPS_PER_FRAME = 5

function Application.new(opts)
    opts = opts or {}
    return setmetatable({
        bus           = Bus.new(),
        services      = {},                    -- ordered list: { {name, svc}, ... }
        _running      = false,
        _accum        = 0,
        _fixedDt      = 1 / (opts.fixedRate or 60),
        _frameDeadline = nil,                  -- FPS cap pacing
    }, Application)
end

function Application:registerService(name, svc)
    table.insert(self.services, { name = name, svc = svc })
    ServiceLocator.register(name, svc)
    log.debug("Registered service: %s (#%d)", name, #self.services)
end

-- ============================================================================
-- Lifecycle dispatch
-- ============================================================================

function Application:load()
    self._running = true
    for _, entry in ipairs(self.services) do
        if entry.svc.init then
            local ok, err = pcall(entry.svc.init, entry.svc, self)
            if not ok then log.error("init '%s' errored: %s", entry.name, tostring(err)) end
        end
    end
end

function Application:update(dt)
    -- FPS cap. Live-read so the setting takes effect immediately.
    local cap = Settings.fpsCap()
    if cap > 0 then
        local target = 1 / cap
        local now = love.timer.getTime()
        if self._frameDeadline then
            local sleepFor = self._frameDeadline - now
            if sleepFor > 0 then love.timer.sleep(sleepFor) end
            now = love.timer.getTime()
        end
        self._frameDeadline = now + target
    end

    -- Fixed-step accumulator + spiral guard.
    self._accum = self._accum + dt
    local fixedDt = self._fixedDt
    local steps = 0
    while self._accum >= fixedDt and steps < MAX_FIXED_STEPS_PER_FRAME do
        for _, entry in ipairs(self.services) do
            if entry.svc.fixedUpdate then
                local ok, err = pcall(entry.svc.fixedUpdate, entry.svc, fixedDt)
                if not ok then log.error("fixedUpdate '%s' errored: %s", entry.name, tostring(err)) end
            end
        end
        self._accum = self._accum - fixedDt
        steps = steps + 1
    end
    if steps >= MAX_FIXED_STEPS_PER_FRAME then
        log.warn("Spiral guard fired: dropped %.3fs of accumulator residual", self._accum)
        if Profiler and Profiler.counter then Profiler:counter("fixed.spiralGuards", 1) end
        self._accum = 0
    end

    -- Variable update with interpolation alpha.
    local alpha = self._accum / fixedDt
    for _, entry in ipairs(self.services) do
        if entry.svc.update then
            local ok, err = pcall(entry.svc.update, entry.svc, dt, alpha)
            if not ok then log.error("update '%s' errored: %s", entry.name, tostring(err)) end
        end
    end
end

function Application:draw()
    for _, entry in ipairs(self.services) do
        if entry.svc.draw then
            local ok, err = pcall(entry.svc.draw, entry.svc)
            if not ok then log.error("draw '%s' errored: %s", entry.name, tostring(err)) end
        end
    end
end

function Application:shutdown()
    -- Reverse registration order: last registered tears down first.
    for i = #self.services, 1, -1 do
        local entry = self.services[i]
        if entry.svc.shutdown then
            local ok, err = pcall(entry.svc.shutdown, entry.svc)
            if not ok then log.error("shutdown '%s' errored: %s", entry.name, tostring(err)) end
        end
    end
    self._running = false
end

-- ============================================================================
-- LOVE input callback delegates (forwarded to InputDispatch service if registered)
-- These are installed by bindLove(). InputDispatch implements the matching methods.
-- ============================================================================

local INPUT_CALLBACKS = {
    "keypressed", "keyreleased", "textinput",
    "mousepressed", "mousereleased", "mousemoved", "wheelmoved",
    "gamepadpressed", "gamepadreleased", "gamepadaxis",
    "resize",
}

for _, cb in ipairs(INPUT_CALLBACKS) do
    Application[cb] = function(self, ...)
        local input = ServiceLocator.get("input")
        if input and input[cb] then return input[cb](input, ...) end
    end
end

function Application:quit()
    self:shutdown()
    return false   -- allow quit to proceed (LOVE convention)
end

-- ============================================================================
-- bindLove: install LOVE callback handlers
-- ============================================================================

function Application:bindLove()
    love.load   = function()       self:load() end
    love.update = function(dt)     self:update(dt) end
    love.draw   = function()       self:draw() end
    love.quit   = function()       return self:quit() end
    for _, cb in ipairs(INPUT_CALLBACKS) do
        love[cb] = function(...)   return self[cb](self, ...) end
    end
end

return Application
```

- [ ] **Step 4: Verify tests pass**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```

Expected: ALL RENDER HARNESS CHECKS PASSED, new "Application (M0)" block green.

The tests don't exercise `bindLove` because it touches the global `love` table — that's covered by engine smoke in later tasks.

- [ ] **Step 5: Add to assets harness syntax-check**

In `tests/assets_harness/main.lua` `touched` list:

```lua
    "engine/Application.lua",
```

Run assets harness, confirm green.

- [ ] **Step 6: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/engine/Application.lua GachaClient/tests/render_harness/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(engine): Application lifecycle dispatcher + headless tests (M0b)"
```

---

## Task 3: M0c — `bootstrap.lua` + architecture docs

**Files:**
- Create: `GachaClient/engine/bootstrap.lua`
- Create: `docs/architecture/application.md`
- Create: `docs/events.md`

- [ ] **Step 1: Implement `bootstrap.lua`**

Create `GachaClient/engine/bootstrap.lua`:

```lua
-- engine/bootstrap.lua
-- Before-everything-else setup. Lifted from main.lua lines 1-76. Must run before
-- anything that can error so the resulting crash report is complete.
--
-- Owns:
--   1. stdout unbuffering (so the last line before a hard native crash is never lost)
--   2. runtime.log tee on the global print() (raw print + lua tracebacks + warnings)
--   3. CrashLog.install (overrides love.errorhandler to write structured crash reports)
--   4. Trace setup (Trace.mark() breadcrumbs that survive native crashes)
--
-- DOES NOT own:
--   - Logger.enableFileOutput / setLevel (that's Application:load via Logger config)
--   - The render-graph / service-lifecycle / event-bus stuff (that's Application)

-- (1) Unbuffered stdout. lovec.exe writes to a real console; this ensures
-- the LAST line before a native crash is never buffered away.
pcall(function() io.stdout:setvbuf("no") end)

-- (2) runtime.log tee on global print(). Logger echoes structured lines via
-- io.write (bypassing this tee), so its output reaches console + client.log
-- but never duplicates into runtime.log.
local _rawPrint    = print
local _runtimeFile = nil
pcall(function()
    love.filesystem.createDirectory("logs")
    _runtimeFile = love.filesystem.newFile("logs/runtime.log", "w")
    if _runtimeFile then
        _runtimeFile:write(string.format("===== RUNTIME LOG %s =====\n", os.date("%Y-%m-%d %H:%M:%S")))
        _runtimeFile:flush()
    end
    _rawPrint(string.format("[logs] writing to %s/logs/", love.filesystem.getSaveDirectory()))
end)

_G.print = function(...)
    _rawPrint(...)
    if _runtimeFile then
        local parts = {}
        for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
        local ok = pcall(function()
            _runtimeFile:write(table.concat(parts, "\t") .. "\n")
            _runtimeFile:flush()
        end)
        if not ok then _runtimeFile = nil end   -- sink went bad; stop tee-ing
    end
end

-- (3) CrashLog: writes crash.log + GPU + traceback + Trace ring buffer.
local CrashLog = require "services.CrashLog"
CrashLog.install()

-- (4) Trace: file-output enabled later in love.load when filesystem is fully ready.
local Trace = require "services.Trace"

-- Re-export so callers that need to know the bootstrap fired can probe.
return {
    crashlog = CrashLog,
    trace    = Trace,
}
```

- [ ] **Step 2: Write the architecture one-pager**

Create `docs/architecture/application.md`:

```markdown
# Application Architecture

Three orthogonal types. Each owns ONE thing. No overlap.

## ServiceLocator (existing)

Name -> service lookup. Already in the codebase as `services/ServiceLocator.lua`.

```lua
ServiceLocator.get("network")   -- returns the Network service
```

Predates this work. The Application registers services into ServiceLocator so the
rest of the codebase can look them up by name without coupling to the App.

## Application (new)

Lifecycle dispatcher. Owns an ORDERED service list + the bus. Calls
`:init` / `:fixedUpdate` / `:update` / `:draw` / `:shutdown` in registration order.

```lua
local app = Application.new({ fixedRate = 60 })
app:registerService("network", require "network")
app:registerService("game",    Game.new())
app:bindLove()   -- installs love.load = function() app:load() end, etc.
```

Five duck-typed hooks on services (all optional):

| Hook | Called | Order |
|---|---|---|
| `init(app)` | love.load | registration order |
| `fixedUpdate(fixedDt)` | N times/frame at 60 UPS | registration order |
| `update(dt, alpha)` | once per real frame | registration order |
| `draw()` | once per real frame | registration order |
| `shutdown()` | love.quit | REVERSE registration order |

`fixedDt = 1/60` by default. Accumulator + spiral-of-death guard (max 5 fixed steps
per real frame) handle hitch recovery. `alpha = accum / fixedDt` is the interpolation
residual passed to `:update` for renderers that draw moving physics bodies between
fixed steps.

## Bus (new)

Pub/sub for observable app events. Owned by Application. Sync dispatch in subscription
order. Free-form string event names, single table payload, pcall-guarded subscribers.

```lua
local token = app.bus:subscribe("session.expired", function(e)
    log.info("Logging out: " .. e.reason)
end)
app.bus:publish("session.expired", { reason = "timeout" })
app.bus:unsubscribe(token)
```

NOT for lifecycle (that's the App's job). NOT for cross-service lookup (that's
ServiceLocator's job). NOT a schema-validated typed bus (free-form by design;
see docs/events.md for the convention).

## Where does X live?

| Need | Use |
|---|---|
| Run every frame | service hook (`:update`) |
| Run every fixed step | service hook (`:fixedUpdate`) |
| Find a service by name | `ServiceLocator.get("name")` |
| Observe an app event | `app.bus:subscribe("event.name", fn)` |
| Signal something happened | `app.bus:publish("event.name", payload)` |
| Wire dependencies between services | grab via ServiceLocator in `:init(app)` |
| New top-level subsystem | new service registered in `main.lua` |
```

- [ ] **Step 3: Write the events registry**

Create `docs/events.md`:

```markdown
# Event Registry

Bus is free-form: typos at publish/subscribe sites are silent no-ops. This file is
the human-maintained source of truth for what events exist + their payload shapes.

PR review enforces that new bus events are added here. If drift becomes painful,
upgrade to declared dev-mode validation (see spec R2).

## Convention

Event names use `category.action` namespacing. Single table payload.

## Events

### `game.*` (published by Game service)

| Event | Payload | When |
|---|---|---|
| `game.session_started` | `{ playerName: string, playerId: string }` | After successful login or auto-resume |
| `game.session_ended` | `{ reason: string }` | On logout (manual or forced) |
| `game.world_loaded` | `{}` | After loadGameWorld completes |
| `game.world_unloaded` | `{}` | On logout or world teardown |

### `app.*` (published by Application)

| Event | Payload | When |
|---|---|---|
| (none yet) | | |

(Future: `app.fixed_tick`, `app.suspended`, `app.resumed` — add when consumers
need them.)

## Legacy event surfaces (not on bus)

- **`Network.on(name, fn)`** — Network module has its own callback registration:
  reconnecting / reconnected / reconnectNeedsAuth / reconnectFailed / sessionExpired.
  Retrofit onto the bus is deferred to `[[project_ui_event_bus]]`. New subscribers
  prefer Network.on until that lands.
```

- [ ] **Step 4: Add to assets harness syntax-check**

In `tests/assets_harness/main.lua` `touched` list:

```lua
    "engine/bootstrap.lua",
```

Run assets harness, confirm green.

- [ ] **Step 5: Engine smoke**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 1 --output /tmp/m0c-smoke.csv
```

Nothing should change behaviorally — bootstrap.lua isn't required by anyone yet.

- [ ] **Step 6: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/engine/bootstrap.lua GachaClient/tests/assets_harness/main.lua docs/architecture/application.md docs/events.md
git -C D:/dev/starworks/Gacha commit -m "feat(engine): bootstrap module + architecture docs (M0c)"
```

---

## Task 4: M1 — Extract bootstrap from main.lua

**Files:**
- Modify: `GachaClient/main.lua` (replace lines 1-76 with require)

- [ ] **Step 1: Read main.lua to confirm current shape**

Read the first ~80 lines of `GachaClient/main.lua`. Confirm lines 1-76 are exactly the block being lifted (stdout unbuffering + runtime.log tee + CrashLog + Trace). If line numbers have drifted, adapt the diff below to match.

- [ ] **Step 2: Replace lines 1-76 with a single require**

In `GachaClient/main.lua`, replace the entire block from `require("boot")` through the CrashLog/Trace setup (lines 1-76) with:

```lua
require("boot")
require("engine.bootstrap")    -- stdout unbuffering, runtime.log tee, CrashLog, Trace

local sti    = require "thirdparty.sti"
local tiny   = require "thirdparty.tiny"
-- (rest of the existing requires/locals continue unchanged)
```

The `require "boot"` line stays first (it's untouched per spec). The bootstrap require runs second. All the requires that followed the lifted block continue unchanged.

- [ ] **Step 3: Engine boots cleanly**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 1 --output /tmp/m1-smoke.csv
```

Expected: ~200 frames, `[logs] writing to ...` line still appears, no crash.

- [ ] **Step 4: Crash-test the crash log**

Temporarily add `error("M1 crash test")` to the top of `love.load`. Run the engine. Verify `crash.log` is written in the save directory with the expected structured content (GPU info, traceback, Trace ring buffer). Then REMOVE the test error.

- [ ] **Step 5: Verify runtime.log still tees**

Add a temporary `print("M1 runtime.log test")` near the top of `love.load`. Run the engine briefly, then check that `runtime.log` in the save directory contains the test line. Remove the test print.

- [ ] **Step 6: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(main): extract bootstrap to engine/bootstrap.lua (M1)"
```

---

## Task 5: M2 — Stand up App alongside existing main.lua

**Files:**
- Modify: `GachaClient/main.lua` (construct Application + register into ServiceLocator)

- [ ] **Step 1: Add Application construction near the end of main.lua**

In `GachaClient/main.lua`, just BEFORE the existing `function love.quit()` line near the bottom (or before whatever the last function block is), add:

```lua
-- M2: scaffold the Application. Not bound to LOVE callbacks yet (that's M4).
-- ServiceLocator gets a reference so the rest of the codebase has a path to the bus.
local Application = require "engine.Application"
local app = Application.new({ fixedRate = 60 })
require("services.ServiceLocator").register("app", app)
```

The App exists but `app:bindLove()` is NOT called. The existing `function love.load() ... end` etc. continue to drive the engine. This is pure scaffolding — verifying that the App can be constructed in our real LOVE environment without bringing in any dispatch changes.

- [ ] **Step 2: Engine smoke**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 1 --output /tmp/m2-smoke.csv
```

Expected: ~200 frames, no behavior diff, no crash. The Logger should show `[App] Registered service: app (#0)` if the App got registered into ServiceLocator (the app registers itself into ServiceLocator, not via registerService — see step 1; this debug log may or may not appear depending on how you wrote it). At minimum, no errors.

- [ ] **Step 3: Confirm app is reachable via ServiceLocator**

Run with a debug print added temporarily:

```lua
print("[M2] app via ServiceLocator: " .. tostring(require("services.ServiceLocator").get("app")))
print("[M2] app.bus: " .. tostring(require("services.ServiceLocator").get("app").bus))
```

Drop these after confirming both lines print non-nil values. Then remove.

- [ ] **Step 4: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(engine): stand up Application alongside main.lua (M2)"
```

---

## Task 6: M3 — Migrate Network as a service

**Files:**
- Modify: `GachaClient/main.lua` (register Network as service; remove inline Network.update call)

- [ ] **Step 1: Verify Network.update signature**

Read `GachaClient/network.lua` around line 97 to confirm `Network.update(dt)` is the signature. The duck-typed dispatch in Application will call `Network:update(dt)` — Lua's colon-call passes Network as the first arg (`self`), but since Network.update declares `Network.update(dt)` (no self), the `self` arg goes nowhere harmful. This is the duck-typing the spec relies on; verify it's still true.

If Network.update has changed to accept `self`, this still works — just confirm the call shape.

- [ ] **Step 2: Register Network with the App**

In `GachaClient/main.lua`, AFTER the `local app = Application.new(...)` block, add:

```lua
app:registerService("network", require "network")
```

- [ ] **Step 3: Remove the inline Network.update call from love.update**

Find the existing `love.update(dt)` function. It currently calls Network.update directly (inside a `Profiler:scope("update.network")` block, per P5a). Remove ONLY the Network.update call + its scope wrapper. The other inline updates (UI, world, jobs) stay as-is for now.

The scope-wrapped call looks roughly like:

```lua
local s = Profiler:scope("update.network")
Network.update(dt)
if s then s() end
```

OR Network.update already wraps itself with the Profiler:scope (per P5a — see commit `32cb39a` and the spec). If Network's update is already self-scoping, the inline scope wrapper in main.lua is gone already; just delete the Network.update line.

In either case: after this edit, main.lua's love.update no longer mentions Network. The App's dispatch loop will pick up `app.services` containing Network and call its update.

But — the App's dispatch loop is NOT running yet (bindLove fires in M4). So at this point, Network's update IS NOT CALLED. We need a bridge.

Replace `Network.update(dt)` inside love.update with:

```lua
-- M3 bridge: App's bindLove fires in M4; until then, drive Network through the App
-- manually from this love.update. (Removed in M4 when the App takes over.)
app:update(dt)
```

BUT — this is dangerous because the App's update also runs the fixedUpdate accumulator + spiral guard + the other services' updates. Since only Network is registered, the only service with an update method is Network. So effectively this just dispatches Network.update(dt) via the App.

Confirm by reading Application:update — it walks `self.services` and only calls `:update` on services that implement it. With only Network registered (and assuming UI/Jobs/etc. are NOT yet registered), only Network.update fires. The fixedUpdate loop runs zero iterations because no service implements fixedUpdate.

- [ ] **Step 4: Engine smoke + reconnect storm check**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 5 --output /tmp/m3-smoke.csv --scene login
```

Expected: ~1000 frames captured, login network roundtrip visible in logs (`[NET] [auth] Connected`), profiler capture shows `update.network.ms` similar to the P5a baseline at `docs/audits/p5/network-after.csv` (~0.011 ms avg).

- [ ] **Step 5: Compare against P5a baseline**

```
awk -F, 'NR==3{for(i=1;i<=NF;i++) if($i=="update.network.ms") col=i} NR>62 && NR<400 { sum+=$col; n++ } END { printf "M3 update.network.ms avg: %.4f (n=%d)\n", sum/n, n }' /tmp/m3-smoke.csv
awk -F, 'NR==3{for(i=1;i<=NF;i++) if($i=="update.network.ms") col=i} NR>62 && NR<400 { sum+=$col; n++ } END { printf "P5a after avg: %.4f (n=%d)\n", sum/n, n }' D:/dev/starworks/Gacha/docs/audits/p5/network-after.csv
```

Target: M3 average within 20% of P5a baseline. Any regression means the App dispatch is adding overhead — diagnose before proceeding.

- [ ] **Step 6: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(engine): register Network as App service (M3)"
```

---

## Task 7: M4 — Extract InputDispatch + bindLove

**Files:**
- Create: `GachaClient/engine/InputDispatch.lua`
- Modify: `GachaClient/main.lua` (remove all LOVE input callbacks; register InputDispatch + call bindLove)
- Modify: `GachaClient/tests/assets_harness/main.lua` (syntax-check inclusion)

This is the big "App takes over LOVE binding" step. **Visual gate at the end.**

- [ ] **Step 1: Read main.lua lines 879-1033 to capture all input callbacks**

Read `GachaClient/main.lua` from line 879 (`function love.keypressed`) through 1033 (end of `love.gamepadaxis`). Note the exact contents of each callback. The next step lifts these verbatim into a new module.

- [ ] **Step 2: Implement InputDispatch**

Create `GachaClient/engine/InputDispatch.lua`:

```lua
-- engine/InputDispatch.lua
-- LOVE input callback adapters. Lifted from main.lua lines 879-1033. Owns:
--   keypressed / keyreleased / textinput
--   mousepressed / mousereleased / mousemoved / wheelmoved
--   gamepadpressed / gamepadreleased / gamepadaxis
--   resize
--
-- Implements the service protocol -- :init(app) grabs UI + Game + Input service
-- references; the per-callback methods are called by the App's LOVE delegate
-- (Application's INPUT_CALLBACKS dispatch).
--
-- Single-consumer dispatcher: input events have one consumer per kind (UI today;
-- combat-specific consumers when combat takes focus). Does NOT broadcast to bus.

local ServiceLocator = require "services.ServiceLocator"
local Logger         = require "services.Logger"
local log            = Logger.create("Input")

local InputDispatch = {}
InputDispatch.__index = InputDispatch

function InputDispatch.new()
    return setmetatable({
        -- references cached in :init
        UI       = nil,
        Game     = nil,
        Input    = nil,
        viewport = nil,
    }, InputDispatch)
end

function InputDispatch:init(app)
    self.UI       = require "ui"
    self.Input    = require "services.Input"
    self.viewport = require "ui.viewport"
    -- Game is grabbed lazily because Game registers after InputDispatch:
    --   M6 will register Game; this init runs at M4 before that. Lazy lookup is safe.
end

local function getGame(self)
    if not self.Game then self.Game = ServiceLocator.get("game") end
    return self.Game
end

-- ============================================================================
-- DEV keybinds: F1/F2/F3/F4/F5 -> screen pushes. Lifted verbatim from main.lua.
-- These are input-shaped (key-driven), not game-state-shaped, so they live here.
-- ============================================================================

local function _devKeybind(self, key)
    -- Paste the existing F1/F2/F3/F4/F5 handling from main.lua's love.keypressed
    -- here. Roughly:
    --   F1 -> UI.push(gacha screen)
    --   F2 -> UI.push(inventory)
    --   F3 -> UI.push(party)
    --   F4 -> UI.push(combat test)
    --   F5 -> UI.push(combat)
    -- Adapt the exact lookups (require "ui.screens.inventory" etc.) verbatim.
end

-- ============================================================================
-- LOVE callback methods. Each lifted verbatim from main.lua and re-targeted to
-- call self.UI / self.Input / Game-via-getGame instead of bare module references.
-- ============================================================================

function InputDispatch:keypressed(key, scancode, isrepeat)
    -- Lift the body of main.lua's love.keypressed verbatim. Replace:
    --   UI.keypressed -> self.UI.keypressed
    --   Input.keypressed -> self.Input.keypressed
    --   doLogout(...) -> getGame(self):doLogout(...)   (Game owns doLogout)
    --   _devKeybind -> _devKeybind(self, key)
    -- Other module references stay as-is.
end

function InputDispatch:keyreleased(key, scancode)
    -- Lift verbatim from main.lua's love.keyreleased. Adapt UI/Input refs.
end

function InputDispatch:textinput(text)
    -- Lift verbatim from main.lua's love.textinput. UI.textinput -> self.UI.textinput.
end

function InputDispatch:resize(w, h)
    -- Lift verbatim from main.lua's love.resize. ui.viewport.resize -> self.viewport.resize.
    -- Also notify the Renderer (Pipeline service if registered) via ServiceLocator.
end

function InputDispatch:mousepressed(x, y, button)
    -- Lift verbatim. UI.mousepressed -> self.UI.mousepressed.
end

function InputDispatch:mousereleased(x, y, button)
    -- Lift verbatim.
end

function InputDispatch:mousemoved(x, y, dx, dy)
    -- Lift verbatim.
end

function InputDispatch:wheelmoved(x, y)
    -- Lift verbatim.
end

function InputDispatch:gamepadpressed(joystick, button)
    -- Lift verbatim. UI.gamepadpressed -> self.UI.gamepadpressed.
end

function InputDispatch:gamepadreleased(joystick, button)
    -- Lift verbatim.
end

function InputDispatch:gamepadaxis(joystick, axis, value)
    -- Lift verbatim. The leftx/lefty -> D-pad conversion in main.lua stays here.
end

return InputDispatch
```

(The `-- Lift verbatim` comments are placeholders for the implementer: read the corresponding block in main.lua and copy it in, applying the two transformations noted at each site.)

- [ ] **Step 3: Remove all LOVE callbacks from main.lua**

In `GachaClient/main.lua`, DELETE all of:
- `function love.keypressed(...)` through `end`
- `function love.keyreleased(...)` through `end`
- `function love.textinput(...)` through `end`
- `function love.resize(...)` through `end`
- `function love.mousepressed/released/moved(...)` through `end`
- `function love.wheelmoved(...)` through `end`
- `function love.gamepadpressed/released/axis(...)` through `end`
- `function love.quit() ... end` (App now owns it)
- The existing `function love.load() ... end` and `function love.update() ... end` and `function love.draw() ... end` — DELETE these too. The App takes them over via bindLove.

The previously-inline `love.load` block (lines 374-518) has a lot of setup that wasn't in InputDispatch — see Step 4.

- [ ] **Step 4: Migrate love.load setup into appropriate places**

The pre-bindLove `love.load` did MANY things. Categorize each piece:

1. **Logger config** (`Logger.enableFileOutput`, `setLevel`, `setFileLevel`) — move to top of main.lua right after the bootstrap require (it's process-level config, not service-level):

```lua
local Logger = require "services.Logger"
pcall(function()
    Logger.enableFileOutput(true, "logs/client.log")
    Logger.setLevel(Logger.LEVEL.INFO)
    Logger.setFileLevel(Logger.LEVEL.DEBUG)
end)
```

2. **GPU env logging** (`CrashLog.log(...)` of renderer/window info) — wrap in a NEW App method `:logEnvironment()` called from `Application:load()`. Move the implementation into Application.lua. (Or simplest: move the block into `app:registerService("input", ...)`'s init — but it's not really input-shaped. Cleanest: a tiny `engine/Environment.lua` service that just logs in `:init` and does nothing else. Use your judgment; pragmatic answer is "leave it inline in main.lua right after bootstrap require, before App.new").

3. **`require("ui.viewport").resize()`** — moves to `UI:init(app)` if UI gets an init method, or stays in main.lua's prelude. Pragmatic: leave in main.lua right after the Logger config.

4. **`love.graphics.setDefaultFilter`** — stays in main.lua (pre-App, since it's a one-off global GL setting).

5. **`Input.load()`** — moves to a new `Input.init(self, app)` method or stays in main.lua's prelude. Pragmatic: stays in main.lua.

6. **`Settings.load()` + `Settings.onGraphicsChanged(...)`** — stays in main.lua's prelude.

7. **`Profiler:setEnabled` + `setBudget`** — stays in main.lua's prelude. These are App-config, kept visible at the top.

8. **`love.joystick.loadGamepadMappings(...)`** — moves to `InputDispatch:init`.

9. **`UI.load()`** — moves to a thin `UI:init(self, app)` method OR stays in main.lua's prelude. Pragmatic: stays in main.lua (UI is a big legacy module; don't reshape it in this workstream).

10. **`StringTable.load("data/strings.json")`** — stays in main.lua's prelude.

11. **`require "services.gacha_actions"`** — stays in main.lua's prelude (it's a side-effect require for registration).

12. **`--export-schema` argparse + `love.event.quit()` early-exit** — stays in main.lua, right before `app:bindLove()`.

13. **`--profile-capture` argparse** — stays in main.lua, right before `app:bindLove()`.

14. **`UILoader.loadDirectory(...)`** — stays in main.lua's prelude.

15. **`Assets.bakeAtlases()`** — stays in main.lua's prelude.

16. **`JobSystem.boot()`** — stays in main.lua's prelude (Jobs is registered as a service in M0c-like step but it has its own boot timing).

17. **`UIPreview.start()`** — stays in main.lua's prelude.

18. **Session restore + auto-resume + `showLogin()`** — moves to `Game:init(app)` in M6. For M4, leave inline in main.lua.

19. **`Network.on(...)` event subscriptions for reconnecting / reconnected / etc.** — moves to `Game:init(app)` in M6. For M4, leave inline in main.lua.

20. **`state.performLogout = doLogout`** — stays in main.lua until M6 (Game takes ownership).

After this categorization, main.lua's prelude grows but its love.load body is empty — because the App handles love.load via bindLove. Re-do main.lua's structure roughly like this:

```lua
require "boot"
require "engine.bootstrap"

-- Logger config (pre-App; process-level)
local Logger = require "services.Logger"
pcall(function() ... end)

-- All the prelude items 3, 4, 5, 6, 7, 9, 10, 11, 14, 15, 16, 17 above
-- (UI/Input/Settings/StringTable/UILoader/Assets/JobSystem/UIPreview/Profiler)
-- run inline here. Existing code; this section is the "pre-App configuration"
-- block.

-- App + service registration
local Application = require "engine.Application"
local app = Application.new({ fixedRate = 60 })

-- DISPATCH ORDER -- DO NOT REORDER WITHOUT TESTING
app:registerService("network",  require "network")
app:registerService("input",    require("engine.InputDispatch").new())
-- (game, jobs, ui, render added in M6/M5/M0c respectively when they become services)

-- CLI argparse: --export-schema (early exit) + --profile-capture
if arg then
    -- existing handling, unchanged
end

-- Session restore + auto-resume + showLogin (moves to Game:init in M6)
local CacheManager = require "services.CacheManager"
local cached = ...
if cached and cached.token then ... else showLogin() end

-- Network.on subscriptions (move to Game:init in M6)
Network.on("reconnecting", function(attempt, delay) ... end)
Network.on("reconnected",  function() ... end)
Network.on("reconnectNeedsAuth", function() doLogout(...) end)
Network.on("reconnectFailed",    function() doLogout(...) end)
Network.on("sessionExpired",     function(reason) doLogout(...) end)

-- Hand control to LOVE
app:bindLove()
```

The doLogout / loadGameWorld / etc. helper functions stay in main.lua for now (they move to Game.lua in M6).

- [ ] **Step 5: Engine boot + smoke**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 2 --output /tmp/m4-smoke.csv
```

Expected: ~400 frames captured. No crash. The App's dispatch is now driving everything via bindLove.

- [ ] **Step 6: Add InputDispatch to assets harness syntax-check**

In `tests/assets_harness/main.lua` `touched` list:

```lua
    "engine/InputDispatch.lua",
```

Run assets harness. Confirm green.

- [ ] **Step 7: USER VISUAL GATE — full input flow**

The user runs the game normally. Verify:
1. Engine boots → login screen appears
2. Type a username + password → text appears in fields, tab navigates fields
3. Click login button → reaches click-to-play landing
4. Click play → world loads
5. F1 (gacha) → screen pushes; F2 (inventory); F3 (party); F4 (combat test); F5 (combat)
6. In-game: mouse hover/click, wheel scroll in scrolling lists, gamepad axis if available
7. Resize the window → viewport reflows correctly
8. Quit (close button or `:q` if there's a console) → engine shuts down cleanly

If any input is broken, the lifted callback in InputDispatch is wrong — diagnose by comparing main.lua's original body to the InputDispatch method.

- [ ] **Step 8: Commit on visual GO**

```
git -C D:/dev/starworks/Gacha add GachaClient/engine/InputDispatch.lua GachaClient/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(engine): extract InputDispatch + App takes over LOVE binding (M4)"
```

---

## Task 8: M5 — Extract Pipeline to `systems/render/Pipeline.lua`

**Files:**
- Create: `GachaClient/systems/render/Pipeline.lua`
- Modify: `GachaClient/main.lua` (remove render-pipeline block; register Pipeline as service)
- Modify: `GachaClient/tests/assets_harness/main.lua` (syntax-check inclusion)

**Visual gate at the end.**

- [ ] **Step 1: Read main.lua lines 575-858 to capture the render-graph composition**

Read `GachaClient/main.lua` from line 575 (`local function worldProducer`) through 858 (end of `composeFrame` + `love.draw`). Note all the local functions and how they reference one another.

Functions to lift:
- `worldProducer(ctx)`
- `hdrSupported()`
- `composeScreensScene(ctx)`
- `composeScene(ctx, producer, layer)`
- `produceUI(ctx, layer)`
- `produceHud(ctx, layer)`
- `produceLayer(layer, ctx)`
- `resolveFrame(ctx)`
- `composeFrame(ctx)`
- The lazy `_renderer` build (currently at the top of `love.draw`)

- [ ] **Step 2: Implement Pipeline**

Create `GachaClient/systems/render/Pipeline.lua`:

```lua
-- systems/render/Pipeline.lua
-- Render-graph composition orchestrator. Lifted from main.lua lines 575-858.
-- Owns: composeScene / composeScreensScene / produceUI / produceHud / produceLayer /
-- resolveFrame / composeFrame / worldProducer / hdrSupported / the Renderer instance.
--
-- Pipeline:init(app) builds the Renderer eagerly (love.graphics is available since
-- init runs inside Application:load which is called from love.load). The previous
-- lazy "_renderer built lazily on first draw" pattern goes away.
--
-- Reads from Game (game.gameLoaded, game.world) via ServiceLocator. Reads from
-- the screen stack (UI.stack) via the UI module.

local ServiceLocator = require "services.ServiceLocator"
local Renderer       = require "systems.render.Renderer"
local Pass           = require "systems.render.Pass"
local RenderTargets  = require "systems.render.RenderTargets"
local Profile        = require "systems.render.Profile"
local SceneEffects   = require "systems.render.SceneEffects"
local Profiler       = require("systems.render.Profiler").default
local Logger         = require "services.Logger"

local log = Logger.create("Pipeline")

local Pipeline = {}
Pipeline.__index = Pipeline

function Pipeline.new()
    return setmetatable({
        renderer = nil,
        game     = nil,
        UI       = nil,
    }, Pipeline)
end

function Pipeline:init(app)
    -- Build the Renderer eagerly. main.lua's lazy pattern (_renderer built on first
    -- draw) was a workaround for love.load running before love.graphics was ready;
    -- :init runs FROM love.load, so love.graphics IS available here.
    self.renderer = Renderer.new()   -- adapt to the actual Renderer.new signature
    self.UI       = require "ui"
    -- self.game grabbed lazily because Game registers AFTER render in M6's order:
    --   ... wait. Actually render should be LAST in registration order. So if Game
    --   registers before Pipeline, ServiceLocator.get("game") works here. But for
    --   M5 (before M6 lands), Game isn't a service yet -- game state still lives
    --   on the global `state` proxy. Use a lookup-on-first-draw guard:
end

local function getGame(self)
    if not self.game then self.game = ServiceLocator.get("game") end
    return self.game
end

-- ============================================================================
-- The lifted producer + compose functions. Each was a local function in main.lua;
-- they become methods or module-locals here.
-- ============================================================================

function Pipeline:worldProducer(ctx)
    -- Lift verbatim from main.lua's worldProducer. Replace:
    --   state.gameLoaded -> getGame(self) and self.game.gameLoaded
    --   state.world -> getGame(self) and self.game.world
    --   Any other state.X reads -> ServiceLocator.get(X) or self.X
end

function Pipeline:hdrSupported()
    -- Lift verbatim.
end

function Pipeline:composeScreensScene(ctx)
    -- Lift verbatim. Adapt UI/state refs.
end

function Pipeline:composeScene(ctx, producer, layer)
    -- Lift verbatim.
end

function Pipeline:produceUI(ctx, layer)
    -- Lift verbatim.
end

function Pipeline:produceHud(ctx, layer)
    -- Lift verbatim.
end

function Pipeline:produceLayer(layer, ctx)
    -- Lift verbatim.
end

function Pipeline:resolveFrame(ctx)
    -- Lift verbatim.
end

function Pipeline:composeFrame(ctx)
    -- Lift verbatim.
end

-- ============================================================================
-- Service hook
-- ============================================================================

function Pipeline:draw()
    -- Lift the body of main.lua's love.draw verbatim. The lazy _renderer build at
    -- the top GOES AWAY -- self.renderer is built in :init. Everything else stays.
    -- Adapt: bare worldProducer / composeFrame / etc. -> self:worldProducer / etc.
end

return Pipeline
```

(Again, `-- Lift verbatim` markers indicate "read the corresponding block in main.lua and copy it in, applying the noted adaptations.")

- [ ] **Step 3: Remove the render-pipeline block from main.lua**

In `GachaClient/main.lua`, DELETE:
- The entire block from `local function worldProducer` (line 575) through `function love.draw() ... end` (line 877).

The Pipeline service now owns all of this. The App calls `Pipeline:draw()` during the `:draw` lifecycle hook.

- [ ] **Step 4: Register Pipeline as the LAST service**

In main.lua, AFTER `app:registerService("input", ...)`, add:

```lua
app:registerService("render", require("systems.render.Pipeline").new())
```

Pipeline registers LAST so its `:draw` runs last in the frame.

- [ ] **Step 5: Engine smoke**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 3 --output /tmp/m5-smoke.csv
```

Expected: ~600 frames captured. Login renders. No crash. Profiler shows scene/ui/hud scopes intact.

- [ ] **Step 6: Add Pipeline to assets harness syntax-check**

In `tests/assets_harness/main.lua` `touched` list:

```lua
    "systems/render/Pipeline.lua",
```

Run assets harness. Confirm green.

- [ ] **Step 7: USER VISUAL GATE — render parity**

The user verifies that every screen renders identically:
1. Login (background, panels, frosted glass, post-process grade)
2. Click-to-play landing
3. Gacha screen (F1) — pulls + reveals
4. Inventory (F2)
5. Party (F3)
6. Dailies / WaveGame
7. Combat (F4 + F5)
8. Pause overlay (Esc)
9. Settings menu

Any visual difference means a transformation in Step 2 was wrong — diff Pipeline.lua's method against the original main.lua block.

- [ ] **Step 8: Commit on visual GO**

```
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Pipeline.lua GachaClient/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(render): extract Pipeline service from main.lua (M5)"
```

---

## Task 9: M6 — Extract Game to `game/Game.lua`

**Files:**
- Create: `GachaClient/game/Game.lua`
- Modify: `GachaClient/main.lua` (remove session-lifecycle block; register Game; move Network.on subscriptions)
- Modify: `GachaClient/tests/assets_harness/main.lua` (syntax-check inclusion)

**Visual gate at the end.**

- [ ] **Step 1: Read main.lua lines 78-373 to capture the session-lifecycle block**

Read `GachaClient/main.lua` from line 78 (the GAME_STATE block) through 373 (end of `guarded`). Note all the local functions and state references.

To lift:
- `GAME_STATE` enum
- `state.gameState`, `state.gameLoaded` assignments
- `loadGameWorld()`
- `doLogout(message)`
- `autoResume(cached, landing)`
- `makeLanding()`
- `showLogin()`
- `guarded(label, fn)` (move to Game; if used elsewhere, leave behind in a util module)
- The Network.on subscriptions (lines 506-514 in the love.load block — actually those moved to main.lua's prelude in M4; lift them from there now)

- [ ] **Step 2: Implement Game**

Create `GachaClient/game/Game.lua`:

```lua
-- game/Game.lua
-- Game session lifecycle. Lifted from main.lua lines 78-373 + the Network.on
-- subscriptions from M4's prelude.
--
-- Owns:
--   gameState (LOGIN / LOADING / PLAYING) and gameLoaded as instance fields
--   loadGameWorld / doLogout / autoResume / makeLanding / showLogin / guarded
--   Network event subscriptions (via Network.on -- bus retrofit deferred to
--   [[project_ui_event_bus]])
--
-- Publishes its own events onto the App's bus for future consumers:
--   game.session_started, game.session_ended, game.world_loaded, game.world_unloaded
--
-- DELIBERATELY NOT formalized as a state-machine class -- the future scene
-- system will absorb these state vars. Keep this lean.

local ServiceLocator = require "services.ServiceLocator"
local Network        = require "network"
local Logger         = require "services.Logger"

local log = Logger.create("Game")

local GAME_STATE = { LOGIN = "login", LOADING = "loading", PLAYING = "playing" }

local Game = {}
Game.__index = Game

function Game.new()
    return setmetatable({
        gameState  = GAME_STATE.LOGIN,
        gameLoaded = false,
        world      = nil,
        bus        = nil,    -- set in :init
        UI         = nil,    -- set in :init
    }, Game)
end

function Game:init(app)
    self.bus = app.bus
    self.UI  = require "ui"
    self:_wireNetworkEvents()
    self:_startSessionFlow()
end

function Game:_wireNetworkEvents()
    -- Lift the Network.on(...) subscriptions verbatim from main.lua's prelude.
    -- Replace bare `doLogout` calls with `self:doLogout(...)`.
    Network.on("reconnecting", function(attempt, delay)
        log.info("Reconnecting in %.1fs (attempt %d/5)", delay, attempt)
    end)
    Network.on("reconnected",        function() log.info("Reconnected!") end)
    Network.on("reconnectNeedsAuth", function() self:doLogout("Connection restored -- please log in") end)
    Network.on("reconnectFailed",    function() self:doLogout("Connection lost") end)
    Network.on("sessionExpired",     function(reason) self:doLogout("Session expired: " .. tostring(reason)) end)
end

function Game:_startSessionFlow()
    -- Lift the session-restore-or-show-login block from main.lua's prelude verbatim.
    -- Replace bare showLogin/autoResume/makeLanding refs with self:showLogin/self:autoResume/self:makeLanding.
    local CacheManager = require "services.CacheManager"
    local cached = CacheManager.getRememberUsername() and CacheManager.getSessionForRestore() or nil
    if cached and cached.token then
        local landing = self:makeLanding()
        landing:setConnecting(cached.playerName)
        self.UI.push(landing)
        self:autoResume(cached, landing)
    else
        self:showLogin()
    end
end

-- ============================================================================
-- Session-lifecycle methods. Each lifted from the corresponding main.lua local
-- function. References to `state.gameState` become `self.gameState`; references
-- to `state.world` become `self.world`. Bus publishes added where the spec
-- specifies (game.session_started, game.world_loaded, etc.).
-- ============================================================================

function Game:loadGameWorld()
    -- Lift verbatim. After successful world load, publish:
    self.gameLoaded = true
    if self.bus then self.bus:publish("game.world_loaded", {}) end
end

function Game:doLogout(message)
    -- Lift verbatim. Publish session_ended before clearing state:
    if self.bus then self.bus:publish("game.session_ended", { reason = message or "manual" }) end
    -- ... rest of doLogout body ...
    self.gameLoaded = false
    if self.bus then self.bus:publish("game.world_unloaded", {}) end
end

function Game:autoResume(cached, landing)
    -- Lift verbatim. On successful resume, publish:
    -- if self.bus then self.bus:publish("game.session_started", { playerName = ..., playerId = ... }) end
end

function Game:makeLanding()
    -- Lift verbatim.
end

function Game:showLogin()
    -- Lift verbatim.
end

function Game:guarded(label, fn)
    -- Lift verbatim if used only by Game; otherwise move to a util module.
end

-- For legacy compat: state.performLogout -> Game:doLogout. M4 main.lua had this
-- in its prelude; remove from main.lua after this lands.
function Game:shutdown()
    -- Clean teardown. doLogout-without-network if necessary.
end

return Game
```

- [ ] **Step 3: Remove the session-lifecycle block from main.lua**

In `GachaClient/main.lua`, DELETE:
- Lines 78-373 (the entire game-state / lifecycle block)
- The Network.on subscriptions from main.lua's prelude (added in M4)
- The session-restore / showLogin block from main.lua's prelude (added in M4)
- The `state.performLogout = doLogout` line if present

These all live in Game now.

- [ ] **Step 4: Register Game as a service**

In main.lua, BEFORE `app:registerService("input", ...)` (Game runs before Input so input-driven actions can read game state):

Wait — actually, looking at the dispatch order: network → input → game → jobs → ui → render. So Input is BEFORE Game. Adjust registration order accordingly:

```lua
-- DISPATCH ORDER -- DO NOT REORDER WITHOUT TESTING
app:registerService("network",  require "network")
app:registerService("input",    require("engine.InputDispatch").new())
app:registerService("game",     require("game.Game").new())
app:registerService("jobs",     require "services.jobs.JobSystem")
app:registerService("ui",       require "ui")
app:registerService("render",   require("systems.render.Pipeline").new())
```

(Jobs and UI registrations may already be inline or absent; add them in the correct order if they weren't migrated yet. JobSystem.boot() that was in main.lua's prelude — keep that until Jobs becomes a service with a proper :init or :start hook.)

- [ ] **Step 5: Engine smoke**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 3 --output /tmp/m6-smoke.csv
```

Expected: ~600 frames captured. Login → landing → click play → world loads → screens functional. No crash.

- [ ] **Step 6: Add Game to assets harness syntax-check**

In `tests/assets_harness/main.lua` `touched` list:

```lua
    "game/Game.lua",
```

Run assets harness. Confirm green.

- [ ] **Step 7: USER VISUAL GATE — full session flow**

The user verifies the full session lifecycle:
1. First-launch (no cached session): login screen → enter credentials → click login → click-to-play landing → click play → world loads
2. Re-launch (cached session): launches directly to click-to-play landing in connecting state → auto-resume → world loads
3. F4 combat → combat screen renders + plays correctly
4. Reconnect storm: kill GachaAuth (`taskkill /f /im GachaAuth.exe`), restart it. Verify [NET] logs show reconnecting → reconnectNeedsAuth → Game:doLogout fires → returns to login screen.
5. Manual logout (pause menu → Log Out): returns to login screen cleanly.
6. Session expiry (set local clock forward 25h before launch): auto-resume fails → falls back to login.
7. Quit cleanly.

- [ ] **Step 8: Commit on visual GO**

```
git -C D:/dev/starworks/Gacha add GachaClient/game/Game.lua GachaClient/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(game): extract Game service from main.lua (M6)"
```

---

## Task 10: M7 — fixedUpdate consumer: WaveGame + Box2D

**Files:**
- Modify: `GachaClient/ui/screens/dailies/WaveGame.lua` (convert physics tick to :fixedUpdate)

**Visual gate at the end.**

This validates the entire fixed-update infrastructure (accumulator, spiral guard, alpha residual) under real load.

- [ ] **Step 1: Read WaveGame to find its physics tick**

Read `GachaClient/ui/screens/dailies/WaveGame.lua`. Locate:
- The Box2D world step call (typically `self.world:step(dt)` or similar inside an `:update` method)
- Any per-frame physics-shaped logic that wants determinism

- [ ] **Step 2: Add a `:fixedUpdate` method**

In WaveGame.lua, after the existing `:update(dt)` method, add:

```lua
-- M7: physics step moves into fixedUpdate for determinism. The Application
-- dispatcher calls this 0-5 times per real frame (60 UPS target with spiral
-- guard). Variable-dt logic stays in :update.
function WaveGame:fixedUpdate(fixedDt)
    if self.world then
        self.world:step(fixedDt)   -- Box2D 60Hz step
    end
    -- Any other determinism-sensitive physics-shaped logic moves here from :update.
    -- Common candidates: position integration, collision response, force application,
    -- minigame-internal physics like wave-spawn timers if they should be frame-rate
    -- independent.
end
```

- [ ] **Step 3: Remove the moved logic from `:update`**

In WaveGame.lua's `:update(dt)`, REMOVE the lines that moved into `:fixedUpdate`. The remaining `:update(dt)` keeps variable-dt logic (animation, UI state, camera, anything that legitimately wants dt-frame-rate-tied behavior).

If WaveGame renders moving physics bodies and wants smooth interpolation between fixed steps, change `:update(dt)` signature to `:update(dt, alpha)`:

```lua
function WaveGame:update(dt, alpha)
    -- alpha = (Application._accum / Application._fixedDt) in [0, 1).
    -- Use for smooth rendering of moving Box2D bodies:
    --   local px, py = self.body:getPosition()
    --   local lx, ly = self._lastPos.x, self._lastPos.y
    --   local rx, ry = lx + (px - lx) * alpha, ly + (py - ly) * alpha
    --   self.sprite:setPosition(rx, ry)
    -- Capture self._lastPos in :fixedUpdate BEFORE the world step.
    -- Skip this initially -- only add if visible judder appears in the visual gate.
end
```

- [ ] **Step 4: Wave-game service registration check**

WaveGame is a UI screen, not a top-level service. The App registers UI as a service (M4); UI manages screen stacks. The screen-level `:fixedUpdate` is called by UI's `:fixedUpdate` if UI delegates to the active screen.

Verify: does `ui` module currently have `:update(dt)` that delegates to the screen stack? If yes, add a sibling `:fixedUpdate(fixedDt)` that does the same delegation. If no, the App's dispatch won't see WaveGame's fixedUpdate.

In `GachaClient/ui/init.lua` (or wherever UI module lives), add:

```lua
function UI.fixedUpdate(fixedDt)
    -- Lift the pattern from UI.update(dt). Walk the screen stack; call
    -- :fixedUpdate on screens that implement it.
    for _, screen in ipairs(UI._stack) do   -- adapt to actual stack accessor
        if screen.fixedUpdate then screen:fixedUpdate(fixedDt) end
    end
end
```

Now the App's dispatch finds `UI.fixedUpdate` and calls it, UI walks the screen stack, WaveGame's fixedUpdate runs at 60Hz.

- [ ] **Step 5: Engine smoke**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 5 --output /tmp/m7-smoke.csv --scene dailies
```

Expected: ~1000 frames captured. WaveGame physics runs. The fixed.spiralGuards profiler counter is 0 (or near 0) under normal play.

- [ ] **Step 6: Spiral-guard exercise**

Add a temporary `love.timer.sleep(0.25)` after the WaveGame's `:fixedUpdate` body (one-time, to simulate a 250ms hitch). Run for 3 seconds. Verify:
- The `fixed.spiralGuards` counter ticks (visible in Profiler overlay or CSV column)
- A WARN log line appears: `Spiral guard fired: dropped X.XXX of accumulator residual`
- The game does NOT cascade-hitch — after the artificial sleep, the next frame is normal

Remove the temporary sleep.

- [ ] **Step 7: USER VISUAL GATE — WaveGame feels right**

The user plays WaveGame for a minute and verifies:
1. Movement feels smooth (no judder from physics step interval mismatch)
2. Spawn timing feels consistent
3. Frame-rate dips do NOT cause physics to slow down or jump (this is the whole point of fixedUpdate)
4. No new visual artifacts in WaveGame
5. Other screens still work normally

If physics-driven sprites visibly judder, add the alpha-interpolation pattern from Step 3.

- [ ] **Step 8: Commit on visual GO**

```
git -C D:/dev/starworks/Gacha add GachaClient/ui/screens/dailies/WaveGame.lua GachaClient/ui/init.lua
git -C D:/dev/starworks/Gacha commit -m "feat(physics): WaveGame + Box2D step moves to :fixedUpdate (M7)"
```

---

## Task 11: M8 — Cleanup + main.lua final shape + audit

**Files:**
- Modify: `GachaClient/main.lua` (final cleanup)
- Create: `docs/audits/app-refactor/summary.md`
- Create: `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/project_app_architecture.md` (new memory)
- Modify: `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/MEMORY.md` (index line)
- Modify: `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/project_ui_event_bus.md` (note bus is shipped)

- [ ] **Step 1: Audit main.lua's final shape**

Read `GachaClient/main.lua`. Confirm it's structured as:

```lua
require "boot"
require "engine.bootstrap"

-- Logger config (pre-App; process-level)
local Logger = require "services.Logger"
pcall(function() ... end)

-- Pre-App prelude: viewport, default filter, settings, profiler, UI/StringTable/UILoader/Assets/JobSystem/UIPreview/etc.
require("ui.viewport").resize()
love.graphics.setDefaultFilter("linear", "linear", 16)
require("services.Input").load()
local Settings = require "services.Settings"
Settings.load()
Settings.onGraphicsChanged(function() require("services.Assets").refreshImageFilters() end)
-- ... profiler budgets, joystick mappings, UI.load, StringTable.load, gacha_actions,
--     UILoader.loadDirectory, Assets.bakeAtlases, JobSystem.boot, UIPreview.start

-- App + service registration
local Application = require "engine.Application"
local app = Application.new({ fixedRate = 60 })

-- DISPATCH ORDER -- DO NOT REORDER WITHOUT TESTING
app:registerService("network",  require "network")
app:registerService("input",    require("engine.InputDispatch").new())
app:registerService("game",     require("game.Game").new())
app:registerService("jobs",     require "services.jobs.JobSystem")
app:registerService("ui",       require "ui")
app:registerService("render",   require("systems.render.Pipeline").new())

-- CLI argparse
if arg then
    -- --export-schema early-exit handling, unchanged
    -- --profile-capture handling, unchanged
end

-- Hand control to LOVE
app:bindLove()
```

Count lines:
```
wc -l GachaClient/main.lua
```

Target: under 50 lines (excluding blank lines and comments). If it's over, audit what else can move.

- [ ] **Step 2: Delete any dead code surfaced during migration**

Grep for orphaned references:
- Functions / locals in main.lua that no longer have callers
- requires that are no longer used
- The `state.performLogout` line if it's still hanging around (Game owns this now)

Delete with care; if a reference is in another file you didn't touch, leave it for now.

- [ ] **Step 3: Write the audit summary**

Create `docs/audits/app-refactor/summary.md`:

```markdown
# App Refactor — Outcome Summary

## What shipped

| Milestone | SHA | Notes |
|---|---|---|
| M0a | <sha> | Bus pub/sub primitive + tests |
| M0b | <sha> | Application lifecycle dispatcher + tests |
| M0c | <sha> | bootstrap.lua + architecture docs |
| M1 | <sha> | Extract bootstrap from main.lua |
| M2 | <sha> | Stand up App alongside existing main.lua |
| M3 | <sha> | Migrate Network as a service |
| M4 | <sha> | Extract InputDispatch + bindLove |
| M5 | <sha> | Extract Pipeline to systems/render/Pipeline.lua |
| M6 | <sha> | Extract Game to game/Game.lua |
| M7 | <sha> | WaveGame + Box2D :fixedUpdate consumer |
| M8 | <sha> | Cleanup + main.lua final shape |

## Metrics

| | Before | After | Delta |
|---|---|---|---|
| main.lua lines | 1038 | <count> | <delta> |
| Top-level files in GachaClient/ | <count> | <count> | <delta> |
| update.network.ms (P5a baseline) | 0.0117 | <m3-value> | <delta> |
| Service count | 0 | 6 | +6 |

## Durable lessons

[Fill in what you learned: where the duck-typing worked, where it broke down, what
LOVE-callback nuances surprised you, etc.]

## What's deferred

- Network.on retrofit onto the bus -- [[project_ui_event_bus]] workstream
- state proxy retirement -- own future cleanup
- Formal scene system that absorbs Game's GAME_STATE bits -- future workstream
```

- [ ] **Step 4: Update memory index**

Create a new memory file `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/project_app_architecture.md`:

```markdown
---
name: app-architecture
description: Application + Bus + ServiceLocator three-way design; service protocol; fixed-timestep dispatch; main.lua as entry point only
metadata:
  type: project
---

main.lua is the entry point only (~40 lines): bootstrap require, pre-App prelude,
service registration, bindLove. All lifecycle dispatch goes through
`engine/Application.lua` which owns the bus and walks an ordered service list.

**Architecture (see docs/architecture/application.md):**
- `ServiceLocator` (existing, `services/ServiceLocator.lua`): name -> service lookup
- `Application` (`engine/Application.lua`): lifecycle dispatcher; owns bus + ordered list
- `Bus` (`engine/Bus.lua`): sync pub/sub for app events; free-form names; pcall-guarded

**Service protocol (all hooks optional, duck-typed):**
`init(app)` / `fixedUpdate(fixedDt)` / `update(dt, alpha)` / `draw()` / `shutdown()`.

**Fixed-step:** 60 UPS via accumulator in `Application:update`. Spiral guard caps at 5
fixed steps per real frame. Alpha = accumulator residual passed to `:update` for
interpolated rendering.

**Registration order = dispatch order.** Defined in `main.lua` -- the comment block
`// DISPATCH ORDER -- DO NOT REORDER WITHOUT TESTING` marks it. Network -> Input ->
Game -> Jobs -> UI -> Render.

**Bus convention:** event names are `category.action`. Single table payload. Free-form
(no validation). docs/events.md is the human-maintained event registry.

**Deferred:**
- Network.on retrofit onto bus -> `[[project_ui_event_bus]]`
- `state` proxy retirement (legacy backward-compat) -> own future cleanup
- Formal scene system absorbs Game's GAME_STATE -> future workstream

Spec: `docs/superpowers/specs/2026-05-28-application-engine-design.md` (commit `50bd2aa`).
Audit: `docs/audits/app-refactor/summary.md`.
```

- [ ] **Step 5: Update MEMORY.md index**

In `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/MEMORY.md`, add the new entry:

```markdown
- [App architecture](project_app_architecture.md) — Application + Bus + ServiceLocator design; main.lua as entry point; 60 UPS fixedUpdate via accumulator + alpha + spiral guard
```

- [ ] **Step 6: Update project_ui_event_bus.md**

In `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/project_ui_event_bus.md`, note that the bus infrastructure has shipped:

```markdown
**UPDATE 2026-05-28:** The bus primitive (`engine/Bus.lua`) has shipped via the App
refactor (`docs/superpowers/specs/2026-05-28-application-engine-design.md`, commits
M0-M8). Retrofit of Network.on onto the bus + subselects / reveal results / combat
outcomes / settings / login transitions onto the bus is THIS workstream's remaining
scope. Bus is at `app.bus` (where `app = ServiceLocator.get("app")`).
```

- [ ] **Step 7: Engine smoke + final harness check**

```
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 2 --output /tmp/m8-smoke.csv
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/assets_harness
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/threading_harness
```

All four green.

- [ ] **Step 8: Commit**

```
git -C D:/dev/starworks/Gacha add GachaClient/main.lua docs/audits/app-refactor/summary.md
git -C D:/dev/starworks/Gacha commit -m "docs(engine): App refactor cleanup + summary (M8)"
```

(Memory files are outside the repo; no `git add` needed.)

---

## Self-review

- [x] 11 tasks cover M0a / M0b / M0c / M1 / M2 / M3 / M4 / M5 / M6 / M7 / M8 — all phases in the spec
- [x] Each task ends in a commit; each phase is independently shippable
- [x] Headless TDD on M0a (Bus) and M0b (Application); visual gates on M4 / M5 / M6 / M7
- [x] No placeholders: lift-verbatim markers point at specific main.lua line ranges with the transformations spelled out
- [x] Type consistency: `Application.new(opts)`, `app.bus`, `app.services`, `app._fixedDt` used consistently across tasks
- [x] Service registration order matches dispatch order discussed in spec (network → input → game → jobs → ui → render)
- [x] Combat ABILITIES + F2/F3 deferrals respected; targeted git add only; no `--no-verify`
- [x] Spec's R4 (spiral guard) gets exercised explicitly in M7 step 6
- [x] Spec's R1 (registration-order fragility) lands as the comment block in main.lua at M8
- [x] Bus event registry (docs/events.md) created in M0c; future events get added by hand
- [x] Architecture one-pager (docs/architecture/application.md) created in M0c per spec R5
- [x] Memory updates land in M8 (new `project_app_architecture.md` entry + index + project_ui_event_bus update)
