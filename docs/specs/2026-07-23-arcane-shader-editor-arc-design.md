# Arcane Shader/Material Editor — Arc Design

**Date:** 2026-07-23
**Status:** Approved (user), pre-implementation
**Scope:** The full arc from "no material system" to a complete shader editor in the
Arcane Editor: material/parameter core, runtime HLSL compilation, live preview,
text-snippet authoring, GUID asset identity, a real asset browser, material instances,
in-scene sprite materials, and a node-graph front-end. 9 slices.
**Driver:** a UI/UX designer wants to author HLSL in Arcane's ecosystem and get live
visual results — while the engine gains the material system it needs anyway.

---

## 1. Goal & end state

At the end of the arc the Arcane Editor contains a **full shader editor**:

- Designers author a shader as a **material**: an HLSL *snippet* (the pixel-shading body)
  plus declared parameters. The engine owns all scaffolding (entry points, vertex stage,
  backend `#if SPIRV` blocks, register conventions, the generated cbuffer).
- Authoring has **two front-ends over one artifact**: a text editor and a node graph.
  Both emit the same snippet into the same engine template.
- **Live preview** renders the real engine path (`OffscreenCanvas` → linear canvas →
  tonemap) with sub-second recompiles; the last-good shader stays bound while a compile
  is in flight or failing.
- Parameters are declared once and flow to three consumers automatically: the generated
  cbuffer, the GPU pack loop, and the editor's parameter panel (slider writes update the
  constant buffer live — **no recompile**).
- Compile errors surface as structured diagnostics with source-gutter markers and
  click-to-jump.
- Materials are **unified**: the same material model drives fullscreen effects and
  per-sprite/in-scene shading; the preview surface is switchable (fullscreen ⇄ sprite).
- Materials and instances are **GUID assets** (AssetRegistry); textures inside materials
  are referenced **by GUID, not path**. The editor has a real **asset browser** with
  asset-type → editor routing, and the shader editor is the first **EditorDocument**
  (open/dirty/save lifecycle, CommandStack undo).

## 2. Decisions (locked with the user, 2026-07-23)

1. **End state = text editor + node graph** (both, not text-only). Graph lands last.
2. **Unified material** — fullscreen AND per-sprite, switchable preview surface.
3. **Authoring model = engine-owned template + designer snippet** (the AAA model:
   UE translator→template, Unity Surface Shaders/Shader Graph, Godot `fragment()`).
   No raw-full-HLSL bypass in this arc (purely additive later if ever needed).
   Built-in engine shaders (`sprite.hlsl`, `tonemap.hlsl`, …) stay full hand-authored
   files — programmer tier, outside the material system.
4. **Three folds to avoid interim code** (user: "megaplan"):
   - GUID asset resolution through the `Assets` facade (materials + texture params are
     GUID-clean from day one; kills path-string refs).
   - A **real asset browser** (all asset types + editor routing), not a materials-only
     stub list.
   - The shader editor is born as an **EditorDocument** with CommandStack undo + native
     file dialogs — not a singleton panel to re-plumb later.
5. **Process = RELAXED** (same as the project-format arc): build from this spec's
   per-slice designs, keep headless gates green, commit between slices, windowed
   desk-verify is the user's. No TDD-first ceremony; add focused tests per slice.

## 3. Current state (verified 2026-07-23, post project-format arc @496d7e4e)

Shader pipeline today:
- `ShaderLibrary` (`Arcane/Arcane/src/Arcane/Render/ShaderLibrary.{hpp,cpp}`): loads
  **precompiled** loose `.bin` per backend (`<dir>/{dxil|spirv}/<name>.bin`), keyed by
  string stem (`Get(name, stage)`); entry point derived from stem suffix
  (`_vs→vs_main`, `_ps→ps_main`, `_cs→cs_main`); mtime `Poll()` hot-reload with
  torn-read guard; monotonic `Generation()` that every pipeline cache compares
  (Batcher2D.cpp:396-400, TonemapPass.cpp:80-84). Final call:
  `device->createShader(desc, bytes, size)` (ShaderLibrary.cpp:149-150). **No source
  compile in-engine; no GUID/asset identity for shaders.**
