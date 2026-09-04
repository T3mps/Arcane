# F2b — the asset cook (texture slice) and the bindless material table

**Status:** design, 2026-09-04. Approved in brainstorming; implementation plan to follow.

**Provenance.** F2b of the 3D foundations pivot (decided 2026-08-21). Three inputs bind it:

1. `docs/research/2026-08-21-asset-cook-pipeline-design.md` (@59e18b84) — the cook contract.
   Every decision there stands; this spec instantiates its first slice and does not reopen it.
2. `docs/plans/2026-08-21-nri-phase4-3d-slice.md` §Task 8 (@933f3555) — the bindless material
   table, planned in full and never built (`BindlessTable` exists only in `MeshNode.hpp:23,191`
   comments). F2b executes it.
3. `docs/specs/2026-08-22-f2a-scene-3d-vocabulary-design.md` — F2a's closing ruling (plan
   @22424f6f) moved three editor items here: `"mesh"`-kind `.arcmat` authoring, the
   `albedo`-declared-but-unbound memoized diagnostic (spec `:235`, `:242-244`, `:430`), and the
   snippet/graph-ignored-with-one-diagnostic. The no-`tint` ruling's product validation rides on
   the first.

**The target.** Per the standing directive (2026-09-04), every section is compared against the
Source 2 / Deadlock render target (`docs/research/2026-08-12-deadlock-render-target.md`) and
states match or deliberate divergence. Reference-code policy is unchanged: NO leaked Source 2
source, ever; ValveResourceFormat (MIT) is the legal reference — its `.vtex_c` parser is the
legitimate record of the target's compiled-texture vocabulary.

**UE, read first-hand (2026-09-04).** The target is Source 2, but UE source is the one we can
actually read (`.example/UnrealEngine-release/`), and this spec was reviewed against it: every
`UE:` citation below was verified in that dump, selection-not-declaration discipline applied
(the BC7 finding below is exactly the trap — the module that registers BC7 is not the one that
runs).

**After F2b:** F2c (mesh import — cgltf + meshoptimizer) per the F2a sequencing ruling.

---

## 1. Why

Arcane has layers 1 (sources) and 2 (authored JSON) of the universal three-layer asset split
and no layer 3. The runtime stb-decodes PNGs at load — an interchange parser in the runtime,
which every mature engine forbids. The renderer target's T3 (144-entry box-projected cubemap
array, ~2.7 GB RGBA16F vs ~340 MB BC6H) is unreachable without an offline compile step. And
F2a shipped mesh materials whose `albedo` field is declared with no meaning — Task 8 is the
consumer that field was reserved for.

F2b closes all three at once, with each half proving the other: the cook produces compiled
textures; bindless consumes them; the editor authors the material that binds them. The arc
ends with a textured cube on screen whose albedo travelled source → `.arcart` → BC7 upload →
bindless slot → `mesh.hlsl`, authored end-to-end in the editor.

## 2. Scope — user rulings, 2026-09-03/04

| Ruling | Choice |
|---|---|
| Arc contents | ALL THREE clusters: cook texture slice + Task 8 bindless + F2a's inherited editor trio (+ no-`tint` validation) |
| Sprite path | FULL migration — all content textures cook; two parallel texture routes are forbidden (the directional rule) |
| Sequencing | One spine, consumers in dependency order: cook core → runtime route proven by the sprite cutover → bindless proven by cooked albedo → editor layer as the product |

## 3. Architecture

Three new build artifacts, per the cook contract §9:

```
ArcaneAssetPipeline (static lib)    importers, .arcart read+write, hash-flat store,
                                    cook orchestration. stb_image + bc7enc_rdo live HERE.
        |-- ArcaneEditor            in-process cook on JobSystem workers
        \-- arccook.exe             thin CLI over the same lib
ArcaneClient                        gains ONE reader for our own .arcart binary;
                                    never links the pipeline, never parses interchange
```

- `arccook` surface, this slice: `arccook --project <dir>` (cook everything stale),
  `--check` (exit nonzero if anything would cook — the CI gate), `--verbose`,
  `--dump-dds <guid>` (debug export for external viewers; DDS is never a load path).
