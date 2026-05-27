# Rendering Phase 2c-1c — Layered Compositor (consume composePlan) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `main.lua`'s 2b `scene`/`present`/`hud` Renderer passes with a single layered compositor that executes `Profile.composePlan` (finally consuming the Plan 2c-1′ layer system) — output-identical, no behavior change.

**Architecture:** A `produceLayer(layer, ctx)` dispatch maps a layer's `source` to its producer (`world` → existing `composeWorld`; `ui` → screen-shake + `UI.draw` + transition; `hud` → FPS + flash; `background`/`none` → no-op until 2c-2). `composeFrame(ctx)` picks the frame profile (`UI.hasScreens()` → `Profile.PLAIN_UI`, else `Profile.WORLD`), runs `Profile.composePlan`, and executes each group: `aa` groups render their layers into a pooled `FrameAA` canvas and resolve once; sharp groups draw to the screen. `love.draw` runs it as one `compose` Renderer pass (keeping `ctx.targets` injection + `frameReset`); per-layer `Profiler:scope`s keep the F9 breakdown.

**Tech Stack:** LÖVE 11.x / LuaJIT. Pure-logic frame-profile selection gets a headless harness check; the GL compositor is **visual + F9 parity, human-verified** (not headless), like the 2b/2c-1b migrations.

**Spec:** `docs/superpowers/specs/2026-05-27-rendering-2c-design.md`.

