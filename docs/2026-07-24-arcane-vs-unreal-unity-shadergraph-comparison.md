# Arcane material graph vs Unreal Material Editor vs Unity Shader Graph

Date: 2026-07-24. Arcane state: shader-editor arc slices 1-9 + UE-model conversion
(branch `shader-editor-slice1-material-core` @3e3adc9b). Unity facts sourced from the
17-agent deep dive of `com.unity.shadergraph@17.6.0`
(`2026-07-24-unity-shadergraph-1760-deep-dive.md` -- cited as [DD]). Unreal facts are
UE 5.x-era knowledge (Material Editor / `FHLSLMaterialTranslator`), stated at the
architecture level where they are stable.

Scale honesty up front: Unreal and Unity are decade-mature products with ~150-200
node types, teams, and enormous install bases. Arcane's graph is a week-old MVP with
21 node types built by one engine for one game. The interesting comparison is not
node count -- it is architecture (where we deliberately copied), leverage (where our
constraints let us do things they cannot), and gaps (what maturity buys that we lack).

---

## 1. At a glance

| Dimension | Unreal Material Editor | Unity Shader Graph 17.6 | Arcane (slice 9) |
|---|---|---|---|
| Authoring model | Graph only; HLSL islands via Custom node | Graph only; HLSL islands via Custom Function node | Graph only (UE model, by directive); HLSL islands via Custom node; legacy text tier grandfathered |
| Text -> graph | Never | Never | Never (subset-parser idea shelved with the UE-model decision) |
| Generated-code view | Read-only HLSL window | Read-only "View Generated Shader" | Read-only HLSL toggle in the doc |
| Graph -> shader | `FHLSLMaterialTranslator` -> splice into `MaterialTemplate.ush` per pass/permutation | Description functions -> `$splice` into per-target pass templates [DD 2.4] | Snippet (`//@param` + `shade()`) -> `%{MATERIAL_BODY}` template stitch |
| Node count | ~150+ expressions | ~190-200 [DD 2.2] | 21 (incl. Custom) |
| Node implementation | C++ `UMaterialExpression` subclasses | ~90% data-style `CodeFunctionNode` [DD 2.2] | One static data table + a codegen switch |
| Type system | float1-4 promotion, MaterialAttributes struct pins | Dynamic vectors, min-width rule, exact adaptation table [DD 2.3] | SG's table verbatim (splat / append 0,1 / leading-swizzle truncate), widths 1/2/4 |
| Edge identity | (expression, output index) both ends | (node GUID, slot id) both ends [DD 2.1] | (node id, pin index) both ends |
| Serialization | UAsset binary (UObject) | MultiJson text, sorted, diff-stable [DD 2.1] | JSON in `.armat`, sorted, byte-stable round-trip |
| Determinism | Shader maps keyed by hashes; DDC | Same graph => same HLSL (id-derived names) [DD 2.4] | Same graph => byte-same snippet (SSA `_n<id>`) |
| Params / instances | Master material + Material Instances (Constant/Dynamic), name-bound | Blackboard + Material Variants, reference-name-bound (rename = silent loss) [DD 2.6, 2.10] | `//@param` decls + `.armat` instances w/ sparse overrides; GUID-identified assets; same rename wart (documented) |
| Surfaces / targets | Material Domains (Surface/Deferred Decal/Light Function/Post Process/UI) x shading models x blend modes | Targets (URP/HDRP/BiRP) x SubTargets (Lit/Unlit/Sprite/Fullscreen/Canvas) via master stack [DD 2.7] | 2 surface kinds (fullscreen, sprite); single Output node = one-block stack |
| Passes | Many per material (ShadowCaster, Depth, Forward...) engine-owned | Per-target pass list, engine-owned [DD 2.7] | One pass per surface today; pass-chain design exists for a follow-up slice |
| Vertex stage | Yes (WPO, customized UVs) | Yes (Vertex context + custom interpolators) [DD 2.7] | No (spec non-goal; `%{VERTEX_BODY}` reserved for later) |
| Variants / keywords | Static switches -> permutation explosion (the UE pain) | Boolean/enum keywords -> `shader_feature`/`multi_compile`, 2048-variant cap [DD 2.4] | None, deliberately (deep-dive SKIP) |
| Per-node previews | One node at a time ("Start Previewing Node") on the preview mesh | Thumbnail per node, per-node subtree shaders, async, last-good dimmed [DD 2.9] | None; ONE live preview (fullscreen quad / sprite-on-checkerboard via the real Batcher2D) |
| Error -> node mapping | Translator errors annotate expressions | Only per-node preview shaders map errors; master-shader errors do NOT map [DD 2.9] | DXC diags map to nodes via the codegen line map -- node-accurate on the REAL shader, no preview-shader farm |
| Custom HLSL node | Inputs + output type + body; can hurt optimization folding | CFN String/File mode; cannot see graph properties (pins only) [DD 2.8] | Pins + body as `_cf<id>()`; body CAN read params + Time directly; body lines line-mapped for badges |
| Reuse | Material Functions (+ layers) | Sub Graphs (GUID-named functions, recursion-banned) [DD 2.8] | None yet (far-future per deep dive) |
| Undo | Editor transaction system | Whole-graph JSON snapshot per action -- their #1 complaint [DD 2.5] | Incremental CommandStack; one command per gesture; survives recompiles via doc-anchor |
| Live compile loop | Background compile, "Compiling Shaders" counts, preview swaps when ready | Async preview compiles, "Compiling..." placeholders | 200 ms debounced dual-target compile, last-good stays bound, per-(doc,stage) coalescing |
| Backends from one source | HLSL -> per-RHI via engine shader pipeline | HLSL variants via SRP compiler infra | One source -> DXIL + SPIR-V in one request (in-proc DXC) |
| Extensibility | New expressions = engine/plugin C++ (possible, heavyweight) | Closed (`internal`); CFN + subgraphs only [DD 2.8] | Add a table row + emission case + tests; Custom node for users |
| Canvas UX extras | Comments, reroute, named reroutes, preview-in-node, alignment | Searcher w/ drag-to-canvas auto-connect, groups, sticky notes, redirects, Alt-chords [DD 2.5] | Create menu, silent replace/cycle-refusal, badges, focus-from-error; no searcher/groups/redirects yet |

