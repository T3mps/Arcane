# Phase 4 — Profiler-Driven Batching Design

**Date:** 2026-05-28
**Status:** Brainstormed → spec'd. Plan to follow.
**Spec'd in master:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` § Phase 4.
**Audit reference:** `docs/audits/2026-05-28-aaa-gap-audit-perf-batching.md` (G-CAP-3, G-CAP-4, G-CAP-5, G-CAP-6, G-CAP-7).
**Gate that unlocked this:** CP-3 (per-scope draw stats + headless CSV capture) shipped 2026-05-28 commit `2eb7a4c`.

---

## Goal

Reduce per-frame draw calls + state switches on the three known hot scenes — **combat**, **WaveGame**, and **gacha pull / inventory** — by:

1. Extracting two managed batch primitives (`SpriteBatch`, `MeshBatch`) seeded from the Phase-3 `BlobBatch` pattern.
2. Migrating the two loudest immediate-mode hotspots (combat grid, WaveGame tunnel) to those primitives.
3. Adding a runtime atlas pipeline so texture-shared sprites (portraits, item icons, rarity frames) auto-batch.
4. Adding an opt-in per-Container state-sort so widget trees like `grid_view` stop breaking the auto-batcher every cell.
5. Adding widget-level + screen-level culling for off-screen geometry.
6. Batching one UI worst offender (Panel chrome) as proof the same pattern scales to UI surfaces.

Validation is empirical: every lever ships before/after captures via the CP-3 `--profile-capture` CLI, saved alongside this spec.

---

## Architecture

Two new render primitives live next to `BlobBatch` in `systems/render/`. Both follow the same state-preserving lifecycle as `BlobBatch` (begin → add → draw, restore blend + color on exit). Consumers replace immediate-mode primitives with these.

Atlases extend the existing icon registry pattern (one shared canvas, per-key quads) but operate on image-file textures rather than procedurally-baked shapes. Built at boot from a manifest. `Assets.image(key)` gains a 2-value return: `(texture, quad)` for atlased keys, `(texture, nil)` for standalone keys.

State-sort is implemented as a per-Container opt-in (`sortChildren = true`) plus a per-Widget `batchKey` field. Stable sort preserves z within equal-key groups. No change to default behavior — every existing screen renders identically until it opts in.

Culling is widget-level (`Widget.cullToViewport`) plus screen-specific (WaveGame segment cull, combat indicator cull).

The canonical Layer/Profile/Renderer compositor stays untouched — Phase 4 is consumer-side optimization, not pipeline restructure. CP-1's per-pass attribution lets us measure each lever per-scope.

---

## Tech stack

- **LÖVE 11.x** `SpriteBatch` + `Mesh` as the underlying primitives.
- **LuaJIT** — managed wrappers use plain Lua tables; primitives are FFI-friendly (typed buffers, no GC churn on the hot add-path) per the engine-wide FFI direction (`MEMORY.md` "Engine-wide FFI optimization") but don't depend on FFI yet.
- **CP-3 `--profile-capture`** as the validation gate. Five reference scenes: login (baseline), overworld, F2 inventory, F4 dailies, F5 combat.
- **Existing canonical compositor** (`Layer.lua`, `Profile.lua`, `Renderer.lua`, `RenderTargets.lua`, `FrameAA.lua`, `RenderScale.lua`, `PostFx.lua`).

---

## Levers (in sequenced order)

### 1. Foundation — `SpriteBatch.lua` + `MeshBatch.lua`

Both modules sit next to `BlobBatch.lua` in `systems/render/`. State-preserving managed wrappers over LÖVE's primitives.

**SpriteBatch API:**
```lua
local sb = SpriteBatch.new(texture, capacity, {
    usage = "stream",                        -- love.graphics SpriteBatch usage
    blend = {"alpha", "alphamultiply"},      -- restored on draw
})
sb:begin()
sb:setColor(r, g, b, a)                      -- color of the next add()
sb:add(x, y, rot, sx, sy, ox, oy)            -- forwards to the underlying SpriteBatch
sb:draw(x, y)                                -- emits + restores blend/color/setColor
```

