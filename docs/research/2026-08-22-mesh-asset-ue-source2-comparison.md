# Mesh assets in Unreal and Source 2 — what F2a should copy

**2026-08-22.** Research pass against `docs/specs/2026-08-22-f2a-scene-3d-vocabulary-design.md`,
run before writing that spec's implementation plan. It changed the spec twice;
both amendments are recorded here with the evidence that forced them.

## Sources, and the policy they obey

| Engine | Source | Provenance |
|---|---|---|
| **Unreal 5** | `EpicGames/UnrealEngine` @ `release`, files fetched individually through the GitHub API | Linked Epic account. Standing directive: *read the tree, never answer from memory* ([[project_arcane_outliner_arc]]) |
| **Source 2** | `ValveResourceFormat/ValveResourceFormat` @ `master` (MIT) | Clean-room reverse-engineered parser. **No leaked Source 2 code was read, quoted, or consulted** — standing hard rule ([[project_arcane_deadlock_render_target]]) |

The UE dump at `.example/UnrealEngine-release/` is still gone (removed in the
2026-08-11 cleanup). A ~1 GB blobless sparse clone was **not** needed here: the
questions named specific files, and fetching those files through the API is
still reading real source rather than recalling it. The clone recipe stays the
right tool for a question that needs broad grepping.

Files read: `Runtime/Engine/Classes/Engine/StaticMesh.h`,
`Runtime/Engine/Classes/Components/StaticMeshComponent.h`,
`Runtime/Engine/Classes/Components/MeshComponent.h`,
`Runtime/GeometryCore/Public/Generators/{Capsule,Sphere,Rectangle,GridBox}*.h`;
VRF `Resource/ResourceTypes/{Model,Mesh}.cs`.

---

## The three-way comparison

| Question | Unreal 5 | Source 2 | F2a as specced |
|---|---|---|---|
| Size on the asset? | **None.** `UStaticMeshComponent::GetStreamingScale()` returns `GetComponentTransform().GetMaximumAxisScale()` (`StaticMeshComponent.h:684`) | **None.** Nothing in the model parser reads one | none — **confirmed** |
| Bounds on the asset? | `ExtendedBounds` + `Negative`/`PositiveBoundsExtension` (`StaticMesh.h:61-63`) | baked `m_vMinBounds`/`m_vMaxBounds` per scene object (`Mesh.cs:118-131`) | computed in `BuildMeshData` — **confirmed** |
| Where do materials live? | **On the asset**: `TArray<FStaticMaterial> StaticMaterials` (`StaticMesh.h:1095-1096`), overridable per component: `TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials`, "Per-Component material overrides" (`MeshComponent.h:29-31`) | **On the model**: `m_materialGroups`, each `{ m_name, m_materials[] }` — named skins (`Model.cs:563-565`) | on the component — **CHANGED, see A1** |
| Per-instance colour? | **None.** Only `bOverrideWireframeColor` (editor viz, `:235`) and vertex-colour painting (`:295-335`) | Not in the model data | `tint` field — **REMOVED, see A1** |

### The size answer is unanimous, and it is the spec's load-bearing rule

Neither engine stores a size on a mesh asset. UE goes further and states the
positive rule in code: streaming scale is *read from the component transform*.
F2a's "generators emit UNIT geometry — scale expresses size, rotation expresses
orientation, the asset expresses shape" is the same rule, arrived at
independently from `SpriteRenderer`'s existing comment and now confirmed twice.

**The one UE fact that looks like a counter-example is not one.** UE's
parametric generators DO carry absolute sizes: `FCapsuleGenerator` has
`Radius = 1.0` and `SegmentLength = 1.0` ("total height is SegmentLength +
2*Radius"); `FSphereGenerator` has `Radius`; `FRectangleMeshGenerator` has
`Width`/`Height`. But those are **tool-time** types in `GeometryCore` that
*produce* geometry which is then baked into a sizeless `UStaticMesh`. UE has two
layers where F2a has one — `.arcmesh` persists the generator parameters
themselves, with no bake.

That difference is exactly why F2a's `capsuleLengthRatio` should stay a **ratio**
rather than becoming UE's absolute `Radius` + `SegmentLength` pair: with no bake
step, absolute sizes on a persisted asset would reintroduce the two-spellings
problem (asset size *and* transform scale) that the rule exists to prevent. The
ratio keeps the one shape dimension a scale genuinely cannot reach, and nothing
else.

---

## A1 — Materials move to the asset, and `tint` is deleted

**Both engines put material assignment on the mesh/model, with per-instance
override, and neither has a per-instance colour multiply.** F2a as specced had
it exactly backwards: material on the component, plus a `tint`.

The decisive point is that **Arcane already ships UE's answer.**
`MaterialAssetData::parent` + sparse overrides is `UMaterialInstance`, and it is
**kind-agnostic** — the instance carries "no snippet, no kind (both come from the
base at the end of the parent chain)" (`MaterialAsset.hpp:5-9`). So a red cube
and a blue cube are two *instances* of one mesh material, needing no new
machinery and no new field.

