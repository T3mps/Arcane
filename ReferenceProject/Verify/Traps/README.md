# The engine trap corpus

Engine-specific counterpart to `ThirdParty/playwright-fixtures` (Task 6's vendored
corpus). Where that corpus proves the comparator agrees with Playwright's own verdicts,
this one proves the comparator catches the regressions *this engine* actually produces --
a missing mesh, a wrong normal matrix, an unbound post chain -- and does not flag the one
difference that is legitimate variation, not a regression (cross-backend text rounding).

Each pair is `<name>-expected.png` (the correct/blessed state) and `<name>-actual.png`
(the deliberately broken or differently-configured state). `should-fail/` pairs must be
CAUGHT by `Arcane::CompareImages` (i.e. `.passed == false`); `should-match/` must PASS.
All renders are `ReferenceProject`, headless, D3D12, 1280x720, unless stated otherwise.

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

**How:** `ArcaneRuntime.exe --project ReferenceProject --headless --backend dx12
--frames 60 --settle 30 --screenshot <name>.png` against the scene with (expected) and
without (actual) the component. Census confirms it: `meshBound`/`meshReferenced` go from
1/1 to 0/0, everything else (`spriteBound`, `postBound`) unchanged.

**What it proves:** the coarsest regression this gate exists to catch -- geometry that
silently stops drawing.

**A byte-identity worth explaining, not hiding.** `missing-mesh-expected.png` is git-blob-
identical (`git hash-object` -> `203b6049...`) to `one-pixel-text-shift-expected.png`
(itself Plan A's `desk_off.png`, copied verbatim, see below). This is NOT a copy-paste
shortcut: both are independently-run `ArcaneRuntime.exe --project ReferenceProject
--headless --backend dx12 --frames 60 --settle 30` captures of the SAME unmodified,
committed scene, two days apart, on the same desk GPU. `--settle` freezes the simulated
clock once `--frames 60` is reached and only repeats the capture (never advancing sim time)
until two consecutive frames match byte-for-byte AND the shader compiler is idle -- so the
simulated content at convergence is pinned to "60 simulated frames of this exact scene"
regardless of how many settle repeats (67, 68, or Plan A's 62) it took to prove that
convergence. A GPU render with no randomness, no wall-clock-driven animation, and an
identical scene/engine/driver is a deterministic function of its inputs, so bit-identical
output across two independent runs is the expected result, not a coincidence. Verified
directly rather than argued: re-ran the exact same command a THIRD time, in a fresh process
on 2026-08-26, and its output (`git hash-object`) matched both existing files exactly. The
committed `missing-mesh-expected.png` is unchanged (replacing it with an identical file
would be a no-op); this paragraph and its fresh third-run proof are the resolution.

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

**How:** both renders are headless, D3D12, **no `--settle`** (deliberately -- see below):

```
ArcaneRuntime.exe --project ReferenceProject --headless --backend dx12 --frames 120 --screenshot unbound-post-expected.png
ArcaneRuntime.exe --project ReferenceProject --headless --backend dx12 --frames 60  --screenshot unbound-post-actual.png
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
`one-pixel-text-shift-actual.png`). Both are `ArcaneRuntime.exe` headless captures (its
own ImGui debug HUD -- `ImGui::Begin("ArcaneRuntime")` in `RuntimeFrame.cpp`'s `BuildHud`,
NOT `ArcaneEditor.exe` -- an earlier draft of this file misattributed the host) at the SAME
fully-converged census (`meshBound`/`spriteBound`/`postBound` all true on both sides, per
their sibling `.json` reports) -- the only variable is graphics backend.

**CORRECTED 2026-08-26, after review.** The first pass at this file described the 121
differing pixels as "ImGui glyph pixels" flipping under cross-backend text rasterization --
that was wrong about the mechanism, and the corrected picture is better news than the
original one. A pixel-level diff (both images, RGBA, exact) shows:

- **120 of the 121 pixels** sit in one 9-row band, `x 131-171, y 89-97` -- the HUD's
  `ImGui::Text("Backend: %s", ...)` line (`RuntimeFrame.cpp:292`). Cropped and zoomed, the
  two images read `Backend: D3D12` and `Backend: Vulkan` -- **two different words**, not one
  word antialiased two different ways. The colour histogram inside that band is a hard
  binary flip between background and glyph foreground (not partial-coverage blending), which
  is the fingerprint of different *characters*, not rounding on the same characters.
- **Exactly 1 pixel**, `(872, 241)`, is genuine cross-backend rendering variance: the two
  images differ by a single green-channel unit (164 vs 163) on a rotated sprite's edge --
  antialiasing coverage rounding a hair differently between the two rasterizers, the only
  pixel in the whole 1280x720 frame where that's true.

**So the corrected fact this trap demonstrates is: offscreen scene content is consistent
across D3D12 and Vulkan to within about ONE pixel, not 121** -- the renderer is far more
cross-backend-deterministic than Plan A's desk measurement implied. Plan A located the 121
pixels correctly (same coordinates, same report) but misattributed their cause; the label
text is the reason 120 of them exist, not glyph rasterization.

**Why 120, not 121, differs from Plan A's own count of "121 measured at the desk":** the
desk's 121 already included this same borderline pixel at `(872, 241)`. This corpus's
measured `diffCount` from `Arcane::CompareImages` is **120** because that one-unit
green-channel difference falls under the comparator's per-channel/perceptual threshold and
is not counted as "differing" by the comparator's own rules -- it is a real, tiny pixel
difference that the tool correctly treats as noise, not a different capture round or any
other unexplained cause.

**What it proves, and the risk that made this trap load-bearing rather than decorative:**
a debug HUD reporting a different literal backend name is exactly the kind of "difference"
a comparator must tolerate -- it is not a rendering regression, and a gate that flags it
gets its budget loosened by whoever hits it next, until it stops catching anything. But
that also means this trap's entire signal (120 of its 121 differing pixels) comes from an
incidental HUD string, not from the 3D scene the corpus is otherwise about. If that HUD
label is ever unified or removed (an ordinary, unrelated cleanup with no reason to think
about this test), `diffCount` collapses from 120 to ~1, `CHECK(passed)` keeps passing, and
the trap silently stops demonstrating anything -- the same failure class `wrong-normal-matrix`
almost shipped as a should-fail trap that could never fire, just arriving from the
should-match side instead. `ImageCompareConformanceTest.cpp`'s should-match loop therefore
also asserts `CHECK(result.diffCount > 50)` alongside `CHECK(result.passed)`: this pair must
keep genuinely differing by a comparator-visible amount, or it is no longer demonstrating
that a real difference can be tolerated. The conformance test's `maxDiffPixels = 200` budget
is unchanged, derived from Plan A's 121 (rounded up), not chosen to make the test pass.
