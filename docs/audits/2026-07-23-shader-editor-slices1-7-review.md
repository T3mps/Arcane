# Shader/Material Editor Arc — Slices 1-7 Review

**Date:** 2026-07-23 (evening; same session that built the slices)
**Scope:** the 7 commits on `shader-editor-slice1-material-core`
(@8c7ad17b @b4a5ae7d @8f655a29 @a0894630 @1914f103 @00b54cce @5f2b80fc),
reviewed against the arc spec
(`docs/superpowers/specs/2026-07-23-arcane-shader-editor-arc-design.md`)
and compared with Unreal Engine 5 (vendored source at
`Arcane/.example/UnrealEngine-release`; file:line citations from the spec's
section-4 research) and Unity (2022-era: BiRP/SRP, Shader Graph, Material
Variants; from working knowledge, not vendored source).
**Method:** three parallel code-review agents (core+compiler / engine plumbing /
editor shell), findings verified against the code before inclusion; the
architecture comparison is the reviewer's own. Findings are REPORTED here,
not fixed — dispositions belong to the arc owner.

**Disposition (2026-07-24):**
- C1 fixed @7b8e71fd (it was the live editor crash; + C4715 promoted to error
  workspace-wide). M1/M2/M4 fixed @587c9dba (with the registry write path and
  save-dialog defaults).
- Fixed in the follow-up hardening commit (this doc lands with it): M3 (undo
  survives recompile via doc-identity commands), M5 (post-crash DXC instance
  reset + second-target skip), M6 (`environmental` results never cached), M7
  (is_string guards), M9 (Clear() drops pendingGeneric); minors: dirty-vs-undo
  (EffectiveSerial baseline), shared statics x2 (per-doc members), duplicate-
  open (DocumentHost peek routes), SwitchProject doc pass (refuse-if-dirty +
  CloseAll), SetMaterial half-swap (create-into-locals), binding-set cache
  (real-key equality + bound), parser trailing-junk diagnostic, dump() UTF-8
  replace, provider drive-/root-relative escape, supersede-keeps-cache,
  CompileNow cache-hit LastGood refresh. Plus the Section-4 headless tests
  (ShaderEditorDocument save/chain/routing, CommandStack Push/Clear, last-good
  service contract).
- Deferred knowingly: NaN round-trip, Widen UTF-8, cache-hit identity fields,
  hash framing, unbounded compiler caches (LruCache exists), maxVersions
  headroom, def.type validation, CompileNow/latestJobForKey interplay,
  preview-texture pinning in the ImGui-NVRHI cache (needs an engine eviction
  hook), per-texture samplers (recognized grammar extension), b0/b1 constants
  into ShaderConventions.hpp.

---

## 1. Executive verdict

The seven slices land a coherent, small-scale re-derivation of the two
production material systems that matter, and the load-bearing decisions match
what UE and Unity converged on independently:

- **layout/values split** (MaterialTemplate vs MaterialInstance) = UE's
  `FUniformExpressionSet` vs `UMaterialInstance`;
- **template + snippet authoring** = Unity Surface Shaders' shape (the best
  authoring model Unity ever shipped, and one they abandoned for graph-only
  reasons, not technical ones);
- **one resolve seam** (override -> parent chain -> default) = UE's
  `GetParameterValue` chain and Unity's Material Variants;
- **live param edits never recompile; structural edits debounce-recompile
  with last-good bound** = exactly UE's Material Editor contract;
- **content-addressed compile cache including toolchain bytes** = UE's
  DDC/DXCWrapper trick, memory-tier only for now.

The overall architecture is sound — every adversarially-traced structural
suspect (cross-generation compile pairing, SwitchProject lifetimes, the
weak_ptr undo pattern, CPU-vs-HLSL offset agreement, the thread split in the
compile service) came back CLEAN. The defects that did surface (1 CRITICAL,
8 MAJOR, ~15 MINOR — Section 2) are edge-hardening and wiring bugs
concentrated where the test strategy doesn't look: ImGui halves, pre-bind
windows, failure paths. Three of them (missing return / hidden error
messages / save-before-bind data loss) must land before the editor is
desk-verified or handed to a designer; none require design changes except
the undo-across-recompile decision (M3). Section 5 sequences the work.

## 2. Code-review findings (three agents, verified)

### 2.1 Slices 1-2: material core + compile service

**CRITICAL:** none.

**MAJOR:**