- Offline compile: `Arcane/shaders/compile-shaders.bat` — vendored
  `ThirdParty/tools/dxc/dxc.exe` (with **`dxcompiler.dll` + `dxil.dll` beside it —
  all three vendored**), fixed enumerated shader list, dual-target per entry point.
  SPIR-V shifts (bat:21): `-fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0
  -fvk-u-shift 384 0` — must match `nvrhi::VulkanBindingOffsets` defaults
  (t=0/s=128/b=256/u=384; DeviceVulkan does not override). Every HLSL dual-compiles
  via `#if SPIRV` push-constant/cbuffer blocks (sprite.hlsl:12-21 pattern).
- `Batcher2D` (the canonical 2D path): **fixed 16-byte push constants**
  (`{invHalfViewport, pad}`, Batcher2D.cpp:24-28), hardcoded 3-item binding layout
  (PushConstants(0) + Texture_SRV(0) + Sampler(0), :78-84), pipeline chosen by the
  hardcoded `BatchKind{Sprite,Circle,Text}` enum switch (:410-421), sort key
  `layer|order|kind|textureSlot` (:323-326), blend/raster/depth shared + hardcoded.
  **No per-draw material, no general uniform abstraction.**
- `TonemapPass` (TonemapPass.cpp): the fullscreen-pass shape to copy — SRV+Sampler
  pixel-only layout, `SV_VertexID` fullscreen triangle (no VB), FramebufferInfo-hash
  pipeline cache, Generation invalidation.
- `OffscreenCanvas` (OffscreenCanvas.{hpp,cpp}): the **reusable preview mechanism** —
  owns linear RGBA16F Canvas + Batcher2D + TonemapPass + BGRA8 display-referred output
  texture + own command list. `Draw(FunctionRef<void(Batcher2D&)>, clear)` runs the
  whole path; `TextureId()` = raw `nvrhi::ITexture*` as ImTextureID (the ImGui-NVRHI
  backend binds any texture pointer, ImGuiNvrhi.cpp:247/273-287); `OutputFramebuffer()`
  for overlay passes; `Resize()`. Gap: `Draw` only exposes the Batcher2D — needs one
  overload taking a raw pass functor.
- Assets/identity (from the project-format arc): `Assets` facade
  (GetTexture=sRGB / GetBytes / GetJson / LoadDisplayTexture=UNORM, SetContentRoot);
  `AssetId` is GUID-backed; `AssetRegistry` maps Guid → mount path (native JSON =
  embedded top-level `"id"`; imported binaries = `.meta` sidecar). **Shaders and the
  facade are not yet connected to the registry** — Assets.hpp:60-61 declares the
  destination ("resolves behind the AssetId seam").
- Editor: panels are per-frame free functions in a dockspace
  (EditorApp.cpp MainLoop + EditorPanels.cpp; add a panel = one function + one call +
  one `DockBuilderDockWindow` line). The **Assets panel is a stub** ("coming soon").
  `Arcane::Edit::CommandStack` undo exists and is wired (Inspector/gizmo).
  `Window::ShowOpenFolderDialog` exists (SDL3 async dialog pattern to mirror for
  file open/save). `EnkiTaskExecutor`/`ITaskExecutor` = the worker-thread seam.
  imgui + **imgui-node-editor already vendored**.
- Constraints: **homogenized rendering** (one canonical path — no parallel bespoke
  chains); **/MD cross-DLL** (all NVRHI objects + compiles live in Arcane.dll; the
  editor exe passes plain data: source strings in, handles/diag lists out).

## 4. Reference findings (UE5 source at `Arcane/.example/UnrealEngine-release`)

Full agent reports were produced 2026-07-23; key citations preserved here.

**Material/param system (adopt, scaled down):**
- Tagged-union param value: `FMaterialParameterValue` = type enum + union
  (MaterialTypes.h:338/379-394). One value type carries any parameter.