**Why output-identical:** today (post-2c-1b) the overworld frame = `FrameAA.beginInto` → `composeWorld` (world→sceneRT→grade→upscale into AA) + `UI.draw` into AA → `FrameAA.present` → FPS/flash sharp; a screen frame = `FrameAA.beginInto` → `UI.draw` (renders the screen's own bg/post/widgets) into AA → `present` → FPS/flash sharp. The compositor reproduces both: `WORLD` → `composePlan` = `[{aa,{scene,ui}},{sharp,{hud}}]`; `PLAIN_UI` → `[{aa,{ui}},{sharp,{hud}}]`. The `scene` layer is a no-op for screens (no `world`/`background` producer there yet), so screen bg keeps coming through `UI.draw` exactly as now.

**Scope:** structural refactor only. `selectActive` + per-screen `renderProfile` (multi-screen profiles), screen scene producers + `frosted`, and the linear/HDR flip are all later (2c-2 / 2c-3).

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `GachaClient/main.lua` | `composeWorld` (kept) + new `produceUI`/`produceHud`/`produceLayer`/`composeFrame`; `love.draw` runs one `compose` pass; `love.load` sets scene/ui/present/hud budgets | Modify |
| `GachaClient/tests/render_harness/main.lua` | `== frame profile selection ==` headless check (pure logic) | Modify |

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`. Game: `GachaClient/run.bat`, F9.

---

### Task 1: `Profile.frameProfile` — pure frame-profile selection (headless)

A trivial pure helper so the overworld-vs-screen profile choice is unit-tested and reusable; `composeFrame` calls it.

**Files:** Modify `GachaClient/systems/render/Profile.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Profile frameProfile ==")
do
    local Profile = require "systems.render.Profile"
    -- no screens -> WORLD; screens present -> PLAIN_UI (2c-1c: screens render via UI.draw)
    eq(Profile.frameProfile(false).layers[1].source, "world", "no screens -> WORLD")
    eq(#Profile.frameProfile(true).layers, 2, "screens -> PLAIN_UI (ui+hud)")
    eq(Profile.frameProfile(true).layers[1].source, "ui", "screens -> first layer ui")
end
```

- [ ] **Step 2: Run → FAIL** (`Profile.frameProfile` is nil → `attempt to call ... a nil value`).
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 3: Implement** — add to `GachaClient/systems/render/Profile.lua`, immediately BEFORE the final `return Profile`:
```lua
-- Choose the frame's profile from whether any screen is shown. For 2c-1c: overworld -> WORLD;
-- any screen -> PLAIN_UI (the screen's own background still renders inside the ui layer's UI.draw
-- until screen producers land in 2c-2). selectActive + per-screen renderProfile arrive in 2c-2.
function Profile.frameProfile(hasScreens)
    if hasScreens then return Profile.PLAIN_UI end
    return Profile.WORLD
end
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0).

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Profile.frameProfile (overworld->WORLD, screens->PLAIN_UI)"
```

---

### Task 2: Restructure `love.draw` into the layered compositor (atomic, parity-gated)

Replace `drawScene`/`drawPresent`/`drawHud` + the three-pass `love.draw` with `produceUI`/`produceHud`/`produceLayer`/`composeFrame` + a single `compose` pass. `composeWorld` is unchanged. Output-identical; **visual + F9 parity is the human's gate.**

**Files:** Modify `GachaClient/main.lua`.

- [ ] **Step 1: Read `GachaClient/main.lua`** around the draw section (the `composeWorld`, `drawScene`, `drawPresent`, `drawHud`, `love.draw` functions, currently ~lines 505–619) and `love.load`.

- [ ] **Step 2: Replace `drawScene` + `drawPresent` + `drawHud` with the producers + compositor.** Delete the three functions `local function drawScene(ctx) ... end`, `local function drawPresent(ctx) ... end`, and `local function drawHud(ctx) ... end` (keep `composeWorld` above them unchanged), and in their place put:
```lua
-- UI layer producer: the warp-launch screen-shake wrapping UI.draw, plus the screen transition.
-- Draws into the current target (the AA canvas during an aa group, or the screen).
local function produceUI(ctx)
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

-- HUD layer producer: the FPS readout + warp flash, drawn sharp.
local function produceHud(ctx)
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

-- Produce one layer by its source into the current target. world/ui/hud have producers;
-- background/none are no-ops for now (screen backgrounds still render inside UI.draw until 2c-2).
local function produceLayer(layer, ctx)
    local s = layer.source
    if s == "world" then
        composeWorld(ctx)
    elseif s == "ui" then
        produceUI(ctx)
    elseif s == "hud" then
        produceHud(ctx)
    end
end

-- The frame compositor: pick the active profile, plan its layer groups (maximal aa-runs), and
-- execute each group. aa groups render their layers into a pooled FrameAA canvas and resolve once;
-- sharp groups (or all groups when aa=="none") draw straight to the screen. Per-layer Profiler
-- scopes keep the F9 breakdown (scene/ui/hud) plus a present scope around the AA resolve.
local function composeFrame(ctx)
    local FrameAA  = require "ui.components.FrameAA"
    local Settings = require "services.Settings"
    local plan = Profile.composePlan(Profile.frameProfile(UI.hasScreens()))
    for _, group in ipairs(plan) do
        if group.aa and FrameAA.active() then
            ctx.frame = FrameAA.beginInto(ctx.targets:acquire(ctx.viewportW, ctx.viewportH, nil, Settings.canvasMSAA()))
            for _, layer in ipairs(group.layers) do
                local done = Profiler:scope(layer.name); produceLayer(layer, ctx); done()
            end
            local donePresent = Profiler:scope("present"); FrameAA.present(ctx.frame); donePresent()
            ctx.frame = nil
        else
            ctx.frame = nil
            for _, layer in ipairs(group.layers) do
                local done = Profiler:scope(layer.name); produceLayer(layer, ctx); done()
            end
        end
    end
end
```

- [ ] **Step 3: Rewire `love.draw` to a single `compose` pass.** Replace the current `love.draw`:
```lua
function love.draw()
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler, targets = RenderTargets.new() })
        _renderer:add(Pass.new{ name = "scene",   draw = drawScene,   budgetMs = 8.0 })
        _renderer:add(Pass.new{ name = "present", draw = drawPresent, budgetMs = 1.0 })
        _renderer:add(Pass.new{ name = "hud",     draw = drawHud,     budgetMs = 0.5 })
    end
    local vw, vh = love.graphics.getDimensions()
    _renderer:frame({ dt = love.timer.getDelta(), viewportW = vw, viewportH = vh })
    Profiler:captureStats()
    Profiler:endFrame()
    Profiler:drawOverlay(8, 30)
end
```
with:
```lua
function love.draw()
    if not _renderer then
        _renderer = Renderer.new({ profiler = Profiler, targets = RenderTargets.new() })
        _renderer:add(Pass.new{ name = "compose", draw = composeFrame, budgetMs = 10.0 })
    end
    local vw, vh = love.graphics.getDimensions()
    _renderer:frame({ dt = love.timer.getDelta(), viewportW = vw, viewportH = vh })
    Profiler:captureStats()
    Profiler:endFrame()
    Profiler:drawOverlay(8, 30)
end
```

- [ ] **Step 4: Register the per-layer budgets in `love.load`.** The old `scene`/`present`/`hud` budgets were auto-registered from the now-removed passes' `budgetMs`. Find the budget block in `love.load` (it currently sets `Profiler:setBudget("update.network", 1.0)` and `Profiler:setBudget("update.world", 3.0)`) and add the compositor's per-layer budgets right after them:
```lua
    Profiler:setBudget("update.network", 1.0)
    Profiler:setBudget("update.world", 3.0)
    Profiler:setBudget("scene", 8.0)
    Profiler:setBudget("ui", 1.0)
    Profiler:setBudget("present", 1.0)
    Profiler:setBudget("hud", 0.5)
