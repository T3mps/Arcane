# Mesh IMPORT in Unreal and Source 2 — what F2c should copy

**2026-09-10.** Reference-engine comparison pass against
`docs/specs/2026-09-10-f2c-mesh-import-design.md`, run before that spec's
implementation plan is written. Precedent and format:
`docs/research/2026-08-22-mesh-asset-ue-source2-comparison.md` (which amended its
own spec twice). **This document is a DRAFT: it proposes amendments, it does not
apply them.** The spec is untouched; the controller folds the accepted
amendments after user review.

Scope note: the 08-22 pass compared mesh **assets** (what a mesh *is*). This one
compares mesh **import** (how a file becomes one). Where the two overlap —
bounds, materials-on-the-asset — the earlier verdicts are re-confirmed, not
re-litigated.

## Sources, and the policy they obey

| Engine | Source | Provenance |
|---|---|---|
| **Unreal 5** | The local dump at `.example/UnrealEngine-release/` | Real source on disk, read file by file. Standing directive: *read the tree, never answer from memory* ([[project_arcane_outliner_arc]]). Every UE citation below is `path:line` in that dump. |
| **Source 2** | `ValveResourceFormat/ValveResourceFormat` @ `master` (MIT) | Clean-room reverse-engineered parser, fetched over the web. **No leaked Source 2 code was read, quoted, or consulted** — standing hard rule ([[project_arcane_deadlock_render_target]]). VRF line numbers are approximate (reported by the fetch), and are marked `~`. |
| **Unity** | — | **Out of this pass by instruction.** No readable source exists; the 09-10 research §7 already used its documentation for the one question (cook-spine shape) that needed it. |

UE files read: `Editor/UnrealEd/Classes/Factories/{FbxAssetImportData,
FbxMeshImportData, FbxStaticMeshImportData}.h`;
`Editor/UnrealEd/Private/Fbx/{FbxMainImport, FbxStaticMeshImport,
FbxSkeletalMeshImport, FbxStaticMeshImportData}.cpp`;
`Editor/UnrealEd/Public/FbxImporter.h`;
`Editor/UnrealEd/Classes/ThumbnailRendering/StaticMeshThumbnailRenderer.h`;
`Editor/UnrealEd/Public/ObjectTools.h`;
`Runtime/Engine/Classes/Engine/{StaticMesh,EngineTypes}.h`;
`Runtime/Engine/Public/{StaticMeshResources,RawIndexBuffer}.h`;
`Runtime/Engine/Public/Rendering/PositionVertexBuffer.h`;
`Runtime/Engine/Private/StaticMesh.cpp`;
`Runtime/Engine/Private/Streaming/StreamingManagerTexture.cpp`;
`Plugins/Interchange/Runtime/Source/Import/Private/Gltf/{InterchangeGltfTranslator,
InterchangeGltfMesh}.cpp`;
`Plugins/Interchange/Runtime/Source/Parsers/GLTFCore/Private/GLTFMeshFactory.cpp`;
`Plugins/Interchange/Runtime/Source/Pipelines/Public/{InterchangeGenericAssetsPipeline,
InterchangeGenericAssetsPipelineSharedSettings, InterchangeGenericMeshPipeline}.h`.

VRF files read: `Resource/Blocks/VBIB.cs`, `Resource/ResourceTypes/{Mesh,Model}.cs`,
`IO/Gltf/GltfModelExporter.Mesh.cs`.

**UE has two live import stacks and this pass read both.** The legacy
`UFbxFactory` path under `Editor/UnrealEd/Private/Fbx/`, and the modern
**Interchange** plugin (`Plugins/Interchange/`), which is where UE's actual glTF
importer lives. Interchange is the closer analog to F2c and is cited in
preference; the FBX path is cited where it states a rule more explicitly or
where Interchange inherits it.

---

## Decision 1 — Granularity, node bake, winding flip (spec §4.3 / R1)

Three separable claims. Two confirm, one diverges, one needs an amendment.

### 1a. Node TRS baked into vertices — **CONFIRMED**

Both UE stacks bake by default, and glTF's is the one that matters:

- Interchange's shared default is `bBakeMeshes = true`, documented *"If enabled,
  meshes are baked with the scene instance hierarchy transform"*
  (`InterchangeGenericAssetsPipelineSharedSettings.h:83-85`).
- The glTF mesh payload is produced by
  `MeshFactory.FillMeshDescription(GltfMesh, MeshGlobalTransform, &MeshDescription)`
  (`InterchangeGltfMesh.cpp:287`) — the node's global transform is an *input to
  geometry construction*, not scene data carried alongside it.
- Inside the factory, positions are transformed vertex by vertex:
  `const FVector TransformedPosition = TotalMatrix.TransformPosition(FVector(FinalPosition));`
  (`GLTFMeshFactory.cpp:361`), with an identity fast path at `:367-375`.
- Normals use the inverse-transpose, exactly as specced:
  `TotalMatrixForNormal = TotalMatrix.Inverse(); TotalMatrixForNormal = TotalMatrixForNormal.GetTransposed();`
  (`GLTFMeshFactory.cpp:456-457`; the FBX path spells the same two lines at
  `FbxStaticMeshImport.cpp:647-648`).
- The legacy FBX path agrees and defaults the same way:
  `bTransformVertexToAbsolute = true` (`FbxStaticMeshImportData.cpp:16`), whose
  property comment is *"If this option is true the node absolute transform
  (transform, offset and pivot) will be apply to the mesh vertices"*
  (`FbxMeshImportData.h:65-67`).

Source 2 cannot confirm or deny this: VRF shows that a compiled mesh's vertex
data is already in one space and that draw calls index into it, but **whether
Valve's compiler bakes authored node transforms is tool-side behavior VRF cannot
observe** — stated, not claimed.

### 1b. Negative-determinant winding flip — **CONFIRMED**, with one extra obligation

UE's glTF importer does exactly what §4.3 specifies, by the same test:

```cpp
const bool bIsMirrored = TotalMatrix.Determinant() < 0.f;   // GLTFMeshFactory.cpp:459
```

and the mirrored branch is a *duplicated triangle loop* that walks corners from
the end of the index range instead of the start
(`GLTFMeshFactory.cpp:597-605`, the comment at `:594-596` says the duplication is
for performance). The FBX path states the rule in a comment:

```cpp
// If there are odd number negative scale, invert the vertex order for triangles
int32 UnrealVertexIndex = OddNegativeScale ? 2 - VertexIndex : VertexIndex;   // FbxSkeletalMeshImport.cpp:3603-3604
```

