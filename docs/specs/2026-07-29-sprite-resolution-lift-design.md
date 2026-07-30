# Sprite/material resolution lift — design

Brief (the WHY, read first): `docs/superpowers/plans/2026-07-29-sprite-resolution-lift.md`.
This file is the HOW, written after reading the source the brief named.

## What the source actually says (facts, verified 2026-07-29)

| Capability | Editor | ArcaneRuntime |
|---|---|---|
| `SpriteTable` published | yes, `EditorAppFrame.cpp:1115` | **never** |
| `SpriteMaterialTable` published | yes, `EditorAppFrame.cpp:1113` | **never** |
| sprite resolution cache | `ArcaneEditor/src/SpriteCache.hpp` (**editor-side**) | none |
| material cache | `Arcane/Render/SpriteMaterialCache` (**already engine-side**) | never constructed |
| post-chain cache | `Arcane/Render/PostChainCache` (**already engine-side**) | never constructed; the tonemap hook exists but `m_postChain`/`m_postInstance` stay null (`RuntimeApp.hpp:37-43`, marked INTERIM: "the .arcproj runtime host is the convergence vehicle") |
| `Batcher2D::SetGlobals` (material Time/Delta/Viewport) | yes, `EditorAppFrame.cpp:892` | **never** — registered materials would see zeros |
| `ShaderCompiler` + `ShaderSourceProvider` | owned by `EditorApp` | never constructed |
| dxcompiler.dll / dxil.dll beside the exe | yes | **yes already** (`premake5.lua:422-423`, ArcaneRuntime's postbuild) — and `shaders/materials` templates too |

So the brief's open questions resolve as:

1. The material table does **not** need a lift — `SpriteMaterialCache` is already `ARCANE_API` engine code. It needs a *driver* in the runtime. Only `SpriteCache` is misplaced.
2. Mounts are fine: the runtime opens the project (`RuntimeApp::Init`), so `Project::ResolveAsset` works there exactly as in the editor.
3. Ordering: one call, before the frame's `Batcher2D::Begin` — that is both *before* `SubmitRender` and *outside* Begin/End (the drain registers materials with the batcher, and the editor already does that outside a Begin/End).

## Design

Two engine units. The caches stay caches; a new host service owns the per-frame choreography that both hosts were supposed to share.

### 1. `Arcane/Render/SpriteCache.{hpp,cpp}` — lifted, not copied

`Arcane::Editor::SpriteCache` → `Arcane::SpriteCache`, `ARCANE_API`, pimpl (matching its
two neighbours `SpriteMaterialCache` / `PostChainCache` exactly, so the family reads as
one family and the header needs no 4251 pragma). The editor copy is **deleted**.

Semantics preserved verbatim — they are the point of the move:
- `Request` is once-per-Guid; `m_table.contains(id)` is the never-re-parse guard.
- A failed resolve caches a **default** `SpriteEntry` in the *published* table (a broken
  sprite must stay VISIBLE as the 1x1 m placeholder), deliberately unlike
  `SpriteMaterialCache`, which keeps failures out of its table.
- `Invalidate` evicts `Batcher2D::RemoveTexture` **before** dropping the keep-alive handle
  (`Batcher2D.hpp:181-191`: stale binding-set entry pins the texture / ABA on reuse).
- `Clear` does the same eviction for every live entry, then drops both maps.

One shape change: `Services::resolveAsset` becomes `Guid`-shaped (`ResolveAssetFn`, the
twins' signature) instead of `AssetId`-shaped, so the three caches take one identical
service. `AssetId::FromGuid` moves inside.

### 2. `Arcane/Host/SceneRenderResolver.{hpp,cpp}` — the shared choreography

`class ARCANE_API SceneRenderResolver` (pimpl), in `Arcane/Host/` beside `GpuContext` /
`ProjectBoot` — it is host-boot layer, not renderer internals. It owns all three caches:
sprite, material, post. Owning all three is *less* code than owning two, because the
compile drain site is shared — leaving post out would mean the editor kept a private
drain for it.

```
Services { Runtime* runtime; Batcher2D* batcher; nvrhi::IDevice* device;
           GraphicsBackend backend; ShaderCompiler* compiler;      // null => sprites only
           ShaderSourceProvider* sources;
           std::function<bool(const ShaderCompileResult&)> consumeFirst; }
FrameInfo { double now, dt; float viewportWidth, viewportHeight; }

void Refresh(const FrameInfo&);   // the one per-frame call, outside Begin/End
const GlobalParams&      Globals()      const;   // host feeds Batcher2D::SetGlobals
FullscreenMaterialChain* PostChain()    const;   // null = no chain (today's path)
const MaterialInstance*  PostInstance() const;
void InvalidateSprite(const Guid&);              // evict + synchronous re-Request
void InvalidateMaterial(const Guid&);            // material AND post caches
void Clear();                                    // project switch
```

`Refresh` order, and why:
1. Build `GlobalParams` from the frame (time/dt/viewport).
2. ONE `CreateView<SpriteRenderer>` walk requesting **both** Guids — `sprite` always
   (synchronous, no compiler needed) and `material` only when the compiler is available.
   This is the editor's F1 fix from the sprite arc; it must survive the move.
3. `CreateView<PostProcess>` walk: first valid material wins, warn-once above one.
4. Compile pump (compiler only): `Poll(now)`, then each drained result is offered
   `consumeFirst` → material cache → post cache. `consumeFirst` is the editor's open
   shader documents; it exists so there is still exactly ONE drain site in the process.
5. Latch `PostChain()`/`PostInstance()` **after** the drain (fresher than the editor's
   current pre-drain latch, same frame).
6. Publish `SetSpriteTable` + `SetSpriteMaterials`.

Lifetime contract, stated in the header: the registry holds **non-owning** pointers to the
resolver's maps, so `~SceneRenderResolver` publishes nulls through the Runtime — which
means every host MUST declare its resolver so it destructs BEFORE the Runtime and before
the render device (it holds nvrhi keep-alive handles).

### 3. Editor migration (behaviour-preserving)

`m_sprites` + `m_spriteMaterials` + `m_postChains` → one `m_resolver`.
- `EditorApp.cpp`: one construction site; `invalidateSprite` → `InvalidateSprite`.
- `EditorAppProject.cpp`: asset-saved / watch-fired `Invalidate` → `InvalidateMaterial`;
  project-switch `Clear` → one `Clear`.
- `EditorAppFrame.cpp`: the phase-9 post sweep becomes `PostChain()`/`PostInstance()`
  reads; `PumpShaderEditor`'s sweep+pump+publish becomes `Refresh`, with the shader
  documents routed through `consumeFirst`. `PollMaterialWatch` + `TickAll` stay.
- One deliberate behaviour change: `Refresh` moves ahead of the scene render (it must be,
  for the runtime), so the tables a frame publishes are consumed by THAT frame's submit
  instead of the next. Strictly better; the old order cost one frame of placeholder.

### 4. ArcaneRuntime wiring

`RuntimeApp` gains `ShaderCompiler` + `ShaderSourceProvider` (`AddRoot("shaders")`,
`Initialize(0.2)`, missing DXC = warn and sprites still resolve) and the resolver,
declared last so it destructs first. `MainLoop` calls `Refresh` before `Batch().Begin`,
feeds `Batch().SetGlobals(Globals())`, and takes the post hook from the resolver.

## Verification

`Tests/src/SceneRenderResolverTest.cpp`, two groups in one file (6 cases, 51 assertions,
all CPU-only so they run in the `~[gpu]` gate):

- **SpriteCache semantics**, pinned because they MOVED and nothing tested them before:
  resolve-once (a 5x re-Request costs one filesystem hit), a failed resolve memoised as
  the *visible* 1x1 placeholder, `Invalidate` forcing a re-read, `Clear` emptying.
- **The resolver publishes**: `Refresh` puts a `SpriteTable` in the registry that resolves
  the scene's sprite Guid, with no compiler/device/batcher at all; a sprite added after
  the first `Refresh` is picked up; `~SceneRenderResolver` leaves nulls behind.
  Asserted through the registry RESOURCE the submission path reads, not the cache's own
  `Table()` — "the cache resolved it" was never the broken half.
- Registry-touching tests use `Arcane::Test::SharedTypeContext()`, never a bare `Runtime`.
- **Red-check performed**: with the two publish lines commented out, exactly those 3
  resolver cases fail (on `table != nullptr`) and the 3 cache cases still pass. The first
  red run also exposed a chained-deref in one test that CRASHED the suite instead of
  failing; fixed to two steps.

Not covered by automated tests, and honestly so:

- `RuntimeApp`'s call site (an exe TU, not in ArcaneTests) — desk-verify only.
- Anything on a GPU: material compile/bind, the post chain, actual pixels.
- Real acceptance is the user's: a textured sprite visible in a separate-window play,
  identical to the editor viewport. Desk-only (GPU hazard on this box).

## Tasks

- T1 — lift `SpriteCache` into `Arcane.dll`; editor consumes it; delete the editor copy.
- T2 — `SceneRenderResolver` + editor migration onto it.
- T3 — ArcaneRuntime wiring (compiler, sources, resolver, globals, post hook).
- T4 — tests, gate, docs/memory; SPEC-BULLET WALK over this file (the process gap the
  last arc's final review named: reviewers read the plan and never the spec).
