# NRI Phase 4 — the 3D slice

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development`
> (recommended) or `superpowers:executing-plans` to implement this task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put a lit, textured, depth-tested 3D mesh on screen through the NRI
frame graph, with bindless material indexing — and close the render-layer gaps
that have no other natural home, each with a real consumer in the same slice.

**Architecture:** Two standalone infrastructure tasks first (device capability
queries, a compute pipeline cache path) because they carry zero design risk and
nothing to guess. Then the slice proper, in dependency order: widen the graph's
texture description, light up the depth path that is already plumbed, add a
perspective camera, generate geometry procedurally, render it, then index its
materials bindlessly. Procedural geometry is deliberate — it keeps this slice
inside the renderer and out of the asset pipeline, which is its own arc
(`docs/research/2026-08-21-asset-cook-pipeline-design.md`).

**Tech stack:** NRI v180 (D3D12 + Vulkan), HLSL 6.5 dual-compiled to DXIL +
SPIR-V, glm, Catch2. **No new vendored libraries** — verified against the
target contract; the first vendoring is MikkTSpace at T1.

---

## Global Constraints

- **`ARCANE_SDK` must be set per-invocation.** The process value can be stale
  even when the User-scope value is correct: `export ARCANE_SDK="D:\\dev\\starworks\\Arcane"`.
- **Build `Arcane.slnx`, never a bare `.vcxproj`** — a project file defaults to
  Win32 and fails MSB8013.
- **Build all three configs.** A Dist-only break hides behind
  `#if !defined(ARCANE_DIST)`. Run Dist in its own MSBuild call; three-in-one
  exceeds a 10-minute wall.
- **Gate baseline at plan start: Debug 47519/995 · Release 47519/995 ·
  Dist 47458/990.** Every task states what it does to these numbers. A task that
  moves them without saying so is a defect.
- Run the gate **from each config's own exe dir**:
  `bin/{Debug,Release,Dist}-windows-x86_64-md/ArcaneTests` → `ArcaneTests.exe "~[gpu]"`.
- **`[gpu]` tests do not run in an agent session** — machine hazard (windowed
  d3d12 SIGSEGVs under Parsec). Agent runs prove they compile and link; the desk
  checkpoint runs them.
- **A green gate proves nothing about either host.** `ArcaneTests` compiles
  neither `EditorApp.cpp` nor `RuntimeApp.cpp`. Anything touching host frame
  order is desk-verified or unverified.
- **Never re-capture a golden to make a compare pass.** A delta is a defect
  until proven otherwise.
- Shader artifacts: add entry points to `data/shaders/compile-shaders.bat`. The
  stem suffix must agree with the entry prefix (`_vs`→`vs_main`, `_ps`→`ps_main`,
  `_cs`→`cs_main`) per `ShaderConventions.hpp`; SPIR-V register shifts are
  `-fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0`.

## Out of scope, deliberately

| Item | Why |
|---|---|
| **Async compute / a second queue** (gap B) | Deadlock's own default path is DX11 — no async compute. It is a *performance* feature with no 3D frame to profile yet, and it reworks the graveyard's one-timeline-per-graph fence model. **Trigger to revisit: a profiled 3D frame showing a raster bubble worth filling.** |
| Mesh import, `.arcmesh`, cgltf | The asset cook arc. This slice is procedural. |
| Mesh shaders | `NRIMeshShader.h` is vendored and available; no consumer yet. |
| Texture arrays / 3D textures / cubemaps in `RgTextureDesc` | Task 3 adds only `mipCount`, because that is all this slice consumes. The rest arrives with T2/T3, each with a consumer. |
| Lighting beyond one directional light | T1 owns the cluster grid and the forward-vs-deferred decision. |
| `nodes/` domain reorg | **Trigger does not fire here** — 6 pass types today, 7–8 after this, against a ~10 threshold; `MeshNode` is a new file so no existing `.cpp` crosses ~2000 lines. Stays keyed to T1. |

---

## File structure

**Created**

