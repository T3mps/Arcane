# Homogenization W1 — WaveGame Inline Shaders → `Assets.shader` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move WaveGame's 5 inline GLSL strings to standalone `.glsl` files loaded through `services.Assets`, so every shader in the client goes through the same loader path and the canonical manifest can warn on collisions.

**Architecture:** WaveGame currently embeds five shader programs as Lua `[[ ... ]]` heredocs (`_BLUR_GLSL`, `_BRIGHT_GLSL`, `_INNER_GLOW_GLSL`, `_WALL_GLSL`, `_LENS_GLSL`) compiled via raw `love.graphics.newShader(STRING)` inside `_initGfx`. Same module already loads `fiber_bg` via `Assets.shader("fiber_bg")` — extending that pattern. Files land under `data/shader/wave/` for organization, but the manifest keys ALL shaders by bare filename stem regardless of subdirectory (see `services/assets/manifest.lua:6` and `tests/assets_harness/main.lua:94`), so each new file MUST have a unique stem across the whole `data/shader/` tree. The existing stem `blur` (in `data/shader/post/blur.glsl`) collides with `_BLUR_GLSL`; prefixing all five with `wave_` resolves the collision. Source GLSL bytes are unchanged → shader behavior is bit-identical.

**Tech Stack:** LÖVE 11.x / LuaJIT, GLSL, `services.Assets` (manifest + shader loader), assets_harness (`lovec` headless).

**Stem collision audit (done at plan time, locked):** Of the 20 existing shader stems in `data/shader/`, `blur` collides with `_BLUR_GLSL`. The four other proposed bare names (`bright`, `inner_glow`, `wall`, `lens`) don't collide today, but using a consistent `wave_*` prefix for all five (a) keeps them grouped in stem listings, (b) self-documents the owner module, and (c) leaves room for a future scene to introduce its own `bright`/`wall` shader without retroactively breaking WaveGame. Final stems:

| Inline var | New stem | File path |
|---|---|---|
| `_BLUR_GLSL` | `wave_blur` | `data/shader/wave/wave_blur.glsl` |
| `_BRIGHT_GLSL` | `wave_bright` | `data/shader/wave/wave_bright.glsl` |
| `_INNER_GLOW_GLSL` | `wave_inner_glow` | `data/shader/wave/wave_inner_glow.glsl` |
| `_WALL_GLSL` | `wave_wall` | `data/shader/wave/wave_wall.glsl` |
| `_LENS_GLSL` | `wave_lens` | `data/shader/wave/wave_lens.glsl` |

**Constraints (standing for this whole spec):**
- Working tree is heavily dirty (~40 modified files). **Targeted `git add <file>` only — NEVER `git add -A` or `git add .`.**
- Never skip hooks (`--no-verify`) or bypass signing. Fix the underlying issue if a hook fails.
- Combat ABILITIES gameplay is off-limits (not relevant to W1).

---

## File Structure

**Created:**
- `GachaClient/data/shader/wave/wave_blur.glsl` — 9-tap separable Gaussian (uniform `pixelStep` driving direction)
- `GachaClient/data/shader/wave/wave_bright.glsl` — luminance threshold extract (uniform `threshold`)
- `GachaClient/data/shader/wave/wave_inner_glow.glsl` — tc.y-driven pow-curve glow over per-segment mesh
- `GachaClient/data/shader/wave/wave_wall.glsl` — fiber-bundle cross-section + evanescent wave field
- `GachaClient/data/shader/wave/wave_lens.glsl` — screen-space pixelation + amber near-miss vignette

**Modified:**
- `GachaClient/ui/screens/dailies/WaveGame.lua` — delete 5 heredoc string locals (`_BLUR_GLSL`/`_BRIGHT_GLSL`/`_INNER_GLOW_GLSL`/`_WALL_GLSL`/`_LENS_GLSL`, ~145 lines) and 5 `newShader(STRING)` calls inside `_initGfx`; replace with 5 `Assets.shader(...)` calls.
- `GachaClient/tests/assets_harness/main.lua` — bump hardcoded shader count `20 → 25` and add 5 new stems to the discovery-validation list.

