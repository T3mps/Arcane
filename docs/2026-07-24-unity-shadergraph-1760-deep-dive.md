# Unity Shader Graph 17.6.0 deep dive — reference model for the Arcane material graph (Slice 9)

Date: 2026-07-24. Product studied: `com.unity.shadergraph@17.6.0` (Unity 6.x line; Graphics-repo
`master` == 17.6.0 per `package.json`, verified). This document synthesizes 15 research reports
(9 dimensions + 6 gap-fills) into decisions for Slice 9 of the shader-editor arc
(`docs/superpowers/specs/2026-07-23-arcane-shader-editor-arc-design.md` §5.8, `Arcane/Arcane/src/Arcane/Material/MaterialGraph.hpp`).

Full dimension reports (drill-down): session scratchpad `shadergraph/` —
`data-model.md`, `node-library.md`, `type-system.md`, `codegen.md`, `ux-interaction.md`,
`blackboard-properties.md`, `master-stack-targets.md`, `subgraphs-custom.md`, `previews-errors.md`,
`gap-1.md` (materials/variants), `gap-2.md` (preview shader content), `gap-3.md` (color space),
`gap-4.md` (verbatim sprite/fullscreen shaders), `gap-5.md` (convert-to-subgraph), `gap-6.md` (templates).
Where a fact matters long-term the primary URL is cited here directly. **Version flags**: nearly
everything below is long-standing (SG 10.x–12.x era, unchanged through 17.6); 17.x-specific items
are marked.

---

## 1. Executive summary — the 15 facts that most affect our design

1. **Edge identity is (node GUID, per-node int slot id) on BOTH ends.** SG has no single-output
   assumption anywhere; multi-output nodes are pervasive (Sample Texture 2D = RGBA + R/G/B/A + more;
   Split; Combine emits RGBA/RGB/RG). Our `GraphLink` carrying only `fromNode` is the one structural
   choice in `MaterialGraph.hpp` that will hurt: even our 14-node set wants `TextureSample.a` for
   sprite alpha. Add `fromPin` before anything serializes. [data-model.md §1.6/§3, node-library.md §3b]

2. **One edge per input, silent replace; outputs fan out; cycles refused silently at connect
   time** (`GraphData.Connect` does a reachability check and returns null — no dialog, no badge; the
   drag-filter does NOT check cycles). This trio is the entire connection UX and it feels great.
   [ux-interaction.md §5, previews-errors.md §2]

3. **Codegen = post-order DFS from the output(s) = topological order; dead code never emits**
   (only reachable-and-active nodes generate). Per node: outputs declared as locals named from the
   *serialized* node id (`_Add_<objectId>_Out_2_Vector4`) → **same graph file ⇒ byte-same HLSL**,
   deterministic by construction, no hashing. Our `_n<id>` SSA plan is the same idea. [codegen.md §3/§6/§11]

4. **~90% of the ~190-node library is one data table**: `CodeFunctionNode` = typed ports + one HLSL
   body string (`Add` = `Out = A + B;`), reflected into ports + a dedup'd function registry.
   Hand-written code exists only for texture sampling, space transforms, swizzle validation, and the
   custom-function escape hatch. Build our node set as a static table, not a switch-per-node.
   [node-library.md §3/§8]

5. **Type adaptation is tiny and exact**: any vector↔vector connects; scalar promotes by splat
   (`.xxxx`); V2→V4 appends `0.0, 1.0`, V3→V4 appends `1.0`; truncation = leading swizzle (`.xy`);
   dynamic nodes (Add/Mul/Lerp) resolve to the **smallest connected width** (scalars never pin width).
   Copy this table verbatim; it is the whole "it just works" wiring feel. [type-system.md §3/§4]

6. **Unconnected input defaults become hidden material properties in preview compiles** (literal
   inlined only in the final shader) — this is why scrubbing an inline value updates the preview with
   ZERO recompiles. Our params already live-write the CB; routing Const-node values the same way in
   preview compiles is the single best perf trick to steal. [type-system.md §5, gap-2.md §3]

