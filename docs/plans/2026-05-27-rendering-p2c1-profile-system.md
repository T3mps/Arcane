# Rendering Phase 2c-1 — Render-Profile System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the headless-testable render-profile foundation — a composable capability table (`systems/render/Profile.lua`) with named defaults, dependency normalization, and per-frame active-profile selection, plus a `Renderer:applyProfile` that maps a profile onto pass enablement — the logic the scene-unification (2c-1b onward) drives the graph with.

**Architecture:** A render profile is plain data: `{ scene = "world"|"background"|"none", effects = {set of names}, post = bool, tonemap = bool, overlay = bool }`. The canonical pass order is fixed in the `Renderer`; a profile only toggles which passes are `enabled` for a frame. `Profile.normalize` enforces dependency sanity (scene-dependent capabilities require a scene; unknown effect names dropped). `Profile.selectActive` picks the frame's profile from the screen stack (topmost non-`overlay`, else the world). `Renderer:applyProfile` flips `scene`/`effects`/`post`/`tonemap` enablement from a normalized profile. All pure logic — no GL — so it's fully unit-tested in the existing headless harness.

**Tech Stack:** LÖVE 11.x / LuaJIT. Headless unit tests via `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` → expect `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

**Spec:** `docs/superpowers/specs/2026-05-27-rendering-2c-design.md`.

**Scope of THIS plan (2c-1):** the profile *logic units* only — `Profile.lua` (`resolve`/`normalize`/`selectActive`/named defaults) and `Renderer:applyProfile`. **Deferred to Plan 2c-1b** (needs a dedicated `RenderSystem.lua` study): the `sceneRT` scaffolding, the global `scene`/`effects`/`post`/`tonemap` passes, wiring per-frame selection into `love.draw`, the portal-distortion dead-code removal, and the overworld migration. This plan ships a tested foundation with no behavior change (nothing calls the new code in-game yet — exactly like 2a's Pool/Pass/Renderer shipped before the migration consumed them).

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `GachaClient/systems/render/Profile.lua` | Composable capability table: named defaults, `resolve`, `normalize`, `selectActive`, `KNOWN_EFFECTS` | Create |
| `GachaClient/systems/render/Renderer.lua` | `applyProfile(profile)` — map capabilities → pass enablement | Modify (add one method) |
| `GachaClient/tests/render_harness/main.lua` | Unit sections for resolve / normalize / selectActive / applyProfile | Modify |

Run (from repo root `D:/dev/starworks/Gacha`): `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`.

---

### Task 1: `Profile.lua` — named defaults + `resolve` (composition)

**Files:** Create `GachaClient/systems/render/Profile.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line of `GachaClient/tests/render_harness/main.lua`:
```lua
print("== Profile resolve ==")
do
    local Profile = require "systems.render.Profile"
    local p = Profile.resolve(Profile.MENU, { effects = {} })   -- MENU without frosting
    eq(p.scene, "background", "inherits scene from MENU base")
    eq(#p.effects, 0, "override clears effects set")
    eq(p.post, true, "inherits post from base")
    eq(#Profile.MENU.effects, 1, "named default not mutated by override")
    local q = Profile.resolve(Profile.PLAIN_UI, { scene = "world", post = true })
    eq(q.scene, "world", "override scene"); eq(q.post, true, "override post")
    eq(q.overlay, false, "overlay defaults false")
end
```

- [ ] **Step 2: Run → FAIL** (`module 'systems.render.Profile' not found`).
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`

- [ ] **Step 3: Create `GachaClient/systems/render/Profile.lua`:**
```lua
-- systems/render/Profile.lua
-- A render profile is a composable capability table deciding which passes run for a screen or
-- state: { scene = "world"|"background"|"none", effects = {set of names}, post = bool, tonemap = bool }.
-- The canonical pass order is fixed in the Renderer; a profile only toggles enablement. Named
-- defaults are spread-and-override conveniences, not a closed set. effects is an open SET of
-- named screen-space effects (e.g. {"frosted"}) so new effects compose in without a schema
-- change. normalize() enforces dependency sanity. overlay=true marks a modal that contributes
-- no scene (selectActive skips it). Plain data: FFI-neutral, headless-testable.
local Profile = {}

-- Recognized screen-space effects; extended as effects are (re)introduced (pixelate, ...).
Profile.KNOWN_EFFECTS = { frosted = true }

-- Named defaults (plain tables; compose via Profile.resolve(base, overrides)).
Profile.WORLD    = { scene = "world",      effects = {},            post = true,  tonemap = true,  overlay = false }
Profile.MENU     = { scene = "background", effects = { "frosted" }, post = true,  tonemap = true,  overlay = false }
Profile.PLAIN_UI = { scene = "none",       effects = {},            post = false, tonemap = false, overlay = false }
Profile.OVERLAY  = { scene = "none",       effects = {},            post = false, tonemap = false, overlay = true  }

