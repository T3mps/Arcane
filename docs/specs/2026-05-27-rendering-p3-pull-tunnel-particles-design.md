# Rendering Phase 3 — Pull-Reveal Performance: Particles → Geometry Design

**Date:** 2026-05-27
**Scope:** GachaClient (Love2D / LuaJIT / OpenGL)
**Status:** Approved design, pending implementation plan
**Parent spec:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` (Phase 3 — caching). This sub-spec **re-scopes** Phase 3 against current reality.

## Context & re-scoping

The master spec's Phase 3 ("caching") had three parts:
1. **Structural de-duplication** (the `effects` pass samples `sceneRT` instead of re-invoking the bg shader) — **already delivered in 2c-2** (`FrostedGlass.fromScene`, reveal 2–3×→1×). ✅
2. **`CachedPass`** (over `lib/Memo`) — throttled/reduced-scale re-render of heavy *time-driven* shaders into a *persistent* RT, for **ambient** backgrounds.
3. **Per-background policy** (static/ambient/interactive).

The original ~50fps motivator (FrostedGlass double-rendering `pull_tunnel`) was fixed in 2c-2. Per the **profiler-first** mandate, the user re-confirmed the current hotspot: **the gacha pull reveal is still below FPS target.** Because the reveal **animates every frame**, temporal caching (`CachedPass`, reduced *rate*) does not apply — it would stutter. So Phase 3 is re-scoped to **fix the reveal's measured cost directly.**

Investigation of `pull_tunnel.glsl` (the reveal's full-screen background, `ui/components/PullTunnel.lua` → `ShaderRegistry.backgrounds.pull_tunnel`) found it runs **~397 per-pixel loop iterations** across **seven** loops:

| Loop (function) | Iterations | Nature |
|---|---|---|
| speed lines (`tunnelEffect`) | 32 | angular radial streaks |
| flying particles (`flyingParticles`) | 30 | **point blobs** (gaussian) |
| spiral energy (`spiralEnergy`) | ≤3 | angular spirals |
| volumetric rays (`volumetricRays`) | 12 | angular rays |
| particle dust (`particleDust`) | 80 | **point blobs** (gaussian) |
| space dust L1/L2/L3 (`spaceDust`) | 120 + 80 + 40 | **point blobs** (sharp / glow) |

At native resolution (full window, ~2M px at 1080p) every frame. **~350 of those iterations are point-particle/dust blobs** whose position/size/amplitude depend only on `fi` + `time` (pixel-invariant), yet are recomputed for all 2M pixels, and the structure is `O(pixels × particles)` — each pixel evaluates all 350 blobs though it's within range of a handful. This particle mis-architecture (a full-screen loop instead of geometry) is the dominant cost, not a "heavy shader" needing micro-tuning.

## Decision (locked with the user, 2026-05-27)

**Technique = B2: move the point-particle/dust loops to batched geometry; keep the look.** (Chosen over B1 lossless CPU-hoist — which keeps the `O(pixels×397)` structure and realistically only buys ~1.5–2× — and over reduced-resolution rendering, which the user rejected to preserve the reveal's crispness.)

- The **5 point-blob loops** (`flyingParticles`, `particleDust`, `spaceDust` ×3 = **350 blobs**) become real **additive point-sprite geometry** (one soft-blob quad per particle in a `SpriteBatch`), turning `O(pixels × particles)` into `O(particles)`.
- The **angular/structural loops stay in the slimmed full-screen shader**: tunnel rings, the 32 speed-lines, 12 volumetric rays, ≤3 spirals, central light, depth fog, vignette, bloom. (They're screen-space-coherent angular structure, not point blobs; moving them to geometry is awkward and they're a small slice of the cost.) Shader drops from ~397 to ~47 loop iterations/pixel.
- **Look fidelity is near-identical, not bit-identical** (inherent to B2: sprite-texture falloff vs the analytic `exp`/`smoothstep`). The visual before/after gate is the judge; blob textures/scales are tunable if off.
- Scope = `pull_tunnel`/the reveal only. Temporal `CachedPass`/`Memo`/persistent-RTs stay **deferred** (no flagged ambient hotspot; Phase 6 builds its own dirty-tracked SDF).

## Architecture

Both pieces composite into the **scene layer** (`ctx.sceneRT`), before widgets (which already draw in the `ui` layer post-2c-2). `PullTunnel:draw()` becomes: (1) draw the slimmed full-screen shader, then (2) draw the additive blob batch on top — both into the current scene target.

### Slimmed full-screen shader (`pull_tunnel.glsl`)
Remove `flyingParticles`, `particleDust`, and `spaceDust` (3 layers) and their call sites in `effect()`. Keep `tunnelEffect` (incl. its 32 speed-lines), `centralLight`, `depthFog`, `spiralEnergy`, `volumetricRays`, `vignette`, `applyBloom`, and the HSV/utility helpers. The uniform interface (`time`, `intensity`, `rotation`, `rarity_color`, `rarity_glow_color`, `depth_scale`, `screen_size`, `rarity_level`, `center_offset`) is unchanged.

### Additive blob batch (`systems/render/BlobBatch.lua` — new, reusable)
A small, general additive soft-blob renderer (NOT a full particle engine), so Phase 4's particle batching reuses it:
- Holds a `SpriteBatch` over a procedurally-generated **soft-blob texture** (a radial gaussian; plus a sharp-point variant for `spaceDust` L1/L2, and the L3 "core+glow" = core sprite + a dimmer larger glow sprite). One batch per texture variant, or a single batch with an atlas — decided in planning to keep draw calls minimal (target: ≤ a few draws for all 350).
- API: `BlobBatch:set(particles)` (fills/clears the batch from a flat list/buffer of `{x, y, scale, r,g,b, alpha}`) and `BlobBatch:draw()` (one additive-blend draw per batch). FFI-friendly: fed from a flat reused buffer, no per-particle table churn per frame.

### Particle model (`lib/` pure module — new, headless-testable)
Each blob is a deterministic function of `time` (matching the GLSL's `phase = fract(time*speed + offset)` over a `1/depth` perspective). Rather than bit-mirroring the GLSL `hash`, each particle gets a **stable per-particle seed** (angle / speed / phase-offset / layer params, generated once at construction) — an equivalent particle field with the same distribution and motion. Per frame, the pure module evaluates each particle's screen `{x, y, scale, color, alpha}` from `time` + its seed + `screen_size`/`center_offset`/`rarity` (mirroring each loop's perspective + fade math: `zDepth`, `perspective = 1/zDepth`, `screenR`, position, size, intensity, journey fade-in/out). `PullTunnel.lua` feeds the result into `BlobBatch`.

### HDR synergy (free)
Blobs draw **additively into the rgba16f `sceneRT` before the 2c-3 grade**, so bright cores exceed 1.0 and get ACES rolloff + bloom — coherent with the rest of the now-HDR scene (same pipeline position as the old in-shader particle contribution, which was also pre-grade).

## Components & ownership

| Unit | Responsibility | New/changed |
|---|---|---|
| `data/shader/fx/pull_tunnel.glsl` | Remove the 5 point-blob loops + their `effect()` call sites; keep tunnel/speed-lines/rays/spirals/central light/fog/vignette/bloom | Modify (slim) |
| `ui/components/PullTunnel.lua` | Own a `BlobBatch` + the particle set; per frame compute blobs (via the pure module) and feed the batch; `draw()` = shader pass then batch pass; `update(dt)` advances time as today | Modify |
| `systems/render/BlobBatch.lua` | Reusable additive soft-blob `SpriteBatch` renderer (`set`/`draw`) | New |
| `lib/<particle-math>.lua` | Pure: stable-seed generation + per-frame position/size/alpha evaluation per loop type. No LÖVE deps | New (pure) |
| blob-texture generation | Procedural soft-gaussian + sharp-point (+ glow) blob textures, created once (via `services.Assets` or `love.graphics.newCanvas` at init) | New (small) |

## Testing & verification

- **Profiler-first (primary gate):** reveal FPS before/after via F9 — the win must show; target = the reveal back at the user's FPS target. CPU frame-time + draw-call count (the batch should be a handful of draws vs the prior single ultra-heavy full-screen draw).
- **Headless (`tests/render_harness`):** the pure particle-math module — stable seeds are deterministic; per-frame evaluation follows the `1/depth` perspective curve (e.g. `phase→0` maps near center, `phase→1` near edge); particle counts per layer correct (30/80/120/80/40); journey fade-in/out bounds. No GL.
- **Headless (`tests/assets_harness`):** if blob textures are procedurally generated as named assets, assert they exist/load (the harness already enumerates shader/texture stems).
- **Visual (user-gated, GL):** before/after the reveal must read as the same effect — particle density, motion, color, glow, and the tunnel structure unchanged. Blob textures/scales tunable if it drifts. (Controller cannot run GL.)

## Risks

- **Look fidelity** (sprite falloff vs analytic) → 1–2 tuned blob textures (gaussian + sharp, + glow for L3); visual gate; scale/alpha tunable.
- **Draw-call count** (350 sprites) → one `SpriteBatch` per texture variant (≤ ~3 draws), not per-particle draws. Confirm via the profiler draw counter.
- **Uniform/SpriteBatch capacity** → SpriteBatch sized to the max particle count once; reused each frame (`:clear()`/`:set`), no per-frame reallocation.
- **GC churn** → fill the batch from a flat reused buffer; no per-particle Lua tables per frame (FFI-friendly per the initiative mandate).
- **Determinism vs the old look** → stable per-particle seeds reproduce an equivalent field; exact particle identities differ but the distribution/motion match (visually equivalent). Acceptable per the B2 decision.
- **Scope creep into a particle engine** → `BlobBatch` is intentionally minimal (set/draw additive blobs); no simulation, forces, or emitters beyond what the reveal needs.

## Decomposition into implementation plan

A single plan (`docs/superpowers/plans/2026-05-27-rendering-p3-pull-tunnel-particles.md`), TDD on the pure particle-math, visual-gated on the GL. Suggested task order:

1. **Pure particle-math module** + headless tests (stable seeds, per-frame evaluation per loop type, counts, perspective/fade). No rendering yet.
2. **`BlobBatch`** + procedural blob texture(s) + assets-harness coverage. No consumer yet (or a trivial smoke draw).
3. **Wire `PullTunnel` + slim the shader together (the look-preserving swap).** In one task/commit: make `PullTunnel` compute blobs (pure module) and feed `BlobBatch` (draw shader, then batch), AND remove the 5 point-blob loops + their `effect()` call sites from `pull_tunnel.glsl`. They must land together — adding the sprites without removing the shader loops would double the particles; removing the loops without the sprites would lose them. Visual gate: before/after the reveal looks equivalent and F9 shows the FPS win; commit after the visual gate.

Each task ends with its own commit (targeted `git add` — the working tree is heavily dirty). The game stays playable; combat abilities are off-limits (n/a here).