driven by `IsOddNegativeScale`, which counts negative scale components and
returns true for 1 or 3 (`FbxMainImport.cpp:2100-2109`) — the determinant test
spelled arithmetically.

**The extra obligation the references reveal:** UE does not stop at winding. The
same flag flips the tangent-basis handedness —
`const float BinormalMultipler = bIsMirrored ? -1.f : 1.f;`
(`GLTFMeshFactory.cpp:460`), and the FBX path computes
`VertexInstanceBinormalSigns[AddedVertexInstanceId] = GetBasisDeterminantSign(...)`
(`FbxStaticMeshImport.cpp:1006`). F2c has no tangents, so this costs nothing
today — but it is a real, easily-missed second half of the mirror rule, and it
belongs recorded against the reserved `Tangents` tag now rather than discovered
during T1. See amendment **A3**.

### 1c. One asset per FILE — **DIVERGENCE-KEEP**

UE's default granularity is **one static mesh per source mesh**, not per file:
`bCombineStaticMeshes = false`, documented *"If enabled, all translated static
mesh nodes will be imported as a single static mesh"*
(`InterchangeGenericMeshPipeline.h:54-56`). The legacy FBX path defaults the
other way (`bCombineMeshes` is set true by the reimport factory,
`EditorFactories.cpp:5829`), so UE is not even internally unanimous. Source 2's
model is a *list* of meshes — `m_refMeshes` paired with `m_refLODGroupMasks`
(`Model.cs:~189-192`) plus embedded meshes (`~213-239`) — i.e. many meshes under
one model resource.

**Kept, and the justification is structural rather than aesthetic.** UE's
per-mesh default is coherent *because UE also imports the scene hierarchy as
actors*, so N meshes become N assets plus a spawned actor tree that restores
their relative placement. F2c's reserved seam explicitly forbids that half:
*"no component and no scene change"* (`MeshAsset.hpp:23-24`). Per-mesh splitting
without hierarchy reconstruction would scatter N assets that no longer assemble
into the authored object — strictly worse than one baked asset. The spec already
records per-mesh splitting as a future `MeshMetaSettings` option (§2, R1), which
is the right place for it: it becomes correct exactly when F4 gives us somewhere
to put the hierarchy.

### 1d. One section per PRIMITIVE — **AMEND**

Both references key sections on the **material**, not the primitive, and both put
an **index indirection** between a section and its material.

- UE glTF: polygon groups are created once per *used material index* —
  `for (int32 MaterialIndex : MaterialIndicesUsed) { const FPolygonGroupID& PolygonGroupID = MeshDescription->CreatePolygonGroup(); ... }`
  (`GLTFMeshFactory.cpp:379-387`), and each primitive is then filed into
  `MaterialIndexToPolygonGroupID[Primitive.MaterialIndex]`
  (`GLTFMeshFactory.cpp:444`). Two primitives sharing a material land in **one**
  group.
- UE makes the merge an explicit, defaulted policy:
  `bool bKeepSectionsSeparate = false;` — *"If checked, sections with matching
  materials are kept separate and will not get combined."*
  (`InterchangeGenericAssetsPipelineSharedSettings.h:91-93`).
- The runtime section carries an **index**, not a name:
  `struct FStaticMeshSection { /** The index of the material with which to render this section. */ int32 MaterialIndex; uint32 FirstIndex; uint32 NumTriangles; uint32 MinVertexIndex; uint32 MaxVertexIndex; ... }`
  (`StaticMeshResources.h:201-209`).
- Source 2 carries the material **on the draw call itself**:
  `drawCall.GetStringProperty("m_material") ?? drawCall.GetStringProperty("m_pMaterial")`
  (`GltfModelExporter.Mesh.cs:~278-279`), alongside `m_nStartIndex`,
  `m_nIndexCount`, `m_nBaseVertex` (`~354-356`).

The spec as written derives a section's material **positionally from the slot
array** ("the section's slot guid", §4.4) while naming sections after their
material ("name = the primitive's material name", §4.3). Those two rules
contradict each other the moment two primitives share one glTF material: the
slot array then contains two entries with the *same* name, R3's name-keyed
re-association becomes ambiguous, and the user is asked to assign one material
twice. This is not hypothetical — it is the ordinary shape of an exported prop
(one metal material, five separate mesh parts). See amendment **A1**.

---

## Decision 2 — Import transform knobs (spec §5.4: v1 `MeshMetaSettings` has none)

**Verdict: DIVERGENCE-KEEP.**

UE has the knobs, in both stacks, and has had them for a decade:

```cpp
FVector  ImportTranslation;      // FbxAssetImportData.h:29-30
FRotator ImportRotation;         // FbxAssetImportData.h:32-33
float    ImportUniformScale;     // FbxAssetImportData.h:35-36
```
defaulted `(0)`, `(0)`, `(1.0f)` (`FbxAssetImportData.cpp:18-20`); Interchange
carries them forward as `ImportOffsetTranslation` / `ImportOffsetRotation` /
`ImportOffsetUniformScale = 1.0f`, *"applied to meshes and animations"*
(`InterchangeGenericAssetsPipeline.h:74-84`).

So the surface answer is "the first knob everyone needs is scale". **But the
references also show *why* UE needs it, and the reason does not read across.**
UE's scale plumbing exists first and foremost as a **unit conversion**, applied
unconditionally and *not* as a user knob:

```cpp
MeshFactory.SetUniformScale(GltfUnitConversionMultiplier); // GLTF is in meters while UE is in centimeters
```
(`InterchangeGltfMesh.cpp:112` and `:286`; node translations get the same
treatment via `ScaleNodeTranslations(..., GltfUnitConversionMultiplier)`,
`InterchangeGltfTranslator.cpp:606`). The FBX path spells the same need as
`bConvertSceneUnit` — *"Convert the scene from FBX unit to UE unit
(centimeter)"* (`FbxAssetImportData.h:50-52`) — alongside `bConvertScene` and
`bForceFrontXAxis` for handedness (`:42-48`).

Arcane's multiplier is **1.0 and its axis conversion is identity**, because
glTF's conventions are Arcane's conventions (research §2: meters, right-handed,
+Y up, CCW). The single most-used import knob in UE is a fix for a mismatch F2c
does not have. That is exactly the "zero conversion" claim the spec makes in §1,
and it is what makes a no-knobs v1 defensible rather than merely lazy.

The spec has also already paid the forward cost honestly: §5.4 hashes the empty
settings block, *"so the first future knob changes keys honestly."* No amendment.

---

## Decision 3 — Named slots and re-import re-association (spec §4.4 / R3)