**Untouched:** WaveGame's bloom/lens/wall pipelines (canvases, draw order, uniform sends, mesh format) — bit-identical. The bespoke 4-canvas bloom is producer-internal per the 2c-2b decision.

---

## Task 1: Add the 5 shader files

**Files:**
- Create: `GachaClient/data/shader/wave/wave_blur.glsl`
- Create: `GachaClient/data/shader/wave/wave_bright.glsl`
- Create: `GachaClient/data/shader/wave/wave_inner_glow.glsl`
- Create: `GachaClient/data/shader/wave/wave_wall.glsl`
- Create: `GachaClient/data/shader/wave/wave_lens.glsl`

Each new `.glsl` file contains exactly the GLSL bytes from the corresponding `[[ ... ]]` heredoc in `GachaClient/ui/screens/dailies/WaveGame.lua` (lines 65–209), with the surrounding Lua `[[` / `]]` markers stripped. Leading 4-space indentation inside the heredocs is preserved — GLSL is whitespace-insensitive, but keeping it makes a diff against the inline source trivial to verify.

- [ ] **Step 1: Verify the parent directory exists (or create it)**

Run: `ls GachaClient/data/shader/wave/ 2>&1 | head -5`

Expected: either a listing of any pre-existing files, or "No such file or directory". If it doesn't exist, the Write tool's first file create will materialize it.

- [ ] **Step 2: Create `wave_blur.glsl`**

Write `GachaClient/data/shader/wave/wave_blur.glsl` with the following content (matches WaveGame.lua:65–80, indentation preserved):

```glsl
    uniform vec2 pixelStep;
    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        vec4 s = vec4(0.0);
        s += Texel(tex, tc - pixelStep * 4.0) * 0.0276;
        s += Texel(tex, tc - pixelStep * 3.0) * 0.0663;
        s += Texel(tex, tc - pixelStep * 2.0) * 0.1238;
        s += Texel(tex, tc - pixelStep * 1.0) * 0.1802;
        s += Texel(tex, tc)                   * 0.2041;
        s += Texel(tex, tc + pixelStep * 1.0) * 0.1802;
        s += Texel(tex, tc + pixelStep * 2.0) * 0.1238;
        s += Texel(tex, tc + pixelStep * 3.0) * 0.0663;
        s += Texel(tex, tc + pixelStep * 4.0) * 0.0276;
        return s * color;
    }
```

- [ ] **Step 3: Create `wave_bright.glsl`**

Write `GachaClient/data/shader/wave/wave_bright.glsl` with the following content (matches WaveGame.lua:82–90):

```glsl
    uniform float threshold;
    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        vec4  c   = Texel(tex, tc);
        float lum = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        float knee = max(0.0, lum - threshold);
        return vec4(c.rgb * (knee / max(lum, 0.001)), c.a) * color;
    }
```

- [ ] **Step 4: Create `wave_inner_glow.glsl`**

Write `GachaClient/data/shader/wave/wave_inner_glow.glsl` with the following content (matches WaveGame.lua:95–100):

```glsl
    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        float glow = pow(max(0.0, 1.0 - tc.y), 2.5);
        return vec4(color.rgb, color.a * glow);
    }
```

- [ ] **Step 5: Create `wave_wall.glsl`**

Write `GachaClient/data/shader/wave/wave_wall.glsl` with the following content (matches WaveGame.lua:141–209):