- **ShaderCompiler.cpp:446-471 — the dxc compiler instance is reused after an
  SEH crash, including for the same job's second target.** After
  `CompileGuarded` catches a crash in the DXIL compile, the SPIR-V compile
  runs on the SAME possibly-corrupt `IDxcCompiler3`, and the worker's
  instance persists across jobs — every later compile runs through the
  poisoned instance, defeating the "a crash fails the job, never the engine"
  contract. Fix: on `crashed`, skip the second target and Reset() the worker
  (and CompileNow main-thread) instance so the next job recreates it.
- **ShaderCompiler.cpp:514-524 (+337-345, 470-484) — environmental failures
  are permanently cached by content hash.** The `Compile failed (hr=...)`
  (e.g. E_OUTOFMEMORY) and `DxcCreateInstance failed` results are
  `!crashed`, so `Absorb` caches them: one transient failure makes that
  snippet's hash return a phantom error for the rest of the session. Fix:
  tag environmental branches and skip the cache store for them.

**MINOR (verified; fix-cheap):**

- `CompileNow` bypasses `latestJobForKey`, so an older async result can
  overwrite a newer synchronous one in `lastGood` (mixed Submit+CompileNow
  on one key). ShaderCompiler.cpp:705-737.
- Cache hits keep the ORIGINAL request's `debugName`/`diag.file`/repro line
  — two materials with identical stitched source show the first one's
  filename in diagnostics. :496-505, :717-724.
- `ContentHash` concatenates fields with no separators/length framing —
  define boundary shifts can collide (`{FOO,12}` vs `{FOO1,2}`). :422-437.
- Superseded results are dropped BEFORE the cache store — undoing back to a
  just-compiled state recompiles content that finished seconds ago (keep the
  cache store on supersede; keep `lastGood` latest-only). :673-686.
- `CompileNow`'s cache-hit path returns before `Absorb` — `LastGood` never
  updates on a synchronous cache hit (async path does). :717-724.
- `Widen` is per-byte Latin-1, not UTF-8 — non-ASCII material names mojibake
  through DXC diagnostics. :220-227.
- `IsIdle`/`UndrainedCount`: main-thread contract undocumented, and
  `UndrainedCount` locks `mx` around `readyMain`, which `mx` does not
  actually guard — misleading. :689-703.
- `cache`/`lastGood`/`latestJobForKey` grow unbounded for the process
  lifetime (every debounced keystroke inserts dual-target bytecode);
  `Arcane::LruCache` exists in Core. :408-409, :514-524.
- **MaterialTemplate::Build never validates `def.type == decl.type`** — a
  mismatched default flows Guid bytes into the CB / float bits into the
  texture table silently. Warn + zero-reset on mismatch.
  MaterialTemplate.cpp:32-73.

**NIT:** ParseDiagHeader picks tokens by priority order, not leftmost
occurrence (contrived messages misparse); `EffectiveSerial()==0` fresh-chain
sentinel trap; b0/b1 slot constants live in GlobalParams.hpp, not the
self-declared source-of-truth ShaderConventions.hpp; PackCB is
O(params x chain x overrides) per pack with a per-frame warn on undersized
dst (fine for one fullscreen material, resolve-by-decl overload wanted
before Slice-8 per-sprite packing); GetModuleFileNameW/MAX_PATH truncation
undetected; JoinRepro doesn't quote spaced args.

**Agent's coverage verdict (slices 1-2):** Slice-1 coverage strong (packing
edges, defaults blob, chains, strict typing, serials). Slice-2 gaps: the
async cache-hit path is entirely untested (the path carrying the stale
debugName bug); the arc's headline "last-good stays bound while a newer
compile FAILS" has no test; failure memoization, keyless async submits,
CompileNow-vs-async interplay, and Shutdown-with-in-flight-jobs untested.

**Agent's overall verdict:** architecture sound at the center — the cbuffer
bump logic exactly matches HLSL packing semantics, EffectiveSerial is
provably monotone, the thread split is honored at every site, the supersede
protocol can never deliver an older result after a newer one. The two MAJORs
are "the day the guard rails were built for" bugs; fix before real designer
traffic.

### 2.2 Slices 3-4 + engine-side 5/7: plumbing, source pipeline, render pass

**CRITICAL:** none.

**MAJOR:**

- **MaterialAsset.cpp:169-170 — `LoadMaterialAsset` THROWS (unhandled
  nlohmann `type_error`) on a non-string `"name"` or `"kind"`.**
  `doc.value("name", ...)` calls `get<std::string>()` when the key exists
  with the wrong type; a hand-edited/merge-mangled `.armat` with
  `"name": 5` crashes the editor on double-click (the factory has no
  try/catch). Everything else in the loader is deliberately non-throwing —
  these two lines break the pattern. Fix: `is_string()` guards like the
  neighboring fields.
