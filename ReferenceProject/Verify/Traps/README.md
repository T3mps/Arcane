# The engine trap corpus

Engine-specific counterpart to `ThirdParty/playwright-fixtures` (Task 6's vendored
corpus). Where that corpus proves the comparator agrees with Playwright's own verdicts,
this one proves the comparator catches the regressions *this engine* actually produces --
a missing mesh, a wrong normal matrix, an unbound post chain -- and does not flag the one
difference that is legitimate variation, not a regression (cross-backend text rounding).

Each pair is `<name>-expected.png` (the correct/blessed state) and `<name>-actual.png`
(the deliberately broken or differently-configured state). `should-fail/` pairs must be
CAUGHT by `Arcane::CompareImages` (i.e. `.passed == false`); `should-match/` must PASS.
All renders are `ReferenceProject`, offscreen, D3D12, 1280x720, unless stated otherwise.

Provenance matters here more than in most fixture corpora: every `should-fail` pair below
was produced by a TEMPORARY, REVERTED edit to a committed file (the scene, a mesh asset,
or engine source). None of those edits are present in the working tree or history -- only
the resulting PNGs are committed. If a future trap needs regenerating, this file is the
recipe.

## should-fail/missing-mesh

**What was broken:** `ReferenceProject/Content/scenes/main.arcscene`'s `MeshCube` entity
had its `Arcane::MeshRenderer` component removed entirely (Identity + Transform kept).
Rendered, then the scene file was reverted (`git checkout --`) and the runtime's staged
copy restored to match.

**How:** `ArcaneRuntime.exe --project ReferenceProject --offscreen --backend dx12
--frames 60 --settle 30 --screenshot <name>.png` against the scene with (expected) and
without (actual) the component. Census confirms it: `meshBound`/`meshReferenced` go from
1/1 to 0/0, everything else (`spriteBound`, `postBound`) unchanged.

**What it proves:** the coarsest regression this gate exists to catch -- geometry that
silently stops drawing.

## should-fail/wrong-normal-matrix

**What was broken:** `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:867`'s call
site, temporarily changed from

```cpp
push.normalMatrix = PackedNormalMatrix(NormalMatrixFor(instance.model));
```

to

```cpp
push.normalMatrix = PackedNormalMatrix(glm::mat3(instance.model));
```

i.e. skipping `NormalMatrixFor`'s inverse-transpose and pushing the model's upper-3x3
directly -- reproducing the historical bug `NormalMatrixFor`'s own header comment in
`MeshNode.hpp` describes (correct for a uniform scale, wrong for a non-uniform one).
Reverted with `git checkout --` after rendering, followed by a rebuild that recompiled
`MeshNode.cpp` and relinked `ArcaneClient.dll` (verified in the build log, and by a
byte-identical sanity re-render against the pre-corruption baseline).

**A first attempt at this trap under a NON-uniform scale on the stock `MeshCube` (a box,
`reference_cube.arcmesh` with `"source": "cube"`) produced a BYTE-IDENTICAL image between
the correct and broken code paths** -- not a caught difference, an *invisible* one. The
reason is geometric, not a mistake in the render: a box's face normals are exactly
axis-aligned, and any *diagonal* scale matrix (which is what a `Transform.scale` vec3
always produces) maps an axis-aligned vector to a scalar multiple of itself whether you
apply the scale directly or its inverse transpose -- the two differ only in magnitude,
never in direction, and direction is all that survives normalization. No amount of added
rotation on that single Transform changes this, because the rotation is orthogonal and
commutes with the direction-preserving property. **The bug is only visible on a mesh
whose local-space normals are not eigenvectors of the scale**, i.e. not axis-aligned.

The trap therefore temporarily re-sourced the existing mesh asset
(`ReferenceProject/Content/meshes/reference_cube.arcmesh`, `"source"` field only) from
`"cube"` to `"uvsphere"` -- the file already carried `rings`/`segments` fields unused by
`"cube"` -- and gave `MeshCube`'s `Transform.scale` a non-uniform value (`[1.8, 0.5, 1.2]`,
reverted afterward alongside the mesh source). A UV sphere's radial normals are not
axis-aligned almost everywhere, so the wrong/right normal-matrix computations diverge in
direction, not just magnitude, and the images differ (176,783 vs 175,892 bytes; `cmp`
reports the first differing byte at offset 36 into the file, i.e. well into the pixel
data). Both the scene and mesh-asset edits were reverted with `git checkout --` alongside
the engine-source revert.