**MeshBatch API:**
```lua
local mb = MeshBatch.new(vertexFormat, capacity, {
    mode    = "triangles",
    usage   = "stream",
    texture = nil,                           -- optional sampled texture
    blend   = nil,                           -- optional blend override
})
mb:begin()                                   -- write head -> 0
mb:addVertex(x, y, r, g, b, a)               -- raw vertex
mb:addTriangle(x1, y1, x2, y2, x3, y3, color)
mb:addQuad(x, y, w, h, color)                -- convenience: 2 triangles
mb:flush()                                   -- upload pending vertices to underlying Mesh
mb:draw(x, y)                                -- emits + restores state
```

`BlobBatch` becomes a wrapper of `SpriteBatch` + its blob texture; its public API stays exactly the same so `PullTunnel.lua:48` is untouched.

**Tests (headless):** harness validates capacity bounds, blend restoration, color restoration, flush/draw alias semantics. Uses injectable factory fakes (the same pattern `RenderTargets` uses today).

### 2. POC 1 — Combat grid via MeshBatch

**File:** `GachaClient/ui/screens/combat/CombatRenderer.lua`.

**Current state:** `_drawGrid` runs a Lua double-loop over a 10×10 grid (`CombatRenderer.lua:349-353`) drawing `polygon("fill")` + `polygon("line")` per tile. Highlight pulses, hover ring, unit shadows, HP bars, status icons are also immediate-mode primitives.