**Verdict: CONFIRMED.** This is the strongest match in the pass — F2c's R3
reproduces UE's mechanism nearly field for field, and the 08-22 doc's deferral
("to F2c or not at all") resolves correctly.

The two names, and the comment that states the purpose:

```cpp
/*This name should be use by the gameplay to avoid error if the skeletal mesh Materials array topology change*/
FName MaterialSlotName;            // StaticMesh.h:510-511
/*This name should be use when we re-import a skeletal mesh so we can order the Materials array like it should be*/
FName ImportedMaterialSlotName;    // StaticMesh.h:513-515
```
with a first-class lookup: `int32 GetMaterialIndexFromImportedMaterialSlotName(FName ImportedMaterialSlotName) const;`
(`StaticMesh.h:1978`).

Re-import behaviour, read in the source rather than inferred
(`FbxStaticMeshImport.cpp:1954-1974`):

- match candidates against existing slots **by name**:
  `if (StaticMeshMaterial.ImportedMaterialSlotName == CandidateMaterial.ImportedMaterialSlotName) { FoundExistingMaterial = true; break; }`
- **append only when unmatched**: `if (!FoundExistingMaterial) { StaticMesh->GetStaticMaterials().Add(CandidateMaterial); }`
- **never remove**: there is no deletion arm. A slot whose imported name vanished
  from the source simply stays.
- **position is the tiebreak**: when the name lookup fails,
  `MaterialIndex = PolygonGroupID.GetValue();` (`:1946-1948`).

That is §4.2's reconciliation rule verbatim — *"new names appended, existing
guid assignments kept, assignments whose names vanished kept-but-WARNed"* — plus
R3's *"position is the tiebreak for unnamed/duplicates"*.

**And the slot name is the glTF material name**, which UE arranges deliberately.
The factory initially names each polygon group after the material *index*
(`const FName ImportedSlotName(*FString::FromInt(MaterialIndex));`,
`GLTFMeshFactory.cpp:385-386`), and a dedicated pass then renames them:

```cpp
// Patch polygon groups material slot names to match Interchange expectations (rename material slots from indices to material names)
...
StaticMeshAttributes.GetPolygonGroupMaterialSlotNames()[MaterialSlotIndex] = *MaterialName;
```
(`InterchangeGltfMesh.cpp:54-71`). UE explicitly moves *away* from positional
identity toward the glTF material name — the exact move R3 makes.

Source 2 confirms the association-per-draw-call half but takes the positional
route for skins: each draw call names its own material
(`GltfModelExporter.Mesh.cs:~278-279`), while `m_materialGroups`, each
`{ m_name, m_materials[] }` (`Model.cs:~410-411`), overrides them **by position
within the group**. Named container, positional contents — weaker than UE's and
weaker than ours.

One honest gap: the UE path read here **keeps an orphaned slot silently**; no
warning is emitted in `FbxStaticMeshImport.cpp:1954-1974`. UE instead surfaces
material-order conflicts through a separate interactive dialog
(`Editor/UnrealEd/Private/Fbx/FbxMaterialConflictWindow.cpp` exists; its
behaviour was not read). F2c's keep-and-WARN sits between the two and is a
strict improvement over silence. No amendment.

---

## Decision 4 — Artifact payload shape (spec §5.2)

Three sub-questions with three different answers.

### 4a. Interleaved fixed 32-byte pos/normal/uv — **DIVERGENCE-KEEP**

Neither reference bakes a fixed layout into its cooked payload.

**UE de-interleaves**, and does so on purpose. Static mesh render data holds
three separate streams:

```cpp
struct FStaticMeshVertexBuffers
{
    FStaticMeshVertexBuffer StaticMeshVertexBuffer;   // :322  (tangents + UVs, interleaved)
    FPositionVertexBuffer   PositionVertexBuffer;     // :325  (positions alone)
    FColorVertexBuffer      ColorVertexBuffer;        // :328
};
```
(`StaticMeshResources.h:319-328`), and the position buffer carries its own
independent stride (`GetStride()`, `PositionVertexBuffer.h:96-98,138`) — the
classic split that lets depth/shadow passes bind positions only.

**Source 2 interleaves but declares the layout as data.** VBIB's
`OnDiskBufferData` is `{ ElementCount, ElementSizeInBytes, RenderInputLayoutField[] InputLayoutFields, byte[] Data }`
(`VBIB.cs:~40-64`), with the comment *"For vertex buffers, this is the stride.
For index buffers, this is the type size"* (`~53`), and each attribute is
described by `{ SemanticName, SemanticIndex, DXGI_FORMAT Format, uint Offset,
int Slot, RenderSlotType SlotType, int InstanceStepRate }` (`~71-105`). Data is
interleaved into one buffer, but *which* attributes at *which* offsets in *which*
formats is read from the file, not assumed.

**Kept.** Arcane's mesh pipeline binds exactly one 32-byte vertex input
(`MeshNode.cpp:207-222`); a self-describing layout in the artifact would be
stored-but-unread, which §6's own discipline forbids ("Nothing stored-but-unread").
The honest consequence to record: because the artifact's `VertexData` stride is
fixed, the reserved `Tangents` tag necessarily grows as a **parallel array**
rather than a re-strided vertex block — which, as decision 5 notes, is also what
the reference layouts suggest is fine.

### 4b. Section table `{name, indexOffset, indexCount}` — **AMEND** (see 1d)

Both references' section records are the same three quantities plus one more
apiece:

| | UE `FStaticMeshSection` (`StaticMeshResources.h:201-209`) | Source 2 draw call (`GltfModelExporter.Mesh.cs:~354-356`) | F2c §5.2 |
|---|---|---|---|
| range start | `FirstIndex` | `m_nStartIndex` | `indexOffset` ✅ |
| range length | `NumTriangles` | `m_nIndexCount` | `indexCount` ✅ |
| vertex window | `MinVertexIndex` / `MaxVertexIndex` | `m_nBaseVertex` | *(absent — correct)* |
| material | `MaterialIndex` (an **index**) | `m_material` (a **path**) | *(implied by order — the defect)* |

The vertex-window fields are genuinely not ours: both engines have them because
a section may live in a *shared* or *offset* vertex buffer (S2's `ReadIndices`
literally adds `baseVertex` to every index, `~579-589`). Our artifact has one
vertex buffer per mesh with absolute indices, so `MinVertexIndex`/`m_nBaseVertex`
would be a constant zero. Confirmed absent-by-design; worth stating so a later
reader doesn't "restore" it.

The material column is the amendment. **A1.**

### 4c. u32-only indices — **DIVERGENCE-KEEP**, with one cheap hardening

Both references store an **adaptive** index width **in the cooked payload**:

- UE: `namespace EIndexBufferStride { Force16Bit = 1, Force32Bit = 2, AutoDetect = 3 }`
  with `AutoDetect` documented *"Use 16 bits unless an index exceeds MAX_uint16"*
  (`RawIndexBuffer.h:74-85`), and `FRawIndexBuffer16or32` carrying a `bool b32Bit`
  member decided by `ComputeIndexWidth()` (`:35-68`).
- Source 2: `ElementSizeInBytes` is the index type size; VRF's reader branches on
  4-byte vs 2-byte and widens on read (`GltfModelExporter.Mesh.cs:~572-589`).

The spec's §2 files 16-bit indices under "Perf-only; all plumbing is
unconditionally u32 today (`MeshNode.cpp:972-973`)". The runtime half of that is
true and the deferral is right — but the references reframe the *storage* half:
for both engines index width is a **container property**, not a perf toggle, and
both readers have always had to ask. F2c's artifact is a versioned, tagged,
skip-unknown container, so a future `IndexData16` tag is additive without a
format break — the deferral is genuinely safe.

The one cheap hardening: the mesh header already spends 12 bytes on
`vertexCount`/`indexCount`/`sectionCount`. One more byte making the width
**explicit rather than assumed** turns a future flip into a reader branch instead
of a tag-presence dance across two independent reader implementations
(engine + ArcaneClient, which F2b's rule keeps deliberately separate). Proposed
as a **minor, optional** amendment: **A5**.

---

## Decision 5 — Tangents (spec §2: deferred, reserved section tag)

**Verdict: DIVERGENCE-KEEP.** The references do not reveal artifact-format churn
beyond what the reserved tag covers — but they do reveal a *correctness* debt
that the reserved tag alone does not cover.

Both engines carry tangents in cooked mesh data, by default:

- UE generates them at build time unless told otherwise:
  `FMeshBuildSettings` constructs with
  `bUseMikkTSpace(true), bRecomputeNormals(true), bRecomputeTangents(true)`
  (`EngineTypes.h:2763-2765`; fields at `:2674-2686`). The import-side option
  exists too — `EFBXNormalGenerationMethod { BuiltIn, MikkTSpace }`, the latter
  documented *"Use MikkTSpace to generate normals and tangents"*
  (`FbxMeshImportData.h:19-29, 81-83`).
- Source 2 packs normal and tangent **together** in the vertex stream: the mesh
  parser exposes `IsCompressedNormalTangent(KVObject drawCall)` reading
  `m_bUseCompressedNormalTangent` (`Mesh.cs:~131-165`).

Neither fact argues against deferring. Both engines have normal mapping; Arcane's
mesh shader is *"deliberately Lambert+ambient — a decision, not a placeholder"*
(research §3.5, `mesh.hlsl:1-7`), so tangents today would be the definition of
stored-but-unread. The deferral is consumer-driven and stays.

**Two things the references do change, both cheap and both worth recording now:**

1. **The mirror rule has a tangent half** (decision 1b). `bIsMirrored` drives
   `BinormalMultipler = -1.f` (`GLTFMeshFactory.cpp:460`) as well as the winding
   reversal. F2c's §9 fixture corpus already contains "a negative-scale node" and
   its test asserts "the winding flip". When tangents land, that same fixture must
   also assert **handedness**, or a mirrored asset will render with inverted
   normal-map lighting while every existing test stays green. Recording the
   obligation against the reserved tag costs one sentence now and saves a
   T1-era bug hunt. **A3.**
2. **Format shape is settled by 4a, favourably.** Because our `VertexData` stride
   is pipeline-fixed, tangents arrive as their own tagged section (a parallel
   array), not as a re-strided vertex block — no rewrite of `VertexData`, no
   artifact version bump, skip-unknown does the rest. Source 2's packed
   normal+tangent is the alternative and would have forced exactly the rewrite we
   avoid. The deferral's cost is therefore **bounded and known**, which is what
   the question asked.

---

## Decision 6 — Bounds (spec §5.2 / §7.1)

**Verdict: CONFIRMED** (and this one was largely settled on 08-22 — the new fact
is that UE's bounds are *serialized with the cooked payload*, not merely
computed).

- UE stores bounds **on the render data**: `FBoxSphereBounds Bounds;`
  (`StaticMeshResources.h:797`), and `FStaticMeshRenderData::Serialize` writes it:
  `Ar << Bounds;` (`StaticMesh.cpp:2586`). That is the DDC/cooked payload — the
  precise analog of F2c putting the AABB in the artifact header rather than
  recomputing it on load.
- On top of that, and **editor-side only**, sit the author-facing extensions the
  08-22 pass found: `FVector PositiveBoundsExtension` (`StaticMesh.h:1327`),
  `FVector NegativeBoundsExtension` (`:1355`), `FBoxSphereBounds ExtendedBounds`
  (`:1383`). F2c has no equivalent and needs none.
- Source 2 stores baked min/max per scene object — `m_vMinBounds` / `m_vMaxBounds`
  (`Mesh.cs:~107-112`), aggregated across scene objects by `GetBounds()`
  (`~103-127`).

§7.1's split — artifact AABB for imported meshes, `ComputeMeshBounds` retained for
generated primitives — matches UE's own two-layer arrangement (cooked bounds for
built meshes, computed at build time from the same vertices). No amendment.

---

## Decision 7 — Residency and eviction (spec §7.2 / R2)

**Verdict: CONFIRMED, and not novel** — with the novel parts named honestly.

**UE's default for a static mesh is all-resident.** The LOD-group defaults are
`DefaultMaxNumStreamedLODs(0)` and `bSupportLODStreaming(false)`
(`StaticMeshResources.h:70-76`); mesh LOD streaming is opt-in per LOD group
(`IsLODStreamingSupported()`, `:110-112`). So F2c's "upload once on first draw,
keep it" is UE's out-of-the-box behaviour, not a shortcut.

**When streaming is enabled, the mechanism is a byte budget, and it is a cvar.**
Static meshes are managed by `FRenderAssetStreamingManager` — the same manager as
textures — whose budget is `int64 FRenderAssetStreamingManager::GetPoolSize() const { return GTexturePoolSize; }`
(`StreamingManagerTexture.cpp:455-458`), set from `r.Streaming.PoolSize` or, when
that is `-1`, from a VRAM percentage:
`TexturePoolSize = Stats.TotalGraphicsMemory * GPoolSizeVRAMPercentage / 100;`
(`:1744-1767`), logging *"Texture pool size now %d MB"*. The streaming path for
meshes lives beside it (`Private/Streaming/StaticMeshUpdate.{h,cpp}`), and the
resource side exposes `ReleaseRHIForStreaming` per index buffer
(`StaticMeshResources.h:516-521`) with `bBuffersInlined` / `NumInlinedLODs` /
`CurrentFirstLODIdx` as the residency state (`:461, :713, :820-822`).

