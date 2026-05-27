# Rendering Phase 2c-1′ — Layer-Model Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Plan 2c-1's single-capability-table profile with the layer-compositor foundation — a `Layer` capability descriptor, a layered `Profile` (ordered layers + named defaults), and a pure `Profile.composePlan` (AA-run grouping) — all headless-testable, no in-game behavior change.

**Architecture:** A frame is an ordered list of uniform `Layer`s, each a capability table (`name`/`source`/`scale`/`grade`/`tonemap`/`aa`/`effects`). `Layer.resolve`/`Layer.normalize` are the per-layer composition + dependency rules (generalizing 2c-1's table logic). A `Profile` is `{ layers = {...}, overlay }`; named defaults (`WORLD`/`MENU`/`PLAIN_UI`/`OVERLAY`) are layer lists. `Profile.selectActive` picks the frame's profile from the screen stack; `Profile.composePlan` groups maximal runs of equal `aa` into the ordered composite plan the GL compositor (Plan 2c-1b) will execute. This **supersedes** 2c-1's `Profile` single table and removes `Renderer:applyProfile`.

**Tech Stack:** LÖVE 11.x / LuaJIT. Fully headless via `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` → `ALL RENDER HARNESS CHECKS PASSED`, exit 0. No GL, no in-game gate.

**Spec:** `docs/superpowers/specs/2026-05-27-rendering-2c-design.md` (see "Reconciliation with the executed Plan 2c-1").

**Supersession note:** Plan 2c-1 (commits `2e56386`..`1ff88a4`) created `systems/render/Profile.lua` (single table) + `Renderer:applyProfile` + four harness sections (`== Profile resolve ==`, `== Profile normalize ==`, `== Profile selectActive ==`, `== Renderer applyProfile ==`). `Profile.lua` has **no in-game consumer**, so this rework is low-risk. Task order keeps the harness green at every commit.

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `GachaClient/systems/render/Layer.lua` | Layer capability descriptor: `KNOWN_EFFECTS`, `resolve`, `normalize` | Create |
| `GachaClient/systems/render/Profile.lua` | Layered profile: named-default layer lists, `resolve`, `normalize`, `selectActive`, `composePlan` | Rewrite (was single table) |
| `GachaClient/systems/render/Renderer.lua` | Remove the obsolete `applyProfile` (superseded by `composePlan`); rest unchanged | Modify (deletion) |
| `GachaClient/tests/render_harness/main.lua` | Swap 2c-1's four sections for Layer/Profile/composePlan sections | Modify |

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`.

---

### Task 1: `Layer.lua` — descriptor + `resolve`

**Files:** Create `GachaClient/systems/render/Layer.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line of `GachaClient/tests/render_harness/main.lua`:
```lua
print("== Layer resolve ==")
do
    local Layer = require "systems.render.Layer"
    local base = { name = "scene", source = "world", scale = true, grade = true, tonemap = true, aa = true, effects = { "frosted" } }
    local l = Layer.resolve(base, { aa = false, effects = {} })
    eq(l.source, "world", "inherits source"); eq(l.grade, true, "inherits grade")
    eq(l.aa, false, "override aa"); eq(#l.effects, 0, "override effects")
    eq(#base.effects, 1, "base effects not mutated"); eq(base.aa, true, "base aa not mutated")
    local m = Layer.resolve(base)             -- no overrides == deep copy
    eq(m.effects[1], "frosted", "copy preserves effects")
    m.effects[1] = "x"
    eq(base.effects[1], "frosted", "copy effects array is independent")
end
```

- [ ] **Step 2: Run → FAIL** (`module 'systems.render.Layer' not found`).
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 3: Create `GachaClient/systems/render/Layer.lua`:**
```lua
-- systems/render/Layer.lua
-- A rendering-layer capability descriptor: plain data deciding how one layer of the frame is
-- produced and composited. Fields: { name, source = "world"|"background"|"ui"|"hud"|"none",
-- scale = bool (render at internal render-scale res, then upscale), grade = bool (PostFx grade),
-- tonemap = bool (linear->display ACES; flip phase), aa = bool (joins the AA resolve group),
-- effects = { ordered set of screen-space effect names, e.g. "frosted" } }. The scene/UI/HDR
-- distinctions are just flag values on uniform layers. FFI-neutral, headless-testable.
local Layer = {}

-- Recognized screen-space effects; extended as effects are (re)introduced (pixelate, ...).
Layer.KNOWN_EFFECTS = { frosted = true }

local FIELDS = { "name", "source", "scale", "grade", "tonemap", "aa" }

-- Copy a layer, deep-copying the effects array so overrides never mutate a base/named default.
local function copy(l)
    local effects = {}
    if l.effects then for i = 1, #l.effects do effects[i] = l.effects[i] end end
    local out = { effects = effects }
    for _, k in ipairs(FIELDS) do out[k] = l[k] end
    return out
end

-- Resolve a layer from a base plus overrides; returns a NEW table (deep-copied effects).
function Layer.resolve(base, overrides)
    local l = copy(base or {})
    if overrides then
        for k, v in pairs(overrides) do
            if k == "effects" and type(v) == "table" then
                local e = {}; for i = 1, #v do e[i] = v[i] end; l.effects = e
            else
                l[k] = v
            end
        end
    end
    return l
end

return Layer
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0).

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Layer.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Layer capability descriptor + resolve"
```

---

### Task 2: `Layer.normalize` — per-layer dependency rules

**Files:** Modify `GachaClient/systems/render/Layer.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Layer normalize ==")
do
    local Layer = require "systems.render.Layer"
    -- sourceless layer clears scene-dependent caps (aa is a compositing flag, left as-is)
    local a = Layer.normalize({ name = "x", source = "none", scale = true, grade = true, tonemap = true, aa = true, effects = { "frosted" } })
    eq(a.scale, false, "no source -> no scale"); eq(a.grade, false, "no source -> no grade")
    eq(a.tonemap, false, "no source -> no tonemap"); eq(#a.effects, 0, "no source -> no effects")
    eq(a.aa, true, "aa left as-is")
    -- nil source is treated as "none"
    local b = Layer.normalize({ name = "y", grade = true })
    eq(b.grade, false, "nil source -> no grade")
    -- valid source: unknown effect names dropped, known kept (order preserved)
    local c = Layer.normalize({ name = "s", source = "background", grade = true, effects = { "frosted", "bogus" } })
    eq(#c.effects, 1, "unknown effect dropped"); eq(c.effects[1], "frosted", "known kept"); eq(c.grade, true, "grade kept with a source")
    -- no input mutation
    local src = { name = "s", source = "none", effects = { "frosted" }, grade = true }
    Layer.normalize(src)
    eq(#src.effects, 1, "input not mutated"); eq(src.grade, true, "input grade not mutated")
end
```

- [ ] **Step 2: Run → FAIL** (`Layer.normalize` is nil → `attempt to call ... a nil value`).

- [ ] **Step 3: Implement** — add to `GachaClient/systems/render/Layer.lua`, immediately BEFORE the final `return Layer`:
```lua
-- Enforce dependency sanity; returns a normalized COPY (never mutates input). A sourceless layer
-- (source nil or "none") cannot scale/grade/tonemap/have effects; with a source, effect names must
-- be recognized (others dropped, order preserved). The aa flag is compositing-only, left untouched.
function Layer.normalize(l)
    local n = Layer.resolve(l)
    if n.source == nil or n.source == "none" then
        n.scale = false; n.grade = false; n.tonemap = false; n.effects = {}
        return n
    end
    local filtered = {}
    for i = 1, #n.effects do
        local name = n.effects[i]
        if Layer.KNOWN_EFFECTS[name] then filtered[#filtered + 1] = name end
    end
    n.effects = filtered
    return n
end
```

- [ ] **Step 4: Run → PASS.**

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Layer.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Layer.normalize per-layer dependency rules"
```

---

### Task 3: Remove the obsolete `Renderer:applyProfile`

`applyProfile` (toggling four named passes from a single-table profile) is superseded by `Profile.composePlan` (Task 5) and the GL compositor (Plan 2c-1b). It has no consumer. Removing it now — before the `Profile` rewrite — keeps the harness green (the remaining old `Profile` sections don't use it).

**Files:** Modify `GachaClient/systems/render/Renderer.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Delete the `applyProfile` method** from `GachaClient/systems/render/Renderer.lua`. Remove these exact lines (the comment block + method, currently between `Renderer:setEnabled` and the local `isEnabled`):
```lua
-- Map a normalized profile's capabilities onto pass enablement. Passes not registered are skipped
-- (setEnabled guards on byName). present/ui/hud are NOT profile-gated — they stay at their
-- registered enablement (always on). Call once per frame after selecting the active profile.
function Renderer:applyProfile(profile)
    self:setEnabled("scene",   profile.scene ~= nil and profile.scene ~= "none")
    self:setEnabled("effects", profile.effects ~= nil and #profile.effects > 0)
    self:setEnabled("post",    profile.post == true)
    self:setEnabled("tonemap", profile.tonemap == true)
end

```
So `Renderer:setEnabled` is immediately followed by `local function isEnabled(pass, ctx)`. Do NOT change anything else in `Renderer.lua` (`new`/`add`/`get`/`setEnabled`/`isEnabled`/`frame` stay).

- [ ] **Step 2: Delete the `== Renderer applyProfile ==` harness section** from `GachaClient/tests/render_harness/main.lua` — remove the entire block starting at `print("== Renderer applyProfile ==")` through its closing `end` (the `do ... end` that builds a Renderer with scene/effects/post/tonemap/present passes and asserts enablement under WORLD/MENU/PLAIN_UI + the `r2` missing-pass case). Remove nothing else.

- [ ] **Step 3: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0). The remaining sections — including the still-present `== Profile resolve/normalize/selectActive ==` (old single-table Profile, untouched here) — pass.
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 4: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Renderer.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(render): drop Renderer:applyProfile (superseded by layer composePlan)"
```

---

### Task 4: Rewrite `Profile.lua` → layered profile (+ swap its harness sections)

Atomic: the layered `Profile` and its tests change together (the old single-table tests would break against the new API). After this, `Profile` is `{ layers, overlay }` built on `Layer`.

**Files:** Rewrite `GachaClient/systems/render/Profile.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Replace the three old `Profile` harness sections.** In `GachaClient/tests/render_harness/main.lua`, delete the three consecutive blocks `print("== Profile resolve ==") do ... end`, `print("== Profile normalize ==") do ... end`, and `print("== Profile selectActive ==") do ... end` (the 2c-1 single-table tests), and replace them — in the same location — with:
```lua
print("== Profile defaults + resolve/normalize ==")
do
    local Profile = require "systems.render.Profile"
    eq(#Profile.WORLD.layers, 3, "WORLD has 3 layers")
    eq(Profile.WORLD.layers[1].source, "world", "WORLD layer1 = world scene")
    eq(Profile.WORLD.layers[2].source, "ui", "WORLD layer2 = ui")
    eq(Profile.WORLD.layers[3].aa, false, "WORLD hud not aa'd")
    eq(Profile.MENU.layers[1].effects[1], "frosted", "MENU scene has frosted")
    eq(#Profile.PLAIN_UI.layers, 2, "PLAIN_UI = ui+hud (no scene)")
    eq(Profile.OVERLAY.overlay, true, "OVERLAY is overlay")
    -- resolve deep-copies layers: mutating the result must not touch the named default
    local p = Profile.resolve(Profile.WORLD)
    p.layers[1].grade = false
    eq(Profile.WORLD.layers[1].grade, true, "resolve copy independent of WORLD default")
    -- normalize normalizes each layer (a sourceless layer's caps cleared)
    local n = Profile.normalize({ layers = { { name = "s", source = "none", grade = true, effects = { "frosted" } } }, overlay = false })
    eq(n.layers[1].grade, false, "normalize cleared sourceless layer grade")
    eq(#n.layers[1].effects, 0, "normalize cleared sourceless effects")
end

print("== Profile selectActive ==")
do
    local Profile = require "systems.render.Profile"
    local w = Profile.selectActive({}, Profile.WORLD)
    eq(w.layers[1].source, "world", "empty stack -> world")
    local w2 = Profile.selectActive(nil, Profile.WORLD)
    eq(w2.layers[1].source, "world", "nil stack -> world")
    -- topmost non-overlay wins (a modal over a menu keeps the menu's scene)
    local sel = Profile.selectActive({ Profile.OVERLAY, Profile.MENU }, Profile.WORLD)
    eq(sel.layers[1].source, "background", "top-non-overlay = menu scene")
    eq(sel.layers[1].effects[1], "frosted", "menu frosted preserved")
    -- screens but all overlays -> PLAIN_UI (no scene layer; first layer is ui)
    local none = Profile.selectActive({ Profile.OVERLAY, Profile.OVERLAY }, Profile.WORLD)
    eq(none.layers[1].source, "ui", "all-overlay -> PLAIN_UI")
end
```

