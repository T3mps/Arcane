# F3 Plan 2 — GPU cull, blend modes, and deterministic transparency

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` and implement one task at a time with a fresh implementer and a fresh task reviewer. Every behavior change follows `superpowers:test-driven-development`; preserve RED and GREEN evidence in the task report.

**Goal:** Replace plan 1's CPU-authored mesh visibility with a real compute cull, add opaque/masked/transparent plus independent two-sided material state, and render conventional transparency in deterministic order while preserving explicit later seams for Deadlock-style MBOIT and refraction.

**Architecture:** `GpuSceneSync` owns persistent row identity and material-driven batch membership. `BuildGpuSceneFrame` emits zeroed indirect arguments and full transparent draw records. `GpuSceneSyncNode` uploads rows and frame data, `MeshCullNode` writes per-batch row indices and instance counts, and `MeshNode` consumes indirect opaque/masked draws followed by direct ordered transparent draws. Offline shader artifacts and the shared pipeline cache provide blend/depth/cull variants. Delayed readback compares exact GPU row sets against the CPU oracle without a per-frame flush.

**Tech stack:** C++23, Astra registry/change tracking, NRI D3D12/Vulkan, HLSL compiled offline by `data/shaders/compile-shaders.bat`, Catch2/RapidCheck, Arcane render graph.

**Spec:** `docs/specs/2026-09-18-f3-visibility-and-gpu-scene-design.md`, especially §§5–10 and rulings R4, R5, R9.

## Non-negotiable contracts

- Arcane is right-handed, +Y up, and the camera looks down -Z. Transparent projected depth is `-viewSpaceCenter.z + translucencyDepthSortBias`; sort render order ascending, depth descending, then stable `(entity, mesh, section)` identity ascending.
- Conventional ordered transparency is the F3 baseline. Do not implement MBOIT or refraction in this plan; retain their named pass seams.
- `twoSided` is independent of blend mode and participates in the pipeline/batch key.
- Material state changes re-key a live row in the same sync; changing only GPU flags is incorrect.
- Every indirect `instanceNum` begins at zero each frame. The compute pass is its sole incrementer.
- The compute shader uses typed layouts mirroring C++ and guards `dispatchThreadId >= rowCount` before reading a row.
- Transparent draw records contain enough geometry data to draw without reconstructing mesh/section identity from the row id.
- Shader variants are offline artifacts. No runtime preprocessor-definition mechanism is invented.
- Oracle readback is delayed and asynchronous; no `WaitForIdle`, queue flush, or per-frame fence wait is added.
- `ResolvedMeshMaterial` and `MeshRenderer` change binary shape, so `kGamePluginABIVersion` becomes 38 and `ReferenceProject.arcproj` becomes 38. Gacha's currently stale ABI 31 is restamped only when its game module is rebuilt through its normal workflow.

## Baseline and common commands

Run from the isolated Arcane worktree unless a step names a different directory.

```powershell
cmd /c GenerateProjects.bat
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Arcane.slnx /p:Configuration=Debug /m
cd ReferenceProject
& '..\ThirdParty\premake5\premake5.exe' vs2026
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' ReferenceProject.slnx /p:Configuration=Debug /m
cd ..
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "~[gpu]" --reporter compact
```

The clean baseline is a zero-warning Debug build. The separate ReferenceProject build is required before `[witness][server]`; after staging it, all three server witness tests pass.

---

## Task 1: Material and component contract, editor authoring, ABI 38

**Ownership:**

- Create `ArcaneCore/src/Arcane/Material/MaterialBlendMode.hpp`.
- Modify `ArcaneCore/src/Arcane/Material/MaterialAsset.hpp` and `.cpp`.
- Modify `ArcaneCore/src/Arcane/Scene/SceneResources.hpp`.
- Modify `ArcaneCore/src/Arcane/Scene/Components.hpp` and the existing reflection/scene-serialization sites that enumerate `MeshRenderer` fields.
- Modify `ArcaneClient/src/Arcane/Render/MeshMaterialCache.cpp`.
- Modify `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp` and `.hpp` only where its material draft/state requires it.
- Modify `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` and `ReferenceProject/ReferenceProject.arcproj`.
- Tests: `ArcaneTests/src/MaterialAssetTest.cpp`, `MaterialTest.cpp`, `ShaderEditorDocumentTest.cpp`, `SceneJsonTest.cpp`, and focused ABI assertions where already established.

**Interfaces produced:**

```cpp
enum class MaterialBlendMode : std::uint8_t { Opaque = 0, Masked = 1, Transparent = 2 };

struct MaterialAssetData {
    // existing fields
    std::optional<MaterialBlendMode> blend;
    std::optional<float> alphaCutoff;
    std::optional<bool> twoSided;
};

