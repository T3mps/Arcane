# F2a — 3D vocabulary in the scene: design

**Decided 2026-08-22.** This is the design contract for the first of three arcs
that the 3D-foundations pivot originally called "F2". Read
`docs/research/2026-08-21-3d-foundations-assessment.md` (the pivot) and
`docs/research/2026-08-21-asset-cook-pipeline-design.md` (the cook contract)
first; this document resolves a sequencing disagreement between them and then
scopes the arc that comes out of it.

Predecessor: `docs/plans/2026-08-21-f1-transform-spine.md` (F1, landed
`b86c74f6`).

---

## Why F2 is three arcs

The two governing documents disagree about ordering, and the disagreement is
load-bearing rather than cosmetic.

- The **pivot** says F2 is `MeshRenderer` + mesh/material assets *with* the cook
  pipeline, on the grounds that "without it, 3D content cannot be authored,
  saved, or reopened."
- The **cook contract** scopes its own first slice to **textures only** —
  meshes explicitly out, "needs the mesh arc + cgltf" — and sequences itself as
  *Phase 4 → cook (texture-only) → T1 → mesh arc*.

Checking the pivot's claim rather than accepting it: it is **false for
procedural meshes and true for imported ones**. Phase 4 Task 6 already shipped
`BuildCube`/`BuildUvSphere` as pure, device-free, headlessly-testable
generators — and `MeshBuilder.hpp` says why in as many words: this phase
"renders procedural geometry ON PURPOSE ... precisely so the renderer slice does
not drag the asset pipeline in behind it." An `.arcmesh` naming a generator and
its parameters authors, saves, reopens and renders deterministically with no
cook infrastructure at all.

So the thing genuinely blocking Phase 4 Task 9 — *nothing in the scene to
populate `MeshSceneDesc` from* — is closable without vendoring a single library.

| Arc | Content | Vendors |
|---|---|---|
| **F2a** (this doc) | `MeshRenderer`, `.arcmesh`, mesh material kind, resolver caches, Task 9 host wiring, `MeshDocument` | none |
| **F2b** | The cook contract's own texture-only first slice; then Phase 4 Task 8's bindless table, whose *policy* falls out of it | bc7enc_rdo |
| **F2c** | Mesh import | cgltf, meshoptimizer |

F2a is what this document specifies.

---

## Scope

**Goal, stated as the gap it closes.** Make the scene produce meshes — authored,
saved, reopened, rendered in both hosts — so that Task 9 becomes implementable
and the editor stops being unable to author 3D content.

**Non-goal, stated because it is tempting.** Making meshes look *good*. F2a ends
with flat-shaded, single-directional-light geometry, which is exactly what
Phase 4 Task 7's `[pixel]` case already proved on the GPU. The addition is that
the **scene** drives it instead of a hand-built descriptor.

---

## 1. The mesh asset

`.arcmesh` — flat JSON with an embedded top-level `"id"`, riding
`AssetRegistry::ScanContent`'s native path exactly like `.arcsprite` and
`.arcmat`.

```cpp
enum class MeshSource : std::uint8_t {
    Plane = 0, Cube = 1, UvSphere = 2, Cylinder = 3, Capsule = 4
};

struct MeshAssetData {
    Guid          id{};
    std::string   name;
    MeshSource    source = MeshSource::Cube;

    // TOPOLOGY -- density a Transform cannot express.
    std::uint32_t rings        = 16;   // UvSphere; Capsule (per cap)
    std::uint32_t segments     = 32;   // UvSphere, Cylinder, Capsule (radial)
    std::uint32_t subdivisions = 1;    // Plane (1 = a single quad)

    // SHAPE RATIO -- a family no scale can reach.
    float capsuleLengthRatio = 2.0f;   // total height / diameter; >= 1
};
```

Flat and tagged, with per-source field meaning documented rather than enforced
by the type — the same form `Manifold2D::Physics::Shape` uses, where `halfLen`
simply means nothing to a `Circle`
(`ThirdParty/Manifold2D/include/Manifold2D/Physics/Shapes.hpp:86-135`).

`BuildMeshData(const MeshAssetData&) -> MeshData` is a pure dispatch onto
`MeshBuilder`. `MeshAssetData` is F2c's seam: an imported mesh becomes another
`MeshSource` plus an artifact reference, with no component and no scene change.

