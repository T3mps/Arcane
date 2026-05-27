# Rendering Phase 2b — Render-Graph Wiring & Pass Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the `RenderTargets` pool into the `Renderer`/frame `ctx`, split the single output-identical `legacy` pass into discrete profiled `scene` / `present` / `hud` passes, and migrate the full-frame AA canvas onto the pool — all parity-gated (identical pixels), establishing the graph the linear/HDR color pipeline and scene-unification ride on in Phase 2c.

**Architecture:** Phase 2a stood up `lib/Pool`, `systems/render/{RenderTargets,Pass,Renderer}` and routed `love.draw` through the Renderer via one `legacy` pass. This phase makes the Renderer own a `RenderTargets` instance (exposed to passes as `ctx.targets`, reset each frame), then peels the monolithic `legacy` pass into three ordered passes that reproduce today's exact draw sequence — `scene` (FrameAA-begin + world + UI + transition) → `present` (FrameAA-resolve/FXAA blit) → `hud` (FPS readout + warp flash, drawn sharp on top). Finally the AA frame canvas moves from FrameAA's private cache onto the pool, giving the pool its first real in-game consumer.

**Tech Stack:** LÖVE 11.x / LuaJIT. Headless unit tests via `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`. GL passes (Tasks 2–3) are verified by boot + visual + F9 profiler parity (LÖVE has no headless GL), exactly as Phase 2a Task 4 was.

**Spec:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` (Phase 2 — render-graph + render-target pool).

**Scope decisions (locked with the user, 2026-05-27):**
- **This plan = the tractable, output-identical slice only.** It wires the pool and splits the present-side passes that already live in `love.draw`. It does **not** unify the distributed scene→post→upscale chains that each renderer (`RenderSystem`, `WaveGame`, `CombatRenderer`, screen backgrounds) currently owns — that cross-cutting refactor needs its own research and is **deferred to Phase 2c**.
- **Color pipeline (linear-space + HDR `rgba16f` + tonemap) is deferred to Phase 2c.** This phase introduces NO look change. Scope of linear when it lands: **scene-only, UI stays sRGB** (industry-standard — engines render the scene in linear-HDR, tonemap, then draw UI on top in sRGB). **Tonemap operator: ACES filmic.** These are recorded here for 2c; they do not affect 2b.
- Every GL task is **parity-gated**: the game must look identical before proceeding.

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `GachaClient/systems/render/Renderer.lua` | Frame graph executor | Modify: own an optional `targets` (a `RenderTargets`); inject it as `ctx.targets`; call `targets:frameReset()` after the pass loop. |
| `GachaClient/ui/components/FrameAA.lua` | Full-frame AA capture/resolve | Modify (Task 3): add `beginInto(canvas)` (set+clear a caller-supplied canvas); remove the now-dead `begin()` and its private canvas cache. `active()`/`present()` unchanged. |
| `GachaClient/main.lua` | Frame entry | Modify: pass a `RenderTargets` to the Renderer (T1); replace the `legacy` pass with `scene`/`present`/`hud` passes and drop the `legacy` budget (T2); make `scene` acquire its AA canvas from `ctx.targets` (T3). |
| `GachaClient/tests/render_harness/main.lua` | Headless unit harness | Modify (T1): add a "Renderer targets wiring" section. |

Run command (from repo root `D:/dev/starworks/Gacha`): `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` → expect `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

Game launch (Tasks 2–3 visual verification): `GachaClient/run.bat`, press **F9** for the profiler overlay.

---

### Task 1: Wire `RenderTargets` into the `Renderer` and frame `ctx`

The Renderer gains an optional `targets` (a `RenderTargets` instance). When present, `frame(ctx)` exposes it to every pass as `ctx.targets` and calls `targets:frameReset()` once after all passes run (returning per-frame transient canvases to the pool). When absent, behavior is unchanged (so the existing `== Renderer ==` test, which injects no targets, still passes). Pure-logic — headless-testable with a fake targets object.