struct ResolvedMeshMaterial {
    // existing fields
    MaterialBlendMode blend = MaterialBlendMode::Opaque;
    float alphaCutoff = 0.5f;
    bool twoSided = false;
};

struct MeshRenderer {
    // existing fields
    std::int32_t translucencyRenderOrder = 0;
    float translucencyDepthSortBias = 0.0f;
};
```

The asset fields are optional because material instances are sparse overrides.
Resolution starts from the concrete defaults in `ResolvedMeshMaterial`, then
applies base→leaf values. A missing base field therefore gets the documented
default, while a missing instance field inherits instead of resetting its base.

**Steps:**

- [ ] Add failing tests first for absent-field defaults, exact round-trip save/load, invalid blend refusal naming `blend`, out-of-range/non-finite cutoff refusal, base→instance inheritance, and independent `twoSided` inheritance/override.
- [ ] Add failing editor tests proving mesh materials author all three fields and the cutoff control clamps to `[0,1]`; non-mesh material surfaces do not expose the controls.
- [ ] Add failing scene serialization/reflection tests for both translucency fields and their defaults.
- [ ] Implement `MaterialBlendMode`, JSON load/save, inheritance in `MeshMaterialCache`, editor state/UI, and component reflection/serialization. Keep these as material metadata, not `MaterialSource` shader-stitching state.
- [ ] Raise `kGamePluginABIVersion` 37→38 with a concise ABI ledger entry and restamp `ReferenceProject.arcproj`.
- [ ] Run the focused tests, then build `ArcaneCore`, `ArcaneClient`, `ArcaneEditor`, and `ArcaneTests` through `Arcane.slnx`.

**Focused verification:**

```powershell
.\ArcaneTests.exe "MaterialAsset*,ShaderEditorDocument*,SceneJson*,Mesh material*" --reporter compact
```

**Commit:** `feat(material): add mesh blend and translucency contracts`

---

## Task 2: Re-key GPU rows and build deterministic transparent draw records

**Ownership:**

- Modify `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp`.
- Modify `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp`.
- Modify the HLSL mirror in `data/shaders/gpu_scene.hlsli` only for shared flag/layout constants; row size remains 240 bytes.
- Tests: `ArcaneTests/src/GpuSceneSyncTest.cpp` and a focused transparent-order test in the same CPU-only suite.

**Interfaces produced:**

```cpp
struct GpuBatchKey {
    Guid mesh;
    std::uint32_t section;
    MaterialBlendMode blend;
    bool twoSided;
};