-- Copy a profile, deep-copying the effects array so overrides never mutate a named default.
local function copy(p)
    local effects = {}
    if p.effects then for i = 1, #p.effects do effects[i] = p.effects[i] end end
    return { scene = p.scene, effects = effects, post = p.post, tonemap = p.tonemap, overlay = p.overlay or false }
end

-- Resolve a profile from a base (named default or table) plus overrides; returns a NEW table.
function Profile.resolve(base, overrides)
    local p = copy(base or Profile.PLAIN_UI)
    if overrides then
        for k, v in pairs(overrides) do
            if k == "effects" and type(v) == "table" then
                local e = {}; for i = 1, #v do e[i] = v[i] end; p.effects = e
            else
                p[k] = v
            end
        end
    end
    return p
end

return Profile
```

- [ ] **Step 4: Run → PASS** (`ALL RENDER HARNESS CHECKS PASSED`, exit 0).

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): render Profile capability table + resolve"
```

---

### Task 2: `Profile.normalize` — dependency rules

**Files:** Modify `GachaClient/systems/render/Profile.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Profile normalize ==")
do
    local Profile = require "systems.render.Profile"
    -- scene=none clears every scene-dependent capability
    local a = Profile.normalize({ scene = "none", effects = { "frosted" }, post = true, tonemap = true })
    eq(#a.effects, 0, "no scene -> no effects"); eq(a.post, false, "no scene -> no post"); eq(a.tonemap, false, "no scene -> no tonemap")
    -- unknown effect names dropped, known kept, order preserved
    local b = Profile.normalize({ scene = "background", effects = { "frosted", "bogus" }, post = true })
    eq(#b.effects, 1, "unknown effect dropped"); eq(b.effects[1], "frosted", "known effect kept")
    -- a valid profile passes through unchanged in substance
    local c = Profile.normalize(Profile.WORLD)
    eq(c.scene, "world", "world scene kept"); eq(c.post, true, "world post kept")
    -- normalize returns a copy (does not mutate input)
    local src = { scene = "none", effects = { "frosted" }, post = true }
    Profile.normalize(src)
    eq(#src.effects, 1, "input not mutated"); eq(src.post, true, "input post not mutated")
end
```

- [ ] **Step 2: Run → FAIL** (`Profile.normalize` is nil → `attempt to call ... a nil value`).

- [ ] **Step 3: Implement** — add to `GachaClient/systems/render/Profile.lua`, immediately before the final `return Profile`:
```lua
-- Enforce dependency sanity; returns a normalized COPY (never mutates the input). Scene-dependent
-- capabilities (effects/post/tonemap) require scene ~= "none"; effect names must be recognized.
function Profile.normalize(p)
    local n = Profile.resolve(p)
    if n.scene == nil or n.scene == "none" then
        n.effects = {}; n.post = false; n.tonemap = false
        return n
    end
    local filtered = {}
    for i = 1, #n.effects do
        local name = n.effects[i]
        if Profile.KNOWN_EFFECTS[name] then filtered[#filtered + 1] = name end
    end
    n.effects = filtered
    return n
end
```