```glsl
    extern float time;
    extern vec2  screen_size;
    extern float phase_x_span;   // screen-space X width of the challenge zone

    float hash21(vec2 p) {
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
    }

    // Returns (distance to nearest hex centre in cell units, cell hash).
    // Standard two-candidate method: (1, sqrt(3)) rectangle grid, offset rows.
    vec2 hexGrid(vec2 p) {
        vec2 r  = vec2(1.0, 1.7320508);
        vec2 h  = r * 0.5;
        vec2 a  = mod(p,     r) - h;
        vec2 b  = mod(p - h, r) - h;
        vec2 gv = dot(a, a) < dot(b, b) ? a : b;
        vec2 id = floor(p - gv + 0.5);
        return vec2(length(gv), hash21(id));
    }

    // Phase-based tint: blue → indigo → teal across the level X span.
    vec3 phaseTint(float screenX) {
        float t  = clamp(screenX / phase_x_span, 0.0, 1.0);
        float s1 = clamp(t * 2.0, 0.0, 1.0);
        float s2 = clamp((t - 0.5) * 2.0, 0.0, 1.0);
        vec3 blue   = vec3(0.30, 0.80, 1.00);
        vec3 indigo = vec3(0.50, 0.30, 1.00);
        vec3 teal   = vec3(0.15, 0.85, 0.80);
        vec3 half1  = mix(blue, indigo, s1);
        vec3 half2  = mix(indigo, teal, s2);
        return mix(half1, half2, step(0.5, t));
    }

    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        // ── Fiber bundle cross-section ────────────────────────────────────────
        // Each hex cell is one sub-fiber core viewed end-on.  A Gaussian dot
        // marks the core; a rightward-traveling pulse mimics photon packets.
        float FSCALE    = 9.0;
        vec2  hexResult = hexGrid(sc / FSCALE);
        float coreDist  = hexResult.x * FSCALE;   // px from cell centre
        float cellHash  = hexResult.y;

        float core  = exp(-coreDist * coreDist * 0.22);

        float speed  = 0.06 + cellHash * 0.10;
        float travel = fract(sc.x / screen_size.x - time * speed + cellHash * 0.9183);
        float pulse  = exp(-pow(travel - 0.50, 2.0) * 80.0)
                     + exp(-pow(travel - 0.60, 2.0) * 250.0) * 0.30;

        float fiberGlow = core * (0.07 + 0.93 * pulse);

        // ── Evanescent wave field ─────────────────────────────────────────────
        // Photon energy leaks exponentially into the cladding from the core
        // axis (screen centre).  Two-wave interference adds spatial structure.
        float depth = abs(sc.y - screen_size.y * 0.5);
        float decay = exp(-depth * 0.018);
        float w1    = sin(sc.x * 0.048 + sc.y * 0.020 + time * 0.24);
        float w2    = sin(sc.x * 0.024 - sc.y * 0.042 + time * 0.17);
        float field = decay * smoothstep(0.30, 0.85, (w1 * w2 + 1.0) * 0.5);

        // ── Compose ──────────────────────────────────────────────────────────
        vec3 tint = phaseTint(sc.x);
        vec3 col = color.rgb
                 + tint * fiberGlow * 0.18
                 + tint * field     * 0.32;
        return vec4(col, color.a);
    }
```

- [ ] **Step 6: Create `wave_lens.glsl`**

Write `GachaClient/data/shader/wave/wave_lens.glsl` with the following content (matches WaveGame.lua:113–136):

```glsl
    uniform vec2  screen_size;
    uniform float nearMiss;  // near-miss vignette [0,1]

    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        // Pixelation: uniform 4px block grid across the whole screen.
        float blockSize = 4.0;
        float snappedX  = (floor(sc.x / blockSize) + 0.5) * blockSize;
        float snappedY  = (floor(sc.y / blockSize) + 0.5) * blockSize;
        vec2  pixTC     = clamp(vec2(snappedX, snappedY) / screen_size, vec2(0.001), vec2(0.999));
        vec4  scene     = Texel(tex, pixTC);

        // Amber edge vignette when near a wall
        if (nearMiss > 0.001) {
            float ex   = min(sc.x, screen_size.x - sc.x) / (screen_size.x * 0.5);
            float ey   = min(sc.y, screen_size.y - sc.y) / (screen_size.y * 0.5);
            float edge = 1.0 - pow(clamp(min(ex, ey), 0.0, 1.0), 0.45);
            scene.rgb  = mix(scene.rgb, vec3(1.0, 0.65, 0.08), edge * nearMiss * 0.52);
            scene.rgb += vec3(0.12, 0.06, 0.0) * edge * nearMiss * 0.28;
        }

        return scene * color;
    }
```

