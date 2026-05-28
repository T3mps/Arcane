# Rendering Phase 3 — Pull-Reveal Particles → Geometry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the gacha pull reveal hit FPS target by moving `pull_tunnel.glsl`'s 5 point-blob loops (350 particles, drawn as a full-screen `O(pixels × particles)` loop) into an additive batched point-sprite layer (`O(particles)`), while keeping the look.

**Architecture:** A pure, headless-testable particle-math module evaluates each frame's 350 blobs (position/size/color/amplitude) from stable per-particle seeds + time, in the shader's aspect-corrected UV space. A reusable additive `BlobBatch` (SpriteBatch over a procedural soft-blob texture) draws them. `PullTunnel` draws the slimmed full-screen shader, then the batches, into the scene layer (`ctx.sceneRT`) before widgets. The shader keeps only its angular/structural loops (tunnel rings, speed-lines, rays, spirals, central light, fog).

**Tech Stack:** LÖVE 11.x / LuaJIT / GLSL. SpriteBatch + additive blend. Headless tests via `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness` (pure logic) and `.../assets_harness` (syntax + pure helpers). `luajit` is NOT on PATH — use the bundled `lovec.exe`. Spec: `docs/superpowers/specs/2026-05-27-rendering-p3-pull-tunnel-particles-design.md` (committed `f7fcfdf`).

---

## Context the implementer needs

- **The reveal animates every frame** → this is a *spatial/structural* optimization, NOT temporal caching. The win is turning `O(pixels × 350 particles)` into `O(350 particles)` by drawing particles as geometry.
- **Coordinate math (critical for fidelity):** the shader computes `uv = screen_coords/screen_size` (∈[0,1]²), then `uvCorrected.x = (uv.x-0.5)*aspect + 0.5` (aspect = W/H), and all 5 particle loops position blobs in this corrected space around `center=(0.5,0.5)`, with a circular gaussian/point falloff of radius `size` (corrected units). Because `W/aspect = H`, a blob at corrected `(cx,cy)` radius `size` maps to **pixel** `px=(cx-0.5)*H + W*0.5`, `py=cy*H`, with a **circular** pixel radius `size*H`. So sprites reproduce the exact footprint. The pure module outputs corrected-space `(cx,cy,size)`; `PullTunnel` does the `*H` pixel conversion at fill time (resolution-independent module).
- **Look-equivalence, not bit-equivalence:** per the spec (B2 decision), particles get *stable per-particle seeds* (deterministic Lua RNG, not GLSL-hash mirroring) — an equivalent field with the same distribution/motion. The before/after visual gate is the judge.
- **HDR (2c-3):** the reveal scene is `rgba16f` and tonemapped. Draw blobs **additively into `ctx.sceneRT` before the grade** (same pipeline position as the old in-shader particle contribution), so bright cores exceed 1.0 and get ACES rolloff + bloom.
- **Two falloff profiles** in the source: gaussian `exp(-d²/(2·size²))` (flyingParticles, particleDust) and sharp `smoothstep(size, size·0.1, d)` (spaceDust L1/L2); L3 is a sharp **core** + a softer **glow**. → two procedural textures (gaussian + sharp); L3 emits two blobs (sharp core + gaussian glow). Each blob carries a `kind` (1=gaussian, 2=sharp).
- **Per-sprite shading to match the shader:** the old particles were inside `finalColor` before `finalColor *= intensity` and `*= vignette(uv)`. Reproduce by multiplying each blob's amplitude by `self.intensity` and by `vignette(cx,cy) = 1 - smoothstep(0.3,0.9,len(cc))*0.6` (cc = corrected pos − 0.5). The shader's internal `applyBloom` is dropped for particles — the 2c-3 post-grade bloom blooms the HDR additive sprites instead (minor, acceptable, visual-gated).
- **Working tree is heavily dirty** (~40 unrelated files). Use **targeted `git add`** of only each task's named files. Never `git add -A`/`.`.
- **Combat abilities gameplay is off-limits** (n/a here).

## File structure