- **CommandStack.cpp:123-129 — `Clear()` does not clear `m_pendingGeneric`.**
  `Begin -> Push(cmd) -> Clear()` (e.g. on project switch) leaves the pushed
  command queued; the NEXT unrelated Begin/Commit cycle silently splices it
  in, and undoing that transaction replays an edit from before the cleared
  history against a previous project's instance (no UAF — the shared_ptr
  keeps it alive — but wrong-history mutation). One-line fix.

**MINOR (verified):**

- `//@param` parser silently accepts trailing junk when no `=`/`[` follows
  the name: `//@param float Speed 2.0` yields Speed=0 with NO diagnostic
  (forgotten `=` = black material, silently). Comment tolerance is also
  inconsistent across the three positions. MaterialSource.cpp:199-256.
- Non-finite values are lost across the `.armat` round-trip (parser accepts
  `inf`/`nan`; nlohmann serializes non-finite as `null`; load drops the
  entry) — a NaN from a drag edit silently disappears on save/load.
- `SaveMaterialAsset` can throw out of `dump(2)` on invalid UTF-8 in the
  snippet (paste path) — crash during Save, the worst moment. Use the
  `error_handler_t::replace` dump overload. MaterialAsset.cpp:139.
- `FullscreenMaterialPass::GetBindingSet` uses an accumulated hash as
  IDENTITY (no stored key, no collision check) and never evicts — cached
  sets pin evicted textures in VRAM; every picker swap adds an immortal
  entry. :200-205.
- `SetMaterial` partial failure leaves the pass half-swapped (members
  assigned before resource creation is checked) — contradicts its own
  "keeps previous material" contract; destroys last-good on a resource
  failure. Create into locals, swap on success. :64-110.
- `ShaderSourceProvider::Get` escape guard misses Windows drive-relative
  (`C:foo`) and root-relative (`\foo`) forms — both resolve OUTSIDE the
  roots. Also reject `has_root_name() || has_root_directory()`. :37-48.
- Volatile CBs at `maxVersions(16)`: correct for today's 1-2 Render
  calls/frame, but browser thumbnails (planned) through the same pass will
  exhaust versions. Size generously or document the budget. :85-141.

**NIT:** Runtime Impl member order leaves the resolver lambda dangling
during `~Impl` (latent only); `Cancel()` discarding already-applied pushed
generics is contract-consistent but leaves an applied edit with no undo
record if a generic joins a cancelled gesture by timing accident; FNV-32
name-hash collision would desync `metas` from `Params()` (~2^-16/pair);
MaterialAsset.hpp doc drift (claims decl-typed load; implementation is
self-typed + apply-gated); unterminated `%{` passes through with no
`unresolved` report; `from_chars` rejects leading `+` in defaults.

**Notable non-findings (verified sound):** CPU-vs-HLSL cbuffer offset
agreement including float2 straddle and texture-gap ordering; the
Vulkan stripped-CB/layout-superset concern (fullscreen VS references no
resources; superset layouts are valid); ParseTuple/range edges (negatives,
exponents, `[-1..1]`); the Runtime resolver lambda's address-stability
claim; SDL dialog ctx ownership on all paths; ResolveId memo growth.

**Agent's coverage verdict:** happy paths well covered end-to-end (incl. the
dual-backend GPU readback with live override). Gaps track the findings:
no parser tests for the silent missing-`=` case / non-finite / tabs; no
float2-straddle GPU offset-agreement test; **no GPU test with a texture
param** (t0../s0 arm, white fallback, Guid resolution in Render never run
on a device); `CommandStack::Push` has ZERO tests; no malformed-`.armat`
robustness tests beyond one shape.

**Agent's overall verdict:** engine-side slices architecturally sound and
spec-faithful; the MAJORs are edge-robustness bugs, not design flaws — but
findings 1-5 should land before a designer (the exact trigger population:
hand-editing text and files) gets the editor.



### 2.3 Slices 5-7 editor shell

**CRITICAL:**

- **C1 — AssetBrowser.cpp:136-138: `DrawAssetBrowserPanel` falls off the end
  of a value-returning function (UB).** Only the no-project early-out
  returns `actions`; the main path has NO return statement (MSVC C4715 is a
  warning, so the build passed). The caller immediately tests
  `browserActions.createInstanceOf.IsValid()` — when the return slot is
  stack garbage, a spurious "New Instance" dialog fires with a garbage
  parent Guid and mints an unresolvable instance. Fix: `return actions;`.

**MAJOR:**