**Files:** Modify `GachaClient/systems/render/Renderer.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert immediately before the final `if fails == 0` line of `GachaClient/tests/render_harness/main.lua`:
```lua
print("== Renderer targets wiring ==")
do
    local Renderer = require "systems.render.Renderer"
    local Pass     = require "systems.render.Pass"
    local events = {}
    local fakeTargets = { frameReset = function() events[#events + 1] = "reset" end }
    local sawTargets
    local r = Renderer.new({ targets = fakeTargets })
    r:add(Pass.new{ name = "p", draw = function(ctx) sawTargets = ctx.targets; events[#events + 1] = "draw" end })
    r:frame({ dt = 1 })
    eq(sawTargets, fakeTargets, "ctx.targets injected for passes")
    eq(table.concat(events, ","), "draw,reset", "frameReset runs once, after all passes")
    -- No targets supplied: ctx.targets stays nil, no crash, no frameReset.
    local r2 = Renderer.new({})
    local t2 = "unset"
    r2:add(Pass.new{ name = "q", draw = function(ctx) t2 = ctx.targets end })
    r2:frame({ dt = 1 })
    eq(t2, nil, "no targets -> ctx.targets nil")
end
```

- [ ] **Step 2: Run → FAIL.**
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: a FAIL line for `ctx.targets injected for passes` (currently `ctx.targets` is nil because the Renderer never sets it) and `frameReset runs once, after all passes` (no frameReset call), so the run ends with `N FAILED`, exit 1.

- [ ] **Step 3: Implement** — in `GachaClient/systems/render/Renderer.lua`, store `targets` in `new` and use it in `frame`.

In `Renderer.new`, add the `targets` field. Change:
```lua
function Renderer.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Renderer)
    self.passes   = {}     -- ordered array
    self.byName   = {}     -- name -> pass
    self.profiler = opts.profiler   -- expects :scope(name) -> finish closure; optional
    return self
end
```
to:
```lua
function Renderer.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Renderer)
    self.passes   = {}     -- ordered array
    self.byName   = {}     -- name -> pass
    self.profiler = opts.profiler   -- expects :scope(name) -> finish closure; optional
    self.targets  = opts.targets    -- a RenderTargets pool; exposed as ctx.targets, reset per frame; optional
    return self
end
```

Then change `Renderer:frame` to inject `ctx.targets` before the loop and reset after it. Change:
```lua
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
```
to:
```lua
function Renderer:frame(ctx)
    if self.targets then ctx.targets = self.targets end
    for i = 1, #self.passes do
        local pass = self.passes[i]
        if isEnabled(pass, ctx) then
            local done
            if self.profiler then done = self.profiler:scope(pass.name) end
            pass.draw(ctx)
            if done then done() end
        end
    end
    -- Transient render targets acquired this frame return to the pool (reused next frame).
    if self.targets then self.targets:frameReset() end
end
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0). The pre-existing `== Renderer ==` section (no `targets`) still passes because the `if self.targets` guards skip injection and reset.

- [ ] **Step 5: Wire a real `RenderTargets` in `main.lua`.** This change is output-identical (the pool is constructed and reset each frame but the single `legacy` pass does not yet acquire from it). Near the top requires of `main.lua`, after:
```lua
local Renderer         = require "systems.render.Renderer"
local Pass             = require "systems.render.Pass"
```
add:
```lua
local RenderTargets    = require "systems.render.RenderTargets"
```
Then in `love.draw`, change the lazy build from:
```lua
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler })
        _renderer:add(Pass.new{ name = "legacy", draw = function() drawLegacy() end })
    end
```
to:
```lua
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler, targets = RenderTargets.new() })
        _renderer:add(Pass.new{ name = "legacy", draw = function() drawLegacy() end })
    end
```

- [ ] **Step 6: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Renderer.lua GachaClient/tests/render_harness/main.lua GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): wire RenderTargets pool into Renderer ctx + per-frame reset"
```

---

### Task 2: Split the `legacy` pass into `scene` / `present` / `hud`

The monolithic `drawLegacy` body is divided at its natural seam. Today the order is: FrameAA-begin → world → UI(+shake) → transition → **FrameAA.present** → FPS readout → warp flash. The `present` blit sits in the MIDDLE: content is AA-resolved to the screen, then the FPS readout and warp flash are drawn **sharp on top** (not through AA). So the split must be three passes, not two: everything-into-the-AA-canvas (`scene`), the resolve (`present`), the sharp-on-top overlays (`hud`). The captured frame canvas crosses passes via `ctx.frame`. Output is pixel-identical; the profiler now shows three scopes instead of one `legacy`.

**Files:** Modify `GachaClient/main.lua`.

- [ ] **Step 1: Replace `drawLegacy` with three pass functions.** The current `main.lua` has (from Phase 2a) a `local function drawLegacy()` immediately before `function love.draw()`. Replace that ENTIRE `drawLegacy` function with these three `local function`s (the body is the same code, partitioned at the seam; `ctx.frame` carries the captured canvas from `scene` to `present`):
```lua
-- Scene pass: capture the frame (when AA is active), draw the world (only when visible) and
-- the full UI stack (with the warp-launch screen-shake), then the screen transition. Everything
-- here composes into the AA canvas that `present` resolves.
local function drawScene(ctx)
    -- Anti-aliasing: when FXAA/MSAA is on, render the whole frame into a canvas; `present`
    -- blits it through FXAA / resolves MSAA. (ctx.frame stays nil when aa == "none".)
    local FrameAA = require "ui.components.FrameAA"
    ctx.frame = FrameAA.active() and FrameAA.begin() or nil

    -- Only draw the overworld when it is actually visible (no screen covering it).
    if state.gameState == GAME_STATE.PLAYING and state.world and not UI.hasScreens() then
        state.world:update(0, nil, "draw")
    end

    -- Warp-launch FX: a brief camera shake around the UI (overscanned so the offset shows no edge).
    local SpaceBackground = require "ui.components.SpaceBackground"
    local sx, sy = SpaceBackground.getShake()
    if not require("services.Settings").get("screenShake") then sx, sy = 0, 0 end
    if sx ~= 0 or sy ~= 0 then
        local w, h = love.graphics.getDimensions()
        love.graphics.push()
        love.graphics.translate(w * 0.5 + sx, h * 0.5 + sy)
        love.graphics.scale(1.04)
        love.graphics.translate(-w * 0.5, -h * 0.5)
        UI.draw()
        love.graphics.pop()
    else
        UI.draw()
    end

    UI.drawTransition()
end

-- Present pass: resolve the captured frame to the screen (FXAA shader / MSAA resolve). No-op
-- when aa == "none" (ctx.frame is nil and content already went straight to the screen).
local function drawPresent(ctx)
    if ctx.frame then
        local FrameAA = require "ui.components.FrameAA"
        FrameAA.present(ctx.frame)
    end
end

-- HUD pass: the FPS readout and the warp flash, drawn sharp on top of the resolved frame
-- (deliberately AFTER present so they are not anti-aliased / blurred).
local function drawHud(ctx)
    if require("services.Settings").get("showFps") then
        love.graphics.setColor(0, 0, 0, 0.45)
        love.graphics.rectangle("fill", 6, 6, 62, 20, 3, 3)
        love.graphics.setColor(0.6, 0.95, 1.0, 1)
        love.graphics.print("FPS " .. love.timer.getFPS(), 12, 7)
        love.graphics.setColor(1, 1, 1, 1)
    end

    local SpaceBackground = require "ui.components.SpaceBackground"
    local flash = SpaceBackground.getFlash()
    if flash > 0 then
        local w, h = love.graphics.getDimensions()
        love.graphics.setColor(1, 1, 1, flash)
        love.graphics.rectangle("fill", 0, 0, w, h)
        love.graphics.setColor(1, 1, 1, 1)
    end
end
```

- [ ] **Step 2: Register the three passes instead of `legacy`.** In `love.draw`, change the lazy build from:
```lua
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler, targets = RenderTargets.new() })
        _renderer:add(Pass.new{ name = "legacy", draw = function() drawLegacy() end })
    end