- Template vs instance: `UMaterial` (declarations + defaults) vs `UMaterialInstance`
  (Parent pointer + **sparse** override arrays, MaterialInstance.h:578/599/702).
- Compiled param **table separate from values**: `FUniformExpressionSet` — ordered
  `{paramInfo, type, defaultValueOffset}` + defaults blob + CB layout
  (MaterialShared.h:640/741/748/756, :486).
- One resolve seam: `FMaterialRenderProxy::GetParameterValue` — override map → parent
  chain → compiled default (MaterialRenderProxy.h:170; chain MaterialInstance.cpp:337-359).
- Pack loop: resolve-then-memcpy per layout slot into a byte buffer → one uniform
  buffer (MaterialUniformExpressions.cpp:1005-1041). Textures bound as resources via a
  **parallel table**, never packed in the CB.
- Named-slot HLSL template: `FStringTemplate` `%{slot}` substitution
  (StringTemplate.h:34/119; MaterialSourceTemplate.h:12).
- Global params: `UMaterialParameterCollection` — one shared CB of engine globals
  (MaterialParameterCollection.h:78).
- One-off override proxy (`FColoredMaterialRenderProxy`, MaterialRenderProxy.h:296) —
  the "hit-flash tint without a full instance" pattern.
- **Skip as UE-scale overkill:** the 10k-line graph translator/IR (keep only the
  template stitch), preshader bytecode VM, static-switch permutations, material
  layers, FMemoryImage freezing, GT/RT param double-buffering, LWC doubles, VT/profiles.

**Editor UX (adopt):**
- 4-panel anatomy: preview viewport / authoring surface / params / stats+errors
  (layout MaterialEditorModes.cpp:20-84). UE is graph-first with HLSL as read-only
  byproduct; **Arcane inverts: HLSL-first, graph later**.
- **Edit-buffer vs Apply split**: edits hit a working copy; Apply/Save commits to the
  asset with a pre-save error guard (UpdateOriginalMaterial MaterialEditor.cpp:2767-2868).
- **Live param edits skip recompile** — numeric changes push straight to the render
  proxy (SetNumericParameterDefaultOnDependentMaterials :5638). Structural edits
  recompile (debounced live loop behind a toggle, :3872).
- Errors surface **on the source** (list + gutter/badges + click-to-jump;
  UpdateMaterialInfoList :3428-3520).
- Instance editor = separate params-only mode: per-param override checkbox +
  reset-to-default, "show only overridden" filter
  (MaterialInstanceEditor.h:31-292; MaterialEditorInstanceDetailCustomization.cpp).
- 2D/UI preview precedent: `SMaterialEditorUIPreviewViewport` (checkerboard bg, zoom).

**Compile pipeline (adopt):**
- Time-sliced main-thread result pump; workers never touch engine state
  (ProcessAsyncResults ShaderCompiler.cpp:2723/2730/2799).
- **In-process DXC**: `IDxcCompiler3::Compile(sourceBlob, argv, …)` — the argv IS the
  CLI arg list (D3DShaderCompilerDXC.cpp:335-346/518-524); results via
  GetStatus/GetOutput(DXC_OUT_OBJECT)/GetErrorBuffer — **read the error buffer even on
  success** (warnings, :712). SEH-guard the Compile call (:502-532) — treat the
  compiler as untrusted.