Mapping F2c onto that:

| F2c §7.2 | UE | Verdict |
|---|---|---|
| Resident by default, upload once | `bSupportLODStreaming(false)`, `DefaultMaxNumStreamedLODs(0)` | same |
| One combined CPU+GPU **byte** budget | `GetPoolSize()` / `r.Streaming.PoolSize` | same shape |
| Compile-time constant now, cvar when the cvar arc lands | is a cvar | same destination |
| **Whole-mesh** eviction granularity | **per-LOD** | ours, because we have no LODs |
| **Strict LRU**, never evict a mesh drawn this frame | priority/wanted-mips heuristic | ours, and simpler |
| Budget **separate from textures** | **shared pool** across render assets | ours, for now |

The last row is the only one worth flagging forward: UE budgets *all* GPU-resident
render assets against one pool, because two independent budgets can each stay
under their limit while together exhausting VRAM. Arcane has no texture byte
budget today, so a separate `kMeshResidencyBudgetBytes` is the right first move —
but unification is the eventual shape, and the parked cvar arc is where it lands.
Recorded, **not** amended (adding a texture budget is out of F2c's scope and the
spec correctly does not reach for it).

Source 2 contributes nothing here: VRF parses files and cannot observe runtime
residency policy. Stated, not claimed.

---

## Decision 8 — Degenerate/empty refusal and validation (spec §4.5)

**Verdict: CONFIRMED on the core rule, AMEND on two edges.**

UE's postures, all read in source, sort into three distinct tiers:

**Tier 1 — nothing drawable ⇒ loud ERROR, abort the mesh:**
```cpp
if(PolygonCount == 0)
{
    AddTokenizedErrorMessage(FTokenizedMessage::Create(EMessageSeverity::Error,
        FText::Format(LOCTEXT("Error_NoPolygonFoundInMesh", "No polygon were found on mesh  '{0}'"), ...)),
        FFbxErrors::StaticMesh_NoTriangles);
    return false;
}
```
(`FbxStaticMeshImport.cpp:651-655`). §4.5's *"refuses loudly at cook with a
diagnostic — never a silent empty artifact"* is UE's rule, including the
name-the-mesh diagnostic. **Confirmed.**

**Tier 2 — partial damage ⇒ WARN and drop the bad geometry, keep the import.**
The glTF factory detects degenerate triangles inline (a corner sharing a vertex
with another, `GLTFMeshFactory.cpp:611-616` and `:679`), skips them, and reports:
*"Mesh {0} has primitive with degenerate triangles: {1}"* at
`EMessageSeverity::Warning` (`:418-421`). Sibling warnings use the same tier:
*"Mesh has an invalid primitive : {0}"* (`:340`), *"Mesh has primitives with no
materials assigned: {0}"* (`:423-425`).

F2c's §4.5 is *already* consistent with this — it refuses only when the file
yields "**nothing** drawable (no meshes, **all**-degenerate)". But it never states
the partial case, and an implementer reading "degenerate ⇒ refuse loudly" out of
context will reject a 50k-triangle prop over three bad faces. One clarifying
sentence closes it. **A2 (part 1).**

**Tier 3 — a required capability we do not have ⇒ loud refusal, naming it.** This
is the find of the section, because UE's rule is *strictly more general than
ours*:

```cpp
TArray<FString> NotSupportedRequiredExtensions;
if (GltfAsset.ExtensionsRequired.Num() != 0)
{
    for (const FString& RequiredExtension : GltfAsset.ExtensionsRequired)
    { ... NotSupportedRequiredExtensions.Add(RequiredExtension); }
}
...
//In case of non supported extensions fail out:
if (NotSupportedRequiredExtensions.Num() > 0)
{
    ... ErrorResult->Text = FText::Format(
        LOCTEXT("UnsupportedRequiredExtensions", "Not all required extensions are supported. (Unsupported extensions: {0})"),
        FText::FromString(NotSupportedRequiredExtensionsStringified));
    return false;
}
```
(`InterchangeGltfTranslator.cpp:557-604`). UE walks glTF's own
`extensionsRequired` array and refuses the whole import, naming **every**
unsupported entry.

F2c hand-lists one extension: *"`EXT_meshopt_compression`-compressed buffers
refuse loudly naming the extension"* (§4.5). That leaves `KHR_draco_mesh_compression`
and `KHR_texture_basisu` — both already rejected in §2 — with **no stated refusal
behaviour**, and it leaves every future extension to be hand-added. Adopting UE's
general rule covers all three and everything after them with one sentence and no
extra machinery, since cgltf already parses `extensionsRequired`. **A2 (part 2).**

**One posture we should consciously NOT copy.** UE repairs NaN geometry rather
than refusing it: `if (!FStaticMeshOperations::ValidateAndFixData(...))` reports
*"Invalid mesh data (NAN) was found and changed to zero. This may affect the mesh
rendering."* (`InterchangeGltfTranslator.cpp:1374-1379`). Substituting zeros for
unreadable input is precisely the fabrication F2c's §4.5 and §7.1 forbid ("Never
render an unknown as a cube"). Our loud refusal is the better answer; recorded so
the divergence is deliberate rather than accidental.

`cgltf_validate` being mandatory (§4.5, §10) has no direct UE analog to cite —
UE's reader has its own validation with an error-severity log tier
(`InterchangeGltfTranslator.cpp:569-581` refuses when any reader message is
`GLTF::EMessageSeverity::Error`), which is the same "the parser's own verdict is
a gate" shape. Confirmed by structure rather than by one line.

---

## Decision 9 — Thumbnails (spec §8 / R5)

**Verdict: CONFIRMED.** UE's static-mesh thumbnails are an editor-side live
render, persisted outside cooked data — exactly R5.

- The renderer is editor code:
  `Editor/UnrealEd/Classes/ThumbnailRendering/StaticMeshThumbnailRenderer.h`
  declares `UStaticMeshThumbnailRenderer : public UDefaultSizedThumbnailRenderer`
  with `virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)`
  (`:24-25`) and a private `class FStaticMeshThumbnailScene* ThumbnailScene;`
  (`:32`). A real scene, rendered offscreen into a render target — the same shape
  as R5's "offscreen lit render framed by the artifact AABB" via the
  material-harvester architecture.
- Persistence is a separate editor-side cache, not the cooked payload:
  `FObjectThumbnail* CacheThumbnail(const FString& ObjectFullName, FObjectThumbnail* Thumbnail, UPackage* DestPackage);`
  (`ObjectTools.h:769`).