---

## 2. Architecture: where all three agree

All three systems share the same load-bearing skeleton, which is exactly why the
deep dive validated our slice-9 design before we built it:

1. **The graph never owns the pass.** Graph output is a pure description function
   (`shade()` for us, `SurfaceDescriptionFunction` for SG, the material template's
   inputs for UE); the engine-owned pass file does combine/tint/discard/blend.
   Consequence for us: sprite batching, tonemap, and the instance system were
   untouched by the entire graph arc.
2. **Deterministic, id-derived codegen.** Variable names come from serialized node
   identity in all three, so identical graphs produce identical shaders and caches
   key cleanly. Ours is the strictest (byte-same snippet, golden-tested).
3. **Text lives in an island, not a tier.** The Custom(-Function) node with typed
   pins is the entire designer-facing text story in UE, Unity, and now Arcane.
4. **Instances are sparse name-keyed overrides over a parent.** UE Material
   Instances, Unity Material Variants [DD 2.10], Arcane `.armat` instances -- the
   same model, independently converged. All three share the rename wart (overrides
   key on names); we documented ours, Unity loses values silently, UE mitigates
   with parameter GUID remapping in some paths.

## 3. Where Arcane is genuinely ahead

Not "ahead for our size" -- ahead in absolute terms, each enabled by owning the
whole stack and having a two-surface, single-pass problem:

- **Node-accurate compiler errors on the real shader.** SG only maps errors via its
  per-node preview shaders; its master-shader errors are explicitly not mapped
  [DD 2.9]. UE annotates expressions through its translator but the mapping
  degrades inside folded/custom code. Our one-statement-per-node codegen plus a
  snippet-line->nodeId map gives exact badges from the actual DXC diagnostics --
  including lines INSIDE a Custom node's body -- with zero extra shader
  compilation.
- **Custom HLSL that sees the material's params and Time directly.** SG's CFN sees
  only its pins [DD 2.8]; UE's Custom node likewise pipes everything through
  inputs. Our generated `_cf` functions land after the cbuffer/Globals
  declarations, so a body can just write `Tint * sin(Time)`. Compile-gate tested.