**`MeshBuilder` gains three generators.** It ships `BuildCube` and
`BuildUvSphere` today and says so in its banner ("the interface below is exactly
BuildCube and BuildUvSphere -- no plane, no cylinder, no capsule"). That
sentence was scoped to Phase 4, whose stated reason for the exclusion was
tangent generation needing a vendored library — not the primitives themselves.
F2a is the arc that owns mesh vocabulary, so `BuildPlane`, `BuildCylinder` and
`BuildCapsule` land here, under `MeshBuilder`'s existing counter-clockwise
winding contract and its device-free discipline. Still no tangents.

### The sizing rule

**Generators emit UNIT geometry. Scale expresses size, rotation expresses
orientation, the asset expresses shape.**

There is no `sizeMeters` and no `radiusMeters`. The engine already wrote this
rule down once, on `SpriteRenderer` (`Scene/Components.hpp:157-161`):

> "There is NO size field: an entity is sized by its Transform scale, matching
> Unity/Unreal (neither puts a size on the sprite renderer), so one asset can be
> shared by many entities at different scales."

Two consequences, and the second is the one that matters:

1. **A 2 m cube has one spelling, not two.** With a size on the asset *and* a
   scale on the transform, "why is my cube 4 m" has two places to look.
2. **`MeshCache` shares.** With size on the asset, two differently-sized cubes
   are two Guids, two `MeshData` and two vertex uploads of *identical topology*.
   With unit geometry every cube in the project shares one entry.

A byte-count argument was considered and is the wrong lens: `MeshAssetData` is
loaded once per unique asset and cached — tens of them, never per-frame. The
struct that deserves byte scrutiny is `MeshInstance` (a `mat4` plus a `vec4`,
one per drawn entity per frame). The size fields are removed on the design
argument above, not on their footprint.

### Why `capsuleLengthRatio` survives when no other size does

This is the test every future field must face.

A cylinder scaled non-uniformly in Y is still a correct cylinder: its caps are
flat discs and nothing distorts. A capsule scaled in Y is **not** a capsule —
its hemispherical caps become ellipsoids. So the cylinder needs no ratio and the
capsule does. The field earns its place by naming a shape family that scale
genuinely cannot reach, not by being convenient.

The same reasoning is why the roster is five rather than six: `Plane` is XZ with
a `+Y` normal (a ground plane — matching the engine's `+Y`-up convention and
Unity's `Plane`), and Unity's separate camera-facing `Quad` is that same asset
under a −90° X rotation. Orientation is the Transform's job for exactly the
reason size is.

### Refusal

`BuildMeshData` **refuses** degenerate input — emitting no geometry and one
memoized diagnostic that names the offending field. An *invalid* mesh is an
error; a *nil* mesh Guid on a component is not (it draws nothing, like a nil
sprite).

Refusal is evaluated **per source, over the fields that source actually reads** —
a `Plane` with `segments = 0` is valid, because a plane has no radial segments
and the field is as meaningless to it as `halfLen` is to a `Circle`. Validating
the whole struct regardless of tag would refuse legal assets and is the flat
struct's one real hazard.

| Source | Validated | Rule |
|---|---|---|
| `Plane` | `subdivisions` | `>= 1` |
| `Cube` | — | always valid |
| `UvSphere` | `rings`, `segments` | `>= 3` |
| `Cylinder` | `segments` | `>= 3` |
| `Capsule` | `rings`, `segments`, `capsuleLengthRatio` | `>= 3`, `>= 3`, `>= 1.0` |

### Bounds

`BuildMeshData` computes a local AABB from the emitted vertices. This is an F3
item arriving early for a real reason — `MeshDocument`'s preview must frame the
mesh — and it is computed from vertices rather than derived analytically per
source because F2c's imported meshes need that path regardless. **Nothing culls
with it in F2a.**

---

## 2. The mesh material

`.arcmat` gains a third kind beside `fullscreen` and `sprite`, joining the
existing `MaterialSurface` enum (`Material/MaterialSource.cpp:321`). It carries
**two params and no snippet**:

| Param | Type | Status in F2a |
|---|---|---|
| `baseColor` | vec4, linear, may exceed 1 | consumed |
| `albedo` | Guid | **declared, not bound** |

`MeshMaterialCache` reads the param values directly and **never touches
`MaterialSource` or `ShaderCompiler`**. That is deliberate: it adds no second
compile-drain site, which is what keeps `SceneRenderResolver`'s single-`Drain()`
rule (`Host/SceneRenderResolver.hpp:22-28`) intact.

`albedo` is declared so that Phase 4 Task 8 has a real field to give meaning to
— the pivot's ruling that "a material asset design should precede or accompany
any further bindless work." A non-nil `albedo` produces one memoized diagnostic
naming Task 8 as where it gets bound. It is refused **loudly rather than ignored
silently**, because a texture slot that quietly does nothing is the worse
affordance.

**No `metallic`, no `roughness`.** `mesh.hlsl` is Lambert plus a flat ambient
term. Declaring PBR params nothing reads is precisely the guess-ahead that
Task 7's fix round already deleted ~135 lines for.

A `"mesh"`-kind material refuses `passes` (as `"sprite"` already does) and
ignores `snippet`/`graph` with one diagnostic. Custom mesh shading is F2b/Task 8
work, once there is a pipeline surface to compile a variant into.

---

## 3. The component

```cpp
struct MeshRenderer {
    Guid      mesh{};                        // .arcmesh; nil draws nothing
    Guid      material{};                    // nil = white; see below
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};  // multiplies the material baseColor
};
```

`tint` mirrors `SpriteRenderer::tint` and exists so one material asset serves
many entities at different colours — without it, every distinctly-coloured cube
needs its own `.arcmat`. It costs nothing on the GPU: `MeshInstance::baseColor`
already carries the product.

**A nil `material` is not an asset.** It resolves to `baseColor = (1,1,1,1)`
directly, with no lookup and no built-in Guid — so `baseColor` becomes `tint`
alone and a mesh with no material assigned still draws. There is deliberately no
"default material asset" file: one would have to live somewhere, be resolvable
from every project, and survive a project that deletes it. The nil case is a
branch, not a resource. Same shape as a nil `SpriteRenderer::material` falling
back to the plain sprite pipeline.

---

## 4. Scene → render

Following `SceneRenderResolver`'s established shape exactly. It already owns
three Guid→bindable caches and publishes them as `Runtime` resources that
submission systems read; `RenderSubmissionSystem` never touches the `Assets`
facade by design (`Scene/SceneResources.hpp:4`).

- **`MeshCache`** — Guid → owned `MeshData`, generated once. It owns the storage
  that `MeshInstance::mesh` borrows, so the "must outlive the `RenderFrame`
  call" contract (`nodes/MeshNode.hpp:103-107`) holds by construction rather
  than by discipline. Entries invalidate on asset change, matching
  `SpriteCache::Invalidate`; there is **no byte-budget eviction** in F2a, and
  that is a deliberate limit rather than an oversight — unit generators produce
  small buffers and a project holds tens of mesh assets, not thousands. F2c's
  imported meshes are what make eviction a real question, and they should be
  what answers it.
- **`MeshMaterialCache`** — Guid → `{ glm::vec4 baseColor; }`.
- **`MeshSubmissionSystem`** — sweeps `View<WorldTransform, MeshRenderer>` minus
  `Hidden`, filling a host-owned `std::vector<MeshInstance>`. `model` comes
  straight from `WorldTransform::matrix`; `baseColor` is
  `material.baseColor * tint`.
- **Task 9 host wiring** — `FrameDesc` gains the mesh scene and `depth`; both
  hosts populate it. Phase 4's plan already specifies this task step by step,
  including its structural test
  (`docs/plans/2026-08-21-nri-phase4-3d-slice.md:416-435`).

### The normal-matrix landmine

`MeshInstance::model` is documented "Rotation + translation + UNIFORM scale
only: mesh.hlsl transforms normals by the upper 3x3 rather than the inverse
transpose, which is exact for those and wrong for a non-uniform scale."

F1 gave `Transform` a `glm::vec3 scale` that the Inspector authors freely, so
scene-driven meshes hit this the first time anyone drags a scale handle. F2a
fixes it rather than propagating it: `MeshNode::Record` computes
`mat3(transpose(inverse(model)))` per instance into the constant arena it
already writes, and `mesh.hlsl` uses that for normals. `MeshInstance` stays pure
scene data — the fix lives entirely inside the node.

Task 7's existing `[pixel]` case must stay green (uniform scale is unchanged by
the fix); a new case pins non-uniform shading.

---

## 5. Camera pose

`ActivePerspectiveSceneCamera` stops being pinned. Today it builds its eye from
`vec2(worldMatrix[3])` at Z = 0 with orientation fixed at forward `(0,0,-1)`,
up `(0,1,0)` — deliberately, with F1's note reserving real pose for F4
(`Scene/SceneCamera.hpp:162-176`).

```
eye     =  vec3(m[3])
forward = -normalize(vec3(m[2]))
up      =  normalize(vec3(m[1]))
```

Orthonormalizing the world basis columns rather than `glm::quat_cast`, because
the world matrix carries scale. A degenerate (zero-scale) basis falls back to
today's pinned behaviour with one warning.

**The orthographic path is not touched.** F1's stated reason for deferring —
"wiring it early would silently re-frame every scene the moment one gained a
non-zero Z or a tilt" — is measurably inapplicable to the perspective lens:
there are exactly two authored `.arcscene` files in the tree
(`ReferenceProject/Content/scenes/main.arcscene` and Gacha's
`Game/Content/scenes/test.arcscene`), neither camera carries a `projection` key,
so both default to `Orthographic` — and the two sweeps are independent, with
`ActiveSceneCamera` skipping any camera whose projection is not `Orthographic`.

F4 still owns the interactive orbit/fly **controls**, which is the genuinely
large part of the camera story.

---

## 6. `MeshDocument`

A second consumer of `ShaderEditorDocument`'s proven pattern: its own offscreen
`NriGraphContext`, the `retireGraphPreview` hook, and device-less services
skipping preview resources in the constructor so headless tests drive the pure
halves while `Draw` is never called.

- **Preview** — one `MeshInstance` of the document's generated mesh under the
  stock directional light, camera framed from the local AABB.
- **Params** — a `MeshSource` dropdown plus that source's topology numbers,
  undo-bracketed through `EditGesture` the way `SpriteDocument::PushDataEdit`
  does.
- **Routing** — `RegisterFactory(".arcmesh", …)` in `EditorApp`, alongside the
  existing `.arcmat` and `.arcsprite` factories.
- **Creation** — an AssetBrowser context-menu entry, alongside "Create Sprite".

`MeshRenderer`'s two Guid fields inherit the Inspector's generic asset-pick
popup for free.

---

## 7. Explicitly not in F2a

| Deferred | To | Why |
|---|---|---|
| Cook pipeline, BC, `arccook.exe`, vendoring | F2b | Its own contract scopes slice 1 to textures |
| glTF import (cgltf, meshoptimizer) | F2c | Needs the cook arc under it |
| Bindless table; any bound texture on a mesh | Task 8, after F2b | Table *policy* falls out of how materials load |
| Frustum culling, draw sorting | F3 | The AABB lands; nothing consumes it |
| 3D gizmo, camera orbit/fly controls | F4 | |
| Declarative clear-op, unified world pass | F5 | |
| PBR params (metallic, roughness) | whenever a shading model reads them | `mesh.hlsl` is Lambert + ambient |

**Known-wrong-looking, documented rather than fixed:** until F5's declarative
clear-op, 3D draws *over* `Batcher2D` content — anything declared before
`batch2d` is erased by its mid-pass `CmdClearAttachments`, and anything after
composites on top. A scene holding both a sprite and a mesh will composite
wrong. That is F5's problem, and F2a must not paper over it with an ordering
hack.

`SpriteRenderer` stays 2D-positioned. The assessment's open question — does it
become a world quad in 3D space — stays open until F5, which is the phase that
resolves it properly rather than by an ordering rule.

---

## 8. Verification

Headless, in the existing gate:

- `.arcmesh` save→load round-trip; `BuildMeshData` determinism per source; the
  local AABB; refusal on each degenerate input, naming the field.
- Mesh-kind material resolution; the `albedo`-declared-unbound diagnostic fires
  once; `passes` refused.
- `SceneRenderResolver` publishes the mesh table; cache hit reuses the same
  `MeshData` pointer; an unresolvable Guid fails once and memoizes.
- `MeshSubmissionSystem` skips `Hidden`, skips a missing `WorldTransform`, skips
  a nil mesh; `model` equals `WorldTransform::matrix`; `baseColor` equals
  `material.baseColor * tint`.
- Perspective camera honours pose; degenerate basis falls back with a warning;
  the orthographic path is byte-identical.
- Task 9's `FrameDesc` structural test (no mesh scene → Task 4's frame; with one
  → Task 7's).
- `MeshDocument`'s pure halves: param edits, the undo bracket, the peek.

On the GPU: one `[gpu][pixel]` case for non-uniform-scale shading, and Task 7's
existing case stays green. Both are **desk-run** — the dev-loop gate is
`~[gpu]`, so a green loop gate says nothing about either.

**Plugin ABI 16 → 17** (new component, new exports), with `Aphelyon.arcproj` and
`ReferenceProject` restamped. **Scene schema stays v3** — a new name-keyed
component is purely additive.

---

## Exit criteria

1. A `.arcmesh` and a `"mesh"`-kind `.arcmat` can be created, edited, saved and
   reopened entirely inside the editor.
2. An entity carrying `MeshRenderer` renders in **both** hosts, positioned and
   oriented by its `Transform`, from a scene that was saved and reopened.
3. A non-uniformly-scaled mesh shades correctly.
4. A perspective camera entity can be posed **through its Transform in the
   Inspector** to frame that mesh. Interactive orbit/fly is F4 and is not part
   of this criterion.
5. Gate green in all three configs, 0 warnings, from a clean rebuild.
