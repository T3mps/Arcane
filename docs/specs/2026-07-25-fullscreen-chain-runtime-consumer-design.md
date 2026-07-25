# Fullscreen chain runtime consumer (scene post-processing)

**Status: design.** The shader editor's biggest remaining gap vs UE/Unity: our
materials drive the editor preview and scene sprites, but nothing in the GAME
can run a fullscreen material chain. This arc makes an authored `.arcmat` pass
DAG the scene's post-processing stack — assigned in the editor, serialized in
the scene, hot-reloading through the existing cache/watcher machinery, and
rendered through the ONE canonical canvas→tonemap path (homogenized-rendering
mandate: no bespoke chain).

## The model (UE analogue)

UE post-process materials sample `PostProcessInput0` (scene color) and are
chained by the PP stack outside the material. We do it better in one place —
the material IS the stack (our pass DAG) — and match UE where it counts: the
scene color enters as an input texture, the result feeds the tonemapper, and
the whole thing lives between the linear canvas and TonemapPass.

```
Batcher2D -> Canvas (RGBA16F, linear)
                |          (no chain: tonemap reads the canvas — today's path,
                v           byte-identical)
        FullscreenMaterialChain     scene color = the "Scene" input
                |                   (each pass's own RGBA16F intermediates,
                v                    already built — Slice 4's runner)
        post buffer (RGBA16F, linear)
                |
                v
        TonemapPass -> display-referred output
```

## 1. Scene input: the `Scene` source on the pass canvas

New contract, engine-side (`MaterialSource` + `FullscreenMaterialChain`):

- `inputs` entries gain a sentinel `kSceneInput = 0xFFFFFFFFu` — "the external
  scene color", not a chain index. Serialized as-is in `.arcmat` (`"inputs":
  [4294967295]` is ugly but exact; the loader clamps unknown large values to
  the sentinel).
- `BuildMaterialChainSource` gains `bool externalInput` (post mode):
  - sentinel entries are VALID (any pass, including pass 0 — the "pass 0
    takes no inputs" rule becomes "pass 0 takes only the scene input");
  - the sentinel occupies an InputTexture slot like any other input (it
    counts toward `kMaxPassInputs`), so codegen/`PassInput` need ZERO changes
    — a pass graph sees it as a wired slot.
  - `externalInput == false` (sprite preview, plain fullscreen use): sentinel
    entries are chain errors ("this material reads the scene — assign it as
    the scene post chain"). The EDITOR always builds in post mode so authoring
    and preview keep working (see §4).
- `FullscreenMaterialChain::Render` gains `nvrhi::ITexture* externalInput`
  (default null): bound wherever a sentinel slot appears; null falls back to
  the existing 1x1 black.
- Editor pass canvas: a fixed **Scene** node (id sibling of `kPassOutputNodeId`,
  output pin only, undeletable, only drawn for fullscreen base materials).
  Wiring it into a pass writes the sentinel. Its thumbnail shows the stand-in
  (§4).

Alternative considered and rejected: implicit "pass 0 = scene" (closest to
UE's PostProcessInput0). Rejected because our pass 0 is a real authored pass,
and implicit rebinding would make the same asset mean different things in
different contexts; the explicit Scene wire keeps WIRES ARE THE DATA true.

## 2. The hook: canvas → chain → tonemap

Two call sites drive canvas→tonemap today; the hook lands in both, same
three lines (INTERIM for Loom per the folds-into-Arcane directive):

- **`OffscreenCanvas`** (editor viewport, Play mode, doc previews): grows
  `SetPostChain(FullscreenMaterialChain* chain, const MaterialInstance* inst)`
  (null = off, today's path byte-identical) and an owned RGBA16F post buffer
  sized with the canvas. In `Draw()`, after the batcher pass closes the canvas:
  chain->Render(cl, postFb, *inst, globals, assets, viewIndex=last,
  externalInput=canvasTexture); tonemap samples the post buffer instead of the
  canvas. `GlobalParams` come from the existing `SetGlobals` seam.
- **Loom host** (`Canvas` + `TonemapPass` driven directly): the same sequence
  inline at its present site.

No new render architecture: the chain runner, per-pass intermediates, and
atomic last-good already exist. The hook is plumbing.

## 3. Assignment: the `PostProcess` scene component

```cpp
struct PostProcess { Guid material{}; };   // + ASTRA_REFLECT
```

- Editor Inspector: works for FREE — Guid fields classify as AssetRef and the
  "material" field name infers the Material kind (Slice 8 picker + tonight's
  search box).
- Scene save/load: reflection→JSON, free.
- Host sweep (EditorApp today, the .arcproj runtime host later): each frame,
  first entity with a valid `PostProcess.material` wins (0 or >1 = no chain /
  first-found, warn once on >1). The game can flip it at runtime by editing
  the component — no new EngineContext surface, the plugin already holds
  `Runtime*` → registry.
- **ABI note:** a NEW header-only component appends a type; it changes no
  existing layout, but the rule says verify — rebuild Aphelyon.dll against the
  new SDK and bump `kGamePluginABIVersion` if the tripwire test or soak says
  so. Budget the bump; celebrate if it's free.

## 4. `PostChainCache` + editor preview

- `PostChainCache`, `SpriteMaterialCache`'s twin (same Services, same
  Request/Invalidate/ConsumeResult/Clear shape): loads the SAVED asset (never
  a working copy), resolves instance parent chains, `BuildMaterialChainSource`
  in post mode, compiles every pass's stages through the shared compiler, and
  binds via atomic `SetChain` — failed recompiles keep last-good. Exposes
  `Chain()`/`Instance()` for the hook.
- Invalidation rides everything that exists: doc Save → `onAssetSaved`;
  external edits → tonight's `PollMaterialWatch` (add the post cache beside
  `m_spriteMaterials->Invalidate`).
- **Editor doc preview of a Scene-wired material:** builds in post mode and
  binds a STAND-IN as the external input (a bundled checkerboard/test-pattern
  texture — deterministic, no scene dependency). Same stand-in feeds the
  Scene node's thumbnail. Nicety (later, not this arc): bind the live
  viewport output instead.

## 5. Slices

1. **Scene-input contract** — sentinel + post-mode builder + `Render`
   externalInput + the Scene canvas node + doc-preview stand-in. Tests:
   builder validation both modes, sentinel round-trip, [gpu] chain readback
   with an external input (scene=red, invert pass → cyan).
2. **The hook + cache** — OffscreenCanvas `SetPostChain` + post buffer +
   Loom-host wiring + `PostChainCache` (+ watcher/invalidate wiring). Tests:
   [gpu] OffscreenCanvas with a chain = scene → chain → tonemap readback;
   no-chain path byte-identical to today.
3. **`PostProcess` component + host sweep** — component + reflection + sweep
   + scene round-trip + ABI verify (+ bump if needed). Desk-verify: assign a
   bloom-ish chain to a scene in the editor, watch the viewport; edit the
   material → live update; Play mode; Loom.

Estimated 2–3 sessions. Slice 1 is pure engine + editor polish and can land
independently; the no-chain paths stay byte-identical throughout (the free
tripwire).

## Non-goals (this arc)

- Multiple simultaneous chains / per-camera stacks (one scene, one chain).
- Depth/normal/velocity inputs (2D engine; scene color only).
- Loom-host convergence beyond the inline hook (the .arcproj runtime host is
  the convergence vehicle, per the standing directive).
