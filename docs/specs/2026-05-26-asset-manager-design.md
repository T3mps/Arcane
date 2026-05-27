# Asset Manager — Design Spec

**Date:** 2026-05-26
**Scope:** GachaClient (Love2D / LuaJIT)
**Status:** Approved design, pending implementation plan

## Problem

The client loads everything from disk ad-hoc. Concretely:

- **Shaders** — ~15 modules each hand-roll the same `local s, tried; function load() if tried then return end; tried=true; local ok,x = pcall(love.graphics.newShader, "data/shader/X.glsl") ... end` dance (FrameAA, RenderScale, PostFx, VoidBackground, WarpEnergy, PullTunnel, FrostedGlass, SpaceBackground, CombatRenderer, …), plus a dynamic `"data/shader/ability_"..type..".glsl"` path.
- **Images** — at least three independent per-module caches (`ui/widgets/image.lua` `_cache`, `ui/particles/textures.lua` `cache`, plus `nine_slice`/`reveal_item` ad-hoc) all repeating `pcall(newImage, path, {mipmaps=true})`. A 1×1 "dummy image" idiom is copy-pasted across ~6 modules (WarpEnergy, FiberBackground, VoidBackground, PullTunnel, SpaceBackground, glass_lattice).
- **Fonts** — `ui/theme.lua` (rebuilds on viewport scale), `ui/icons/lucide.lua` (`_fonts[px]`), CombatRenderer, character_card — all independent.
- **Data JSON** — `love.filesystem.read` + `json.decode` scattered across ui_loader, string_table, ProgressionService, Settings, Input.
- **Audio** — no `love.audio.newSource` calls exist today (loading absent/stubbed).

Folder layout (`data/shader/`) is flat, and every move would touch ~25 call sites because paths are hard-coded at the point of use.

## Goals / Non-goals

**Goals**
- One unified API for every asset loaded from disk: shaders, images, fonts, data JSON, sound.
- Kill the duplicated lazy-load + pcall + cache boilerplate.
- Decouple call sites from folder layout via logical keys, making the pending `data/shader/` reorg (and future moves) trivial.
- Build ref-counting / eviction / async as designed-in **seams** (simple or stubbed now), ready for a later perf pass.
- Centralize per-type load options and invalidation (mipmaps/filter/anisotropy, font dpi-rebuild on viewport resize).

**Non-goals (now)**
- Full streaming, automatic eviction triggers, or threaded loading (seams only).
- Folding stateful persistence (`Settings`, `CacheManager`) into the manager — those write + version their own files and are not read-only assets.
- Converting WaveGame's inline-GLSL-*string* shaders (`_BLUR_GLSL`, etc.) — they are string literals, not disk files.
- Building FFI bookkeeping now (the data structures are merely shaped to port later).

## Driver (decided)

Primary driver is **unify + future-proof**, not current memory pressure or hitching. Therefore: build the clean unified API + central cache + invalidation now; keep eviction/async/ref-count as seams.

## Architecture (Approach A: facade + per-type loaders)

```
services/
  Assets.lua          -- public facade: the unified API + cache + invalidation wiring
  assets/
    manifest.lua      -- key <-> path resolution (auto-discovery by convention + overrides)
    cache.lua         -- central store: entries, refcount/size fields, LRU list
    shader.lua        -- newShader; plain; compile-fail -> nil
    image.lua         -- newImage; mipmaps + filter/anisotropy from Settings; 1x1 dummy fallback
    font.lua          -- newFont; keyed by (name,size,dpi); rebuilds on viewport scale
    data.lua          -- filesystem.read + json.decode; optional version-discard; {} fallback
    sound.lua         -- audio.newSource; "static"|"stream"; nil fallback
```

`Assets.lua` is the only module call sites import. Each `Assets.<type>(key, …)` resolves the key through `manifest`, checks `cache`, and on a miss delegates to the matching loader, stores the result, and returns it. Loaders are isolated — each knows only its own LÖVE constructor and option set, so font-dpi logic never tangles with shader-compile or json-versioning. `cache` and `manifest` are the two shared cores (and the single place the future FFI bookkeeping concentrates).

Named `Assets` because `CacheManager` is already taken (session/prefs persistence, unrelated).

## Public API

```lua
-- Accessors (key is always a logical name, never a path)
Assets.shader(key)            -> Shader|nil      -- Assets.shader("fxaa")
Assets.image(key, opts?)      -> Image           -- never nil; shared 1x1 dummy on failure
Assets.font(key, size, opts?) -> Font            -- Assets.font("din", 24)
Assets.data(key, opts?)       -> table           -- decoded JSON; {} on failure
Assets.sound(key, mode?)      -> Source|nil      -- mode "static"|"stream"

-- Lifecycle / scaling seams
Assets.preload(keys)          -- warm cache (synchronous now; async seam later)
Assets.release(type, key)     -- decrement refcount (no auto-evict yet)
Assets.evict(budgetBytes?)    -- drop refs==0 LRU entries (manual; not auto-triggered)
Assets.update(dt)             -- per-frame drain hook (no-op now; load-budget later)
Assets.stats()                -- per-type counts + bytes (feeds future eviction)
Assets.reload(key)            -- dev hot-reload (behind a flag; optional)

-- Invalidation
Assets.onViewportScale(scale) -- called by the resize path; rebuilds fonts at new dpiscale
```

**Cache key** = `type .. ":" .. key`, plus a param suffix for parameterized assets — fonts append `+size+dpi`; images append a small opts hash only when opts change the GPU object. Two call sites asking for the same shader share one entry.