- [ ] **Step 7: Sanity-glob the new files**

Run: `ls GachaClient/data/shader/wave/`

Expected (in any order):
```
wave_blur.glsl
wave_bright.glsl
wave_inner_glow.glsl
wave_lens.glsl
wave_wall.glsl
```

- [ ] **Step 8: Commit the new shader files**

```bash
git add GachaClient/data/shader/wave/wave_blur.glsl GachaClient/data/shader/wave/wave_bright.glsl GachaClient/data/shader/wave/wave_inner_glow.glsl GachaClient/data/shader/wave/wave_wall.glsl GachaClient/data/shader/wave/wave_lens.glsl
git commit -m "feat(render): add WaveGame shaders as standalone .glsl files (homog W1 task 1)"
```

Expected: one commit, five files added. Run `git status GachaClient/data/shader/wave/` to confirm clean. The working tree's other modified files MUST remain unstaged — verify with `git status --short | grep -E '^(M|A|D)' | head -20` showing untouched M/A counts other than these five.

---

## Task 2: Update assets_harness shader-discovery assertions

The harness recursively scans `client/data/shader` and asserts (a) the discovered stem count and (b) a fixed list of expected stems. Both must learn about the five new wave shaders or the harness will fail.

**Files:**
- Modify: `GachaClient/tests/assets_harness/main.lua:144` (count assertion)
- Modify: `GachaClient/tests/assets_harness/main.lua:145–151` (stem validation list)

- [ ] **Step 1: Run the harness BEFORE the change to confirm it fails as expected**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`

Expected: at least one FAIL — specifically `assertEq` failure at the line that says `20 shaders discovered by recursive scan` (the actual count is now 25 because Task 1 added 5 files). The summary line should read `N CHECK(S) FAILED` with `N >= 1`. This is the RED step that justifies the upcoming change.

If for some reason this passes despite Task 1's files being on disk, STOP and investigate — most likely Task 1's commit didn't materialize the files. Re-run `ls GachaClient/data/shader/wave/`.

- [ ] **Step 2: Bump the count assertion**

Edit `GachaClient/tests/assets_harness/main.lua` line 144. Change:

```lua
        assertEq(n, 20, "20 shaders discovered by recursive scan")
```

To:

```lua
        assertEq(n, 25, "25 shaders discovered by recursive scan")
```

- [ ] **Step 3: Add the five new stems to the validation list**

Edit `GachaClient/tests/assets_harness/main.lua` lines 145–151 (the existing `for _, k in ipairs({ ... })` block). Add five new entries — `wave_blur`, `wave_bright`, `wave_inner_glow`, `wave_wall`, `wave_lens` — at the end of the existing list. The full block becomes:

```lua
        for _, k in ipairs({ "fxaa", "fsr_easu", "fsr_rcas", "lanczos", "cas_sharpen",
                             "post_process", "blur", "pixelate", "void_background",
                             "pull_background", "fiber_bg", "map_edge_fade",
                             "warp_energy", "pull_tunnel", "portal_distort", "glass_lattice",
                             "star_fill", "holo_outline", "card_glow_trail", "pull_results_fx",
                             "wave_blur", "wave_bright", "wave_inner_glow", "wave_wall", "wave_lens" }) do
            if not stems[k] then fail("shader stem '" .. k .. "' not found after reorg") end
        end
```

- [ ] **Step 4: Re-run the harness to confirm GREEN**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`

Expected: the final summary prints `ALL ASSET HARNESS CHECKS PASSED` and the process exits 0. Specifically the line `== real-fs shader scan (mounted GachaClient) ==` should now show no FAIL entries — the count matches and all 25 stems are found.

- [ ] **Step 5: Commit the harness update**

```bash
git add GachaClient/tests/assets_harness/main.lua
git commit -m "test(assets): cover wave_* shader stems in discovery harness (homog W1 task 2)"
```

Expected: one commit, one file modified.

---

## Task 3: Migrate WaveGame.lua to load via `Assets.shader`