**Target:** ONE `MeshBatch` holds the 100 tile diamonds, built once on grid load (vertex positions are static — tiles don't move). Per-tile colors stream via `:begin()` + 100×`:addQuad` per frame. The frame's grid layer becomes ONE draw call.

Highlight pulses become a separate `MeshBatch` (variable count, stream-mode). Hover ring stays immediate (one ring per frame is fine).

HP bars + floating damage numbers + status icons stay as-is for now — they're small in absolute draw count and their batching is a follow-up that pairs better with the atlas + text-batch work in Phase 6 surface.

**Validation:** before/after `--profile-capture 4 --output before/combat.csv` and `after/combat.csv`. Target: `scene.drawcalls` per frame drops from ~120 (grid 100 + highlights N + chrome) to ~8-12.

### 3. POC 2 — WaveGame tunnel via MeshBatch

**File:** `GachaClient/ui/screens/dailies/WaveGame.lua`.

**Current state:** `_drawTunnel` (lines 881-887, 920-954) issues `polygon("fill")` calls for each segment, run twice — once for the segment fill, once for the inner-glow bloom pass. ~150 segments × 5 polys × 2 passes ≈ 1500 polys/frame at peak.

**Target:** ONE `MeshBatch` per bloom pass streams every segment's triangles. `mb:begin()` per frame, `mb:addTriangle` for each segment edge, `mb:flush()` after the loop. Two `MeshBatch` instances (one per pass) means 2 draw calls per frame instead of ~1500.

The reused 4-vertex `_innerGlowMesh` fan (line 116) stays as-is — it's already a Mesh, just gets one `:flush()` call inside the wrapper. The four corner-vignette fan meshes (line 92) likewise.

Near-miss lens vignette stays immediate (it's one shader pass, not a polygon storm).

**Validation:** before/after capture of F4 at peak segment count. Target: `scene.drawcalls` per frame on WaveGame drops from ~1500+ to ~5.

### 4. Atlas pipeline — `services/assets/atlas.lua`

Runtime bake at boot. Atlases declared by manifest:

```json
// data/atlases/ui.json
{
  "max_size": 2048,
  "filter":   "linear",
  "keys": [
    "portrait_*",
    "item_*",
    "rarity_*"
  ]
}
```

`Atlas.bake(name)` loads each matching key's source image (existing `Assets.image` path with `atlas = false` to bypass the new atlas-aware branch — internal-only), packs into a 2048² (configurable) canvas using **skyline-pack** (small, well-bounded, O(n log n)), returns `(canvas, key_to_quad_map)`. The pack is deterministic — sort inputs by descending height before placement.

`Assets.image(key)` gains a 2-value return:
```lua
local tex, quad = Assets.image("portrait_ravi")
-- atlased:  tex = atlas canvas, quad = packed Quad
-- standalone: tex = standalone Image, quad = nil
```

`Assets.draw(key, x, y, w, h, opts)` is a new helper that handles the optional quad transparently — widget consumers route through this rather than calling `love.graphics.draw` with their own scale math (mirrors the `IconRegistry.drawSized` pattern from Phase 4b).

**Initial atlas: `ui`** — covers party portraits, item icons, rarity frames. Adds ~50-200ms at boot (one-time, before login). A second `combat` atlas can come later for combat-specific sprites once the shape is proven.

**Failure modes:**
- Missing source for a declared key → log + skip (atlas misses that key; `Assets.image(key)` returns `(nil, nil)`, same as today's missing-image path).
- Atlas overflow → log + fall back to standalone-image path for the overflowing keys (same fallback shape as IconRegistry's per-icon-canvas overflow).
- Hot-reload (QW-2): if any atlas source's mtime changes, re-bake the atlas (existing shader hot-reload pattern, generalized).

**Validation:** F2 (inventory) + F3 (party) captures show portrait + item-icon draws collapse into shared-texture auto-batch runs. Target: F3 `ui.drawcalls` drops by ~30% (4 portraits × N item icons all under one atlas binding).

### 5. State-sort policy + grid_view POC

**Files:**
- `GachaClient/systems/render/BatchKey.lua` (new): pure helper computing `(shader, texture, blend)` keys.
- `GachaClient/ui/core/widget.lua`: `widget.batchKey` field (default nil).
- `GachaClient/ui/core/container.lua`: `container.sortChildren` flag (default false).

When `sortChildren = true`, the Container does a **stable** sort of children by their `batchKey` before draw. Children with `batchKey == nil` (or with `sortable = false`) keep their hand-authored position — they bookend the sortable runs. The stable property means z-equal children preserve hand-authored order.

**grid_view POC:** `ui/widgets/grid_view.lua` opts in. The current alpha→add→alpha→add interleave (per-card, lines 382-399) becomes:
- alpha sweep (all cells)
- additive sweep (all rim highlights)
- additive sweep (all rarity glow underlays)

Because the existing intra-card ordering only sets up z by render order within ONE pass, and the cell/rim/glow are at three logical depths that don't cross, sorting within each depth-pass is safe.

**Validation:** F1 (gacha) profile capture. Target: `ui.canvasswitches` per frame drops by ≥10×.

### 6. Culling + overdraw

**Widget-level:** `widget.cullToViewport = true` opt-in. Containers that scroll past viewport (item lists, character lists) check each child's absolute rect against the viewport before recursing. Children fully outside skip their draw + their subtree.

**WaveGame:** tunnel segments outside `[tunnelStart, tunnelEnd]` don't add to the `MeshBatch`. Already partially the case; verify + tighten.

**Combat:** off-grid unit indicators draw only when their target unit is off-grid. (RenderSystem already handled this for the deleted enemy suite per Phase 4a; verify combat-side logic is similar.)

**UI compositor:** no global cull — the layer compositor already gates by `visible`. The frosted scene-effect's full-screen sample is intrinsic to the effect and stays.

**Validation:** F3 (inventory list) capture with the inventory scrolled past 30 items. Target: `ui.drawcalls` plateaus once items are off-screen, doesn't grow linearly.

### 7. UI batching — Panel chrome

**File:** `GachaClient/ui/widgets/panel.lua` (~8 state ops per panel today: background + border + shadow + lit-edge highlight + frosted prep, per the audit).

**Target:** bundle Panel chrome geometry into one `MeshBatch` per instance. Vertex layout includes per-vertex color + per-vertex UV (so the lit-edge highlight pattern can stream as vertex colors instead of a separate gradient pass). Frosted-sampling layer stays separate (it's a scene-effect shader, can't fold into a mesh).

NOT in scope: rewriting the rest of the UI widgets to batch. The Panel POC validates the pattern; broader UI batching is a separate phase.

**Validation:** F2 capture with the typical 4-5 panels visible. Target: `ui.drawcalls` per panel drops from ~8 to 1-2.

---

## Validation strategy

Each lever ships with a CP-3 `--profile-capture` before/after pair saved to `docs/audits/p4/<lever>-<scene>-{before,after}.csv`. Plan task definition includes the specific scene + target metric.

**Reference scenes** (drive `--profile-capture` + a custom `--scene` flag added in lever-2 if needed):
- login (baseline / regression watch — already in the current CP-3 baseline at `/tmp/profile-capture-smoke2.csv`)
- overworld (no UI, world only)
- inventory F3 (UI-heavy, atlas-relevant)
- party F2 (UI-heavy, atlas-relevant)
- dailies F4 → WaveGame (POC 2)
- combat F5 (POC 1)
- gacha F1 + pull (state-sort + atlas POC)

**Regression gate:** the CP-3 baseline at each lever's start is committed alongside the spec. A follow-up CI step (later) can assert "no scope > budget for more than X% of frames" using these baselines. Not in scope for this phase but the captures make it trivial when wanted.

---

## Scope guards

- **Combat ABILITIES gameplay is off-limits** (`[[feedback_abilities_system_ownership]]`). Rendering plumbing inside `ui/screens/combat/` is OK; combat logic, ability data, damage execution are not.
- **No JSON-screen refactors.** The homog mandate says every change should leave consumers closer to JSON-migratable. Phase 4's lever-7 (Panel) touches a widget consumed by both hand-coded and JSON screens — the chrome change must preserve the public draw contract.
- **No FFI yet.** Primitives are shaped to be FFI-friendly (typed inputs, no GC churn on the add-path) but don't introduce ffi.new dependencies. FFI pass is a separate engine-wide workstream.
- **No GPU timer queries** (SD-4 — separate). Phase 4 still uses CPU submission time + draw stats.
- **One initial atlas only** (`ui`). Combat / character-portrait / sprite-sheet atlases stay future work until lever-4 is proven.
- **F2/F3 menus** are slated for a complete rework (`[[project_inventory_party_rework]]`). Phase 4's cross-cutting contract migrations (atlas-aware `Assets.image`, state-sort opt-in) on these screens are OK; non-trivial F2/F3 internals stay deferred.

---

## Open risks

1. **Combat grid Mesh:setVertexAttribute path.** Re-uploading per-vertex colors for 100 tiles every frame may be slower than the immediate-mode `setColor` stream we have today (`setColor` doesn't break the auto-batcher per the LÖVE 11.x batcher rules). Mitigation: bench both via CP-3; pick winner. If immediate-mode wins, the grid POC becomes a per-tile `polygon("fill")` stream + tightened color order rather than a Mesh, and the lever's value is in providing the API for the few places that genuinely benefit (WaveGame for sure).

2. **Atlas bake hits at boot.** Boot already does shader compilation + font rasterization + asset preload. Adding ~150ms is fine for dev but tightens once Phase 5 (async loader) lands. Mitigation: bake on a background thread once Phase 5 ships; until then, do it inline.

3. **State-sort + animation.** Widgets that animate must keep their position even if their batchKey doesn't change (it shouldn't, since shader/texture/blend rarely change mid-animation). Verify with AnimPlayer-driven screens (login transitions, the few cards that pulse).

4. **Hot-reload + atlas.** Editing one PNG invalidates the whole atlas. Acceptable for dev. The reload cost is the bake cost (~150ms), not noticeable.

5. **Capture deterministic-input.** Today `--profile-capture` boots the engine and captures whatever the engine renders. For reference scenes other than login we'll need either (a) auto-pushing a screen at boot via a `--scene <id>` flag, or (b) a small input scripting that presses F-keys after delay. (a) is the lighter change. Either lives in lever 0 / a CP-3 follow-up.

---

## Cross-references

- Master spec: `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` § Phase 4 (lines 131-140).
- Audit: `docs/audits/2026-05-28-aaa-gap-audit-perf-batching.md` (full perf domain — 18 gaps catalogued).
- Synthesis: `docs/audits/2026-05-28-aaa-gap-audit-synthesis.md` SD-1 ("Phase 4 batching").
- Foundation seed: `GachaClient/systems/render/BlobBatch.lua` (the one production batch primitive today).
- Memory: `[[aaa-rendering-initiative]]`, `[[feedback_abilities_system_ownership]]`, `[[project_inventory_party_rework]]`, `[[ffi_optimization]]`, `[[asset_manager]]`.

---

## Self-review

- [x] No placeholders, TBDs, or "implement later" markers.
- [x] Each lever has a concrete target metric + reference scene.
- [x] API examples are complete (signatures + types).
- [x] Scope guards explicitly named per applicable memory.
- [x] Risks include mitigations, not just statements of risk.
- [x] Validation strategy is mechanical (CSV diffs), not visual-only.
- [x] Sequenced so each lever ships working code + tests + a measurable win.