- Nothing in `FStaticMeshRenderData` / `FStaticMeshLODResources`
  (`StaticMeshResources.h`) carries a thumbnail.

F2c's `Saved/Thumbnails/<guid>.png` is UE's `CacheThumbnail` in miniature, and
leaving the artifact's `Thumbnail` tag **reserved and unwritten** matches UE
keeping thumbnails out of render data entirely. No amendment.

(Source 2: not applicable. Hammer's asset browser thumbnails are tool-side
behaviour VRF cannot verify — stated, not claimed.)

---

## Decision 10 — Embedded-texture extraction (spec §5.5)

**Verdict: CONFIRMED that every glTF texture becomes a standalone asset;
DIVERGENCE-KEEP on the extraction mechanism.**

**Confirmed half.** UE's glTF translator creates one standalone texture node per
glTF texture, embedded or external without distinction:

```cpp
UInterchangeTexture2DNode* TextureNode = UInterchangeTexture2DNode::Create(&NodeContainer, GltfTexture.UniqueId);
TextureNode->SetDisplayLabel(GltfTexture.Name);
```
(`InterchangeGltfTranslator.cpp:655-656`), plus sampler wrap modes
(`:673-674`) and sRGB resolution driven by how the material consumes it
(`:1120-1131`). These become ordinary `UTexture2D` assets that the minted
materials then reference — the shape §5.5 and §6 assume.

**Divergent half — UE never writes a loose file.** The payload fetch branches on
whether an image has a file path:

```cpp
if (GltfTexture.Source.FilePath.IsEmpty())
{
    // Embedded texture -- try using ImageWrapper to decode it
    TArray64<uint8> ImageData(GltfTexture.Source.Data, GltfTexture.Source.DataByteLength);
    ... TexturePayloadData = ImageWrapperTranslator->GetTexturePayloadDataFromBuffer(ImageData);
}
else { /* external: resolve the path, delegate to a file translator */ }
```
(`InterchangeGltfTranslator.cpp:1001-1021`). An embedded image is decoded **in
memory** straight into the import payload. F2c's §5.5 instead extracts embedded
textures to *"loose `.png`s beside the source, becoming ordinary registered
textures on the texture cook path."*

**Kept, and the architecture forces it.** UE can stay in memory because its DDC
keys off the importer's own inputs; F2c's cook spine is **source-file-driven** —
registry discovery, a `.meta` sidecar, and a cook key over *source bytes*
(§4.1, §5.4). An in-memory image has no registered source, no sidecar, and no
cook key, so it cannot ride the existing texture path — which is the whole point
of §5.5's "keeping the one texture cook path". Extraction buys that at the cost of
a file on disk. The 09-10 research reached the same conclusion independently
(§5: *"The F2b architecture strongly favors extraction"*).

**But the reference exposes two consequences UE never has to answer**, because a
file on disk is a thing a user can touch:

1. **Re-extraction.** Once extracted, the `.png` is a user-visible asset. What
   happens on the *next* cook of the same `.glb` — is an existing extracted file
   overwritten, left alone, or re-extracted only when absent? §6's *"import never
   overwrites an existing material asset"* invariant answers the material case
   explicitly and by name; the texture case is left unstated, and the two are
   plainly meant to agree.
2. **Name collisions.** Two `.glb` files can both embed `basecolor.png`, and glTF
   images may have no name at all. §6 already specifies "suffixed on collision"
   for minted materials; extracted textures need the same sentence.

One paragraph in §5.5 closes both, matching rules the spec already states
elsewhere. **A4.**

**One more thing to record, not to fix.** UE flips the green channel for any
texture used as a normal map — *"According to GLTF documentation the normal maps
are right handed (following OpenGL convention), however UE expects left handed
normal maps"* — `TextureNode->SetCustombFlipGreenChannel(true)`
(`InterchangeGltfTranslator.cpp:660-668`). F2c drops normal maps entirely (§6),
so this is inert today; it is the first thing that bites when T1 brings BC5 and
normal mapping, and it belongs in the T1 notes rather than in this spec.

An unreadable image is skipped, not fatal: unknown format ⇒ an error message then
`continue` — the import proceeds without that texture
(`InterchangeGltfTranslator.cpp:634-652`). Consistent with tier 2 above.

---

## Proposed amendments

Five, ordered by weight. **A1** is the only one that changes a data structure;
**A5** is optional.

### A1 — A section names its slot by INDEX; slots deduplicate by material name

**Spec sites:** §4.3 (Sections), §4.4 (The slot array), §5.2 (`SectionTable` tag),
§7.4 (Draws), §9 (tests).

**The defect.** §4.3 says one section per glTF primitive, named after the
primitive's material; §4.4 resolves a section's material through "the section's
slot guid" — i.e. positionally. When two primitives share one glTF material
(the ordinary shape of an exported prop: one metal material, five mesh parts),
those two rules produce a slot array with two identically-named entries, an
ambiguous R3 name re-association, and a user asked to assign the same material
twice.

**The reference answer, unanimous.** UE creates one polygon group **per used
material**, not per primitive (`GLTFMeshFactory.cpp:379-387, 444`), makes the
merge an explicit default (`bKeepSectionsSeparate = false`,
`InterchangeGenericAssetsPipelineSharedSettings.h:91-93`), and puts an **index**
between section and material (`FStaticMeshSection::MaterialIndex`,
`StaticMeshResources.h:203-204`). Source 2 attaches the material identity to each
draw call directly (`GltfModelExporter.Mesh.cs:~278-279`).

**The amendment.**

1. **Sections stay per-primitive** (preserving each primitive's index range as
   its own draw — cheaper than merging index buffers, and the only form that
   survives non-contiguous ranges after meshopt reordering).
2. Each section gains **`slotIndex` (u32)** — an index into the slot array.
   §5.2's `SectionTable` entry becomes: name (u16 length + UTF-8) + index offset +
   index count + **slotIndex**.
3. **The slot array deduplicates by material name.** Each distinct glTF material
   name mints one slot; every primitive using it points its `slotIndex` there.
   Primitives with no material share one unnamed slot appended last.
4. §4.4's per-section resolution chain reads through the index: component
   `materialOverride` (scalar, repaints all) → `slots[section.slotIndex].material`
   → white.
5. §9 gains one fixture assertion: the multi-primitive `.glb` corpus entry
   includes **two primitives sharing one material**, and the test asserts two
   sections, **one** slot, and both sections' `slotIndex` equal.