## Key → path resolution (manifest)

- Each type has a **base dir + extensions**: shader → `data/shader/ *.glsl`; image → `data/image/ *.png|jpg`; font → `data/font/ *.ttf|otf`; data → `data/ *.json`; sound → `data/audio/ *.ogg|wav`.
- **Auto-discovery** at first use: recurse the base dir, derive `key = path-minus-basedir-minus-ext`. So `data/image/characters/ravi.png` → `"characters/ravi"`. **Shaders use stem-only keys** (`data/shader/post/fxaa.glsl` → `"fxaa"`) so moving them between subfolders never changes the key.
- **Overrides table** in `manifest.lua` for aliases / disambiguation / cross-dir cases (e.g. `space_bg → backgrounds/space.png`).
- **Ambiguity rule:** stem-only keys require stems be unique within a type. Duplicate stems (e.g. two `blur.glsl` in different subfolders) are detected at boot and **error loudly**, forcing an explicit override. Failing at boot is intended.

## Per-type loader behavior & invalidation

- **shader** — `pcall(newShader, path)`; compile failure logs once, returns `nil` (callers tolerate nil). Shaders are immutable once compiled, so they are **not** tied to `gfxGeneration` (that invalidation is for *canvases*, which the manager deliberately does not own — canvases stay on `Settings.newCanvas`). Shader entries are permanent unless `reload`'d.
- **image** — `pcall(newImage, path, {mipmaps=true})` + `setMipmapFilter("linear")`, anisotropy/filter applied centrally from `Settings`. Failure returns the one shared 1×1 dummy (collapses the 6 copied idioms). No gfxGen wiring (textures need no recreation on settings change).
- **font** — `pcall(newFont, path, size, hinting, dpiscale)`, cache-keyed by `(name,size,dpi)`. The one type with real invalidation: `Assets.onViewportScale(scale)` (called by the existing resize path) re-creates fonts at the new dpiscale. `theme.rebuildFonts`, `lucide._fonts`, CombatRenderer, character_card converge here.
- **data** — `filesystem.read` + `json.decode`; optional `{version=N}` discard for read-mostly files (protocol, strings, screens). Failure returns `{}` + logs once. `Settings` / `CacheManager` keep their own write+version logic.
- **sound** — `newSource(path, mode)`, `"static"` SFX / `"stream"` music; nil fallback. Slot exists even with no call sites today.

**Error handling (all types):** every load is pcall-wrapped; failures log **once per key** (dedup set) and return the type's typed fallback. No crashes, no log spam, no half-loaded state.

## Scaling seams (built as seams, simple now)

- **Ref-count** — each entry has a `refs` int, default retained (1) on first load. `release` decrements; eviction only ever considers `refs==0`. Nothing calls `release` yet, so nothing evicts — safe by construction.
- **Eviction** — `Assets.evict(budget)` is implemented (drop `refs==0` LRU entries until under budget) but wired to **no automatic trigger**. `stats()` feeds it.
- **Async/preload** — `preload` loads synchronously now, structured so disk-read + `ImageData` decode can move to a `love.thread` worker later, GL upload staying main-thread and time-sliced by `Assets.update(dt)` (no-op drain hook today).

## FFI posture (designed-for, not built)

Cache bookkeeping — LRU links, refcounts, byte sizes — is plain Lua now but shaped to port to `ffi.new` typed arrays during the planned engine-wide FFI pass. LÖVE asset objects (Shader/Image/Font userdata) **cannot** be FFI structs, so they live in a parallel Lua table; FFI only ever touches the bookkeeping.

## Migration (everything now)

- **Shaders:** ~15 modules + `RenderScale` + dynamic `ability_<type>` → `Assets.shader("…")`. WaveGame inline-string shaders stay inline.
- **Images:** `widgets/image.lua`, `particles/textures.lua` (loaded ones only), `nine_slice`, `reveal_item`, `main.lua` sprites; 6 dummies → shared fallback.
- **Fonts:** `theme.rebuildFonts`, `lucide`, CombatRenderer, character_card → `Assets.font`.
- **Data:** read-mostly callers (ui_loader, string_table, ProgressionService) → `Assets.data`. Settings/CacheManager untouched.
- **Sound:** slot wired, no call sites yet.

## Shader-dir reorg (trivial behind stem keys)

```
data/shader/
  post/        post_process, fxaa, pixelate, blur
  upscale/     lanczos, cas_sharpen, fsr_easu, fsr_rcas
  background/  void_background, pull_background, fiber_bg
  world/       sprite_element, map_edge_fade, ability_*
  fx/          warp_energy, pull_tunnel, portal_distort, glass_lattice,
               star_fill, holo_outline, card_glow_trail, pull_results_fx
```

Keys stay bare stems, so call sites are unaffected by the move.

## Testing

- **Unit (headless, no GL):** manifest resolution (key→path, overrides, ambiguity error), cache keying (param suffix, hit/miss, refcount/release), data loader (decode + version discard + fallback).
- **Smoke (live GL context):** shader compile + image/font creation verified via a boot load-all + report pass. Explicitly not unit-tested.

## Open risks

- Auto-discovery cost at boot: one recursive `getDirectoryItems` per type. Acceptable; can be cached to a generated manifest later if it ever matters.
- `data/` base dir for the data type is broad (the whole data tree contains json); the data loader scopes discovery to known read-only subsets or relies on explicit keys to avoid clashing with stateful files.
