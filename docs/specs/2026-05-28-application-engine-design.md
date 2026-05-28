# Application + Bus + Service Lifecycle — Design

**Date:** 2026-05-28
**Status:** Brainstormed → spec'd. Plan to follow.
**Driver:** Decompose the 1038-line `main.lua` into a clean entry point + an `Application` class that owns LÖVE callback hosting, service lifecycle dispatch, and an event bus. Build on the existing `ServiceLocator` rather than replacing it.
**Cross-references:** `[[project_ui_event_bus]]`, `[[aaa-rendering-initiative]]`, `[[asset_manager]]`, `[[feedback_abilities_system_ownership]]`, `[[project_inventory_party_rework]]`.

---

## Goals and non-goals

**Goal.** Replace the kitchen-sink `main.lua` with a clean entry point + an `Application` class that owns LÖVE callback hosting, service lifecycle dispatch, and an event bus. Decompose the four mixed-concern blocks in today's main.lua (game session, render pipeline, input callback adapters, bootstrap) into focused modules that the `Application` orchestrates.

**The abstraction owns:**
- **`Application`** — LÖVE callback bindings (`love.load` / `update` / `draw` / `keypressed` / etc.), the service lifecycle dispatch loop (init / fixedUpdate / update / draw / shutdown calls in registered order), and the event bus instance
- **`Bus`** — pub/sub for app-level events. Free-form string event names, single table payload, synchronous dispatch in subscription order. Subscriptions return a token used to unsubscribe.
- A small set of new "service" modules — `Game` (session lifecycle), `RenderPipeline` (the compose chain), `InputDispatch` (LÖVE input adapters), plus a `bootstrap` module for the stdout / CrashLog / runtime.log setup that must run before anything else

**The abstraction does NOT own:**
- A new service registry — `ServiceLocator` already exists with `register` / `get` / `registerFactory` / `activate` / `reset`. The `Application` registers services *into* `ServiceLocator` and keeps its own ordered service list for dispatch.
- Combat ABILITIES gameplay (boundary memory)
- Replacing the `state` proxy — the existing `ServiceLocator.createStateProxy()` shim stays during migration so legacy `state.X` reads keep working
- Rewriting existing subsystems (UI, Network, Jobs, Assets, Settings) as classes — they slot into the App via **duck-typed dispatch**. The App's update/draw loop checks "does this entry have `:update` / `:draw`?" and calls if present. Existing modules require zero changes to participate.
- Defining the retrofit migration of subselects / reveal / combat / settings / login transitions onto the bus — that's the `[[project_ui_event_bus]]` workstream. This spec ships the bus; consumers migrate at their own pace.

**Migration mandate.** Every line currently in `main.lua` either (a) moves to a focused module, (b) stays in `main.lua` as bootstrap/entry-point code (~40 lines target), or (c) gets deleted as dead code surfaced during the move. New code added after this lands must use the `Application` + bus pattern.

**Explicit non-goals:**
- A generic game-engine framework. `Application` is opinionated about *our* loop (LÖVE, single-threaded GL, our service shape). No DI container, no plugin loader.
- A scene system. A formal scene system is a future workstream; `Game`'s GAME_STATE machine + show-login / show-landing logic stays deliberately un-formalized so it can be absorbed cleanly when the scene system arrives.

---

## `Application` + `Bus` + `ServiceLocator` integration

Three types, three responsibilities. No overlap.

### `Application` (singleton, one per process)

```lua
local Application = {}
Application.__index = Application

function Application.new(opts)
    opts = opts or {}
    return setmetatable({
        bus       = Bus.new(),
        services  = {},                  -- ORDERED list for dispatch
        _running  = false,
        _accum    = 0,
        _fixedDt  = 1 / (opts.fixedRate or 60),
    }, Application)
end
```

Owns: the bus instance, the ordered service list, the `_running` flag, the fixed-step accumulator. Methods are `:registerService(name, instance)`, `:bindLove()`, plus the LÖVE-callback delegates (`:load()`, `:update(dt)`, `:draw()`, `:keypressed(...)`, ..., `:quit()`).

Construction is split from start:
- `Application.new()` — creates the bus + empty service list. Nothing else runs yet.
- `app:registerService(name, svc)` — appends to `services`, also calls `ServiceLocator.register(name, svc)`. Order of registration = order of dispatch.
- `app:bindLove()` — installs callback handlers (`love.load = function() app:load() end`, etc.). Only at this point are LÖVE callbacks live.