struct TransparentDraw {
    std::uint32_t row;
    Guid mesh;
    std::uint32_t section;
    std::uint32_t indexOffset;
    std::uint32_t indexCount;
    MaterialBlendMode blend;
    bool twoSided;
    std::int32_t renderOrder;
    float projectedDepth;
    Astra::Entity entity;
};
```

`GpuSceneRow` stores the last resolved blend/cutoff/two-sided values so a material-only change is detected. `GpuSceneFrame` gains ordered `transparentDraws`; `HasDraws()` includes them. `GpuBatchDraw` carries `twoSided`.

**Steps:**

- [ ] Add failing CPU tests proving material-only Opaque→Masked, Masked→Transparent, and `twoSided` changes decrement the old batch count, assign the new key id, and stage the row once with correct flags/cutoff.
- [ ] Add failing tests proving transparent rows never populate indirect batches or `visibleIndices`, yet emit complete direct draw metadata for every visible section.
- [ ] Add failing order tests using a -Z camera: render order ascending; within one order, farther projected depth first; depth bias affects the key; identical depth resolves by entity/mesh/section identity; repeat builds produce byte-identical order.
- [ ] Implement independent flag bits for blend and two-sided, update material tracking, and perform row re-keying before staging.
- [ ] Change indirect args generation from plan 1's CPU `cursor[b]` count to zero. Preserve the CPU oracle row lists separately from device output so later tests can compare expected and actual contents.
- [ ] Build transparent records from resolved mesh sections and `MeshRenderer` controls; do not defer mesh/section lookup to `MeshNode`.
- [ ] Run focused CPU tests and the Debug build.

**Focused verification:**

```powershell
.\ArcaneTests.exe "GpuScene*" --reporter compact
```

**Commit:** `feat(render): rekey gpu rows and order transparent draws`

---

## Task 3: Offline mesh shader variants and pipeline state

**Ownership:**

- Modify `data/shaders/mesh.hlsl` and `data/shaders/compile-shaders.bat`.
- Modify `ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.hpp` and `.cpp` only as required to key depth-write and cull state rather than hiding those states in callbacks.
- Modify `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp` and `.cpp` for shader loading and `PipelineFor(blend, twoSided)`.
- Tests: `ArcaneTests/src/RenderGraphTest.cpp`, `MeshNodeTest.cpp`, and shader artifact assertions in `ShaderCompilerTest.cpp` if that is the existing home.

**Required variants:**

| Variant | Pixel artifact | Depth write | Blend | Cull |
|---|---|---:|---|---|
| opaque one-sided/two-sided | `mesh_ps` | yes | opaque | back/none |
| masked one-sided/two-sided | `mesh_masked_ps` | yes | opaque | back/none |
| transparent one-sided/two-sided | `mesh_transparent_ps` | no | alpha-over | back/none |

All variants share `mesh_vs`. Masked evaluates `alpha = baseColor.a * sampledAlbedo.a` and clips against `GpuInstance.boundsMax.w`. Transparent outputs that alpha. Opaque behavior remains bit-for-bit compatible.

**Steps:**

- [ ] Add failing key tests showing depth-write and cull mode differences cause distinct graphics-cache entries.
- [ ] Add failing mesh-node tests for the six blend×cull combinations and failure when a required artifact is missing.
- [ ] Split/guard HLSL entry points so the batch script emits fixed DXIL and SPIR-V artifacts; compile them before changing runtime loading.
- [ ] Extend the graphics pipeline key with explicit depth-write and cull state, give both safe default member initializers, update every key factory/caller, and stamp those fields centrally in `NriPipelineCache`.
- [ ] Load all mesh artifacts at `MeshNode::Create`, derive stable shader-pair ids from the actual variant identity, and select the correct key in `PipelineFor`.
- [ ] Preserve the existing descriptor layout and 8-byte root constants.
- [ ] Run `data\shaders\compile-shaders.bat`, focused tests, and the Debug build.

**Commit:** `feat(render): add offline mesh blend pipeline variants`

---

## Task 4: Production compute cull node and graph/resource wiring

**Ownership:**

- Create `data/shaders/mesh_cull.hlsl`.
- Create `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.hpp` and `.cpp`.
- Modify `data/shaders/compile-shaders.bat`.
- Modify `ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp` and `.cpp`.
- Modify `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp` and `.cpp`.
- Modify `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp` and `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp`.
- Modify render-graph comments in `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp`.
- Tests: `ArcaneTests/src/RenderGraphTest.cpp`, `MeshNodeTest.cpp`, and a new device-free `MeshCullNodeTest.cpp` for descriptors/dispatch math.

**GPU batch metadata:** upload one typed record for every stable batch key id, including non-emitted keys:

```cpp
struct GpuCullBatch {
    std::uint32_t firstOutput;
    std::uint32_t capacity;
    std::uint32_t argIndex;
    std::uint32_t emitted;
};
```

The compute shader reads `StructuredBuffer<GpuInstance>` and `StructuredBuffer<GpuCullBatch>`, writes `RWStructuredBuffer<uint> visibleIndices` and `RWStructuredBuffer<DrawIndexedArgs> args`; the HLSL argument struct mirrors the exact 20-byte C++ layout. Use `InterlockedAdd(args[argIndex].instanceNum, 1, oldCount)` with its required out parameter. `rowCount` and six widened frustum planes are compute constants.

**Steps:**

- [ ] Add failing graph tests for node order `gpuscene-sync → mesh-cull → mesh`, resource usages CopyDst→StorageWrite→IndirectArgs/ShaderRead, and the no-row/no-emitted-batch declaration rules.
- [ ] Add failing tests for dispatch group count `(rowCount + 63) / 64`, batch metadata for emitted/non-emitted/transparent keys, and zeroed args upload every frame.
- [ ] Extend each frame slot's GPU-scene resources with UAV-capable visible-index and args buffers plus batch metadata. Reuse/grow by the existing graveyard/fence discipline; do not allocate or wait inside record callbacks.
- [ ] Make `GpuSceneSyncNode` copy zeroed args, batch metadata, and any resized identity data before compute.
- [ ] Implement the compute pipeline layout, descriptor sets per frame slot, shader artifact loading, dispatch, and graph imports using actual `RenderGraph::AddNode(name, kind, setup, execute)` and `nri::CoreInterface` APIs.
- [ ] Wire `AddMeshCullNode` between sync and mesh. `MeshNode` reads compute output for indirect draws, then records `transparentDraws` directly with the per-record geometry and pipeline state.
- [ ] Keep a compile-time `kMeshCullEnabled` defaulting true; false still dispatches the compute shader but bypasses the frustum predicate, so compute remains the sole counter writer while producing identity/all-emitted results. It remains documented as owed to the cvar arc.
- [ ] Compile shaders, regenerate projects for the new `.cpp`, run focused tests, and build Debug.

**Commit:** `feat(render): add compute mesh culling pass`

---

## Task 5: Exact asynchronous oracle and visibility observability

**Ownership:**

- Add `ArcaneTests/src/GpuSceneCullTest.cpp`.
- Modify `ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp` and `.cpp` for an opt-in delayed debug readback ring.
- Modify `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` and `.cpp` plus the existing overlay/HUD site that already displays render statistics.
- Modify focused tests in `ArcaneTests/src/VerifyReportTest.cpp` and `NriHostFlavorTest.cpp` as needed.

**Oracle fixture:** two meshes, multiple sections, at least two rows per shared batch, boxes inside/outside/straddling all six planes, plus masked, transparent, and two-sided material variants. Expected opaque/masked row ids come from the CPU frustum predicate and stable batch membership; transparent rows are asserted absent from compute output.

**Steps:**

- [ ] Add failing serialization/unit tests for `visibility { total, coarseVisible, gpuVisible, batches, draws, transparentRows }` and delayed-readback state transitions.
- [ ] Add the GPU oracle test on D3D12 and Vulkan through the existing `GpuCapability` skip discipline. Compare each emitted batch's `instanceNum` and the exact set of row ids in `[firstOutput, firstOutput + instanceNum)`; assert ids belong to that batch, are in range, have no duplicates, and do not include stale tail entries.
- [ ] Implement an opt-in multi-slot readback ring. Copy args and visible indices after the compute pass, publish results only when the owning frame fence has already retired, and never stall the queue.
- [ ] Feed the most recently completed result into `VerifyReport` and the overlay. While no readback has completed, report GPU visibility as unavailable rather than copying CPU counts.
- [ ] Run CPU tests, then oracle tests on every locally available backend. A backend skip is acceptable only through the existing capability reason; a wrong row set is a blocker.

**Focused verification:**

```powershell
.\ArcaneTests.exe "GpuSceneCull*,VerifyReport*" --reporter compact
```

**Commit:** `test(render): verify gpu cull contents against cpu oracle`

---

## Task 6: End-to-end scenes, goldens, documentation, and final verification

**Ownership:**

- Add the smallest ReferenceProject material/scene fixtures required by the existing verification harness.
- Modify golden/reference metadata through the repository's existing bless workflow only.
- Finalize comments in `RenderGraph.hpp`, `MeshNode.hpp`, and the F3 spec where implementation names differ.
- Do not reorganize `Nri/nodes`; this plan adds the threshold-crossing pass but a directory move is unrelated churn and remains a separate maintenance item.

**Required visual cases:**

- A box straddling a frustum plane remains visible.
- Opaque and masked materials preserve depth-writing order.
- Two overlapping transparent meshes prove far-to-near ordering.
- Equal-depth transparent meshes prove stable identity order.
- A nonzero render order and depth bias visibly override ordinary depth ordering.
- One-sided and two-sided variants prove culling is independent of transparency.

**Steps:**

- [ ] Add/extend ReferenceProject fixtures without changing unrelated existing golden compositions.
- [ ] Run the existing headless settle/compare lanes first; inspect any diff before blessing. Bless only the intentionally new reference names.
- [ ] Run Debug build, all `~[gpu]` tests, `[witness][server]`, the GPU oracle on available D3D12/Vulkan backends, and the relevant golden lanes.
- [ ] Rebuild `ReferenceProject` against ABI 38 and restage the solution; confirm all server witness cases pass.
- [ ] Search for stale plan assumptions: `MaterialSource` owning blend metadata, transparent-implies-two-sided, CPU-authored nonzero indirect counts, runtime shader defines, or projected-Z described as the permanent solution. Remove or correct each occurrence.
- [ ] Record final test counts, backend availability/skips, golden diff counts, and the pass-type inventory in the SDD report.

**Commit:** `test(render): cover culling and transparency end to end`

---

## Definition of done

- Material files round-trip and inherit blend/cutoff/two-sided state; editor authors it; ABI is 38.
- GPU rows re-key on material state changes and their HLSL/C++ layouts agree.
- Indirect counts start at zero and the compute node alone fills them.
- Opaque/masked draw indirectly; transparent draws directly in the specified deterministic order.
- One-sided/two-sided, depth-write, and blend differences are explicit pipeline-cache state.
- D3D12 and Vulkan oracle runs pass where the backend is available, comparing exact row contents rather than counts alone.
- Observability uses delayed readback and never stalls every frame.
- MBOIT and refraction remain named later seams, not accidental F3 scope.
- Final Debug build and all applicable CPU, witness, GPU, and golden tests pass.
