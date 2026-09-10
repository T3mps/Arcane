# F2c mesh import — pre-spec research

**2026-09-10.** Research pass ahead of the F2c (mesh import: glTF via cgltf +
meshoptimizer) brainstorm/spec, run against Arcane `main` @ `35cf5a91`. Three
strands: the in-repo seam survey (every anchor below verified at that commit),
the external library landscape (web, September 2026), and the prior research
this extends — `docs/research/2026-08-22-mesh-asset-ue-source2-comparison.md`
(UE/Source 2 mesh-asset comparison; its A1/A2 amendments are law in the F2a
spec) and `docs/research/2026-08-21-asset-cook-pipeline-design.md` (the cook
contract F2b implemented).

---

## 1. External libraries

### cgltf — parse

- **Latest release: v1.15 (2025-02-09).** C99, single-file, MIT. Loader AND
  writer; broad KHR/EXT extension coverage (1.15 added
  KHR_materials_diffuse_transmission, EXT_texture_webp, sampler enums,
  `cgltf_find_accessor`).
- **CVE-2026-32845 (CVSS 6.9, disclosed 2026): integer overflow in
  `cgltf_validate()`'s sparse-accessor validation** — unchecked arithmetic
  lets a crafted file cause a heap over-read in `cgltf_calc_index_bound()`
  (DoS / info disclosure). Affects ≤ 1.15; the fix lives in maintainer PR
  jkuhlmann/cgltf#293, unreleased as of this writing. **Vendoring guidance:
  pin 1.15 + that patch (or post-merge master), by commit — never the bare
  release tag** — and treat `cgltf_validate` as required, not optional, since
  imported files are untrusted input by definition.

### meshoptimizer — everything after parse

