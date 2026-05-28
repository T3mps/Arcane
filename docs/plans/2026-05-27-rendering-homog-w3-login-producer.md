# Homogenization W3 — login.lua → Producer Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `ui/screens/login.lua` from its fully-bespoke draw chain (own `bgCanvas`, manual frosted via `HudEffects.applyFrostedGlass`, per-pixel-rect gradient line, manual child iteration outside `Widget.draw`) onto the canonical scene-producer contract — same shape as InventoryScreen / PartyScreen / DailiesScreen after W2 Task 1 and WaveGame / CombatScreen after 2c-2b.

**Architecture:** Login becomes a scene producer (`renderProfile = Profile.forProducer({effects={"frosted"}})`, `gradeSettings = nil`, `drawScene(ctx)` clears bg + draws `SpaceBackground`). The compositor's `composeScreensScene` (`main.lua:525`) picks login up automatically as a sceneScreen; the scene-layer `frosted` effect runs `FrostedGlass.fromScene(ctx.sceneRT)` once per frame, populating `FrostedGlass.canvas()` (blurred) for any consumer to sample. Login's `:draw` becomes widgets + chrome only — bg is gone from `:draw`, the bespoke `bgCanvas`/`ensureCanvas` delete entirely, and the panel-region frosted blit reads `FrostedGlass.canvas()` instead of running its own blur via `HudEffects`. The per-pixel gradient line (~364 `rectangle("fill")` calls) becomes one 6-vertex triangle-strip mesh.

**Tech Stack:** LÖVE 11.x / LuaJIT, `Screen` base + the layer compositor (`main.lua`), `systems.render.Profile`, `ui.components.FrostedGlass`, `ui.components.SpaceBackground`, `love.graphics.newMesh`.

**Constraints (standing):**
- Working tree heavily dirty — **targeted `git add <file>` only, NEVER `git add -A` or `git add .`.**
- Never skip hooks (`--no-verify`) or bypass signing.
- Combat ABILITIES gameplay off-limits (not relevant to W3).
- Login is **NOT** in the F2/F3 inventory/party rework scope (per `project_inventory_party_rework.md`), so this migration is durable.