- [ ] **Step 2: Run → FAIL** — the new sections reference the layered API (`Profile.WORLD.layers`) which the current single-table `Profile.lua` doesn't have, so `== Profile defaults + resolve/normalize ==` fails (e.g. `#Profile.WORLD.layers` indexes nil).
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 3: Rewrite `GachaClient/systems/render/Profile.lua`** (read it first, then replace its entire contents with):
```lua
-- systems/render/Profile.lua
-- A render profile is an ordered list of rendering Layers (+ an overlay flag for modals):
-- { layers = { <Layer>, ... }, overlay = bool }. The compositor (Renderer, Plan 2c-1b) produces
-- and composites the active profile's layers per their flags. Named defaults are spread-and-
-- override layer lists, not a closed set. Plain data: FFI-neutral, headless-testable.
local Layer = require "systems.render.Layer"

local Profile = {}

-- Named-default layer building blocks. The scene/UI/HDR distinctions are just flag values.
local SCENE_WORLD = { name = "scene", source = "world",      scale = true,  grade = true,  tonemap = true,  aa = true,  effects = {} }
local SCENE_MENU  = { name = "scene", source = "background", scale = false, grade = true,  tonemap = true,  aa = true,  effects = { "frosted" } }
local UI_LAYER    = { name = "ui",    source = "ui",         scale = false, grade = false, tonemap = false, aa = true,  effects = {} }
local HUD_LAYER   = { name = "hud",   source = "hud",        scale = false, grade = false, tonemap = false, aa = false, effects = {} }

-- Named default profiles (ordered layer lists; compose via Profile.resolve / per-layer overrides).
Profile.WORLD    = { layers = { SCENE_WORLD, UI_LAYER, HUD_LAYER }, overlay = false }
Profile.MENU     = { layers = { SCENE_MENU,  UI_LAYER, HUD_LAYER }, overlay = false }
Profile.PLAIN_UI = { layers = { UI_LAYER, HUD_LAYER },              overlay = false }
Profile.OVERLAY  = { layers = { UI_LAYER },                         overlay = true  }

-- Deep-copy a profile (each layer via Layer.resolve); returns a NEW profile.
local function copy(p)
    local layers = {}
    if p.layers then for i = 1, #p.layers do layers[i] = Layer.resolve(p.layers[i]) end end
    return { layers = layers, overlay = p.overlay or false }
end

-- Resolve a profile from a base plus overrides (overrides may set `layers` (replaces) and/or `overlay`).
function Profile.resolve(base, overrides)
    local p = copy(base or Profile.PLAIN_UI)
    if overrides then
        if overrides.layers then
            local layers = {}
            for i = 1, #overrides.layers do layers[i] = Layer.resolve(overrides.layers[i]) end
            p.layers = layers
        end
        if overrides.overlay ~= nil then p.overlay = overrides.overlay and true or false end
    end
    return p
end

-- Normalize every layer in the profile; returns a NEW profile.
function Profile.normalize(p)
    local n = copy(p)
    for i = 1, #n.layers do n.layers[i] = Layer.normalize(n.layers[i]) end
    return n
end

-- Pick the active profile for a frame. stackTopDown = the screen profiles, TOP FIRST; worldProfile
-- is used when there are no screens. Returns the topmost non-overlay profile (normalized); nil/empty
-- stack -> world; screens-but-all-overlay -> PLAIN_UI. Always normalized.
function Profile.selectActive(stackTopDown, worldProfile)
    if not stackTopDown or #stackTopDown == 0 then
        return Profile.normalize(worldProfile or Profile.WORLD)
    end
    for i = 1, #stackTopDown do
        local p = stackTopDown[i]
        if p and not p.overlay then return Profile.normalize(p) end
    end
    return Profile.normalize(Profile.PLAIN_UI)
end

return Profile
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0).

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): rework Profile into ordered layers over Layer descriptor"
```