The actual switch. After this commit, the inline GLSL strings are gone and `_initGfx` loads all 5 wave shaders the same way it already loads `fiber_bg`.

**Files:**
- Modify: `GachaClient/ui/screens/dailies/WaveGame.lua` — delete inline string locals at lines 65–209 (the five `local _X_GLSL = [[ ... ]]` blocks AND any descriptive comment block headers that are now orphans); change 5 lines in `_initGfx` (the `_X_Shader = _X_Shader or love.graphics.newShader(_X_GLSL)` lines).

The local module variables (`_blurShader`, `_brightShader`, `_innerGlowShader`, `_wallShader`, `_lensShader`) STAY — their identifiers, their lazy-init pattern, and every callsite are unchanged. Only the value-producing expression on the right of `or` changes.

- [ ] **Step 1: Re-read the inline GLSL block to know exact boundaries**

Use the Read tool: `Read(file_path="D:/dev/starworks/Gacha/GachaClient/ui/screens/dailies/WaveGame.lua", offset=60, limit=155)`

Expected: the output begins with the section header comment around line 59–64 (`-- ── Shader pipeline ──...`) followed by the five `local _X_GLSL = [[ ... ]]` blocks ending around line 209. Note the exact starting line of each block to drive the Edit calls in step 2.

Reference (from the spec, may shift by ±2 if the file has been touched):
- Lines 59–64: section comment header (KEEP — still relevant to `_initGfx`)
- Lines 65–80: `_BLUR_GLSL`
- Lines 82–90: `_BRIGHT_GLSL`
- Lines 92–100: `_INNER_GLOW_GLSL` (and the descriptive comment above it on lines 92–94)
- Lines 102–108: `_INNER_GLOW_FMT` (KEEP — mesh format, unrelated to inline shader strings)
- Lines 110–136: `_LENS_GLSL` (and its descriptive header on lines 110–112)
- Lines 138–209: `_WALL_GLSL` (and its descriptive header on lines 138–140)

- [ ] **Step 2: Delete the five inline GLSL string locals**

