# F2c — Mesh Import (glTF) — Design

**Date:** 2026-09-10
**Status:** Approved (brainstorm run same day; every ruling below user-decided in-session)
**Research:** `docs/research/2026-09-10-f2c-mesh-import-research.md` (@853b6ccd, incl. the
UE/Unity/Source 2 cook-spine addendum) — all file:line anchors below verified there
against `main` @ `35cf5a91`. Prior law this spec builds on:
`docs/specs/2026-08-22-f2a-scene-3d-vocabulary-design.md` (the reserved seam, the unit
rule, the material-scalar ruling), `docs/specs/2026-09-04-f2b-asset-cook-and-bindless-design.md`
(the artifact/cook contract), `docs/research/2026-08-22-mesh-asset-ue-source2-comparison.md`
(A1/A2 amendments; named-slots deferral "to F2c or not at all" — F2c is now).

---

## 1. What this is

F2c makes a dropped `.gltf`/`.glb` file render: registry discovery → cook to a mesh
artifact → `MeshSource::Imported` resolution → resident GPU geometry → per-section
draws with minted materials — fulfilling F2a's reserved seam verbatim: *"an imported
mesh becomes another MeshSource plus an artifact reference, with no component and no
scene change"* (`MeshAsset.hpp:23-24`).

glTF-only, deliberately: the format's conventions match Arcane's exactly (meters,
right-handed +Y-up, CCW — research §2), so the importer contains **zero** axis, unit,
or winding conversion. Libraries: **cgltf** (parse; 1.15 + the CVE-2026-32845 patch,
pinned by commit) and **meshoptimizer** (v1.2; remap/optimize; its simplification and
tangent generation stay dormant but vendored).

## 2. Scope and non-goals

**In scope:** registry recognition of `.gltf`/`.glb` + `.meta` sidecar; the companion
`.arcmesh` mint; the generalized cook spine (kind dispatch) + the mesh artifact kind;
`MeshImporter` (parse → bake → optimize); the slot array with named slots; material
minting (reuse-by-name, instance-of-parent); `MeshSource::Imported` resolution; the
resident mesh-buffer cache with byte-budget LRU eviction; cook-completion invalidation;
per-section draws; browser derived-fold presence + harvested thumbnails; the fixture
corpus, both golden lanes, and the vendoring.

**Non-goals — each recorded with its trigger, none improvised back in:**

| Out | Trigger to revisit |
|---|---|
| Skinning, animation, morph targets | A consumer exists (no renderer/scene support today) |
| Draco (`KHR_draco_mesh_compression`) | Rejected outright — heavy dep duplicating meshopt compression |
| `KHR_texture_basisu` | F2b's Basis/KTX2 rejection reads across (WASM/web firming up) |
| LOD / simplification | First asset needing distance LODs (`meshopt_simplifyWithUpdate` is vendored, waiting) |
| Tangents + normal maps | Reserved artifact section tag; T1 owns BC5; meshoptimizer v1.2 generates when wanted |
| `EXT_meshopt_compression` decode | First real gltfpack asset — slice one REFUSES loudly, naming the extension; decoder already vendored, flip is cheap |
| 16-bit indices | Perf-only; all plumbing is unconditionally u32 today (`MeshNode.cpp:972-973`) |
| Per-mesh file splitting | Future `MeshMetaSettings` option (granularity ruling R1) |
| Per-section `MeshRenderer` overrides | `materialOverride` stays scalar and, when set, repaints ALL sections; per-section override arrays wait for a real need |
| Normalize-to-unit import option | REJECTED, not deferred — the unit rule's two-spellings rationale is satisfied by authored meters (§4.3) |

**ABI:** bumps once (tail-append), for the `Assets`-facade mesh seams (§5.6). The Gacha
Game-DLL rebuild debt deepens by one, recorded, not nagged about.

## 3. Rulings ledger (user-decided 2026-09-10, in-session)