- **Incremental undo.** SG snapshots the whole graph as JSON per action and users
  report minutes-long undos on large graphs [DD 2.5]. Our CommandStack pushes one
  gesture-scoped command that survives recompiles (doc-anchor pattern). UE is fine
  here too; Unity is the outlier we avoided copying.
- **One source, both backends, one request.** In-proc DXC emits DXIL + SPIR-V per
  submit with shared conventions; neither engine's editor loop is that direct.
- **Asset identity.** `.armat` GUIDs + registry give us reference integrity both
  engines approximate differently (UE: UAsset GUIDs but name-bound params; Unity:
  GUID-in-graph but deliberately name-bound materials [DD 2.10]). We have the
  foundation to fix the rename wart properly (rewrite instances on rename) --
  neither reference product does.
- **Diff-able, byte-stable asset text.** Sorted JSON round-trips byte-identically
  (tested). SG's MultiJson is also diff-stable [DD 2.1]; UE's binary UAssets are
  not diffable without tooling.

## 4. Where Arcane is behind, and what each gap costs

Ordered roughly by how soon the desk will feel them:

1. **Node library (21 vs 150-200).** The kernel that matters is ~25 nodes [DD 2.2];
   we have most of the arithmetic core but lack Combine, Clamp, Smoothstep, Step,
   Power, Remap, TilingOffset, Swizzle, Cos, Abs/Min/Max, and a noise. Cost: one
   table row + emission case + tests each; the Custom node covers the tail today.
2. **Searcher UX.** No fuzzy create-search, and above all no drag-wire-to-empty-
   canvas -> filtered menu -> auto-connect, SG's signature interaction [DD 2.5].
   Already the #1 planned follow-up.
3. **Per-node preview thumbnails.** SG's biggest at-a-glance win [DD 2.9]. Our
   plan-of-record: the big preview + line-mapped badges may suffice; if not, SG's
   recipe (subtree compiles, <=2 in flight, last-good dimmed) is documented. UE
   ships without thumbnails, so this is a want, not a need.
4. **Vertex stage.** No WPO/vertex-context equivalent; fullscreen+sprite surfaces
   have not needed one. `%{VERTEX_BODY}` is the reserved seam.
5. **Multi-pass targets.** UE materials generate many passes; SG targets own pass
   lists. We have one pass per surface; the fullscreen pass-chain design (additive
   `"passes"` schema) is specced as a post-arc slice. Sprite multi-pass is
   correctly renderer-owned in all three worlds.
6. **Reuse (functions/subgraphs).** None. Deliberately far-future; SG's conventions
   are documented for when it matters [DD 2.8].
7. **Variants/keywords.** None, and the deep dive's SKIP verdict stands -- variant
   explosion is both engines' largest operational pain (UE permutations, SG's
   2048 cap). We will not import it without a driving feature.
8. **Canvas niceties.** Groups, comments, redirects/reroutes, copy/paste (!),
   multi-select alignment, zoom-to-fit shortcuts. Copy/paste is the first real
   absence a power user will hit; imgui-node-editor gives selection + clipboard
   hooks to build on.
9. **Ecosystem maturity.** Versioned migrations (SG's per-object `m_SGVersion`
   [DD 2.1]), deprecation flows, template/starter galleries, docs, and ten years
   of bug fixes. Ours is: the format is young, the suite is green, and the
   unknown-node policy is refuse-don't-corrupt (vs SG's round-trip-unknowns --
   worth adopting if the format ever needs forward-compat).

## 5. Verdict

Architecturally we are a faithful, validated member of the same family: engine-owned
pass + description-function graph + custom-HLSL islands + sparse-override instances.
Within that family we hold four real advantages (line-mapped errors on the real
shader, params-visible custom bodies, incremental undo, dual-target single-source)
and one structural one (GUID asset identity that could eventually fix the rename
wart every engine shares). Everything we lack is either scheduled (searcher
interaction, node-library growth, pass chains), consciously skipped with evidence
(keywords, preview farms, full-snapshot undo), or maturity that only time buys
(migrations, copy/paste polish, ecosystem).

Recommended order stands: desk-verify -> drag-wire searcher -> node-library growth
via the table -> copy/paste -> pass-chain slice when a real effect demands it.