- Premake: static-lib and console-exe patterns per `premake5.lua:110-112` (ArcaneCore) and
  `:292-294` (ArcaneServer). ArcaneTests links the lib (it is host-free) and hosts its unit
  tests; ArcaneTests globs its own `src/**` so tests need no premake edit.
- **Target comparison:** the lib/CLI split is `resourcecompiler.exe` with the editor linked
  in-process — Source 2's shape exactly. Divergence: none in this section.

## 4. The `.arcart` texture artifact

Binary, little-endian, versioned:

```
[Header]   magic 'ARCA', artifactVersion, contentKind (Texture), sourceGuid,
           sourceHash, importerVersion, pixelFormat (BC7|RGBA8; BC5/BC6H reserved),
           width, height, mipCount, sRGB flag, section offsets/sizes
[MipTable] per-mip: offset, size, width, height
[Payload]  BC blocks (or raw RGBA8), mip-major, tightly packed
[Thumb]    uncompressed RGBA mip, <=64 px long edge — the editor-preview route
```

- Self-describing (contract §4): the header alone rebuilds the index by directory scan.
- Deterministic (contract §6): pinned encoder settings, no timestamps, fixed field order.
  Cook-twice-byte-compare is a unit test, not an aspiration. UE treats texture builds as
  expected-but-not-guaranteed deterministic and merely instruments the gap
  (`TextureBuildUtilities.h:53-58` pre-encode mip digests); we pin it outright — achievable
  because we are single-platform, single-encoder (UE's own named nondeterminism is
  cross-architecture, `TextureDerivedData.cpp:508-515`). One UE trap adopted verbatim: hash
  explicit fields, never struct memory — "struct padding RNG" makes whole-struct hashes
  nondeterministic (`TextureCompressorModule.cpp:3828-3880`).
- Atomic writes (contract §5): `<hash>.tmp` + rename. Non-negotiable.
- Store: `Intermediate/Artifacts/<hh>/<hash>.arcart`; `Intermediate/artifacts.index` is a
  rebuildable cache keyed Guid → hash. Cache key is the triple
  `hash(source bytes + import settings + importer version)`. The importer-version term
  EXPLICITLY covers the encoder and resize-library versions, not just our own importer code —
  UE keys the exact same way: a global recook GUID (`TextureDerivedData.cpp:68`), a per-format
  encoder version mixed in separately (`:434`, `:469`), and even stb-resize's own version
  string (`TEXTURE_DDC_STB_IMAGE_RESIZE_VERSION "2.06"`, `:79`). Bump any one constituent and
  affected artifacts recook; slice one carries a single composite constant with the
  constituents named in its comment.