7. **Undo is a full-graph JSON snapshot per action, and it is SG's #1 complaint** ("Four minutes
   per undo"; users abandon undo entirely). Every edit also regenerates whole preview shaders.
   Do NOT mimic: our CommandStack incremental undo + snippet-hash compile cache is already the fix
   Unity never shipped. [ux-interaction.md §10/§19]

8. **The graph never owns the pass — it emits description functions only.** RP-owned vert/frag call
   `SurfaceDescriptionFunction(...)` through adapters; the pass file owns combine/tint/discard.
   Verbatim sprite frag: `half4 color = half4(surface.BaseColor, surface.Alpha)` →
   `if (color.a == 0) discard;` → `color *= vertexColor` unless `_DISABLE_COLOR_TINT`. Our
   `%{MATERIAL_BODY}` + engine-owned `ps_main` is exactly this architecture. Fullscreen target:
   **no vertex context at all**, `uv0` re-based to screen UV — precedent for one node vocabulary over
   two Varyings contracts. [gap-4.md §4.4/§5.5/§8, master-stack-targets.md §5/§6]

9. **Sprite graphs have NO implicit sprite texture** — the author must create a Texture2D property
   with reference `_MainTex` and sample UV0 manually. Our sprite template hands the batcher texture
   to the snippet directly; that is *better* for us (batcher owns binding), keep it. [master-stack-targets.md §6]

10. **Properties have two names** (display vs sanitized HLSL reference name; reference tracks display
    renames until user-overridden, then freezes) — and **materials bind values by reference-name
    STRING, never GUID**. Rename = silent value loss on every material; deleted properties orphan
    forever; Unity has GUIDs in-graph and deliberately never uses them for binding. Our `.armat`
    has real GUID identity available — we can beat Unity here, or at minimum document the same wart.
    [blackboard-properties.md §2, gap-1.md §2]

11. **Material Variants = our instance model, validated**: parent asset ref + sparse name-keyed
    override dicts, presence-in-dict == "is override", parent changes propagate to non-overridden
    params. SG 17.0.3 added an auto-generated default-material sub-asset so variants can track graph
    defaults — the exact `.armat` template/instance split we already shipped in Slice 7. [gap-1.md §4/§5]

12. **Deleting a property with live nodes silently converts each PropertyNode to the equivalent
    inline value node** (position/edges preserved); pasting a property node into a graph lacking the
    property degrades to an inline node too. Graceful degradation everywhere: unknown nodes round-trip
    their JSON; broken subgraphs emit zeroed outputs and keep the parent compiling. [blackboard-properties.md §8, previews-errors.md §2]