| File | Responsibility |
|---|---|
| `ArcaneClient/src/Arcane/Render/Nri/NriDeviceCaps.hpp` | POD snapshot of the device's tiers/features, queried once |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.{hpp,cpp}` | The opaque-geometry raster pass |
| `ArcaneClient/src/Arcane/Render/MeshBuilder.{hpp,cpp}` | Pure procedural geometry (cube, UV sphere) — no device |
| `ArcaneClient/src/Arcane/Render/Nri/BindlessTable.{hpp,cpp}` | Descriptor-array ownership + slot allocation (NRI-coupled — takes `NriDevice&`, hands out `nri::Descriptor*` slots) |
| `data/shaders/mesh.hlsl` | `vs_main` / `ps_main` for the lit textured mesh |
| `ArcaneTests/src/NriDeviceCapsTest.cpp` | caps snapshot |
| `ArcaneTests/src/MeshBuilderTest.cpp` | pure geometry properties |
| `ArcaneTests/src/PerspectiveCameraTest.cpp` | pure matrix properties |
| `ArcaneTests/src/BindlessTableTest.cpp` | slot allocation, capacity refusal |

**Modified**

| File | Change |
|---|---|
| `Render/Nri/NriDevice.{hpp,cpp}` | query + expose `Caps()` |
| `Render/Nri/NriPipelineCache.{hpp,cpp}` | `ComputeKey` + `GetCompute` |
| `Render/Nri/RenderGraph.{hpp,cpp}` | `RgTextureDesc::mipCount` |
| `Render/Nri/RenderGraphExec.cpp` | honour `mipCount` at realize + view creation |
| `Render/Nri/NriGraphContext.{hpp,cpp}` | `FrameDesc`/`RgFrameShape` gain the mesh scene + depth; `DeclareGraphFrame` declares `MeshNode` |
| `Scene/Components.hpp` | `Camera` gains `projection` + `fovYDegrees` + `nearZ`/`farZ` |
| `Scene/SceneCamera.hpp` | perspective view/projection alongside the ortho path |
| `data/shaders/compile-shaders.bat` | `mesh_vs` / `mesh_ps` entries |
| `ArcaneTests/src/RenderGraphTest.cpp` | mip + depth + mesh-frame declaration cases |
| `ArcaneTests/src/NriGraphPixelTest.cpp` | `[gpu][pixel]` mesh + bindless cases |
| `premake5.lua` | new source dirs if not glob-covered |

---

## Task 1: The device capability snapshot

Closes gap **E**. Standalone: no 3D consumer needed, nothing to guess.

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/Nri/NriDeviceCaps.hpp`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriDevice.hpp`, `NriDevice.cpp`
- Test: `ArcaneTests/src/NriDeviceCapsTest.cpp`

**Interfaces produced:**
```cpp
namespace Arcane
{
    struct NriDeviceCaps
    {
        std::uint8_t bindlessTier   = 0;   // nri::DeviceDesc::tiers.bindless
        std::uint8_t rayTracingTier = 0;   // tiers.rayTracing
        bool         meshShader     = false;
        std::uint32_t maxDescriptorSetTextures = 0;
        [[nodiscard]] bool SupportsBindless() const noexcept { return bindlessTier > 0; }
    };
}
// NriDevice gains:  [[nodiscard]] const NriDeviceCaps& Caps() const noexcept;
```

**Why it exists now rather than at T2:** `features.rayTracing` is what T2's baker
must gate on, and `tiers.bindless` is what Task 8 must gate on. Today nothing
reads either — `GetDeviceDesc` is called in six places, all for alignments. A
missing gate becomes a hard crash on the first machine without the feature.

- [ ] **Step 1: Write the failing test**

```cpp
// ArcaneTests/src/NriDeviceCapsTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

TEST_CASE("nri device caps: the NONE backend reports a coherent snapshot", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    const Arcane::NriDeviceCaps& caps = device->Caps();
    // NONE answers every query with defaults; what is pinned is that the
    // snapshot is POPULATED (not left at construction defaults by a missed
    // query) and internally consistent.
    CHECK(caps.SupportsBindless() == (caps.bindlessTier > 0));
    CHECK(caps.maxDescriptorSetTextures == device->Core()
              .GetDeviceDesc(device->Device()).shaderStage.descriptorSet.textureMaxNum);
}
```

- [ ] **Step 2: Run it and confirm it fails**

`ArcaneTests.exe "[nri]"` from the Debug exe dir. Expected: compile error —
`Caps` is not a member of `NriDevice`.