| File | Responsibility | Task |
|---|---|---|
| `GachaClient/ui/components/pull_tunnel_particles.lua` | Pure: stable per-particle seeds + per-frame eval of the 5 loops → flat blob buffer `{cx,cy,size,r,g,b,amp,kind}` in corrected-UV space. No LÖVE deps | 1 |
| `GachaClient/tests/render_harness/main.lua` | Headless tests for the particle module | 1 |
| `GachaClient/systems/render/BlobBatch.lua` | Reusable additive soft-blob renderer: a SpriteBatch over one texture; `begin()/add(...)/flush()/draw()`. Plus a pure `BlobBatch.profile(kind, nd)` falloff fn for the texture gen | 2 |
| `GachaClient/tests/assets_harness/main.lua` | Syntax-check the new files + test `BlobBatch.profile` | 2 |
| `GachaClient/ui/components/PullTunnel.lua` | Own the particle module + 2 `BlobBatch`es (+ textures); `draw()` = slim shader pass then batch passes; fill from the module with `*H` + intensity + vignette | 3 |
| `GachaClient/data/shader/fx/pull_tunnel.glsl` | Remove the 5 point-blob loops + their 3 `effect()` call sites; keep tunnel/speed-lines/rays/spirals/central light/fog/vignette/bloom | 3 |

> Note (plan refinement vs spec): the pure particle module is placed at `ui/components/pull_tunnel_particles.lua` (co-located with its sole consumer, `PullTunnel`) rather than `lib/`. It is pure (no LÖVE at require-time), so it is still required and tested headlessly via `render_harness`. `lib/` is reserved for *general* primitives; this module encodes pull-tunnel-specific particle formulas. `BlobBatch` (the general, reusable primitive) lives in `systems/render/`.

---

## Task 1: Pure particle-math module + headless tests

**Files:**
- Create: `GachaClient/ui/components/pull_tunnel_particles.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (add a test block before the final `if fails == 0` line)

- [ ] **Step 1: Write the failing test**

Insert into `GachaClient/tests/render_harness/main.lua` immediately BEFORE the final `if fails == 0 then ...` line. Uses the existing `eq`/`approx`/`fail` helpers.

```lua
print("== PullTunnelParticles ==")
do
    local PTP = require "ui.components.pull_tunnel_particles"
    local p = PTP.new()                 -- builds stable seeds deterministically
    local out = {}
    local rc, rg = { 1, 0.5, 0.2 }, { 1, 0.8, 0.5 }
    local n = p:evaluate(1.0, 0.3, 5, rc, rg, out)

    -- 5 loops: flying 30 (gaussian) + dust 80 (gaussian) + space 120+80 (sharp)
    -- + space L3 40 emitted as TWO blobs each (sharp core + gaussian glow) = 80.
    -- Total emitted = 30 + 80 + 120 + 80 + 80 = 390.
    eq(n, 390, "emitted blob count")
    eq(#out, 390 * 8, "flat buffer stride-8 length")

    -- Stride layout: cx,cy,size,r,g,b,amp,kind. Validate field ranges on blob 1.
    local cx, cy, size, r, g, b, amp, kind = out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8]
    eq(type(cx), "number", "cx numeric")
    if not (size > 0) then fail("size positive") end
    if not (amp >= 0) then fail("amp non-negative") end
    if not (kind == 1 or kind == 2) then fail("kind is 1 (gaussian) or 2 (sharp)") end

    -- Determinism: same inputs -> identical buffer.
    local out2 = {}
    local n2 = p:evaluate(1.0, 0.3, 5, rc, rg, out2)
    eq(n2, n, "deterministic count")
    local same = true
    for i = 1, n * 8 do if out[i] ~= out2[i] then same = false; break end end
    eq(same, true, "deterministic values")

    -- Two instances build the SAME stable field (seed is fixed, not global-RNG dependent).
    local p2 = PTP.new()
    local outB = {}
    p2:evaluate(1.0, 0.3, 5, rc, rg, outB)
    local sameField = true
    for i = 1, n * 8 do if out[i] ~= outB[i] then sameField = false; break end end
    eq(sameField, true, "stable field across instances")

    -- Animation: a different time generally moves things (buffer changes).
    local outT = {}
    p:evaluate(2.7, 0.3, 5, rc, rg, outT)
    local moved = false
    for i = 1, n * 8 do if math.abs(out[i] - outT[i]) > 1e-6 then moved = true; break end end
    eq(moved, true, "particles animate with time")

    -- All blobs sit in a sane corrected-UV neighborhood (loose bound; particles fly outward).
    local inBand = true
    for k = 0, n - 1 do
        local bx, by = out[k*8 + 1], out[k*8 + 2]
        if bx < -2 or bx > 3 or by < -2 or by > 3 then inBand = false; break end
    end
    eq(inBand, true, "blob positions within a sane band")