**What it proves:** F2a's subtlest fix -- shading changes under non-uniform scale,
silhouette does not (vertex positions use `model` directly; only the lighting term reads
the normal matrix). It also documents, empirically, exactly which geometry this bug needs
to be observable on -- a fact worth keeping for the next person who reaches for a box.

## should-fail/unbound-post

**Nothing was broken here.** This is a CENSUS-STATE trap, not a rendering bug: both
images are correct renders of the identical, unmodified, committed scene and engine.
They differ only in `--frames`, and that alone is enough to put them in different census
states -- which is precisely why `SceneRenderResolver`'s census (`meshBound`,
`spriteBound`, `postBound`) exists as a precondition before any comparison, not an
afterthought. Comparing across a mismatched census state manufactures a "regression" that
is actually just two different, both-correct moments of the same convergence process.

**How:** both renders are offscreen, D3D12, **no `--settle`** (deliberately -- see below):

```
ArcaneRuntime.exe --project ReferenceProject --offscreen --backend dx12 --frames 120 --screenshot unbound-post-expected.png
ArcaneRuntime.exe --project ReferenceProject --offscreen --backend dx12 --frames 60  --screenshot unbound-post-actual.png
```

Measured census (`--report`, same run):

| frames | postBound | meshBound | spriteBound |
|---|---|---|---|
| 60  | **false** | 1 | 1 |
| 120 | **true**  | 1 | 1 |

`--settle` is deliberately OMITTED, unlike every other trap in this corpus. `--settle`
renders until convergence, which for `--frames 60` would carry the run past the point the
post chain binds and destroy the very unbound state this trap exists to capture -- the
opposite of every other pair here, where `--settle` is what makes the capture
reproducible.

This is also why the pair is offscreen-only on both sides, unlike the original desk
measurement of this artifact (99.632% differing pixels, `docs/plans/2026-08-23-agent-
verification-offscreen-hosts.md`), which compared a **windowed** 60-frame capture against
an **offscreen** reference and so mixed the post-binding difference with an unrelated
`format=9` (offscreen) vs `format=11` (windowed) conversion difference (~36,288 pixels
measured elsewhere in that same session). Holding format constant (both offscreen) and
varying only frame count isolates the ONE variable this trap is named for.

**What it proves:** the census precondition is load-bearing, not decorative -- comparing
two genuinely-correct renders at different convergence points looks like catastrophic
failure and is entirely an artifact of not checking `postBound` first.

## should-match/one-pixel-text-shift

**Nothing was broken, and nothing was re-rendered.** Copied verbatim from Plan A's own
desk evidence:
`.superpowers/sdd/2026-08-23-agent-verification-offscreen-hosts/evidence/desk-2026-08-24/desk_off.png`
(dx12, -> `one-pixel-text-shift-expected.png`) and `desk_off_vk.png` (Vulkan, ->
`one-pixel-text-shift-actual.png`). Both are `ArcaneEditor.exe` offscreen captures at the
SAME fully-converged census (`meshBound`/`spriteBound`/`postBound` all true on both
sides, per their sibling `.json` reports) -- the only variable is graphics backend.

**What it proves:** 121 ImGui glyph pixels flip `(16,14,16)` <-> `(255,255,255)` between
D3D12 and Vulkan text rasterization, in both directions, confined to the ImGui overlay
region -- not the 3D scene. This is legitimate backend-specific glyph rounding, not a
rendering regression, and the gate must NOT flag it: a comparator that catches 121
one-off text pixels gets its budget loosened by whoever hits it next, until it stops
catching anything. The conformance test's `maxDiffPixels = 200` budget for this one pair
is derived from this measurement (121, rounded up), not chosen to make the test pass.