```
to:
```lua
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler, targets = RenderTargets.new() })
        _renderer:add(Pass.new{ name = "scene",   draw = drawScene,   budgetMs = 8.0 })
        _renderer:add(Pass.new{ name = "present", draw = drawPresent, budgetMs = 1.0 })
        _renderer:add(Pass.new{ name = "hud",     draw = drawHud,     budgetMs = 0.5 })
    end
```
(The pass `budgetMs` values are now registered with the profiler automatically by `Renderer:add` — wired in Phase 2a — so the overlay shows `scene 0.xx/8.0`, `present 0.xx/1.0`, `hud 0.xx/0.5`.)

- [ ] **Step 3: Drop the obsolete `legacy` budget.** In `love.load`, the Phase-2a line `Profiler:setBudget("legacy", 8.0)` now refers to a scope that no longer exists. Delete that one line (leave the `update.network` / `update.world` budgets). Find:
```lua
    Profiler:setBudget("legacy", 8.0)
    Profiler:setBudget("update.network", 1.0)
    Profiler:setBudget("update.world", 3.0)
```
and change it to:
```lua
    Profiler:setBudget("update.network", 1.0)
    Profiler:setBudget("update.world", 3.0)
```

- [ ] **Step 4: Verify in-game (visual + profiler parity).** This is NOT headless-verifiable — confirm by launching.
Run: `GachaClient/run.bat`.
Expected: the game looks **identical** — login screen, overworld, any open menu, the screen transition, the FPS readout (toggle Settings → Video → Show FPS), and the warp-launch flash all render exactly as before, with the FPS/flash still crisp on top. Press **F9**: the overlay now shows three rows — `scene`, `present`, `hud` — replacing the single `legacy` row, with `scene` ms ≈ the old `legacy` ms and `present`/`hud` tiny. No crash, no flicker. (If you cannot launch, say so — do not claim visual parity you did not observe.)

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): split legacy pass into scene/present/hud passes (parity)"
```