**Why not simply merge same-material primitives into one section** (UE's literal
default)? Because meshopt's vertex-fetch/cache reordering runs after the bake
(§5.3) and there is no guarantee a merged material's triangles stay contiguous in
the index buffer. Keeping per-primitive ranges plus an index is UE's *data model*
without UE's merge pass — strictly simpler for us, and it makes `bKeepSectionsSeparate`
a non-question rather than a future option.

**Cost:** 4 bytes per section in the artifact, one field in `MeshSection`, one
indirection at draw time. §7.4 is untouched (`MeshInstance` already carries the
bindless material slot per draw). Nothing about ABI, cook keys, or the container
version changes — the `SectionTable` tag is new in F2c, so there is no
compatibility burden.

### A2 — Refusal generalizes to `extensionsRequired`; partial degenerates warn and drop

**Spec site:** §4.5 (Validation). Two sentences, no new machinery.

**Part 1 — the refusal rule generalizes.** Replace the single hand-named
extension with the general rule UE implements:

> Every entry in the file's `extensionsRequired` that the importer does not
> implement causes the cook to **refuse loudly, naming every unsupported entry
> in one diagnostic**. This covers `EXT_meshopt_compression`,
> `KHR_draco_mesh_compression`, and `KHR_texture_basisu` (all rejected in §2)
> without listing them, and covers every future extension by construction.

Evidence: `InterchangeGltfTranslator.cpp:557-604` — UE walks
`GltfAsset.ExtensionsRequired`, accumulates the unsupported ones, and emits
*"Not all required extensions are supported. (Unsupported extensions: {0})"*
before `return false`. cgltf already parses `extensionsRequired`, so this is a
loop and a message, not a feature. §2's "flip is cheap" note for
`EXT_meshopt_compression` survives unchanged — implementing an extension simply
removes it from the unsupported set.

**Part 2 — state the partial-damage tier.** Add:

> **Degenerate triangles within an otherwise valid mesh are dropped with one
> WARN naming the primitive, not refused.** Refusal is reserved for a file that
> yields *nothing* drawable.

Evidence: `GLTFMeshFactory.cpp:611-616, 679` (degenerate corners are detected and
skipped) and `:418-421` (*"Mesh {0} has primitive with degenerate triangles: {1}"*
at Warning severity), against the empty-mesh refusal at
`FbxStaticMeshImport.cpp:651-655`. §4.5 is already *consistent* with this — it
refuses only on "no meshes, all-degenerate" — but says nothing about the partial
case, which is the case an implementer will actually meet.

### A3 — Record the mirror rule's tangent half against the reserved `Tangents` tag