- Structured errors: `FShaderCompilerError{file, line,col, message, sourceLine, caret}`
  (ShaderCompilerCore.h:406-480); parse once at the boundary with the **Clang grammar**
  `path:line:col: {error|warning}: msg` (DXC's native form, ShaderCore.cpp:3749-3779);
  panels render records, never re-parse text.
- Live update = **invalidate-and-repull** via a generation counter (exactly Arcane's
  `Generation()` model), never mutate in-flight GPU state
  (PropagateMaterialChanges → MarkRenderStateDirty :2308/2330).
- Content-addressed cache keyed `hash(source) ⊕ hash(dxcompiler.dll+dxil.dll bytes) ⊕
  hash(args)` — the DLL-bytes trick auto-invalidates on toolchain bump
  (DXCWrapper.cpp:90-111; note dxil.dll presence = signed output, :105-108).
- DXIL and SPIR-V are **two independent compiles of one source** — report per-target;
  publish per-backend only on that backend's success.

## 5. Architecture

### 5.1 Material core (engine, `Arcane/Arcane/src/Arcane/Material/`)

```cpp
enum class MatParamType : uint8_t { Float, Float2, Float4, Color, Texture };
struct MatParamValue { MatParamType type; union { float f[4]; /*texture=*/Guid tex; }; };
struct ParamDecl     { std::string name; uint32_t nameHash; MatParamType type;
                       MatParamValue def; uint32_t cbOffset;      // 16B-packing rules
                       /* editor-only metadata alongside, not inside: */ };
struct ParamMeta     { std::string group, tooltip; float sliderMin, sliderMax; };
class MaterialTemplate {   // the compiled "param table" — layout, never values
    // shader identity (template kind + snippet hash), ordered ParamDecls,
    // defaults blob, cbSize; BuildLayout() assigns cbOffsets (float4 alignment,
    // no 16B straddle); immutable after build.
};
class MaterialInstance {   // values only — sparse overrides + parent
    // parent (template or instance), vector<pair<nameHash, MatParamValue>> overrides;
    // bool GetParam(nameHash, type, out) — my override → parent → template default;
    // PackCB(uint8* dst) — the resolve-then-memcpy loop; dirty serial for re-pack.
};
class GlobalParams { /* Time, DeltaTime, ViewportSize — one shared volatile CB,
                        updated once per frame, bound at a fixed slot (b1) */ };
```

Textures resolve through a **parallel texture table** (Guid → `Assets` facade →
`nvrhi::TextureHandle`) into the binding set; never packed into the CB. Editor metadata
(`ParamMeta`) lives beside, not inside, the runtime decl (UE's
`FMaterialParameterMetadata` split). All of this is plain CPU code — fully unit-testable
headless.

### 5.2 Runtime compile service (engine, `Arcane/Arcane/src/Arcane/Render/ShaderCompiler.{hpp,cpp}`)

- **In-process `dxcompiler.dll`** via `DxcCreateInstance`/`IDxcCompiler3`; explicit
  LoadLibrary of the vendored copies (verify `dxil.dll` found — signing). No new deps.
- API shape: `Submit(CompileRequest{ sourceUtf8, entry, profile, targets, defines })`
  → worker (enkiTS / ITaskExecutor) runs **two Compile() calls** (DXIL; SPIR-V with the
  shift flags) from the same in-memory blob → result struct
  `{ perTarget: {bytes|empty, vector<ShaderDiag>}, contentHash }` onto a mutex queue.
- `Drain()` on the main thread (called from the editor tick / ShaderLibrary::Poll
  site): only there do `device->createShader` + cache swap + `Generation()` bump occur.
  NVRHI is not free-threaded; workers never touch it.
- `struct ShaderDiag { std::string file; int line, col; enum Sev; std::string msg,
  sourceLine, caret; std::string reproCmdLine; }` — parsed once with the Clang grammar;
  capture the two follow lines for sourceLine/caret; always parse warnings.
- **The shift-flag list + entry/profile/stem conventions live in ONE shared constant**
  (used by ShaderCompiler and referenced as the source of truth vs compile-shaders.bat)
  so they can never drift from `VulkanBindingOffsets`.
- Debounce ~200ms quiet-window + coalesce by (material, target); superseded jobs
  dropped. SEH `__try/__except` around Compile → failed-job, never unwind.
- Two-tier cache: in-memory map keyed by content hash (source ⊕ dxc DLL bytes ⊕ args);
  optional on-disk `.bin` beside `generated/` later. Keep last-good blob + last diags.
- `compile-shaders.bat` stays for build-time AOT of built-in engine shaders.

### 5.3 Authoring model — template + snippet + declared params

Engine-owned templates in `Arcane/shaders/materials/` (start with
`fullscreen_material.hlsl`, add `sprite_material.hlsl` in Slice 8):

```hlsl
// fullscreen_material.hlsl (ENGINE-OWNED skeleton, abridged)
%{MATERIAL_CBUFFER}          // generated from ParamDecls: cbuffer Material : register(b0)
// + GlobalParams cbuffer at b1 (Time, ViewportSize); #if SPIRV blocks; t0/s0 texture
struct Varyings { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Varyings vs_main(uint id : SV_VertexID) { /* fullscreen triangle */ }
%{MATERIAL_BODY}             // designer snippet: defines  float4 shade(Varyings v)
float4 ps_main(Varyings v) : SV_Target { return shade(v); }
```

The designer authors a **material source** = param declarations + the snippet body:

```hlsl
//@param color Tint     = (1, 1, 1, 1)
//@param float Speed    = 1.0   [0..4]
//@param texture Noise                      // -> t-slot + Guid ref

float4 shade(Varyings v) {
    float w = sin(v.uv.x * 10 + Time * Speed);
    return Tint * w;
}
```

`//@param` lines are parsed into `ParamDecl`s (one source → cbuffer + pack + UI; final
grammar settled in Slice 4). The stitcher is a trivial `%{slot}` string substitution
(UE's StringTemplate, minus everything else). The resolved source is what the compile
service sees — and its hash keys the cache. The **node graph emits the same snippet**
(Slice 9), which is why this seam exists.

### 5.4 Preview path

`FullscreenMaterialPass` modeled on TonemapPass: binding layout = volatile CB(b0,
material) + CB(b1, globals) + declared texture SRVs + sampler; FramebufferInfo-hash
pipeline cache; Generation-based invalidation; renders into the OffscreenCanvas's
**linear** canvas, then the standard tonemap runs — the preview shows the true engine
output. One new `OffscreenCanvas::Draw` overload takes
`FunctionRef<void(nvrhi::ICommandList*, nvrhi::IFramebuffer* canvasFb)>` so the pass
draws without exposing canvas internals. **On compile failure or in-flight compile,
the previous pipeline keeps rendering** (never blank the preview); the error panel
lights up instead.

### 5.5 Asset model & identity (Fold 1)

- **`.armat`** = a material asset: native JSON with embedded top-level `"id"` (rides
  `AssetRegistry::ScanContent` unchanged). Contains: template kind
  (fullscreen/sprite), the snippet text, param declarations + defaults, texture refs
  as **Guids**, and later the node graph. (Snippet inline vs sidecar `.hlsl`: start
  inline — one file = one atomic asset; revisit if diff pain. Open question §8.)
- Instance asset: same file shape with `"parent": <guid>` + sparse overrides only.
- **`Assets` facade learns GUID resolution**: `GetTexture(const AssetId&)` etc. resolve
  `AssetId → Project's AssetRegistry → mount → path → existing loader`. Path overloads
  remain as the legacy fallback (exactly the destination Assets.hpp:60-61 declares).
- `ShaderSourceProvider` seam: logical shader/template name → source text (today:
  `Arcane/shaders/` files; later: Guid-addressed). Both ShaderLibrary's built-in loads
  and the compile service resolve through it.

### 5.6 Editor shell — documents, browser, routing (Folds 2 + 3)

- **EditorDocument** (thin): `{ assetGuid, title, dirty, Save(), Draw() }` + a document
  list on EditorApp; unsaved-close guard; save-all. The shader editor is the first
  implementation. Deliberately NOT a docking framework.
- **Asset-editor routing registry**: asset type/extension → open-document factory.
  Double-click `.armat` in the browser → shader editor document.
- **Asset Browser panel** (replaces the stub): lists ALL AssetRegistry entries
  (type filter, search, name/path); type icons first, thumbnails later (material
  thumbnails fall out of the preview pass — render once to a small texture, cache).
  Drag-source: dragging a texture asset onto a texture param slot sets the Guid;
  the params panel also gets a picker popup backed by the same query.
- **File dialogs**: add `Window::ShowSaveFileDialog/ShowOpenFileDialog` beside the
  existing folder-picker (same SDL3 async trampoline pattern).
- **Undo**: param edits + (later) graph edits are CommandStack commands (one undo
  system); text-widget undo stays internal to the text editor widget.
- Shader editor = 4 panels inside its document: snippet text editor
  (`InputTextMultiline` + line-number gutter first; colorizing widget = Slice-9-era
  polish, open question §8), live preview (target selector, checkerboard/solid bg),
  params (auto-generated widgets: Color→ColorEdit4, Float→SliderFloat with
  `[min..max]`, Texture→picker; live CB writes, no recompile), errors (ShaderDiag
  list + gutter markers + click-to-jump). Toolbar: Save (error-guarded), Live toggle,
  manual Compile, preview-surface combo.

### 5.7 In-scene sprite materials (the deep engine move, Slice 8)

Generalize Batcher2D:
- `BatchKind` enum switch → a **MaterialId → { vs/ps shader handles, blend/raster
  state, MaterialTemplate*, instance }** table; built-ins (sprite/circle/msdf) become
  entries 0..2 so the existing path is the degenerate case.
- `DrawRecord` + sort key gain the material id (widen the `kind` bits); binding layout
  gains the optional per-material volatile CB; binding-set cache keys on
  (texture, material) instead of texture alone.
- `sprite_material.hlsl` template: standard sprite vertex stage; snippet sees
  `Varyings { pos, uv, color }` + the sprite texture.
- `SpriteRenderer` gains an optional `MaterialRef` (Guid); the render-submission path
  resolves it to a MaterialId. Byte-identical rendering for sprites without one.
- Preview surface switcher completes: fullscreen ⇄ sprite-on-checkerboard, same
  material.

### 5.8 Node graph (Slice 9)

- imgui-node-editor canvas as a second view inside the shader-editor document.
- Node set (deliberately small): Const(float/float2/float4/color), Param(decl ref),
  TextureSample, UV, Time, Add, Mul, Lerp, Sin, OneMinus, Output(float4).
- Codegen: topological walk → straight-line HLSL statements → wrapped as
  `float4 shade(Varyings v)` → **the same `%{MATERIAL_BODY}` slot** → same compile /
  preview / params loop. Graph serialized into the `.armat` JSON.
- A material is graph-owned or text-owned: graph-owned shows generated text
  read-only; one-way "convert to text" unlocks freeform editing (severs the graph).
  No text→graph decompilation (nobody ships that).
- Param nodes create/reference the same ParamDecls — the params panel is identical in
  both modes.

## 6. The arc — 9 slices

| # | Slice | Contents | Gate |
|---|-------|----------|------|
| 1 | **Material core** | ParamValue/ParamDecl/MaterialTemplate/MaterialInstance, cbuffer layout rules, pack loop, GlobalParams; unit tests | new `[material]` suite green; `~[gpu]` unchanged |
| 2 | **Compile service** | in-proc IDxcCompiler3 dual-target, ShaderDiag parsing, worker+drain, debounce/coalesce, SEH, content-hash cache | `[shadercompile]`: compile tiny HLSL → bytecode both targets; bad HLSL → parsed diags (line/col); warning surfaced |
| 3 | **GUID asset wiring** *(small)* | Assets facade resolves AssetId→Registry→loader; ShaderSourceProvider; path stays fallback | `[assets]`/`[project]` extended: texture-by-Guid round-trip |
| 4 | **Template + first pixels** | `%{slot}` stitcher, `//@param` parser, fullscreen_material.hlsl, FullscreenMaterialPass, OffscreenCanvas overload, bare preview panel w/ live Time | headless GPU readback test of a known material; desk-verify animating panel |
| 5 | **Shader Editor MVP** | EditorDocument + routing skeleton + file dialogs (Fold 3); 4-panel editor; debounced live loop, last-good-bound; params live-CB; errors+gutter; Save/Load `.armat` (GUID asset) | `[editor]` units for doc lifecycle + param widget mapping; desk-verify the full edit loop |
| 6 | **Asset Browser** | real browser over AssetRegistry (filter/search/icons), asset→editor routing live, texture picker + drag to param slot | `[editor]` browser/query units; desk-verify open-from-browser |
| 7 | **Instances** *(small)* | `.armat` instance assets (parent Guid + sparse overrides), instance document mode (params-only, override checkbox + reset, source hidden) | `[material]` override-chain asset round-trip; desk-verify |
| 8 | **Sprite materials** | Batcher2D MaterialId generalization, sprite_material template, SpriteRenderer MaterialRef, surface switcher | existing `[gpu]` suites byte-stable for materialless sprites; new sprite-material readback test |
| 9 | **Node graph** | node canvas, node set, graph→snippet codegen, graph in `.armat`, owned-mode rules | codegen unit tests (graph → expected snippet → compiles); desk-verify authoring loop |

Ordering rationale: 1+2 independent foundations; 3 tiny and unblocks GUID-clean
Slice 4+; 4 = first pixels; 5 = designer becomes productive; 6 kills the stub browser
before instances need a parent-picker; 7 rides 5+6; 8 is the invasive engine change,
after the param model is battle-tested; 9 last — the graph targets a proven seam,
so it's built once.

## 7. Non-goals / deferred (beyond this arc)

- Raw-full-HLSL escape hatch & custom vertex stages (additive later; template gains a
  `%{VERTEX_BODY}` slot when needed).
- Multi-pass materials, blend-state authoring UI beyond template presets.
- Preshader/computed-uniform VM; static-switch permutation system; material layers.
- Shader include files for snippets; a stdlib beyond what templates provide.
- Asset rename/move UI with reference fixup; import pipeline beyond existing `.meta`.
- General docking/tab framework for documents; multi-window.
- Node-graph niceties: per-node preview thumbnails, subgraphs/functions, comments.
- Text→graph decompilation. Instruction-count stats (partial DXC-reflection strip is
  optional polish, not a slice).

## 8. Open questions (resolve at slice time, none block Slice 1)

1. `//@param` final grammar (types, ranges, groups, tooltips) — settle in Slice 4;
   keep the parser isolated so syntax changes stay cheap.
2. Snippet storage: inline JSON string (start here) vs sidecar `.hlsl` — revisit at
   Slice 5 if diffs/tooling hurt.
3. Text-editor widget: `InputTextMultiline`+gutter vs vendoring a colorizing editor
   (ImGuiColorTextEdit-class) — decide during Slice 5/9 polish; vendoring is a
   ThirdParty addition (follow the vendoring conventions).
4. `.armat` name/extension bikeshed; and whether instances get a distinct extension
   (`.armi`) or a `"kind"` field (start: same extension, `"parent"` implies instance).
5. Sprite-material varyings surface (exact fields exposed to snippets) — Slice 8.
6. Material thumbnails in the browser: when to add caching (post-Slice 6 polish).

## 9. Process & build facts (for cold-start execution)

- Branch per the team workflow; this arc starts from local `main` @496d7e4e (which
  carries the whole project-format arc; main is ahead of origin — push is the user's
  call).
- Build: premake regen after new .cpp/premake edits
  (`Set-Location D:\dev\starworks\Gacha\Arcane; $env:_APH_NOPAUSE=1; .\GenerateProjects.bat`);
  MSBuild `Arcane.slnx /p:Configuration=Debug /m /clp:ErrorsOnly` (MSBuild 18 at
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`).
- Tests run **from the exe dir** (`Push-Location ...\ArcaneTests`); baseline gates at
  arc start: `~[gpu]` 28031/385, `[project]` 122/28, `[hotreload]` 47/7, `[loom]` 49/13.
- Windowed exes (ArcaneEditor/Loom) = desk-verify only (Parsec GPU hazard).
- New engine code: `/MD`, ASCII comments, UTF-8 no BOM, no `/fp:fast`; compiles +
  NVRHI object creation stay inside Arcane.dll.
- UE reference stays available at `Arcane/.example/UnrealEngine-release` — §4 has the
  file:line map for deeper digs during any slice.