- [ ] **Step 4: Run → PASS.**

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Profile.normalize dependency rules"
```

---

### Task 3: `Profile.selectActive` — per-frame profile selection

**Files:** Modify `GachaClient/systems/render/Profile.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Profile selectActive ==")
do
    local Profile = require "systems.render.Profile"
    -- no screens -> the world profile
    local w = Profile.selectActive({}, Profile.WORLD)
    eq(w.scene, "world", "empty stack uses world profile")
    -- nil stack also -> world
    local w2 = Profile.selectActive(nil, Profile.WORLD)
    eq(w2.scene, "world", "nil stack uses world profile")
    -- topmost non-overlay wins (a modal over a menu keeps the menu's scene)
    local sel = Profile.selectActive({ Profile.OVERLAY, Profile.MENU }, Profile.WORLD)
    eq(sel.scene, "background", "top-non-overlay = the menu")
    eq(#sel.effects, 1, "menu's effects preserved through selection")
    -- screens present but all overlays -> PLAIN_UI (no scene owner)
    local none = Profile.selectActive({ Profile.OVERLAY, Profile.OVERLAY }, Profile.WORLD)
    eq(none.scene, "none", "all-overlay stack -> no scene")
    -- result is normalized (selecting a sceneless-but-post profile clears post)
    local bad = Profile.selectActive({ { scene = "none", effects = {}, post = true } }, Profile.WORLD)
    eq(bad.post, false, "selected profile is normalized")
end
```

- [ ] **Step 2: Run → FAIL** (`Profile.selectActive` is nil).

- [ ] **Step 3: Implement** — add to `GachaClient/systems/render/Profile.lua`, immediately before the final `return Profile`:
```lua
-- Pick the active profile for a frame. stackTopDown = the screen profiles, TOP FIRST; worldProfile
-- is used when there are no screens. Returns the topmost non-overlay profile (normalized); if there
-- are screens but every one is an overlay, returns PLAIN_UI (no scene owner). Always normalized.
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
```

- [ ] **Step 4: Run → PASS.**

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Profile.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Profile.selectActive per-frame stack selection"
```

---

### Task 4: `Renderer:applyProfile` — map a profile onto pass enablement

**Files:** Modify `GachaClient/systems/render/Renderer.lua`; Modify `GachaClient/tests/render_harness/main.lua`.

- [ ] **Step 1: Add the failing test** — insert before the final `if fails == 0` line:
```lua
print("== Renderer applyProfile ==")
do
    local Renderer = require "systems.render.Renderer"
    local Pass     = require "systems.render.Pass"
    local Profile  = require "systems.render.Profile"
    local r = Renderer.new({})
    for _, name in ipairs({ "scene", "effects", "post", "tonemap", "present" }) do
        r:add(Pass.new{ name = name, draw = function() end })
    end
    r:applyProfile(Profile.normalize(Profile.WORLD))
    eq(r:get("scene").enabled, true, "WORLD enables scene")
    eq(r:get("effects").enabled, false, "WORLD has no effects")
    eq(r:get("post").enabled, true, "WORLD enables post")
    eq(r:get("tonemap").enabled, true, "WORLD enables tonemap")
    eq(r:get("present").enabled, true, "present untouched (always on)")
    r:applyProfile(Profile.normalize(Profile.MENU))
    eq(r:get("effects").enabled, true, "MENU enables effects (frosted in the set)")
    r:applyProfile(Profile.normalize(Profile.PLAIN_UI))
    eq(r:get("scene").enabled, false, "PLAIN_UI disables scene")
    eq(r:get("post").enabled, false, "PLAIN_UI disables post")
    eq(r:get("effects").enabled, false, "PLAIN_UI disables effects")
    -- applyProfile tolerates passes that aren't registered (no crash if the graph lacks one)
    local r2 = Renderer.new({})
    r2:add(Pass.new{ name = "scene", draw = function() end })
    local ok = pcall(function() r2:applyProfile(Profile.normalize(Profile.WORLD)) end)
    eq(ok, true, "applyProfile tolerates missing passes")
    eq(r2:get("scene").enabled, true, "present passes still set when others absent")
end
```

- [ ] **Step 2: Run → FAIL** (`applyProfile` is nil → `attempt to call method 'applyProfile' (a nil value)`).

- [ ] **Step 3: Implement** — add to `GachaClient/systems/render/Renderer.lua`, after the `Renderer:setEnabled` method:
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

- [ ] **Step 4: Run → PASS.**

- [ ] **Step 5: Commit**
```bash
git -C D:/dev/starworks/Gacha add GachaClient/systems/render/Renderer.lua GachaClient/tests/render_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(render): Renderer:applyProfile maps profile -> pass enablement"
```

---

## Self-review (completed)

- **Spec coverage (2c-1 scope):** composable capability table + named defaults + `resolve` → T1; `normalize` dependency rules (scene-gated caps, `effects` as a set with unknown-name filtering via `KNOWN_EFFECTS`) → T2; per-frame `selectActive` (top-non-`overlay`, empty→world, all-overlay→PLAIN_UI, normalized result) → T3; `Renderer:applyProfile` (capabilities → `scene`/`effects`/`post`/`tonemap` enablement, present/ui/hud untouched, tolerant of missing passes) → T4. The `sceneRT` scaffolding, global passes, `love.draw` wiring, portal removal, and the overworld migration are explicitly **deferred to Plan 2c-1b** and are not in scope here.
- **Placeholders:** none — every step has complete code and exact run/expected output. GL is not involved, so all four tasks are fully headless-verifiable (no "human verifies in-game" gaps in this plan).
- **Type/name consistency:** `Profile.resolve(base, overrides)`, `Profile.normalize(p)`, `Profile.selectActive(stackTopDown, worldProfile)`, `Profile.KNOWN_EFFECTS`, and the named defaults `WORLD`/`MENU`/`PLAIN_UI`/`OVERLAY` are used identically across tasks. Profile fields (`scene`, `effects` (array), `post`, `tonemap`, `overlay`) match the spec's amended table. `Renderer:applyProfile` uses the existing `Renderer:setEnabled(name, v)` (guards on `byName`) and `Renderer:get(name)` from Phase 2a. `effects` is consistently an array/set throughout (`#profile.effects`).

## Next: Plan 2c-1b

`docs/superpowers/plans/2026-05-27-rendering-p2c1b-*.md` (after a dedicated `RenderSystem.lua` study): the Stage-1 `sceneRT` infra + global `scene`/`effects`/`post`/`tonemap`(passthrough) pass scaffolding, wiring `Profile.selectActive` + `applyProfile` into `love.draw` (reading each screen's declared `renderProfile`), the invisible portal-distortion dead-code removal, and the Stage-2 overworld migration (`RenderSystem:draw` → `drawScene(ctx)` into `sceneRT`; global `post`/`present` replace its internal `PostFx`/`RenderScale`) — visual + F9 parity gated.