- [ ] **Step 3: Add `NriDeviceCaps.hpp` and populate it in `FinishWrap`**

Query once, immediately after `GetQueue` succeeds, and store on the device.
Populate every field from `nri::DeviceDesc`; do not leave a field defaulted.

- [ ] **Step 4: Log a one-line identity banner**

Extend the existing `LogNriIdentity` call site so a run states what the device
can do. One line, `[nri]` tag, alongside the existing identity log.

- [ ] **Step 5: Run the test, then the full gate**

Expected: PASS. **Gate moves: +2 assertions / +1 case in all three configs.**

- [ ] **Step 6: Commit** — `feat(nri): snapshot device capability tiers at wrap`

---

## Task 2: `NriPipelineCache::GetCompute`

Closes gap **C**. Standalone: the shape is fully determined by its `GetGraphics`
sibling, so there is nothing to guess.

**Files:** Modify `Render/Nri/NriPipelineCache.{hpp,cpp}`; test in
`ArcaneTests/src/RenderGraphTest.cpp` beside the existing `NriPipelineCache` cases.

**Interfaces produced:**
```cpp
struct ComputeKey
{
    std::uint64_t shaderId = 0;                  // same id space as GraphicsKey::shaderPairId
    std::uint32_t layoutId = kInvalidLayout;     // kInvalidLayout is REFUSED
    [[nodiscard]] bool operator==(const ComputeKey&) const noexcept = default;
};
[[nodiscard]] nri::Pipeline* GetCompute(const ComputeKey& key,
                                        const std::function<void(nri::ComputePipelineDesc&)>& fill);
```

**Context:** `NodeKind::Compute` already exists on the graph and is exercised
only in tests. The tree's one real compute pipeline is the deliberate GPU-fault
injector (`NriDiagnostics.cpp`), hand-built outside the cache. T1's cluster grid
is compute-built and will be the first production consumer.

- [ ] **Step 1: Write the failing test** — mirror the existing graphics cases:
  a second `GetCompute` with an equal key must NOT invoke `fill` again
  (count invocations through a captured counter); `kInvalidLayout` must return
  null without invoking `fill`.