**Notes locked at plan time:**
- `Panel` widget supports `frosted = true` as a top-level option (`ui/widgets/panel.lua:32`) and uses `FrostedGlass.canvas()` (the blurred result) per `panel.lua:78`. The plan uses the **manual blit fallback** path (a one-line `love.graphics.draw(FrostedGlass.canvas(), ...)` over the panel rect inside `:draw`'s chrome block) — same canonical sceneCanvas, just custom geometry. This avoids `Panel.frosted = true`'s auto-shadow/tint/lit-edge which would change the look. If the user later wants to consolidate, they can flip to `Panel.frosted = true` and remove the manual blit.
- The dark-overlay-on-transition caveat tracked in `project_aaa_rendering.md` does NOT apply here in normal flow: login is a top-level screen with no world behind it at boot. On logout, `doLogout` (`main.lua:315`) tears down `state.world` BEFORE pushing the login screen, so `composeScreensScene`'s world-behind path won't trigger.
- The `HudEffects` module is still used for `HudEffects.getEdgeGlowSettings()` (edge-glow stripe). Only the `clearRegions`/`addRegion`/`applyFrostedGlass` calls are replaced; the require + the `getEdgeGlowSettings` call stay.

---

## File Structure

**Modified (one file, one commit):**
- `GachaClient/ui/screens/login.lua` (~965 lines today → ~870-880 lines after)

**Untouched:**
- `ui/components/SpaceBackground.lua` (login uses it via `SpaceBackground.new("default")`)
- `ui/components/FrostedGlass.lua` (login switches from `HudEffects` to `FrostedGlass.canvas()` directly)
- `ui/hud/hud_effects.lua` (login still uses `HudEffects.getEdgeGlowSettings()`)
- `main.lua` (already supports `composeScreensScene` + scene-layer `frosted` effect from W2 Task 1)

---

## Target shape — after migration

The new `LoginScreen.new` constructor sets the producer contract:

```lua
function LoginScreen.new(onLoginSuccess)
    local screen = Screen.new({
        escapeCloses = false,
        onEnter = function(self)
            -- ...existing onEnter body...
        end
    })
    setmetatable(screen, {__index = setmetatable(LoginScreen, {__index = Screen})})

    -- Scene producer: bg flows through ctx.sceneRT; frosted scene-layer effect
    -- populates FrostedGlass.canvas() for the panel's manual frosted blit.
    screen.renderProfile = Profile.forProducer({ effects = { "frosted" } })
    screen.gradeSettings = nil

    -- ...rest of init unchanged (state, savedServer, etc., spaceBackground, buildUI)...
    return screen
end
```

`drawScene` is a new method:

```lua
function LoginScreen:drawScene(ctx)
    if not self.visible then return end
    love.graphics.clear(0, 0, 0, 1)
    self.spaceBackground:draw()
end
```

`:draw` becomes widgets + chrome only (bg gone; manual blit reads `FrostedGlass.canvas()`):

```lua
function LoginScreen:draw()
    if not self.visible then return end

    self:updateLayout()

    local screenW = love.graphics.getWidth()
    local screenH = love.graphics.getHeight()
    local panelX  = self.panel.x
    local panelY  = self.panel.y
    local panelW  = self.panel.width
    local panelH  = self.panel.height

    local useTransition = self:beginTransitionDraw()

    -- Frosted panel region (canonical scene-layer frosted effect populates this)
    local frostCanvas = FrostedGlass.canvas()
    if frostCanvas then
        love.graphics.setScissor(panelX, panelY, panelW, panelH)
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.draw(frostCanvas, 0, 0)
        love.graphics.setScissor()
    end

    -- Panel border (manual, login-specific style)
    love.graphics.setColor(BORDER_COLOR)
    love.graphics.setLineWidth(1)
    love.graphics.rectangle("line", panelX, panelY, panelW, panelH)

    -- Accent stripe at top with edge glow
    local glowSettings = HudEffects.getEdgeGlowSettings()
    if glowSettings then
        local glowSize = glowSettings.size
        local glowAlpha = glowSettings.intensity * 0.5
        for i = glowSize, 1, -1 do
            local alpha = glowAlpha * (1 - i / (glowSize + 1))
            love.graphics.setColor(ACCENT_COLOR[1], ACCENT_COLOR[2], ACCENT_COLOR[3], alpha)
            love.graphics.rectangle("fill", panelX, panelY - i, panelW, ACCENT_HEIGHT + i * 2)
        end
    end
    love.graphics.setColor(ACCENT_COLOR)
    love.graphics.rectangle("fill", panelX, panelY, panelW, ACCENT_HEIGHT)

    -- Decorative gradient line under title (mesh; replaces ~364 1x1 rects)
    local lineY = panelY + self.lineY
    local lineWidth = panelW - CONTENT_PADDING * 4
    local lineX = panelX + CONTENT_PADDING * 2
    if self._gradLineMesh then
        love.graphics.push()
        love.graphics.translate(lineX, lineY)
        love.graphics.scale(lineWidth, 1)
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.draw(self._gradLineMesh)
        love.graphics.pop()
    end

    -- Panel children (recursion via Widget.draw)
    Widget.draw(self)

    -- Dropdown arrow on server dropdown (manual; depends on toggle state)
    local arrowX = panelX + self.serverDropdownArrowX
    local arrowY = panelY + self.serverDropdownArrowY
    local arrowSize = 5
    love.graphics.setColor(theme.colors.textMuted)
    if self.showServerDropdown then
        love.graphics.polygon("fill",
            arrowX, arrowY - arrowSize,
            arrowX - arrowSize, arrowY + arrowSize/2,
            arrowX + arrowSize, arrowY + arrowSize/2
        )
    else
        love.graphics.polygon("fill",
            arrowX, arrowY + arrowSize,
            arrowX - arrowSize, arrowY - arrowSize/2,
            arrowX + arrowSize, arrowY - arrowSize/2
        )
    end

    -- Status indicator at bottom right (inside transition)
    local dotColor   = self.statusDotColor or theme.colors.textMuted
    local statusFont = theme.getFont("small")
    local statusText = self.statusText or "Disconnected"
    local textWidth  = statusFont:getWidth(statusText)
    local padding    = 16
    local dotRadius  = 5
    local textX = screenW - padding - textWidth
    local dotX  = textX - dotRadius - 8
    local dotY  = screenH - padding

    love.graphics.setColor(dotColor[1], dotColor[2], dotColor[3], 0.3)
    love.graphics.circle("fill", dotX, dotY, 10)
    love.graphics.setColor(dotColor)
    love.graphics.circle("fill", dotX, dotY, dotRadius)
    love.graphics.setColor(1, 1, 1, 0.4)
    love.graphics.circle("fill", dotX - 1, dotY - 1, 2)
    love.graphics.setColor(dotColor)
    love.graphics.setFont(statusFont)
    love.graphics.print(statusText, textX, dotY - statusFont:getHeight() / 2)

    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.setLineWidth(1)

    if useTransition then
        self:endTransitionDraw()
    end
end
```

The gradient mesh is built once in `buildUI` (gets baked at the end of buildUI, in mesh-space coordinates 0..1 × 0..1, then scaled+translated per-draw):

```lua
-- At end of buildUI (after all widgets created):
local gold = theme.colors.gold
self._gradLineMesh = love.graphics.newMesh({
    -- {x, y, u, v, r, g, b, a}    (mesh space: x 0..1, y 0..1)
    {0.0, 0, 0, 0, gold[1], gold[2], gold[3], 0.0},   -- top-left,    alpha 0
    {0.0, 1, 0, 0, gold[1], gold[2], gold[3], 0.0},   -- bottom-left, alpha 0
    {0.5, 0, 0, 0, gold[1], gold[2], gold[3], 0.4},   -- top-mid,     alpha 0.4 (peak of sin(πt)*0.4)
    {0.5, 1, 0, 0, gold[1], gold[2], gold[3], 0.4},   -- bottom-mid,  alpha 0.4
    {1.0, 0, 0, 0, gold[1], gold[2], gold[3], 0.0},   -- top-right,   alpha 0
    {1.0, 1, 0, 0, gold[1], gold[2], gold[3], 0.0},   -- bottom-right,alpha 0
}, "strip", "static")
```

Triangle-strip with 6 vertices makes 4 triangles forming two 1-px-tall quads from left→mid and mid→right. Vertex alpha interpolates linearly across each segment. Two segments give a piecewise-linear approximation of `sin(πt) * 0.4`: 0 at edges, 0.4 at center. Visually equivalent to the per-pixel sine curve at this thickness.

---

## Task 1: Migrate login.lua to producer contract (atomic, one commit)

**Files:**
- Modify: `GachaClient/ui/screens/login.lua`

### Step 1: Read login.lua

Use the Read tool: `Read(file_path="D:/dev/starworks/Gacha/GachaClient/ui/screens/login.lua")`

Confirm the current state matches the plan's assumptions:
- Imports at lines 5–17 include `local SpaceBackground = require "ui.components.SpaceBackground"`, `local HudEffects = require "ui.hud.hud_effects"`, but NO `local Profile = ...` and NO `local Widget = ...`.
- Module-local `local bgCanvas = nil` at line 53.
- Constructor at lines 56–108 sets up `screen.state`, `savedServer`, etc., and calls `buildUI` + `rebuildServerSection`.
- `:ensureCanvas` method at lines 827–832.
- `:draw` method at lines 834–962 has the Steps 1–8 structure with the bespoke bg/frosted/gradient-line/manual-children blocks.

If lines have shifted (file changed since the plan was written), use grep anchors instead of line numbers:
- `local bgCanvas = nil`
- `function LoginScreen:ensureCanvas`
- `-- Step 1: Render space background to canvas for frosted glass source`
- `-- Step 6: Draw decorative line under title`
- `-- Step 7: Draw panel children`

### Step 2: Add `Profile` and `Widget` imports

Use Edit to find:

```lua
local UI = require "ui"
local Network = require "network"
local CacheManager = require "services.CacheManager"
local log = require("services.Logger").create("Login")
local SpaceBackground = require "ui.components.SpaceBackground"
local HudEffects = require "ui.hud.hud_effects"
local Screen = UI.Screen
local Button = UI.Button
local Label = UI.Label
local Panel = UI.Panel
local TextInput = UI.TextInput
local theme = UI.theme
local draw = require "ui.util.draw"
```

Replace with:

```lua
local UI = require "ui"
local Network = require "network"
local CacheManager = require "services.CacheManager"
local log = require("services.Logger").create("Login")
local SpaceBackground = require "ui.components.SpaceBackground"
local HudEffects = require "ui.hud.hud_effects"
local FrostedGlass = require "ui.components.FrostedGlass"
local Profile = require "systems.render.Profile"
local Screen = UI.Screen
local Widget = UI.Widget
local Button = UI.Button
local Label = UI.Label
local Panel = UI.Panel
local TextInput = UI.TextInput
local theme = UI.theme
local draw = require "ui.util.draw"
```

Three lines added (`FrostedGlass`, `Profile`, `Widget`).

### Step 3: Delete the module-local `bgCanvas`

Use Edit to find:

```lua
-- Canvas for background capture (for frosted glass)
local bgCanvas = nil

```

Replace with the empty string (delete both lines plus the blank line after).

After deletion, the next code is `function LoginScreen.new(onLoginSuccess)`.

### Step 4: Add `renderProfile` and `gradeSettings` in the constructor

Use Edit to find:

```lua
    setmetatable(screen, {__index = setmetatable(LoginScreen, {__index = Screen})})

    screen.onLoginSuccess = onLoginSuccess
```

Replace with:

```lua
    setmetatable(screen, {__index = setmetatable(LoginScreen, {__index = Screen})})

    -- Scene producer: bg flows through ctx.sceneRT (per-screen transition applies to bg + UI
    -- together). The "frosted" scene-layer effect populates FrostedGlass.canvas() so the
    -- panel-region blit below samples a properly-blurred sceneRT instead of running its own.
    screen.renderProfile = Profile.forProducer({ effects = { "frosted" } })
    screen.gradeSettings = nil

    screen.onLoginSuccess = onLoginSuccess
```

### Step 5: Add the gradient-line mesh at the end of `buildUI`

Find the very last lines of `buildUI` — after the `self.footerLabel` block, ending with `panel:addChild(self.footerLabel)`:

```lua
    -- Footer
    self.footerLabel = Label.new({
        x = 0,
        y = panelH - 28,
        width = panelW,
        text = "Press Tab to navigate | Enter to submit",
        font = theme.getFont("tiny"),
        alignH = "center",
        color = {0.35, 0.35, 0.4, 1}
    })
    panel:addChild(self.footerLabel)
end
```

Use Edit to replace the closing `end` so the mesh creation lives inside `buildUI`:

```lua
    -- Footer
    self.footerLabel = Label.new({
        x = 0,
        y = panelH - 28,
        width = panelW,
        text = "Press Tab to navigate | Enter to submit",
        font = theme.getFont("tiny"),
        alignH = "center",
        color = {0.35, 0.35, 0.4, 1}
    })
    panel:addChild(self.footerLabel)

    -- Gradient-line mesh: 6-vertex triangle strip approximating sin(πt) * 0.4 gold falloff.
    -- One draw call instead of ~364 per-pixel rects. Mesh space is 0..1 × 0..1; scaled to
    -- (lineWidth, 1) and translated to (lineX, lineY) at draw time.
    local gold = theme.colors.gold
    self._gradLineMesh = love.graphics.newMesh({
        {0.0, 0, 0, 0, gold[1], gold[2], gold[3], 0.0},
        {0.0, 1, 0, 0, gold[1], gold[2], gold[3], 0.0},
        {0.5, 0, 0, 0, gold[1], gold[2], gold[3], 0.4},
        {0.5, 1, 0, 0, gold[1], gold[2], gold[3], 0.4},
        {1.0, 0, 0, 0, gold[1], gold[2], gold[3], 0.0},
        {1.0, 1, 0, 0, gold[1], gold[2], gold[3], 0.0},
    }, "strip", "static")
end
```

### Step 6: Delete the `:ensureCanvas` method

Find:

```lua
function LoginScreen:ensureCanvas(width, height)
    if not bgCanvas or bgCanvas:getWidth() ~= width or bgCanvas:getHeight() ~= height then
        bgCanvas = love.graphics.newCanvas(width, height)
    end
    return bgCanvas
end

```

Replace with the empty string (delete the whole method + its trailing blank line).

### Step 7: Add the `:drawScene` method

Insert the new method immediately AFTER the now-deleted `:ensureCanvas` slot (i.e., right BEFORE `function LoginScreen:draw()`).

Use Edit to find:

```lua
function LoginScreen:draw()
    if not self.visible then return end

    self:updateLayout()
```

Replace with:

```lua
function LoginScreen:drawScene(ctx)
    if not self.visible then return end
    love.graphics.clear(0, 0, 0, 1)
    self.spaceBackground:draw()
end

function LoginScreen:draw()
    if not self.visible then return end

    self:updateLayout()
```

### Step 8: Rewrite the body of `:draw` to widgets + chrome only

Use Edit to find the entire current `:draw` body from the layout-read lines through `if useTransition then self:endTransitionDraw() end`. The current body is large; the precise lines (per the file as of the plan write) are 837–961.

Anchor the Edit on the unique pair: starts with `local screenW = love.graphics.getWidth()` right after `self:updateLayout()` (a few lines below the `function LoginScreen:draw()` signature), ends with `if useTransition then self:endTransitionDraw() end` followed by the closing `end` of the function.

Find:

```lua
    local screenW = love.graphics.getWidth()
    local screenH = love.graphics.getHeight()
    local panelX = self.panel.x
    local panelY = self.panel.y
    local panelW = self.panel.width
    local panelH = self.panel.height

    -- Step 1: Render space background to canvas for frosted glass source
    local canvas = self:ensureCanvas(screenW, screenH)
    local _prevCanvas = love.graphics.getCanvas()   -- frame target (FrameAA canvas) or screen
    love.graphics.setCanvas(canvas)
    love.graphics.clear(0, 0, 0, 1)
    self.spaceBackground:draw()
    love.graphics.setCanvas(_prevCanvas)

    -- Step 2: Draw the background to screen
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.draw(canvas, 0, 0)

    -- Begin transition for all UI elements
    local useTransition = self:beginTransitionDraw()

    -- Step 3: Apply frosted glass to panel region (no rounded corners)
    HudEffects.clearRegions()
    HudEffects.addRegion(panelX, panelY, panelW, panelH, 0)
    HudEffects.applyFrostedGlass(canvas)

    -- Step 4: Draw panel border (no rounded corners)
    love.graphics.setColor(BORDER_COLOR)
    love.graphics.setLineWidth(1)
    love.graphics.rectangle("line", panelX, panelY, panelW, panelH)

    -- Step 5: Draw accent stripe at top (full width) with edge glow
    local glowSettings = HudEffects.getEdgeGlowSettings()
    if glowSettings then
        local glowSize = glowSettings.size
        local glowAlpha = glowSettings.intensity * 0.5
        for i = glowSize, 1, -1 do
            local alpha = glowAlpha * (1 - i / (glowSize + 1))
            love.graphics.setColor(ACCENT_COLOR[1], ACCENT_COLOR[2], ACCENT_COLOR[3], alpha)
            love.graphics.rectangle("fill", panelX, panelY - i, panelW, ACCENT_HEIGHT + i * 2)
        end
    end

    -- Accent stripe at top (full width)
    love.graphics.setColor(ACCENT_COLOR)
    love.graphics.rectangle("fill", panelX, panelY, panelW, ACCENT_HEIGHT)

    -- Step 6: Draw decorative line under title
    local lineY = panelY + self.lineY
    local lineWidth = panelW - CONTENT_PADDING * 4
    local lineX = panelX + CONTENT_PADDING * 2

    for i = 0, lineWidth do
        local t = i / lineWidth
        local alpha = math.sin(t * math.pi) * 0.4
        love.graphics.setColor(theme.colors.gold[1], theme.colors.gold[2], theme.colors.gold[3], alpha)
        love.graphics.rectangle("fill", lineX + i, lineY, 1, 1)
    end

    -- Step 7: Draw panel children (labels, inputs, buttons)
    for _, child in ipairs(self.children) do
        if child.visible ~= false then
            child:draw()
        end
    end

    -- Step 7.5: Draw dropdown arrow on server dropdown
    local arrowX = panelX + self.serverDropdownArrowX
    local arrowY = panelY + self.serverDropdownArrowY
    local arrowSize = 5
    love.graphics.setColor(theme.colors.textMuted)
    if self.showServerDropdown then
        -- Arrow pointing up (dropdown open)
        love.graphics.polygon("fill",
            arrowX, arrowY - arrowSize,
            arrowX - arrowSize, arrowY + arrowSize/2,
            arrowX + arrowSize, arrowY + arrowSize/2
        )
    else
        -- Arrow pointing down (dropdown closed)
        love.graphics.polygon("fill",
            arrowX, arrowY + arrowSize,
            arrowX - arrowSize, arrowY - arrowSize/2,
            arrowX + arrowSize, arrowY - arrowSize/2
        )
    end

    -- Step 8: Draw status indicator at bottom right
    local dotColor = self.statusDotColor or theme.colors.textMuted
    local statusFont = theme.getFont("small")
    local statusText = self.statusText or "Disconnected"
    local textWidth = statusFont:getWidth(statusText)
    local padding = 16
    local dotRadius = 5
    local textX = screenW - padding - textWidth
    local dotX = textX - dotRadius - 8
    local dotY = screenH - padding

    -- Dot glow
    love.graphics.setColor(dotColor[1], dotColor[2], dotColor[3], 0.3)
    love.graphics.circle("fill", dotX, dotY, 10)

    -- Dot
    love.graphics.setColor(dotColor)
    love.graphics.circle("fill", dotX, dotY, dotRadius)

    -- Inner highlight
    love.graphics.setColor(1, 1, 1, 0.4)
    love.graphics.circle("fill", dotX - 1, dotY - 1, 2)

    -- Status text
    love.graphics.setColor(dotColor)
    love.graphics.setFont(statusFont)
    love.graphics.print(statusText, textX, dotY - statusFont:getHeight() / 2)

    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.setLineWidth(1)

    -- End transition
    if useTransition then
        self:endTransitionDraw()
    end
end
```

Replace with:

```lua
    local screenW = love.graphics.getWidth()
    local screenH = love.graphics.getHeight()
    local panelX  = self.panel.x
    local panelY  = self.panel.y
    local panelW  = self.panel.width
    local panelH  = self.panel.height

    local useTransition = self:beginTransitionDraw()

    -- Frosted panel region. The canonical scene-layer "frosted" effect populated FrostedGlass.canvas()
    -- this frame from ctx.sceneRT (set by composeScreensScene via our drawScene); we just blit the
    -- blurred result, scissored to the panel rect, with no rounded corners. Replaces the bespoke
    -- HudEffects.applyFrostedGlass(bgCanvas) path that ran its own blur on a duplicate bgCanvas.
    local frostCanvas = FrostedGlass.canvas()
    if frostCanvas then
        love.graphics.setScissor(panelX, panelY, panelW, panelH)
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.draw(frostCanvas, 0, 0)
        love.graphics.setScissor()
    end

    -- Panel border (manual, login-specific style)
    love.graphics.setColor(BORDER_COLOR)
    love.graphics.setLineWidth(1)
    love.graphics.rectangle("line", panelX, panelY, panelW, panelH)

    -- Accent stripe at top with edge glow
    local glowSettings = HudEffects.getEdgeGlowSettings()
    if glowSettings then
        local glowSize = glowSettings.size
        local glowAlpha = glowSettings.intensity * 0.5
        for i = glowSize, 1, -1 do
            local alpha = glowAlpha * (1 - i / (glowSize + 1))
            love.graphics.setColor(ACCENT_COLOR[1], ACCENT_COLOR[2], ACCENT_COLOR[3], alpha)
            love.graphics.rectangle("fill", panelX, panelY - i, panelW, ACCENT_HEIGHT + i * 2)
        end
    end
    love.graphics.setColor(ACCENT_COLOR)
    love.graphics.rectangle("fill", panelX, panelY, panelW, ACCENT_HEIGHT)

    -- Decorative gradient line under title (one mesh draw, replaces ~364 per-pixel rects)
    local lineY = panelY + self.lineY
    local lineWidth = panelW - CONTENT_PADDING * 4
    local lineX = panelX + CONTENT_PADDING * 2
    if self._gradLineMesh then
        love.graphics.push()
        love.graphics.translate(lineX, lineY)
        love.graphics.scale(lineWidth, 1)
        love.graphics.setColor(1, 1, 1, 1)
        love.graphics.draw(self._gradLineMesh)
        love.graphics.pop()
    end

    -- Panel + its children (standard Widget.draw recursion)
    Widget.draw(self)

    -- Dropdown arrow on server dropdown (manual; depends on toggle state)
    local arrowX = panelX + self.serverDropdownArrowX
    local arrowY = panelY + self.serverDropdownArrowY
    local arrowSize = 5
    love.graphics.setColor(theme.colors.textMuted)
    if self.showServerDropdown then
        love.graphics.polygon("fill",
            arrowX, arrowY - arrowSize,
            arrowX - arrowSize, arrowY + arrowSize/2,
            arrowX + arrowSize, arrowY + arrowSize/2
        )
    else
        love.graphics.polygon("fill",
            arrowX, arrowY + arrowSize,
            arrowX - arrowSize, arrowY - arrowSize/2,
            arrowX + arrowSize, arrowY - arrowSize/2
        )
    end

    -- Status indicator at bottom right
    local dotColor   = self.statusDotColor or theme.colors.textMuted
    local statusFont = theme.getFont("small")
    local statusText = self.statusText or "Disconnected"
    local textWidth  = statusFont:getWidth(statusText)
    local padding    = 16
    local dotRadius  = 5
    local textX = screenW - padding - textWidth
    local dotX  = textX - dotRadius - 8
    local dotY  = screenH - padding

    love.graphics.setColor(dotColor[1], dotColor[2], dotColor[3], 0.3)
    love.graphics.circle("fill", dotX, dotY, 10)
    love.graphics.setColor(dotColor)
    love.graphics.circle("fill", dotX, dotY, dotRadius)
    love.graphics.setColor(1, 1, 1, 0.4)
    love.graphics.circle("fill", dotX - 1, dotY - 1, 2)
    love.graphics.setColor(dotColor)
    love.graphics.setFont(statusFont)
    love.graphics.print(statusText, textX, dotY - statusFont:getHeight() / 2)

    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.setLineWidth(1)

    if useTransition then
        self:endTransitionDraw()
    end
end
```

What this rewrite does (delta from the old body):
- **Deletes** Step 1 (bgCanvas capture), Step 2 (blit), Step 3 (HudEffects frosted block), the Step 6 per-pixel loop, and the Step 7 manual children iteration.
- **Replaces** the HudEffects frosted block with a one-line `love.graphics.draw(FrostedGlass.canvas(), …)` scissored to the panel rect.
- **Replaces** the per-pixel gradient loop with a `love.graphics.draw(self._gradLineMesh, …)` inside push/translate/scale.
- **Replaces** the manual children iteration with `Widget.draw(self)`.
- **Keeps**: border, accent stripe + edge glow, dropdown arrow, status indicator, transition wrap, `local screenW/screenH/panelX/panelY/panelW/panelH` reads.

### Step 9: Confirm `bgCanvas` and `ensureCanvas` are fully gone

After Steps 3 and 6, neither should appear anywhere in the file. Verify:

```bash
grep -n "bgCanvas\|ensureCanvas\|HudEffects\.applyFrostedGlass\|HudEffects\.clearRegions\|HudEffects\.addRegion" GachaClient/ui/screens/login.lua
```

Expected: **zero matches**.

If any matches show up, fix them — leftover references will either error at runtime (`bgCanvas` is no longer a module local, so a stray reference is a nil-index) or run the duplicate frosted blur (`HudEffects.applyFrostedGlass`).

### Step 10: Confirm `HudEffects.getEdgeGlowSettings` is still referenced

```bash
grep -n "HudEffects\." GachaClient/ui/screens/login.lua
```

Expected: exactly ONE match — the `local glowSettings = HudEffects.getEdgeGlowSettings()` line. The require at the top stays (it's needed for this call).

### Step 11: Headless syntax check

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
```

Expected: `ALL ASSET HARNESS CHECKS PASSED`.

The harness's `touched` list (`GachaClient/tests/assets_harness/main.lua:9–30`) does NOT currently include `ui/screens/login.lua`. Add it at the end of the array (same pattern as W2 Task 1's harness additions). The harness edit belongs in the same commit as the login.lua change. Re-run the harness to confirm it still passes after the addition.

The final `touched` list should have `"ui/screens/login.lua",` as a new entry.

### Step 12: Visual gate — USER GATED, do NOT commit until confirmed

The implementer subagent dispatched for Task 1 STOPS at this step. Main session pauses for the user.

When the user runs the game:

1. **Launch fresh.** Login screen should appear with: space background fading + growing into view (now part of the per-screen transition since login is a scene producer); frosted-glass panel center-screen with same translucent darkening as before; pink accent stripe at top of panel; gold gradient line below the title.
2. **Click on the server dropdown** → server presets list expands. Repeat → collapses. (Dropdown arrow flips correctly.)
3. **Type username + password → Login** → screen transitions out (UI.fade, the global fade-to-black) → game world loads. Same flow as before.
4. **Logout from pause menu** (in-game ESC → Save & Logout) → fade → login screen reappears. Bg + panel + widgets fade in together (not bg-then-widgets).
5. **Resize the window while on login** → bg + panel reflow correctly, no canvas mismatch artifacts (the old bespoke `bgCanvas` had to handle this; the canonical sceneRT does it automatically).
6. **F9 profiler** while on login → there should now be a `scene` scope (login wasn't producing one before; now it does, because the scene layer is active).

If anything regresses, hold the commit. Likely failure modes:
- **Panel frosted region looks wrong**: if `FrostedGlass.canvas()` returns nil or the blur isn't running, the panel region shows whatever's behind without the blur. Confirm `composeScreensScene`'s scene-layer "frosted" effect is firing — it should, because login's `renderProfile = Profile.forProducer({ effects = { "frosted" } })`.
- **Gradient line looks different**: the 6-vertex mesh approximates sin(πt) with two linear segments. If it visibly doesn't peak at the center or has wrong endpoints, recheck the mesh vertex order/colors.
- **Widgets misaligned or missing**: `Widget.draw(self)` should produce the same set of draws as the old manual iteration. If a widget is missing, it's because `self.panel` (the parent) wasn't a direct child of `self`. Confirm `self:addChild(self.panel)` runs at line ~159 of the original file.

### Step 13: Commit (after user confirms)

```bash
git add GachaClient/ui/screens/login.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(render): login.lua -> producer contract (homog W3)"
```

Verify:
- `git log -1 --stat` shows exactly 2 files modified (login.lua + assets_harness/main.lua)
- `git status --short` should still show all the other M/?? entries from the dirty working tree (untouched)
- The commit's `--stat` for login.lua should show ~85-100 lines deleted, ~30-40 lines added (net shrinkage ~50-60 lines)

---

## Verification summary

| Check | Where | Gate |
|---|---|---|
| Syntax-check login.lua | assets_harness loadfile | Step 11 |
| `bgCanvas`/`ensureCanvas`/`HudEffects.applyFrostedGlass` fully removed | grep | Step 9 |
| `HudEffects.getEdgeGlowSettings` still called once | grep | Step 10 |
| Visual: login looks identical | user visual gate | Step 12 |
| Visual: bg fades + grows with UI (new behavior, matches F1/F4) | user visual gate | Step 12 |
| Visual: gradient line looks like sin(πt) gold falloff | user visual gate | Step 12 |
| F9 profiler shows new `scene` scope on login | user visual gate | Step 12 |
| Commit boundary clean | git log --stat | Step 13 |

## Risks (from spec, W3-specific mitigations)

| Risk | Mitigation |
|---|---|
| `FrostedGlass.canvas()` look diverges from old `HudEffects.applyFrostedGlass(bgCanvas)` | Visual gate catches it. If divergent, `HudEffects.applyFrostedGlass` may apply different scissor/tint than the manual blit. Fallback: read `ui/hud/hud_effects.lua` for what `applyFrostedGlass` does and replicate any missing tint/scissor in the new path. The plan's path matches `Panel.frosted = true`'s behavior (per `panel.lua:78-83`), which is the canonical seam. |
| Gradient mesh look diverges from per-pixel sin(πt) | The 6-vertex strip approximates sin with two linear segments; peak at center matches exactly (0.4 alpha), endpoints match exactly (0 alpha). Mid-points (t=0.25, t=0.75) differ slightly: real sin = 0.4*sin(π*0.25) = 0.283; linear approx = 0.2. ~28% relative difference at quarter points but absolute alpha difference is 0.08 — likely imperceptible at 1px tall. If the difference is visible, increase mesh vertex count (e.g., 10 vertices at t=0, 0.25, 0.5, 0.75, 1 with real sin alphas). Visual gate catches it. |
| `Widget.draw(self)` doesn't produce the same set of draws as the manual loop | The screen's direct children should be just `self.panel` (added in `buildUI` at line ~159). The panel recursively draws its own children. `Widget.draw(self)` does the same recursion. If widgets are missing, check whether anything else was `addChild`'d directly to `self` outside the panel (e.g., the loading label, error label — they ARE direct children per `buildUI`). All such widgets should still render. |
| Dark-overlay-on-transition caveat applies to login | Per the plan-time analysis: it does NOT apply in normal flow. Login is top-level (no world behind at boot); logout tears down world before login pushes. If the caveat surfaces in some unforeseen flow, falls back to the same tracked workstream. |
| Working-tree contamination | Step 13's `git add` enumerates the two files explicitly. No `-A`, no `.`. |

## Out of scope for W3

- **Replacing the panel's manual border / accent stripe / edge glow with Panel widget features.** They're login-specific styling; the manual draw is fine.
- **`Panel.frosted = true` direct adoption** (with the widget handling the blit). The plan uses the manual blit fallback to avoid Panel's auto-shadow/tint/lit-edge changing the look. A later cleanup can consolidate if desired.
- **The connection-state FSM (`STATE.DISCONNECTED/CONNECTING/etc.`)** and its interaction with `tryAutoLogin` / `doAuth`. The migration is purely visual; behavior is untouched.
- **The pre-existing dropdown-arrow handling.** It's currently rendered manually outside the dropdown widget. A future cleanup could move it inside a Dropdown widget, but that's a separate concern.