| # | Question | Ruling |
|---|---|---|
| R1 | Import granularity | **One asset per file.** Node TRS baked into vertices; each glTF primitive becomes a SECTION. Per-mesh split = future `.meta` option. |
| R2 | Residency & eviction | **Resident GPU cache + budget LRU** — one cache owns CPU+GPU lifetime for ALL meshes (primitives included); the per-frame ring path for mesh geometry retires. |
| R3 | Slot identity | **Named slots** (the glTF material name); re-import re-associates by name, position is the tiebreak for unnamed/duplicates. |
| R4 | Materials at import | **UE shape** (verified in the dump — `FbxImportUI.h:188-194`, `FbxMaterialImport.cpp:571/624-689`): reuse-by-name first, else mint an INSTANCE of a shared import parent; textures always extracted; unmapped inputs drop with one WARN naming them. |
| R5 | Thumbnails | **Editor harvest** via the material-harvester architecture; the artifact's thumbnail section tag stays reserved, unwritten. |
| R6 | Cook spine | **Generalize the spine, per-kind leaves** — backed by the unanimous UE/Unity/Source 2 comparison (research §7). Recorded fallback: if generalization forces awkward signatures, generalize only the store/index and keep sessions separate. |

## 4. Import contract

### 4.1 Registry

`.gltf` and `.glb` join `IsImportedBinary` (`AssetRegistry.cpp:34-38`); drop-discovery
auto-mints the `.meta` sidecar exactly like textures (guid + `MeshMetaSettings`). The
source registers under a new **`AssetKind::Model`** (browser rail "Models") — distinct
from `Mesh` for the same reason Texture and Sprite are distinct kinds; the editor's
kind-icon/hue tables gain one row (an editor-vocabulary addition the plan details
against the asset-manager spec's §11.2 value conventions).

### 4.2 The companion `.arcmesh`

Beside the source, the editor mints a companion `.arcmesh` — the
`MintOrReuseSpriteForTexture` lineage (`EditorAppProject.cpp` companion-mint precedent):

- `source: "imported"` — `MeshSource::Imported = 5`, appended, never reordering the
  persisted ordinals (`MeshAsset.hpp:51-58`). An older engine build reading such a file
  gets today's tolerant posture: one `ARC_WARN` + Cube fallback, visible not fatal.
- New field `Guid importedSource` — the registered `.gltf`/`.glb`'s guid. Resolution is
  source-guid → cooked artifact via the store, the texture lookup shape.
- The mint happens **after the first successful cook** (that is when authoritative slot
  names exist). Re-cooks reconcile slots by name: new names appended, existing guid
  assignments kept, assignments whose names vanished kept-but-WARNed (name-keyed and
  harmless; deleting user work over a re-export hiccup is worse).
- In the browser, the companion folds under its source as a **derived child** — the
  sprite-under-texture `foldedUnder` machinery — backed by a `DerivesFrom` edge in the
  reference index. The Graph panel renders the import web with zero new code.

### 4.3 Geometry semantics

- **Bake:** every node's TRS bakes into vertices; normals via inverse-transpose; a
  negative-determinant node flips its triangles' winding so the bake never mirrors a
  face inside-out.
- **Unit rule:** authored meter scale kept VERBATIM; `Transform.scale` stays 1. The
  asset still solely expresses shape — now at authored size — so the two-spellings
  rationale holds without a normalize option (rejected, §2).
- **Sections:** one per glTF primitive: `{name, indexOffset, indexCount}` (name = the
  primitive's material name, empty when absent). `MeshData` grows `sections[]`;
  a primitive-source mesh is one unnamed section — F2a files read forward losslessly.

### 4.4 The slot array (the F2a scalar grows)

`MeshAssetData::material` (scalar Guid, `MeshAsset.hpp:89`) is **retired into**
`slots: [{name, material}]`. The tolerant loader maps the legacy `"material"` key to a
single unnamed slot, so every existing `.arcmesh` loads unchanged. The growth touches
the four recorded sites (research §3.2): the asset struct + JSON, `MeshEntry`
(`SceneResources.hpp:161-166`), the resolution chain — now **per-section**:
component `materialOverride` (scalar; repaints all sections when set) → the section's
slot guid → white — and the reference classifier (`Assets.cpp:960-965`), which walks
every slot guid as a `References` edge.

### 4.5 Validation

`cgltf_validate` is **mandatory** (untrusted input; the CVE patch rides the vendor). A
file that parses but yields nothing drawable (no meshes, all-degenerate) **refuses
loudly at cook** with a diagnostic — never a silent empty artifact (the never-fabricate
discipline applied to geometry). `EXT_meshopt_compression`-compressed buffers refuse
loudly naming the extension (§2).

## 5. Cook integration

### 5.1 The generalized spine (R6)

- `CookSession` grows a kind table — per kind: source-extension set, settings loader,
  importer entry point, artifact writer — and iterates it where
  `EnumerateTextureSources` hardcodes `.png` (`CookSession.cpp:49-66`).
- `ArtifactStore::RebuildIndexFromScan` (`ArtifactStore.cpp:150-179`) switches to
  reading the **kind-agnostic header prefix** (magic, `artifactVersion`, `contentKind`,
  `sourceGuid` — laid out before any texture field by F2b's own design) for Guid
  recovery; it never needs kind knowledge.
- `arccook` iterates kinds under its existing CLI/`--check`/summary contract; Jenkins'
  `arccook --check` covers the mesh kind with no pipeline change.
- **Texture behavior stays byte-identical throughout** — its suites and both golden
  lanes are the regression net for the spine surgery.

### 5.2 The mesh artifact (`ContentKind::Mesh = 2`)

Same `'ARCA'` container disciplines (byte-explicit writes — never a struct memcpy —
versioned, tagged section table with skip-unknown forward-compat,
`ArtifactFormat.hpp:10-13, 309-313`). After the common prefix: a mesh header —
`vertexCount` (u32), `indexCount` (u32), `sectionCount` (u32), AABB min/max (6×f32) —
then sections:

| Tag | Contents |
|---|---|
| `VertexData` | Interleaved pos/normal/uv, the pipeline's 32-byte stride (`MeshNode.cpp:207-222`) |
| `IndexData` | u32 indices |
| `SectionTable` | Per section: name (u16 length + UTF-8) + index offset/count |
| `Tangents` | **Reserved, unwritten** (additive later per skip-unknown) |
| `Thumbnail` | **Reserved, unwritten** (R5 harvests editor-side) |

A mesh artifact gets its own writer/reader pair, and ArcaneClient gets its own
independent reader mirror, per F2b's deliberate-reimplementation rule
(`ArtifactFormat.hpp:15-21`).

### 5.3 `MeshImporter`

cgltf parse + validate → bake (§4.3) → meshoptimizer remap/dedupe (indexing) →
vertex-cache + vertex-fetch optimization. Deterministic for fixed input — pinned by a
byte-identity test (§9). Shape mirrors `TextureImporter.hpp:42-55`.

### 5.4 Cook key

The F2b triple, explicit-field hashing (`CookKey.hpp:3-10`):
`hash(source bytes + MeshMetaSettings fields + mesh importer version)`. **For `.gltf`
with external buffers, "source bytes" covers the `.gltf` file PLUS every referenced
external `.bin` buffer** — editing a buffer must re-cook. (External *images* are their
own registered texture assets and cook separately; they never enter the mesh key.)
v1 `MeshMetaSettings` carries **no user knobs** — guid + settings version only — but
the empty settings block still hashes, so the first future knob changes keys honestly.

### 5.5 Division of labor

`arccook` turns sources into artifacts; the **editor** mints sources. `.glb` embedded
textures extract at editor discovery time (loose `.png`s beside the source, becoming
ordinary registered textures on the texture cook path); the companion `.arcmesh` and
materials mint editor-side. A headless `arccook` run on a never-opened project cooks
geometry artifacts only — correct, not a gap, and exactly how texture `.meta` minting
already behaves.

### 5.6 Refusal, pending, and the engine seams

Same `ArtifactRefusal` vocabulary through the generalized reader; the `Assets` facade
gains the mesh-artifact accessor, `InvalidateMeshArtifact`, and the pending-probe
consult on `Missing` — tail-appended after the existing seams (the F2b/asset-manager
append discipline). This is the ABI bump.

## 6. Materials at import (R4)

- **The shared parent:** first import mints `Content/mesh_import_base.arcmat`
  (`MaterialSurface::Mesh`, `baseColor` white, `albedo` nil) at that fixed path; every
  later import finds and reuses it. Arcane's parent + sparse-overrides chain
  (`MaterialAsset.hpp` — the 08-22 research's A1: it IS `UMaterialInstance`) does the
  rest with zero new machinery.
- **Per glTF material, in order:** (1) reuse-by-name — registry lookup for an existing
  mesh-material asset matching the glTF material name; found ⇒ the slot points at it,
  nothing is created. (2) Else mint an instance: parent = the import base, overrides =
  `baseColorFactor`→`baseColor`, `baseColorTexture`→`albedo` (the extracted/registered
  texture's guid), named after the glTF material, suffixed on collision.
- **Invariant: import never overwrites an existing material asset.** Re-export/re-cook
  touches artifacts, never `.arcmat` files; user edits to minted materials are
  permanent.
- **Dropped inputs, named loudly** — one editor-side import WARN per file, per
  material: metallic/roughness (factors + texture), normal, occlusion, emissive,
  `COLOR_0` vertex colors (fixed vertex layout), double-sided + alpha modes (backface
  cull and opaque are baked into the mesh path, `MeshNode.hpp:92-98`), glTF sampler
  settings (one immutable trilinear sampler by F2b design, `MeshNode.hpp:414-430`).
  Nothing stored-but-unread; the reserved artifact tags are the future's additive path.
- **Create-invariant note (pre-empting review):** these are companion mints in the
  `MintOrReuseSpriteForTexture` lineage — registry/import-time automation. The "no
  creation path may bypass `CreateAssetRequest`" invariant governs the user dialog path
  and is untouched.

## 7. Runtime

### 7.1 Resolution

`BuildMeshData`'s `Imported` arm (`MeshAsset.cpp:216-235` dispatch) resolves the
artifact by `importedSource` via the `Assets` facade, decodes sections into
`MeshData{vertices, indices, sections}`, and takes the **artifact's stored AABB**
(cooked from the same vertices; `ComputeMeshBounds` stays for primitives). Refusal
posture, §13-flavored for geometry: **pending → draw nothing quietly** (a placeholder
cube would fabricate a shape); **missing/refused → draw nothing loudly** (diagnostic).
Never render an unknown as a cube.

### 7.2 Residency (R2) — `NriMeshBufferCache`

Render-side, mirroring `NriTextureCache`'s architecture and lifecycle hooks (device
loss/recreate included): keyed by guid; entry = CPU `MeshData` + resident GPU
vertex/index buffers + byte sizes + last-drawn frame. `MeshNode::Record` consults it
instead of the per-frame transient ring (`MeshNode.cpp:857-973` today) — **for all
meshes, primitives included**; the ring path for mesh geometry retires, and with it the
per-frame re-upload cliff the research flagged. Upload once on first draw; the CPU copy
is kept (re-upload after eviction/device recreate; editor reads) and counted in the
budget.

**Eviction — the answer F2a assigned to F2c by name:** one combined CPU+GPU byte
budget, compile-time constant for now (`kMeshResidencyBudgetBytes = 512 MiB` — a cvar
when the parked cvar arc lands; its spec's trigger discipline applies). LRU at frame
boundaries: least-recently-drawn evicted on over-budget; entries drawn this frame are
never evicted.

### 7.3 Invalidation

Mirrors the texture family wire-for-wire: mesh cook completion (`CookResult::
cookedGuids`) → `Assets::InvalidateMeshArtifact` → residency entry dropped (CPU+GPU) →
resolver re-request. Two changes stay distinguished: an **artifact** change drops
geometry; a **slot reassignment** in `.arcmesh` rides the existing `InvalidateMesh`
path (`SceneRenderResolver.cpp:245-255`) on the material side only and **keeps the
resident buffers** — re-pointing a material never re-uploads a two-million-triangle
prop. The document-preview invalidation gap is **re-accepted by the same F2b ruling**
(viewport-only; heals on close/reopen).

### 7.4 Draws

Per-section submission: one draw per section with its resolved slot material.
`MeshInstance` already carries the bindless material slot per draw, so the 128-byte
zero-headroom `MeshConstants` block (`mesh.hlsl:20-28`) is untouched; a multi-section
prop issues `sections.size()` draws.

## 8. Editor surface

- **Browser:** source `Model` row with the companion `.arcmesh` folded under it
  (§4.2); minted materials are ordinary standalone assets with `References` edges to
  their textures.
- **Thumbnails (R5):** extend the material-harvester architecture — offscreen lit
  render framed by the artifact AABB, persisted `Saved/Thumbnails/<guid>.png`,
  visible-first LIFO queue, one harvest per frame with the device-idle discipline the
  asset-manager arc shipped.
- **F2c/F4 line:** this arc ships drop-to-cook-to-render + browser presence + thumbs.
  The dedicated Import dialog, import-options UI, and re-import button UX are
  F4/post-F2c (the F2b closing record already parks "asset-creation UX unification"
  there). Slice-one workflow: drop the file; discovery + auto-cook-on-change does the
  rest.

## 9. Testing and verification

- **Fixture corpus** (checked in, tiny): single-mesh `.glb`; multi-primitive `.glb`
  with multiple named materials; nested transforms; a negative-scale node;
  embedded-texture `.glb`; **a `.gltf` + external `.bin` pair** (exercises §5.4's
  buffer hashing — editing the `.bin` must change the cook key); degenerate/empty;
  malformed sparse accessor (pins the CVE-hardened validate path — the fixture is a
  truncated/overflowing index bound, not a working exploit).
- **Tests:** importer unit tests over the corpus (bake correctness incl. the winding
  flip, section extraction, slot names); artifact write→read round-trip on BOTH reader
  implementations; cook-key stability (+ the external-`.bin` coverage of §5.4);
  **determinism** — same input, byte-identical artifact, twice; registry discovery +
  companion-mint reconciliation (name re-association, vanished-name keep-and-WARN);
  material minting (reuse-by-name, no-overwrite invariant); residency cache — device-
  less logic split + `[gpu]`-tagged buffer/eviction tests per the `NriTextureCache`
  coverage pattern; refusal postures (pending-quiet vs missing-loud).
- **End-to-end net:** ReferenceProject gains **one imported-mesh fixture in the golden
  scene**, so both golden lanes (dx12 + vulkan) exercise drop-to-pixels every CI run.
  One re-bless cycle when it lands — Arc 2 discipline (bless SOURCE, restage BOTH
  hosts, JSON-only verdicts, archive diffs before re-runs).
- Suites run from the exe dir; count deltas attributed per task, as always.

## 10. Vendoring

`ThirdParty/cgltf/` (1.15 + the CVE-2026-32845 patch from jkuhlmann/cgltf#293, pinned
by commit — never the bare tag) and `ThirdParty/meshoptimizer/` (v1.2 tag, pinned) —
bc7enc_rdo pattern: curated file subsets, in-dir LICENSE, `NOTICE.md` +
`ThirdParty/README.md` rows, small premake StaticLib (meshoptimizer) / header +
implementation-TU (cgltf, stb pattern). **Consumed by ArcaneAssetPipeline only — never
ArcaneClient** (the 08-21 placement rule).

## 11. Relationship to prior specs

- **F2a** (`2026-08-22-...-design.md`): the reserved seam is fulfilled as written; the
  material scalar grows into slots per its own "additive" ruling (§4.4 maps the legacy
  key); the unit rule extends to imports by keeping authored meters; the vertices-
  derived AABB path is consumed as predicted. F2a's deferred eviction question is
  answered (§7.2).
- **F2b** (`2026-09-04-...-design.md`): the artifact container, cook-key, refusal, and
  pending-probe disciplines extend to a second kind; the store layout is unchanged
  (one directory, kind-dispatched index rebuild); the document-preview gap ruling is
  re-accepted, not re-litigated.
- **08-22 research:** named material slots — deferred there "to F2c or not at all" —
  ship now (R3); LODs and mesh groups stay out (§2 triggers).
- Where this spec is silent, F2a/F2b stand; where they disagree on mesh import, this
  spec wins.

## 12. Hazards ledger (from the research, each addressed)

| Hazard | Where addressed |
|---|---|
| Per-frame ring re-upload cliff for real polycounts | §7.2 — residency cache; ring path retires for mesh geometry |
| Texture-coupled cook machinery fails closed on kind 2 | §5.1/§5.2 — spine generalized; per-kind reader/writer pairs |
| `RebuildIndexFromScan` reads texture headers to recover Guids | §5.1 — kind-agnostic prefix read |
| glTF PBR inputs with no destination | §6 — drop-with-WARN; reserved tags are the additive future |
| Slot assignments shuffled by re-export | R3/§4.2 — named slots, name re-association, keep-and-WARN |
| External `.bin` edits not re-cooking | §5.4 — buffers hashed into the cook key |
| Untrusted input (CVE class) | §4.5/§10 — mandatory validate + pinned patch + malformed fixture |
| 2D/3D +Y sense confusion | §1 note via research §2 — both paths correct; stated once so nobody "fixes" it |
| Eviction question left half-answered | §7.2 — budget LRU now; cvar later per the parked cvar arc's own trigger |