`app:load()` (called by LÖVE) runs each service's `init(app)` hook in registration order; that's the first time services see each other.

### `Bus` (one per Application)

```lua
local Bus = {}
Bus.__index = Bus

function Bus.new()
    return setmetatable({
        _subs      = {},     -- name → array of {fn, token}
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
            if sub.token == token then table.remove(list, i); return end
        end
    end
end

function Bus:publish(name, payload)
    local list = self._subs[name]
    if not list then return end
    for _, sub in ipairs(list) do
        local ok, err = pcall(sub.fn, payload or {})
        if not ok then log.error("Bus subscriber for '%s' errored: %s", name, tostring(err)) end
    end
end
```

~25 lines total. Sync dispatch, pcall-guarded subscribers (one bad subscriber doesn't break others), token-based unsubscribe.

**Explicitly NOT in the bus:** queued publish, priority/order beyond subscription order, wildcards/filters, replay/buffering, async dispatch, declared event schemas. Add only if a concrete use case demands them.

### `ServiceLocator` integration

`ServiceLocator` is unchanged. The Application registers each service into BOTH places:
- `app.services` (ordered list) — for lifecycle dispatch
- `ServiceLocator` (named map) — for cross-service lookup by name

Services access each other via `ServiceLocator.get(name)` OR by being passed the `app` in their `init(app)` hook (so they can grab `app.bus` and other services). Both paths are valid; pick whichever reads cleaner per call site.

### Game-specific state migration

The current `state.gameState = GAME_STATE.LOGIN` and `state.gameLoaded = false` writes in main.lua move into `Game`'s constructor as instance fields (`game.gameState`, `game.gameLoaded`). Other modules that read `state.gameState` either (a) migrate to `ServiceLocator.get("game").gameState` (preferred long-term) or (b) keep reading via the state proxy — since Game gets auto-registered, `state.gameState` continues to resolve through ServiceLocator. Same proxy fallback applies to camera, etc.

---

## Service protocol + dispatch

Five duck-typed hooks. Services implement only what they care about; the App's dispatcher skips missing ones.

### The protocol

```lua
-- Optional methods on any registered service:
service:init(app)              -- once, after all services registered
service:fixedUpdate(fixedDt)   -- N times per real frame, fixedDt = 1/60 by default
service:update(dt)             -- once per real frame; reads/writes authoritative state
service:draw(alpha)            -- once per real frame; alpha = accumulator residual for render interp
service:shutdown()             -- once, on love.quit (reverse registration order)
```

**Alpha lives on draw, not update.** Render-side smoothing (lerping a Box2D body's
display position between `_prevX` captured at the last fixed step and the current
authoritative `e.position`) belongs in the draw path. Update reads + writes
authoritative state -- AI / collision / quest scripts that consume `e.position`
during update get the true post-step value, not a visually smoothed one. Renderers
consume `alpha` via the service's `:draw(alpha)` parameter, which the App threads
through from `_lastAlpha` stashed at the end of `:update`.

No required methods. A service can be an instance (`Game.new()`) or a module (`require "network"`); the App dispatches via `svc:method(...)` and Lua's colon-call ignores the implicit `self` for modules whose methods are plain functions.

### Application's `update` is the load-bearing piece

```lua
function Application:update(dt)
    -- 1) FPS cap (unchanged from current main.lua)
    local cap = Settings.fpsCap()
    if cap > 0 then ... apply _frameDeadline pacing ... end

    -- 2) Fixed-step accumulator + spiral-of-death guard
    self._accum = self._accum + dt
    local fixedDt = self._fixedDt
    local steps   = 0
    while self._accum >= fixedDt and steps < 5 do
        for _, entry in ipairs(self.services) do
            if entry.svc.fixedUpdate then entry.svc:fixedUpdate(fixedDt) end
        end
        self._accum = self._accum - fixedDt
        steps = steps + 1
    end
    if steps >= 5 then
        -- Hitch: drop residual accumulator. Log + count for diagnostics.
        Profiler:counter("fixed.spiralGuards", 1)
        self._accum = 0
    end

    -- 3) Variable update (authoritative state). Stash alpha for the draw path.
    self._lastAlpha = self._accum / fixedDt
    for _, entry in ipairs(self.services) do
        if entry.svc.update then entry.svc:update(dt) end
    end
end

function Application:draw()
    local alpha = self._lastAlpha or 0
    for _, entry in ipairs(self.services) do
        if entry.svc.draw then entry.svc:draw(alpha) end
    end
end
```

### Fixed-step parameters

| | Value | Why |
|---|---|---|
| Fixed rate | 60 UPS (`fixedDt = 1/60`) | Matches GachaCombat server `GameLoop.hpp` cadence; standard physics step |
| Configurable | `Application.new({ fixedRate = 60 })` | Future-proof without committing |
| Spiral guard | Max 5 fixed steps per real frame; drop excess | A 200ms hitch shouldn't cascade |
| Alpha | Passed as `service:draw(alpha)` | Optional arg; ignore if not interpolating. NOT passed to `:update` -- update is for authoritative state, render-side interpolation lives in `:draw`. |
| Order within tick | All `fixedUpdate` before any `update` before any `draw` | Physics → game logic → render |
| Bus events on tick | None by default | Add `app.fixed_tick` only if observers need it |

### Order of execution within one real frame

```
1.  fixedUpdate   (0..5 times, in registration order)
       network → input → game → jobs → ui → render
2.  update        (1 time, in registration order, with alpha)
       same order
3.  draw          (1 time, in registration order)
       same order
```

### Registration order is the only order

No priorities, no dependency declarations, no auto-topo-sort. `main.lua` registers services in the order they need to run; reordering is editing that block.

```lua
app:registerService("network", require "network")
app:registerService("input",   InputDispatch.new())
app:registerService("game",    Game.new())
app:registerService("jobs",    require "services.jobs.JobSystem")
app:registerService("ui",      UI)
app:registerService("render",  Pipeline.new())
```

### `init(app)` is where services see each other

By the time any service's `init` runs, all services are already in `ServiceLocator`. `init` is where a service grabs `app.bus` to subscribe + grabs other services to wire dependencies.

```lua
function Game:init(app)
    app.bus:subscribe("network.reconnectNeedsAuth", function() self:doLogout("Connection restored") end)
    app.bus:subscribe("network.sessionExpired",     function(e) self:doLogout("Session: " .. e.reason) end)
    self.input = ServiceLocator.get("input")
end
```

### `shutdown()` mirrors `init`

Walked in REVERSE registration order on `love.quit`. The last thing registered tears down first. Each service that allocated something in `init` cleans up here.

### What does NOT change

- LÖVE's `love.load` / `update` / `draw` / `quit` / `keypressed` / etc. callbacks still exist. They're bound to `app:load()`, `app:update(dt)`, etc., in one place (`Application:bindLove()`).
- Profiler scoping (`Profiler:scope("update.network")` etc.) stays inline inside each service's `update` — same scopes as today, dispatched centrally instead of inline.

---

## Module decomposition

Six modules. Five new files; `main.lua` shrinks to ~40 lines.

### `engine/bootstrap.lua` (new, ~50 lines)

The before-everything-else block currently at the top of `main.lua` (lines 1–76). Owns stdout unbuffering, runtime.log tee on global `print()`, `CrashLog.install()`, `Trace` setup. No dependencies on the Application or any service — just runs before anything that can error.

Required by `main.lua` BEFORE `Application.new()` so a crash during App construction still produces a crash report.

### `engine/Application.lua` (new, ~150 lines)

Detailed above. Class + dispatch loop + `bindLove()`. No game-specific logic.

### `engine/Bus.lua` (new, ~30 lines)

Detailed above. Pub/sub primitive.

### `engine/InputDispatch.lua` (new, ~180 lines)

LÖVE input callback adapters — the block currently at lines 879–1033 in `main.lua`. Owns `keypressed` / `keyreleased` / `textinput` / `mousepressed` / `mousereleased` / `mousemoved` / `wheelmoved` / `gamepadpressed` / `gamepadreleased` / `gamepadaxis` / `resize`.

Implements the service protocol — `:init(app)` grabs UI + Game + Input service references; the per-callback methods are called by the App's LÖVE delegate. Stays a single-consumer dispatcher; does NOT broadcast to bus (input events have one consumer per kind; bus is for observable app events, not single-target dispatch).

DEV keybinds (F1 = gacha, F2 = inventory, F3 = party, F4 = combat test, F5 = combat) live here as `_devKeybind(key)`.

### `systems/render/Pipeline.lua` (new, ~280 lines)

The render-graph composition currently at lines 575–858 in `main.lua`. Owns `composeScene` / `composeScreensScene` / `produceUI` / `produceHud` / `produceLayer` / `resolveFrame` / `composeFrame` / `worldProducer` / `hdrSupported` and the Renderer build.

The lazy-build pattern (`_renderer = built lazily on first draw`) goes away. `Pipeline:init(app)` runs from inside the App's `load()` — `love.graphics` is available there, so we build the Renderer eagerly in `init` and drop the nil-check at the top of every draw.

`worldProducer` is currently a closure over `state.gameLoaded` and the world tilemap. After the split, it grabs `self.game = ServiceLocator.get("game")` in `init`, then reads `self.game.gameLoaded` / `self.game.world` in `worldProducer`. No global `state` reads in the producer.

`Pipeline:draw()` is the entry point the App calls — sets up the FrameCtx, calls `composeFrame(ctx)`, done.

Lives at `systems/render/Pipeline.lua` (not `engine/`) — sits alongside `Renderer.lua` / `Profile.lua` / `RenderTargets.lua` / etc. as part of the render system.

### `game/Game.lua` (new, ~280 lines)

The session-lifecycle block currently at lines 78–373 in `main.lua`. Owns:
- `gameState` (LOGIN / LOADING / PLAYING) and `gameLoaded` as instance fields
- `loadGameWorld()`, `doLogout(message)`, `autoResume(cached, landing)`, `makeLanding()`, `showLogin()`
- Network event subscriptions (`reconnecting` / `reconnected` / `reconnectNeedsAuth` / `reconnectFailed` / `sessionExpired`) — wired in `Game:init(app)` via `Network.on(...)` (NOT bus; Network.on retrofit deferred to `[[project_ui_event_bus]]`)
- The `guarded(label, fn)` error wrapper if used only by Game (otherwise moves to a utilities module)

**Deliberately NOT in Game:** a formal state machine class with transitions/guards; "scene" terminology; any abstraction designed for the future scene system. When the scene system lands later, the LOGIN/LOADING/PLAYING state vars and the show-landing/show-login logic likely get absorbed into it. Designing for that now would over-engineer Game.

Game publishes its OWN events onto the bus going forward — `bus:publish("game.session_started")`, `bus:publish("game.session_ended", {reason})`, `bus:publish("game.world_loaded")` — so future consumers have a clean attach point.

### `main.lua` (refactored, ~40 lines)

```lua
require "boot"
require "engine.bootstrap"   -- stdout / CrashLog / Trace / runtime.log tee

local Application = require "engine.Application"
local app = Application.new({ fixedRate = 60 })

-- DISPATCH ORDER — DO NOT REORDER WITHOUT TESTING
app:registerService("network",  require "network")
app:registerService("input",    require("engine.InputDispatch").new())
app:registerService("game",     require("game.Game").new())
app:registerService("jobs",     require "services.jobs.JobSystem")
app:registerService("ui",       require "ui")
app:registerService("render",   require("systems.render.Pipeline").new())

-- CLI argparse (--profile-capture, --export-schema)
if arg then
    -- ... existing handling, mutates app config before bindLove ...
end

app:bindLove()
```

Two things stay in main.lua because they're genuinely entry-point concerns:
- `require "boot"` and `require "engine.bootstrap"` — must run first
- CLI argparse — visible-at-the-top intent for `--profile-capture` / `--export-schema`

### File tree summary

```
GachaClient/
├── main.lua                            (40 lines, was 1038)
├── boot.lua                            (existing, untouched)
├── engine/
│   ├── bootstrap.lua                   (new)
│   ├── Application.lua                 (new)
│   ├── Bus.lua                         (new)
│   └── InputDispatch.lua               (new)
├── systems/render/
│   └── Pipeline.lua                    (new — sits alongside Renderer.lua, Profile.lua, etc.)
└── game/
    └── Game.lua                        (new)
```

---

## Migration plan

9 phases (M0–M8). Each ends in a commit; each commit ships green harnesses + (where user-facing) a visual gate.

### M0 — Build `Application` + `Bus` + `bootstrap` (1 session, headless TDD)

**Files created:**
- `engine/Bus.lua`
- `engine/Application.lua`
- `engine/bootstrap.lua`
- `tests/render_harness/main.lua` — new test block: Bus (subscribe / publish / unsubscribe, pcall isolation between subscribers, token uniqueness, unknown-event no-op) + Application (registerService order, duck-typed dispatch, fixedUpdate accumulator behavior with synthetic dt sequences, spiral-guard caps at 5 steps, init/shutdown reverse order)

**Validation:** render harness green. Nothing in main.lua changes yet — App is built and tested in isolation.

### M1 — Extract `bootstrap` from main.lua (0.5 session, mechanical)

Replace main.lua lines 1–76 with `require "engine.bootstrap"`. Verify runtime.log + crash.log files are written identically.

**Validation:** engine boots; controlled crash via `error("test")` in love.load produces the same `crash.log` as before; runtime.log still tees stdout.

### M2 — Stand up App alongside existing main.lua (0.5 session, scaffolding)

Construct `Application.new()` near the end of main.lua's current flow. Don't bind LÖVE callbacks yet. `ServiceLocator.register("app", app)` so the rest of the codebase has a path to reach the bus.

**Validation:** engine boots, frame count captures normally, no behavior diff.

### M3 — First service migration: `network` (0.5 session)

Register Network as a service: `app:registerService("network", require "network")`. Network already has `Network.update(dt)` — duck-typed dispatch picks it up. Remove the `Network.update(dt)` call from main.lua's `love.update`. Run the P5a network validation capture to confirm `update.network.ms` is unchanged.

**Network.on stays as-is.** This phase does NOT retrofit Network's existing event surface onto the bus — that's `[[project_ui_event_bus]]` territory.

**Validation:** engine boot + post-cutover profiler capture matches `docs/audits/p5/network-after.csv`.

### M4 — Extract `InputDispatch` + bind LÖVE callbacks via App (1 session, visual gate)

Move main.lua lines 879–1033 into `engine/InputDispatch.lua`. Register as a service. `app:bindLove()` runs at the end of main.lua and installs callbacks that delegate to App methods, which call InputDispatch's `:keypressed(...)` etc.

This is the big "App takes over LÖVE binding" step. Previously-inline `love.load` setup migrates into each affected service's `init(app)`.

**Validation:** USER VISUAL GATE. Login click + type + tab navigation, click-to-play, F1 / F2 / F3 / F4 / F5 dev keybinds, in-screen mouse/wheel, gamepad if available. Quit cleanly.

### M5 — Extract `Pipeline` to `systems/render/Pipeline.lua` (1 session, visual gate)

Move main.lua lines 575–858. Register as the LAST service so it dispatches `draw()` last in the frame. Drop the lazy `_renderer` build — `Pipeline:init(app)` builds the Renderer eagerly.

`worldProducer` becomes a method on Pipeline that reads `self.game` (grabbed in `init` via `ServiceLocator.get("game")`).

**Validation:** USER VISUAL GATE. Every screen renders identically: login, landing, gacha, inventory, party, dailies, combat, post-process grade unchanged. Profile capture confirms no scope regression.

### M6 — Extract `Game` to `game/Game.lua` (1 session, visual gate)

Move main.lua lines 78–373. Register as a service. Network event subscriptions move into `Game:init(app)`, still calling `Network.on(...)` (not bus). Game publishes its own bus events going forward.

`state.gameState` / `state.gameLoaded` become `self.gameState` / `self.gameLoaded` (instance fields). Other modules that read `state.gameState` continue to work via the state proxy + Game's auto-registration.

**Validation:** USER VISUAL GATE. Login → click play → world loads → F4 combat → quit. Reconnect storm (kill GachaAuth, restart). Auto-resume from saved session on relaunch.

### M7 — First `fixedUpdate` consumer: WaveGame + Box2D (1 session, visual gate)

Convert WaveGame's per-frame physics tick to `:fixedUpdate(fixedDt)`. Box2D's `world:step(fixedDt)` moves into the same hook. Alpha-interpolation in the render path (`:draw(alpha)` -> FrameCtx -> RenderSystem.drawEntity lerping between `_prevX` snapshot and current `e.position`) for rendering moving bodies between physics steps if visual smoothness needs it (start without, add if combat-style judder appears).

This validates the accumulator + alpha pattern under real load. Spiral guard exercised by manually stalling for ~200ms and watching that physics doesn't fast-forward.

**Validation:** USER VISUAL GATE. WaveGame feels at minimum as good as today — judder check, hitch check, physics doesn't drift when frame rate dips.

### M8 — Cleanup + main.lua final shape + memory update (0.5 session)

- main.lua reaches its ~40-line target shape
- Dead code surfaced during migration deleted
- `state` proxy still in place for legacy reads — retirement is its own future workstream
- Audit: `docs/audits/app-refactor/summary.md` records before/after line counts + commit SHAs
- Memory updates: a `project_app_architecture.md` entry documenting App + Bus + lifecycle protocol; `project_ui_event_bus.md` updated to reference the now-shipped bus

**Validation:** assets harness syntax-checks new files; engine smoke; final main.lua line count under 50.

### What stays unchanged across the entire migration

- LÖVE version, no new dependencies, no protocol changes
- ServiceLocator API and the `state` proxy backward compat
- Network.on event surface (retrofit deferred to `[[project_ui_event_bus]]`)
- Combat ABILITIES gameplay (boundary memory)
- F2 / F3 inventory / party menus (deferred per memory)
- All harnesses (assets / render / threading)
- Shader abstraction plan — this refactor doesn't depend on it and vice versa; either can land first

### Total budget

~7 sessions (M0: 1, M1: 0.5, M2: 0.5, M3: 0.5, M4: 1, M5: 1, M6: 1, M7: 1, M8: 0.5 = 7.0). Each phase is independently shippable.

---

## Risks and mitigations

### R1 — Service registration order is load-bearing and edited by hand

Dispatch order is registration order. The block in `main.lua` IS the source of truth — reorder it and the engine subtly breaks.

**Mitigation:** comment block `// DISPATCH ORDER — DO NOT REORDER WITHOUT TESTING` in main.lua. Plus a render-harness test that registers a probe service asserting which service ran before it; catches reorderings in CI.

### R2 — Bus is free-form; typos are silent no-ops

Chosen explicitly. `bus:publish("session.expred")` does nothing; `bus:subscribe("sesion.expired", fn)` never fires.

**Mitigation:** `docs/events.md` updated by hand listing every published event + payload shape. PR review enforces additions. If drift / typos become painful, upgrade to dev-mode declared validation later (a known follow-on path).

### R3 — pcall guards inside dispatch can mask errors

`Bus:publish` and `Application:update` / `draw` pcall-wrap individual subscribers / services so one bad one doesn't break others. That isolation can swallow errors.

**Mitigation:** pcall failures log at ERROR level via Logger (client.log + console + CrashLog trace ring buffer). In DEV builds (non-fused), App's dispatch logs every error verbosely; in release builds it logs once per service then suppresses (avoiding spam on stuck errors). One env-flag toggles.

### R4 — Spiral guard hides hitches silently

If `fixedUpdate` runs the max 5 times in one real frame and the accumulator still has residual, the residual is dropped. Right behavior for hitch recovery, but it means physics silently skips work.

**Mitigation:** when the guard fires, increment a `fixed.spiralGuards` profiler counter. Visible in the Profiler overlay and `--profile-capture` CSV columns. If the counter ticks during normal play, that's a real frame-time problem to investigate. Logging once per occurrence at WARN.

### R5 — Three concepts (ServiceLocator + App + Bus) need clear boundaries

A new reader sees `ServiceLocator.get("game")`, `app.services`, `app.bus:subscribe(...)` and reasonably asks "which is for what?"

**Mitigation:** one-pager at `docs/architecture/application.md` written during M0:
- `ServiceLocator` = name → service lookup (existing primitive)
- `Application` = lifecycle dispatcher (`:update` / `:draw` / `:fixedUpdate` in order)
- `Bus` = pub/sub for app events (observers; not lifecycle; not name lookup)

Plus a flowchart of "where does X live": frame-shaped hot work → service hook; observable event → bus; cross-service ref → ServiceLocator.

### R6 — Network.on retrofit deferred; two pub/sub patterns coexist

This workstream ships `Bus` but doesn't migrate `Network.on` onto it. For the indefinite period between this work and `[[project_ui_event_bus]]`, the codebase has two pub/sub shapes.

**Mitigation:** convention in the M6 commit message + a comment in `network.lua`: "Network.on is the legacy event surface; new subscribers prefer the Bus once Network is retrofitted." Game's M6 work uses `Network.on` for *existing* events but publishes its own *new* events via Bus. The patterns own disjoint event sets, so they coexist cleanly.

### R7 — Game will be absorbed by the future scene system

Game's GAME_STATE machine (LOGIN / LOADING / PLAYING) and show-login / show-landing logic are scene-system concerns in waiting.

**Mitigation:** keep Game deliberately un-formalized — instance fields, not a state-machine class; no transition guards, no scene callbacks, no scene terminology. When the scene system arrives, those bits lift cleanly without dragging architectural baggage. Refactor-later cost beats design-now-for-uncertain-future cost.

### R8 — `fixedUpdate` hook lands without a real first consumer

M0–M6 ship the App + Bus + service split. M7 is the first concrete `fixedUpdate` consumer (WaveGame + Box2D). Until M7 runs, the hook + accumulator + alpha + spiral guard are infrastructure with no validation under real load.

**Mitigation:** keep M7 in scope as the validation step for the whole fixed-update mechanism. If `fixedUpdate` needs a tweak after the first real workload exercises it (different signature, different alpha semantics, different spiral policy), do it in M7. Headless harness tests in M0 cover mechanical correctness; M7 covers "does it feel right."

### R9 — Profiler scoping currently lives inside service `update` methods

`Network.update` opens `Profiler:scope("update.network")` itself (committed `32cb39a`). After this refactor, the App dispatches updates centrally; should the App wrap each service's `update` in a scope using the service name?

**Mitigation:** keep inline. Existing scopes (`update.network`, `update.ui`, `update.world`, `update.jobs`) work and are calibrated for budgets. Wrapping in the App would either duplicate scopes or require deleting all inline ones (large mechanical change for no behavior gain). Re-evaluate after M6 — if a service lacks a scope, add inline.

### R10 — Net file count rises even as main.lua shrinks

Net +5 files (Bus, Application, bootstrap, InputDispatch, Pipeline, Game) for one removed responsibility (main.lua's kitchen-sink shape).

**Mitigation:** acknowledged trade-off. Five focused files at 50–280 LoC each beat one 1038-line file. Not a real risk; flagged so review isn't surprised.

---

## Cross-references

- Master spec: `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md`
- Rendering homogenization: `docs/superpowers/specs/2026-05-27-rendering-homogenization-design.md`
- Shader abstraction (parallel workstream): `docs/superpowers/specs/2026-05-28-shader-abstraction-design.md`
- Memory: `[[project_ui_event_bus]]`, `[[aaa-rendering-initiative]]`, `[[asset_manager]]`, `[[feedback_abilities_system_ownership]]`, `[[project_inventory_party_rework]]`

---

## Self-review

- [x] Aggressive scope (App + bus + ServiceLocator integration) but no DI / plugin loader / scene framework
- [x] Direct method dispatch for lifecycle hot path; bus for observable app events; no double-dispatch
- [x] Free-form bus, namespaced by convention; safety upgrade is a known follow-on
- [x] ServiceLocator stays as the registry; App composes with it instead of replacing
- [x] Five duck-typed hooks (init / fixedUpdate / update / draw / shutdown); services implement only what they need
- [x] Fixed-step at 60 UPS with accumulator + alpha + spiral guard; first consumer (WaveGame + Box2D) validates in M7
- [x] main.lua reaches ~40 lines; entry point + service registration + LÖVE binding
- [x] Migration is 9 phases (M0–M8), each independently shippable
- [x] Visual gates on M4 (input), M5 (render), M6 (game), M7 (fixedUpdate)
- [x] 10 risks enumerated with mitigations
- [x] Combat ABILITIES + F2/F3 deferrals respected
- [x] Network.on retrofit deferred to `[[project_ui_event_bus]]` to keep this workstream bounded
- [x] Future scene system explicitly accommodated by NOT over-formalizing Game
- [x] Shader abstraction workstream is independent; either can land first
- [x] No new dependencies, no protocol changes, no JSON schema changes