- [ ] **Step 2: Run it and confirm it fails** (`GetCompute` undeclared).
- [ ] **Step 3: Implement**, mirroring `GetGraphics`: same layout lookup, same
  re-stamp discipline (the fill callback owns transient `stages` storage in a
  scope enclosing the call — see the header's rule 2), same graveyard burial in
  `Clear`.
- [ ] **Step 4: Run the test, then the full gate.**
  **Gate moves: +N assertions / +2 cases** — state the exact numbers from the run.
- [ ] **Step 5: Commit** — `feat(nri): compute pipelines join the cache`

---

## Task 3: `RgTextureDesc` gains `mipCount`

Closes gap **A** *as far as this slice consumes it*. The header's own comment
says mip/layer/sample counts are absent by YAGNI; this adds exactly the one the
slice needs and leaves array/3D/cube for T2/T3, each with its own consumer.

**Files:** `Render/Nri/RenderGraph.hpp` (`RgTextureDesc`), `RenderGraphExec.cpp`
(realize + view creation), tests in `RenderGraphTest.cpp`.

**Interface change:**
```cpp
struct RgTextureDesc
{
    nri::Format   format = nri::Format::UNKNOWN;
    std::uint32_t width = 0, height = 0;
    std::uint32_t mipCount = 1;          // NEW. 1 = no chain. 0 is REFUSED at Compile.
    bool          depthStencil = false;
};
```

- [ ] **Step 1: Write the failing tests** — (a) a transient declared with
  `mipCount = 4` reports 4 mips on the realized `nri::TextureDesc`;
  (b) `mipCount = 0` makes `Compile` fail with a message naming the texture;
  (c) **pool aliasing respects mip count** — two transients identical except for
  `mipCount` must NOT share a pool slot.
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Plumb `mipCount`** into the realize path's `nri::TextureDesc`,
  into the pool-slot compatibility key, and into attachment-view creation
  (a colour view targets mip 0).
- [ ] **Step 4: Run the tests, then the full gate.** State the delta.
- [ ] **Step 5: Commit** — `feat(rendergraph): transient textures can carry a mip chain`

---

## Task 4: Depth in a real frame

Closes gap **F**. `RgUsage::DepthWrite`, `SetDepthAttachment` and the barrier
mapping all already exist and no production node uses them. This gives them a
consumer.

**Files:** `NriGraphContext.{hpp,cpp}` (`RgFrameShape` gains `depth`),
`RenderGraphTest.cpp`.

- [ ] **Step 1: Write the failing test** — with `shape.depth = true`,
  `DeclareGraphFrame` creates a depth transient, the mesh pass declares it via
  `SetDepthAttachment`, and the compiled node reports a `DEPTH_STENCIL_ATTACHMENT`
  barrier. With `depth = false` the frame is byte-for-byte the current one
  (same node count, same pool slot count) — the established "not asking costs
  nothing" property every frame-shape case in this file pins.
- [ ] **Step 2: Run and confirm it fails.**
- [ ] **Step 3: Add the depth transient** to `DeclareGraphFrame`, gated on
  `shape.depth`. Format `nri::Format::D32_SFLOAT`.
- [ ] **Step 4: Run the tests, then the full gate.** State the delta.
- [ ] **Step 5: Commit** — `feat(nri): the frame can carry a depth attachment`

---

## Task 5: The perspective camera

Closes the camera half of gap **G**. `Camera` is orthographic-only today
(`orthographicSize`, half-height in METERS) and `SceneCameraView` is
`glm::vec2`-shaped throughout, so this is genuinely new, not a tweak.

**Files:** `Scene/Components.hpp`, `Scene/SceneCamera.hpp`,
`ArcaneTests/src/PerspectiveCameraTest.cpp`.

**Interface:**
```cpp
enum class CameraProjection : std::uint8_t { Orthographic, Perspective };
// Camera gains:
CameraProjection projection    = CameraProjection::Orthographic;  // default preserves 2D
float            fovYDegrees   = 60.0f;
float            nearZ         = 0.1f;    // meters (MKS)
float            farZ          = 1000.0f;
```

Reflection: add `ASTRA_REFLECT_FIELD` rows beside the existing
`orthographicSize` row so the inspector shows them.

**Units:** meters, per the engine's MKS decision. `nearZ`/`farZ` are meters.

- [ ] **Step 1: Write the failing tests** (pure, no device):
  a point at `-nearZ` maps to NDC z≈0 and `-farZ` to z≈1 (reverse-Z is NOT used
  — state that explicitly so a later change is deliberate); aspect ratio scales
  x only; a 90° fov puts a point at `(z, 0, -z)` exactly on the right clip plane;
  **`projection` defaults to Orthographic so every existing 2D scene is
  unchanged.**
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Implement** `SceneCamera`'s perspective branch returning
  view + projection matrices. Keep the ortho path byte-identical.
- [ ] **Step 4: Run the tests, then the full gate.** State the delta.
- [ ] **Step 5: Commit** — `feat(scene): Camera can project perspectively`

---

## Task 6: Procedural geometry

**Files:** `Render/MeshBuilder.{hpp,cpp}`, `ArcaneTests/src/MeshBuilderTest.cpp`.

Pure and device-free — the same discipline `GraphGridPhase` follows, and what
lets the whole thing be driven headlessly.

**Interface:**
```cpp
struct MeshVertex { glm::vec3 position; glm::vec3 normal; glm::vec2 uv; };
struct MeshData   { std::vector<MeshVertex> vertices; std::vector<std::uint32_t> indices; };

[[nodiscard]] MeshData BuildCube(float sizeMeters);
[[nodiscard]] MeshData BuildUvSphere(float radiusMeters, std::uint32_t rings, std::uint32_t segments);
```

- [ ] **Step 1: Write the failing tests** — cube has 24 vertices / 36 indices
  (per-face normals require split vertices); every index is in range; every
  normal is unit length; every cube vertex lies on the box of the given size;
  every sphere vertex is `radius` from the origin within 1e-5; sphere winding is
  consistent (every triangle's geometric normal agrees with its vertex normals).
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Implement both builders.**
- [ ] **Step 4: Run the tests, then the full gate.** State the delta.
- [ ] **Step 5: Commit** — `feat(render): procedural cube and sphere geometry`

---

## Task 7: `MeshNode` — the opaque pass

**Files:** `Render/Nri/nodes/MeshNode.{hpp,cpp}`, `data/shaders/mesh.hlsl`,
`data/shaders/compile-shaders.bat`, `NriGraphContext.cpp`, `RenderGraphTest.cpp`.

Follow the established node lifecycle exactly — `Batch2DNode` is the template:
`static std::unique_ptr<MeshNode> Create(NriGraphContext&)` → `Prepare(...)` →
`Record(RenderGraphNodeContext&, ...)` → `Release(Graveyard&, fence)`.

`mesh.hlsl` is a single directional light, Lambert diffuse plus a constant
ambient term, one albedo texture. **Deliberately not PBR** — the PBR/GGX
material model is T1's, and putting a half-version here would create a second
one to reconcile.

- [ ] **Step 1: Write the failing structural test** — with a mesh scene present,
  `DeclareGraphFrame` emits `mesh` before `tonemap`, the mesh node writes the
  canvas as `ColorWrite` and the depth as `DepthWrite`, and the derived barrier
  chain matches. With no mesh scene the frame is byte-for-byte Task 4's.
- [ ] **Step 2: Run and confirm it fails.**
- [ ] **Step 3: Author `mesh.hlsl`** and add `mesh_vs` / `mesh_ps` to
  `compile-shaders.bat`. Dual DXIL + SPIR-V; `#if SPIRV` blocks for the
  push-constant/cbuffer split, matching `sprite.hlsl`'s shape.
- [ ] **Step 4: Implement `MeshNode`** — vertex/index buffer upload through
  `NriUploadRing`, pipeline through `NriPipelineCache::GetGraphics` with
  `depthFormat = D32_SFLOAT`, per-frame camera constants.
- [ ] **Step 5: Run the structural tests, then the full gate.** State the delta.
- [ ] **Step 6: Write the `[gpu][pixel]` case** in `NriGraphPixelTest.cpp` on an
  offscreen `NriGraphContext` + `ReadCapture`: a cube at the origin under a
  known light covers the centre pixel; a corner pixel stays background;
  **the face nearer the camera occludes the farther one** (that is the assertion
  that proves depth, and nothing else in the suite can).
  Colour is asserted **structurally** (relations that survive any sane tonemap
  curve), never as literal bytes — the file's own banner explains why.
- [ ] **Step 7: Confirm the `[gpu]` case compiles and links** (it does not run
  in an agent session). Commit — `feat(nri): the frame graph renders opaque meshes`

---

## Task 8: The bindless material table

Closes gap **D**, with the consumer that makes it more than unrun code.

**Files:** `Render/Nri/BindlessTable.{hpp,cpp}`, `MeshNode.{hpp,cpp}`,
`mesh.hlsl`, `ArcaneTests/src/BindlessTableTest.cpp`, `NriGraphPixelTest.cpp`.

**Gate on `NriDeviceCaps::SupportsBindless()`** (Task 1). On a device that
reports tier 0, refuse loudly at node creation rather than producing wrong
pixels — the house philosophy.

**Interface:**
```cpp
class BindlessTable
{
public:
    static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;
    [[nodiscard]] static std::unique_ptr<BindlessTable> Create(NriDevice&, std::uint32_t capacity);
    [[nodiscard]] std::uint32_t Add(nri::Descriptor* srv);   // kInvalidSlot when full
    void Release(Graveyard&, std::uint64_t fence);
};
```

- [ ] **Step 1: Write the failing unit tests** (NONE backend) — slots are
  allocated densely from 0; `Add` past capacity returns `kInvalidSlot` and warns
  once rather than asserting; `Create` with capacity 0 is refused.
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Implement** the descriptor array + slot allocator.
- [ ] **Step 4: Extend `mesh.hlsl`** to take a per-instance material index and
  sample `Texture2D` from the bindless array by that index.
- [ ] **Step 5: Run the unit tests, then the full gate.** State the delta.
- [ ] **Step 6: Write the `[gpu][pixel]` case that actually proves it** — four
  cubes, four distinct solid-colour generated textures, each instance indexing a
  different slot. Assert **each cube's centre pixel is dominated by its own
  texture's channel**. A binding bug that ignores the index makes all four the
  same and this fails. This is the case that makes bindless proven rather than
  merely present.
- [ ] **Step 7: Commit** — `feat(nri): mesh materials index a bindless table`

---

## Task 9: Host wiring

**Files:** `NriGraphContext.hpp` (`FrameDesc`), `NriGraphContext.cpp`
(`DeclareGraphFrame`), `ArcaneRuntime/src/RuntimeFrame.cpp`,
`ArcaneEditor/src/App/EditorAppFrame.cpp`.

`FrameDesc` gains the mesh scene and `depth`, following the existing convention
exactly: **a null/absent field is how a caller asks for the frame WITHOUT that
pass**, which is the mechanism every frame-shape case in `RenderGraphTest.cpp`
pins. Both hosts populate it from the scene.

- [ ] **Step 1: Write the failing structural test** — a `FrameDesc` carrying no
  mesh scene produces Task 4's frame exactly; carrying one produces Task 7's.
- [ ] **Step 2: Run and confirm it fails.**
- [ ] **Step 3: Add the fields and the `DeclareGraphFrame` gate.**
- [ ] **Step 4: Wire both hosts.** The editor path goes through
  `ArmGraphViewportFrame`; the runtime path through `RuntimeFrame::RenderGraph`.
- [ ] **Step 5: Run the gate in all three configs.** State the delta.
- [ ] **Step 6: Commit** — `feat(host): both hosts submit a mesh scene`

---

## Task 10: DESK CHECKPOINT D4 (USER)

**Do not attempt, simulate, or mark this done. Stop and hand off.**

No GPU or windowed runs in an agent session. Everything below is the half no
automated signal in this repo can reach.

- [ ] Run `ArcaneTests.exe "[pixel]"` from the Debug **and** Release exe dirs.
      Tasks 7 and 8's cases are UNRUN as committed. **A first-run failure is
      likelier to be a wrong expectation in the test than a renderer defect —
      read it before believing it.**
- [ ] Drive the Release editor (`--project ReferenceProject`): the mesh renders,
      depth-sorts correctly when the camera orbits, and the four bindless cubes
      show four distinct textures.
- [ ] Confirm the 2D path is untouched — sprites, the selection outline in Edit
      **and** Play, and the game UI in Play all still composite.
- [ ] Confirm all 12 runtime goldens still compare at maxDelta 0. **A 3D slice
      that changed a 2D pixel is a defect.**
- [ ] Both hosts load their game module on both backends.

---

## Verification ladder (Phase 4 exit criteria)

1. All three configs build with **0 warnings**.
2. The gate passes in all three configs, at the number this plan's tasks
   accumulate to — **derived by running it, never recalled**.
3. `verify.py`-style discipline does not apply here (this phase changes code);
   instead every task states its own gate delta and the sum is checked at the end.
4. The 12 runtime goldens compare at maxDelta 0.
5. `[pixel]` cases pass at the desk in Debug and Release.
6. Both hosts load their module in all three configs.
7. The drive items above are confirmed by hand.

## Carried into T1 / later

- **Gap B (async compute).** Deferred with a stated trigger — see "Out of scope".
- **`RgTextureDesc` is still 2D-shaped** beyond `mipCount`: no array layers, no
  3D, no cube, no sample count. T2 (probe volumes, paged lightmap arrays) and T3
  (the 144-entry cubemap array) each bring their own consumer.
- **Framework header size.** `NriGraphContext.hpp` (1385) and `RenderGraph.hpp`
  (1103) are included by everything under `Nri/`, and this phase adds to both.
  The reorg memo calls this out as worth addressing *before* the `nodes/` reorg.
- **`nodes/` domain reorg** stays keyed to T1 — the trigger does not fire here.
- **The forward-vs-deferred decision is still owed at T1 spec time.** This slice
  is deliberately neutral: one opaque pass writing a colour target and a depth
  target is the shared prefix of both designs.
- **GPU pixel coverage** for the pre-existing 2D passes (outline JFA pixels,
  pick GPU-side, post-chain end-to-end, ImGui through `ImGuiNriNode`) remains
  open from the Phase 5a carry list. This phase adds coverage for the new work
  only; it does not discharge that debt.