- **M1 — ShaderEditorDocument::Save() before the first successful bind wipes
  the asset's saved params (data loss).** `m_data.params.clear()` runs
  unconditionally, but the repopulate needs `m_instance`, which exists only
  after the first successful compile+bind. Reachable: Save within the
  debounce+compile window right after opening; save-with-errors on an asset
  that opened broken; dxcompiler.dll missing (no doc ever binds — every save
  strips params); ConfirmSaveAndClose. Fix: keep `m_data.params` untouched
  while `m_instance == nullptr`.
- **M2 — the errors panel never displays the diagnostic MESSAGE.** The
  message is concatenated AFTER `##` in the Selectable label, so it is ID,
  not text (verified against vendored imgui: render stops at
  FindRenderedTextEnd). Rows read `error(12): ` with no message — the panel
  is functionally gutted. Fix: message before `##`, index-only ID.
  ShaderEditorDocument.cpp:491-495.
- **M3 — every successful recompile permanently orphans all prior param-edit
  undo steps.** BindIfComplete swaps `m_instance`; ParamEditCommand's
  weak_ptr to the OLD instance expires; Ctrl+Z pops "Edit Speed" and
  visibly does nothing. UAF-safe by design, but contradicts the spec's
  one-undo-system line. Fix needs a small design decision: commands target
  the DOCUMENT (stable identity, forward to current instance by nameHash)
  or get re-targeted during the migrate loop.
- **M4 — New Material / New Instance are never registered with the
  AssetRegistry.** The registry is scan-at-project-open; created assets are
  invisible to the browser, unresolvable by Guid (an instance whose parent
  was created this session fails ResolveParentChain until project reopen).
  Fix: incremental registration (`AddContent`-style) for files saved inside
  a mount root, warn when saving outside all mounts.

**MINOR (verified):**

- m1 — Undo/redo of a param edit never updates `m_dirty`: edit → save →
  Ctrl+Z → doc claims clean → closes without confirm, undone state silently
  lost. Track dirtiness via EffectiveSerial vs saved serial, or have
  commands mark the doc.
- m2 — the shared `static SnippetCallbackCtx` can leak a pending
  click-to-jump into ANOTHER document's editor (1-frame focus race).
  Per-document ctx member.
- m3 — shared `static s_hadBefore/s_before` gesture state can cross
  documents on same-frame keyboard-nav active-ID transfer → wrong undo
  before-value. Per-document members, same pass as m2.
- m4 — `OpenPath` on an already-open asset fully constructs a second
  document (ctor submits compiles on the SAME coalesce keys) before
  focus-not-reopen discards it — cancelling the live doc's in-flight
  compile and leaking a throwaway canvas+pass. Peek the asset id before
  constructing, or defer the first Rebuild.
- m5 — open documents survive SwitchProject with no prompt (fully traced:
  NO UAF — runtime mutates in place, every DocServices pointer stays
  valid) — but project-A docs stay open over project B, undo history is
  cleared mid-session, texture params resolve against B's registry. Run the
  dirty-confirm/close pass inside SwitchProject.
- m6 — closed documents' 512x512 preview textures are pinned forever by the
  ImGui-NVRHI binding-set cache (strong ref, no eviction) — one leak per
  open/close cycle. (The same strong ref is what makes close-mid-frame
  safe.) Needs an eviction hook.