- **Latest: v1.2** (the project graduated from its long 0.x line to 1.x; the
  README's own install instruction pins `-b v1.2`). MIT.
- Coverage relevant to F2c, all in the one dependency: indexing/remap
  (deduplication), vertex-cache/overdraw/fetch optimization, simplification
  (2026's `meshopt_simplifyWithUpdate` + `meshopt_SimplifyPermissive` are
  aimed exactly at higher-quality LOD chains), `EXT_meshopt_compression`
  encode/decode, clusterization/meshlets, and — new in the 1.x line —
  **tangent-frame generation**: "a MikkTSpace-like construction but by
  default uses a modified weighting scheme that significantly improves
  tangent quality around beveled regions."
- **That last item changes a standing assumption.** The 2026-08-21 cook
  research sequenced "T1 (needs BC + MikkTSpace) → mesh arc" on the premise
  that tangent generation required vendoring MikkTSpace separately
  (`2026-08-21-asset-cook-pipeline-design.md:200-203`). meshoptimizer v1.x
  covers it. A separate mikktspace vendor is now needed only if bit-exact
  MikkTSpace conformance is required (it matters for baked-normal-map
  interchange with external tools; glTF's spec language asks for MikkTSpace
  when tangents are absent). See §6 Q6.

Sources: [cgltf releases](https://github.com/jkuhlmann/cgltf/releases) ·
[cgltf](https://github.com/jkuhlmann/cgltf) ·
[CVE-2026-32845 advisory](https://www.vulncheck.com/advisories/jkuhlmann-cgltf-sparse-accessor-validation-integer-overflow) ·
[meshoptimizer](https://github.com/zeux/meshoptimizer) ·
[meshoptimizer releases](https://github.com/zeux/meshoptimizer/releases).
(Release dates for meshoptimizer's 1.x tags came back inconsistent across
fetches; the version and feature set are confirmed from the repo's README.
Pin the exact tag/commit at vendoring time.)

---

## 2. Convention fit: glTF is a near-perfect match

| Convention | Arcane (anchor) | glTF 2.0 | Import work |
|---|---|---|---|
| Units | Meters/MKS (`MeshBuilder.hpp:35-36`) | Meters | **None** |
| Handedness / up | Right-handed, +Y up, camera looks −Z (`SceneCamera.hpp:123-137`, `MeshBuilder.hpp:22-36`) | Right-handed, +Y up, −Z forward | **None** |
| Winding | CCW front faces, `frontCounterClockwise=true` + backface cull (`MeshBuilder.hpp:21-33`, `MeshNode.hpp:92-98`) | CCW | **None** |
| Vertex attributes | `MeshVertex{pos, normal, uv}` (`MeshBuilder.hpp:47-52`) | POSITION/NORMAL/TEXCOORD_0 core | Direct; extras are §6 questions |

No axis flip, no unit conversion, no winding reversal. (An FBX-family
importer would need all three — one more reason glTF-only is the right first
slice.) **One landmine to state in the spec even though it is not a 3D-path
problem:** the same `Transform` component feeds the 2D sprite path, whose
convention is screen +Y DOWN ("the renderer applies no Y-flip" tooltip,
`Components.hpp:303`) — the 3D path is +Y up. Both readings are correct in
their own paths; the spec should say so once so nobody "fixes" it.

---

## 3. The seams, as they exist at 35cf5a91

### 3.1 `.arcmesh` and `MeshSource` — the designed entry point

- `MeshSource` (`MeshAsset.hpp:51-58`) is an explicitly-numbered persisted
  enum `{Plane=0, Cube=1, UvSphere=2, Cylinder=3, Capsule=4}`; F2c appends
  (`Imported=5`), never reorders. `MeshAssetData` is a flat tagged struct
  (not a variant), every field always written so source-switching is
  lossless (`MeshAsset.cpp:72-81`).
- The reserved seam, verbatim (`MeshAsset.hpp:23-24`): *"F2c's SEAM: an
  imported mesh becomes another MeshSource plus an artifact reference, with
  no component and no scene change."*
- Forward-compat already behaves well: an unknown `source` string parses
  with one `ARC_WARN` and falls back to `Cube` — which is exactly what an
  older engine build will do with a future `"imported"` file
  (`MeshAsset.cpp` load path). Not a crash; a visible wrong-shape.
- `BuildMeshData` (`MeshAsset.cpp:216-235`) is a pure switch dispatch; the
  `Imported` arm resolves an artifact instead of generating.

### 3.2 Material slot — scalar today, and the array growth is a real schema move

- `MeshAssetData::material` is a single `Guid` (`MeshAsset.hpp:89`), by
  F2a's recorded ruling (*"F2c's imported multi-section meshes grow this
  into a slot array, which is ADDITIVE"*, `MeshAsset.hpp:84-88`).
- "Additive" is true at the component (nothing moves), but the growth
  touches **four sites**: `MeshAssetData` + the JSON shape,
  `MeshEntry::material` (`SceneResources.hpp:161-166`), the
  override→default→white resolution chain in `MeshSubmissionSystem`, and
  the asset-reference classifier that walks `.arcmesh`'s `"material"` key
  as a References edge (`Assets.cpp:960-965`). Named slots (UE's
  `MaterialSlotName`/`ImportedMaterialSlotName` re-association machinery)
  were explicitly deferred "to F2c or not at all" by the 08-22 research —
  the spec must now decide.

### 3.3 Cooking a mesh is wholly new ground, not a branch-add

- The only generic hook is the artifact header's
  `ContentKind{Texture=1}` byte, reserved with the comment "F2c's mesh
  artifacts discriminate on this same header field"
  (`ArtifactFormat.hpp:62-65`). Everything downstream is texture-specific
  and **fails closed** on any other kind (`ArtifactFormat.cpp:220-223`).
- Needed as parallel machinery, mirroring the texture family's disciplines
  rather than branching into it: a mesh artifact struct +
  `WriteMeshArtifact`/`ReadMeshArtifact` (byte-explicit, no memcpy —
  `ArtifactFormat.hpp:10-13`); `MeshImporter` (shape:
  `TextureImporter.hpp:42-55`); `MeshMetaSettings` (tolerant-JSON +
  explicit-field cook-key hashing — the "struct padding RNG" trap,
  `CookKey.hpp:6-10`); a client-side reader mirror (ArcaneClient's
  `ArtifactReader` is a *deliberate* independent reimplementation of the
  byte contract — `ArtifactFormat.hpp:15-21` — and a mesh artifact gets its
  own by the same rule).
- Generalization pressure points: `EnumerateTextureSources` hardcodes
  `.png`+`.meta` (`CookSession.cpp:49-66`); `ArtifactStore::
  RebuildIndexFromScan` calls `ReadTextureArtifact` directly to recover
  Guids (`ArtifactStore.cpp:150-179`); `arccook`'s own banner says
  "textures today". The premake breadcrumb already anticipates the growth:
  "texture now, mesh in F2c" (`premake5.lua:174`).
- Cook-state plumbing (pending probe, refusal enum, invalidation-on-cook)
  is texture-purpose-built (`Assets.hpp:200-291`,
  `ArtifactRefusal` in `ArtifactReader.hpp:173`); the mesh side inherits
  the *shape* (artifact accessor + `InvalidateX` + pending probe) but every
  wire is new. There is no mesh-cook-completion invalidation path today
  because nothing cooks meshes.
- The artifact thumbnail is a texture-shaped ≤64px RGBA raster
  (`ArtifactFormat.hpp:95-96`); a mesh artifact's analog (rendered
  turntable thumb? none?) is undecided — the asset-manager panels resolve
  thumbs from artifacts, so "none" means kind-icon fallback in the Browser.

### 3.4 Geometry lifecycle — the two questions F2a deliberately left for F2c

1. **No persistent GPU residency exists.** `MeshNode::Record` uploads every
   visible mesh's vertex/index streams through the frame's transient upload
   ring every frame (`MeshNode.cpp:857-973`, dedupe is per-frame only,
   `MeshNode.hpp:497-528`). Fine for unit primitives; an imported
   100k-triangle mesh would re-upload ~3 MB per frame per mesh. F2c must
   either add a resident vertex/index buffer cache (the `NriTextureCache`
   analog) or consciously accept ring pressure for a slice.
2. **CPU-side `MeshCache` has no eviction, by recorded intent** (F2a spec
   §"limits": *"F2c's imported meshes are what make eviction a real
   question, and they should be what answers it"*). Invalidation today is
   editor-save-driven only (`SceneRenderResolver::InvalidateMesh`,
   `SceneRenderResolver.cpp:245-255`); wholesale drop on project switch.
   The known deferred document-preview invalidation gap (F2b spec §
   deferred: *"Document-preview NriGraphContext instances are not
   invalidated on cook completion... Not fixed this arc (ruled)"*) will
   apply to mesh cooking identically.

### 3.5 Material model — the mapping gap

- `MaterialSurface::Mesh` is params-only and never compiled
  (`MaterialSource.hpp:64-71`; `ARC_ENSURE`-refused in template/binding/
  snippet generation). Exactly two parameters exist: `baseColor` (vec4) and
  `albedo` (texture Guid → one bindless slot, capacity 256, one sampler —
  `SceneResources.hpp:188-235`, `MeshNode.hpp:414-430`).
- `mesh.hlsl` is *deliberately* Lambert+ambient — "a decision, not a
  placeholder" (`mesh.hlsl:1-7`) — and F2a's spec explicitly deleted
  guessed-ahead PBR params.
- A glTF PBR metallic-roughness material therefore maps as:
  `baseColorFactor`→`baseColor`, `baseColorTexture`→`albedo`, and
  **everything else (metallic/roughness factors + texture, normalTexture,
  occlusion, emissive) has no destination.** Extending the shading model is
  a renderer arc, not an importer concern; the spec must pick a policy for
  the unmapped remainder (§6 Q4). Constants headroom is zero
  (`MeshConstants` is exactly 128 bytes, the Vulkan push-constant floor —
  `mesh.hlsl:20-28`), so even one more bindless slot field is a real cost.

### 3.6 Fixed plumbing an importer inherits as-is

- **Vertex layout is pipeline-baked**: one interleaved 32-byte stream
  (pos/normal/uv), one binding (`MeshNode.cpp:207-222`). Tangents/UV1/color
  are vertex-input + pipeline + shader changes, not importer additions.
- **Tangents are consumed nowhere** — `VSInput` is POSITION/NORMAL/
  TEXCOORD0 (`mesh.hlsl:87-92`); `MeshBuilder.hpp:11-15` refuses tangent
  generation by name ("needs MikkTSpace, a vendored library this arc
  refuses").
- **Indices are 32-bit only, unconditionally** (`MeshBuilder.hpp:57`,
  `nri::IndexType::UINT32` hardcoded at `MeshNode.cpp:972-973`).
- **AABB path is ready**: `ComputeMeshBounds` is vertices-derived
  specifically "because F2c's imported meshes need that path regardless"
  (`MeshBuilder.hpp:83-98`); computed once per `MeshCache::Request`, stored
  on `MeshEntry::bounds`, read today only by the mesh document's preview
  framing. Reused unchanged.

### 3.7 Registry + vendoring

- `.gltf`/`.glb` are **absent** from `IsImportedBinary`'s extension list
  (`AssetRegistry.cpp:34-38`) — a dropped glTF is invisible to the registry
  today. The texture pattern to mirror: drop-discovery + auto-minted `.meta`
  sidecar (`AssetRegistry.cpp:30-32, 96-125`).
- Vendoring model = `ThirdParty/bc7enc_rdo/` (curated file subset, LICENSE
  in-dir, small premake StaticLib, NOTICE.md + ThirdParty/README.md rows) or
  `ThirdParty/stb` for header-only; consumption belongs in
  **ArcaneAssetPipeline, never ArcaneClient** (the 08-21 research's
  placement rule; neither library exists anywhere in the tree yet —
  grep-confirmed).

### 3.8 The F2c / F4 boundary

Today's Create▸Mesh **mints** a default unit cube (`MintMeshAsset`,
`EditorAppProject.cpp:1152-1175`) — there is no import flow of any kind.
The line consistent with how F2a/F2b parked UX: **F2c = plumbing** (registry
recognition of `.gltf`/`.glb` + sidecar settings, the mesh cook kind and
artifact, `MeshSource::Imported` resolution, drop-to-cook-to-render working
with hand-editable `.arcmesh`), **F4/post-F2c = the UX** (a dedicated
Import-Mesh dialog, browser polish — the F2b closing record already parks
"asset-creation UX unification" post-F2c).

---

## 4. What the prior research already settled (do not re-litigate)

From `2026-08-22-mesh-asset-ue-source2-comparison.md`: no size on the asset
(unanimous across UE/S2/F2a); bounds computed, not stored; material default
on the asset with per-component override (shipped as the scalar chain);
LODs, mesh groups, and named material slots are import-era concerns deferred
*to* F2c. From the F2b contract: Basis/KTX2 transcoding rejected (revisit
only if WASM/web firms up) — which reads across to rejecting
`KHR_texture_basisu` on import; textures referenced by an imported material
go through the existing `.png`+`.meta` cook path, not a new one.

---

## 5. Scope observations (for the brainstorm, not conclusions)

- **Draco (`KHR_draco_mesh_compression`): reject** — a heavy C++ dependency
  duplicating what `EXT_meshopt_compression` does with a library we already
  vendor; gltfpack-produced files are the meshopt flavor anyway.
- **`EXT_meshopt_compression`: cheap to accept at parse time** (cgltf parses
  it; meshoptimizer decodes it), but slice-scope is a spec call.
- **Skinning/animation/morph targets: out** — no consumer exists anywhere in
  the renderer or scene model; classic YAGNI.
- **A `.glb`'s embedded textures** raise a contract question: extract to
  loose `.png` sources at import (keeping the one texture cook path) vs.
  cook-from-embedded (a second texture enumeration path). The F2b
  architecture strongly favors extraction.

---

## 6. What the F2c spec must settle (the payoff list)

1. **Import granularity.** A glTF file is a scene: many meshes, many nodes,
   a transform hierarchy. One `.arcmesh` per glTF *mesh*? Per file with
   section merging? Is the node hierarchy flattened (bake node transforms
   into vertices) or discarded? (The reserved seam — "no component and no
   scene change" — rules out spawning entity hierarchies at import; the
   granularity within that rule is open.)
2. **The unit rule for imports.** Primitives are unit-geometry by rule;
   glTF geometry arrives at authored meter scale, which the rule's
   two-spellings rationale actually permits (Transform.scale stays 1).
   State the policy explicitly; decide whether a normalize-on-import option
   exists at all.
3. **Multi-section meshes and the slot array** — including whether named
   slots (re-import re-association) ship now or the array is positional;
   §3.2 lists the four touch sites.
4. **Unmapped glTF material inputs** (metallic/roughness/normal/occlusion/
   emissive): drop-with-WARN, or store-in-artifact-unread for a future
   shading arc? (Extending `mesh.hlsl` is explicitly out of F2c's scope by
   F2a's anti-guess-ahead precedent; the artifact's skip-unknown-sections
   table means "add later, additively" is genuinely available.)
5. **GPU residency** (§3.4-1): resident vertex/index cache now, or accept
   per-frame ring uploads for the slice? Paired with **CPU eviction**
   (§3.4-2), which F2a's spec assigns to F2c by name.
6. **Tangents** (§3.6 + §1): nothing consumes them; meshoptimizer v1.x can
   generate them without a separate MikkTSpace vendor. Import-and-store
   glTF TANGENT? Generate? Or defer entirely (cheapest — the artifact
   section table makes later addition additive)? This also resolves the
   stale "T1 before mesh arc" sequencing note in the 08-21 research —
   normal-map cooking (BC5) remains T1's, but tangent generation no longer
   forces T1 first.
7. **Cook infrastructure shape**: parallel mesh-cook path vs. generalizing
   `CookSession`/`ArtifactStore`/`arccook` to kind-dispatch (the
   `RebuildIndexFromScan` Guid-recovery coupling is the forcing point).
8. **Cook-completion invalidation + pending probe for meshes** — the wiring
   family that doesn't exist yet (§3.3), including whether the known
   document-preview gap is accepted again by the same ruling.
9. **Index width**: stay 32-bit-only (matches all current plumbing) or add
   a 16-bit path (new plumbing end-to-end; perf-only benefit).
10. **Mesh artifact preview**: no thumb (kind icon in the Browser) vs. a
    rendered thumbnail section; decide with the asset-manager's thumb
    resolution seam in mind.
11. **Registry contract**: `.gltf` + `.glb` into `IsImportedBinary`, the
    `MeshMetaSettings` sidecar fields (and therefore the cook key), and the
    `.glb` embedded-texture extraction policy (§5).
12. **Vendoring pins**: cgltf 1.15+CVE-fix by commit; meshoptimizer v1.x
    exact tag; curated-subset file lists per the bc7enc_rdo pattern, both
    into ArcaneAssetPipeline.