**Spec sites:** §4.3 (Bake, one clause), §2 (the Tangents deferral row), §9 (the
negative-scale fixture's test).

> A negative-determinant node inverts triangle **winding** and, once tangents
> exist, also the **tangent-basis handedness**. F2c ships only the winding half;
> the reserved `Tangents` section inherits the handedness obligation, and the
> negative-scale fixture's test grows a handedness assertion at that time.

Evidence: UE drives both from the one flag —
`const bool bIsMirrored = TotalMatrix.Determinant() < 0.f;` (`GLTFMeshFactory.cpp:459`)
feeding both the reversed corner loop (`:597-605`) **and**
`const float BinormalMultipler = bIsMirrored ? -1.f : 1.f;` (`:460`); the FBX
path computes the same sign explicitly
(`VertexInstanceBinormalSigns[...] = GetBasisDeterminantSign(...)`,
`FbxStaticMeshImport.cpp:1006`).

Why it earns a line now: §9's negative-scale fixture and its winding test will
**stay green** when tangents arrive and are wrong, because nothing asserts
handedness. This is a silent-regression trap disarmed by one sentence in the
spec that already owns the fixture.

### A4 — §5.5 states re-extraction and collision behaviour for extracted textures

**Spec site:** §5.5 (Division of labor), one paragraph.

> An embedded texture is extracted **only when the destination file is absent**;
> an existing extracted `.png` is never overwritten — the same
> import-never-overwrites invariant §6 states for materials, so a user's edited
> or replaced texture survives re-import. Extracted files are named after the
> glTF image/texture name, **suffixed on collision** (§6's rule), falling back to
> a source-derived name when the glTF supplies none.

Why: §6 states the no-overwrite invariant for `.arcmat` files by name and calls
it out as load-bearing ("user edits to minted materials are permanent"), while
§5.5's extracted `.png`s — equally user-visible, equally editable, equally
subject to name collision across two `.glb` files — are left unstated. The
reference is what surfaced this: UE decodes embedded images in memory
(`InterchangeGltfTranslator.cpp:1001-1009`) and therefore never faces either
question, so our divergence is precisely where our rules must be more explicit
than UE's, not less.

### A5 — (minor, optional) An explicit `indexWidth` byte in the mesh header

**Spec site:** §5.2 (mesh header).

> Mesh header becomes: `vertexCount` (u32), `indexCount` (u32), `sectionCount`
> (u32), `indexWidth` (u8, **4** in v1), AABB min/max (6×f32).

Both references treat index width as a **container property**, decided per mesh
and stored: UE's `EIndexBufferStride::AutoDetect` — *"Use 16 bits unless an index
exceeds MAX_uint16"* (`RawIndexBuffer.h:74-85`) with `FRawIndexBuffer16or32::b32Bit`
(`:35-68`); Source 2's `ElementSizeInBytes` as the index type size, branched on at
read (`VBIB.cs:~53`, `GltfModelExporter.Mesh.cs:~572-589`).

F2c's u32-only deferral is right (§2: perf-only; all runtime plumbing is
unconditionally u32 at `MeshNode.cpp:972-973`), and the tagged skip-unknown
container means a future `IndexData16` tag would be additive regardless — so this
is genuinely optional. It buys one thing: a future 16-bit path becomes a **read
of a declared field** rather than tag-presence inference duplicated across the
two deliberately-independent readers (engine + ArcaneClient, `ArtifactFormat.hpp:15-21`).
One byte, written once, asserted `== 4` on read in v1.

---

## Confirmations — the spec's rulings the references upheld

Everything below survives the pass **unchanged**, now with reference backing:

1. **Node TRS bakes into vertices, normals via inverse-transpose** (§4.3) —
   `bBakeMeshes = true` default (`InterchangeGenericAssetsPipelineSharedSettings.h:83-85`);
   `TotalMatrix.TransformPosition` per vertex (`GLTFMeshFactory.cpp:361`);
   `TotalMatrix.Inverse().GetTransposed()` for normals (`:456-457`).
2. **Negative determinant flips winding** (§4.3) —
   `bIsMirrored = TotalMatrix.Determinant() < 0.f` + reversed corner walk
   (`GLTFMeshFactory.cpp:459, 597-605`); *"If there are odd number negative scale,
   invert the vertex order for triangles"* (`FbxSkeletalMeshImport.cpp:3603-3604`).
3. **One asset per file** (R1) — a deliberate divergence from UE's per-mesh
   default (`bCombineStaticMeshes = false`,
   `InterchangeGenericMeshPipeline.h:54-56`), justified because our seam forbids
   the actor-hierarchy half that makes UE's default coherent.
4. **Zero import-transform knobs in v1** (§5.4) — UE's knobs exist
   (`FbxAssetImportData.h:29-36`; `InterchangeGenericAssetsPipeline.h:74-84`) but
   its *mandatory* scale is a unit conversion (`GltfUnitConversionMultiplier`,
   `InterchangeGltfMesh.cpp:112, 286`) that is 1.0 for Arcane. The empty settings
   block still hashes, so the first knob is honest.
5. **Named slots, name re-association, positional tiebreak, never delete** (R3) —
   `MaterialSlotName` / `ImportedMaterialSlotName` with the re-import comment
   (`StaticMesh.h:510-515`), `GetMaterialIndexFromImportedMaterialSlotName`
   (`:1978`), append-if-unmatched with no deletion arm and
   `MaterialIndex = PolygonGroupID.GetValue()` fallback
   (`FbxStaticMeshImport.cpp:1946-1974`).
6. **The slot name is the glTF material name** (§4.3/R3) — UE renames polygon
   groups from material *index* to material *name* in a dedicated pass
   (`InterchangeGltfMesh.cpp:54-71`).
7. **Section range = index offset + count** (§5.2) — `FirstIndex`/`NumTriangles`
   (`StaticMeshResources.h:207-208`); `m_nStartIndex`/`m_nIndexCount`
   (`GltfModelExporter.Mesh.cs:~354-356`). Both references' extra vertex-window
   field (`MinVertexIndex`, `m_nBaseVertex`) is correctly absent from ours: it
   exists for shared/offset vertex buffers, and our indices are absolute into one
   buffer.
8. **Fixed interleaved pos/normal/uv, u32-only** (§5.2) — a deliberate
   divergence (UE de-interleaves positions, `StaticMeshResources.h:319-328` +
   `PositionVertexBuffer.h:96-98`; Source 2 declares layout as data,
   `VBIB.cs:~40-105`), kept because our vertex input is one 32-byte binding and a
   self-describing layout would be stored-but-unread.
9. **Tangents deferred** (§2) — UE bakes them by default
   (`EngineTypes.h:2763-2765`) and Source 2 packs them with normals
   (`Mesh.cs:~131-165`), but both have normal mapping and Arcane's mesh shader
   deliberately does not. The deferral's format cost is bounded: a parallel tagged
   section, no `VertexData` rewrite.
10. **AABB stored in the artifact** (§5.2/§7.1) —
    `FBoxSphereBounds Bounds` on render data (`StaticMeshResources.h:797`),
    serialized (`StaticMesh.cpp:2586`); `m_vMinBounds`/`m_vMaxBounds`
    (`Mesh.cs:~107-112`). Author-side extensions
    (`StaticMesh.h:1327, 1355, 1383`) are editor-only and we need none.
11. **All-resident + byte-budget LRU, cvar later** (§7.2/R2) — UE's static-mesh
    default is all-resident (`StaticMeshResources.h:70-76`); its opt-in streaming
    is a byte pool driven by `r.Streaming.PoolSize`
    (`StreamingManagerTexture.cpp:455-458, 1744-1767`). Novel to us: whole-mesh
    granularity (no LODs) and strict LRU (vs UE's priority model) — both
    simplifications, both honest. UE's single pool shared with textures is the
    eventual unification; recorded, out of F2c's scope.
12. **Nothing drawable ⇒ refuse loudly with a diagnostic** (§4.5) —
    `Error_NoPolygonFoundInMesh` at Error severity then `return false`
    (`FbxStaticMeshImport.cpp:651-655`).
13. **Never fabricate geometry** (§4.5/§7.1) — a deliberate divergence: UE
    replaces NaN vertex data with zeros and warns
    (`InterchangeGltfTranslator.cpp:1374-1379`). Our loud refusal is the better
    answer for a persisted artifact, and is now a recorded choice rather than an
    accident.
14. **Thumbnails are an editor harvest, artifact tag stays reserved** (R5) —
    `UStaticMeshThumbnailRenderer` + `FStaticMeshThumbnailScene` render offscreen
    into a render target (`StaticMeshThumbnailRenderer.h:24-32`); persistence is
    `ObjectTools::CacheThumbnail(..., UPackage* DestPackage)` (`ObjectTools.h:769`),
    never cooked render data.
15. **Every imported texture becomes an ordinary standalone texture asset**
    (§5.5/§6) — `UInterchangeTexture2DNode::Create` per glTF texture, embedded or
    external alike (`InterchangeGltfTranslator.cpp:655-656`).
16. **Generalized cook spine with per-kind leaves** (R6) — settled by research §7
    and **not re-litigated here**, per instruction.

## What could not be verified, and why

- **Whether Valve's Source 2 model compiler bakes authored node transforms into
  mesh vertices.** VRF reads compiled output: it shows vertex data in one space
  and draw calls indexing it, but the compiler's input-side behaviour leaves no
  trace VRF can observe. Tool-side — not claimed. (VRF's own `BakePositions` /
  `GetPlacementTransform` at `GltfModelExporter.Mesh.cs:~212, 526-527` is VRF's
  coordinate conversion for *export*, not evidence about Valve's importer.)
- **Source 2 runtime residency, eviction, or streaming policy for meshes.** VRF
  is a file parser; it has no view of runtime memory management. Decision 7 rests
  on UE alone, and says so.
- **Source 2 thumbnail generation** (Hammer/asset-browser). Tool-side, outside
  VRF. Decision 9 rests on UE alone.
- **UE's material-conflict warning on re-import.** `FbxMaterialConflictWindow.cpp`
  exists in the dump but its trigger conditions were not read; the claim made here
  is only the narrow one verified in `FbxStaticMeshImport.cpp:1954-1974` — that
  path appends unmatched slots and has no deletion arm. Whether UE *warns*
  elsewhere about an orphaned slot is unverified, so A-nothing rests on it.
- **VRF line numbers** are approximate (marked `~`), reported by the fetching
  summarizer rather than read directly from a local checkout. Field names,
  structure, and quoted comments are verbatim; the numbers are navigational.
- **Unity** — excluded by instruction; no source exists to read.