**NIT:** VS-only compile failures leave "compiling..." with an empty errors
panel (ConsumeResult keeps PS diags only); double "New Instance..." dialogs
share one pending-parent slot; nil-guid .armat files merge into one ImGui
window (identical ###label) and skip focus-not-reopen; the texture-param row
leaves gesture-state item queries pointed at its last button (benign today);
missing dxcompiler.dll shows perpetual "compiling..." instead of a
"compiler unavailable" state.

**Verified clean under adversarial tracing (the review's top suspects):**
cross-generation VS/PS pairing is IMPOSSIBLE by construction (monotonic
jobIds never reused + both stage ids refreshed and both byte buffers cleared
per Rebuild + drain-side supersede); SwitchProject dangles nothing;
the weak_ptr command pattern has no UAF; instance chain layering matches
MaterialInstance semantics exactly; DocumentHost's every-frame OpenPopup is
explicitly legal; the InputTextMultiline buffer pattern is byte-for-byte the
imgui_stdlib contract; ImGuiChildFlags_Borders exists in vendored 1.92.9;
all Begin/End pairs balanced including the collapsed early-out; teardown
order sound (every DocServices borrow dies before its lender).

**Agent's coverage verdict:** DocumentHost close-flow and browser pure-half
well tested — but the pure/ImGui split left C1 exactly where no test looks
(moving the action result into AssetBrowserState would make it assertable);
**zero tests for ShaderEditorDocument** (a headless Save round-trip would
have caught M1 immediately; ResolveParentChain cycle/missing-parent/no-base
and ConsumeResult generation-routing are all unit-testable without a device).

### 2.4 Merged disposition

| # | Finding | Sev | Fix shape |
|---|---|---|---|
| 1 | Browser panel missing `return actions` (UB) | CRITICAL | 1 line |
| 2 | Save-before-bind wipes saved params | MAJOR | guard clause |
| 3 | Errors panel hides message text (`##` placement) | MAJOR | 1 line |
| 4 | Recompile orphans param undo steps | MAJOR | small design decision |
| 5 | Created assets never enter the registry | MAJOR | registry write path |
| 6 | Post-SEH-crash DXC instance reuse | MAJOR | reset-on-crash |
| 7 | Environmental compile failures cached forever | MAJOR | skip cache store |
| 8 | LoadMaterialAsset throws on non-string name/kind | MAJOR | is_string guards |
| 9 | CommandStack::Clear leaks m_pendingGeneric | MAJOR | 1 line |
| 10-24 | MINOR set (parser junk tolerance, NaN round-trip, dump() UTF-8 throw, binding-set hash-as-identity + pinning, SetMaterial half-swap, provider drive-relative escape, maxVersions headroom, CompileNow/coalesce interplay, cache-hit identity fields, hash framing, supersede-drops-cache, Widen UTF-8, unbounded caches, def.type validation, dirty-vs-undo, shared statics x2, duplicate-open compile cancel, SwitchProject doc pass, preview-texture pinning) | MINOR | mostly 1-10 lines each |

Nine findings at MAJOR+ out of ~4,400 landed lines, none architectural:
every one is an edge-hardening or wiring bug in code the tests didn't reach
(ImGui halves, pre-bind windows, failure paths). The adversarially-traced
architecture suspects all came back clean.

---

## 3. Architecture comparison: Arcane vs Unreal vs Unity

### 3.1 Parameter model

| Aspect | Arcane (slices 1-7) | Unreal 5 | Unity |
|---|---|---|---|
| Value carrier | `MatParamValue` tagged union: type enum + `float[4]`/`Guid` | `FMaterialParameterValue`: type enum + union (MaterialTypes.h:338/379-394) — same shape, ~12 types incl. doubles (LWC), fonts, RVTs, static switches | Serialized per-type property lists in the `.mat` YAML; `MaterialPropertyBlock` for transient per-renderer values |
| Declaration source | `//@param` lines inside the snippet (parsed to `ParamDecl`) | Graph parameter nodes (UMaterialExpressionParameter) | ShaderLab `Properties {}` block — the closest existing analog to `//@param`: `_Name ("Display", Range(0,4)) = 1.0` |
| Layout vs values | `MaterialTemplate` (immutable layout, offsets, defaults blob) vs `MaterialInstance` (sparse overrides) | `FUniformExpressionSet` (ordered decls + defaults blob + CB layout, MaterialShared.h:640/741/748/756) vs `UMaterialInstance` sparse arrays (MaterialInstance.h:578/599/702) | Shader (property table) vs Material (values) vs MaterialPropertyBlock (sparse override) |
| Resolve chain | override -> parent chain -> `//@param` default, one seam (`MaterialInstance::GetParam`) | `FMaterialRenderProxy::GetParameterValue`: override map -> parent chain -> compiled default (MaterialRenderProxy.h:170; MaterialInstance.cpp:337-359) | Material Variants (2022.1+): parent + overrides list; MPB shadows material at draw time |
| GPU packing | resolve-then-memcpy into one byte buffer -> one volatile CB (b0); HLSL packing rules mirrored CPU-side | identical pack loop (MaterialUniformExpressions.cpp:1005-1041) -> one uniform buffer | one `UnityPerMaterial` cbuffer (SRP batcher REQUIRES it — same one-CB conclusion) |
| Editor metadata | `ParamMeta` beside the decl (group/tooltip/slider range) | `FMaterialParameterMetadata` beside the value — same split, adopted deliberately | Property attributes inline in ShaderLab (`[Header]`, `Range(...)`) |

**Assessment.** This is the industry-consensus shape and we hit it with ~600
lines instead of UE's tens of thousands, because we skipped the two things
that make UE's version huge: the **preshader VM** (CPU-evaluated uniform
expressions — `Time*Speed` folded on CPU per frame) and **LWC doubles**. The
consequence of skipping the preshader: any computed uniform costs GPU ALU in
the pixel shader instead of CPU. At 2D-engine scale that is the right trade;
it becomes worth revisiting only if materials grow expression-heavy uniform
math executed per-pixel. Unity made the same call (no preshader; SRP batcher
just uploads the CB).

One real structural difference vs both: **our texture params carry asset
Guids in the union** where UE stores `UTexture*` and Unity stores object
references. Guids force resolution through the Assets facade at bind time
(cached, so cost is a hash lookup) but give us serialization for free and
keep the CPU core GPU-agnostic. Good trade; noted because it means a
missing texture is only discoverable at BIND time, not load time — a
validation pass in the browser (Slice 6 polish) could pre-flight that.

### 3.2 Authoring model

- **Arcane:** engine-owned template (`fullscreen_material.hlsl`) with two
  slots; designer snippet = `//@param` decls + `float4 shade(Varyings)`.
  Text-first; the graph (Slice 9) emits into the SAME slot.
- **Unreal:** graph-first. The translator emits HLSL into
  `MaterialTemplate.ush` via named-slot substitution (FStringTemplate,
  StringTemplate.h:34/119; MaterialSourceTemplate.h:12) — the identical
  seam, fed by a ~10k-line translator instead of a designer snippet. Raw
  HLSL exists only as Custom-node islands. There is NO text-first path.
- **Unity:** Surface Shaders (BiRP) were exactly our model — user writes a
  `surf()` function + pragmas, Unity generates the full shader family. In
  SRP land Unity abandoned them for Shader Graph (graph-first) and never
  shipped the promised SRP surface-shader replacement; hand-written SRP
  shaders today mean copying ~200 lines of boilerplate per shader, which is
  widely regarded as Unity's biggest authoring regression.

**Assessment.** Our model is "Surface Shaders done right on DXC": the
template owns backend divergence (register conventions, entry points) and
the snippet owns only shading. Two judgments embedded here that both engines
validate: (a) putting the graph LAST and making it emit into the same slot
is the cheapest possible graph integration — Shader Graph and the UE
translator are both proofs that the slot seam is where a graph plugs in;
(b) refusing a raw-full-HLSL bypass (spec decision 3) keeps every material
compatible with future template evolution (Slice 8's sprite template, later
multi-backend changes) — UE enforces the same closure by construction, and
Unity's lack of it (raw ShaderLab) is precisely why SRP migrations broke the
world's shaders. The cost: power users will eventually want custom vertex
work; the spec already reserves `%{VERTEX_BODY}` as the additive escape.

The `//@param` grammar itself is more ergonomic than both comparisons for
its scope: co-located with the code it feeds (ShaderLab Properties are in a
separate block; UE params are graph objects), and reserved-name checking at
parse time is something neither engine surfaces as a parse error.

### 3.3 Compile pipeline

| Aspect | Arcane | Unreal | Unity |
|---|---|---|---|
| Process model | in-process `dxcompiler.dll`, SEH-guarded, dedicated worker thread | out-of-process ShaderCompileWorker fleet (crash isolation by process boundary); in-proc DXC wrapper underneath (D3DShaderCompilerDXC.cpp:335-346) | out-of-process UnityShaderCompiler worker processes |
| Result delivery | main-thread `Drain()` = the only NVRHI touchpoint | time-sliced game-thread `ProcessAsyncResults` (ShaderCompiler.cpp:2723/2730/2799) — same rule | async import pipeline |
| Failure UX | last-good pipeline stays bound; structured diags in-panel | same (old FMaterialResource keeps rendering) | magenta error shader on failure (worse); last shader kept during in-flight compile |
| Cache | content hash: source ⊕ args ⊕ **compiler DLL bytes**, memory tier | DDC: key includes compiler version via DLL-bytes hash (DXCWrapper.cpp:90-111) — we lifted this directly; disk + network tiers | Library/ShaderCache, content-keyed, disk tier |
| Diagnostics | Clang grammar parsed ONCE at the boundary into `ShaderDiag` records | `FShaderCompilerError` parsed with the same grammar (ShaderCore.cpp:3749-3779) | raw log lines surfaced in inspector (no click-to-jump for hand shaders) |
| Variants | none — one material = one shader (per backend target) | permutation explosion (static switches, quality levels), the dominant cost of their pipeline | `multi_compile`/`shader_feature` explosion, ditto |
| Debounce/coalesce | ~200 ms quiet window per (doc, stage); supersede-drop at drain | live-update toggle + explicit apply (MaterialEditor.cpp:3872) | reimport on save (seconds, plus domain reload) |

**Assessment.** Two deliberate departures from UE deserve scrutiny:

1. **In-process vs out-of-process.** UE pays the IPC/process-pool tax to
   survive compiler crashes and to distribute compilation. Our SEH guard
   catches the common crash class (access violation inside dxcompiler) and
   fails the job, but a truly corrupting fault (heap smash before the AV)
   could still poison the editor process — a risk UE's model structurally
   cannot have. At one-compile-at-a-time editor scale with a vendored,
   known-good DXC, in-proc is the right call, and the `ShaderCompiler` API
   (Submit/Drain over plain data) is exactly the seam an out-of-proc worker
   would slot behind if scale ever demands it. Nothing to change now; the
   escape hatch is already shaped.

2. **No permutation system** (spec non-goal). This is the single biggest
   simplification vs both engines and it is *why* the whole service fits in
   ~700 lines. The moment gameplay wants "same material, fog on/off" the
   pressure will appear; the right response at that point is UE-style static
   switches compiled as separate cache entries (the content-hash cache
   already keys on defines, so the mechanism exists — only the authoring
   surface is missing). Flagging so the eventual request is recognized as a
   designed-for extension, not scope creep.

The dual-target-always-both choice (DXIL + SPIR-V compiled eagerly per
request) is something neither engine does (both compile per-active-platform
and defer others to cook/build). At our scale it doubles a sub-100 ms cost
and buys the guarantee that a material author can never break the OTHER
backend silently — a real workflow win for a two-backend engine. Revisit
only if compile volume grows (Slice 9 graphs recompiling on every node edit).

### 3.4 Asset identity and instances

- **Guid strategy:** native JSON assets embed `"id"`; imported binaries get
  `.meta` sidecars. This is Unity's system verbatim for binaries (their
  single best infrastructure decision, and the one UE lacks — UE identity
  is package-path-based, which is why UE asset renames historically produce
  redirectors and pain). Embedding the id in native JSON instead of a
  sidecar-for-everything is a small improvement over Unity for diff
  locality; the cost (an id field in every asset) is trivial.
