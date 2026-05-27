# Rendering Phase 2c-2b — Combat & WaveGame Scene Producers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the two hand-coded gameplay screens — CombatScreen and WaveGame — onto the unified `composeScene` pipeline as scene producers, via the 2c-2 hand-coded opt-in contract. Removes their bespoke present/`frameTarget` handling; each keeps its signature effects producer-internal. Parity-gated; no look change.

**Architecture:** Each screen implements `drawScene(ctx)` (scene + signature internal effects → the current target, which `composeScreensScene` has set to `ctx.sceneRT`/scratch) + a `renderProfile` (scene+ui+hud, no frosted, no grade) + `gradeSettings=nil`. `:draw` shrinks to screen-space HUD/overlays (the ui layer). No compositor/`main.lua` changes — `resolveFrame`/`composeScreensScene` already drive any screen with the contract.

**Tech Stack:** LÖVE 11.x / LuaJIT. `systems/render/{Profile,Layer}.lua`, `composeScene`/`composeScreensScene` (main.lua, built in 2c-2). Headless harness: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`.

**Spec:** `docs/superpowers/specs/2026-05-27-rendering-2c2b-combat-wave-producers-design.md`.

**Decision (locked):** producer-internal effects — the compositor owns only cross-cutting passes (grade/upscale/AA/frosted); each producer renders its scene+effects into `ctx.sceneRT`, like the WORLD producer.

**Git hygiene:** working tree is dirty — targeted `git add <file>` only, never `git add -A`/`.`.

---

## File Structure

| File | Change |
|---|---|
| `GachaClient/systems/render/Profile.lua` | **Modify** — add `Profile.forProducer(opts)` (scene+ui+hud, no-frosted/no-grade default) |
| `GachaClient/tests/render_harness/main.lua` | **Modify** — test `forProducer` |
| `GachaClient/ui/screens/combat/init.lua` | **Modify** — add `CombatScreen:drawScene`, shrink `:draw`, set `renderProfile`/`gradeSettings` |
| `GachaClient/ui/screens/dailies/WaveGame.lua` | **Modify** — add `WaveGame:drawScene` (steps 1–4), shrink `:draw` (step 5), set `renderProfile`/`gradeSettings` |

**Verification legend:** _Headless_ = `lovec.exe GachaClient/tests/render_harness` → "ALL RENDER HARNESS CHECKS PASSED". _Visual+F9_ = launch the screen in-game — **flag for the user; do not claim success on visual gates** (these are GL producers, not headless-testable).

---

### Task 1: `Profile.forProducer(opts)` — profile for hand-coded scene producers

**Files:**
- Modify: `GachaClient/systems/render/Profile.lua` (add before `return Profile`)
- Test: `GachaClient/tests/render_harness/main.lua`

- [ ] **Step 1: Write the failing test** — insert before the final `if fails == 0` summary line in `tests/render_harness/main.lua`:

```lua
print("== Profile forProducer ==")
do
    local Profile = require "systems.render.Profile"
    -- Defaults: a self-contained producer — scene(background) + ui + hud, NO frosted, NO grade, NO scale.
    local p = Profile.forProducer()
    eq(#p.layers, 3, "forProducer: 3 layers")
    eq(p.layers[1].source, "background", "scene is background source")
    eq(#p.layers[1].effects, 0, "no effects by default (no frosted)")
    eq(p.layers[1].grade, false, "no scene grade by default")
    eq(p.layers[1].scale, false, "no scale by default")
    eq(p.layers[1].aa, true, "scene aa")
    eq(p.layers[2].source, "ui", "layer2 ui")
    eq(p.layers[2].grade, false, "ui not graded by default")
    eq(p.layers[3].source, "hud", "layer3 hud")
    eq(p.layers[3].aa, false, "hud sharp")
    eq(p.overlay, false, "not an overlay profile")
    -- opts honored
    local q = Profile.forProducer({ effects = { "frosted" }, grade = true, scale = true })
    eq(q.layers[1].effects[1], "frosted", "effects opt honored")
    eq(q.layers[1].grade, true, "grade opt honored")
    eq(q.layers[1].scale, true, "scale opt honored")
    eq(q.layers[2].grade, true, "ui grade tracks the grade opt")
    -- independent tables per call
    q.layers[1].effects[1] = "x"
    local r = Profile.forProducer({ effects = { "frosted" } })
    eq(r.layers[1].effects[1], "frosted", "forProducer returns independent tables")
end
```

- [ ] **Step 2: Run to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: FAIL — `attempt to call field 'forProducer' (a nil value)`.

- [ ] **Step 3: Implement** — in `GachaClient/systems/render/Profile.lua`, add before `return Profile`:

```lua
-- Build a render profile for a HAND-CODED scene-producing screen (no def table). MENU-shaped:
-- scene(source="background") + ui + hud. Defaults reproduce a self-contained producer that renders its
-- own internal effects into ctx.sceneRT with NO compositor grade/upscale/frosted. opts (all optional):
--   effects = { ... } (scene-layer effect names; default none), grade = bool (default false),
--   scale = bool (default false). Pure: FFI-neutral, headless-testable; independent tables per call.
function Profile.forProducer(opts)
    opts = opts or {}
    local effects = opts.effects or {}
    local grade = opts.grade and true or false
    local scale = opts.scale and true or false
    return {
        layers = {
            Layer.resolve({ name = "scene", source = "background", scale = scale, grade = grade, tonemap = grade, aa = true,  effects = effects }),
            Layer.resolve({ name = "ui",    source = "ui",         scale = false, grade = grade, tonemap = false, aa = true,  effects = {} }),
            Layer.resolve({ name = "hud",   source = "hud",        scale = false, grade = false, tonemap = false, aa = false, effects = {} }),
        },
        overlay = false,
    }
end
```

- [ ] **Step 4: Run to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: PASS — "ALL RENDER HARNESS CHECKS PASSED".

- [ ] **Step 5: Commit**

```bash
git add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(render): Profile.forProducer — profile for hand-coded scene producers"
```

---

### Task 2: CombatScreen → scene producer

**Files:**
- Modify: `GachaClient/ui/screens/combat/init.lua`

CombatScreen (extends `Screen`, constructor at `init.lua:31`, `:draw` at `init.lua:109`). Today `:draw` does bg fill → world → bloom → HUD → reset, all to the active canvas. Split: scene (bg + world + bloom) → `drawScene`; HUD → `:draw`.

- [ ] **Step 1: Require Profile** — at the top of `init.lua` with the other `require`s, add (if not already present):

```lua
local Profile = require "systems.render.Profile"
```

- [ ] **Step 2: Bind the contract in the constructor** — in `CombatScreen.new`, immediately after the `setmetatable(screen, ...)` line (`init.lua:38`), add:

```lua
    -- 2c-2b: CombatScreen is a scene producer. Its scene (bg + grid/units + holographic bloom) renders
    -- into ctx.sceneRT via drawScene; HUD draws in the ui layer (:draw). No grade/scale (parity).
    screen.renderProfile = Profile.forProducer()
    screen.gradeSettings = nil
```

- [ ] **Step 3: Add `CombatScreen:drawScene`** — add immediately before `function CombatScreen:draw()` (`init.lua:109`):

```lua
-- Scene producer: render the combat world (bg fill + isometric grid/units + holographic bloom) into
-- the current target (ctx.sceneRT, set by the compositor). HUD is drawn separately in :draw (ui layer).
function CombatScreen:drawScene(ctx)
    local sw = love.graphics.getWidth()
    local sh = love.graphics.getHeight()

    -- Background fill
    love.graphics.setColor(0.07, 0.07, 0.10, 1)
    love.graphics.rectangle("fill", 0, 0, sw, sh)

    -- World (isometric grid + units)
    self.camera:attach()
    self.renderer:draw()
    self.camera:detach()

    -- Holographic bloom (screen-space additive glow over the world)
    self.renderer:drawBloom()

    love.graphics.setShader()
    love.graphics.setColor(1, 1, 1, 1)
end
```

- [ ] **Step 4: Shrink `CombatScreen:draw` to the HUD (ui layer)** — replace the entire existing `function CombatScreen:draw() ... end` (`init.lua:109-134`) with:

```lua
function CombatScreen:draw()
    local sw = love.graphics.getWidth()
    local sh = love.graphics.getHeight()

    -- HUD (screen-space). The world + bloom are now produced into ctx.sceneRT by drawScene.
    self.hud:draw(sw, sh)

    -- Defensive graphics-state reset (the compositor owns the canvas; leave shader/color clean).
    love.graphics.setShader()
    love.graphics.setColor(1, 1, 1, 1)
end
```

- [ ] **Step 5: Verify (Visual+F9 — flag for user)** — launch a combat encounter (e.g. F4 test encounter):
  - Grid, units, hover hologram, float numbers, the additive holographic bloom, and HUD render identically to before.
  - Enter/exit transition (scale+fade) intact.
  - F9 shows `compose` → `scene` (the combat world) / `ui` (HUD) / `hud` scopes; stable across None/FXAA/MSAA + resize.

- [ ] **Step 6: Commit**

```bash
git add GachaClient/ui/screens/combat/init.lua
git commit -m "feat(render): CombatScreen is a scene producer (world+bloom -> sceneRT, HUD in ui layer)"
```

---

### Task 3: WaveGame → scene producer

**Files:**
- Modify: `GachaClient/ui/screens/dailies/WaveGame.lua`

WaveGame (extends `Screen`, constructor at `WaveGame.lua:283`, `:draw` at `WaveGame.lua:836`). Today `:draw` runs steps 1–4 (scene → bloom → composite → lens vignette) then step 5 (screen-space overlays: particles, flash, HUD). Split: steps 1–4 → `drawScene`; step 5 → `:draw`. The internal `_canvasScene`/`_canvasGlow`/`_canvasBlur`/`_canvasComp` chain and the lens vignette stay producer-internal.

- [ ] **Step 1: Require Profile** — at the top of `WaveGame.lua` with the other requires, add (if not present):

```lua
local Profile = require "systems.render.Profile"
```

- [ ] **Step 2: Bind the contract in the constructor** — in `WaveGame.new`, immediately after the `setmetatable(screen, ...)` line (`WaveGame.lua:286`), add:

```lua
    -- 2c-2b: WaveGame is a scene producer. Its scene + tuned bloom + near-miss lens vignette render into
    -- ctx.sceneRT via drawScene; screen-space overlays (particles/flash/HUD) draw in the ui layer (:draw).
    screen.renderProfile = Profile.forProducer()
    screen.gradeSettings = nil
```

- [ ] **Step 3: Rename the scene half of `:draw` to `drawScene`** — change the function header at `WaveGame.lua:836` from:

```lua
function WaveGame:draw()
    if not self.visible then return end
    local sw = love.graphics.getWidth()
    local sh = love.graphics.getHeight()
    _initGfx(sw, sh)
    local frameTarget = love.graphics.getCanvas()  -- present target (screen, or the FrameAA canvas)
```

to:

```lua
function WaveGame:drawScene(ctx)
    if not self.visible then return end
    local sw = love.graphics.getWidth()
    local sh = love.graphics.getHeight()
    _initGfx(sw, sh)
    local frameTarget = love.graphics.getCanvas()  -- the scene RT, set by the compositor (composeScreensScene)
```

Then **end this method** right after step 4 (the near-miss lens vignette draw). The current step-4 block ends at `WaveGame.lua:943`:

```lua
    -- ── 4. Near-miss vignette → screen ───────────────────────────────────────
    _lensShader:send("nearMiss", self.nearMissAlpha)
    love.graphics.setShader(_lensShader)
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.draw(_canvasComp, 0, 0)
    love.graphics.setShader()
```

Immediately after that `love.graphics.setShader()` (line 943), insert a new line `end` to close `drawScene`, then a blank line, then begin the new `:draw` (Step 4 below). (Leave steps 1–3 between lines 843–936 exactly as-is — only the function header changed and a closing `end` is added after step 4. The `setCanvas(frameTarget)` at line 936 is unchanged; `frameTarget` is now `ctx.sceneRT`.)

- [ ] **Step 4: New `WaveGame:draw` = step 5 (ui layer)** — the existing step-5 block (`WaveGame.lua:945-973`, "Screen-space overlays: particles, flash, HUD" through its closing `end`) becomes the body of a new `:draw`. Replace the step-5 comment header line (`-- ── 5. Screen-space overlays: particles, flash, HUD ──`) and rely on the inserted function header so the result reads:

```lua
-- Screen-space overlays drawn in the ui layer, on top of the produced scene: death particles,
-- flash, HUD, framing, and end-state overlays.
function WaveGame:draw()
    if not self.visible then return end
    local sw = love.graphics.getWidth()
    local sh = love.graphics.getHeight()

    -- Death particles: additive bright sparks at the kill point
    if #self.particles > 0 then
        love.graphics.setBlendMode("add")
        for _, p in ipairs(self.particles) do
            local life01 = p.life / p.maxLife
            local a      = life01 * life01
            love.graphics.setColor(p.r, p.g, p.b, a)
            love.graphics.circle("fill", p.x, p.y, p.size * (0.5 + life01 * 0.5))
        end
        love.graphics.setBlendMode("alpha")
    end

    if self.flashAlpha > 0 then
        love.graphics.setColor(C_DEAD[1], C_DEAD[2], C_DEAD[3], self.flashAlpha * 0.46)
        love.graphics.rectangle("fill", 0, 0, sw, sh)
    end
    if self.entryFlash > 0 then
        love.graphics.setColor(1, 1, 1, self.entryFlash * 0.38)
        love.graphics.rectangle("fill", 0, 0, sw, sh)
    end

    local hudBottom = self:_drawHUD(sw, sh)
    self:_drawFraming(sw, sh, hudBottom)
    if self.gameState == "waiting" then self:_drawOverlayWaiting(sw, sh) end
    if self.gameState == "won"     then self:_drawOverlayWin(sw, sh)     end

    love.graphics.setColor(1, 1, 1, 1)
end
```

(Net effect: the old single `WaveGame:draw` becomes two methods — `drawScene` = old lines 836–943, `draw` = old lines 945–973 wrapped in the new header above. No logic inside either half changes.)

- [ ] **Step 5: Verify (Visual+F9 — flag for user)** — launch the dailies WaveGame minigame:
  - Fiber background, tunnel/trail/wave head, the tuned 2-pass bloom (toggle Settings bloom on/off), the near-miss lens vignette, death particles, flash, and HUD all render identically; `displayScale` framing unchanged.
  - F9 shows `compose` → `scene` (the wave world+bloom+lens) / `ui` (overlays+HUD) / `hud` scopes; stable across None/FXAA/MSAA + resize.

- [ ] **Step 6: Commit**

```bash
git add GachaClient/ui/screens/dailies/WaveGame.lua
git commit -m "feat(render): WaveGame is a scene producer (scene+bloom+lens -> sceneRT, overlays in ui layer)"
```

---

## Self-Review

**Spec coverage:** §Architecture (contract: `drawScene`/`renderProfile`/`gradeSettings`) → Tasks 2/3; the `forProducer` profile shape (no frosted, no grade, no scale) → Task 1. §Combat migration → Task 2. §WaveGame migration → Task 3. "No compositor/main.lua changes" → honored (only Profile + the two screens touched). Producer-internal effects → both `drawScene`s keep their bloom/lens. Deferred (render-scale/grade) → `forProducer()` defaults `scale=false`/`grade=false`, `gradeSettings=nil`.

**Type/name consistency:** `Profile.forProducer(opts)` defined in Task 1, called identically in Tasks 2/3. `drawScene(ctx)` / `renderProfile` / `gradeSettings` match the 2c-2 contract consumed by `composeScreensScene` (sets the target canvas + reads `top.gradeSettings`). `frameTarget` in WaveGame stays the active canvas (now `ctx.sceneRT`).

**Placeholder scan:** none — Task 1 has full code; Tasks 2/3 give exact headers, the constructor insert, and explicit move boundaries with line anchors. Visual gates are flagged as human-verified.

**Risk check:** WaveGame's `_initGfx` canvases are window-sized; `sceneScaled=false` → sceneRT window-sized → 1:1 present (no change). Combat `drawBloom` restores `prevCanvas` (= sceneRT) internally → bloom composites into sceneRT. Both keep base-`Screen` transitions (compositor applies scale/alpha around `drawScene`; overlays/HUD in `:draw` behave as today).

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-27-rendering-p2c2b-combat-wave-producers.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task + two-stage review; Tasks 2/3 pause for your in-game visual+F9 confirmation.
2. **Inline Execution** — execute here with checkpoints.

Which approach?