Open `GachaClient/ui/screens/dailies/WaveGame.lua` and remove the five `local _X_GLSL = [[ ... ]]` heredoc blocks. Keep the section header comment (`-- ── Shader pipeline ──...`), keep `_INNER_GLOW_FMT` (it's a mesh vertex format, not a shader string), and keep the descriptive comments where they continue to describe the shader the next-loaded variable holds.

Concretely, the post-edit shape of the deleted region:

```lua
-- ── Shader pipeline ───────────────────────────────────────────────────────────
-- _blurShader    : 9-tap separable Gaussian (parameterised by pixelStep uniform)
-- _brightShader  : luminance threshold extract → feeds blur input
-- _innerGlowShader : per-segment tunnel gradient; tc.y=0 at wall surface, 1 at IDEP depth inward
-- Bloom uses two H+V blur passes at increasing step sizes for wide soft falloff.

-- Vertex format for the inner glow mesh: position + UV only.
-- Color is driven entirely by love.graphics.setColor so the shader receives it
-- cleanly via the `color` argument without needing per-vertex color storage.
local _INNER_GLOW_FMT = {
    {"VertexPosition", "float", 2},
    {"VertexTexCoord", "float", 2},
}

local _blurShader      = nil
local _brightShader    = nil
local _innerGlowShader = nil
local _wallShader      = nil   -- fiber-bundle + evanescent-field wall material
local _innerGlowMesh   = nil   -- 4-vertex dynamic mesh; reused every segment
local _bgShader        = nil   -- fiber optic background (loaded from file)
local _lensShader      = nil   -- post-process pixelation + near-miss vignette
local _canvasScene     = nil   -- full-res: full game world render
local _canvasGlow      = nil   -- half-res: brightpass + double-blur result
local _canvasBlur      = nil   -- half-res: intermediate blur ping-pong
local _canvasComp      = nil   -- full-res: bloom composite; input to lens pass
local _gfxGen          = -1    -- last graphics-settings generation the canvases were built at
local _vigMeshes       = nil   -- 4 pre-allocated fan meshes for corner vignettes
```

I.e.: the section header + `_INNER_GLOW_FMT` block + all the `local _X = nil` module vars stay; the five heredoc strings and their immediately-preceding descriptive comments (the lines that introduce them as "Screen-space post-process:" etc.) all delete. The total deletion is ~145 lines.

Use the Edit tool with a sufficiently large `old_string` to uniquely match this region (start anchored at the unique `Bloom uses two H+V blur passes` comment and end anchored at the unique `Vertex format for the inner glow mesh` comment, OR delete each heredoc individually with five Edit calls anchored on its `local _X_GLSL = [[` opening). Either approach is fine — Edit will reject ambiguous matches, so the implementer can lean on that.

- [ ] **Step 3: Switch the five shader-creation lines to `Assets.shader`**

In the `_initGfx(sw, sh)` function (currently around line 225–265), find the block:

```lua
    _blurShader      = _blurShader      or love.graphics.newShader(_BLUR_GLSL)
    _brightShader    = _brightShader    or love.graphics.newShader(_BRIGHT_GLSL)
    _innerGlowShader = _innerGlowShader or love.graphics.newShader(_INNER_GLOW_GLSL)
    _wallShader      = _wallShader      or love.graphics.newShader(_WALL_GLSL)
    _bgShader        = _bgShader        or Assets.shader("fiber_bg")
    _lensShader      = _lensShader      or love.graphics.newShader(_LENS_GLSL)
```

Replace it with:

```lua
    _blurShader      = _blurShader      or Assets.shader("wave_blur")
    _brightShader    = _brightShader    or Assets.shader("wave_bright")
    _innerGlowShader = _innerGlowShader or Assets.shader("wave_inner_glow")
    _wallShader      = _wallShader      or Assets.shader("wave_wall")
    _bgShader        = _bgShader        or Assets.shader("fiber_bg")
    _lensShader      = _lensShader      or Assets.shader("wave_lens")
```

`Assets` is already required locally inside `_initGfx`:

```lua
    local Settings = require "services.Settings"
    local Assets   = require "services.Assets"
```

so no new require is needed.

- [ ] **Step 4: Syntax-check the touched file (headless, fast)**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`

Expected: `ALL ASSET HARNESS CHECKS PASSED`. The harness syntax-checks `ui/screens/dailies/WaveGame.lua` via `loadfile` (see line 22 of the harness — it's in the `touched` list); if the inline-string deletions left dangling syntax, this is where it surfaces.

- [ ] **Step 5: Visual gate — launch the game and confirm WaveGame is bit-identical**

Run the game manually (this requires the user — the headless harness cannot exercise GL):

1. Launch with the run script: `./GachaClient/run.bat` (or whatever the user uses). Wait for the login → click-to-play landing → world.
2. Press **F4** to push the Dailies screen, then enter the Wave minigame from there.
3. Play one wave run (hold SPACE/W/UP to ascend, release to dive). Observe:
   - Fiber-optic background renders (this was already `Assets.shader("fiber_bg")`; should be unchanged).
   - Tunnel walls render with the hex-fiber pattern (`wave_wall`).
   - Wall inner-glow falloff renders (`wave_inner_glow`).
   - Brightpass + bloom blur visible on the wave head and edges (`wave_bright` + `wave_blur` two-pass).
   - On a near-miss, amber edge vignette appears (`wave_lens` `nearMiss > 0`).
   - Pixelation grid visible across the whole screen (`wave_lens` pixelation block).
4. **STOP HERE** and have the user confirm visually that WaveGame looks identical to before this change. Take a screenshot to `GachaClient/.screenshots/wave_w1_check.png` (the F12 binding if present, or OS screenshot). The user is the gate — they must confirm "looks identical" before commit. Do NOT proceed past this checkpoint without their confirmation.

If the user reports any visual difference: a single broken shader points to either (a) wrong stem in `Assets.shader(...)` for that slot, (b) heredoc bytes diverged from the `.glsl` file content (re-diff against the WaveGame.lua history before Task 1 — `git show HEAD~3:GachaClient/ui/screens/dailies/WaveGame.lua | awk 'NR>=65 && NR<=210'` — and compare). Roll back by reverting the WaveGame.lua change (`git restore GachaClient/ui/screens/dailies/WaveGame.lua`) and investigate before re-trying.

- [ ] **Step 6: Commit the WaveGame migration**

Once the user has confirmed visual parity:

```bash
git add GachaClient/ui/screens/dailies/WaveGame.lua
git commit -m "feat(render): WaveGame shaders via Assets.shader (homog W1 task 3)"
```

Expected: one commit, one file modified, ~145 lines removed and ~5 lines changed.

- [ ] **Step 7: Final assets_harness pass + git-status hygiene check**

Run: `ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness`

Expected: `ALL ASSET HARNESS CHECKS PASSED`.

Then run: `git log --oneline -3`

Expected (3 most recent commits):
```
<sha> feat(render): WaveGame shaders via Assets.shader (homog W1 task 3)
<sha> test(assets): cover wave_* shader stems in discovery harness (homog W1 task 2)
<sha> feat(render): add WaveGame shaders as standalone .glsl files (homog W1 task 1)
```

Then run: `git diff --stat HEAD~3..HEAD`

Expected: exactly 7 files touched across the 3 commits — 5 new `.glsl`, 1 `WaveGame.lua`, 1 `assets_harness/main.lua`. No other files. If any other file appears, a stale staged path slipped in — investigate before continuing to W2.

---

## Verification summary

| Check | Where it runs | Gate |
|---|---|---|
| File system has 5 new shaders | `ls GachaClient/data/shader/wave/` | Task 1 step 7 |
| Manifest discovers them as unique stems | assets_harness real-fs scan (25 count, 25 stems in list) | Task 2 step 4 |
| WaveGame.lua still parses (no syntax break from deletion) | assets_harness loadfile pass | Task 3 step 4 |
| WaveGame visually identical in-game | User checkpoint at Task 3 step 5 | Visual gate |
| No collateral files committed | `git diff --stat HEAD~3..HEAD` exactly 7 files | Task 3 step 7 |

No GL test exercises the actual shader compile headlessly — the visual gate at Task 3 step 5 is the only place a bad GLSL byte-extract gets caught. Worth doing slowly.

## Risks (carried from the spec, with W1-specific mitigations)

| Risk | Mitigation in this plan |
|---|---|
| Stem collision with existing `blur` | Resolved at plan time: all five new stems use `wave_*` prefix. Confirmed unique against the 20 existing stems. |
| Heredoc-to-file byte divergence | Plan steps inline the exact GLSL bytes for each file (Task 1 steps 2–6). Diff-able against `git show HEAD:GachaClient/ui/screens/dailies/WaveGame.lua` for the original heredoc bodies. |
| Working tree contamination | Each commit step uses targeted `git add <file>` enumerating each file by name. No `-A`, no `.`. Task 3 step 7 audits via `--stat`. |
| Game-load order issue (shaders requested before disk mount) | Not a risk: `Assets.shader("fiber_bg")` already runs in `_initGfx` and works. The five new shaders use the identical code path. |
| Subagent skips the visual checkpoint | Task 3 step 5 is explicitly user-gated. Do NOT commit Task 3 without the user's explicit confirmation. |

---

## Out of scope for W1

- **WaveGame's bespoke 4-canvas bloom pipeline** (`_canvasScene` / `_canvasGlow` / `_canvasBlur` / `_canvasComp`): producer-internal effect, locked by 2c-2b decision. Untouched.
- **`_canvasGlow` / `_canvasBlur` using raw `love.graphics.newCanvas`** (bypass `Settings.newCanvas`): canvas-pool inconsistency — separate audit workstream.
- **Lens pixelation block possibly duplicating `post_process.glsl`'s `pixelate_size` uniform**: lens does pixelation AFTER bloom for a specific look reason; folding it into the shared grade would change effect order. Not a W1 concern.

These each get their own plan if they become priorities. W1 stays narrow.