- **Instances as assets:** `.armat` with `parent` + sparse self-typed
  overrides = UE's `UMaterialInstanceConstant` and Unity's Material
  Variants. The **self-typed on-disk values** decision (forced by wanting
  instance files loadable without their parent) matches Unity's YAML
  (values carry their type) and diverges from our own slice-5 format —
  the clean break was correct given zero legacy files.
- **Chains:** we support arbitrary-depth instance-of-instance with cycle
  detection; UE supports the same; Unity Variants also chain. Parity.
- **The params-only instance editor with override checkboxes** is UE's
  MaterialInstanceEditor model (MaterialInstanceEditor.h:31-292) including
  the show-only-overridden filter. Unity's variant inspector shows bold
  overrides + revert context menu — same concept, checkbox is the clearer UX.

**Gap vs both:** neither reference-fixup-on-rename nor a "what uses this
material?" reverse index exists yet (spec non-goal). Unity solves this with
the global GUID database; UE with the asset registry's dependency map. Our
AssetRegistry::All() is the seed of the same database; when rename lands it
should grow dependency tracking rather than us building a separate thing.

### 3.5 Editor shell

- **4-panel anatomy** (source/preview/params/errors) matches UE's material
  editor layout (MaterialEditorModes.cpp:20-84) with the axis inverted:
  HLSL-first, graph later. Unity has no in-app shader editing at all —
  the external-IDE + reimport + domain-reload loop is measured in tens of
  seconds; ours is sub-second. For hand-written shaders this is the
  clearest competitive edge of the whole arc.