`tint` also violated the spec's own rule. A red cube would have been expressible
twice — as a red material, or as a white material with a red tint — which is the
same two-spellings defect the sizing rule rejects three sections earlier. Keeping
it would have been a consistency debt, not a convenience.

**Amended model:**

```
.arcmesh          Guid material   -- the mesh's default
MeshRenderer      Guid materialOverride  -- per-entity, UE's OverrideMaterials in miniature
resolution        override -> mesh default -> white
```

Scalar rather than an array because F2a's primitives are single-section. F2c's
imported multi-section meshes grow the scalar into a slot array, which is
**additive** — where the specced design would have had to *move* the material off
the component, a component schema change plus a scene re-author.

---

## A2 — Refusal keeps its threshold, but gains a widget guard

UE's generators **clamp silently**:

```cpp
NumCircleSteps        = FMath::Max(NumCircleSteps, 3);         // CapsuleGenerator.h:267
NumHemisphereArcSteps = FMath::Max(NumHemisphereArcSteps, 2);  // CapsuleGenerator.h:265
NumPhi = FMath::Max(NumPhi, 3);  NumTheta = FMath::Max(NumTheta, 3);  // SphereGenerator.h:200-201
```

F2a specced a loud refusal instead. **Both thresholds independently agree at 3**,
which is a useful confirmation — but the clamp-vs-refuse split is real, and UE is
not simply right here: its generators are tool-time transients where a clamp is
invisible and harmless. `.arcmesh` is a **persisted asset**, so a silent clamp
means the file says `segments = 1` while the mesh is 3, forever. That is the
two-spellings problem again.

**Neither is the best answer available.** Arcane has a third option the
references do not: `Astra::Range` attributes, read by the Inspector through
`RangeOfField` (`InspectorMeta.hpp:52`). Putting the minimum on the reflected
field makes the invalid state **unauthorable at the widget**, so refusal only
ever fires on a hand-edited file. Belt and braces, and the user never meets the
error in normal use.

Thresholds adopted from UE, one correction to the spec: a capsule's cap rings
floor is **2**, not 3.

| Source | Field | Floor |
|---|---|---|
| `Plane` | `subdivisions` | 1 |
| `Cube` | — | — |
| `UvSphere` | `rings`, `segments` | 3, 3 |
| `Cylinder` | `segments` | 3 |
| `Capsule` | `rings`, `segments`, `capsuleLengthRatio` | **2**, 3, 1.0 |

---

## Divergences kept deliberately

**A procedural-primitive ASSET type is ours alone.** UE's basic shapes are
shipped `.uasset` content; its parametric generators are tool-time and produce
baked meshes. Source 2's primitives are map geometry, not model assets. F2a's
`.arcmesh` naming a `MeshSource` plus parameters matches neither — it matches
ProBuilder/Blender more than either reference engine.

Kept, because it is the honest shape for *this* phase: it gives authorable,
saveable, reopenable 3D content with **no import pipeline and no cook step**,
which is the entire reason F2a exists as a separate arc. It is also not a dead
end — the same struct absorbs an imported mesh at F2c as another `MeshSource`
plus an artifact reference, at which point Arcane has both of UE's layers.

**No LODs, no mesh groups.** UE has `m_refLODGroupMasks`-equivalent LOD chains
and Source 2 has `m_meshGroups` + `m_nDefaultMeshGroupMask`. Both are import-era
concerns; F2a's primitives have one section and one LOD.

**No named material slots.** `FStaticMaterial` carries `MaterialSlotName` and
`ImportedMaterialSlotName` so a re-import can re-associate materials by name.
That machinery exists to survive re-import — which F2a has no concept of. It
arrives with F2c or not at all.

---

## Net effect on the spec

| Change | Section |
|---|---|
| Material default moves to `.arcmesh`; `MeshRenderer` carries `materialOverride`; `tint` deleted | §2, §3 |
| Resolution chain: override → mesh default → white | §3, §4 |
| Capsule `rings` floor 3 → 2 | §1 |
| `Astra::Range` minimums on the reflected topology fields | §1, §6 |
| Unit-geometry rule cited to UE + Source 2 rather than to `SpriteRenderer` alone | §1 |

Everything else in the spec survived the pass unchanged.