- **Mip policy, modeled on UE first-hand:** mips are generated in LINEAR space — decode,
  linearize sRGB, downsample in float, re-encode on write (UE hard-asserts RGBA32F + linear
  at the mip loop, `TextureCompressorModule.cpp:1817`, linearize trigger `:4518-4522`). The
  default filter is a plain 2×2 box average — UE's actual default (`TextureLODSettings.cpp:
  333-353`, kernel-2 short-circuit `TextureCompressorModule.cpp:196-204`); kaiser-by-default
  is folklore. Sharpen/blur kernels are out of scope. Mip dims respect BC's 4×4 blocks
  (standard padding of the encoded mip, never of the source).
- Format policy, this slice: **BC7 default** (sRGB-aware; albedo and sprites), **RGBA8** as a
  `.meta` opt-out for content BC visibly harms (pixel art, UI atlases). BC5 arrives with T1
  (normal maps + MikkTSpace), BC6H with T3 (HDR cubemaps) — enum values reserved now so the
  format field never renumbers.
- `.meta` (today `{guid, version}`, parsed at `AssetRegistry.cpp:99-119`) gains a typed
  `texture` block: `format` (auto|bc7|rgba8), `srgb`, `generateMips`, `maxSize`. Absent block
  = defaults; an untouched `.meta` cooks sensibly. Growth shape, decided now so the knobs
  never churn: when T1 needs normal maps, `format` grows toward UE's SEMANTIC-INTENT enum
  (`TC_Default/Normalmap/Masks/HDR/...`, `TextureDefines.h:375-394`) rather than raw format
  names — the intent picks the format. UE's essential build-settings core is eight-ish knobs
  (compression setting, sRGB, mip-gen, max size, LOD group/bias, POT mode, alpha forcing)
  atop an ~80-field `FTextureBuildSettings` (`TextureCompressorModule.h:95-300`); our four
  are the honest subset of that core, and the ceiling to grow into is the core, never the 80.
- **Target comparison:** `.vtex_c` is the analog; VRF's parser is the legal reference for what
  the target records per compiled texture (format enum, mip dims, sRGB, array layout — read
  before pinning the header in the plan). Divergence, already ruled in the contract: hash-flat
  storage over Source 2's path mirroring, for staleness.

## 5. Runtime route and the sprite cutover

**Target comparison: this section IS the pillar — "the runtime reads `_c` only."** No
divergence beyond storage layout.

Seams, verified in source 2026-09-03:

- **`Assets` splits info from pixels.** New `Assets::TextureInfoFor(Guid)` serves true
  dims/format/mipCount from the artifact header. `SpriteCache` — which reads `PixelsFor` for
  dimensions only (`SpriteCache.cpp:102`) — retargets to it. `PixelsFor` survives with its
  contract narrowed to PREVIEW pixels (the thumbnail), keeping every preview call site
  (`SpriteDocument.cpp:288-292`, `ShaderEditorDocument.cpp:1507`) working unmodified. The
  thumbnail serving a caller that needs true resolution is the bug class this split prevents.
- **`NriTextureCache` learns compiled textures.** Today: RGBA8 upload via `PixelSupplyFn`
  (`NriTextureCache.cpp:190` CreateCommittedTexture, `:219` UploadData, `:231` view). Gains:
  BC7_SRGB/BC7_UNORM (+RGBA8 passthrough) and multi-mip upload driven by the artifact's mip
  table. The supply seam becomes artifact-shaped, wired at the same two host sites the pixel
  supply is wired today (`EditorApp.cpp:1739-1743`, `RuntimeApp.cpp:542`). Downstream is
  untouched: `Batch2DNode` still receives an `nri::Descriptor*` (`Batch2DNode.cpp:726-734`),
  now viewing a BC texture with mips. The cache KEEPS a raw-RGBA supply path for
  `ColorSpace::Display` / chrome images (`NriTextureCache.hpp:90`) — engine-internal images
  never cook, and the plan's grounding pass verifies the chrome upload route before Task 1.
- **Refusal semantics** (contract §8): missing artifact, source-hash mismatch, or importer
  version newer than the engine → loud memoized failure — red in the editor's Problems pane,
  fatal at runtime load. Plugin-ABI-gate philosophy verbatim. Failure memoization follows the
  existing decode-failure precedent (`Assets.cpp:259-272`).
- **stb stays for exactly two things, stated so nobody "finishes the job" later:**
  1. The verify/compare oracle — reference load `RuntimeApp.cpp:592` and
     `EditorAppFrame.cpp:252`, diff write `RuntimeApp.cpp:1084`, bless write
     `ReferenceImages.cpp:85`. Reference images are the acceptance oracle: human-viewable
     PNGs blessed from captures, committed to git, NOT project content. They never cook.
  2. Editor chrome — `LoadDisplayPixels` (`Assets.cpp:505-545`) and the window icon
     (`Window.cpp:188`). Engine-internal images, not project content.
  Content-texture interchange parsing leaves `ArcaneClient` entirely (`Assets.cpp:634`'s
  `LoadPngRgba` content route dies; the symbol survives for the oracle).

## 6. The bindless material table — Phase 4 Task 8, executed

**Target comparison, and this is the arc's one deliberate divergence: Deadlock ships on
DX11-class per-draw binding — the target has no bindless.** We diverge because Arcane's floor
is D3D12 + Vulkan only and parity is the FEATURE SET above the RHI, never the API technique.
The target's feature set is descriptor-indexing-shaped at scale: clustered deferred with
per-cluster light/probe lists, and T3's 144-entry box-projected cubemap array is an indexed
texture array fed by per-cluster indices. Per-draw binding built now would be torn out at T3;
bindless is the idiomatic D3D12/VK spelling of the capability the target's scale demands.

- `BindlessTable` lands with the Phase 4 plan's interface verbatim (Create/Add/Release/
  `kInvalidSlot`, capacity-0 refused, dense slots from 0, full → `kInvalidSlot` + one warn),
  gated on `NriDeviceCaps::SupportsBindless()`; tier 0 refuses loudly at node creation.
- `mesh.hlsl` gains the per-instance material index and samples the bindless array by it.
- Slot lifecycle rides the existing Graveyard fence model (`Release(Graveyard&, fence)`).
- Feed: mesh-material resolution binds a non-nil `albedo` Guid → `NriTextureCache` resolves
  the cooked artifact → the SRV is Added to the table → the instance carries the slot. A nil
  `albedo` keeps F2a's flat-color path; the F2a diagnostic (declared-but-unbound) is replaced
  by the real binding and retires.
- Proof, in two layers: the plan's four-cube `[gpu][pixel]` case survives verbatim (four
  GENERATED textures, each cube's centre pixel dominated by its own channel — a binding bug
  that ignores the index fails all four), PLUS the F2b integration case: one cube whose albedo
  is a COOKED BC7 artifact resolved through the cache into a slot — the moment the arc's two
  halves prove each other.
- Sprites stay on per-sprite descriptor sets (`Batch2DNode.cpp:737-804`). Batcher2D bindless
  is not in scope.
- **UE, first-hand — validations and two noted deltas.** UE gates bindless on real device
  queries (SM 6.6 AND `RESOURCE_BINDING_TIER_3`, `D3D12Adapter.cpp:1077-1080`) and refuses
  LOUDLY with a logged reason when unsupported (`:1160-1167`) — our tier-0 refusal is the
  same doctrine. Its handles are flat with NO generation field (`RHIDefinitions.h:1347-1355`);
  safety comes from fence-deferred release (delete-queue drained behind a GPU sync point,
  `D3D12RHI.cpp:396-424`) — exactly our Graveyard model, validating `Release(Graveyard&,
  fence)`. Its bindless and descriptor-table PSOs coexist per-frame, branched on a
  root-signature bit (`D3D12StateCache.cpp:452-459`) — the shape of our sprites-on-sets
  choice. Deltas noted, not adopted at this scale: UE's allocator is a coalescing free-range
  list and its resource heap GROWS by doubling, fatal only at the hardware ceiling
  (`D3D12BindlessDescriptors.cpp:182-222`, default capacity 1,000,000); our fixed capacity +
  `kInvalidSlot` + one warn is right for a slice-one table, and UE's growth shape is the
  documented path if capacity ever bites.

## 7. Editor layer

**Target comparison: matches Source 2's tooling model outright** — assets compile behind the
tools; the user never runs the compiler by hand. No divergence.

- **Background cook seam.** The JobSystem exposes only `ParallelFor` today
  (`TaskExecutor.hpp:25-27`); the never-block ergonomic needs a small engine-level
  fire-and-forget `Submit` API (enkiTS supports unwaited task sets; thin exposure, engine-side
  per the directional rule). The pipeline's cook queue rides it: watcher event (the existing
  `last_write_time` watcher triggers; the hash decides) → cook job → thumbnail/table refresh.
- **Ergonomics held to the contract's list verbatim:** drop a `.png` → appears immediately,
  cooks in background, thumbnail resolves when ready; import-settings change → that one asset
  recooks and live-reloads; failures land red in the Problems pane, clickable; **no "Reimport
  All" button, ever** — if one becomes necessary the hash key is wrong; orphan sweep at
  project open (inert by hash-addressing).
- **While a cook is in flight, the scene shows a PLACEHOLDER — modeled on UE first-hand.**
  UE serves a built-in stand-in while a texture compiles (checkerboard/typed defaults chosen
  per usage, `Texture2D.cpp:237-268`; the live resource is constructed OVER the default's
  resource at `Texture2D.cpp:1053-1068`) and swaps the real one in on completion via the
  ordinary update path (`TextureCompiler.cpp:210-240`). Ours: `NriTextureCache` serves a
  built-in checkerboard for a Guid whose cook is pending, distinct from the refusal state
  (failed/missing = red diagnostic, pending = placeholder), and completion swaps in through
  the existing invalidate path. In-progress and broken must never look alike.
- **Capture and bless WAIT for pending cooks.** UE blocks its save path on outstanding
  compiles so no artifact-bearing output embeds a placeholder (`Texture.cpp:1463-1467`,
  `ObjectTools.cpp:5576-5581` blocks before caching a thumbnail). Same rule here: the
  editor's verify/capture/bless paths refuse-or-wait while any content cook is in flight —
  a golden blessed from a checkerboard is the failure mode this buys out. (Gate runs never
  hit this: staged trees are cooked post-build, §8.)
- **The thumbnail's consumer this arc: the inspector texture preview** — the exact spot
  `InspectorView.cpp:287-293` reserved ("PixelsFor is the supply"). The asset-browser
  thumbnail GRID is out (a UI arc of its own; the browser keeps glyph icons).
- **F2a's inherited trio:** the surface picker and `CreateMaterialAt` learn `"mesh"`; the
  params panel gets a decl path for mesh materials (albedo texture slot + scalar params); the
  snippet/graph-ignored diagnostic completes (F2a shipped only the `passes`/`baseInputs`
  half). The albedo-unbound diagnostic RETIRES rather than ships: F2a declared it interim by
  its own text ("`albedo` is declared so that Phase 4 Task 8 has a real field to give meaning
  to", F2a spec `:242-244`) — F2b IS Task 8, the trigger condition (declared-but-unbound)
  stops existing, and the §5 refusal diagnostics cover the surviving failure mode (a non-nil
  `albedo` whose artifact is missing/stale). The F2a spec lines are annotated when this lands.
- **No-`tint` validation, at the desk:** two entities, one `"mesh"` material template, two
  instances differing in colour — F2a's accepted-deferred desk item 7, finally run.

## 8. Staging, CI, setup, ABI

- **Staging:** the source `ReferenceProject/Intermediate/Artifacts` is cooked ONCE by a
  post-build `arccook` step; `{COPYDIR}` stages `Intermediate/Artifacts` beside `Content`
  into both hosts' trees. One cook, staged everywhere, idempotent by hash; the additive-
  staging trap stays harmless because hash-flat orphans are inert by design.
- **One re-bless, mid-arc, planned:** BC7 + mips change sprite renders, and the inspector
  preview may shift editor-ui. References re-blessed once at the sprite-cutover task and
  RESTAGED TO BOTH HOSTS (the arc-2 ledger trap: a bless the gate never sees).
- **ABI:** the `Assets` facade is pure-virtual; `TextureInfoFor` moves the vtable → plugin
  ABI 20 → 21, comment-block entry in the established voice, both `.arcproj` restamped, game
  modules rebuilt (an ABI bump strands every module — F2a lesson).
- **CI:** pipeline unit tests are CPU-only and ride `~[gpu]` on both lanes free; Jenkins'
  gate exercises cooked staging automatically; `arccook --check` joins the Jenkins build as
  the nothing-stale gate. Fresh-clone flow stays `GenerateProjects → build → run` — the
  post-build cook keeps it true.
- **Gacha consequence, named:** after the SDK update, Aphelyon boots only once cooked. The
  editor heals it in-process on open; headless runs want `arccook` first (a one-line
  `scripts/setup.ps1` follow-up in the Gacha repo, not this arc).

## 9. Testing

- **Unit (CPU, `~[gpu]`):** the triple cache key including the importer-version term; store
  atomicity; index rebuild from a directory scan; cook-twice byte-determinism; refusal paths
  (missing / hash-mismatch / version-newer) each WATCHED FIRING ONCE (the Arc B standard);
  orphan sweep; `.meta` settings round-trip; BC7 payload sanity (decode a block, compare
  against source within encoder tolerance).
- **GPU:** BC7 multi-mip upload case in the texture cache; Task 8's four-cube case; the
  cooked-BC7-albedo integration case.
- **Product:** the golden gate's four lanes running entirely on cooked content are the
  end-to-end proof; then the desk checkpoint — authoring UX, the ergonomics list exercised by
  hand (drop, settings-change recook, error-to-Problems), no-`tint` validation.
- Baselines will move; re-derive at close from each run's own final line, never recall.

## 10. Out of scope, named

- **Meshes, `.arc*` JSON compiling, team DDC, paks, platform variants, cook-on-demand** —
  all per the cook contract's scope table, with its reasons.
- **Batcher2D bindless** — sprites keep descriptor sets; revisit only with a profiled need.
- **Asset-browser thumbnail grid** — a UI arc; the inspector preview is this arc's consumer.
- **BC5/BC6H encoding** — enum reserved; T1/T3 own the encoders' first use.
- **Basisu/KTX2 transcoding** — rejected in the contract; revisit only if WASM/web firms up.
- **F2c (mesh import: cgltf + meshoptimizer)** — the next arc, per the F2a ruling.

## 11. Vendoring

**`bc7enc_rdo`** (MIT) arrives this arc, in `ThirdParty/bc7enc_rdo/`, with the standing
obligations: `LICENSE` in its subdir, a `NOTICE.md` row, a `ThirdParty/README.md` inventory
row with pinned version + upstream URL. The hand-rolled DDS writer is our code (dump path
only). **Rider, one line, while `NOTICE.md` is open:** AgilitySDK — the tree's only non-OSS
dependency, flagged twice in prior records — finally gets its missing row.

**The encoder question, settled against UE first-hand:** a stock UE Windows build compresses
BC7 with **Oodle Texture** — proprietary, selected not by module priority but by ini-driven
format-NAME rewriting (`[AlternateTextureCompression]` → prefix `TFO_` →
`BuildSettings.TextureFormatName = "TFO_BC7"`, `Texture.cpp:3892-3970`,
`BaseEngine.ini:3676-3677`); Intel ISPC TexComp registers `BC7` and is never selected — the
declaration-is-not-selection trap in the wild. bc7enc does not ship in UE at all. Oodle is
not licensable-vendorable for us; ISPC TexComp is UE's open fallback but carries a real build
system. `bc7enc_rdo` stands on its own merits: MIT, drop-in compile shape (the house
vendoring rule), RDO-capable. Not a copy of anyone — a fit.

## 12. Decisions log

| Decision | Choice | Why |
|---|---|---|
| Arc contents | All three clusters | User ruling 2026-09-03; the clusters prove each other; "F2b closed" means the textured-cube product loop works |
| Sprite path | Full migration | User ruling; two texture routes is forked infrastructure; the contract's refusal philosophy ships whole |
| Sequencing | One spine, consumers in order | Phase 4's organising rule — nothing lands unrun; approach 2 (bindless-first) integrates twice, approach 3 (editor-first) builds on deleted code |
| stb carve-outs | Verify oracle + editor chrome keep stb | Reference PNGs are the acceptance oracle, not content; icons are engine-internal |
| Thumbnail consumer | Inspector preview | Cheapest real consumer; the browser grid is a UI arc |
| Bindless despite the target | Deliberate divergence | Target is DX11-class per-draw; our floor is D3D12+VK; parity = feature set above the RHI; T3's arrays are indexing-shaped |
| Staging | Cook source once post-build, COPYDIR stages artifacts | One cook, both hosts; per-staged-tree cooking multiplies work for nothing |
| ABI 21 | Bump + restamps + module rebuilds | `Assets` vtable moves; bumps are cheap during engine dev (standing directive) |
| Format default | BC7, RGBA8 opt-out | Universal on D3D12+VK; pixel-art escape hatch; BC5/BC6H reserved for their consumers |
| Re-bless | Once, mid-arc, restaged to both hosts | BC changes pixels by design; a delta from compression is expected, not a defect — but blessed deliberately, never casually |
| Mip policy | Linear-space 2×2 box, per UE first-hand | Kaiser-default is folklore; linearize-before-downsample is the correctness rule UE hard-asserts |
| Cook-pending state | Checkerboard placeholder, distinct from refusal | UE model; in-progress and broken must never look alike; capture/bless waits on pending cooks |
| Encoder | bc7enc_rdo despite UE shipping Oodle | Oodle proprietary; ISPC = nested build; bc7enc_rdo = MIT drop-in with RDO — the house vendoring shape |
| Determinism | Pinned byte-identity, stronger than UE | Single platform + single encoder makes it achievable; UE only instruments the gap |