- **Edit-buffer vs apply:** UE separates working copy from applied asset
  with a pre-apply error guard (UpdateOriginalMaterial,
  MaterialEditor.cpp:2767-2868). We collapsed this to dirty-document +
  error-guarded save — simpler and adequate because our preview renders the
  WORKING copy (UE's guard exists because Apply propagates to a live scene).
  When Slice 8 puts materials on live sprites, revisit: an erroring working
  copy must not propagate to scene sprites, which falls out naturally if
  scene sprites render from the SAVED asset, not the document buffer. Worth
  stating as a Slice-8 invariant now.
- **Undo:** one CommandStack for component edits AND material params (via
  the new generic `Push`) matches UE's single transaction buffer. Unity's
  Undo.RecordObject is also unified. Parity, at 1/100th the machinery.
- **Document lifecycle** (unsaved-close confirm, focus-not-reopen) is table
  stakes both engines have; we now have it as a reusable seam
  (DocumentHost) rather than a shader-editor special.

### 3.6 Runtime binding (present state, pre-Slice-8)

`FullscreenMaterialPass` mirrors TonemapPass conventions: volatile CBs
(b0 material / b1 globals), SRV table, fb-info-keyed pipeline cache. Two
observations vs the comparisons:

1. **One shared sampler (s0, linear-wrap) for all material textures.** UE
   exposes per-texture sampler settings; Unity generates sampler-per-texture
   with `sampler_`/`SamplerState` sharing conventions. Ours is an MVP
   simplification a designer WILL hit (point-sampled pixel-art noise vs
   linear gradient in one material). Cheap forward path: a `[point]`/
   `[clamp]` annotation in `//@param texture` emitting extra sampler slots —
   grammar and stitcher are already shaped for it.
2. **GlobalParams (Time/DeltaTime/ViewportSize) at b1** = UE's
   MaterialParameterCollection (one shared CB, MaterialParameterCollection.h:78)
   and Unity's built-in `_Time`/`unity_` globals. All three converge; ours is
   the minimal viable set and the struct/template lockstep comment is the
   maintenance seam to watch.

### 3.7 What we skipped, and whether the comparisons say we were right

| Skipped (spec §7) | UE has it | Unity has it | Verdict at Arcane's scale |
|---|---|---|---|
| Preshader/uniform-expression VM | yes | no | right to skip (Unity agrees) |
| Static-switch permutations | yes | yes | right for now; cache already keys defines — recognized extension |
| Material layers | yes | no | right to skip |
| Out-of-proc compile workers | yes | yes | right for now; API seam already shaped for it |
| Disk-tier compile cache | yes (DDC) | yes | worth doing cheaply later (bytes already content-keyed) |
| Instruction/cost stats | yes | yes (compiled-shader inspector) | defer; DXC reflection strip is the known path |
| Text->graph decompile | no | no | correctly rejected — nobody ships it |
| Per-texture samplers | yes | yes | the one skip a DESIGNER will feel; cheap grammar extension |

## 4. Test coverage assessment (merged)

The pattern across all three reports is consistent: **happy paths and pure
logic are well covered; the bugs live exactly where the coverage strategy
doesn't look.** Specifically:

- The pure/ImGui split (deliberate, ConsoleBuffer-pattern) worked for the
  close-flow and browser queries, but the one CRITICAL sits in the ImGui
  half the split leaves untested. Cheap structural remedy: route panel
  ACTIONS through pure state (AssetBrowserState) so they become assertable.
- **ShaderEditorDocument has zero tests** despite most of its risk being
  headless-testable: Save round-trip (catches M1), ResolveParentChain
  (cycle/missing/no-base), ConsumeResult generation routing, dirty-flag
  semantics.
- The compile service's headline contract — last-good stays bound while a
  NEWER compile fails — is untested, as are the async cache-hit path,
  failure memoization, and Shutdown-with-in-flight-jobs.
- No GPU test exercises a TEXTURE param (t-table arm, white fallback, Guid
  resolution inside Render); no float2-straddle offset-agreement GPU test.
- `CommandStack::Push` has zero tests (would have caught the Clear() leak).
- Parser negative-space (missing `=`, non-finite, trailing junk) untested.

## 5. Recommendations before Slice 8

1. **Fix-first (pre-desk-verify):** C1 (missing return — UB feeding a
   dialog trigger), M2 (errors panel message — desk-verify of the error loop
   is meaningless without it), M1 (save-before-bind data loss).
2. **Fix in the same pass (behavior bugs a designer hits):** M4 (registry
   write path for created assets — the New Instance flow is broken without
   it), the two compile-service MAJORs (post-crash instance reuse,
   environmental-failure caching), LoadMaterialAsset throw guards,
   CommandStack::Clear leak, the shared statics (m2/m3), dirty-vs-undo (m1).
3. **Decide M3** (undo survival across recompile): recommend doc-identity
   commands (weak handle to the document, forwarded to its current instance
   by nameHash) — it also fixes m1 naturally if Dirty() derives from
   EffectiveSerial vs saved serial.
4. Add the missing headless tests from Section 4 WITH the fixes (they are
   the regression net for exactly these bug classes).
5. Desk-verify the slice 4-7 UI only after 1-2 land.
6. Adopt the Slice-8 invariant from 3.5: scene sprites render from the
   SAVED asset, never the document's working copy.
7. Consider the `//@param texture` sampler-annotation extension while the
   grammar is young (3.6); and move kMaterialCbSlot/kGlobalCbSlot into
   ShaderConventions.hpp (the self-declared source of truth) while touching
   the area.