---

### Task 3: Migrate the full-frame AA canvas onto the `RenderTargets` pool

Today `FrameAA.begin()` creates and caches its own full-frame canvas (`love.graphics.newCanvas` via `Settings.newCanvas`, recreated on resize / graphics-generation change). This task moves that canvas onto the pool: the `scene` pass acquires it from `ctx.targets` each frame, and `Renderer:frame`'s `targets:frameReset()` (Task 1) returns it for reuse. This is the pool's **first real in-game consumer** — it exercises `Pool:acquire`/`frameReset` against a live LÖVE `Canvas` (userdata), validating the Phase-2a fix that removed the `_poolKey` write (writing arbitrary fields onto userdata crashes). It is output-identical: the pooled canvas is created with the exact same parameters FrameAA used.

**Why parity holds:** `Settings.newCanvas(w,h,{msaa=Settings.canvasMSAA()})` reduces to `love.graphics.newCanvas(w,h,{msaa = m>=2 and m or nil})`. `RenderTargets`' default factory does the same: `acquire(w,h,nil,Settings.canvasMSAA())` yields `love.graphics.newCanvas(w,h, {msaa = m>=2 and m or nil})` — identical canvas. The pool keys on `WxH|fmt|msaa`, so a resize or AA-mode change naturally allocates a new bucket (matching FrameAA's old resize-recreate), and stale-size canvases are simply never reused.

**Files:** Modify `GachaClient/ui/components/FrameAA.lua`; Modify `GachaClient/main.lua`.

- [ ] **Step 1: Add `FrameAA.beginInto(canvas)` and remove the dead `begin()` + its private cache.** In `GachaClient/ui/components/FrameAA.lua`:

Remove the canvas-cache locals (the FXAA shader locals stay). Change:
```lua
local shader, shaderTried
local canvas, cw, ch, cgen
```
to:
```lua
local shader, shaderTried
```

Replace the entire `FrameAA.begin` function:
```lua
function FrameAA.begin()
    local W, H = love.graphics.getDimensions()
    local gen  = Settings.gfxGeneration()
    if not canvas or cw ~= W or ch ~= H or cgen ~= gen then
        canvas = Settings.newCanvas(W, H, { msaa = Settings.canvasMSAA() })
        cw, ch, cgen = W, H, gen
    end
    love.graphics.setCanvas(canvas)
    love.graphics.clear(0, 0, 0, 1)
    return canvas
end
```
with:
```lua
--- Begin capturing the frame into a caller-supplied canvas (from the RenderTargets pool):
--- set it as the target and clear it, returning it. The canvas's MSAA/size is the caller's
--- responsibility (the pool keys on them); this is the one canvas that opts into MSAA when
--- aa == "msaa". The whole frame draws into it and resolves on present().
function FrameAA.beginInto(canvas)
    love.graphics.setCanvas(canvas)
    love.graphics.clear(0, 0, 0, 1)
    return canvas
end
```
Leave `FrameAA.active()` and `FrameAA.present(frame)` unchanged. (`Settings` is still required for `active()`/`canvasMSAA` callers, and `Assets` for the FXAA shader — leave both requires.)

- [ ] **Step 2: Acquire the AA canvas from the pool in the `scene` pass.** In `GachaClient/main.lua`, change the top of `drawScene`:
```lua
local function drawScene(ctx)
    -- Anti-aliasing: when FXAA/MSAA is on, render the whole frame into a canvas; `present`
    -- blits it through FXAA / resolves MSAA. (ctx.frame stays nil when aa == "none".)
    local FrameAA = require "ui.components.FrameAA"
    ctx.frame = FrameAA.active() and FrameAA.begin() or nil
```
to:
```lua
local function drawScene(ctx)
    -- Anti-aliasing: when FXAA/MSAA is on, capture the whole frame into a pooled canvas;
    -- `present` blits it through FXAA / resolves MSAA. (ctx.frame stays nil when aa == "none".)
    local FrameAA  = require "ui.components.FrameAA"
    local Settings = require "services.Settings"
    if FrameAA.active() then
        local cv = ctx.targets:acquire(ctx.viewportW, ctx.viewportH, nil, Settings.canvasMSAA())
        ctx.frame = FrameAA.beginInto(cv)
    else
        ctx.frame = nil
    end
```