```
(If those `update.*` budget lines aren't adjacent or are worded differently, just add the four `scene`/`ui`/`present`/`hud` `setBudget` lines anywhere in the `love.load` budget setup, after `Profiler:setEnabled(...)`.)

- [ ] **Step 5: Confirm no dangling references.** Grep `GachaClient/main.lua` for `drawScene`, `drawPresent`, `drawHud` → expect 0 matches (all replaced). Grep for `composeFrame`, `produceLayer`, `produceUI`, `produceHud` → each defined once and used. `composeWorld` still present and called only by `produceLayer`.

- [ ] **Step 6: Verify (syntax + harness).** Attempt a syntax check (`luajit -bl GachaClient/main.lua nul` or `luac -p GachaClient/main.lua`); if neither tool exists, report that plainly (do NOT claim a pass). Run the render harness (confirms shared modules unbroken): `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` → `ALL RENDER HARNESS CHECKS PASSED`. Do NOT launch the game.

- [ ] **Step 7: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): route love.draw through the layered compositor (composePlan)"
```

- [ ] **Step 8: Verify in-game (visual + F9 parity) — human gate.** Run `GachaClient/run.bat`. Confirm **identical** rendering: the overworld (incl. post grade + render-scale), a menu/screen (its background + widgets), the screen transition, the FPS readout, and the warp flash. Toggle Anti-aliasing **None / FXAA / MSAA**; toggle render-scale and post-processing; **resize**. Each must match the prior build. Press **F9**: scopes now read `compose` (frame total) with `scene` (overworld only) / `ui` / `present` / `hud` nested — `scene` appears on the overworld, not on menus; no `present` row when aa=="none". `vram` stable frame-over-frame. No crash. (If you can't launch, say so — do not claim parity you didn't observe.)

---

## Self-review (completed)

- **Spec coverage (2c-1c scope):** the layer compositor that *executes* `Profile.composePlan` → `composeFrame` (Task 2); the produce-by-source dispatch → `produceLayer` + `produceUI`/`produceHud`/`composeWorld` (Task 2); frame-profile selection (overworld→WORLD, screens→PLAIN_UI) → `Profile.frameProfile` (Task 1, headless-tested). Deferred per spec: `selectActive` + per-screen `renderProfile`, screen scene producers + `frosted` (2c-2), linear/HDR+ACES (2c-3).
- **Placeholders:** none — Task 1 is exact + headless; Task 2 gives the full new function bodies (verbatim-moved UI/HUD blocks), the exact `love.draw` replacement, and the `love.load` budget addition, with a grep gate for dangling refs. The GL executor is human-verified (no false headless claim).
- **Parity by construction:** `composeFrame` reproduces the post-2c-1b frame exactly. Overworld `WORLD` → `[{aa,{scene,ui}},{sharp,{hud}}]`: `beginInto` → `composeWorld` (world→sceneRT→grade→upscale into AA) → `produceUI` into AA → `present` → `produceHud` sharp. Screen `PLAIN_UI` → `[{aa,{ui}},{sharp,{hud}}]`: `beginInto` → `produceUI` (UI.draw renders the screen's bg+widgets) into AA → `present` → `produceHud` sharp. `aa=="none"` → `FrameAA.active()` false → all groups take the else branch (`ctx.frame=nil`, draw to screen, no present), matching today. `composeWorld` is unchanged and only runs via the `world` layer (overworld). The screen-shake (`produceUI`) and FPS/flash (`produceHud`) blocks are moved verbatim.
- **Type/name consistency:** `Profile.frameProfile(hasScreens)`, `Profile.composePlan(profile)` (returns `{ {aa, layers={...}}, ... }`), `layer.source`/`layer.name`, `ctx.targets:acquire(w,h,nil,msaa)`, `ctx.frame`, `Profiler:scope(name) -> done()` (the Phase-1 contract: always returns a callable closure), `FrameAA.active()`/`beginInto`/`present`, `composeWorld(ctx)` (unchanged) all match their existing definitions. The Renderer's single `compose` pass keeps `ctx.targets` injection + `frameReset` + profiler; per-layer scopes nest under it.

## Next: Plan 2c-2, then 2c-3

- **2c-2:** migrate screen scene producers onto `sceneRT` (each screen declares a `renderProfile`; `Profile.selectActive` + a screen-stack→profiles accessor go live; `frameProfile`/the `hasScreens→PLAIN_UI` shortcut is replaced by real per-screen profiles); `frosted` becomes a scene-layer `effects` member sampling `sceneRT` (kills the reveal 2–3×); retire `FrostedGlass`/`PostProcessEffect` re-render.
- **2c-3:** the linear/HDR `rgba16f` + ACES tonemap flip + `PostFx` linear re-tune (per-layer `tonemap` flag flips on the scene layer; UI/hud stay sRGB).