13. **Per-node previews are per-node *subtree shaders*, not an uber-shader** — that's what lets HLSL
    errors badge the right node (first compiler message of that node's shader). Master-shader errors
    intentionally do NOT map to nodes. Stale previews keep the last-good texture dimmed to 30% alpha;
    errors show a magenta/black 2×2 checker; ≤2 compiles in flight, master first. NOTE: because OUR
    codegen is one statement per node in one body, we can map DXC diag lines → node ids with a simple
    line map — node-accurate error badges *without* per-node shaders. Cheaper than Unity. [previews-errors.md §1/§2, gap-2.md]

14. **Extensibility is closed** (`AbstractMaterialNode`/`Target` all internal; "SG2" unshipped); the
    only supported escape hatches are the **Custom Function node** (String mode = auto-generated
    signature + user body — literally our snippet codegen shape) and Sub Graphs (compiled to
    `SG_{name}_{assetGuid}` functions; recursion banned via a transitive-descendents GUID set).
    String-mode CFN is our cheapest high-value follow-up. [subgraphs-custom.md §6/§7/§9]

15. **Color space**: Default-mode color literals are serialized as sRGB numbers and converted
    **in generated code** (`IsGammaSpace() ? raw : SRGBToLinear(...)` — compile-time folds); HDR
    colors are stored linear with intensity premultiplied. Arcane is linear-HDR end to end with no
    gamma-project duality — decide once: **ConstColor stores linear floats** (convert at the picker
    if the picker is sRGB), and no shader-side conversion machinery is needed. [gap-3.md]

---

## 2. Per-dimension distilled reference

### 2.1 Data model & serialization [data-model.md]

- `.shadergraph` = "MultiJson": flat sequence of pretty-printed JSON objects split by blank lines;
  GraphData root first, all others **sorted by objectId** — deliberately diff/merge-stable. Format
  unchanged since SG 10.0 (2020).
- Every object: `m_SGVersion` (per-object schema version), `m_Type` (class name + `[FormerName]`
  aliases = rename-safe), `m_ObjectId` (32-hex GUID). Two reference kinds: `JsonData<T>` (owning —
  file contents = reachable-ownership closure) vs `JsonRef<T>` (id-only pointer).
- Edges are inline plain structs `{outputSlot: {node, slotId}, inputSlot: {node, slotId}}`; slots are
  their own objects with `int m_Id` unique per node (stable constants in node code), `m_Value` +
  `m_DefaultValue`.
- Unknown types survive: `UnknownJsonObject` stand-ins re-emitted verbatim on save. Saving always
  rewrites at the latest schema version (no old-format preservation); migrations mint deterministic
  RFC-4122-v5 ids so upgraded graphs generate stable shaders.
- **Templates (17.3.0+)**: `m_UseAsTemplate`/`ShaderGraphTemplate` live on the **importer** (.meta),
  never in the graph; template instantiation is a dumb file copy with fresh identity; discovery is
  search-index-driven; "hidden unless Expose As Shader" keeps templates out of the shader picker at
  the compile level. [gap-6.md]
- URL: https://docs.unity3d.com/Packages/com.unity.shadergraph@17.6/manual/index.html ; source
  `Packages/com.unity.shadergraph/Editor/Serialization/*` on github.com/Unity-Technologies/Graphics.

### 2.2 Node library [node-library.md]

- ~190–200 nodes across Artistic/Channel/Input/Math/Procedural/Utility/UV (+ pipeline extras).
  Three codegen patterns: (1) `CodeFunctionNode` data-style (bulk; ports from `[Slot]` attributes,
  body = HLSL string, dedup'd via `FunctionRegistry`, `$precision` + dynamic-dimension name suffixes);
  (2) hand-written `GenerateNodeCode` (Sample Texture 2D, Transform, Swizzle, noise, Custom Function);
  (3) zero-code variable nodes (`Time` → `IN.TimeParameters.x`; ~20 `IMayRequire*` interfaces drive
  interpolator generation).
- Redirect node = data-level passthrough, 1-in/1-out any type, **zero codegen** (chains collapsed at
  resolve), excluded from search, created by double-clicking a wire.
- Minimal kernel that covers tutorials + reduces the rest to sugar (~25): Sample Texture 2D, UV,
  Tiling+Offset, Time, Multiply, Add, Subtract, Lerp, Split, Combine, One Minus, Saturate/Clamp,
  Remap, Step, Smoothstep, Power, Fraction, Sine/Cosine, Position, Normal, Screen Position, Fresnel,
  Simple Noise, Branch, Custom Function. Properties come from the Blackboard, not the library.

### 2.3 Type system & ports [type-system.md]

- Declared `SlotValueType` (incl. 3 dynamics) vs resolved `ConcreteSlotValueType`. No int type;
  Boolean compiles to a float. Resources (textures/sampler/gradient) are exact-match-only connections;
  matrices truncate-only (no promotion); vectors↔matrices never connect except through Multiply's
  fully-dynamic slot.
- Dynamic resolution: 0 connected → V1; else drop scalars/bools (they splat) and take the **minimum
  connected width**; result stamped on all dynamic inputs AND outputs. Multiply classifies
  vector/matrix/mixed and emits `*` or `mul`.
- Adaptation strings (exact): splat `x.xxxx`; fill V2→V3 `+0`, V2→V4 `+0,1`, V3→V4 `+1`; truncate
  `.xy`/`.xyz`/`.x`. Previews force `half4(x,y,z,1)` (alpha discarded).
- Port dot colors (steal the palette): Float `#84E4E7`, V2 `#9AEF92`, V3 `#F6FF9A`, V4 `#FBCBF4`,
  Bool `#9481E6`, Matrix `#8FC1DF`, textures `#FF8B8B`, misc gray.
- Precision (`$precision` → float/half, per-node min-inherit rules) — orthogonal axis we don't need.

### 2.4 Codegen pipeline [codegen.md, gap-4.md]

- One `Generator` class; per-target SubShader; per-pass template splice
  (`$splice(Name)`, `$include(...)`, `$FieldName:` line predicates; empty splice emits
  `// {token}: <None>`). Direct analogue of `%{MATERIAL_BODY}`, line-oriented + predicate-capable.
- Emission: `SurfaceDescription SurfaceDescriptionFunction(SurfaceDescriptionInputs IN) { ... nodes
  in DFS order ...; surface.<Block> = <slotValue>; }`. Function-per-node-type registered once
  (insertion-order-preserving registry; duplicate name+body rolled back; conflicting bodies = error);
  call-per-instance with args by slot id; unconnected inputs inline literals (ForReals) or preview
  properties (Preview).
- Properties: dedup by referenceName; `CBUFFER_START(UnityPerMaterial)` for per-material,
  textures/globals outside; ShaderLab block ordered by Blackboard order. PropertyNode always emits a
  local alias.
- Keywords → `#pragma multi_compile/shader_feature/dynamic_branch` + Cartesian permutations wrapped
  in `KEYWORD_PERMUTATION_n` (limit 2048 build / 128 preview) — we skip all of it.
- "View Generated Shader" runs the real generator with `humanReadable: true` to a temp file — worth
  copying as a debug affordance (we already show generated text read-only in graph-owned mode).

### 2.5 Canvas UX [ux-interaction.md]

- Searcher: Spacebar/right-click; fuzzy + synonyms + Tab autocomplete; **drag-wire-to-empty-canvas
  opens the searcher filtered per-slot and auto-connects the first compatible slot** — the most
  copied node-editor interaction, and the one to prioritize.
- Zoom 5%–800% weighted steps; A = frame all, F = frame selection; Alt+letter node-creation chords
  (17.x); Ctrl+R redirect; Ctrl+T toggle previews.
- Copy/paste: serialized subgraph clipboard, paste recentered at mouse preserving relative layout;
  cross-graph property nodes rebind by objectId OR (type+referenceName) else degrade to inline nodes.
- Blackboard drag-to-canvas instantiates property nodes; "Convert To Property ⇄ Inline Node"
  round-trip is a heavily-used micro-flow.
- Known failures to avoid: whole-graph snapshot undo, full preview regeneration per edit, no
  variable get/set portals ("C# without functions"), stateful edge-drag crashes.

### 2.6 Blackboard/properties/keywords [blackboard-properties.md]

- Display name dedup `Foo (1)`; reference name = `_`+display sanitized (`ConvertToValidHLSLIdentifier`),
  dedup `_Foo_1` across ONE namespace (properties+keywords+dropdowns); tracks renames until user
  override freezes it; right-click Reset Reference.
- Exposure: "Show In Inspector" (17.0.1 rename of "Exposed") + Scope popup
  (DoNotDeclare/Global/PerMaterial/HybridPerInstance); only PerMaterial+shown enter the ShaderLab
  Properties block. Float modes Default/Slider(Range)/Integer/Enum; texture defaults
  white/black/grey/bump; not-exposable: Gradient, matrices, SamplerState.
- Deletion converts PropertyNodes to inline value nodes in place. Copies re-dedup names (keywords
  keep reference names for cross-graph correctness).

### 2.7 Master stack / targets / surfaces [master-stack-targets.md, gap-4.md]

- Exactly two contexts (Vertex + Fragment); nodes wired to both are generated twice (no implicit
  interpolation; Custom Interpolators are the explicit channel, 32-float budget). Blocks are
  target-owned descriptors; active set = union over active targets; inactive blocks gray out but are
  preserved if connected/edited.
- Surface switching = Target (pipeline) × "Material" (SubTarget) popup; one material active per
  target. Sprite subtargets hard-wire transparent + double-sided + ZWrite off (no Surface Type UI).
  Fullscreen: fragment-only, engine-owned vertex emits screen UV + view dir, blend/stencil authoring
  in Graph Settings, `_FlipY` global.
- Our analog: template kind (fullscreen/sprite) = the SubTarget popup; single `Output(float4)` =
  a one-block stack. Sufficient until a vertex slot exists.

### 2.8 Sub graphs / custom function / extensibility [subgraphs-custom.md, gap-5.md]

- Sub graph = own asset; inputs = its blackboard, outputs = an Output node (vector/matrix/bool only);
  compiles at import to `SG_{name}_{assetGuid}_$precision(...)` with outputs as `out` params; parent
  references by GUID; port identity via property-GUID lists so renames keep wires; recursion banned
  via `descendents` transitive GUID set; broken subgraph → empty function + error propagated (parents
  stay loadable).
- Custom Function: File mode (GUID-referenced .hlsl, must define `Name_float`) vs **String mode**
  (Unity generates signature/braces; user writes the body only) — the latter IS our snippet model.
- Convert-to-subgraph: boundary inference groups by unique source slot (one property per upstream
  slot, one output per inner source slot); defaults NOT transferred; Color degrades to Vector4;
  a spurious default `Out_Vector4` slot is always added first. Evidence that extraction is doable but
  fiddly — fine to defer.
- No public node/target API in 17.6. CFN + subgraphs are the entire supported extension surface.

### 2.9 Previews & errors [previews-errors.md, gap-2.md]

- Node previews compile against a pipeline-agnostic `PreviewTarget` (NOT the real target):
  `#define SHADERGRAPH_PREVIEW 1`, generic mesh template, zero lighting code, frag returns
  `surfaceDescription.Out`. Scene-dependent nodes hit neutral fallbacks via macro indirection
  (Scene Depth→1, Scene Color→0, ambient→0); Time is real/animated. Master preview compiles the REAL
  target shader (+`SHADERGRAPH_PREVIEW_MAIN`, 17.0.3).
- Rig: FOV 15°, two directional lights; 2D previews = ortho 0.5 + unit quad ⇒ UV==screen; node RTs
  200×200, master 400×400.
- Validation pipeline: cleanup dangling edges → setup → concretize (type errors) → validate (target
  compat); **no NaN/value validation exists**. Errors: `MessageManager` provider→node→messages; one
  badge per node, message on hover; import falls back to the magenta error shader with a console line
  naming the first error.
- Deprecation: per-node `sgVersion` + dismissible "newer version available" warning + `(Legacy vN)`
  title suffix.

### 2.10 Materials & instances [gap-1.md]

- Flat .mat = full snapshot of every shader property at creation, keyed by reference-name string;
  defaults never re-propagate; orphans accumulate (manual "Remove Unused Properties"). Variant .mat =
  `m_Parent` ref + only overridden entries; locks stored on the ancestor; editor-only concept
  (builds flatten). Keyword state = valid + retained-invalid lists, largely recomputed from float
  props by pipeline GUIs (self-healing).
- SG 17.0.3 auto-generates a default material sub-asset per graph (regenerated every import ⇒ always
  mirrors graph defaults) — parenting variants to it = default-tracking instances with sparse
  overrides. That is the `.armat` template/instance model, independently arrived at.

---

## 3. MIMIC / ADAPT / SKIP / DEFER

Constraints: imgui-node-editor immediate-mode canvas; snippet codegen into `%{MATERIAL_BODY}`;
`.armat` JSON assets with GUID identity; two surfaces (fullscreen, sprite); existing params panel =
our blackboard; existing instances = our per-material overrides; CommandStack undo; DXC compile
service with content-hash cache.

| Shader Graph capability | Verdict | Our shape / rationale |
|---|---|---|
| Edge = (node id, slot id) both ends | **MIMIC** | Add `fromPin` to `GraphLink`. Multi-output nodes (TextureSample.a, Split) are unavoidable; retrofitting breaks saved graphs. |
| One-edge-per-input silent replace; output fanout; silent cycle refusal at connect | **MIMIC** | Implement in the canvas connect handler exactly (reachability check → refuse, no dialog). |
| Deterministic codegen naming from serialized ids; topo DFS; dead-code-by-reachability | **MIMIC** | Already the `GenerateGraphSnippet` plan; make id allocation monotonic (see §4.3). |
| Vector adaptation table (splat / fill 0,1 / leading-swizzle truncate) | **MIMIC** | Copy the exact strings; this is the whole wiring feel in ~20 lines. |
| Dynamic min-width resolution on math nodes | **ADAPT** | MVP: fixed widths per node table + adaptation at edges (simpler, predictable). Revisit if float4-everywhere annoys. |
| Node-as-data-table (CodeFunctionNode pattern) | **ADAPT** | Static table `{token, pins[], HLSL expr pattern}` instead of C# reflection; the codegen walks the table. Makes library growth ~5 lines/node. |
| Function registry / function-per-node-type | **SKIP** | Our nodes are single expressions inlined as SSA statements; no shared functions needed at this scale. Revisit with noise nodes. |
| Preview trick: unconnected defaults → hidden cbuffer params (no recompile on scrub) | **ADAPT** | High-value follow-up: in preview compiles, route Const*/unwired defaults through the existing live-CB path. |
| Full-snapshot undo | **SKIP** | Their #1 complaint. CommandStack incremental undo (graph ops = commands) — we already have the machinery. |
| Per-node preview subtree shaders for error attribution | **SKIP** (mechanism) / **MIMIC** (outcome) | Our one-statement-per-node body lets a codegen line-map attribute DXC diags to node ids directly — node badges without N shaders. |
| Per-node preview thumbnails | **DEFER** | Spec non-goal. When built: last-good dimmed 30%, magenta checker on error, ≤2 compiles in flight. |
| Drag-wire-to-canvas → filtered create menu → auto-connect first compatible | **MIMIC** | imgui-node-editor's `QueryNewNode` supports it; the single best UX in SG. Can land as fast-follow within Slice 9. |
| Searcher fuzzy/synonyms/Tab | **ADAPT** | Simple substring-filtered popup first; synonyms later. |
| Redirect (elbow) nodes | **DEFER** | Cheap (passthrough collapsed at codegen, excluded from search); pure polish. |
| Blackboard two-name model (display vs reference) | **ADAPT** | One name for MVP, sanitized to a valid HLSL identifier at edit time; dedup on conflict. Split later only if display-name freedom is requested. |
| Property deletion → inline-node conversion; paste degradation | **DEFER** | MVP: dangling param node = codegen error with node badge. Graceful conversion later. |
| Blackboard drag-to-canvas / Convert To Property ⇄ Inline | **DEFER** | Great micro-flow; needs params panel drag source. Post-MVP. |
| Master stack / blocks / multi-target | **SKIP** | Our template-kind enum + single Output node is the degenerate (and sufficient) case. Revisit only when a `%{VERTEX_BODY}` slot exists. |
| Custom interpolators | **SKIP** | No vertex graph. |
| Sprite: author-declared `_MainTex` convention | **SKIP** | Engine-provided sprite texture in the template is strictly better for the batcher. |
| Fullscreen: same node vocabulary re-based on screen semantics (uv0 == screen UV) | **MIMIC** | Already implicit in our per-template Varyings; document it on the UV node. |
| Keywords / variants / permutations / precision (`$precision`) | **SKIP** | Spec non-goals; single-precision engine; dropdown-style compile-time switches only if subgraphs ever land. |
| String-mode Custom Function node | **DEFER (first follow-up)** | Matches our codegen exactly (auto signature + user body + typed pins); the escape hatch that makes a 14-node library survivable. |
| Sub graphs (GUID-in-function-name, descendents recursion set, error → zeroed outputs) | **DEFER** | Spec non-goal. Adopt SG's exact conventions when built. |
| Material Variants sparse overrides | **(already ours)** | Validated by gap-1; note the rename wart (§4.4). |
| Diff-stable serialization (stable ordering on save) | **ADAPT** | Sort nodes/links by id when writing the `.armat` graph object; near-free diff stability. |
| Unknown-data round-trip (`UnknownJsonObject`) | **ADAPT** | `GraphFromJson` currently returns nullopt on unknown node types — acceptable for a closed set, but never *drop* unknown fields on rewrite; fail the load loudly rather than guess (already the contract). |
| Templates (importer sidecar + dumb copy + hidden-from-picker) | **DEFER** | Validated pattern for engine-shipped starter `.armat`s (Unity ships exactly our two surfaces as templates). |
| Color modes / heatmap / groups / sticky notes | **SKIP/DEFER** | Organizational polish, no data-model impact except optional group record. |
| Port colors by type | **MIMIC** | Steal the hex palette (§2.3); costs nothing, reads instantly to anyone who knows SG. |

---

## 4. Critique of `MaterialGraph.hpp` (change these BEFORE implementation hardens)

**What the draft gets right (validated against SG):**
- Snippet-emitting graph over an engine-owned template = exactly SG's description-function contract
  (graph never owns the pass; the pass owns combine/tint/discard) — the architecture is confirmed
  correct, including the sprite/fullscreen split. [gap-4.md §8]
- Graph-owned vs text-owned with one-way convert and no text→graph decompile — nobody ships
  decompilation; SG has no text mode at all. Our model is a strict superset.
- Persisted ids driving codegen naming (deterministic output), reachability-based dead code,
  last-good-on-error, unreachable islands legal — all match SG's proven behavior.
- Params-as-nodes sharing one declaration by name with type-conflict rejection mirrors SG's
  reference-name model; the unified params panel is the Blackboard equivalence we wanted.
- The fat `GraphNode` struct (union-of-payloads) is right for a 14-type set; SG's 40 slot subclasses
  exist to serve 200 nodes. Keep it.

**Changes, each with rationale:**

1. **Add `std::uint32_t fromPin = 0;` to `GraphLink` now.** SG edges carry a slot id on both ends;
   multi-output nodes are already needed by our own set (TextureSample must expose `.rgba` and `.a`
   for sprite alpha; a future Split is the #1 kernel node we lack) — adding it later invalidates
   every serialized graph.
2. **Give nodes a small output-pin table alongside `GraphInputPinCount`** — e.g.
   `GraphOutputPinCount(type)` + per-pin result width — so TextureSample can be (rgba: float4,
   a: float) day one. The header's "every node has exactly ONE output pin" comment is the one line of
   the contract to retract.
3. **Make id allocation monotonic (serialize a `nextId` counter) instead of `NextId() = max+1`.**
   Max+1 re-mints deleted ids; with incremental CommandStack undo (not whole-graph snapshots), a
   delete→create→undo sequence can alias a stale link onto a new node. SG never reuses ids (GUIDs);
   a persisted counter buys the same safety for 4 bytes.
4. **Structure codegen errors as `{nodeId, message}` instead of "node N: ..." strings.** The canvas
   needs to badge nodes (SG: one badge per node, message on hover); parsing prefixes back out of
   strings is silly when we own both ends. Reserve nodeId 0 for graph-level messages.
5. **Specify the wire-type/adaptation rule in the header contract.** The node set mixes widths
   (ConstFloat vs ConstFloat4) but the header is silent on what `Add(float, float4)` emits. Adopt
   SG's table verbatim (splat scalars, fill `0,1`, leading-swizzle truncate) and state each node's
   fixed operand width in the pin-order comment — this is load-bearing for both codegen and the
   canvas's pin coloring.
6. **Declare pin order append-only.** The header correctly makes pin order the canvas↔codegen
   contract; add the evolution rule (new pins append; never reorder/remove) since `toPin` is a bare
   index into it — SG survives schema drift only because slot ids are explicit constants.
7. **Decide ConstColor color space in the header comment: linear floats, converted at the picker if
   needed.** SG burns real complexity on sRGB-vs-linear literal conversion because it supports
   gamma-space projects (gap-3.md); Arcane is linear-only — say so once and skip the machinery.
8. **Note the param-rename wart.** Instance overrides and codegen both key on `paramName`; renaming
   a param orphans instance overrides exactly like Unity's reference-name rename (silent value loss,
   gap-1.md §2.2). MVP: document it; later: editor-assisted rename that rewrites instances (we have
   the GUID-identified asset graph to find them — Unity doesn't).
9. **Missing node types worth adding to the enum while it's cheap** (each is one table row once
   multi-output lands): `Sub` (a−b), `Saturate`, `Fraction`, `Split`/`Swizzle` (needs #1/#2), and a
   surface-input `VertexColor` for the sprite template's `Varyings.color` (SG sprite authors reach
   for Vertex Color constantly; our sprite template already interpolates it). Everything else in the
   ~25 kernel is sugar that can wait.
10. **Keep `GraphToJson` output ordered (sort nodes/links by id).** SG sorts objects and edges on
    save purely for diff stability; one `std::sort` buys the same for `.armat` diffs.

---

## 5. Revised Slice 9 build plan

### MVP (the slice gate: codegen unit tests green + desk-verified authoring loop)

1. **Data-model corrections first** (§4.1–4.8): `fromPin`, output-pin table, monotonic `nextId`,
   structured errors, adaptation rule documented, append-only pin contract, linear ConstColor,
   sorted serialization. All before any canvas code — these are the choices that harden.
2. **Node table as data**: one static `constexpr`-style table
   `{type, token, display, inPins[{name,width}], outPins[{name,width}], emitPattern}` consumed by
   codegen, the canvas (pin labels/colors), and the create menu. Adding a node type = one row + tests.
3. **`GenerateGraphSnippet`**: topo walk from Output (post-order DFS, visited-set dedup), SSA locals
   `_n<id>` (+ `_n<id>_<pin>` for multi-output), SG adaptation table at each edge, param
   declaration coherence (name→type conflict = error on both nodes), neutral defaults for unwired
   pins per the header contract, **line map (snippet line → nodeId) returned alongside the snippet**
   for compiler-diag badge mapping. Golden tests: graph → expected snippet → compiles via the
   compile service (both templates).
4. **Canvas (imgui-node-editor)**: create menu (filtered list), connect semantics = silent replace +
   silent cycle refusal, delete/duplicate, SG port-color palette, param nodes editing the shared
   ParamDecl, node badges from structured errors + compiler line map. Graph edits = CommandStack
   commands (AddNode/RemoveNodes/Connect/Disconnect/MoveNodes/EditValue), one command per gesture.
5. **`.armat` integration + owned-mode rules**: `"graph"` object (nodes/links/nextId, sorted),
   graph-owned ⇒ text panel read-only showing the generated snippet, one-way convert-to-text
   (drops the graph object, keeps the snippet), save always writes the generated snippet so every
   existing loader/instance/sprite path is untouched.

### Follow-ups, in priority order

1. **Drag-wire-to-empty-canvas → filtered create menu → auto-connect** first compatible pin
   (SG's signature interaction; imgui-node-editor `QueryNewNode`).
2. **Preview no-recompile scrubbing**: in preview compiles, emit Const-node values and unwired-pin
   defaults as hidden cbuffer params driven by the existing live-CB path; final save still inlines
   literals. (SG's single best editor-perf mechanism.)
3. **String-mode Custom Function node**: user names typed in/out pins + writes a body; codegen
   auto-generates the signature — identical shape to SG's CFN and to our snippet seam. The escape
   hatch that keeps the node library small honestly.
4. **Library growth via the table** toward the kernel: Split/Swizzle, Combine, Clamp, Smoothstep,
   Step, Power, Remap, TilingOffset, SimpleNoise (first node needing a shared helper function —
   introduce a minimal emit-once function registry then, not before).
5. **Param UX parity**: params-panel drag-to-canvas creates a Param node; Convert To Param ⇄ Inline
   round-trip; delete-param → inline-node conversion (SG's graceful path).
6. **Redirect nodes** (double-click wire, zero codegen) + optional groups/comments.
7. **Per-node preview thumbnails** (SG recipe: subtree compile, ≤2 in flight, last-good dimmed 30%,
   magenta checker on error) — only if desk use demands it; the big preview + line-mapped badges may
   suffice.
8. **Starter templates**: engine-shipped example `.armat`s per surface (sprite, fullscreen) surfaced
   in the browser — importer-sidecar metadata + dumb copy with fresh GUID, per SG's template browser
   pattern.
9. **Subgraphs** (far future): SG conventions wholesale — GUID-in-function-name, transitive
   `descendents` recursion set, invalid asset ⇒ empty function + propagated error, port identity by
   param GUID.

### Explicitly not doing (SG evidence)

Full-snapshot undo; uber- or per-node preview shader farms; keywords/variants/permutations;
precision axis; text→graph decompilation; master-stack block system; a public node plugin API
(SG itself never shipped one — the data table + custom-function node is the entire extension story).