- [ ] **Step 3: Provide the viewport in `ctx`.** `drawScene` now reads `ctx.viewportW/H`. In `love.draw`, change:
```lua
    _renderer:frame({ dt = love.timer.getDelta() })
```
to:
```lua
    local vw, vh = love.graphics.getDimensions()
    _renderer:frame({ dt = love.timer.getDelta(), viewportW = vw, viewportH = vh })
```

- [ ] **Step 4: Verify in-game (visual + profiler parity), all AA modes.** Run: `GachaClient/run.bat`.
Expected: identical rendering in every AA mode — set Settings → Video → Anti-aliasing to **None**, **FXAA**, and **MSAA** in turn and confirm each looks exactly as before (None = direct-to-screen, no canvas; FXAA = edge-smoothed; MSAA = resolved). Resize the window and confirm no corruption (the pool allocates a new-size canvas). Press **F9**: `scene`/`present`/`hud` rows still present, `draws`/`switches`/`vram` counters comparable to before (one pooled canvas instead of FrameAA's cached one — VRAM should not grow frame-over-frame, confirming reuse). No crash on first frame (this is the first time a real `Canvas` flows through `Pool:acquire`). (If you cannot launch, say so — do not claim parity you did not observe.)

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/ui/components/FrameAA.lua GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): move full-frame AA canvas onto the RenderTargets pool"
```

---

## Self-review (completed)

- **Spec coverage (2b slice):** `RenderTargets` wired into the frame loop with per-frame transient release → T1; the spec's migration "step 2 — peel off present" → realized as the T2 `scene`/`present`/`hud` split (three passes because the FPS/flash overlays must stay sharp on top, after the resolve); "RenderTargets replaces the FrameAA canvas" → T3. The deeper "peel post/scene out, then add effects/lights" and the linear/HDR/tonemap color pipeline are explicitly **deferred to Phase 2c** (the user chose the split scope after the finding that scene→post→present is distributed across renderers, not centralized in `love.draw`).
- **Placeholders:** none — every code step shows the exact before/after. The two GL tasks (T2, T3) use boot + visual + F9 parity verification because LÖVE GL is not headless-testable; this is called out, not hand-waved (same pattern as Phase 2a Task 4).
- **Type/name consistency:** `Renderer.new{ profiler, targets }` and `Renderer:frame(ctx)` setting `ctx.targets` then calling `targets:frameReset()` match the `RenderTargets:acquire(w,h,fmt,msaa)` / `:frameReset()` interface from Phase 2a. `ctx.frame` is written in `drawScene` and read in `drawPresent`. `ctx.viewportW/H` is written in `love.draw` (T3) and read in `drawScene` (T3). `FrameAA.beginInto(canvas)` (added T3) replaces `FrameAA.begin()` (removed T3); the only caller is `drawScene`. Pass `budgetMs` registration relies on the Phase-2a `Renderer:add` wiring.
- **Parity discipline:** T1 is output-identical (pool constructed + reset, not yet acquired from). T2 reproduces the exact draw order. T3 produces a byte-identical canvas (verified arithmetic against `Settings.newCanvas`). No look change anywhere in 2b.

## Next: Phase 2c (own plan, needs research)

`docs/superpowers/plans/2026-05-27-rendering-p2c-*.md`. Two coupled efforts, both needing investigation of `RenderSystem`, `WaveGame`, `CombatRenderer` (rendering plumbing only — the abilities system is off-limits), the screen-background system, and `FrostedGlass`:
1. **Scene/post unification:** have renderers draw into a shared `sceneRT` from `ctx.targets` instead of each owning its scene→`PostFx.apply`→`RenderScale.upscale` chain; introduce a single `post` pass (PostFx) sampling `sceneRT`; collapse the reveal's 2–3× background re-render (`effects` samples `sceneRT`). Also migrate `RenderScale`'s `midCanvas` onto the pool.
2. **Color pipeline:** make `sceneRT` `rgba16f` **linear**; add a `tonemap` pass (**ACES filmic**) mapping linear-HDR → sRGB as the final pre-UI step; re-tune `PostFx` for linear input. **Scope: scene-only — UI stays sRGB, drawn after tonemap** (industry-standard seam). This is the one deliberate, NOT-parity look change; gate it with before/after screenshots accepted as intentional.