---

### Task 5: `Profile.composePlan` — AA-run grouping (the compositor's pure core)

**Files:** Modify `GachaClient/systems/render/Profile.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Profile composePlan ==")
do
    local Profile = require "systems.render.Profile"
    -- default WORLD: scene(aa)+ui(aa) form one AA group, hud(sharp) a second
    local plan = Profile.composePlan(Profile.normalize(Profile.WORLD))
    eq(#plan, 2, "WORLD -> 2 groups")
    eq(plan[1].aa, true, "group1 is the AA group"); eq(#plan[1].layers, 2, "AA group has 2 layers")
    eq(plan[1].layers[1].name, "scene", "AA group layer1 = scene"); eq(plan[1].layers[2].name, "ui", "AA group layer2 = ui")
    eq(plan[2].aa, false, "group2 sharp"); eq(plan[2].layers[1].name, "hud", "sharp group = hud")
    -- non-contiguous aa -> a group per maximal run (degenerate but defined)
    local prof = { layers = {
        { name = "a", source = "world", aa = true,  effects = {} },
        { name = "b", source = "ui",    aa = false, effects = {} },
        { name = "c", source = "ui",    aa = true,  effects = {} },
    }, overlay = false }
    local p2 = Profile.composePlan(prof)
    eq(#p2, 3, "non-contiguous aa -> 3 groups")
    eq(p2[1].aa, true, "g1 aa"); eq(p2[2].aa, false, "g2 sharp"); eq(p2[3].aa, true, "g3 aa")
    eq(p2[2].layers[1].name, "b", "g2 = the sharp layer")
end
```