end
```

- [ ] **Step 2: Run the harness to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: failure — `module 'ui.components.pull_tunnel_particles' not found`. Must NOT print `ALL RENDER HARNESS CHECKS PASSED`.

- [ ] **Step 3: Create the module**

Create `GachaClient/ui/components/pull_tunnel_particles.lua`:

```lua
-- ui/components/pull_tunnel_particles.lua
-- Pure (no LÖVE) per-frame particle field for the pull reveal. Replaces the 5 point-blob
-- loops that used to run per-pixel inside pull_tunnel.glsl (flyingParticles, particleDust,
-- spaceDust x3). Each particle gets a STABLE per-particle seed at construction; evaluate()
-- computes the current-frame blob list from `time`, in the shader's aspect-corrected UV space
-- (center 0.5,0.5). PullTunnel converts (cx,cy,size) -> pixels via *H (W/aspect == H, so a
-- corrected-space circle is a pixel circle of radius size*H). Output is a flat stride-8 buffer
-- {cx,cy,size, r,g,b, amp, kind} (kind: 1=gaussian, 2=sharp) reused across frames (low GC).
local PTP = {}
PTP.__index = PTP

local TAU = math.pi * 2
local floor = math.floor
local sin, cos = math.sin, math.cos
local function fract(x) return x - floor(x) end
local function clamp(x, a, b) return x < a and a or (x > b and b or x) end
-- Hermite smoothstep matching GLSL smoothstep(e0,e1,x).
local function smoothstep(e0, e1, x)
    local t = clamp((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)
end

-- Deterministic LCG so the field is identical every run AND independent of the global RNG.
local function makeRng(seed)
    local s = seed % 2147483648
    return function()
        s = (s * 1103515245 + 12345) % 2147483648
        return s / 2147483648
    end
end

-- Layer descriptors. Each particle stores {angle, speed, phaseOff, extra}. The per-frame math
-- mirrors the corresponding shader loop. amp is the non-spatial brightness; the spatial falloff
-- is the sprite texture (gaussian/sharp), so amp excludes the exp()/smoothstep(dist) term.
-- depth->radius uses 1/zDepth perspective exactly as the shader.

local GAUSS, SHARP = 1, 2

function PTP.new()
    local self = setmetatable({}, PTP)
    local rng = makeRng(1337)
    self.flying = {}     -- 30, gaussian
    self.dust   = {}     -- 80, gaussian
    self.space1 = {}     -- 120, sharp
    self.space2 = {}     -- 80, sharp
    self.space3 = {}     -- 40, sharp core + gaussian glow (2 blobs)

    for i = 1, 30 do
        self.flying[i] = { a = rng() * TAU, depthPhase0 = rng(), depthRate = 0.3 + rng() * 0.3,
                           orbit = 0.02 + rng() * 0.08 }
    end
    for i = 1, 80 do
        self.dust[i] = { a = rng() * TAU, speed = 0.3 + rng() * 0.4, phaseOff = rng(), wobbleSeed = rng() * 100 }
    end
    for i = 1, 120 do
        self.space1[i] = { a = rng() * TAU, speed = 0.5 + rng() * 0.6, phaseOff = rng() }
    end
    for i = 1, 80 do
        self.space2[i] = { a = rng() * TAU, speed = 0.35 + rng() * 0.4, phaseOff = rng() }
    end
    for i = 1, 40 do
        self.space3[i] = { a = rng() * TAU, speed = 0.25 + rng() * 0.3, phaseOff = rng() }
    end
    return self
end

-- Edge-darken matching the shader's vignette(uv): cc = (cx-0.5, cy-0.5) in corrected space.
local function vignette(cx, cy)
    local dx, dy = cx - 0.5, cy - 0.5
    local d = math.sqrt(dx * dx + dy * dy)
    return 1.0 - smoothstep(0.3, 0.9, d) * 0.6
end

-- Push one blob into the flat stride-8 buffer.
local function emit(out, idx, cx, cy, size, r, g, b, amp, kind)
    local base = idx * 8
    out[base + 1] = cx; out[base + 2] = cy; out[base + 3] = size
    out[base + 4] = r;  out[base + 5] = g;  out[base + 6] = b
    out[base + 7] = amp; out[base + 8] = kind
    return idx + 1
end

-- intensity is applied by PullTunnel (self.intensity), NOT here, so the module stays render-agnostic.
-- @return number of blobs emitted (buffer is `out`, stride 8).
function PTP:evaluate(t, rotation, rarity, rc, rg, out)
    local idx = 0
    local rcr, rcg, rcb = rc[1], rc[2], rc[3]
    local rgr, rgg, rgb = rg[1], rg[2], rg[3]

    -- Layer: flying particles (30) -- gaussian. Shader: flyingParticles(uv, center, particleTime, rotation)
    for i = 1, 30 do
        local p = self.flying[i]
        local pAngle = p.a + rotation * 0.15
        local pDepth = fract(p.depthPhase0 + t * p.depthRate)
        local targetDepth = 4.0 + pDepth * 12.0
        local screenR = 1.0 / targetDepth + p.orbit * pDepth
        local cx = 0.5 + cos(pAngle) * screenR
        local cy = 0.5 + sin(pAngle) * screenR
        local size = 0.002 + pDepth * pDepth * 0.008
        local amp = pDepth * pDepth * smoothstep(0, 0.15, pDepth) * smoothstep(1.0, 0.75, pDepth) * 0.35
        amp = amp * vignette(cx, cy)
        local mr = rcr * 0.8 + (rgr - rcr * 0.8) * pDepth
        local mg = rcg * 0.8 + (rgg - rcg * 0.8) * pDepth
        local mb = rcb * 0.8 + (rgb - rcb * 0.8) * pDepth
        idx = emit(out, idx, cx, cy, size, mr, mg, mb, amp, GAUSS)
    end

    -- Layer: particle dust (80) -- gaussian. Shader: particleDust(uv, center, particleTime)
    for i = 1, 80 do
        local p = self.dust[i]
        local phase = fract(t * p.speed + p.phaseOff)
        local zDepth = 8.0 - phase * 7.0
        local perspective = 1.0 / zDepth
        local screenR = perspective * 0.35
        local offsetAngle = p.a + sin(t * 2.0 + p.wobbleSeed) * 0.1
        local cx = 0.5 + cos(offsetAngle) * screenR
        local cy = 0.5 + sin(offsetAngle) * screenR
        local size = 0.003 * perspective
        local amp = perspective * 0.5 * smoothstep(0, 0.15, phase) * smoothstep(1.0, 0.8, phase) * 0.5
        amp = amp * vignette(cx, cy)
        -- color mix(white, rg, 0.4)
        local mr = 1.0 + (rgr - 1.0) * 0.4
        local mg = 1.0 + (rgg - 1.0) * 0.4
        local mb = 1.0 + (rgb - 1.0) * 0.4
        idx = emit(out, idx, cx, cy, size, mr, mg, mb, amp, GAUSS)
    end

    -- Layer: space dust L1 (120) -- sharp. Shader spaceDust loop 1.
    for i = 1, 120 do
        local p = self.space1[i]
        local phase = fract(t * p.speed + p.phaseOff)
        local zDepth = 10.0 - phase * 9.5
        local perspective = 1.0 / zDepth
        local screenR = perspective * 0.5
        local cx = 0.5 + cos(p.a) * screenR
        local cy = 0.5 + sin(p.a) * screenR
        local size = 0.0015 + perspective * 0.002
        local amp = (0.2 + perspective * 0.4) * smoothstep(0, 0.1, phase) * 0.3
        amp = amp * vignette(cx, cy)
        idx = emit(out, idx, cx, cy, size, 1.0, 1.0, 1.0, amp, SHARP)
    end

    -- Layer: space dust L2 (80) -- sharp.
    for i = 1, 80 do
        local p = self.space2[i]
        local phase = fract(t * p.speed + p.phaseOff)
        local zDepth = 8.0 - phase * 7.5
        local perspective = 1.0 / zDepth
        local screenR = perspective * 0.45
        local cx = 0.5 + cos(p.a) * screenR
        local cy = 0.5 + sin(p.a) * screenR
        local size = 0.002 + perspective * 0.003
        local amp = (0.3 + perspective * 0.5) * smoothstep(0, 0.12, phase) * 0.25
        amp = amp * vignette(cx, cy)
        local mr = 1.0 + (rgr - 1.0) * 0.2
        local mg = 1.0 + (rgg - 1.0) * 0.2
        local mb = 1.0 + (rgb - 1.0) * 0.2
        idx = emit(out, idx, cx, cy, size, mr, mg, mb, amp, SHARP)
    end

    -- Layer: space dust L3 (40) -- sharp CORE + gaussian GLOW (two blobs each).
    for i = 1, 40 do
        local p = self.space3[i]
        local phase = fract(t * p.speed + p.phaseOff)
        local zDepth = 6.0 - phase * 5.5
        local perspective = 1.0 / zDepth
        local screenR = perspective * 0.4
        local cx = 0.5 + cos(p.a) * screenR
        local cy = 0.5 + sin(p.a) * screenR
        local size = 0.004 + perspective * 0.006
        local fade = (0.4 + perspective * 0.6) * smoothstep(0, 0.15, phase) * 0.35 * vignette(cx, cy)
        local mr = 1.0 + (rgr - 1.0) * 0.5
        local mg = 1.0 + (rgg - 1.0) * 0.5
        local mb = 1.0 + (rgb - 1.0) * 0.5
        -- core: sharp, radius ~ size*0.5; glow: gaussian, radius ~ size*2, dimmer (0.4x in shader).
        idx = emit(out, idx, cx, cy, size * 0.5, mr, mg, mb, fade, SHARP)
        idx = emit(out, idx, cx, cy, size * 2.0, mr, mg, mb, fade * 0.4, GAUSS)
    end

    return idx
end

PTP.GAUSS = GAUSS
PTP.SHARP = SHARP
return PTP
```

- [ ] **Step 4: Run the harness to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: `ALL RENDER HARNESS CHECKS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add GachaClient/ui/components/pull_tunnel_particles.lua GachaClient/tests/render_harness/main.lua
git commit -m "feat(render): pure pull-tunnel particle field module (p3 task 1)

Extracts the 5 per-pixel point-blob loops from pull_tunnel.glsl into a pure,
headless-testable per-frame evaluator (390 blobs from stable seeds + time in
corrected-UV space). No renderer yet.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: BlobBatch + procedural blob textures

**Files:**
- Create: `GachaClient/systems/render/BlobBatch.lua`
- Modify: `GachaClient/tests/assets_harness/main.lua`

- [ ] **Step 1: Write the failing test (pure profile fn)**

In `GachaClient/tests/assets_harness/main.lua`: (a) add the three new files to the `touched` list so they're syntax-checked, and (b) add a `BlobBatch.profile` unit block.

Add to the `touched` table (after the existing `"ui/components/PullTunnel.lua",` entry):
```lua
    "systems/render/BlobBatch.lua", "ui/components/pull_tunnel_particles.lua",
```

Add this block before the final `if fails == 0 then` line:
```lua
print("== BlobBatch.profile ==")
do
    local BlobBatch = assert(loadfile(CLIENT .. "systems/render/BlobBatch.lua"))()
    -- profile(kind, nd): falloff at normalized distance nd in [0,1] from texel center to edge.
    -- gaussian (kind 1): 1 at center, ~0 at edge, smooth. sharp (kind 2): bright core, fast cutoff.
    assertEq(BlobBatch.profile(1, 0.0) > 0.99, true, "gaussian center ~1")
    assertEq(BlobBatch.profile(1, 1.0) < 0.05, true, "gaussian edge ~0")
    assertEq(BlobBatch.profile(1, 0.3) > BlobBatch.profile(1, 0.7), true, "gaussian monotone decreasing")
    assertEq(BlobBatch.profile(2, 0.0) > 0.99, true, "sharp center ~1")
    assertEq(BlobBatch.profile(2, 1.0) < 0.05, true, "sharp edge ~0")
    -- sharp falls off faster than gaussian at mid distance.
    assertEq(BlobBatch.profile(2, 0.5) < BlobBatch.profile(1, 0.5), true, "sharp tighter than gaussian")
end
```

- [ ] **Step 2: Run the asset harness to verify it fails**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`
Expected: failure — the syntax-check of `systems/render/BlobBatch.lua` fails (file missing) and/or `BlobBatch.profile` block errors. Must NOT print `ALL ASSET HARNESS CHECKS PASSED`.

- [ ] **Step 3: Create BlobBatch**

Create `GachaClient/systems/render/BlobBatch.lua`. The `profile` fn is pure (testable headless); `newTexture`/`new`/`draw` touch `love.*` (used only in-game). Texture is generated lazily on first `new`, not at require, so headless syntax-check + `profile` tests don't need GL.

```lua
-- systems/render/BlobBatch.lua
-- Reusable additive soft-blob renderer: a SpriteBatch over one procedurally-generated blob
-- texture, drawn with additive blend. Fill per frame with begin()/add(...)/flush(); draw() blits.
-- Two falloff profiles (gaussian, sharp) match the pull-tunnel particle look; the generator uses
-- the pure profile(kind, nd) fn so the curve is unit-testable headless.
local BlobBatch = {}
BlobBatch.__index = BlobBatch

local GAUSS, SHARP = 1, 2
BlobBatch.GAUSS = GAUSS
BlobBatch.SHARP = SHARP

local TEX_SIZE = 64                 -- texture resolution (square)
local TEX_SIGMA = TEX_SIZE * 0.5    -- the texture's built-in radius in its own pixels (edge)

-- Falloff value at normalized distance nd in [0,1] (center->edge). Pure: no LÖVE.
function BlobBatch.profile(kind, nd)
    if nd >= 1.0 then return 0.0 end
    if kind == SHARP then
        -- Sharp point: bright tight core, quick smooth cutoff (matches spaceDust smoothstep look).
        local t = 1.0 - nd
        local s = t * t            -- tightens the core
        return s * s
    end
    -- Gaussian: exp(-(nd*k)^2). k chosen so edge (nd=1) ~ 0.018.
    local k = 2.0
    local x = nd * k
    return math.exp(-x * x * 2.0)
end

-- Generate the blob texture (CPU ImageData -> Image). Called in :new (in-game only).
function BlobBatch.newTexture(kind)
    local data = love.image.newImageData(TEX_SIZE, TEX_SIZE)
    local c = (TEX_SIZE - 1) * 0.5
    data:mapPixel(function(x, y)
        local dx, dy = (x - c) / TEX_SIGMA, (y - c) / TEX_SIGMA
        local nd = math.sqrt(dx * dx + dy * dy)
        local v = BlobBatch.profile(kind, nd)
        return 1, 1, 1, v          -- white, alpha = falloff (color comes from per-sprite tint)
    end)
    local img = love.graphics.newImage(data)
    img:setFilter("linear", "linear")
    return img
end

-- @param kind GAUSS|SHARP  @param maxSprites number
function BlobBatch.new(kind, maxSprites)
    local self = setmetatable({}, BlobBatch)
    self.texture = BlobBatch.newTexture(kind)
    self.batch = love.graphics.newSpriteBatch(self.texture, maxSprites, "stream")
    self.halfTex = TEX_SIZE * 0.5
    -- A blob of pixel radius R should map the texture's TEX_SIGMA-radius footprint to R:
    self.scaleFor = function(radiusPx) return radiusPx / TEX_SIGMA end
    return self
end

function BlobBatch:begin() self.batch:clear() end

-- Add one blob centered at pixel (px,py) with pixel radius, tint color, and additive amplitude.
function BlobBatch:add(px, py, radiusPx, r, g, b, amp)
    local s = self.scaleFor(radiusPx)
    self.batch:setColor(r * amp, g * amp, b * amp, 1.0)   -- premultiplied-style: additive uses RGB
    self.batch:add(px, py, 0, s, s, self.halfTex, self.halfTex)
end

function BlobBatch:flush() end   -- SpriteBatch auto-uploads on draw; kept for API symmetry

-- Draw additively. Caller is responsible for the current canvas/target.
function BlobBatch:draw()
    local prevMode, prevAlpha = love.graphics.getBlendMode()
    love.graphics.setBlendMode("add", "alphamultiply")
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.draw(self.batch, 0, 0)
    love.graphics.setBlendMode(prevMode, prevAlpha)
end

return BlobBatch
```

- [ ] **Step 4: Run the asset harness to verify it passes**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`
Expected: `ALL ASSET HARNESS CHECKS PASSED`, exit 0. (Confirms syntax of all three files + the `profile` curve. The texture/SpriteBatch are GL → exercised in-game in Task 3.)

- [ ] **Step 5: Commit**

```bash
git add GachaClient/systems/render/BlobBatch.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(render): reusable additive BlobBatch + procedural blob textures (p3 task 2)

SpriteBatch-over-soft-blob renderer with gaussian/sharp falloff profiles
(pure profile() fn unit-tested headless). No consumer yet.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: The look-preserving swap (VISUAL-GATED — wire PullTunnel + slim the shader in ONE commit)

These two edits MUST land together: adding the sprites without removing the shader loops would double the particles; removing the loops without the sprites would lose them.

**Files:**
- Modify: `GachaClient/ui/components/PullTunnel.lua`
- Modify: `GachaClient/data/shader/fx/pull_tunnel.glsl`

- [ ] **Step 1: Slim the shader — remove the 5 point-blob loops + call sites**

In `GachaClient/data/shader/fx/pull_tunnel.glsl`:

(a) Delete the three call sites in `effect()` (currently lines 574–581):
```glsl
    // Layer 5: Flying particles
    finalColor += flyingParticles(uvCorrected, center, particleTime, rotation);

    // Layer 6: Particle dust (specs flying out of tunnel)
    finalColor += particleDust(uvCorrected, center, particleTime);

    // Layer 7: Space dust (many small particles streaming outward)
    finalColor += spaceDust(uvCorrected, center, particleTime);
```
Replace with a single comment:
```glsl
    // Layers 5-7 (flying particles / particle dust / space dust) are now drawn as additive
    // geometry by PullTunnel.lua via BlobBatch (O(particles), not O(pixels x particles)).
```

(b) Delete the now-unused functions entirely: `flyingParticles` (the `for i<30` block), `particleDust` (`for i<80`), and `spaceDust` (the three `for` loops). Leave `tunnelEffect`, `centralLight`, `depthFog`, `spiralEnergy`, `volumetricRays`, `vignette`, `applyBloom`, `hash`, `hash1`, `rgb2hsv`, `hsv2rgb` intact. The `particleTime` local in `effect()` stays (still used by `volumetricRays`).

Verify (grep) that no remaining code references the deleted functions:
```bash
grep -nE "flyingParticles|particleDust|spaceDust" GachaClient/data/shader/fx/pull_tunnel.glsl
```
Expected: only the new comment line matches (no call/def remains).

- [ ] **Step 2: Wire PullTunnel to the particle module + BlobBatches**

In `GachaClient/ui/components/PullTunnel.lua`:

(a) Add requires near the top (after `local Assets = require "services.Assets"`):
```lua
local PTP       = require "ui.components.pull_tunnel_particles"
local BlobBatch = require "systems.render.BlobBatch"
```

(b) In `PullTunnel.new`, after the existing field table is created (before `return`), attach the particle system. The current `new` does `return setmetatable({...}, PullTunnel)`; change it to capture the instance, build the particle pieces, and return it:
```lua
    local self = setmetatable({
        time           = 0,
        rotation       = 0,
        intensity      = opts.intensity     or 1.0,
        baseIntensity  = opts.intensity     or 1.0,
        rotationSpeed  = opts.rotation_speed or 0.8,
        rarity         = opts.rarity        or 5,
        fadeInDuration = opts.fade_in_duration or 0.6,
        stopped        = false,
        _fade          = nil,
    }, PullTunnel)

    -- Particle field (Phase 3): the old in-shader point-blob loops, now additive geometry.
    self.particles = PTP.new()
    self._blobBuf  = {}                       -- reused flat stride-8 buffer (low GC)
    self._gauss    = BlobBatch.new(BlobBatch.GAUSS, 256)   -- flying(30)+dust(80)+L3 glow(40) = 150
    self._sharp    = BlobBatch.new(BlobBatch.SHARP, 384)   -- space1(120)+space2(80)+L3 core(40) = 240

    return self
```
(Remove the original `return setmetatable({ ... }, PullTunnel)` that this replaces.)

(c) Replace `PullTunnel:draw()`'s tail. Keep the existing shader-draw exactly as-is (the slimmed shader still renders tunnel/rays/etc.), then append the particle pass. After the existing final `love.graphics.setShader()` line in `draw()`, add:
```lua
    -- Particle geometry pass (Phase 3): evaluate this frame's blobs and draw them additively
    -- over the tunnel, into the current scene target. Skipped while stopped/invisible.
    if self.intensity > 0.0 then
        local n = self.particles:evaluate(self.time, self.rotation, self.rarity,
            { rc[1], rc[2], rc[3] }, { rg[1], rg[2], rg[3] }, self._blobBuf)
        local buf = self._blobBuf
        local cxBase = W * 0.5
        self._gauss:begin()
        self._sharp:begin()
        for k = 0, n - 1 do
            local b = k * 8
            local cx, cy, size = buf[b+1], buf[b+2], buf[b+3]
            local amp = buf[b+7] * self.intensity      -- shader applied finalColor *= intensity
            if amp > 0.0 then
                local px = (cx - 0.5) * H + cxBase     -- W/aspect == H: corrected->pixel
                local py = cy * H
                local radiusPx = size * H
                local kind = buf[b+8]
                local target = (kind == BlobBatch.SHARP) and self._sharp or self._gauss
                target:add(px, py, radiusPx, buf[b+4], buf[b+5], buf[b+6], amp)
            end
        end
        self._gauss:draw()
        self._sharp:draw()
    end
```
Note: `rc`/`rg`/`W`/`H` are already locals in `draw()` (rarity colors and `love.graphics.getDimensions()`). Confirm they are in scope where this block is inserted; if the shader-draw early-returns when `not shader`, place this block AFTER that guard so the fallback path (no shader) still has W,H — or simply compute `W,H` again at the top of the particle block via `love.graphics.getDimensions()` to be safe.

- [ ] **Step 3: Headless smoke — both harnesses green**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/render_harness`
Expected: `ALL RENDER HARNESS CHECKS PASSED`.
Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`
Expected: `ALL ASSET HARNESS CHECKS PASSED` (the real-fs shader scan still finds 20 shaders — slimming `pull_tunnel.glsl` removes loops, not the file).

- [ ] **Step 4: VISUAL GATE (user-confirmed — cannot verify headlessly)**

Launch `GachaClient/run.bat`, trigger a pull reveal (5★ for the full effect), and confirm with the user:
- **FPS win (primary):** F9 — the reveal should now hold the FPS target; the bg draw is no longer one ultra-heavy full-screen pass. Report before/after FPS.
- **Look equivalence:** the reveal reads as the same effect — particle density, the streaming starfield (sharp points), the soft glows (gaussian), color tint by rarity, motion toward/around center, and the tunnel/rays/spirals unchanged.
- **HDR coherence:** bright particle cores roll off (ACES) and bloom rather than clip.
- **Tuning knobs if off:** `BlobBatch.profile` curves (gaussian `k`, sharp exponent), the L3 core/glow split, per-layer `amp` multipliers, `TEX_SIZE`/`TEX_SIGMA`.

> Implementer: do NOT claim the visual items pass. Report them as "needs user confirmation" and stop for review.

- [ ] **Step 5: Commit (after user confirms the visual gate)**

```bash
git add GachaClient/ui/components/PullTunnel.lua GachaClient/data/shader/fx/pull_tunnel.glsl
git commit -m "perf(render): pull reveal particles to batched geometry (p3 task 3)

PullTunnel now draws its 5 point-blob layers (390 sprites) via two additive
BlobBatches fed by the pure particle module, and pull_tunnel.glsl drops those
loops (~397 -> ~47 per-pixel iterations). O(pixels x particles) -> O(particles).
Look preserved; HDR-additive into the rgba16f scene. Visual + F9 confirmed.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Self-review (against the spec)

**Spec coverage:**
- "Slimmed full-screen shader keeps angular/structural loops" → Task 3 Step 1 removes only the 5 point-blob fns/call sites; keeps tunnel/speed-lines/rays/spirals/light/fog/vignette/bloom. ✓
- "Additive batched point-sprite layer, O(particles)" → Task 2 `BlobBatch` + Task 3 fill/draw. ✓
- "Reusable BlobBatch (not a one-off), seeds Phase 4" → general `systems/render/BlobBatch.lua`, kind-agnostic. ✓
- "Pure particle model, stable seeds, headless-testable" → Task 1 module (deterministic LCG, no LÖVE) + render_harness tests. ✓
- "Two falloff profiles + L3 core/glow" → `profile(GAUSS/SHARP)`; L3 emits sharp core + gaussian glow. ✓
- "Corrected-UV → pixel mapping (W/aspect==H)" → Task 3 fill `px=(cx-0.5)*H+W*0.5, py=cy*H, radius=size*H`. ✓
- "Per-sprite intensity + vignette match" → vignette in the module amp; `*self.intensity` at fill. ✓
- "HDR additive before grade" → drawn into the scene target after the shader, additive. ✓
- "Validation: profiler FPS, visual gate, headless particle-math + assets" → Task 1 render_harness, Task 2 assets_harness, Task 3 visual gate. ✓
- "Scope: pull_tunnel only; CachedPass/Memo deferred" → no Memo/persistent-RT work. ✓

**Placeholder scan:** none — full code in every code step; exact commands + expected output.

**Type/name consistency:** `evaluate(t, rotation, rarity, rc, rg, out) -> n`; stride-8 `{cx,cy,size,r,g,b,amp,kind}`; `kind` 1=GAUSS/2=SHARP consistent between the module (`PTP.GAUSS/SHARP`) and `BlobBatch.GAUSS/SHARP`; `BlobBatch.new(kind,max)/begin()/add(px,py,radiusPx,r,g,b,amp)/draw()/profile(kind,nd)/newTexture(kind)` used consistently in Task 3. Emitted count 390 (350 logical + 40 extra L3 glow blobs) asserted in Task 1 and matched by the Task 3 batch capacities (gauss 150 ≤ 256, sharp 240 ≤ 384).

**Gap check:** Task 3's particle block references `rc/rg/W/H` from `PullTunnel:draw`'s existing locals — Step 2(c) explicitly flags the scope/early-return caveat and the safe fallback (recompute `W,H`). No undefined refs.