- [ ] **Step 2: Run → FAIL** (`Profile.composePlan` is nil → `attempt to call ... a nil value`).

- [ ] **Step 3: Implement** — add to `GachaClient/systems/render/Profile.lua`, immediately BEFORE the final `return Profile`:
```lua
-- Pure compositor-ordering logic: walk the profile's layers in order, grouping MAXIMAL RUNS of
-- equal `aa` into groups. Returns an ordered list { { aa = bool, layers = { <Layer>, ... } }, ... }.
-- The Renderer's GL compositor (Plan 2c-1b) executes this: aa=true groups -> one AA-canvas resolve
-- each; aa=false groups -> drawn sharp. Default WORLD/MENU -> { {aa=true,{scene,ui}}, {aa=false,{hud}} }.
function Profile.composePlan(profile)
    local groups = {}
    local cur = nil
    local layers = profile.layers or {}
    for i = 1, #layers do
        local l = layers[i]
        local aa = l.aa == true
        if not cur or cur.aa ~= aa then
            cur = { aa = aa, layers = {} }
            groups[#groups + 1] = cur
        end
        cur.layers[#cur.layers + 1] = l
    end
    return groups
end
```

- [ ] **Step 4: Run → PASS.**

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Profile.composePlan groups layers into AA runs"
```

---

## Self-review (completed)

- **Spec coverage (2c-1′ scope):** `Layer` descriptor + `resolve` (composition, no base mutation) → T1; `Layer.normalize` (sourceless clears `scale`/`grade`/`tonemap`/`effects`; unknown-effect filtering) → T2; remove obsolete `Renderer:applyProfile` → T3; layered `Profile` (named-default layer lists, `resolve`, `normalize`, `selectActive`) → T4; `Profile.composePlan` (maximal-run AA grouping) → T5. Supersession of 2c-1's single-table `Profile` + its harness sections is handled explicitly in T3 (applyProfile) and T4 (the three Profile sections). The GL compositor execution, `sceneRT`, and any in-game wiring are out of scope (Plan 2c-1b).
- **Placeholders:** none — every step has complete code and exact run/expected output; the file rewrite (T4) gives the full new file; removals (T3) quote the exact lines/block to delete. No GL, so every task is headless-verifiable.
- **Harness-green-at-every-commit:** T1/T2 add new sections (old Profile/applyProfile sections untouched → green). T3 removes `applyProfile` + its section (old Profile sections don't use it → green). T4 swaps the three Profile sections atomically with the `Profile.lua` rewrite (→ green). T5 adds composePlan (→ green). Verified the order leaves no section referencing a removed/changed symbol mid-stream.
- **Type/name consistency:** `Layer.resolve(base, overrides)`, `Layer.normalize(l)`, `Layer.KNOWN_EFFECTS`, layer fields (`name`/`source`/`scale`/`grade`/`tonemap`/`aa`/`effects`); `Profile.resolve(base, overrides)`, `Profile.normalize(p)`, `Profile.selectActive(stackTopDown, worldProfile)`, `Profile.composePlan(profile)`, and the named defaults (`WORLD`/`MENU`/`PLAIN_UI`/`OVERLAY` as `{ layers, overlay }`) are used identically across tasks and match the spec's Layer descriptor + layered-profile model.

## Next: Plan 2c-1b

`docs/superpowers/plans/2026-05-27-rendering-p2c1b-*.md`: the GL side — `sceneRT` infra, the `Renderer` compositor that *executes* `Profile.composePlan` (acquire AA canvas per aa-group, produce each layer by `source`, apply per-layer `effects`/`grade`/`tonemap`/`scale`, resolve, draw sharp groups on top), wiring `selectActive` into `love.draw` (each screen declares a `renderProfile`), the invisible portal dead-code removal, and the Stage-2 overworld migration (`RenderSystem:draw` → `drawScene(ctx)` into `sceneRT`) — visual + F9 parity gated.
