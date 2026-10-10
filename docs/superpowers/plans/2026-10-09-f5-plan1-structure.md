# F5 Plan 1 — Structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the frame a declarative clear, a post-tonemap overlay pass, and one extract stage that publishes an immutable `RenderFramePacket`, while world sprites and world meshes stay pixel-identical.

**Architecture:** Plan 1 does not merge the two world nodes. `batch2d` stays the first world node and clears the canvas with its colour load-op; `mesh` stays next and loads colour while its depth load-op clears to the one depth-convention value. Empty G-buffer, lighting, and TAA nodes are declared and record nothing. After tonemap, one overlay pass samples scene depth and draws the 2D grid, the folded 3D grid, physics debug, the camera rect, and the gizmo. Extract, on the game thread, is the only render-side registry reader; it fills a packet ring of `FramesInFlight()` slots and returns before any GPU work. Sprite submission uses the host's already-registered Batcher2D; extract copies that batcher's drained spans and does not construct one. The GPU never reads packet memory. After Task 9 the overlay pass reads `Packet()->overlay` when a packet is bound, and after Task 8 the outline reads `Packet()->selectedIds` the same way. `FrameDesc::overlay` and `FrameDesc::grid` remain the path only for a frame that did not call `BindPacket`.

**Tech Stack:** C++23, premake5, MSBuild (VS 2026), Catch2 v3, NRI (D3D12 + Vulkan), HLSL via DXC, Astra ECS, PowerShell.

**Spec:** `docs/superpowers/specs/2026-10-09-f5-unified-world-pipeline-design.md`. This plan implements section 8.1 and every ruling that lands in plan 1 (sections 3.1–3.4, 3.7, 4.1–4.4, 7.1). Where this plan and the spec disagree, the spec wins; stop and report.

**Revision:** rev 4, 2026-10-10. Grok review GX1–GX5, on top of rev 3.1. The cache and debug-draw records move into `SceneCacheRecords.hpp` so `MeshCache`, `MeshMaterialCache`, `SpriteCache`, and `PhysicsDebugDraw` preprocess without `Ecs.hpp` or `Registry.hpp`. `PickEmit.cpp` also drops `PhysicsComponents.hpp` and `SceneResources.hpp`. The overlay pass reads the bound packet (2D grid, 3D grid, physics debug, gizmo) through its own `b0`/`t0`/`s0` layout and a `TextureView::TEXTURE` depth SRV. The host-gate scanner strips comments and matches whole words. The editor selection loop writes `packet.selectedIds` and the outline reads that field. PL1, PL2, the camera type, the ImGui snapshot, and `RendererHasTextures` stay as the earlier rounds closed them.

## Global Constraints

- **Worktree:** `D:\dev\starworks\Arcane-f5`, branch `feat/f5-unified-world`. Never push, never force-push, never merge to `main`.
- **Scope:** plan 1 only. Plan 2 (Surface materials, preview-as-sprite-row, greyed pins) and plan 3 (one world pass, shared sprite list, primary camera, `runtime-scene` two-mesh re-bless, 50k budget, view-space slab witness) are out of this document. See "Out of scope" below.
- **ABI:** `kGamePluginABIVersion` stays 54 (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:1131`). This plan does not edit `PluginABI.hpp`. The packet is an internal render type. A later plan that is forced to bump numbers the constant "next after main at execution"; `feat/physics-events` takes 55 first.
- **Every commit message ends with:**

```
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

- **Style:** `/MD` everywhere, no `/fp:fast`, MKS units, ASCII comments, UTF-8 without BOM. New tunables are settings. The depth convention is an enum selected in code (`DepthConvention::ForwardZ`); it is not a cvar. `render.clearColor` stays the canvas clear (`CVarColor` default `{0.02f, 0.02f, 0.04f, 1.0f}`, `RenderDeviceSettings.hpp:82`).
- **Never edit vendored copies in place:** `ThirdParty/Astra`, `ThirdParty/Manifold2D`, `ThirdParty/Mosaic`, `ThirdParty/NRI`.
- **Do not edit** `ReferenceProject/Content/scenes/main.arcscene`, `f3_cull_blend.arcscene`, or any research doc. Camera selection is unchanged: through plan 2 extract still applies today's split (sprites via `ActiveSceneCamera`, meshes via `ActivePerspectiveSceneCamera`).
- **Generate after any new file.** From the worktree, PowerShell:

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
```

`ArcaneClient`, `ArcaneEditor`, and `ArcaneTests` file lists are globs (`premake5.lua`, ArcaneClient at line 671, `files` at 681–684). A new `.cpp`/`.hpp` is invisible to MSBuild until this runs. Premake action is `vs2026`.

- **Build with the PowerShell tool** (MSYS mangles `/p:`). Set `$env:CL = '/FS'` first. For configuration `<Cfg>` (`Debug` or `Release`):

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=<Cfg> /m:8 /nodeReuse:false /v:m
```

Redirect to a log and `Select-String 'error|Warning\(s\)|Error\(s\)'`. `ArcaneClient` defines include `ARC_API_EXPORTS`, `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `VK_USE_PLATFORM_WIN32_KHR`, and build options `/Zc:__cplusplus` `/bigobj` (`premake5.lua:727-744`). The prebuild runs `data/shaders/compile-shaders.bat`.

- **ReferenceProject before the engine solution, per config.** The single slot is `ReferenceProject\Binaries\ReferenceGame.dll`. A Debug-CRT game DLL inside a Release host crashes. For each config, arcbuild the game module *before* `msbuild Arcane.slnx` for that config, so the engine post-build stages the matching CRT:

```powershell
& "D:\dev\starworks\Arcane-f5\bin\<Cfg>-windows-x86_64-md\arcbuild\arcbuild.exe" build --project ReferenceProject --config <Cfg> --sdk D:\dev\starworks\Arcane-f5
```

Pass `--sdk` explicitly. Do not rely on `ARCANE_SDK` pointing at `D:\dev\starworks\Arcane`. The first-ever build of this worktree has no `arcbuild.exe` until ArcaneClient has been built once: build the engine **Debug** once, then the per-config order above applies. After a config flip, `arcbuild rebuild` (not `build`). The last command of Task 13 is `arcbuild rebuild --config Debug`, so the slot ends as a Debug DLL.

- **Tests run from the exe directory:**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\<Cfg>-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[tag]"
```

Record the `Randomness seeded to:` line. GPU tests are tagged `[gpu]`. Device-less graph tests are `[nri]`. `Arcane::Test::RepoRoot()` is `ArcaneTests/src/Helpers/ConstantScan.hpp:34`.

- **Goldens are Task 13 only.** Tasks 1–12 do not run `scripts\golden-gate.ps1`. After Task 3 the editor grid pixels differ from the blessed PNGs on purpose; `editor-ui` stays red until Task 13's explained re-bless. World lanes (`runtime-scene`, `f3-cull-blend`) must stay `diffCount == 0` with no re-bless. Before any golden run, delete `imgui.ini` in both host exe dirs (`bin\<Cfg>-windows-x86_64-md\ArcaneRuntime` and `ArcaneEditor`). Re-bless only the grid/overlay class, and only with the written note in Task 13. A missing 2D grid, 3D grid, physics-debug line, or camera rect is a failure, not a bless. Bless from the host exe directory (the project resolves to the staged copy), then immediately copy the blessed PNGs from the staged `Verify\References` into source `ReferenceProject\Verify\References`. `git status` must show those PNGs. Gate Debug and Release. Do not bless again under Release. Do not bless `inprocess-lit-cube` (it has no bless mode). If mesh thumbnails move, stop: the tonemap include refactor must stay pixel-identical on world content.

- **Nothing on the frame path blocks the main thread.** Extract returns before `RenderFrame` / `RenderFrameOffscreen`. The `/showIncludes` compile is a Catch2 test, not a per-frame step. No `DeviceWaitIdle` is added.

- **Packets and the GPU.** `kMaxFramesInFlight` is 3 (`FramePacing.hpp:38`). `FramesInFlight()` is the latched depth (`FramePacing.hpp:42`). `render.framesInFlight` defaults to 2 and its range is 2..3 (`RenderDeviceSettings.hpp:80`, `:109`). The packet ring stores `kMaxFramesInFlight` slots and uses the live `FramesInFlight()` depth. The GPU reads `NriUploadRing` copies (`NriUploadRing.hpp:21`, `:135`, `NriUploadRing.cpp:102`), never packet memory.

## Review Focus

These are the five inputs most likely to bite that a task's other tests would miss. Each one has a pinning test in the task named, written in that task's step style.

1. **The chrome graph declares a world pass.** `PresentChromeFrame` (`EditorAppFrame.cpp:4016-4035`) sets only `frame.imgui` today, and `DeclareGraphFrame` always starts with `batch2d` (`NriGraphContext.cpp:1277`), so an imgui-only shape clears a canvas through an empty batcher and then tonemaps it (`RenderGraphTest.cpp:6177-6220`). Plan 1 adds `RgFrameShape::world` / `FrameDesc::world`, default `true`. Chrome sets `frame.world = false` and gets a letterbox raster (colour load-op CLEAR to `render.clearColor`, no draws), then tonemap, then imgui. Forbidden names on that shape: `batch2d`, `mesh`, `grid`, `overlay`, `gbuffer`, `lighting`, `taa`, `gpuscene-sync`, `mesh-cull`. An empty-batcher *world* frame still declares `batch2d`. Pinned in Task 2 (`"chrome world=false is letterbox, tonemap, imgui"` and `"PresentChromeFrame sets frame.world = false"`).
2. **A frame still in flight holds the previous packet while extract writes the slot `FramesInFlight()` extracts old.** The GPU never reads packet memory. After `FramesInFlight() - 1` further extracts the first packet's bytes are intact; the next extract overwrites that slot. A span taken from the packet lies inside the packet's own vector. Mutating the source component after extract leaves the packet unchanged. Render origin translation is `{0,0,0}`. The packet header includes `SceneCameraView.hpp` and keeps `spriteView` as `std::optional<SceneCameraView>`. The ImGui snapshot is an owned per-list copy inside that same slot. A user texture (`TexRef._TexData == nullptr`) stores `ImTextureID` as `ImU64`. A managed command stores the `ImTextureData*`, and the snapshot also carries the `ImDrawData::Textures` pointer list, so declaration-time `NewFrameTexUpdates` can `SetTexID` before record. `ImDrawSnapshotRebuild` assigns `ImDrawData` fields directly and never calls `AddDrawList`. List boundaries are kept. A callback command refuses the whole snapshot. Pinned in Task 4 (`"a packet slot stays intact until FramesInFlight extracts have passed"`, `"an ImDrawSnapshot matches the live draw data field for field and refuses a callback"`, and `"an ImDrawSnapshot rebuild reaches ImGuiNri"`).
3. **An empty post chain is not pixel-identity for a blended overlay, and a post chain does not grade an opaque overlay primitive.** Blending a partial-alpha linear colour into scene colour and then running the tonemap differs from running the tonemap operator on the primitive and blending after, including when the chain is empty (C3). Separately, an opaque overlay primitive (alpha 1) reads the same bytes with a scene-tinting post chain and without one, while a world pixel in that same pair of frames differs. Pinned in Task 3 (`"tonemap-then-blend differs from blend-then-tonemap"` and `"an opaque overlay pixel is unchanged by a post chain"`). Task 13 carries the explained re-bless note; it does not claim the empty-chain grid matches today's golden.
4. **A rendering host runs extract once per frame. A non-rendering host leaves the counter at 0.** `RuntimeApp --headless` builds an offscreen vehicle and draws (`RuntimeApp.cpp:447-457`); that process prints `extract-frames: N` after `--frames N`. `Arcane::Runtime` has no client and an empty render scheduler (`ClientRuntimeTest.cpp:26-36`); `Loop().Advance` (`RunLoop.hpp:104`) leaves `ExtractFrameCallCount()` at 0. `ArcaneServer` links `ArcaneCore` only (`premake5.lua:785-792`), ticks with `Loop().Advance` (`ServerApp.cpp:191`), and writes that same Core counter into `ServerReport::extractFrameCalls`, which stays 0. Pinned in Task 11 by running those three hosts. Source text is not the proof.
5. **Overlay class, colour space, a missing depth target, and perspective grid depth.** Each primitive is depth-tested or always-on-top (spec section 3.1). A depth-tested line behind an opaque mesh is hidden; the same line tagged always-on-top is visible. The 2D grid is depth-tested and, because sprites write no depth in this plan, draws over sprites (that crossing is part of the Task 13 note). The 3D grid uses nearer-or-equal and does not write depth. Physics debug does not call `SetLayer` today (`EditorAppFrame.cpp:1893-1981`) and must be tagged always-on-top explicitly. Gizmo colours are already display-referred (`GizmoOverlay.hpp:26-45`, `ColorConvertFloat4ToU32`); a display-referred red `(1,0,0)` reads back unscaled and is not `ACES(1,0,0)`. Linear primitives go through ACES then true sRGB with no exposure term (`tonemap.hlsl:36-56`). With no mesh pass, today's grid comment (`NriGraphContext.cpp:1374-1378`) draws unoccluded and does not attach the placeholder depth; the overlay does the same and does not sample a missing texture. The screen-space witness writes `clip.w = 1`, so a second divide of `SV_Position.z` by `SV_Position.w` still compares the right value on those lines. A perspective grid fragment does not: in the pixel shader `SV_Position.z` is already post-divide and `SV_Position.w` is `1/clip.w`. Both overlay pixel shaders use `input.clip.z`. Pinned in Task 3 (`"depth-tested hides, always-on-top shows, display-referred red is not ACES, missing depth draws unoccluded"` and `"a perspective grid axis is hidden on the cube and visible beside it"`). Task 9 pins the host path those cases do not: extract, `BindPacket`, present, and a 2D-grid pixel, a gizmo pixel, and a physics-debug pixel that are absent when the pass still reads only `FrameDesc`. A missing class is not a Task 13 bless.

## Out of scope

Do not implement any of these in this plan. A task that starts one of them is wrong; stop.

- Plan 2: Surface domain, `.arcmat` migration, vertex factories, the six mesh shaders becoming one material, preview rows, checkerboard moving off the world quad. The checkerboard stays a world quad (spec section 5.7). `Impl::Harvest` and `ShaderEditorDocument::RenderGraphPreview` contain no forbidden registry calls today; they must not grow any, and they do not have to call `ExtractFrame` when they author a local quad.
- Plan 3: one world raster pass, the shared back-to-front sprite list, sprite depth writes, quantized sort keys, sprite texture/cutoff, the 50k budget, sprite-edge parity, `Camera::primary`, editing `main.arcscene` or `f3_cull_blend.arcscene`, the perspective world-mesh occlusion witness, the view-space slab witness, the `runtime-scene` two-mesh re-bless. Task 3's perspective witness is the overlay grid and a world-space overlay line. It does not re-bless `runtime-scene`.
- Selecting reverse-Z. The enum value exists so a later change is one assignment. This plan selects `ForwardZ`.
- An exposure multiply. Overlay conversion is the tonemap operator alone. When D1 adds exposure it scales scene colour and does not scale overlay primitives. `overlay.hlsl` has no `exposure` identifier.
- A velocity target, TAA jitter, or filling the G-buffer, lighting, or TAA slots. Those three nodes are empty.
- Rebasing positions to a non-zero render origin, or rewriting rows into camera-relative positions. Origin translation is `{0,0,0}` until D17.
- Retargeting the pick pass onto a shared list. Pick compare (`PickOutlineNodes.cpp:582`) reads the nearer-wins op. Pick's own `CmdClearAttachments` (`PickOutlineNodes.cpp:400`) stays.
- Deleting `AddGridNode`. The frame stops calling it. The direct declaration test at `RenderGraphTest.cpp:7210` still calls it and must keep compiling.
- An ABI bump, a `PluginABI.hpp` edit, or a change to which camera draws.

## File map

| File | Task | Responsibility |
|---|---|---|
| `ArcaneCore/src/Arcane/Scene/DepthConvention.hpp` (new) | 1 | `DepthConvention`, `kSelectedDepthConvention`. No Astra, no NRI. |
| `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp` | 1, 4 | Task 1 includes `DepthConvention.hpp` above the DEPTH CONVENTION comment (`:120`). Task 4 deletes `struct SceneCameraView` (`:31-42`) and includes `SceneCameraView.hpp`. `ActiveSceneCamera` (`:57`) still returns `std::optional<SceneCameraView>`. `ActivePerspectiveSceneCamera` (`:206`) still returns `std::optional<ViewTransform>`. |
| `ArcaneClient/src/Arcane/Render/DepthConventionMap.hpp` (new) | 1 | `DepthClearValue`, `NearerWinsOp`, `NearerOrEqualOp`. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp` | 1, 2 | Compare at `:832` reads `NearerWinsOp`. Depth clear sites `:83`, `:955`, `:1309` read `DepthClearValue` in Task 2. `CmdClearAttachments` at `:942-957` deleted in Task 2. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp` | 1, 8 | Task 1: compare at `:582` reads `NearerWinsOp`. `:400` stays. Task 8: `AddOutlineNodes` (`:1371-1372`) calls `PrepareSelection` with `Packet()->selectedIds` when a packet is bound, and `CurrentSelectedIds()` (`NriGraphContext.hpp:1056`) when it is not. |
| `ArcaneTests/src/DepthConventionTest.cpp` (new) | 1 | Mapping checks and the source scan of the two compare sites. |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp` | 2 | `RgLoadOp`, `RgAttachmentOp`, setters, queries. `NodeDesc` (`:852-866`). |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.cpp` | 2 | Store the ops beside `SetColorAttachments` (`:211` region). |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp` | 2 | Fill `nri::AttachmentDesc` (`:1368-1382`) from the ops. |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` | 2, 3 | `FrameDesc::world`, `RgFrameShape::world` (default true). Task 3 adds the overlay list pointer. |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` | 2, 3, 9 | `DeclareGraphFrame` (`:1231`) and `BuildFrame` (`:1632-1664`). Task 9's `wantOverlay` and the pointers passed to `AddOverlayNode` come from `Packet()->overlay` when a packet is bound. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp` | 2, 5 | Delete the colour `CmdClearAttachments` (`:1395-1408`). Task 5 feeds the packet's owned spans and still `Prepare`s them against `CurrentBatch()` (the host batcher). |
| `ArcaneTests/src/RenderGraphTest.cpp` | 2, 3 | Every `DeclareGraphFrame` node census. Task 3 moves the grid census to `overlay`. |
| `ArcaneEditor/src/App/EditorAppFrame.cpp` | 2, 3, 5, 7, 8, 9, 10 | `PresentChromeFrame` (`:4016`) sets `world = false` in Task 2. Task 3 retargets grid/physics/camera/gizmo submissions. Task 5 calls `ExtractFrame` after `SubmitSceneToBatcher` returns. Tasks 7–10 move the named builder reads into extract. |
| `ArcaneClient/src/Arcane/Render/GridSceneDesc.hpp` (new) | 3 | `GridSceneDesc` moved out of `GridNode.hpp:95-155`. No NRI, no Astra. |
| `ArcaneClient/src/Arcane/Render/OverlayPrimitive.hpp` (new) | 3 | Overlay enums and primitive records. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.hpp`, `.cpp` (new) | 3, 9 | Node `"overlay"`. Task 3 builds the `b0`/`t0`/`s0` layout and the depth SRV. Task 9's `Record` reads `Packet()->overlay` when a packet is bound. |
| `data/shaders/tonemap_operator.hlsl` (new), `data/shaders/tonemap.hlsl`, `data/shaders/overlay.hlsl` (new), `data/shaders/compile-shaders.bat` | 3 | Shared ACES + sRGB. Overlay entry points. |
| `ArcaneTests/src/OverlayWitnessTest.cpp` (new) | 3 | Review Focus 3 and 5. |
| `ArcaneCore/src/Arcane/Scene/SceneCameraView.hpp` (new) | 4 | `SceneCameraView`. Includes `ViewTransform.hpp`. No registry. |
| `ArcaneCore/src/Arcane/Scene/SceneCacheRecords.hpp` (new) | 12 | `Lerp`, `AngleLerp`, `InterpPose`, `InterpSlot`, `PhysicsInterpBuffer`, `SpriteEntry`, `MeshEntry`, `ResolvedMeshMaterial`. No `Ecs.hpp`, no `Registry.hpp`. `SceneResources.hpp` includes it. |
| `ArcaneCore/src/Arcane/Scene/SceneResources.hpp` | 12 | Keeps `SceneRoot`, `RenderContext2D`, `SpriteTable`, `MeshTable`, `MeshMaterialTable`. Includes `SceneCacheRecords.hpp` and still includes `Ecs.hpp`. |
| `ArcaneClient/src/Arcane/Render/VisibleSet.hpp` (new) | 4 | `VisibleEntry`, `VisibleSet`. No `Registry.hpp`. |
| `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp` | 4 | Includes `VisibleSet.hpp`. `BuildVisibleSet` (`:119`) stays here. The header's own `#include <Astra/Registry/Registry.hpp>` (`:30`) stays; only `ExtractFrame.cpp` may include this header from a Render `.cpp`. |
| `ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp`, `.cpp` (new) | 4 | Packet, `PacketRing`, `CopyImDrawSnapshot`, `ImDrawSnapshotRebuild`. |
| `ArcaneTests/src/RenderFramePacketTest.cpp` (new) | 4 | Review Focus 2, camera field type, field-for-field ImGui snapshot. |
| `ArcaneTests/src/NriGraphPixelTest.cpp` | 4, 5, 6, 8, 9 | Task 4 appends the ImGui rebuild reach case. Task 5 appends the registered-material pixel witness. Task 6's existing `BuildGpuSceneFrame` call updates stay. Task 8 appends the packet-selection polarity case. Task 9 appends the extract-to-present overlay case. |
| `ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp`, `.cpp` (new) | 5 | `NoteExtractFrameCall`, `ExtractFrameCallCount`, `ResetExtractFrameCallCount`. Core, so ArcaneServer links it. |
| `ArcaneClient/src/Arcane/Render/ExtractFrame.hpp`, `.cpp` (new) | 5–10 | The one Render `.cpp` allowed to include `Registry.hpp`. Each task adds one producer family. |
| `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp` | 5 | `InstallRenderSystems` (`:45-49`) stops adding `RenderSubmissionSystem`. |
| `ArcaneTests/src/ClientRuntimeTest.cpp` | 5 | The two `HasSystem<RenderSubmissionSystem>` checks on `ClientRuntime` become `CHECK_FALSE`. |
| `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp` | 6 | `BuildGpuSceneFrame` (`:285`) takes `std::span<const MeshSectionRange>`. Header include of `Registry.hpp` (`:27`) stays; only `ExtractFrame.cpp` includes this header from a Render `.cpp`. |
| `ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp` | 6 | `PrepareSceneForRender` (`:87-111`) becomes a function `ExtractFrame.cpp` calls. |
| `ArcaneRuntime/src/RuntimeFrame.cpp` | 5, 6, 7, 8 | One `ExtractFrame` above `:748` from Task 5. `SetRenderContext` stays in front of that call. Later tasks delete the producer calls this function used to make. `GpuSceneInvalidate` at `:756` stays. |
| `ArcaneRuntime/src/RuntimeApp.cpp` | 7, 11 | Task 7 moves the no-camera `CreateView` (`:870-874`) into extract. Task 11 prints `extract-frames:` once at shutdown. |
| `ArcaneClient/src/Arcane/Render/PickEmit.cpp` | 8 | Move `RotateVec` (`:24-29`) and `CollectPickables` (`:32` through the line before `PickBoundHalfExtents` at `:226`). Drop `SpriteGeometry.hpp` (`:4`), `VisibilitySystem.hpp` (`:5`), `Components.hpp` (`:6`), `PhysicsComponents.hpp` (`:7`), `PhysicsSystem.hpp` (`:8`), `SceneResources.hpp` (`:9`), `Registry.hpp` (`:11`), and `<utility>` (`:17`). `PickPassId` (`:188`), `PickSampleTexel` (`:198`), `PickKindCode` (`:213`), `PickBoundHalfExtents` (`:226`), and `BuildPickIdGeometry` (`:243`) stay. |
| `ArcaneClient/src/Arcane/Render/Grid2D.hpp`, `.cpp` (new) | 9 | `PlanGrid2D` / `EmitGrid2D`, moved from the editor. |
| `ArcaneEditor/src/Viewport/ViewportGrid.hpp`, `.cpp` | 9 | `DrawGrid2D` forwards to `EmitGrid2D`. |
| `ArcaneClient/src/Arcane/Render/RenderSystems.hpp` | 5 | `operator()` from `:50` through its closing brace is what extract runs, on the host `RenderContext2D::batcher`. Extract does not replace that pointer. The header's `Registry.hpp` include (`:37`) stays. Only `ExtractFrame.cpp` includes this header from a Render `.cpp`. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/ImGuiNriNode.cpp` | 10 | `Record` (`:58`) rebuilds `ImDrawData` from the packet snapshot. |
| `ArcaneServer/src/ServerReport.hpp`, `.cpp`, `ArcaneServer/src/ServerApp.cpp` | 11 | `extractFrameCalls` written from `ExtractFrameCallCount()` after the tick loop (`ServerApp.cpp:225-230`). |
| `ArcaneTests/src/ExtractFrameTest.cpp` (new) | 5–10 | One Catch2 case per producer family. |
| `ArcaneTests/src/ExtractHostTest.cpp` (new) | 11 | Review Focus 4. Runs the three hosts. |
| `ArcaneClient/src/Arcane/Render/MeshCache.hpp`, `MeshMaterialCache.hpp`, `SpriteCache.hpp`, `PhysicsDebugDraw.cpp`, `PhysicsDebugDraw.hpp` | 12 | The four TUs include `SceneCacheRecords.hpp` instead of `SceneResources.hpp`. The `.hpp` comment at `PhysicsDebugDraw.hpp:44` names the new header. |
| `ArcaneTests/src/RenderRegistryIncludeTest.cpp` (new) | 12 | `/showIncludes` gate, including a per-TU check of the four cache/debug-draw TUs, and the comment-stripping host frame-builder gate. |
| Golden PNGs under `ReferenceProject/Verify/References` | 13 | Explained grid/overlay re-bless only. |

---

### Task 1: One depth-convention value

**Files:**
- Create: `ArcaneCore/src/Arcane/Scene/DepthConvention.hpp`
- Modify: `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:18-20` (include beside the block at `:120-129`)
- Create: `ArcaneClient/src/Arcane/Render/DepthConventionMap.hpp`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:823-832`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:579-582`
- Test: `ArcaneTests/src/DepthConventionTest.cpp`

**Interfaces:**
- Consumes: `nri::CompareOp` (`ThirdParty/NRI/Include/NRIDescs.h:835-845`: `LESS`, `LESS_EQUAL`, `GREATER`, `GREATER_EQUAL`). Today's mesh compare is `CompareOp::LESS` (`MeshNode.cpp:832`). Today's pick-mesh compare is `CompareOp::LESS` (`PickOutlineNodes.cpp:582`). Today's clear is `kDepthClear = 1.0f` (`MeshNode.cpp:83`).
- Produces, exact names later tasks call:
  - `enum class DepthConvention : std::uint8_t { ForwardZ, ReverseZ };`
  - `inline constexpr DepthConvention kSelectedDepthConvention = DepthConvention::ForwardZ;`
  - `float DepthClearValue(DepthConvention convention) noexcept;`
  - `nri::CompareOp NearerWinsOp(DepthConvention convention) noexcept;`
  - `nri::CompareOp NearerOrEqualOp(DepthConvention convention) noexcept;`
  - ForwardZ returns `1.0f`, `nri::CompareOp::LESS`, `nri::CompareOp::LESS_EQUAL`. ReverseZ returns `0.0f`, `nri::CompareOp::GREATER`, `nri::CompareOp::GREATER_EQUAL`.

`DepthConvention.hpp` includes only `<cstdint>`. It does not include `Astra/Registry/Registry.hpp`, `SceneCamera.hpp`, or `NRI.h`. `SceneCamera.hpp:20` includes the registry, so a render TU that needs the enum includes `DepthConvention.hpp`, not `SceneCamera.hpp`.

- [ ] **Step 1: Write the failing test.**

Create `ArcaneTests/src/DepthConventionTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Scene/DepthConvention.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("ForwardZ clear is 1 and nearer-wins is LESS", "[nri][depth]")
{
    CHECK(Arcane::kSelectedDepthConvention == Arcane::DepthConvention::ForwardZ);
    CHECK(Arcane::DepthClearValue(Arcane::DepthConvention::ForwardZ) == 1.0f);
    CHECK(Arcane::NearerWinsOp(Arcane::DepthConvention::ForwardZ) == nri::CompareOp::LESS);
    CHECK(Arcane::NearerOrEqualOp(Arcane::DepthConvention::ForwardZ) == nri::CompareOp::LESS_EQUAL);
}

TEST_CASE("ReverseZ is the other mapping and is not selected", "[nri][depth]")
{
    CHECK(Arcane::DepthClearValue(Arcane::DepthConvention::ReverseZ) == 0.0f);
    CHECK(Arcane::NearerWinsOp(Arcane::DepthConvention::ReverseZ) == nri::CompareOp::GREATER);
    CHECK(Arcane::NearerOrEqualOp(Arcane::DepthConvention::ReverseZ) == nri::CompareOp::GREATER_EQUAL);
    CHECK(Arcane::kSelectedDepthConvention != Arcane::DepthConvention::ReverseZ);
}

static std::string ReadRepoFile(const char* relative)
{
    std::ifstream in(Arcane::Test::RepoRoot() / relative);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removing the call and writing CompareOp::LESS again goes green on the GPU
// and red here. The two sites are the mesh pipelines and the pick-mesh pipeline.
TEST_CASE("mesh and pick compares call NearerWinsOp", "[nri][depth]")
{
    const std::string mesh = ReadRepoFile("ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp");
    const std::string pick = ReadRepoFile("ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp");
    CHECK(mesh.find("desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);")
          != std::string::npos);
    CHECK(pick.find("desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);")
          != std::string::npos);
}
```

- [ ] **Step 2: Generate and run the test to verify it fails.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[depth]"
```

Expected: FAIL. `DepthConvention.hpp` / `DepthConventionMap.hpp` do not exist, or the source scan does not find `NearerWinsOp(`. `arcbuild.exe` is not required for this test. If this is the first build of the worktree, this Debug engine build is the one that produces `arcbuild.exe` for later tasks.

- [ ] **Step 3: Write the headers and point the two compares at them.**

`ArcaneCore/src/Arcane/Scene/DepthConvention.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace Arcane
{
    enum class DepthConvention : std::uint8_t
    {
        ForwardZ,
        ReverseZ,
    };

    inline constexpr DepthConvention kSelectedDepthConvention = DepthConvention::ForwardZ;
}
```

In `SceneCamera.hpp`, immediately above the comment that starts `DEPTH CONVENTION` (line 120), add:

```cpp
#include <Arcane/Scene/DepthConvention.hpp>
```

Do not move `ActiveSceneCamera` or `ActivePerspectiveSceneCamera`. The value now sits in the header the comment's next include names. Leave the comment text in place so the block still documents why `perspectiveRH_ZO` is used (`SceneCamera.hpp:156`).

`ArcaneClient/src/Arcane/Render/DepthConventionMap.hpp`:

```cpp
#pragma once

#include <Arcane/Scene/DepthConvention.hpp>

#include <NRIDescs.h>

namespace Arcane
{
    [[nodiscard]] inline float DepthClearValue(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? 0.0f : 1.0f;
    }

    [[nodiscard]] inline nri::CompareOp NearerWinsOp(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? nri::CompareOp::GREATER
                                                       : nri::CompareOp::LESS;
    }

    [[nodiscard]] inline nri::CompareOp NearerOrEqualOp(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? nri::CompareOp::GREATER_EQUAL
                                                       : nri::CompareOp::LESS_EQUAL;
    }
}
```

In `MeshNode.cpp`, include `DepthConventionMap.hpp`. Replace the assignment at line 832:

```cpp
desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);
```

In `PickOutlineNodes.cpp`, the same replacement at line 582. Leave `kDepthClear` and both `CmdClearAttachments` calls for Task 2. Leave the grid's `LESS_EQUAL` (`GridNode.cpp:375`) for Task 3; Task 3's overlay reads `NearerOrEqualOp` and this task does not change grid pixels.

- [ ] **Step 4: Run the test to verify it passes.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[depth]"
```

Expected: PASS. Selected value is ForwardZ, so the mesh compare is still `LESS` and the clear constant is still `1.0f`. No golden run in this task.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneCore/src/Arcane/Scene/DepthConvention.hpp ArcaneCore/src/Arcane/Scene/SceneCamera.hpp ArcaneClient/src/Arcane/Render/DepthConventionMap.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp ArcaneTests/src/DepthConventionTest.cpp
git commit -m "feat(render): one depth-convention value drives the mesh and pick compares" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 2: Declarative load ops, empty D1 slots, chrome letterbox

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp` (`SetColorAttachments` at `:551-552`, `NodeDesc` at `:852-866`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.cpp` (the `SetColorAttachments` definition)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp:1368-1382`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` (`FrameDesc` at `:385`, `RgFrameShape` at `:1614`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` (`DeclareGraphFrame` at `:1231`, `BuildFrame` copies at `:1632-1664`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1390-1408` and `:1654-1666`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:77-83`, `:942-957`, `:1301-1320`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp:4034-4035`
- Modify: `ArcaneTests/src/RenderGraphTest.cpp` (every `DeclareGraphFrame` census listed in Step 3)
- Test: the cases added to `ArcaneTests/src/RenderGraphTest.cpp`

**Interfaces:**
- Consumes: Task 1's `DepthClearValue` and `kSelectedDepthConvention`. `nri::LoadOp` is `LOAD`, `CLEAR` (`NRIDescs.h:1552-1555`). `nri::StoreOp` zero value is `STORE` (`NRIDescs.h:1559-1562`). `nri::AttachmentDesc` is `{ descriptor, clearValue, loadOp, storeOp }` (`NRIDescs.h:1572-1578`). `NodeKind` is `Raster`, `Compute`, `Copy` (`RenderGraph.hpp:500`). `Settings<RenderSettings>().clearColor` is a `CVarColor` with `.r .g .b .a` (`CVarTypes.hpp:37`, `RenderDeviceSettings.hpp:82`).
- Produces:
  - `enum class RgLoadOp : std::uint8_t { Load, Clear };`
  - `struct RgAttachmentOp { RgLoadOp load = RgLoadOp::Load; nri::ClearValue clear{}; };`
  - `void RenderGraph::SetColorAttachmentOps(std::span<const RgAttachmentOp> ops);`
  - `void RenderGraph::SetDepthAttachmentOp(RgAttachmentOp op);`
  - `const RgAttachmentOp* RenderGraph::ColorLoadOp(std::size_t nodeIndex, std::size_t attachment) const;`
  - `const RgAttachmentOp* RenderGraph::DepthLoadOp(std::size_t nodeIndex) const;`
  - `bool FrameDesc::world = true;` and `bool RgFrameShape::world = true;`
  - Node names, in order, on a default world frame: `"gbuffer"`, `"lighting"`, `"batch2d"`, `"taa"`, `"tonemap"`.
  - Node names on `world == false` with imgui: `"letterbox"`, `"tonemap"`, `"imgui"`.

`ColorLoadOp` / `DepthLoadOp` return null when that attachment was not declared. When the colour-op span is shorter than the colour-attachment span, the missing ops are `RgLoadOp::Load` with a zero `clear` (today's behaviour). `storeOp` stays `STORE`.

- [ ] **Step 1: Write the failing tests.**

Add these cases to `ArcaneTests/src/RenderGraphTest.cpp`. They include `DepthConventionMap.hpp` and call the new queries, so they fail to compile until Step 3.

```cpp
TEST_CASE("batch2d colour load-op clears to render.clearColor and mesh loads colour", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 64;
    shape.canvasHeight = 64;
    Arcane::MeshInstance one;
    one.mesh = Arcane::Guid{ 1, 1 };
    const Arcane::MeshInstance instances[] = { one };
    Arcane::MeshSceneDesc scene;
    scene.instances = instances;   // span, not a vector (MeshNode.hpp:282)
    shape.mesh = &scene;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    const std::size_t batch = NodeIndex(graph, "batch2d");
    const Arcane::RgAttachmentOp* color = graph.ColorLoadOp(batch, 0);
    REQUIRE(color != nullptr);
    CHECK(color->load == Arcane::RgLoadOp::Clear);
    const Arcane::CVarColor expected = Arcane::Settings<Arcane::RenderSettings>().clearColor;
    CHECK(color->clear.color.f.x == expected.r);
    CHECK(color->clear.color.f.y == expected.g);
    CHECK(color->clear.color.f.z == expected.b);
    CHECK(color->clear.color.f.w == expected.a);

    const std::size_t mesh = NodeIndex(graph, "mesh");
    const Arcane::RgAttachmentOp* meshColor = graph.ColorLoadOp(mesh, 0);
    REQUIRE(meshColor != nullptr);
    CHECK(meshColor->load == Arcane::RgLoadOp::Load);
    const Arcane::RgAttachmentOp* meshDepth = graph.DepthLoadOp(mesh);
    REQUIRE(meshDepth != nullptr);
    CHECK(meshDepth->load == Arcane::RgLoadOp::Clear);
    CHECK(meshDepth->clear.depthStencil.depth ==
          Arcane::DepthClearValue(Arcane::kSelectedDepthConvention));

    const std::size_t gbuffer = NodeIndex(graph, "gbuffer");
    CHECK(graph.ColorLoadOp(gbuffer, 0) == nullptr);
    CHECK(graph.DepthLoadOp(gbuffer) == nullptr);
}

TEST_CASE("reserved D1 slots are empty and sit around the world nodes", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 64;
    shape.canvasHeight = 64;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    const std::vector<std::string> names = NodeNames(graph);
    CHECK(names == std::vector<std::string>{ "gbuffer", "lighting", "batch2d", "taa", "tonemap" });

    for (const char* empty : { "gbuffer", "lighting", "taa" })
    {
        const std::size_t i = NodeIndex(graph, empty);
        CHECK(graph.ColorLoadOp(i, 0) == nullptr);
        CHECK(graph.DepthLoadOp(i) == nullptr);
    }
}

// Review Focus 1.
TEST_CASE("chrome world=false is letterbox, tonemap, imgui", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 1280;
    shape.canvasHeight = 720;
    shape.imgui        = true;
    shape.world        = false;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    CHECK(NodeNames(graph) == std::vector<std::string>{ "letterbox", "tonemap", "imgui" });
    const std::size_t letter = NodeIndex(graph, "letterbox");
    const Arcane::RgAttachmentOp* clear = graph.ColorLoadOp(letter, 0);
    REQUIRE(clear != nullptr);
    CHECK(clear->load == Arcane::RgLoadOp::Clear);
    const Arcane::CVarColor expected = Arcane::Settings<Arcane::RenderSettings>().clearColor;
    CHECK(clear->clear.color.f.x == expected.r);

    for (const char* banned : { "batch2d", "mesh", "grid", "overlay", "gbuffer",
                                "lighting", "taa", "gpuscene-sync", "mesh-cull" })
        CHECK(NodeIndexOrNpos(graph, banned) == static_cast<std::size_t>(-1));
}

TEST_CASE("an empty world batcher still declares batch2d", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 32;
    shape.canvasHeight = 32;
    // world defaults true. No batcher pointer on the shape: declaration only.
    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(NodeIndexOrNpos(graph, "batch2d") != static_cast<std::size_t>(-1));
    CHECK(NodeIndexOrNpos(graph, "letterbox") == static_cast<std::size_t>(-1));
}

TEST_CASE("PresentChromeFrame sets frame.world = false", "[nri][loadop]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t fn = text.find("EditorApp::PresentChromeFrame()");
    REQUIRE(fn != std::string::npos);
    const std::size_t next = text.find("\nbool EditorApp::", fn + 1);
    const std::string body = text.substr(fn, (next == std::string::npos ? text.size() : next) - fn);
    CHECK(body.find("frame.world = false;") != std::string::npos);
}

TEST_CASE("batch2d and mesh Record no longer call CmdClearAttachments", "[nri][loadop]")
{
    auto read = [](const char* rel) {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    auto recordBody = [](const std::string& text, const char* sig) {
        const std::size_t fn = text.find(sig);
        REQUIRE(fn != std::string::npos);
        return text.substr(fn, 2500);
    };
    const std::string batch = recordBody(
        read("ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp"),
        "void Batch2DNode::Record(");
    const std::string mesh = recordBody(
        read("ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp"),
        "void MeshNode::Record(");
    CHECK(batch.find("CmdClearAttachments") == std::string::npos);
    CHECK(mesh.find("CmdClearAttachments") == std::string::npos);
    // Pick keeps its own clear. This scan is the guard that the deletion stayed local.
    const std::string pick = read("ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp");
    CHECK(pick.find("CmdClearAttachments") != std::string::npos);
}
```

Add the helpers next to the other anonymous helpers in that file:

```cpp
std::vector<std::string> NodeNames(const Arcane::RenderGraph& graph)
{
    std::vector<std::string> names;
    names.reserve(graph.NodeCount());
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
        names.emplace_back(graph.NodeName(i));
    return names;
}

std::size_t NodeIndexOrNpos(const Arcane::RenderGraph& graph, const char* name)
{
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
        if (std::string(graph.NodeName(i)) == name)
            return i;
    return static_cast<std::size_t>(-1);
}

std::size_t NodeIndex(const Arcane::RenderGraph& graph, const char* name)
{
    const std::size_t i = NodeIndexOrNpos(graph, name);
    REQUIRE(i != static_cast<std::size_t>(-1));
    return i;
}

// Today's world-frame name list, with the three empty slots inserted.
// Chrome does not use this helper: chrome sets world=false and expects letterbox.
std::vector<std::string> WithReservedSlots(std::vector<std::string> today)
{
    std::vector<std::string> out{ "gbuffer", "lighting" };
    for (const std::string& name : today)
    {
        if (name == "tonemap")
            out.emplace_back("taa");
        out.push_back(name);
    }
    return out;
}
```

`MeshSceneDesc::instances` is `std::span<const MeshInstance>` (`MeshNode.hpp:282`). The one-element array above is the fixture at `RenderGraphTest.cpp:6707-6712`. `wantsMesh` is `shape.mesh != nullptr && !shape.mesh->Empty()` (`NriGraphContext.cpp:1303`). `Empty()` is false when `instances` is non-empty (`MeshNode.hpp:351-354`).

- [ ] **Step 2: Run the new cases to verify they fail.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[loadop]"
```

Expected: FAIL to compile (`RgLoadOp`, `ColorLoadOp`, `shape.world` unknown) or, once those exist and before the behaviour lands, FAIL the name and source checks.

- [ ] **Step 3: Implement the load ops, the three empty nodes, and the letterbox.**

On `RenderGraph::NodeDesc` (`RenderGraph.hpp:852`), add:

```cpp
std::vector<RgAttachmentOp> colorOps;
RgAttachmentOp              depthOp{};
bool                        hasDepthOp = false;
```

Declare `RgLoadOp` and `RgAttachmentOp` in namespace `Arcane` in `RenderGraph.hpp` above `RenderGraph`. Add the two setters next to `SetColorAttachments` (`:551`) and the two queries next to `NodeName` (`:689`).

`SetColorAttachmentOps` stores the span into `m_nodes[m_currentNodeIndex].colorOps`, under the same "only while setup is running" assert `SetColorAttachments` already uses. `SetDepthAttachmentOp` stores `op` and sets `hasDepthOp`. `ColorLoadOp` returns null when `nodeIndex` or `attachment` is out of range, or when `colorOps.size() <= attachment` (shorter span means LOAD, reported as null so a caller that did not set an op does not observe a synthetic CLEAR). `DepthLoadOp` returns null when `!hasDepthOp`.

In `RenderGraphExec.cpp`, replace the colour-attachment loop at `:1368-1373`:

```cpp
m_scratchColorAttachments.clear();
std::size_t colorIndex = 0;
for (const RgTexture& color : node.colorAttachments)
{
    nri::AttachmentDesc attachment = {};
    attachment.descriptor = attachmentView(color, /*depth=*/false);
    if (colorIndex < node.colorOps.size() && node.colorOps[colorIndex].load == RgLoadOp::Clear)
    {
        attachment.loadOp = nri::LoadOp::CLEAR;
        attachment.clearValue = node.colorOps[colorIndex].clear;
    }
    m_scratchColorAttachments.push_back(attachment);
    ++colorIndex;
}
```

After the existing depth-descriptor assignment (`:1379-1380`), add:

```cpp
if (node.hasDepthOp && node.depthOp.load == RgLoadOp::Clear)
{
    renderingDesc.depth.loadOp = nri::LoadOp::CLEAR;
    renderingDesc.depth.clearValue = node.depthOp.clear;
}
```

Do not set `storeOp`. Zero-initialization already stores `STORE`.

`AddBatch2DNode` (`Batch2DNode.cpp:1654`) already writes `optimizedClearValue` from `clearColor` (`:1661-1662`). Inside that setup lambda, after `SetColorAttachments`, add:

```cpp
Arcane::RgAttachmentOp op;
op.load = Arcane::RgLoadOp::Clear;
op.clear.color.f = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
graph.SetColorAttachmentOps(std::span<const Arcane::RgAttachmentOp>(&op, 1));
```

Delete the `CmdClearAttachments` block in `Batch2DNode::Record` (`:1395-1408`), including the comment that says the declaration API carries no clear op. Keep the empty-batch early return at `:1407-1408`: an empty batch still returns after the pass has begun, and the load-op is what cleared the target. The early return stays so an empty batch records no draws.

`MeshNode::AddMeshNode` setup (`:1319-1320` region) after the colour and depth attachments:

```cpp
Arcane::RgAttachmentOp colorOp;   // load defaults to Load
graph.SetColorAttachmentOps(std::span<const Arcane::RgAttachmentOp>(&colorOp, 1));
Arcane::RgAttachmentOp depthOp;
depthOp.load = Arcane::RgLoadOp::Clear;
depthOp.clear.depthStencil.depth = Arcane::DepthClearValue(Arcane::kSelectedDepthConvention);
depthOp.clear.depthStencil.stencil = 0;
graph.SetDepthAttachmentOp(depthOp);
```

Replace `kDepthClear` (`MeshNode.cpp:83`) and its uses at the `optimizedClearValue` write (`:1309`) with `DepthClearValue(kSelectedDepthConvention)`. Delete the depth `CmdClearAttachments` block (`:942-957`). Do not delete pick's clear.

In `DeclareGraphFrame`, when `shape.world` is true, before `AddBatch2DNode` (`:1277`), add two compute nodes:

```cpp
auto addEmpty = [&](const char* name) {
    graph.AddNode(name, RenderGraph::NodeKind::Compute,
        [](RenderGraphBuilder&) {},
        [](RenderGraphNodeContext&) {});
};
addEmpty("gbuffer");
addEmpty("lighting");
```

Immediately before `AddTonemapNode` (`:1445`), `addEmpty("taa")`. These nodes declare no `Read`, no `Write`, and no attachment, so `Compile` gives them no barrier and no pool slot. They do not set a colour clear.

When `shape.world` is false, do not call `addEmpty`, do not call `AddBatch2DNode`, and do not run the mesh, depth-placeholder, grid, or post blocks. Declare one raster node `"letterbox"` that creates the canvas texture with the same format and extent `AddBatch2DNode` uses (`GraphCanvasFormat()`, `shape.canvasWidth/Height`), sets `optimizedClearValue` and a colour `RgAttachmentOp` of `Clear` to `Settings<RenderSettings>().clearColor`, writes the texture as `RgUsage::ColorWrite`, and has an empty exec. Store that texture in `handles.canvas` so `AddTonemapNode` still samples it. Then declare tonemap, then the existing gameui / pick / imgui / capture tail. Capture still appends when `shape.capture` is set. Chrome does not set capture.

`BuildFrame` (`:1633`) copies `shape.world = frame.world`. Default `true` means every current caller stays a world frame until it opts out.

`PresentChromeFrame` (`EditorAppFrame.cpp:4034-4035`), immediately after `FrameDesc frame;`:

```cpp
frame.world = false;
frame.imgui = chrome;
```

**Existing `DeclareGraphFrame` censuses.** Any test that `AddNode`s its own names (`"first"`, `"second"`) is unchanged. Every test that calls `DeclareGraphFrame` on a default (`world == true`) shape changes its expected names by `WithReservedSlots` of today's list, and its `NodeCount` by `+3` (two prefix nodes and one `taa`). Hard indices into `compiled.nodes[i]` that assumed node 0 was `batch2d` retarget through `NodeIndex(graph, "batch2d")`. `compiled.nodes[tonemap]` retargets through `NodeIndex(graph, "tonemap")`. Transient counts and pool-slot counts stay, because the three new nodes allocate nothing.

Apply that rule at each of these sites (today's names, verified in this tree):

| Site | Today's names | After this task |
|---|---|---|
| `:3239` | count 2, offscreen, no extra flags | count 5: gbuffer, lighting, batch2d, taa, tonemap |
| `:4135-4137` | batch2d, tonemap | those five |
| `:4212` | capture is node 2 | capture is the last node; name `"capture"` via `NodeIndex` |
| `:4313-4315` | batch2d, tonemap | the five |
| `:4420-4424` | offscreen vs presenting name equality | both sides gain the same three names; equality stays |
| `:4461` | capture node 2 | `NodeIndex(..., "capture")` |
| `:4656-4660` | batch2d, post0, post1, tonemap | gbuffer, lighting, batch2d, post0, post1, taa, tonemap |
| `:4753`, `:4758`, `:4767` | last is tonemap; count 2 | last is still tonemap; count 5 |
| `:4789` | count 6 (batch2d + four posts + tonemap) | count 9 |
| `:4849` | count 4 | count 7 |
| `:5787-5789` | batch2d, tonemap | the five |
| `:5822-5833` | batch2d is 0, tonemap is 1, pick is 2 | `NodeIndex` of those names; pick stays after tonemap |
| `:6049-6051` | the base pair | the five |
| `:6099-6102` | tonemap at node 4 | `NodeIndex(..., "tonemap")` |
| `:6126-6128`, `:6149-6151` | imgui | imgui stays after tonemap; prefix + taa shift the index |
| `:6177-6230` | batch2d, tonemap, imgui | **set `shape.world = false`**. Expect letterbox, tonemap, imgui. This is the chrome case. Do not run it through `WithReservedSlots`. |
| `:6278-6280`, `:6323-6324`, `:6356-6360` | world frames | `WithReservedSlots` |
| `:6400-6402`, `:6427-6431`, `:6454-6458` | gameui after tonemap | gameui stays after tonemap |
| `:6494-6506`, `:6538-6540` | world frames | `WithReservedSlots` |
| `:6562-6565` | depth placeholder between batch2d and tonemap | gbuffer, lighting, batch2d, depth, taa, tonemap |
| `:6663-6670` | count 2, depth handle invalid (empty mesh section) | count 5; depth handle still invalid |
| `:6673-6695` | batch2d, depth, tonemap (count 3) | gbuffer, lighting, batch2d, depth, taa, tonemap |
| `:6722-6727` | batch2d, gpuscene-sync, mesh-cull, mesh, tonemap (count 5) | gbuffer, lighting, then those names, with taa before tonemap (count 8) |
| `:6810-6812`, `:6900-6903`, `:7007-7012`, `:7054-7056` | world frames | `WithReservedSlots` |
| `:7101-7107` | mesh chain plus `grid` before tonemap | grid stays before taa in this task: `..., mesh, grid, taa, tonemap` |
| `:7149-7176` | depth lifetime | retarget by name. The grid node still writes depth in this task. |
| `:7211-7212` | direct `AddGridNode` | unchanged. It does not call `DeclareGraphFrame`. |
| `:7594-7595` | world frame | `WithReservedSlots` |

A barrier assertion that indexes `compiled.nodes[0]` as the canvas colour barrier now indexes `compiled.nodes[NodeIndex(graph, "batch2d")]`. Two prefix nodes shift every later index by 2, and `taa` shifts tonemap and everything after it by one more.

Do not run the golden gate. World pixels are unchanged: `batch2d` still precedes `mesh`, the clear value is the same `clearColor`, the depth clear is still `1.0`, the compare is still `LESS`. The grid node is still before the post chain.

- [ ] **Step 4: Run the graph tests.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[nri]"
```

Expected: PASS, including `[loadop]` and the updated censuses. Record the seed.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp ArcaneClient/src/Arcane/Render/Nri/RenderGraph.cpp ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/RenderGraphTest.cpp
git commit -m "feat(render): declarative load ops, empty D1 slots, chrome letterbox" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 3: Post-tonemap overlay pass

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/GridSceneDesc.hpp`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp` (replace the struct at `:95-155` with the include)
- Create: `ArcaneClient/src/Arcane/Render/OverlayPrimitive.hpp`
- Create: `ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.hpp`
- Create: `ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.cpp`
- Create: `data/shaders/tonemap_operator.hlsl`
- Modify: `data/shaders/tonemap.hlsl:36-50`
- Create: `data/shaders/overlay.hlsl`
- Modify: `data/shaders/compile-shaders.bat:68-69` (add two `call :compile` lines after the grid lines)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` (`RgFrameShape` grid field at `:1728`, `FrameDesc::grid` at `:451`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` (delete the `AddGridNode` call at `:1392-1397`; declare `"overlay"` after tonemap at `:1445` and before gameui at `:1474`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`SubmitSceneToBatcher` `:1857-1991`, gizmo lambda inside `DrawViewportPanelPhase` at `:3713`)
- Modify: `ArcaneTests/src/RenderGraphTest.cpp` grid census at `:7089-7107`
- Test: `ArcaneTests/src/OverlayWitnessTest.cpp`

**Interfaces:**
- Consumes: `NearerOrEqualOp`, `kSelectedDepthConvention`, `RgFrameShape::world`, node `"tonemap"`, `handles.backbuffer`, `handles.depth`. `GridSceneDesc` fields are the struct at `GridNode.hpp:95-155` (`view`, `plane`, spacings, `minorColor`, `majorColor`, axis colours, `SetPlane`). `ImGuiGizmoSink` methods are `Line`, `Triangle`, `Rect`, `Circle` (`GizmoOverlay.hpp:26-41`), colours already display-referred.
- Produces:
  - `bool RenderGraph::WasRead(RgTexture texture) const;` — true when some node `Read`s that texture. There is no such query today (`WasWritten` is `RenderGraph.cpp:892`).
  - `enum class OverlayDepthClass : std::uint8_t { DepthTested, AlwaysOnTop };`
  - `enum class OverlayColorSpace : std::uint8_t { Linear, DisplayReferred };`
  - `enum class OverlayPrimKind : std::uint8_t { Line, Triangle, Rect, Circle };`
  - `struct OverlayLine { glm::vec3 a; glm::vec3 b; glm::vec4 color; float thickness; OverlayDepthClass depth; OverlayColorSpace space; bool screenSpace; };`
  - `struct OverlayFill { OverlayPrimKind kind; glm::vec3 a, b, c; glm::vec2 size; float radius; glm::vec4 color; OverlayDepthClass depth; OverlayColorSpace space; bool screenSpace; };`
  - `struct OverlayPrimitiveList { std::vector<OverlayLine> lines; std::vector<OverlayFill> fills; bool hasGrid3d = false; GridSceneDesc grid3d{}; };`
  - `const OverlayPrimitiveList* FrameDesc::overlay = nullptr;`
  - `const OverlayPrimitiveList* RgFrameShape::overlay = nullptr;`
  - Node name `"overlay"`, declared after `"tonemap"` and before `"gameui"`, only when `shape.grid != nullptr` or `shape.overlay` has any line, fill, or `hasGrid3d`.

`GridSceneDesc.hpp` includes `ViewTransform.hpp`, `Constant.hpp` (for the existing `ARC_CONSTANT` markers), and `<glm/glm.hpp>`. It does not include `NRI.h` or `Registry.hpp`. `GridNode.hpp` includes `GridSceneDesc.hpp` and deletes the struct body.

- [ ] **Step 1: Write the failing tests.**

`ArcaneTests/src/OverlayWitnessTest.cpp`. The device-less half fails at compile time until the types exist. The GPU half fails until the pass behaves.

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>

static std::size_t NodeIndexOrNpos(const Arcane::RenderGraph& graph, const char* name)
{
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
        if (std::string(graph.NodeName(i)) == name) return i;
    return static_cast<std::size_t>(-1);
}
static std::size_t NodeIndex(const Arcane::RenderGraph& graph, const char* name)
{
    const std::size_t i = NodeIndexOrNpos(graph, name);
    REQUIRE(i != static_cast<std::size_t>(-1));
    return i;
}

TEST_CASE("overlay node sits after tonemap and samples mesh depth", "[nri][overlay]")
{
    Arcane::MeshInstance one;
    one.mesh = Arcane::Guid{ 1, 1 };
    const Arcane::MeshInstance instances[] = { one };
    Arcane::MeshSceneDesc scene;
    scene.instances = instances;

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    shape.mesh = &scene;
    Arcane::OverlayPrimitiveList list;
    Arcane::OverlayLine line{};
    line.a = { 0.0f, 0.0f, 0.0f };
    line.b = { 1.0f, 0.0f, 0.0f };
    line.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    line.depth = Arcane::OverlayDepthClass::DepthTested;
    line.space = Arcane::OverlayColorSpace::Linear;
    list.lines.push_back(line);
    shape.overlay = &list;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::size_t tonemap = NodeIndex(graph, "tonemap");
    const std::size_t overlay = NodeIndex(graph, "overlay");
    CHECK(overlay == tonemap + 1);
    CHECK(NodeIndexOrNpos(graph, "grid") == static_cast<std::size_t>(-1));
    CHECK(graph.DepthLoadOp(overlay) == nullptr);   // sampled, not attached
    REQUIRE(graph.IsHandleValid(handles.depth));
    CHECK(graph.WasRead(handles.depth));
    CHECK(graph.WasWritten(handles.depth));         // the mesh node still writes it
}

TEST_CASE("a runtime frame with no overlay does not declare one", "[nri][overlay]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(NodeIndexOrNpos(graph, "overlay") == static_cast<std::size_t>(-1));
    CHECK(NodeIndexOrNpos(graph, "grid") == static_cast<std::size_t>(-1));
}

TEST_CASE("overlay.hlsl uses the shared operator and has no exposure term", "[nri][overlay]")
{
    auto read = [](const char* rel) {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string overlay = read("data/shaders/overlay.hlsl");
    const std::string tonemap = read("data/shaders/tonemap.hlsl");
    const std::string shared  = read("data/shaders/tonemap_operator.hlsl");
    CHECK(overlay.find("exposure") == std::string::npos);
    CHECK(overlay.find("#include \"tonemap_operator.hlsl\"") != std::string::npos);
    CHECK(tonemap.find("#include \"tonemap_operator.hlsl\"") != std::string::npos);
    CHECK(shared.find("float3 ACESFilmic(float3 x)") != std::string::npos);
    CHECK(shared.find("float3 LinearToSrgb(float3 lin)") != std::string::npos);
    // The constants stay the Narkowicz values. A drift here moves world pixels.
    CHECK(shared.find("const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;")
          != std::string::npos);
    // Screen-space clip.z is the vertex z. A hardcoded 0 is the near plane, so a
    // depth-tested overlay could never sit behind a mesh.
    CHECK(overlay.find("float4(ndc, input.pos.z, 1.0)") != std::string::npos);
    CHECK(overlay.find("float4(ndc, 0.0, 1.0)") == std::string::npos);
    CHECK(overlay.find("g_HasDepth > 0.5 && input.depthTest > 0.5") != std::string::npos);
    // SV_Position.z is already post-divide. A second divide by .w must fail this scan.
    CHECK(overlay.find("input.clip.z / input.clip.w") == std::string::npos);
    CHECK(overlay.find("clip.z / clip.w") == std::string::npos);
    const auto fragD = overlay.find("float fragD = input.clip.z;");
    CHECK(fragD != std::string::npos);
    CHECK(overlay.find("float fragD = input.clip.z;", fragD + 1) != std::string::npos);
}

// The placeholder depth node (NriGraphContext.cpp:1352-1364) stays unconsumed.
// Reading it would make WasRead true. Today's placeholder case already requires
// WasWritten(depth) == false (RenderGraphTest.cpp:6673-6695).
TEST_CASE("overlay with no mesh does not read the placeholder depth", "[nri][overlay]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    shape.depth = true;
    Arcane::OverlayPrimitiveList list;
    Arcane::OverlayLine line{};
    line.a = { 4.0f, 10.0f, 1.0f };
    line.b = { 16.0f, 10.0f, 1.0f };
    line.thickness = 8.0f;
    line.depth = Arcane::OverlayDepthClass::DepthTested;
    line.space = Arcane::OverlayColorSpace::DisplayReferred;
    line.color = { 0.0f, 0.0f, 1.0f, 1.0f };
    line.screenSpace = true;
    list.lines.push_back(line);
    shape.overlay = &list;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::size_t tonemap = NodeIndex(graph, "tonemap");
    const std::size_t overlay = NodeIndex(graph, "overlay");
    CHECK(overlay == tonemap + 1);
    CHECK(NodeIndexOrNpos(graph, "depth") != static_cast<std::size_t>(-1));
    REQUIRE(graph.IsHandleValid(handles.depth));
    CHECK_FALSE(graph.WasRead(handles.depth));
    CHECK_FALSE(graph.WasWritten(handles.depth));
}

// Review Focus 3, CPU half. The operator is nonlinear, so the two composites differ
// even when the post chain is empty. This fails if someone "proves" identity by
// algebra that assumes a linear tonemap.
TEST_CASE("tonemap-then-blend differs from blend-then-tonemap", "[nri][overlay]")
{
    const glm::vec3 scene{ 0.02f, 0.02f, 0.04f };
    const glm::vec3 prim{ 0.8f, 0.8f, 0.2f };
    const float a = 0.5f;
    const glm::vec3 blendThen = TonemapOperator(prim * a + scene * (1.0f - a));
    const glm::vec3 tonemapThen = TonemapOperator(prim) * a + TonemapOperator(scene) * (1.0f - a);
    CHECK(blendThen != tonemapThen);
}
```

`TonemapOperator` is a C++ copy of the HLSL, in the test file, with the same constants:

```cpp
glm::vec3 AcesFilmic(glm::vec3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    glm::vec3 y = (x * (a * x + b)) / (x * (c * x + d) + e);
    return glm::clamp(y, glm::vec3(0.0f), glm::vec3(1.0f));
}
float LinearToSrgb1(float lin)
{
    return std::min(lin * 12.92f, std::pow(std::max(lin, 0.0031308f), 1.0f / 2.4f) * 1.055f - 0.055f);
}
glm::vec3 TonemapOperator(glm::vec3 linear)
{
    const glm::vec3 mapped = AcesFilmic(linear);
    return { LinearToSrgb1(mapped.x), LinearToSrgb1(mapped.y), LinearToSrgb1(mapped.z) };
}
```

`WasRead` is added in Step 3. The runtime-frame case below uses the same `NodeIndexOrNpos`.

GPU cases live in the same file. The helpers they call are in `NriGraphPixelTest.cpp`'s anonymous namespace and are not linkable from here. Paste these ranges into this file's anonymous namespace, unchanged:

- `kW`, `kH`, `PixelVehicle`, `MakeVehicle`, `Rgba`, `At`, `Luma`, `RenderOne` — `NriGraphPixelTest.cpp:139-234`. `kW` is 160 and `kH` is 96. `MakeVehicle` takes `(GraphicsBackend, const NodeSet& = {})`. The default `NodeSet` (`NriGraphContext.hpp:359-378`) already builds the mesh node; there is no mesh flag to set.
- `FillCamera` and `SupplyOne` — `NriGraphPixelTest.cpp:889-926`.
- `struct GpuSceneWorld` — `NriGraphPixelTest.cpp:2149-2187`.

Add the includes those ranges use, the same ones at the top of `NriGraphPixelTest.cpp` (`HostConfig.hpp`, `GpuSceneSync.hpp`, `VisibilitySystem.hpp`, `BoundsSystem.hpp`, `Components.hpp`, `SceneModule.hpp`, `SceneResources.hpp`, `TransformSystems.hpp`, `SceneCamera.hpp`, `MeshBuilder.hpp`, `RenderErrorLatch.hpp`, `NriDevice.hpp`, `GpuScene.hpp`, `NriMeshBufferCache.hpp`, `Helpers/GpuCapability.hpp`, `Astra/Registry/Registry.hpp`, `glm/gtc/matrix_transform.hpp`), plus `PostChainCache.hpp`, `ShaderCompiler.hpp`, `ShaderSourceProvider.hpp`, `ShaderConventions.hpp`, `MaterialSource.hpp`, and `MaterialInstance.hpp`.

`BuildGpuSceneFrame` in this task still takes `const MeshTable*` (`GpuSceneSync.hpp:285`). Task 6 changes that signature and updates the call below in the same commit as the other call sites.

A screen-space line is expanded in Step 3 into a quad of pixel `thickness`. The segments below are 16px long and 8px thick, so the named pixel centre sits inside the quad: `(80, 48)` is the centre of the 160x96 target (`NriGraphPixelTest.cpp:2249` reads that centre as the lit cube), and `(10, 10)` is the background corner that same capture reads.

```cpp
Arcane::OverlayLine ScreenLine(float x0, float y0, float x1, float y1, float z,
                               Arcane::OverlayDepthClass depth,
                               Arcane::OverlayColorSpace space,
                               glm::vec4 color)
{
    Arcane::OverlayLine line{};
    line.a = { x0, y0, z };
    line.b = { x1, y1, z };
    line.thickness = 8.0f;
    line.depth = depth;
    line.space = space;
    line.color = color;
    line.screenSpace = true;
    return line;
}

void CheckOverlayClasses(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    // The cube sequence at NriGraphPixelTest.cpp:2201-2238. Centre pixel is the
    // lit red cube; pixel (10, 10) is background.
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    GpuSceneWorld w;
    w.AddMesh(cubeId, cube);
    w.Spawn(glm::vec3(0.0f), cubeId);
    w.Schedulers();

    Arcane::MeshSceneDesc scene;
    FillCamera(scene);
    Arcane::ViewTransform view;
    view.view       = scene.view;
    view.projection = scene.projection;
    view.viewport   = glm::uvec2{ kW, kH };

    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, view, vis);
    Arcane::GpuSceneMirror mirror;
    Arcane::GpuSceneFrame gpu;
    Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
    Arcane::BuildGpuSceneFrame(mirror, &vis, w.reg.GetResource<Arcane::MeshTable>(), view, gpu);
    REQUIRE(gpu.stage.rows.size() == 1u);
    for (Arcane::GpuInstance& row : gpu.stage.values)
        row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list, bool withMesh) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        if (withMesh)
            frame.mesh = &scene;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    // pos.z = 1 is the far plane (clip.w is 1). The cube's front face is nearer
    // (eye at z = 4, BuildCube(2) half-extent 1, near 0.1, far 100). Forward-Z
    // discards when fragD > sceneD, so this green line is hidden. A shader that
    // writes clip.z = 0 puts the line on the near plane and the centre goes green.
    Arcane::OverlayPrimitiveList hidden;
    hidden.lines.push_back(ScreenLine(72.0f, 48.0f, 88.0f, 48.0f, 1.0f,
        Arcane::OverlayDepthClass::DepthTested,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));
    const std::vector<unsigned char> covered = shoot(hidden, true);
    const Rgba cubePixel = At(covered, kW, kW / 2u, kH / 2u);
    CHECK(cubePixel.r > cubePixel.g + 60);

    Arcane::OverlayPrimitiveList onTop = hidden;
    onTop.lines[0].depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    const std::vector<unsigned char> top = shoot(onTop, true);
    const Rgba topPixel = At(top, kW, kW / 2u, kH / 2u);
    CHECK(topPixel.r == 0);
    CHECK(topPixel.g == 255);
    CHECK(topPixel.b == 0);

    // Display-referred (1,0,0) is not ACES(1,0,0). ACES(1) is about 0.8038,
    // which encodes to a byte near 231, not 255. Pixel (10, 10) is off the cube.
    Arcane::OverlayPrimitiveList red;
    red.lines.push_back(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 0.0f,
        Arcane::OverlayDepthClass::AlwaysOnTop,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
    const std::vector<unsigned char> shown = shoot(red, true);
    const Rgba shownPixel = At(shown, kW, 10u, 10u);
    CHECK(shownPixel.r == 255);
    CHECK(shownPixel.g == 0);
    CHECK(shownPixel.b == 0);

    Arcane::OverlayPrimitiveList graded = red;
    graded.lines[0].space = Arcane::OverlayColorSpace::Linear;
    const std::vector<unsigned char> gradedPx = shoot(graded, true);
    const Rgba gradedPixel = At(gradedPx, kW, 10u, 10u);
    CHECK(gradedPixel.r > 200);
    CHECK(gradedPixel.r < 250);

    // No mesh: g_HasDepth is 0, so z = 1 is still drawn. The declaration test
    // above is what fails if this frame samples the placeholder depth.
    Arcane::OverlayPrimitiveList open;
    open.lines.push_back(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 1.0f,
        Arcane::OverlayDepthClass::DepthTested,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)));
    const std::vector<unsigned char> bare = shoot(open, false);
    const Rgba blue = At(bare, kW, 10u, 10u);
    CHECK(blue.b == 255);
    CHECK(Arcane::RenderErrorCount() == before);
}

// Review Focus 3, GPU half. An opaque (alpha 1) display-referred pixel is
// identical with and without a post chain that tints the scene, because the
// overlay blends after the tonemap and alpha 1 replaces the destination. A
// background pixel the line does not cover changes. The CPU test above is the
// partial-alpha nonlinearity; this one is the pass order.
void CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    // The compile path is MaterialSourceTest.cpp:624-672, in post mode
    // (externalInput true, the sentinel case at :434-450). Bytes are copied
    // into PostChainDesc the way PostChainCache.cpp:271-282 publishes them.
    // CompileNow runs in the test, before either RenderFrame. PrepareChain's
    // DeviceWaitIdle (FullscreenNodes.cpp:867-871) runs only when m_ready is
    // already true; this chain is bound once, so that branch does not run.
    Arcane::ShaderSourceProvider provider;
    provider.AddRoot(Arcane::Test::RepoRoot() / "data" / "shaders");
    const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
    REQUIRE(templateText.has_value());

    const char* shade =
        "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv) * 0.25; }\n";
    const std::uint32_t sceneIn[] = { Arcane::kSceneInput };
    const Arcane::MaterialChainPassDesc authored[] = { { shade, sceneIn } };
    Arcane::MaterialChainBuildResult built = Arcane::BuildMaterialChainSource(
        *templateText, authored, "tint_quarter", {}, /*externalInput=*/true);
    REQUIRE(built.Ok());
    REQUIRE(built.hlsl.size() == 1u);
    REQUIRE(built.passInputs.size() == 1u);
    REQUIRE(built.passInputs[0].size() == 1u);
    REQUIRE(built.passInputs[0][0] == Arcane::kSceneInput);

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(0.0));
    auto compileStage = [&](const char* entry, const char* profile) {
        Arcane::ShaderCompileRequest req;
        req.debugName = "tint_quarter.hlsl";
        req.sourceUtf8 = built.hlsl[0];
        req.entry = entry;
        req.profile = profile;
        const Arcane::ShaderCompileResult cr = compiler.CompileNow(req);
        REQUIRE(cr.AllSucceeded());
        const Arcane::ShaderTargetResult& target =
            backend == Arcane::GraphicsBackend::D3D12 ? cr.dxil : cr.spirv;
        REQUIRE(target.succeeded);
        REQUIRE_FALSE(target.bytecode.empty());
        return std::make_shared<const std::vector<std::uint8_t>>(target.bytecode);
    };
    auto templ = std::make_shared<Arcane::MaterialTemplate>(std::move(built.templ));
    Arcane::PostChainDesc chain;
    chain.templ = templ;
    chain.instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    chain.chainInputSlots = built.chainInputSlots;
    Arcane::PostChainPassDesc pass;
    pass.vsBytes = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
    pass.psBytes = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
    pass.inputs = built.passInputs[0];
    chain.passes.push_back(std::move(pass));
    compiler.Shutdown();

    PixelVehicle v = MakeVehicle(backend);
    Arcane::OverlayPrimitiveList list;
    list.lines.push_back(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 0.0f,
        Arcane::OverlayDepthClass::AlwaysOnTop,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));

    auto shoot = [&](const Arcane::PostChainDesc* post) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        frame.post = post;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };
    const std::vector<unsigned char> noPost = shoot(nullptr);
    const std::vector<unsigned char> withPost = shoot(&chain);

    // (10, 10) is inside the 8px-thick segment. (4, 4) is outside it
    // (the quad covers y in [6, 14]) and is the tonemapped clear colour.
    const Rgba overlayA = At(noPost, kW, 10u, 10u);
    const Rgba overlayB = At(withPost, kW, 10u, 10u);
    CHECK(overlayA.r == 0);
    CHECK(overlayA.g == 255);
    CHECK(overlayA.b == 0);
    CHECK(overlayA.r == overlayB.r);
    CHECK(overlayA.g == overlayB.g);
    CHECK(overlayA.b == overlayB.b);

    const Rgba worldA = At(noPost, kW, 4u, 4u);
    const Rgba worldB = At(withPost, kW, 4u, 4u);
    CHECK((worldA.r != worldB.r || worldA.g != worldB.g || worldA.b != worldB.b));
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("overlay depth class and colour space (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckOverlayClasses(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("overlay depth class and colour space (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckOverlayClasses(Arcane::GraphicsBackend::Vulkan);
}
TEST_CASE("post chain does not grade an opaque overlay pixel (d3d12)", "[gpu][overlay][shadercompile][d3d12]")
{
    CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("post chain does not grade an opaque overlay pixel (vulkan)", "[gpu][overlay][shadercompile][vulkan]")
{
    CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend::Vulkan);
}

// Review Focus 5, perspective half. The screen-space lines above write clip.w = 1,
// so fragD = SV_Position.z / SV_Position.w still equals SV_Position.z and those
// checks stay green under the wrong formula. This one does not.
//
// FillCamera (eye z = 4, fovY 60, near 0.1, far 100, perspectiveRH_ZO) puts the
// cube's front face (world z = +1, view depth 3) at ndc.z ≈ 0.968. A world point
// on z = 0 has view depth 4 and ndc.z ≈ 0.976, farther than the front face and
// nearer than the depth clear of 1. The wrong formula divides that ndc.z by
// SV_Position.w (1/clip.w), producing ≈ ndc.z * viewDepth ≈ 3.9, which is past
// the clear and discards the unoccluded axis as well as the in-front line.
void CheckPerspectiveGridDepth(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    GpuSceneWorld w;
    w.AddMesh(cubeId, cube);
    w.Spawn(glm::vec3(0.0f), cubeId);
    w.Schedulers();

    Arcane::MeshSceneDesc scene;
    FillCamera(scene);
    Arcane::ViewTransform view;
    view.view       = scene.view;
    view.projection = scene.projection;
    view.viewport   = glm::uvec2{ kW, kH };

    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, view, vis);
    Arcane::GpuSceneMirror mirror;
    Arcane::GpuSceneFrame gpu;
    Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
    Arcane::BuildGpuSceneFrame(mirror, &vis, w.reg.GetResource<Arcane::MeshTable>(), view, gpu);
    for (Arcane::GpuInstance& row : gpu.stage.values)
        row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh = &scene;
        frame.overlay = &list;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    // Axis only. Camera at y = 0 looking down -Z, so the world-X axis projects
    // onto y = kH/2. The cube's front face covers about x in [52, 108]; x = 10
    // is beside it. fwidth is about one pixel, so accept the midline and its
    // two neighbours. The wrong divide discards all three.
    Arcane::OverlayPrimitiveList grid;
    grid.hasGrid3d = true;
    grid.grid3d.view = view;
    grid.grid3d.SetPlane(Arcane::GridSceneDesc::Plane::XZ);
    grid.grid3d.minorSpacing = 1.0f;
    grid.grid3d.majorEvery = 8.0f;
    grid.grid3d.fadeDistance = 1000.0f;
    grid.grid3d.minHalfExtent = 20.0f;
    grid.grid3d.extentPerAltitude = 1.0f;
    grid.grid3d.minorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    grid.grid3d.majorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    grid.grid3d.axisUColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    grid.grid3d.axisVColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<unsigned char> gridPx = shoot(grid);
    const Rgba centre = At(gridPx, kW, kW / 2u, kH / 2u);
    CHECK(centre.r > centre.g + 60);
    bool axis = false;
    for (std::uint32_t y = (kH / 2u) - 1u; y <= (kH / 2u) + 1u; ++y)
    {
        const Rgba px = At(gridPx, kW, 10u, y);
        if (px.g > 200 && px.g < 250 && px.r < 80)
            axis = true;
    }
    CHECK(axis);

    auto worldLine = [](float z) {
        Arcane::OverlayLine line{};
        line.a = { -0.4f, 0.0f, z };
        line.b = {  0.4f, 0.0f, z };
        line.thickness = 0.5f;
        line.depth = Arcane::OverlayDepthClass::DepthTested;
        line.space = Arcane::OverlayColorSpace::DisplayReferred;
        line.color = { 0.0f, 1.0f, 0.0f, 1.0f };
        line.screenSpace = false;
        return line;
    };
    Arcane::OverlayPrimitiveList inFront;
    inFront.lines.push_back(worldLine(2.0f));
    const Rgba front = At(shoot(inFront), kW, kW / 2u, kH / 2u);
    CHECK(front.r == 0);
    CHECK(front.g == 255);
    CHECK(front.b == 0);

    Arcane::OverlayPrimitiveList behind;
    behind.lines.push_back(worldLine(-2.0f));
    const Rgba back = At(shoot(behind), kW, kW / 2u, kH / 2u);
    CHECK(back.r > back.g + 60);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("a perspective grid axis is hidden on the cube and visible beside it (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckPerspectiveGridDepth(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("a perspective grid axis is hidden on the cube and visible beside it (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckPerspectiveGridDepth(Arcane::GraphicsBackend::Vulkan);
}
```

`compile-shaders.bat` is an explicit list. The test also reads the bat and requires both lines:

```cpp
CHECK(bat.find("call :compile overlay vs_main vs_6_5 overlay_vs") != std::string::npos);
CHECK(bat.find("call :compile overlay ps_main ps_6_5 overlay_ps") != std::string::npos);
CHECK(bat.find("call :compile overlay grid_vs_main vs_6_5 overlay_grid_vs") != std::string::npos);
CHECK(bat.find("call :compile overlay grid_ps_main ps_6_5 overlay_grid_ps") != std::string::npos);
```

- [ ] **Step 2: Generate, compile shaders by building ArcaneClient, run the overlay tag.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[overlay]"
```

Expected: FAIL (types missing, or the node is absent, or the GPU pixels do not match).

- [ ] **Step 3: Implement the overlay.**

Move `struct GridSceneDesc` (`GridNode.hpp:95-155`) verbatim into `GridSceneDesc.hpp` under `namespace Arcane`. Keep `SetPlane` and the three `ARC_CONSTANT` axis colours. `GridNode.hpp` includes that header.

`OverlayPrimitive.hpp` is the three enums and three structs in the Interfaces block. No NRI, no Astra.

`data/shaders/tonemap_operator.hlsl` contains the two functions cut from `tonemap.hlsl:37-49`, byte for byte, including the constants `2.51, 0.03, 2.43, 0.59, 0.14` and the `LinearToSrgb` body. `tonemap.hlsl` replaces those two function bodies with `#include "tonemap_operator.hlsl"`. `ps_main` stays:

```hlsl
float3 linearColor = g_Scene.Sample(g_Sampler, input.uv).rgb;
float3 display = LinearToSrgb(ACESFilmic(linearColor));
return float4(display, 1.0);
```

`data/shaders/overlay.hlsl`:

```hlsl
#include "tonemap_operator.hlsl"

struct VSIn
{
    float3 pos         : POSITION;
    float4 color       : COLOR0;
    float  screenSpace : TEXCOORD0;   // 1 = pixels, y down; 0 = world metres
    float  linearSpace : TEXCOORD1;   // 1 = run the operator; 0 = display-referred
    float  depthTest   : TEXCOORD2;   // 1 = sample-and-discard; 0 = always on top
};

struct VSOut
{
    float4 clip        : SV_Position;
    float4 color       : COLOR0;
    float  linearSpace : TEXCOORD0;
    float  depthTest   : TEXCOORD1;
};

cbuffer OverlayCB : register(b0)
{
    float4x4 g_ViewProjection;
    float2   g_Viewport;             // pixels
    float    g_DiscardIfGreater;     // 1 = ForwardZ (discard when frag > scene)
    float    g_HasDepth;             // 1 = sample g_Depth; 0 = draw unoccluded
};

Texture2D<float> g_Depth : register(t0);
SamplerState     g_Point : register(s0);

VSOut vs_main(VSIn input)
{
    VSOut o;
    if (input.screenSpace > 0.5)
    {
        float2 ndc;
        ndc.x = input.pos.x * (2.0 / g_Viewport.x) - 1.0;
        ndc.y = 1.0 - input.pos.y * (2.0 / g_Viewport.y);
        o.clip = float4(ndc, input.pos.z, 1.0);
    }
    else
    {
        o.clip = mul(g_ViewProjection, float4(input.pos, 1.0));
    }
    o.color = input.color;
    o.linearSpace = input.linearSpace;
    o.depthTest = input.depthTest;
    return o;
}

float4 ps_main(VSOut input) : SV_Target0
{
    if (g_HasDepth > 0.5 && input.depthTest > 0.5)
    {
        float2 uv = input.clip.xy / g_Viewport;
        float sceneD = g_Depth.Sample(g_Point, uv);
        // SV_Position.z is the post-divide depth. SV_Position.w is 1/clip.w.
        float fragD = input.clip.z;
        bool farther = (g_DiscardIfGreater > 0.5) ? (fragD > sceneD) : (fragD < sceneD);
        if (farther) discard;
    }
    float3 rgb = input.color.rgb;
    if (input.linearSpace > 0.5)
        rgb = LinearToSrgb(ACESFilmic(rgb));
    return float4(rgb, input.color.a);
}
```

The CPU sets `g_DiscardIfGreater` to `1.0f` when `NearerOrEqualOp(kSelectedDepthConvention) == nri::CompareOp::LESS_EQUAL`, else `0.0f`. `g_HasDepth` is `1` only when the frame's mesh pass produced `handles.depth`. Each vertex's `linearSpace` is `1` for `OverlayColorSpace::Linear` and `0` for `DisplayReferred`. Each vertex's `screenSpace` is `1` when `OverlayLine::screenSpace` (or the fill's) is true. Each vertex's `depthTest` is `1` for `OverlayDepthClass::DepthTested` and `0` for `AlwaysOnTop`.

`OverlayNode::Record` expands every line into two triangles before the draw. `thickness == 0` emits nothing. Otherwise `d = b.xy - a.xy`, `n = (-d.y, d.x) / length(d) * (thickness * 0.5)`, and the four corners are `a.xy ± n` and `b.xy ± n`, each with `z = a.z`. Screen-space `thickness` is pixels. World-space `thickness` is metres and the same corner math runs in world units with `screenSpace` 0, so the projected `clip.z` is what the discard compares. A fill `Rect` is the same two triangles over the box whose min corner is `a.xy` and whose size is `size`, at `z = a.z`. `Triangle` is one triangle `a, b, c`. `Circle` is a triangle fan of 16 segments of radius `radius` around `a.xy` at `z = a.z`. Gizmo lines and fills both go through this expansion; their `space` is `DisplayReferred` and their `depth` is `AlwaysOnTop`.

The analytic 3D grid is a second draw in the same pass: six vertices, no vertex buffer, the quad math from `grid.hlsl`'s `vs_main` / `ps_main` copied into `overlay.hlsl` as `grid_vs_main` / `grid_ps_main`. `compile-shaders.bat` gains two more lines beside the overlay pair:

```bat
call :compile overlay grid_vs_main vs_6_5 overlay_grid_vs || exit /b 1
call :compile overlay grid_ps_main ps_6_5 overlay_grid_ps || exit /b 1
```

`grid_vs_main` writes `depthTest = 1` and `clip = mul(g_ViewProjection, float4(world, 1.0))`, the same perspective quad `grid.hlsl`'s `vs_main` (`data/shaders/grid.hlsl:69`) builds. `grid_ps_main` computes the same linear grid colour `grid.hlsl`'s `ps_main` computes today, then `return float4(LinearToSrgb(ACESFilmic(linear.rgb)), linear.a);`. Its discard guard is the same six lines as `ps_main`, including `float fragD = input.clip.z;`. Paste that assignment into both functions so the source scan's second hit exists. Do not change `grid.hlsl` itself: `AddGridNode`'s direct test still loads `grid_vs` / `grid_ps` (`GridNode.cpp:37-38`) for a linear canvas, and that path is no longer on the frame.

Draw order inside `OverlayNode::Record`: 3D grid (when `hasGrid3d` or `shape.grid`), then lines and fills whose `depth` is `DepthTested`, then lines and fills whose `depth` is `AlwaysOnTop`. Depth write stays off (the pass has no depth attachment). Blend is straight alpha, source-over, on the tonemap target.

`OverlayNode::CreateBindings` is this node's own layout, matching `overlay.hlsl` (`b0`, `t0`, `s0`, all implicit space 0). Do not copy `GridNode::CreateBindings` (`GridNode.cpp:109-148`): that layout is one `CONSTANT_BUFFER` at `b1`, `registerSpace` 1, and `PoolSizes` (`:100-102`) says it has no textures and no samplers. `NriGraphContext`'s constructor (`:470`, `m_grid = GridNode::Create`) also constructs `OverlayNode`. That line is construction only.

The layout follows `OutlineNode::CreateBindings` (`PickOutlineNodes.cpp:736-794`) plus a sampler, and the register-space rule at `FullscreenNodes.cpp:142-147` (`rootConstantNum`, `rootDescriptorNum`, and `rootSamplerNum` stay 0; `rootRegisterSpace` is 0 because the set's `registerSpace` is 0). Value-initialise every desc, then assign fields, the same dedup contract those two functions state.

- Range 0: `baseRegisterIndex` 0, `CONSTANT_BUFFER`, `descriptorNum` 1, `VERTEX_SHADER | FRAGMENT_SHADER`. The vertex shader reads `g_ViewProjection` and `g_Viewport`. The pixel shader reads `g_DiscardIfGreater` and `g_HasDepth`.
- Range 1: `baseRegisterIndex` 0, `TEXTURE`, `descriptorNum` 1, `FRAGMENT_SHADER`. This is `g_Depth` at `t0`.
- Range 2: `baseRegisterIndex` 0, `SAMPLER`, `descriptorNum` 1, `FRAGMENT_SHADER`. This is `g_Point` at `s0`.

Range order is CBV, then SRV, then sampler. `OutlineNode`'s comment (`PickOutlineNodes.cpp:748-754`) is why: D3D12 merges consecutive ranges of the same type, and these three types differ, so this is three ranges. On Vulkan, NRI shifts the HLSL register numbers by the device's `vkBindingOffsets` (`b=256`, `t=0`, `s=128`). Write the HLSL numbers.

`descriptorSetMaxNum`, `constantBufferMaxNum`, `textureMaxNum`, and `samplerMaxNum` are each `FramesInFlight()`. One set per frame slot: the constant bytes and the depth texture identity change per frame. Allocate with `AllocateDescriptorSets` the way `TonemapNode` does (`FullscreenNodes.cpp:1217`).

The sampler is `TonemapNode`'s point sampler (`FullscreenNodes.cpp:1154-1162`): `Filter::NEAREST` for min, mag, and mip, `AddressMode::CLAMP_TO_EDGE` on all three axes, `mipMax` 16. Create it once and write it into every set at `rangeIndex` 2.

The constant-buffer view is created once per slot and written once at `rangeIndex` 0, the pattern at `PickOutlineNodes.cpp:856-884`. Record copies that slot's bytes into the arena. The view's `(buffer, offset)` does not change.

The graph does not create a shader-resource view of scene depth. `RealizeAttachmentViews` (`RenderGraphExec.cpp:873-876`) creates only `DEPTH_STENCIL_ATTACHMENT` or `COLOR_ATTACHMENT`. `Read(..., RgUsage::ShaderRead)` adds `TextureUsageBits::SHADER_RESOURCE` (`RenderGraphExec.cpp:218`) and creates no view. At record, `context.Resolve` (`RenderGraph.hpp:234`) yields the `nri::Texture*`. `OverlayNode::EnsureView` is `OutlineNode::EnsureView` (`PickOutlineNodes.cpp:1019-1052`): `TextureView::TEXTURE`, `format = core.GetTextureDesc(*texture).format`, `mipNum` 1, `layerNum` 1. Bind that view at `rangeIndex` 1 when the texture pointer changes, the same update as `PickOutlineNodes.cpp:1088-1101`.

Scene depth defaults to `D32_SFLOAT` (`NriGraphContext.hpp:301`; `nri::Format::D32_SFLOAT` is `NRIDescs.h:339`, `R32_SFLOAT` is `:248`). NRI's D3D12 backend patches `DXGI_FORMAT_D32_FLOAT` to `DXGI_FORMAT_R32_FLOAT` for a `TEXTURE` view (`DescriptorD3D12.hpp:9-10`, and the `TEXTURE_2D` case at `:148-163`). `Texture2D<float>` matches that `R32_SFLOAT` view. Do not create a `DEPTH_STENCIL_ATTACHMENT` view and bind it at `t0`.

`overlay.hlsl` always declares `t0`. An unbound `t0` is a validation failure, including the missing-depth witness and every `RenderErrorCount()` check. The node therefore owns a 1x1 `R32_SFLOAT` texture created in `Create`, the committed-texture shape at `FullscreenNodes.cpp:212-220` with `format = nri::Format::R32_SFLOAT` and `usage = SHADER_RESOURCE`, plus one `TextureView::TEXTURE` of it. Upload a single `0.0f` through `HelperInterface` the way `CreateFallbackTexels` uploads its texels (`FullscreenNodes.cpp:235-260`), `rowPitch` 4 and `slicePitch` 4, so the resource is initialised even though the shader does not sample it. When the depth handle is invalid, do not `Read` a graph depth, set `g_HasDepth` to 0, and bind that dummy at `t0`. When the handle is valid, `Read(depth, RgUsage::ShaderRead)`, `EnsureView` the resolved scene depth, and set `g_HasDepth` to 1. The pipeline still has no depth attachment (`depthFormat` `UNKNOWN`, depth write off).

`BuildFrame` copies `shape.overlay = frame.overlay` and still copies `shape.grid`. `DeclareGraphFrame` deletes the block at `:1392-1397` (`if (shape.grid) AddGridNode(...)`). After `AddTonemapNode` returns `handles.backbuffer` and before gameui (`:1474`):

```cpp
const bool wantOverlay = (shape.grid != nullptr) ||
    (shape.overlay != nullptr &&
     (!shape.overlay->lines.empty() || !shape.overlay->fills.empty() || shape.overlay->hasGrid3d));
if (wantOverlay)
{
    AddOverlayNode(graph, context, handles.backbuffer,
                   wantsMesh ? handles.depth : RgTexture{},
                   shape.grid, shape.overlay, shape.canvasWidth, shape.canvasHeight);
}
```

`WasRead`, next to `WasWritten` (`RenderGraph.cpp:892`):

```cpp
bool RenderGraph::WasRead(RgTexture texture) const
{
    const std::size_t slot = DecodeAndValidateSlot(texture.index, m_textures.size());
    if (slot == kNoSlot)
        return false;
    for (const Access& access : m_accesses)
        if (access.isTexture && access.resourceIndex == slot && !access.isWrite)
            return true;
    return false;
}
```

Declare it beside `WasWritten` (`RenderGraph.hpp:698`).

`AddOverlayNode` declares node `"overlay"`, `NodeKind::Raster`, colour attachment `handles.backbuffer` with load-op `Load` (the tonemap already wrote it), no depth attachment. When `depth` is a valid handle it calls `builder.Read(depth, RgUsage::ShaderRead)`. When `depth` is invalid it does not `Read` any depth texture and the recorded constants set `g_HasDepth` to 0. It does not read the placeholder depth node `shape.depth` creates.

Update the frame-shape section at `RenderGraphTest.cpp:7089`. Today's names are `batch2d, gpuscene-sync, mesh-cull, mesh, grid, tonemap`. After Tasks 2 and 3 they are `gbuffer, lighting, batch2d, gpuscene-sync, mesh-cull, mesh, taa, tonemap, overlay`. There is no node named `"grid"`. `WasRead(handles.depth)` is true. `DepthLoadOp` of the overlay node is null. The depth transient is still the one the mesh node created (`NameOf` `"depth"`).

**Editor submissions move in this same commit.** Otherwise the grid disappears until Task 9.

- `SubmitSceneToBatcher` (`:1857`): stop calling `DrawGrid2D` (`:1887`). Build `OverlayLine`s by walking the same world segments `DrawGrid2D` would have emitted (call `DrawGrid2D` into a local `Batcher2D`, `Drain()` it, and convert each line span into `OverlayLine` with `depth = DepthTested`, `space = Linear`, `screenSpace = false`). Store them on `EditorApp` and point `FrameDesc::overlay` at that list from `ArmGraphViewportFrame`. Task 9 replaces this drain with `Grid2D` inside extract; this task only has to keep the lines on screen.
- Physics debug (`:1898-1922`) and the camera rect (`:1953-1981`) stop submitting into the scene batcher. Convert their `Line`/`Circle`/`Rect` calls into `OverlayLine` / `OverlayFill` tagged `AlwaysOnTop`, `Linear`, `screenSpace = false`. Physics debug has no `SetLayer` today; the tag is what keeps it above the grid.
- The gizmo lambda (`DrawViewportPanelPhase`, `:3713`) stops constructing `ImGuiGizmoSink`. It fills `OverlayLine` and `OverlayFill` from the same `Line`/`Triangle`/`Rect`/`Circle` callbacks, tagged `AlwaysOnTop`, `DisplayReferred`, `screenSpace = true`, positions in viewport pixels. The interaction `GetComponent` calls at `:1241`, `:1283`, `:1339` stay. Removing the ImGui sink draw here is what prevents a double draw once Task 9 also emits those primitives.

`FrameDesc::grid` stays the 3D grid desc the editor already fills, and this task's node reads that pointer on a frame that did not call `BindPacket`. Task 9 stops the editor writing `vp.grid` (`EditorAppFrame.cpp:2108-2119`) and stops pointing `FrameDesc::overlay` at the list this task stores. Runtime frames leave `overlay` null and `grid` null, so they do not declare the node and `runtime-scene` node order is Task 2's order. This task's GPU cases set `frame.overlay` and do not call `BindPacket`; that path has to keep drawing.

Do not run the golden gate. `editor-ui` and `editor-ui-perspective` will differ on grid pixels. That is expected until Task 13.

- [ ] **Step 4: Run the overlay tests and the device-less graph tests.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[overlay]"
.\ArcaneTests.exe "[nri]"
```

Expected: PASS. `[gpu]` cases require a GPU. Record the seed. `RenderErrorCount` stays 0.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/GridSceneDesc.hpp ArcaneClient/src/Arcane/Render/OverlayPrimitive.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/OverlayWitnessTest.cpp ArcaneTests/src/RenderGraphTest.cpp data/shaders/tonemap_operator.hlsl data/shaders/tonemap.hlsl data/shaders/overlay.hlsl data/shaders/compile-shaders.bat
git commit -m "feat(render): post-tonemap overlay pass for grid, debug, and gizmo" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 4: RenderFramePacket and the in-flight ring

**Files:**
- Create: `ArcaneCore/src/Arcane/Scene/SceneCameraView.hpp`
- Modify: `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:31-42` (delete the struct; include the new header beside `ViewTransform.hpp`)
- Create: `ArcaneClient/src/Arcane/Render/VisibleSet.hpp`
- Modify: `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp:48-84` (the two structs move; `BuildVisibleSet` at `:119` stays)
- Create: `ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp`
- Create: `ArcaneClient/src/Arcane/Render/RenderFramePacket.cpp`
- Test: `ArcaneTests/src/RenderFramePacketTest.cpp`
- Modify: `ArcaneTests/src/NriGraphPixelTest.cpp` (append the rebuild reach case after `CheckImGuiDrawsPastOnePool`)

**Interfaces:**
- Consumes: `VisibleEntry` / `VisibleSet` fields (`VisibilitySystem.hpp:48-84`), `GpuSceneFrame` (`GpuSceneTypes.hpp:275-289`), `Batch2DVertex` (`Batcher2D.hpp:126-132`), `Batch2DDrawSpan` (`:139-157`), `Batch2DDrained` (`:163-188`, non-owning, valid until the next `Begin()`), `GlobalParams` (`ArcaneCore/src/Arcane/Material/GlobalParams.hpp:33-39`), `PickDrawable` (`PickEmit.hpp:46-70`, `Astra::Entity` by value), `OverlayPrimitiveList` from Task 3, `PostChainDesc` (`PostChainCache.hpp:66-78`), `ViewTransform` (`ViewTransform.hpp:50`), `SceneCameraView` (`SceneCamera.hpp:31` today; this task moves it), `ActiveSceneCamera`'s return type `std::optional<SceneCameraView>` (`SceneCamera.hpp:57`), `ActivePerspectiveSceneCamera`'s return type `std::optional<ViewTransform>` (`SceneCamera.hpp:206`), `ImTextureID` as `ImU64` (`imgui.h:345`), `ImTextureRef::_TexData` (`imgui.h:383`, public), `ImDrawCmd::GetTexID` (`imgui.h:4073-4081`, asserts when `_TexData != nullptr` and that object's `TexID` is still `ImTextureID_Invalid`), `ImDrawCallback` (`imgui.h:3303`), `ImDrawData` fields (`imgui.h:3590-3601`: `Valid`, `CmdListsCount`, `TotalIdxCount`, `TotalVtxCount`, `CmdLists`, `DisplayPos`, `DisplaySize`, `FramebufferScale`, `OwnerViewport`, `Textures`), `FramesInFlight()`, `kMaxFramesInFlight`. `ViewTransform` has no `operator==`; tests compare `.view`, `.projection`, and `.viewport`.
- Produces:
  - `struct SceneCameraView` in `SceneCameraView.hpp`, with the three fields that are at `SceneCamera.hpp:31-42` today: `glm::vec2 worldCenter`, `float halfHeight`, `ViewTransform view`.
  - `struct RenderOrigin { glm::vec3 translation{0.0f}; };`
  - `struct ImDrawListSnapshot` and `struct ImDrawSnapshot` as written in Step 3. One snapshot owns its lists. A command with `TexRef._TexData == nullptr` stores `ImTextureID` as `ImU64`, initialised to `0` (`ImTextureID_Invalid`). A command with `TexRef._TexData != nullptr` stores that pointer and leaves the `ImU64` at `0`. `hasTextures` distinguishes a null `ImDrawData::Textures` from an empty list. The pointer vector is a copy of the pointers, not a deep copy of `ImTextureData`.
  - `bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out);` — `false` and an empty `out` when any command has `UserCallback != nullptr`. It does not call `GetTexID` on a managed command.
  - `class ImDrawSnapshotRebuild` — builds an `ImDrawData` by direct field assignment. `CmdLists` point at `IM_NEW`'d `ImDrawList`s owned by the rebuild object. It never calls `AddDrawList`. `m_textures` is a member so `Data()->Textures` stays valid for the caller's lifetime.
  - `struct RenderFramePacket` with the fields listed in Step 3. `spriteView` is `std::optional<SceneCameraView>`. `meshView` is `std::optional<ViewTransform>`. No `Astra::Registry` member. The header includes `SceneCameraView.hpp`. It does not include `Astra/Registry/Registry.hpp`, `SceneCamera.hpp`, `VisibilitySystem.hpp`, or `GridNode.hpp`.
  - `class PacketRing` with `RenderFramePacket& BeginExtract();`, `RenderFramePacket& Latest();`, and `const RenderFramePacket& Slot(std::uint32_t extractsAgo) const;`.
  - `BeginExtract` returns the slot at `extractCount % FramesInFlight()`, clears that slot, then increments `extractCount`. `Latest()` returns that same slot.

`ImDrawData` has no clone. `AddDrawList` (`imgui.h:3606`) is the only insert helper, and it is not usable on a hand-filled list: `AddDrawListToDrawDataEx` (`imgui_draw.cpp:2303-2308`) asserts `_VtxWritePtr == VtxBuffer.Data + VtxBuffer.Size` and `_IdxWritePtr == IdxBuffer.Data + IdxBuffer.Size`, and (`:2326`) `_VtxCurrentIdx < 65536`. Those write pointers are left unset by a `VtxBuffer.push_back` the caller did itself. `ImDrawList::CloneOutput` (`imgui.h:3530`, `imgui_draw.cpp:503`) copies the buffers and still leaves the write pointers unset and `SharedData` null. The rebuild therefore `push_back`s each owned list onto `m_data.CmdLists` and increments `CmdListsCount`, `TotalVtxCount`, and `TotalIdxCount` itself (`imgui_draw.cpp:2335-2338` is what `AddDrawList` would have done). `ImGuiNri::RenderDrawData` (`ImGuiNri.cpp:782`) does not read `draw_data->Textures`. It reads `DisplaySize` (`:785`), `TotalVtxCount` (`:794`), each list's `VtxBuffer` / `IdxBuffer` (`:849-857`), `DisplayPos` (`:864`, `:935`), `FramebufferScale` (`:936`), `UserCallback` (asserted null at `:954`), `ClipRect`, `TexRef._TexData` (`:980-991`), `GetTexID` (`:993`), `ElemCount`, `IdxOffset`, and `VtxOffset` (`baseVertex = globalVtxOffset + cmd->VtxOffset`, `:1025`). A flat command vector would make that `VtxOffset` address the wrong buffer, so the snapshot keeps one vertex/index/command vector per list.

`NewFrameTexUpdates` (`ImGuiNri.cpp:772-779`) is what reads `drawData->Textures`, and it returns immediately when that pointer is null. `ImGuiNriNode::PrepareFrame` (`ImGuiNriNode.cpp:41-44`) calls it at declaration (`ImGuiNriNode.cpp:76-91`), before `Record` (`:58-60`). `ImGuiNri.hpp:184-204` says the upload submits and waits and must stay at declaration time. This plan does not service textures on the main thread before the snapshot is published: `NewFrameTexUpdates` blocks, and extract has no graveyard. The snapshot carries the pointer list. Service stays at declaration time, on the rebuilt `ImDrawData`. `ImGui::Render` assigns `Textures = &GetPlatformIO().Textures` (`imgui.cpp:6104`) before the graph is declared, so on the first frame and on an atlas rebuild `TexID` is still `ImTextureID_Invalid` when extract copies. Storing only `GetTexID()` at copy time would keep that invalid id, `GetTexID` would assert at record (`imgui.h:4080-4081`), and the unregistered guard (`ImGuiNri.cpp:980-991`) would drop the draw. Each copied command therefore keeps `TexRef._TexData` when the live command has one, and the raw `ImTextureID` only when `_TexData` is null (the user `AddImage` path; that path still stores `ImU64`). Rebuild writes `cmd.TexRef._TexData = managed` and leaves `_TexID` invalid when `managed` is non-null (`ImTextureRef::GetTexID`, `imgui.h:4068`, asserts they are not both set). Two stack rebuilds, `PrepareFrame` and `Record`, both point at the same `ImTextureData` objects, so the `SetTexID` from declaration is visible at record. The GPU still never reads packet memory.

`VisibleSet.hpp` includes `Aabb.hpp`, `Frustum.hpp`, `ViewTransform.hpp`, `Astra/Entity/Entity.hpp`, and `<vector>`. It does not include `Registry.hpp`. `VisibilitySystem.hpp` includes `VisibleSet.hpp`, deletes the two struct definitions, and keeps `SceneVisibility`, `MainVisibleSet`, and `BuildVisibleSet`.

- [ ] **Step 1: Write the failing test.**

`ArcaneTests/src/RenderFramePacketTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Render/FramePacing.hpp>

#include "Helpers/ConstantScan.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cstring>
#include <fstream>
#include <sstream>

TEST_CASE("a packet slot stays intact until FramesInFlight extracts have passed", "[nri][packet]")
{
    Arcane::LatchFramesInFlight();
    const std::uint32_t n = Arcane::FramesInFlight();
    REQUIRE(n >= 2u);
    REQUIRE(n <= Arcane::kMaxFramesInFlight);

    Arcane::PacketRing ring;
    Arcane::RenderFramePacket& first = ring.BeginExtract();
    CHECK(&ring.Latest() == &first);
    first.batchVertices.push_back(Arcane::Batch2DVertex{});
    first.batchVertices.back().pos = { 1.0f, 2.0f, 3.0f };
    first.origin.translation = { 0.0f, 0.0f, 0.0f };
    const Arcane::Batch2DVertex* span = first.batchVertices.data();
    const float kept = span->pos.x;

    for (std::uint32_t i = 0; i < n - 1; ++i)
    {
        Arcane::RenderFramePacket& slot = ring.BeginExtract();
        slot.batchVertices.push_back(Arcane::Batch2DVertex{});
        slot.batchVertices.back().pos = { 9.0f, 9.0f, 9.0f };
    }

    const Arcane::RenderFramePacket& still = ring.Slot(n - 1);
    REQUIRE(still.batchVertices.size() == 1u);
    CHECK(still.batchVertices[0].pos.x == kept);
    CHECK(still.origin.translation == glm::vec3(0.0f));
    // The span handed out at extract still addresses that slot's vector.
    CHECK(span == still.batchVertices.data());

    Arcane::RenderFramePacket& overwritten = ring.BeginExtract();
    CHECK(overwritten.batchVertices.empty());   // BeginExtract clears the recycled slot
}

TEST_CASE("mutating a source vector after the copy does not change the packet", "[nri][packet]")
{
    std::vector<Arcane::Batch2DVertex> source(1);
    source[0].pos = { 4.0f, 5.0f, 6.0f };
    Arcane::RenderFramePacket packet;
    packet.batchVertices = source;
    source[0].pos = { 0.0f, 0.0f, 0.0f };
    source.clear();
    CHECK(packet.batchVertices.size() == 1u);
    CHECK(packet.batchVertices[0].pos.x == 4.0f);
}

TEST_CASE("RenderFramePacket.hpp does not include the Astra registry", "[nri][packet]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("Registry.hpp") == std::string::npos);
    CHECK(text.find("Astra::Registry") == std::string::npos);
    CHECK(text.find("VisibilitySystem.hpp") == std::string::npos);
    CHECK(text.find("SceneCamera.hpp") == std::string::npos);
    CHECK(text.find("SceneCameraView.hpp") != std::string::npos);
    CHECK(text.find("optional<SceneCameraView> spriteView") != std::string::npos);
    CHECK(text.find("optional<ViewTransform> meshView") != std::string::npos);
}

TEST_CASE("spriteView is a SceneCameraView and meshView is a ViewTransform", "[nri][packet]")
{
    Arcane::RenderFramePacket packet;
    Arcane::SceneCameraView sprite;
    sprite.worldCenter = { 1.0f, 2.0f };
    sprite.halfHeight = 3.0f;
    sprite.view.view = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, 0.0f));
    sprite.view.projection = glm::mat4(1.0f);
    sprite.view.viewport = { 8u, 9u };
    packet.spriteView = sprite;
    Arcane::ViewTransform mesh;
    mesh.projection = glm::mat4(2.0f);
    packet.meshView = mesh;
    REQUIRE(packet.spriteView.has_value());
    CHECK(packet.spriteView->worldCenter == glm::vec2(1.0f, 2.0f));
    CHECK(packet.spriteView->halfHeight == 3.0f);
    CHECK(packet.spriteView->view.view[3][0] == 4.0f);
    CHECK(packet.spriteView->view.viewport == glm::uvec2(8u, 9u));
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.meshView->projection[0][0] == 2.0f);
}

static void DummyImGuiCallback(const ImDrawList*, const ImDrawCmd*) {}

TEST_CASE("an ImDrawSnapshot matches the live draw data field for field and refuses a callback", "[nri][packet]")
{
    // A default ImDrawData has Textures == nullptr (imgui_draw.cpp:2284-2291).
    // That arm is not what ImGui::Render produces.
    ImDrawData cleared;
    Arcane::ImDrawSnapshot clearedSnap;
    REQUIRE(Arcane::CopyImDrawSnapshot(&cleared, clearedSnap));
    CHECK_FALSE(clearedSnap.hasTextures);
    CHECK(clearedSnap.textures.empty());
    Arcane::ImDrawSnapshotRebuild clearedRebuild(clearedSnap);
    CHECK(clearedRebuild.Data()->Textures == nullptr);
    CHECK(clearedRebuild.Data()->Valid);
    CHECK(clearedRebuild.Data()->CmdLists.Size == 0);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(128.0f, 128.0f);
    // Before NewFrame. NewFrame copies this into ImDrawListFlags_AllowVtxOffset
    // (imgui.cpp:5706-5707). A window list created afterwards carries the flag.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // Matches the live backend (ImGuiNri.cpp:119). Without it NewFrame demands a
    // built font atlas (imgui_draw.cpp:2778).
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    ImGui::Begin("one");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(42), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::Begin("two");
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(7), ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f));
    // Chrome already consumed some of the 16-bit index space. Pad from the
    // current _VtxCurrentIdx, not from zero, up to but not over the boundary.
    // The following AddRectFilled's PrimReserve then crosses it
    // (imgui_draw.cpp:737-743) and the new command's VtxOffset is the buffer
    // size at the crossing (imgui_draw.cpp:646-659, AddDrawCmd at :518).
    const int pad = (1 << 16) - static_cast<int>(list->_VtxCurrentIdx) - 1;
    REQUIRE(pad > 0);
    list->PrimReserve(0, pad);
    for (int i = 0; i < pad; ++i)
    {
        list->_VtxWritePtr->pos = ImVec2(0.0f, 0.0f);
        list->_VtxWritePtr->uv = ImVec2(0.0f, 0.0f);
        list->_VtxWritePtr->col = IM_COL32_WHITE;
        ++list->_VtxWritePtr;
        ++list->_VtxCurrentIdx;
    }
    list->AddRectFilled(ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f), IM_COL32(255, 0, 0, 255));
    ImGui::End();
    ImGui::Render();

    ImDrawData* src = ImGui::GetDrawData();
    REQUIRE(src != nullptr);
    REQUIRE(src->Valid);
    REQUIRE(src->CmdLists.Size >= 2);
    REQUIRE(src->Textures != nullptr);
    bool sawOffset = false;
    for (const ImDrawList* live : src->CmdLists)
    {
        REQUIRE(live != nullptr);
        for (const ImDrawCmd& cmd : live->CmdBuffer)
            if (cmd.UserCallback == nullptr && cmd.VtxOffset > 0u)
                sawOffset = true;
    }
    REQUIRE(sawOffset);

    Arcane::ImDrawSnapshot snap;
    REQUIRE(Arcane::CopyImDrawSnapshot(src, snap));
    REQUIRE(snap.lists.size() == static_cast<std::size_t>(src->CmdLists.Size));
    CHECK(snap.hasTextures);
    REQUIRE(snap.textures.size() == static_cast<std::size_t>(src->Textures->Size));
    for (int i = 0; i < src->Textures->Size; ++i)
        CHECK(snap.textures[static_cast<std::size_t>(i)] == (*src->Textures)[i]);
    CHECK(snap.displayPos == glm::vec2(src->DisplayPos.x, src->DisplayPos.y));
    CHECK(snap.displaySize == glm::vec2(src->DisplaySize.x, src->DisplaySize.y));
    CHECK(snap.framebufferScale == glm::vec2(src->FramebufferScale.x, src->FramebufferScale.y));

    bool saw42 = false;
    bool saw7 = false;
    for (std::size_t li = 0; li < snap.lists.size(); ++li)
    {
        const ImDrawList* live = src->CmdLists[static_cast<int>(li)];
        const Arcane::ImDrawListSnapshot& copied = snap.lists[li];
        REQUIRE(copied.vertices.size() == static_cast<std::size_t>(live->VtxBuffer.Size));
        REQUIRE(copied.indices.size() == static_cast<std::size_t>(live->IdxBuffer.Size));
        REQUIRE(copied.cmds.size() == static_cast<std::size_t>(live->CmdBuffer.Size));
        if (live->VtxBuffer.Size > 0)
            CHECK(std::memcmp(copied.vertices.data(), live->VtxBuffer.Data,
                              sizeof(ImDrawVert) * static_cast<std::size_t>(live->VtxBuffer.Size)) == 0);
        if (live->IdxBuffer.Size > 0)
            CHECK(std::memcmp(copied.indices.data(), live->IdxBuffer.Data,
                              sizeof(ImDrawIdx) * static_cast<std::size_t>(live->IdxBuffer.Size)) == 0);
        for (std::size_t ci = 0; ci < copied.cmds.size(); ++ci)
        {
            const ImDrawCmd& liveCmd = live->CmdBuffer[static_cast<int>(ci)];
            const Arcane::ImDrawListSnapshot::Cmd& c = copied.cmds[ci];
            CHECK(c.elemCount == liveCmd.ElemCount);
            CHECK(c.idxOffset == liveCmd.IdxOffset);
            CHECK(c.vtxOffset == liveCmd.VtxOffset);
            CHECK(c.clip.x == liveCmd.ClipRect.x);
            CHECK(c.clip.y == liveCmd.ClipRect.y);
            CHECK(c.clip.z == liveCmd.ClipRect.z);
            CHECK(c.clip.w == liveCmd.ClipRect.w);
            CHECK(c.managed == liveCmd.TexRef._TexData);
            if (liveCmd.TexRef._TexData == nullptr)
            {
                CHECK(c.texture == liveCmd.TexRef._TexID);
                if (c.texture == static_cast<ImTextureID>(42))
                    saw42 = true;
                if (c.texture == static_cast<ImTextureID>(7))
                    saw7 = true;
            }
            else
                CHECK(c.texture == static_cast<ImTextureID>(0));
        }
    }
    CHECK(saw42);
    CHECK(saw7);

    Arcane::ImDrawSnapshotRebuild rebuilt(snap);
    ImDrawData* data = rebuilt.Data();
    REQUIRE(data->Valid);
    CHECK(data->CmdListsCount == src->CmdListsCount);
    REQUIRE(data->CmdLists.Size == src->CmdLists.Size);
    CHECK(data->TotalVtxCount == src->TotalVtxCount);
    CHECK(data->TotalIdxCount == src->TotalIdxCount);
    CHECK(data->DisplayPos.x == src->DisplayPos.x);
    CHECK(data->DisplayPos.y == src->DisplayPos.y);
    CHECK(data->DisplaySize.x == src->DisplaySize.x);
    CHECK(data->DisplaySize.y == src->DisplaySize.y);
    CHECK(data->FramebufferScale.x == src->FramebufferScale.x);
    CHECK(data->FramebufferScale.y == src->FramebufferScale.y);
    REQUIRE(data->Textures != nullptr);
    REQUIRE(data->Textures->Size == src->Textures->Size);
    // The vector is the rebuild's. The ImTextureData objects are the live ones.
    CHECK(data->Textures != src->Textures);
    for (int i = 0; i < src->Textures->Size; ++i)
        CHECK((*data->Textures)[i] == (*src->Textures)[i]);
    for (int li = 0; li < data->CmdLists.Size; ++li)
    {
        const ImDrawList* live = src->CmdLists[li];
        const ImDrawList* copy = data->CmdLists[li];
        REQUIRE(copy->VtxBuffer.Size == live->VtxBuffer.Size);
        REQUIRE(copy->IdxBuffer.Size == live->IdxBuffer.Size);
        REQUIRE(copy->CmdBuffer.Size == live->CmdBuffer.Size);
        if (live->VtxBuffer.Size > 0)
            CHECK(std::memcmp(copy->VtxBuffer.Data, live->VtxBuffer.Data,
                              sizeof(ImDrawVert) * static_cast<std::size_t>(live->VtxBuffer.Size)) == 0);
        if (live->IdxBuffer.Size > 0)
            CHECK(std::memcmp(copy->IdxBuffer.Data, live->IdxBuffer.Data,
                              sizeof(ImDrawIdx) * static_cast<std::size_t>(live->IdxBuffer.Size)) == 0);
        for (int ci = 0; ci < copy->CmdBuffer.Size; ++ci)
        {
            const ImDrawCmd& a = live->CmdBuffer[ci];
            const ImDrawCmd& b = copy->CmdBuffer[ci];
            CHECK(b.ElemCount == a.ElemCount);
            CHECK(b.IdxOffset == a.IdxOffset);
            CHECK(b.VtxOffset == a.VtxOffset);
            CHECK(b.UserCallback == nullptr);
            CHECK(b.ClipRect.x == a.ClipRect.x);
            CHECK(b.ClipRect.y == a.ClipRect.y);
            CHECK(b.ClipRect.z == a.ClipRect.z);
            CHECK(b.ClipRect.w == a.ClipRect.w);
            CHECK(b.TexRef._TexData == a.TexRef._TexData);
            CHECK(b.TexRef._TexID == a.TexRef._TexID);
            // GetTexID asserts when _TexData is set and TexID is still Invalid
            // (imgui.h:4080-4081). This CPU frame never uploads the atlas.
            if (a.TexRef._TexData == nullptr)
                CHECK(b.GetTexID() == a.GetTexID());
        }
    }

    ImGui::NewFrame();
    ImGui::Begin("callback");
    ImGui::GetWindowDrawList()->AddCallback(&DummyImGuiCallback, nullptr);
    ImGui::End();
    ImGui::Render();
    Arcane::ImDrawSnapshot refused;
    CHECK_FALSE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), refused));
    CHECK(refused.lists.empty());
    CHECK_FALSE(refused.hasTextures);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
}
```

- [ ] **Step 2: Generate and run `[packet]`.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[packet]"
```

Expected: FAIL. `RenderFramePacket.hpp` does not exist.

- [ ] **Step 3: Implement the packet and the ring.**

`RenderFramePacket.hpp`:

```cpp
#pragma once

#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/GridSceneDesc.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/PostChainDesc.hpp>
#include <Arcane/Render/VisibleSet.hpp>
#include <Arcane/Scene/SceneCameraView.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    struct RenderOrigin
    {
        glm::vec3 translation{ 0.0f };
    };

    struct ImDrawListSnapshot
    {
        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx>  indices;
        struct Cmd
        {
            ImVec4         clip{};
            ImTextureID    texture = 0;   // ImU64, used only when managed == nullptr. ImTextureID_Invalid is 0.
            ImTextureData* managed = nullptr;   // TexRef._TexData. Not owned.
            std::uint32_t  idxOffset = 0;
            std::uint32_t  elemCount = 0;
            std::uint32_t  vtxOffset = 0;
        };
        std::vector<Cmd> cmds;
    };

    struct ImDrawSnapshot
    {
        std::vector<ImDrawListSnapshot> lists;
        std::vector<ImTextureData*> textures;   // pointer copy. Not owned.
        bool hasTextures = false;               // false when src->Textures was null, including an empty default ImDrawData
        glm::vec2 displayPos{ 0.0f };
        glm::vec2 displaySize{ 0.0f };
        glm::vec2 framebufferScale{ 1.0f };
    };

    bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out);

    class ImDrawSnapshotRebuild
    {
    public:
        explicit ImDrawSnapshotRebuild(const ImDrawSnapshot& snap);
        ~ImDrawSnapshotRebuild();
        ImDrawData* Data() { return &m_data; }

        ImDrawSnapshotRebuild(const ImDrawSnapshotRebuild&) = delete;
        ImDrawSnapshotRebuild& operator=(const ImDrawSnapshotRebuild&) = delete;

    private:
        ImDrawData m_data{};
        ImVector<ImTextureData*> m_textures;
        std::vector<ImDrawList*> m_owned;
    };

    struct RenderFramePacket
    {
        std::optional<SceneCameraView> spriteView;
        std::optional<ViewTransform>   meshView;
        glm::uvec2                   viewport{ 0u, 0u };
        RenderOrigin                 origin{};

        GpuSceneFrame                mesh{};
        VisibleSet                   visible{};

        std::vector<Batch2DVertex>   batchVertices;
        std::vector<std::uint32_t>   batchIndices;
        std::vector<Batch2DDrawSpan> batchSpans;
        GlobalParams                 globals{};
        glm::mat4                    viewProjection{ 1.0f };

        std::vector<PickDrawable>    pickDrawables;
        OverlayPrimitiveList         overlay{};
        PostChainDesc                post{};
        bool                         hasPost = false;
        ImDrawSnapshot               imgui{};
        ImDrawSnapshot               gameUi{};
        std::vector<std::uint32_t>   selectedIds;

        glm::vec3 lightDirection{ 0.0f };
        glm::vec3 lightColor{ 0.0f };
        glm::vec3 ambient{ 0.0f };

        void Clear() noexcept;
    };

    class PacketRing
    {
    public:
        RenderFramePacket&       BeginExtract();
        RenderFramePacket&       Latest();
        const RenderFramePacket& Slot(std::uint32_t extractsAgo) const;

    private:
        std::array<RenderFramePacket, kMaxFramesInFlight> m_slots{};
        std::uint64_t m_extractCount = 0;
    };
}
```

Move `PostChainPassDesc` and `PostChainDesc` (`PostChainCache.hpp:55-78`) into a new `ArcaneClient/src/Arcane/Render/PostChainDesc.hpp` that does not include `Registry.hpp`. `PostChainCache.hpp` includes that header. `PickEmit.hpp` forward-declares `Astra::Registry` and does not include `Registry.hpp` (`PickEmit.hpp:35`); including it from the packet is safe. `GpuSceneTypes.hpp` includes `Astra/Entity/Entity.hpp` and does not include `Registry.hpp`.

`RenderFramePacket::Clear` clears every vector, resets both optionals, sets `origin.translation` to `{0,0,0}`, sets `hasPost` false, and default-constructs `mesh`, `visible`, `overlay`, `post`, `globals`, and the three light vectors.

`PacketRing::BeginExtract`:

```cpp
RenderFramePacket& PacketRing::BeginExtract()
{
    const std::uint32_t depth = FramesInFlight();
    RenderFramePacket& slot = m_slots[m_extractCount % depth];
    slot.Clear();
    ++m_extractCount;
    return slot;
}

const RenderFramePacket& PacketRing::Slot(std::uint32_t extractsAgo) const
{
    const std::uint32_t depth = FramesInFlight();
    const std::uint64_t index = (m_extractCount - 1 - extractsAgo) % depth;
    return m_slots[static_cast<std::size_t>(index)];
}

RenderFramePacket& PacketRing::Latest()
{
    const std::uint32_t depth = FramesInFlight();
    return m_slots[static_cast<std::size_t>((m_extractCount - 1) % depth)];
}
```

`Latest()` is the slot `Slot(0)` names, non-const. Call it only after `BeginExtract` has run (`m_extractCount > 0`). `ExtractFrame` returns `const RenderFramePacket&` and does not write `selectedIds`. Spec 4.1 immutability holds for every other field. The only post-extract writers, both before the frame is recorded, are `ArmGraphViewportFrame`'s selection loop and the runtime `--pick-probe` stand-in. Both write `selectedIds` through `Latest()`. `Clear()` already clears that vector, so a slot starts empty.

`SceneCameraView.hpp`:

```cpp
#pragma once
#include <Arcane/Scene/ViewTransform.hpp>
#include <glm/vec2.hpp>
namespace Arcane
{
    struct SceneCameraView
    {
        glm::vec2 worldCenter{ 0.0f, 0.0f };
        float halfHeight = 0.0f;
        ViewTransform view{};
    };
}
```

`SceneCamera.hpp` includes that header beside `ViewTransform.hpp` and deletes the struct at `:31-42`. `ActiveSceneCamera` (`:57`) still returns `std::optional<SceneCameraView>`. `ActivePerspectiveSceneCamera` (`:206`) still returns `std::optional<ViewTransform>`.

`CopyImDrawSnapshot` and `ImDrawSnapshotRebuild`, in `RenderFramePacket.cpp`:

```cpp
bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out)
{
    out = {};
    if (!src)
        return true;
    out.displayPos = { src->DisplayPos.x, src->DisplayPos.y };
    out.displaySize = { src->DisplaySize.x, src->DisplaySize.y };
    out.framebufferScale = { src->FramebufferScale.x, src->FramebufferScale.y };
    if (src->Textures != nullptr)
    {
        out.hasTextures = true;
        out.textures.assign(src->Textures->Data, src->Textures->Data + src->Textures->Size);
    }
    out.lists.reserve(static_cast<std::size_t>(src->CmdLists.Size));
    for (const ImDrawList* list : src->CmdLists)
    {
        if (!list)
            continue;
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr)
            {
                out = {};
                return false;
            }
        }
        ImDrawListSnapshot snap;
        if (list->VtxBuffer.Size > 0)
            snap.vertices.assign(list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
        if (list->IdxBuffer.Size > 0)
            snap.indices.assign(list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            ImDrawListSnapshot::Cmd c;
            c.clip = cmd.ClipRect;
            c.idxOffset = cmd.IdxOffset;
            c.elemCount = cmd.ElemCount;
            c.vtxOffset = cmd.VtxOffset;
            // Do not call GetTexID here. A managed texture whose TexID is still
            // Invalid asserts (imgui.h:4073-4081). NewFrameTexUpdates stamps
            // that id later, at declaration.
            if (cmd.TexRef._TexData != nullptr)
            {
                c.managed = cmd.TexRef._TexData;
                c.texture = 0;
            }
            else
            {
                c.managed = nullptr;
                c.texture = cmd.TexRef._TexID;
            }
            snap.cmds.push_back(c);
        }
        out.lists.push_back(std::move(snap));
    }
    return true;
}

ImDrawSnapshotRebuild::ImDrawSnapshotRebuild(const ImDrawSnapshot& snap)
{
    m_data.Valid = true;
    m_data.DisplayPos = ImVec2(snap.displayPos.x, snap.displayPos.y);
    m_data.DisplaySize = ImVec2(snap.displaySize.x, snap.displaySize.y);
    m_data.FramebufferScale = ImVec2(snap.framebufferScale.x, snap.framebufferScale.y);
    m_data.OwnerViewport = nullptr;
    m_data.CmdLists.resize(0);
    m_data.CmdListsCount = 0;
    m_data.TotalVtxCount = 0;
    m_data.TotalIdxCount = 0;
    if (snap.hasTextures)
    {
        m_textures.resize(static_cast<int>(snap.textures.size()));
        for (int i = 0; i < m_textures.Size; ++i)
            m_textures[i] = snap.textures[static_cast<std::size_t>(i)];
        m_data.Textures = &m_textures;
    }
    else
        m_data.Textures = nullptr;
    for (const ImDrawListSnapshot& src : snap.lists)
    {
        ImDrawList* list = IM_NEW(ImDrawList(nullptr));
        if (!src.vertices.empty())
        {
            list->VtxBuffer.resize(static_cast<int>(src.vertices.size()));
            std::memcpy(list->VtxBuffer.Data, src.vertices.data(),
                        src.vertices.size() * sizeof(ImDrawVert));
        }
        if (!src.indices.empty())
        {
            list->IdxBuffer.resize(static_cast<int>(src.indices.size()));
            std::memcpy(list->IdxBuffer.Data, src.indices.data(),
                        src.indices.size() * sizeof(ImDrawIdx));
        }
        for (const ImDrawListSnapshot::Cmd& c : src.cmds)
        {
            ImDrawCmd cmd{};
            cmd.ClipRect = c.clip;
            if (c.managed != nullptr)
            {
                cmd.TexRef._TexData = c.managed;
                cmd.TexRef._TexID = ImTextureID_Invalid;
            }
            else
                cmd.TexRef = ImTextureRef(c.texture);
            cmd.IdxOffset = c.idxOffset;
            cmd.ElemCount = c.elemCount;
            cmd.VtxOffset = c.vtxOffset;
            cmd.UserCallback = nullptr;
            list->CmdBuffer.push_back(cmd);
        }
        m_owned.push_back(list);
        m_data.CmdLists.push_back(list);
        m_data.CmdListsCount += 1;
        m_data.TotalVtxCount += list->VtxBuffer.Size;
        m_data.TotalIdxCount += list->IdxBuffer.Size;
    }
}

ImDrawSnapshotRebuild::~ImDrawSnapshotRebuild()
{
    for (ImDrawList* list : m_owned)
        IM_DELETE(list);
}
```

`RenderFramePacket.cpp` includes `<cstring>` for the two `memcpy` calls. The destructor `IM_DELETE`s the owned lists and does not delete the `ImTextureData` pointers. `m_textures` outlives `Data()` for as long as the rebuild object does.

`ImGuiNri::RenderDrawData` still asserts `UserCallback == nullptr` (`ImGuiNri.cpp:954`). The copy refuses before a callback can be stored, which is what the packet test checks. Task 10 builds one stack `ImDrawSnapshotRebuild` in the declaration lambda and another in the record lambda. Both name the same `ImTextureData` objects. `PrepareFrame` is what calls `NewFrameTexUpdates`; `Record` passes `Data()` to `RenderDrawData`. The rebuild is not stored on the node.

Append to `NriGraphPixelTest.cpp`, after `CheckImGuiDrawsPastOnePool`. The helpers `MakeVehicle`, `RenderOne`, `At`, `kW`, and `kH` are file-local and already visible to later cases in that TU. Add `#include <Arcane/Render/RenderFramePacket.hpp>`. The case does not `BindPacket`, so it stays on `FrameDesc::imgui` after Task 10's packet branch. `NodeSet::hostHud` must be set or `ImGuiHud()` is null on an offscreen context (`NriGraphContext.hpp:359-366`). After Task 2, `FrameDesc::world` defaults true, so the frame also declares an empty `batch2d`; the rect still composites after tonemap. `AddRectFilled` samples the font atlas. `NewFrameTexUpdates` no-ops when `Textures` is null (`ImGuiNri.cpp:775-776`), the atlas `TexID` stays invalid, and the unregistered guard (`:980-991`) drops the draw. The red pixel is the witness that the rebuild carried `Textures` and that declaration serviced them before record. Colour is structural, the same rule as the rest of this file: display-referred, not an exact ACES byte.

```cpp
namespace
{
    void CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const imgui = ImGui::CreateContext();
        REQUIRE(imgui != nullptr);
        ImGui::SetCurrentContext(imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.DeltaTime = 1.0f / 60.0f;

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.hostHud = true;
        PixelVehicle v = MakeVehicle(backend, nodes);
        Arcane::ImGuiNriNode* const hud = v.ctx->ImGuiHud();
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(imgui);

        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(70.0f, 40.0f), ImVec2(90.0f, 56.0f), IM_COL32(255, 0, 0, 255));
        ImGui::Render();

        Arcane::ImDrawSnapshot snap;
        REQUIRE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), snap));
        REQUIRE(snap.hasTextures);
        Arcane::ImDrawSnapshotRebuild rebuilt(snap);
        REQUIRE(rebuilt.Data()->Textures != nullptr);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.imgui = rebuilt.Data();
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        REQUIRE(w == kW);
        REQUIRE(h == kH);
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.r > px.g + 60);
        CHECK(px.r > px.b + 60);
        CHECK(Arcane::RenderErrorCount() == before);

        v.ctx.reset();
        ImGui::DestroyContext(imgui);
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("an ImDrawSnapshot rebuild reaches ImGuiNri (d3d12)",
          "[gpu][packet][imgui][nri][d3d12]")
{
    CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("an ImDrawSnapshot rebuild reaches ImGuiNri (vulkan)",
          "[gpu][packet][imgui][nri][vulkan]")
{
    CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend::Vulkan);
}
```

Call `LatchFramesInFlight()` before the first `FramesInFlight()` use if it has not been latched; the test does that. The ring does not allocate `nri::Buffer` or `nri::Device` members.

A comment on `batchVertices` states the contract: `Batch2DDrained`'s spans die at the next `Begin()` (`Batcher2D.hpp:159-162`); the packet owns the copy. Task 5 is what performs that copy. This task does not change hosts.

Sprite rows from spec section 6 are not a field. Plans 1 and 2 carry world sprites as `batchVertices` / `batchIndices` / `batchSpans`.

- [ ] **Step 4: Run `[packet]`.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[packet]"
```

Expected: PASS. The CPU case matches every list, including one command with `VtxOffset > 0`, and the callback frame returns false. The two `[gpu][packet]` cases read a red-dominant pixel at `(80, 48)` and leave `RenderErrorCount` unchanged. Record the seed.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneCore/src/Arcane/Scene/SceneCameraView.hpp ArcaneCore/src/Arcane/Scene/SceneCamera.hpp ArcaneClient/src/Arcane/Render/VisibleSet.hpp ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp ArcaneClient/src/Arcane/Render/PostChainDesc.hpp ArcaneClient/src/Arcane/Render/PostChainCache.hpp ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp ArcaneClient/src/Arcane/Render/RenderFramePacket.cpp ArcaneTests/src/RenderFramePacketTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): RenderFramePacket ring sized to FramesInFlight" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 5: Sprites and the batch copy

**Files:**
- Create: `ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp`
- Create: `ArcaneCore/src/Arcane/Base/ExtractAccounting.cpp`
- Create: `ArcaneClient/src/Arcane/Render/ExtractFrame.hpp`
- Create: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp`
- Modify: `ArcaneRuntime/src/RuntimeFrame.hpp` (`FrameIo` at `:71`, add `PacketRing` as the last member)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:519-550` and the branch at `:748`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` (beside `CurrentBatch()` at `:1113`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` (clear the bound pointer at the end of `RenderFrame` and `RenderFrameOffscreen`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1618-1641`
- Modify: `ArcaneClient/src/Arcane/Client/ClientRuntime.cpp:45-49`
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (a `PacketRing` member on `EditorApp`, class at `:116`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` `RenderSceneToViewport` (`:1642`, editor view already computed at `:1685-1688`)
- Modify: `ArcaneTests/src/ClientRuntimeTest.cpp:38-47`
- Modify: `ArcaneTests/src/NriGraphPixelTest.cpp` (append the registered-material witness)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `PacketRing::BeginExtract` from Task 4. `Batch2DDrained` (`Batcher2D.hpp:163-188`). `RenderContext2D` (`SceneResources.hpp:109-122`) and `ClientRuntime::SetRenderContext` (`ClientRuntime.cpp:116-124`), which is what writes that resource. `RenderSubmissionSystem::operator()` (`RenderSystems.hpp:50` through its closing brace) returns immediately when `ctx->batcher` is null (`:52-53`) and submits a resolved material through `QuadWorld` (`:230-240`). `QuadWorld` rewrites an id that is `>= m_materials.size()` to `kMaterialSprite` (`Batcher2D.cpp:248-253`). `RegisterMaterial` appends and returns `size - 1`, so the first registered id is 3 (`Batcher2D.cpp:96-102`, `Batcher2D.hpp:209-215`). `Begin` clears geometry and resets the view-projection to identity (`Batcher2D.cpp:139-142`). `SetViewProjection` (`Batcher2D.hpp:421`) does not clear geometry. `SpriteMaterialCache::ConsumeResult` registers on the batcher it is given (`SpriteMaterialCache.cpp:260`), and the host passes its own batcher (`SceneRenderResolver.cpp:581`). `ActiveSceneCamera` returns `std::optional<SceneCameraView>` (`SceneCamera.hpp:57`). `ViewTransform::ViewProjection`. `Batch2DNode::Prepare` (`Batch2DNode.hpp:225`) looks up `MaterialDesc` on the batcher it is passed (`Batch2DNode.cpp:1170`); a missing pipeline draws through the plain sprite pipeline (`:1247`, `:1342-1345`). `RenderFrameOffscreen` sets `m_currentBatch = effective.batch` (`NriGraphContext.cpp:1909`). `FrameIo` designated init at `RuntimeApp.cpp:732` (a new member with a default constructor needs no new initializer). Extract does not call `Batcher2D::Create`.
- Produces the stable request and the one extract entry. Later tasks fill fields of this same struct. They do not add members.

```cpp
struct ExtractRequest
{
    glm::uvec2                   viewport{ 0u, 0u };
    std::optional<ViewTransform> editorView;
    bool                         playMode = false;
    bool                         showGrid2d = false;
    bool                         showGrid3d = false;
    bool                         perspective = false;
    GridSceneDesc                grid3d{};
    float                        gridMinorAlpha = 0.35f;
    float                        gridMajorAlpha = 0.55f;
    glm::vec3                    gridLineRgb{ 0.5f, 0.5f, 0.5f };
    float                        gridLineThicknessPx = 1.0f;
    std::int64_t                 gridMaxLinesPerAxis = 4096;
    glm::vec4                    gridAxisX{ 0.85f, 0.25f, 0.25f, 0.9f };
    glm::vec4                    gridAxisY{ 0.3f, 0.8f, 0.3f, 0.9f };
    bool                         physicsDebug = false;
    bool                         physicsWholeWorld = false;
    bool                         cameraRect = false;
    glm::vec4                    cameraFrameColor{ 0.45f, 0.62f, 0.78f, 0.75f };
    OverlayPrimitiveList         gizmoPrims{};
    const PostChainDesc*         post = nullptr;
    const ImDrawData*            imgui = nullptr;    // copied in Task 10; the pointer is not stored
    const ImDrawData*            gameUi = nullptr;
    glm::vec3                    lightDirection{ 0.0f };
    glm::vec3                    lightColor{ 0.0f };
    glm::vec3                    ambient{ 0.0f };
    GpuScene*                    device = nullptr;
};

const RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring);

void NoteExtractFrameCall() noexcept;
std::uint64_t ExtractFrameCallCount() noexcept;
void ResetExtractFrameCallCount() noexcept;
```

`ExtractFrame.hpp` forward-declares `namespace Astra { class Registry; }` and `class GpuScene`. It includes `RenderFramePacket.hpp`. It does not include `Astra/Registry/Registry.hpp` or `SceneCamera.hpp`. `ExtractFrame.cpp` is the one `.cpp` under `ArcaneClient/src/Arcane/Render/` that includes `Registry.hpp`.

`NriGraphContext` gains:

```cpp
void BindPacket(const RenderFramePacket* packet) noexcept;
const RenderFramePacket* Packet() const noexcept;
```

- [ ] **Step 1: Write the failing test.**

Create `ArcaneTests/src/ExtractFrameTest.cpp`. Copy `struct World` from `GpuSceneSyncTest.cpp:32-105` into this file's anonymous namespace (that copy is not visible to other TUs). Add these methods inside `World`:

```cpp
Astra::Entity sprite{};
Astra::Entity AddSprite(glm::vec3 pos)
{
    sprite = reg.CreateEntity();
    Arcane::Transform t;
    t.position = pos;
    reg.AddComponent<Arcane::Transform>(sprite, t);
    reg.SetParent(sprite, root);
    reg.AddComponent<Arcane::SpriteRenderer>(sprite, Arcane::SpriteRenderer{});
    return sprite;
}
```

`WorldTransform` is a `glm::mat4` (`Components.hpp:87-91`). `SpriteRenderer::shape` defaults to `Rect` (`Components.hpp:152`). `GetComponent` returns a pointer. An unresolved Rect sprite is a 1x1 m quad: `Batcher2D.cpp:437-440` pushes four vertices.

`RenderSubmissionSystem` submits nothing when `RenderContext2D::batcher` is null (`RenderSystems.hpp:52-53`). The test installs the host batcher and `Begin`s it before `ExtractFrame`. Extract does not `Begin`. `editorView` stays so Task 6's visible set still contains the origin quad.

```cpp
TEST_CASE("extract copies one sprite quad and ignores a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    // Task 6's BuildVisibleSet only inserts entities that already have
    // WorldBounds (VisibilitySystem.hpp:124-128). RenderSubmissionSystem
    // drops anything that set does not contain (RenderSystems.hpp:62-63).
    Arcane::BoundsSystem{}(world.reg);

    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(ctx);

    Arcane::ResetExtractFrameCallCount();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    // Ortho view so Task 6's visible set still contains this quad.
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    CHECK(packet.origin.translation == glm::vec3(0.0f));
    CHECK(packet.viewport == req.viewport);
    CHECK(packet.batchVertices.size() == 4u);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
    const glm::vec3 copied = packet.batchVertices[0].pos;

    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 50.0f, 50.0f, 0.0f };
    CHECK(packet.batchVertices[0].pos == copied);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
}
```

The registered-id case uses the Severance bytes-only registration (`SeveranceTest.cpp:215-237`): non-empty `vsBytes` / `psBytes`, a template, and an instance. `RegisterMaterial` refuses empty bytes (`Batcher2D.cpp:469-475`). This case does not compile a shader and does not draw. The `SpriteMaterialTable` map outlives the `ExtractFrame` call. `editorView` is the same ortho view, so Task 6 does not cull the origin sprite.

```cpp
TEST_CASE("extract keeps a registered sprite material id", "[nri][extract]")
{
    World world;
    auto batcher = Arcane::Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    auto templ = std::make_shared<Arcane::MaterialTemplate>(Arcane::MaterialTemplate::Build(
        "extract-id", 1, {}));
    auto instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    Arcane::Material2DDesc desc;
    desc.templ = templ;
    desc.instance = instance;
    desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 5, 6, 7, 8 });
    const std::uint16_t id = batcher->RegisterMaterial(desc);
    REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
    REQUIRE(id >= 3u);
    REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
    REQUIRE(batcher->MaterialDesc(id) != nullptr);

    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(ctx);

    const Arcane::Guid matGuid = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, std::uint16_t> ids;
    ids.emplace(matGuid, id);
    world.reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::SpriteRenderer& rend = *world.reg.GetComponent<Arcane::SpriteRenderer>(sprite);
    rend.material = matGuid;
    rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.batchSpans.size() == 1u);
    CHECK(packet.batchSpans[0].material == id);
    CHECK(packet.batchSpans[0].material != Arcane::Batcher2D::kMaterialSprite);
    CHECK(packet.batchSpans[0].worldSpace);
    CHECK(packet.batchVertices.size() == 4u);
}
```

`MaterialTemplate::Build` takes `std::vector<ParamDecl>` (`MaterialTemplate.hpp:37-38`). An empty decl list is a valid layout (`CbSize() == 0`). Include `MaterialTemplate.hpp`, `MaterialInstance.hpp`, `Batcher2D.hpp`, `SceneResources.hpp`, `BoundsSystem.hpp`, `<memory>`, and `<unordered_map>` from `ExtractFrameTest.cpp`. `BuildVisibleSet` only inserts entities that already carry `WorldBounds` (`VisibilitySystem.hpp:124-128`), and once that set exists `RenderSubmissionSystem` drops every other sprite (`RenderSystems.hpp:62-63`). `BoundsSystem` is what writes the box (`BoundsSystem.hpp:150`). The three sprite cases call it after transform propagation so they still submit after Task 6.

The pixel witness is appended to `NriGraphPixelTest.cpp`. Add these includes: `ExtractFrame.hpp`, `MaterialSource.hpp`, `MaterialInstance.hpp`, `ShaderSourceProvider.hpp`, `ShaderCompiler.hpp`, `ShaderConventions.hpp`. `MakeVehicle`, `RenderOne`, `At`, `kW`, and `kH` stay the file-local helpers. The snippet is `float4 shade(Varyings v) { return float4(0.0, 1.0, 0.0, 1.0); }` stitched into `materials/sprite_material.hlsl` (`MaterialTemplateFile(MaterialSurface::Sprite)` returns that name, `MaterialSource.cpp:333-334`; the shade slot is the comment at `data/shaders/materials/sprite_material.hlsl:9`). `BuildMaterialShaderSource` returns `MaterialBuildResult` (`MaterialSource.hpp:117-123`); check `errors.empty()`. There is no `Ok()`. `ShaderCompiler::CompileNow` returns both `dxil` and `spirv` (`ShaderCompiler.hpp:88-89`); do not pass SPIR-V arguments on the request. Pick `spirv` when `backend == GraphicsBackend::Vulkan`, otherwise `dxil`. `Material2DDesc` stores one target (`Batcher2D.hpp:60-68`). A registration whose pipeline will not build falls back to `sprite.hlsl` `ps_main` (`data/shaders/sprite.hlsl:68-70`, `return g_Texture.Sample(g_Sampler, input.uv) * input.color`), which with a red tint is red-dominant after ACES. The constant green is green-dominant. `CHECK(px.g > px.r + 60)` is that difference. Those pixels would change if `QuadWorld` rewrote the id to `kMaterialSprite`.

The sprite sits at world `(80, 48)` with scale `{16, 16, 1}`. Unresolved rects are 1x1 m about the centre pivot (`RenderSystems.hpp:156-157`), so the quad covers about 16 px and the centre sample is inside it. `editorView` is `Ortho2DView(vec2(kW * 0.5, kH * 0.5), kH * 0.5, {kW, kH})`: 1 px/m, the same mapping `NriGraphPixelTest.cpp:431-435` states. Task 6's visible set keeps the sprite because that view contains it. Task 7's view-projection comes from `packet.spriteView`, which extract fills from `editorView`. `frame.batch` is the same batcher the material was registered on, because `Prepare` resolves `MaterialDesc` on `CurrentBatch()` and `RenderFrameOffscreen` sets that from `frame.batch` (`NriGraphContext.cpp:1909`). `BindPacket` is required or the node drains the live batcher again.

```cpp
namespace
{
    void CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get(
            Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Sprite));
        REQUIRE(templateText.has_value());
        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText,
            "float4 shade(Varyings v) { return float4(0.0, 1.0, 0.0, 1.0); }",
            "extract_green",
            Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest compileReq;
            compileReq.debugName = "extract_green.hlsl";
            compileReq.sourceUtf8 = build.hlsl;
            compileReq.entry = entry;
            compileReq.profile = profile;
            return compiler.CompileNow(compileReq);
        };
        const Arcane::ShaderCompileResult vs = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        const Arcane::ShaderCompileResult ps = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        const bool vulkan = backend == Arcane::GraphicsBackend::Vulkan;
        const Arcane::ShaderTargetResult& vsTarget = vulkan ? vs.spirv : vs.dxil;
        const Arcane::ShaderTargetResult& psTarget = vulkan ? ps.spirv : ps.dxil;
        REQUIRE(vsTarget.succeeded);
        REQUIRE(psTarget.succeeded);

        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);
        auto templ = std::make_shared<const Arcane::MaterialTemplate>(build.templ);
        auto instance = std::make_shared<const Arcane::MaterialInstance>(templ);
        Arcane::Material2DDesc desc;
        desc.templ = templ;
        desc.instance = instance;
        desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(vsTarget.bytecode);
        desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(psTarget.bytecode);
        const std::uint16_t id = batcher->RegisterMaterial(std::move(desc));
        REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
        REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
        batcher->Begin(kW, kH);

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        Arcane::RegisterSceneComponents(reg);
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });

        Arcane::RenderContext2D renderCtx;
        renderCtx.batcher = batcher.get();
        reg.SetResource<Arcane::RenderContext2D>(renderCtx);
        const Arcane::Guid matGuid = Arcane::Guid::Generate();
        std::unordered_map<Arcane::Guid, std::uint16_t> ids;
        ids.emplace(matGuid, id);
        reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

        Astra::Entity sprite = reg.CreateEntity();
        Arcane::Transform xform;
        xform.position = { 80.0f, 48.0f, 0.0f };
        xform.scale = { 16.0f, 16.0f, 1.0f };
        reg.AddComponent<Arcane::Transform>(sprite, xform);
        reg.SetParent(sprite, root);
        Arcane::SpriteRenderer rend;
        rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
        rend.material = matGuid;
        reg.AddComponent<Arcane::SpriteRenderer>(sprite, rend);
        Arcane::TransformPropagationSystem{}(reg);
        Arcane::BoundsSystem{}(reg);

        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { kW, kH };
        req.editorView = Arcane::Ortho2DView(
            glm::vec2(static_cast<float>(kW) * 0.5f, static_cast<float>(kH) * 0.5f),
            static_cast<float>(kH) * 0.5f,
            req.viewport);
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(reg, req, ring);
        REQUIRE(packet.batchSpans.size() == 1u);
        CHECK(packet.batchSpans[0].material == id);
        CHECK(packet.batchSpans[0].worldSpace);

        PixelVehicle v = MakeVehicle(backend);
        v.ctx->BindPacket(&packet);
        Arcane::NriGraphContext::FrameDesc frame;
        frame.batch = batcher.get();
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.g > px.r + 60);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("extract keeps a registered sprite material on screen (d3d12)",
          "[gpu][extract][shadercompile][nri][d3d12]")
{
    CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("extract keeps a registered sprite material on screen (vulkan)",
          "[gpu][extract][shadercompile][nri][vulkan]")
{
    CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Generate and run `[extract]`.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
```

Expected: FAIL. `ExtractFrame` does not exist yet.

- [ ] **Step 3: Implement the counter, the sprite copy, and the two host call sites.**

`ExtractAccounting.hpp` declares the three functions in `namespace Arcane`. `ExtractAccounting.cpp`:

```cpp
#include <Arcane/Base/ExtractAccounting.hpp>
#include <atomic>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        std::atomic<std::uint64_t> g_extractFrameCalls{ 0 };
    }

    void NoteExtractFrameCall() noexcept
    {
        g_extractFrameCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t ExtractFrameCallCount() noexcept
    {
        return g_extractFrameCalls.load(std::memory_order_relaxed);
    }

    void ResetExtractFrameCallCount() noexcept
    {
        g_extractFrameCalls.store(0, std::memory_order_relaxed);
    }
}
```

`ExtractFrame` begins with `NoteExtractFrameCall()`, then `RenderFramePacket& packet = ring.BeginExtract()`. It sets `packet.viewport = req.viewport` and `packet.origin.translation = { 0.0f, 0.0f, 0.0f }`. It does not write `packet.spriteView` or `packet.meshView` in this task.

Sprite copy, local to `ExtractFrame.cpp`. There is no `Batcher2D::Create`, no `Begin`, and no `SetGlobals`. `Begin` would clear the host's geometry and reset its view-projection (`Batcher2D.cpp:139-142`). `SetGlobals(GlobalParams{})` would wipe the host's `Time` / `DeltaTime`. The host already called `Begin`, `SetGlobals`, and `SetRenderContext` on its persistent batcher. Registration lives on that batcher (`SceneRenderResolver.cpp:581`, `SpriteMaterialCache.cpp:260`). `QuadWorld` on a fresh batcher rewrites any id `>= m_materials.size()` to `kMaterialSprite` (`Batcher2D.cpp:248-253`), and a `Create()` only has the three built-ins (`:88-94`). Extract does not replace `RenderContext2D::batcher`. When that pointer is null the sprite vectors stay empty and nothing is drained.

```cpp
RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
Batcher2D* batcher = ctx ? ctx->batcher : nullptr;
if (batcher)
{
    glm::mat4 vp{ 1.0f };
    if (req.editorView)
        vp = req.editorView->ViewProjection();
    else if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
        vp = cam->view.ViewProjection();
    batcher->SetViewProjection(vp);   // does not clear geometry
    RenderSubmissionSystem{}(reg);
    const Batch2DDrained drained = batcher->Drain();
    packet.batchVertices.assign(drained.vertices.begin(), drained.vertices.end());
    packet.batchIndices.assign(drained.indices.begin(), drained.indices.end());
    packet.batchSpans.assign(drained.spans.begin(), drained.spans.end());
    if (drained.globals)
        packet.globals = *drained.globals;
    packet.viewProjection = drained.viewProjection;
}
return packet;
```

`ClientRuntime::InstallRenderSystems` (`ClientRuntime.cpp:45-49`) stops calling `render.AddSystem<RenderSubmissionSystem>()`. The function stays, so `OnSystemsCleared` still compiles. The type stays, so a test that constructs `RenderSubmissionSystem` still compiles. In `ClientRuntimeTest.cpp`, rename the second case to `"ClientRuntime owns a Runtime, attaches as its client, and leaves render submission uninstalled"`, and change both `CHECK(...HasSystem<RenderSubmissionSystem>())` assertions on `ClientRuntime` (lines 44 and 46) to `CHECK_FALSE`. Leave the bare-`Runtime` `CHECK_FALSE` assertions as they are.

`FrameIo` gains `Arcane::PacketRing packetRing{};` as its last member. `NriGraphContext` stores `const RenderFramePacket* m_packet = nullptr;`, `BindPacket` assigns it, and `Packet()` returns it. Both `RenderFrame` and `RenderFrameOffscreen` set it back to null before they return.

`RuntimeFrame.cpp` keeps `Batch().Begin` (`:521`), `SetGlobals` (`:522`), `PushSceneCamera` (`:523`), and `SetViewProjection` (`:529`). Replace the block at `:546-550` with `SetRenderContext` only. `SubmitRender` goes away here so the sprite system does not run twice; `InstallRenderSystems` also stops adding it, above. The perf bracket that only timed `SubmitRender` goes with it.

```cpp
io.runtime->SetRenderContext(&io.gpu->Batch());
```

That call is still before the one `ExtractFrame`, which sits immediately above the branch at `:748`, after `graphFrame.imgui` (`:508`) and `graphFrame.post` (`:692`) have been assigned:

```cpp
Arcane::ExtractRequest req;
req.viewport = { frameWidth, frameHeight };
req.device = io.graph->Scene();
const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(io.runtime->Registry(), req, io.packetRing);
io.graph->BindPacket(&packet);
```

`graphFrame.batch = &io.gpu->Batch()` (`:685`) stays, so `Prepare` resolves materials on the same batcher extract drained. Leave `ActivePerspectiveSceneCamera` and `PrepareSceneForRender` in this task. `PushSceneCamera` still calls `ActiveSceneCamera`. There is exactly one `ExtractFrame(` in the file, textually above both `RenderFrame(` and `RenderFrameOffscreen(`. `GpuSceneInvalidate` at `:756` stays. The `Batch().Stats()` reads at `:321` and `:791` use the live batcher, which no longer receives the sprite system. Point them at the packet: draw count is `packet.batchSpans.size()`, quad count is `packet.batchVertices.size() / 4`.

`AddBatch2DNode` (`Batch2DNode.cpp:1628-1641`):

```cpp
Batch2DDrained drained;
if (context && context->Packet())
{
    const RenderFramePacket* packet = context->Packet();
    drained.vertices = packet->batchVertices;
    drained.indices = packet->batchIndices;
    drained.spans = packet->batchSpans;
    drained.globals = &packet->globals;
    drained.viewProjection = packet->viewProjection;
    drained.viewport = glm::vec2(float(width), float(height));
    if (Batcher2D* batcher = context->CurrentBatch())
        if (Batch2DNode* node = context->Batch2D())
            node->Prepare(*batcher, drained, canvasFormat);
}
else if (context)
{
    if (Batcher2D* batcher = context->CurrentBatch())
    {
        drained = batcher->Drain();
        if (Batch2DNode* node = context->Batch2D())
            node->Prepare(*batcher, drained, canvasFormat);
    }
}
```

The spans view the packet vectors. Those vectors live until `FramesInFlight()` later extracts recycle the slot. The packet path does not `Drain` again. `Prepare` still takes `CurrentBatch()`, which the hosts set to the same batcher extract submitted into (`RuntimeFrame.cpp:685`, `EditorAppFrame.cpp:1704`, `NriGraphContext.cpp:1909`). A material id in the span is only drawable when that batcher's `MaterialDesc` still has the registration. A graph with no bound packet, including thumbnail harvest and shader preview, keeps the `Drain()` path. Those two do not gain an `ExtractFrame` call.

`EditorApp` holds `Arcane::PacketRing m_packetRing;`. Inside `RenderSceneToViewport`, when `m_viewportTargets.graph` is set, keep `Begin` (`:1693`) and `SubmitSceneToBatcher`. Delete only `m_runtime->Loop().SubmitRender()` at `:1891`. `SetRenderContext(&b)` at `:1890` stays, and `SetGlobals` / `SetViewProjection` inside `SubmitSceneToBatcher` stay. Call `ExtractFrame` once after `SubmitSceneToBatcher` returns, not after the view block at `:1684-1688` (that block is before `Begin`, so a drain there would copy the previous bracket). If `!InPlayMode()`, `req.editorView = m_runtime->View()`; `req.viewport` is `{ w, h }`; `req.device` is `m_viewportTargets.graph->Scene()`. `m_viewportTargets.graph->BindPacket(&packet)` for the viewport frame. `vp.batch = &b` (`:1704`) stays, the same batcher. `BindPacket(nullptr)` runs when that frame returns. Task 3 already moved grid, physics, the camera rect, and the gizmo off this batcher, so the drain is sprites. This task does not remove `PrepareSceneForRender` or `ActivePerspectiveSceneCamera` from the editor. Task 9's later "stops calling `SubmitRender`" finds the call already gone.

- [ ] **Step 4: Run `[extract]` and the client-runtime case.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
.\ArcaneTests.exe "ClientRuntime owns a Runtime"
```

Expected: PASS. The sprite case reports 4 vertices and a call count of 1. The registered-id case reports one world-space span whose `material` is the registered id. The two `[gpu][extract]` cases read a green-dominant pixel at `(80, 48)` and leave `RenderErrorCount` unchanged. The client-runtime case reports the system absent on `ClientRuntime`. Record the seed. The same `[extract]` filter in Tasks 6 and 7 re-runs the registered-id case and the GPU witness: `editorView` keeps the sprite in the visible set, and Task 7's view-projection is `packet.spriteView`, filled from that `editorView`.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp ArcaneCore/src/Arcane/Base/ExtractAccounting.cpp ArcaneClient/src/Arcane/Render/ExtractFrame.hpp ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp ArcaneClient/src/Arcane/Client/ClientRuntime.cpp ArcaneRuntime/src/RuntimeFrame.hpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/ExtractFrameTest.cpp ArcaneTests/src/ClientRuntimeTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): extract copies the sprite batch into the packet" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 6: GPU scene and the visible set

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp:285-388`
- Modify: `ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp:110`
- Modify: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp` (the sprite copy from Task 5 stays; this task adds the visible set and the mesh frame in front of it)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp` (delete the `PrepareSceneForRender` call beside `:543`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (delete `PrepareSceneForRender` at `:1689`)
- Modify every remaining `BuildGpuSceneFrame` call: `ArcaneTests/src/GpuSceneSyncTest.cpp:499,536,561,617,667,668`, `GpuSceneCullTest.cpp:417,524,570,606`, `MeshCullNodeTest.cpp:67,110,116`, `NriGraphPixelTest.cpp:2221,2301,2358,2387`, and both calls inside `ArcaneTests/src/OverlayWitnessTest.cpp` (`CheckOverlayClasses` and `CheckPerspectiveGridDepth`, which Task 3 wrote against `const MeshTable*`)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp` (the `World` fixture Task 5 added)

**Interfaces:**
- Consumes: `BuildGpuSceneFrame` (`GpuSceneSync.hpp:285`), `GpuSceneSync` (`:92`), `GpuSceneSyncedGeneration` (`GpuScene.hpp:351`, null returns 0), `BuildVisibleSet` (`VisibilitySystem.hpp:119`), `MeshTable::meshes` (`SceneResources.hpp:259`), `MeshEntry::data.sections` (`MeshBuilder.hpp:76`), `MeshSection::indexOffset` / `indexCount` (`:63-64`). `PrepareSceneForRender` (`GpuSceneHost.hpp:87-111`) is what the hosts stop calling. `CheckPerspectiveGridDepth`'s helpers (`FillCamera`, `GpuSceneWorld`, `MakeVehicle`, and the pixel readers) are the copies Task 3 pasted into `OverlayWitnessTest.cpp`. This task does not move them.
- Produces:

```cpp
struct MeshSectionRange
{
    Guid mesh{};
    std::uint32_t section = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
};

std::vector<MeshSectionRange> MeshSectionRanges(const MeshTable* table);

void BuildGpuSceneFrame(const GpuSceneMirror& m, const VisibleSet* vis,
                        std::span<const MeshSectionRange> sections,
                        const ViewTransform& view, GpuSceneFrame& out);
```

The `const MeshTable*` overload is deleted.

- [ ] **Step 1: Write the failing test.**

Append to `ExtractFrameTest.cpp`:

```cpp
TEST_CASE("extract publishes one mesh row and ignores a later transform edit", "[nri][extract][gpuscene]")
{
    World world;
    const Arcane::Guid mesh = world.Mesh(1);
    Astra::Entity spawned = world.Spawn({ 0.0f, 0.0f, 0.0f }, mesh);
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.mesh.stats.total >= 1u);
    const std::uint32_t total = packet.mesh.stats.total;
    world.reg.GetComponent<Arcane::Transform>(spawned)->position = { 40.0f, 0.0f, 0.0f };
    CHECK(packet.mesh.stats.total == total);
    CHECK(packet.visible.entries.size() >= 1u);
}
```

- [ ] **Step 2: Run `[extract]`.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract publishes one mesh row"
```

Expected: FAIL. `packet.mesh.stats.total` is 0. The sprite case from Task 5 still passes.

- [ ] **Step 3: Publish the visible set and the section span.**

`MeshSectionRanges`:

```cpp
std::vector<MeshSectionRange> MeshSectionRanges(const MeshTable* table)
{
    std::vector<MeshSectionRange> out;
    if (!table || !table->meshes)
        return out;
    for (const auto& [guid, entry] : *table->meshes)
    {
        std::uint32_t section = 0;
        for (const MeshSection& s : entry.data.sections)
        {
            out.push_back(MeshSectionRange{ guid, section, s.indexOffset, s.indexCount });
            ++section;
        }
    }
    return out;
}
```

Inside `BuildGpuSceneFrame`, both lookup sites (`GpuSceneSync.hpp:321-323` and `:360-363`) become:

```cpp
const MeshSectionRange* section = nullptr;
for (const MeshSectionRange& candidate : sections)
    if (candidate.mesh == key.mesh && candidate.section == key.section)
        section = &candidate;
if (!section)
    continue;
```

`section->indexOffset` and `section->indexCount` replace `section.indexOffset` and `section.indexCount` from the old `MeshSection`. The opaque/masked emit sort (`:349-357`) and the transparent `renderOrder`-then-depth sort (`:380-388`) stay.

Every old call `BuildGpuSceneFrame(mirror, vis, tablePtr, view, out)` becomes `BuildGpuSceneFrame(mirror, vis, MeshSectionRanges(tablePtr), view, out)`, including a null `tablePtr` (`MeshSectionRanges(nullptr)` returns an empty vector) and both calls in `OverlayWitnessTest.cpp`. `PrepareSceneForRender`'s call at `GpuSceneHost.hpp:110` uses the same wrapper. Hosts stop calling `PrepareSceneForRender`. The function stays for the tests that already call it.

`ExtractFrame`, before the sprite `RenderSubmissionSystem` call, so `MainVisibleSet` sees this frame's set:

```cpp
ViewTransform cullView{};
if (req.editorView)
    cullView = *req.editorView;
else if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
    cullView = cam->view;
cullView.viewport = req.viewport;

SceneVisibility* sv = reg.GetResource<SceneVisibility>();
if (!sv)
    sv = reg.EmplaceResource<SceneVisibility>();
sv->views.resize(1);
BuildVisibleSet(reg, cullView, sv->views[0]);
packet.visible = sv->views[0];

GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
if (!mirror)
    mirror = reg.EmplaceResource<GpuSceneMirror>();
GpuSceneStage stage;
GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(req.device), stage);
const std::vector<MeshSectionRange> sections = MeshSectionRanges(reg.GetResource<MeshTable>());
BuildGpuSceneFrame(*mirror, &packet.visible, sections, cullView, packet.mesh);
packet.mesh.stage = stage;
```

`GpuSceneSync` advances the mirror tick. The host call is gone, so the sync runs once. `RuntimeFrame.cpp` deletes the `PrepareSceneForRender` call at `:543`. The mesh-scene block at `:669-683` moves to immediately after `ExtractFrame` and before the `:748` branch. `MeshSceneDesc::scene` is `const GpuSceneFrame*` (`MeshNode.hpp:331`), so that block sets `meshScene.scene = &packet.mesh`. `EditorAppFrame.cpp:1689` deletes its `PrepareSceneForRender` call, and `:2078` becomes `m_meshScene.scene = &packet.mesh`. `GpuSceneInvalidate` at `RuntimeFrame.cpp:756` and `EditorAppFrame.cpp:1764` stays.

- [ ] **Step 4: Run the mesh tests.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
.\ArcaneTests.exe "[gpuscene]"
.\ArcaneTests.exe "[overlay]"
```

Expected: PASS. `[overlay]` covers `CheckOverlayClasses` and `CheckPerspectiveGridDepth`, both now calling the span overload. `[gpuscene]` covers the updated call sites in `GpuSceneSyncTest.cpp` and `GpuSceneCullTest.cpp`.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/ExtractFrameTest.cpp ArcaneTests/src/GpuSceneSyncTest.cpp ArcaneTests/src/GpuSceneCullTest.cpp ArcaneTests/src/MeshCullNodeTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp ArcaneTests/src/OverlayWitnessTest.cpp
git commit -m "feat(render): extract publishes the visible set and the GPU scene" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 7: Cameras and views

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp` (fill `spriteView` and `meshView`; the cull view and the batch view-projection read them)
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:846-874` (`PushSceneCamera`)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:521-548` (the camera sweeps leave; the one `ExtractFrame` from Task 5 stays above `:748`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` `RenderSceneToViewport` (`:1688`, stop calling `ActivePerspectiveSceneCamera` for the world view)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `ActiveSceneCamera` (`SceneCamera.hpp:57`) returns `std::optional<SceneCameraView>`. `ActivePerspectiveSceneCamera` (`:206`) returns `std::optional<ViewTransform>`. `using PerspectiveCameraView = ViewTransform` (`:146`). `SceneCameraView` fields are `worldCenter`, `halfHeight`, `view` (`SceneCameraView.hpp`). `ViewTransform` has `view`, `projection`, `viewport` and no `operator==` (`ViewTransform.hpp:50`). `BuildVisibleSet` takes a `ViewTransform`.
- Produces: `packet.spriteView` as `std::optional<SceneCameraView>`, `packet.meshView` as `std::optional<ViewTransform>`.

- [ ] **Step 1: Write the failing test.**

`ViewTransform` comparisons are field-wise. Append:

```cpp
void AddOrthoCamera()
{
    Arcane::Camera cam;
    cam.active = true;
    cam.projection = Arcane::CameraProjection::Orthographic;
    cam.orthographicSize = 2.0f;
    reg.AddComponent<Arcane::Camera>(root, cam);
}
void AddPerspectiveCamera()
{
    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform t;
    t.position = { 0.0f, 0.0f, 5.0f };
    reg.AddComponent<Arcane::Transform>(e, t);
    reg.SetParent(e, root);
    Arcane::Camera cam;
    cam.active = true;
    cam.projection = Arcane::CameraProjection::Perspective;
    cam.fovYDegrees = 50.0f;
    cam.nearZ = 0.05f;
    cam.farZ = 500.0f;
    reg.AddComponent<Arcane::Camera>(e, cam);
}

static void CheckView(const Arcane::ViewTransform& a, const Arcane::ViewTransform& b)
{
    CHECK(a.view == b.view);
    CHECK(a.projection == b.projection);
    CHECK(a.viewport == b.viewport);
}

TEST_CASE("runtime extract keeps the ortho sprite view and the perspective mesh view", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 200u, 100u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    const std::optional<Arcane::SceneCameraView> ortho = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    const std::optional<Arcane::ViewTransform> persp = Arcane::ActivePerspectiveSceneCamera(world.reg, req.viewport);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    REQUIRE(ortho.has_value());
    REQUIRE(persp.has_value());
    CheckView(packet.spriteView->view, ortho->view);
    CHECK(packet.spriteView->worldCenter == ortho->worldCenter);
    CHECK(packet.spriteView->halfHeight == ortho->halfHeight);
    CheckView(*packet.meshView, *persp);
}

TEST_CASE("an editorView is the world view and is not replaced by a scene camera", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    world.reg.GetComponent<Arcane::Transform>(world.root)->position = { 4.0f, 0.0f, 0.0f };
    const std::optional<Arcane::SceneCameraView> scene = Arcane::ActiveSceneCamera(world.reg, { 64u, 64u });
    REQUIRE(scene.has_value());
    Arcane::ViewTransform editor = scene->view;
    editor.view[3][0] += 3.0f;
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    req.editorView = editor;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.spriteView->view.view == editor.view);
    CHECK(packet.spriteView->view.projection == editor.projection);
    CHECK(packet.meshView->view == editor.view);
    CHECK(packet.meshView->projection == editor.projection);
    CHECK(packet.spriteView->worldCenter == glm::vec2(0.0f));
    CHECK(scene->worldCenter != packet.spriteView->worldCenter);
}
```

`AddOrthoCamera` and `AddPerspectiveCamera` are methods of `World`. `CameraProjection::Orthographic` is 0 and `Perspective` is 1 (`Components.hpp:256-260`).

- [ ] **Step 2: Run the two cases.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "runtime extract keeps the ortho sprite view"
.\ArcaneTests.exe "an editorView is the world view"
```

Expected: FAIL. `spriteView` is empty.

- [ ] **Step 3: Fill the two views inside extract, and stop the host sweeps.**

At the top of the cull block from Task 6:

```cpp
if (req.editorView)
{
    packet.spriteView = SceneCameraView{};
    packet.spriteView->view = *req.editorView;
    packet.meshView = *req.editorView;
}
else
{
    packet.spriteView = ActiveSceneCamera(reg, req.viewport);
    packet.meshView = ActivePerspectiveSceneCamera(reg, req.viewport);
    if (!packet.spriteView && !warnedNoSceneCamera)
    {
        warnedNoSceneCamera = true;
        int total = 0;
        reg.CreateView<const Camera>().ForEach([&](Astra::Entity, const Camera&) { ++total; });
        if (total == 0)
            ARC_WARN("scene has no Camera component at all -- nothing sets the view. "
                     "Add a Camera component to an entity (a New Scene ships one).");
        else
            ARC_WARN("scene carries {} Camera component(s) but none is usable "
                     "as the sprite camera.", total);
    }
}
```

`warnedNoSceneCamera` is a function-local `static bool`, the same once-per-process warning `PushSceneCamera` prints today (`RuntimeApp.cpp:862-878`). The `CreateView` lives in `ExtractFrame.cpp`.

The cull view becomes `packet.spriteView ? packet.spriteView->view : ViewTransform{}`, with `viewport` set to `req.viewport`. `BuildVisibleSet` receives that `ViewTransform`, not the `SceneCameraView`. The host batcher's `SetViewProjection` becomes `packet.spriteView ? packet.spriteView->view.ViewProjection() : glm::mat4{ 1.0f }`. `BuildGpuSceneFrame` receives `packet.meshView` when it has a value, and skips the batch build when it does not (the stage still lands, matching `PrepareSceneForRender`'s empty-meshView arm at `GpuSceneHost.hpp:103-108`).

`PushSceneCamera` (`RuntimeApp.cpp:846`) takes the packet and, when `spriteView` has a value, calls `SetView(packet.spriteView->view)`. It does not call `ActiveSceneCamera`. Move its call from `:523` to immediately after the `ExtractFrame` call that Task 5 placed above `:748`, and pass that packet. `RuntimeFrame.cpp:541-544` deletes the `ActivePerspectiveSceneCamera` local. The one `ExtractFrame(` stays above both render paths. The editor's `RenderSceneToViewport` keeps passing `req.editorView` on the Edit path and leaves it empty when `InPlayMode()`. Delete the `ActivePerspectiveSceneCamera` call at `:1688`. After `ExtractFrame`, `m_meshView = packet.meshView`.

- [ ] **Step 4: Run the camera cases and the sprite case.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
```

Expected: PASS. The editor case keeps `worldCenter` at 0 while the scene camera's `worldCenter` differs. The runtime case matches both live cameras field by field.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneRuntime/src/RuntimeApp.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/ExtractFrameTest.cpp
git commit -m "feat(render): extract publishes the sprite and mesh views" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 8: Picking

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/PickEmit.cpp` (`CollectPickables` at `:32`; `BuildPickIdGeometry` stays at `:243`; `PickBoundHalfExtents` at `:226` stays because `:294` calls it)
- Modify: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp` (the moved function)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:570-587` and `:615-646` (both blocks move to after `ExtractFrame`; both `CollectPickables` calls leave; `io.pickDrawables` is copied from the packet; `--pick-probe` writes `selectedIds`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp:2165-2190` (`ArmGraphViewportFrame`'s `CollectPickables` leaves; the selection loop at `:2173-2183` writes `packet.selectedIds` and stops assigning `vp.selectedIds`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:1371-1372`
- Modify: `ArcaneTests/src/NriGraphPixelTest.cpp` (append the polarity case after `CheckSelectionOutlineTracesOnlyTheSelection`)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `CollectPickables(Astra::Registry&, std::vector<PickDrawable>&)` (`PickEmit.cpp:32`). `PickEmit.hpp:23` includes `Astra/Entity/Entity.hpp` and `:35` forward-declares `Astra::Registry`. `PickPassId` (`:188`) uses `Astra::Entity` from that header. `PickSampleTexel` (`:198`) uses `std::floor` and `std::clamp`. `PickKindCode` (`:213`), `PickBoundHalfExtents` (`:226`), and `BuildPickIdGeometry` (`:243`) stay in this file; `BuildPickIdGeometry` reads `PickDrawable` and `glm` only. Today's includes, every one of which leaves: `SpriteGeometry.hpp` (`:4`), `VisibilitySystem.hpp` (`:5`), `Components.hpp` (`:6`), `PhysicsComponents.hpp` (`:7`, which includes `Ecs.hpp` at `PhysicsComponents.hpp:41`), `PhysicsSystem.hpp` (`:8`), `SceneResources.hpp` (`:9`, which includes `Ecs.hpp:7`, which includes `Registry.hpp` at `Ecs.hpp:23`), `Registry.hpp` (`:11`), and `<utility>` (`:17`). `PrepareSelection` takes `std::span<const std::uint32_t>` (`PickOutlineNodes.hpp:465`). `CurrentSelectedIds` is `NriGraphContext.hpp:1056`. `RenderFrame` and `RenderFrameOffscreen` null the bound packet before they return (Task 5).
- Produces: `packet.pickDrawables`. The selection loop and the `--pick-probe` stand-in write `packet.selectedIds` through `PacketRing::Latest()`. The outline reads that field when a packet is bound. After this task `PickEmit.cpp` includes only `PickEmit.hpp`, `Constant.hpp`, `glm/glm.hpp`, `<algorithm>`, and `<cmath>`.

- [ ] **Step 1: Write the failing test.**

```cpp
TEST_CASE("extract publishes a pick drawable that survives a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 1.0f, 2.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 4.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.pickDrawables.size() == 1u);
    CHECK(packet.pickDrawables[0].entity == sprite);
    const glm::vec3 corner = packet.pickDrawables[0].corners[0];
    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 30.0f, 30.0f, 0.0f };
    CHECK(packet.pickDrawables[0].corners[0] == corner);
}
```

- [ ] **Step 2: Run the case.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract publishes a pick drawable"
```

Expected: FAIL. `pickDrawables` is empty.

- [ ] **Step 3: Move the collector, drop every registry include, and publish `selectedIds`.**

Move `RotateVec` (`PickEmit.cpp:24-29`) and `CollectPickables` (`:32` through the line before `PickBoundHalfExtents`) into `ExtractFrame.cpp`'s anonymous namespace, and call it so `packet.pickDrawables` receives the appended drawables. `ExtractFrame.cpp` includes `PickEmit.hpp`, `SpriteGeometry.hpp`, `Components.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp`, `SceneResources.hpp`, and `VisibilitySystem.hpp`. Those headers stay legal in this one TU.

`PickEmit.cpp` keeps `PickPassId`, `PickSampleTexel`, `PickKindCode`, `PickBoundHalfExtents`, and `BuildPickIdGeometry`. Delete each of these includes: `SpriteGeometry.hpp`, `VisibilitySystem.hpp`, `Components.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp`, `SceneResources.hpp`, `Registry.hpp`, and `<utility>`. The include list becomes:

```cpp
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Core/Constant.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
```

`packet.pickDrawables` is the list the pick node already consumes. The frame still passes `std::span<const PickDrawable>(packet.pickDrawables)` as `FrameDesc::pickables`. `FrameDesc::selectedIds` is not that list.

The Task 5 `ExtractFrame` call in `RenderSceneToViewport` stays after `SubmitSceneToBatcher` (`EditorAppFrame.cpp:1694`) and before `ArmGraphViewportFrame` (`:1717`), so the loop below reads this frame's drawables. `ArmGraphViewportFrame` deletes the clear and the `CollectPickables` call at `:2164-2165`. The selection loop at `:2173-2183` stays, still calls `PickPassIdOf` (the host-gate strip of that name stays), and writes the ids it already pushes:

`PickPassIdOf` takes `std::span<const PickDrawable>` (`PickEmit.hpp:150`). The loop reads the packet vector. `Latest()` is non-const; `ExtractFrame` has already returned. The click table at `:2207-2209` still walks `m_pickDrawables`, and `TakeRequest` inverts the id through that table, so the member is a copy of the packet vector.

```cpp
Arcane::RenderFramePacket& packet = m_packetRing.Latest();
m_pickDrawables = packet.pickDrawables;
m_pickSelectedIds.clear();
if (wantOutline)
{
    m_pickSelectedIds.reserve(m_selection.Count());
    for (Astra::Entity e : m_selection.Entities())
    {
        const std::uint32_t id = Arcane::PickPassIdOf(packet.pickDrawables, e);
        if (id != 0u)
            m_pickSelectedIds.push_back(id);
    }
}
packet.selectedIds = m_pickSelectedIds;
vp.pickables = packet.pickDrawables;
```

Replace `vp.pickables = m_pickDrawables` (`:2186`) with the snippet's `vp.pickables = packet.pickDrawables`. Delete `vp.selectedIds = m_pickSelectedIds` (`:2190`). `m_pickSelectedIds` may still be filled; it is not what the outline reads. `vp.pickOutline` and `vp.pickView` stay (`:2185`, `:2189`). The early return at `:2149-2156` still clears `m_pickDrawables` and `m_pickSelectedIds` and returns before the snippet, so `packet.selectedIds` stays empty because `BeginExtract` already cleared it. `wantOutline` false and `wantPick` true skips the loop, so `selectedIds` stays empty, which is what that frame does today. The `:2207-2209` walk is not rewritten: it reads the copy.

`AddOutlineNodes` (`PickOutlineNodes.cpp:1371-1372`) prefers the packet:

```cpp
if (OutlineNode* node = context ? context->Outline() : nullptr)
{
    if (context->Packet())
        node->PrepareSelection(context->Packet()->selectedIds);
    else
        node->PrepareSelection(context->CurrentSelectedIds());
}
```

When a packet is bound, `FrameDesc::selectedIds` is ignored. A frame that does not `BindPacket` keeps today's `CurrentSelectedIds()` path, which is what `CheckSelectionOutlineTracesOnlyTheSelection` (`NriGraphPixelTest.cpp:732`) and any `--pick-probe` run with no packet still use.

Runtime: delete both `CollectPickables` calls (`RuntimeFrame.cpp:573` and `:621`). The `--pick-probe` block (`:570-587`, including its `#if !defined(ARC_BUILD_DIST)` guard) and the `pick@` block (`:615-646`) sit before `graphFrame.post` (`:692`). Task 5's one `ExtractFrame` sits after that assignment and above the branch at `:748`, so both blocks run before this frame's packet exists. Move each block, comment text unchanged, to after that `ExtractFrame` call and before the `RenderFrame` / `RenderFrameOffscreen` branch. Task 6 may already have placed the mesh-scene block in that gap; the pick blocks stay before the branch either way. `packet` is then in scope and `Latest()` is this frame's slot. In place of each `CollectPickables` call, assign `io.pickDrawables = packet.pickDrawables`. `FrameIo::pickDrawables` is `RuntimeApp::m_pickDrawables` (`RuntimeApp.cpp:749`), and shutdown inverts the probe through `PickEntityForId(m_pickDrawables, *id)` at `:931` and `:1048`. An empty member reports every hit as unmapped. Replace `graphFrame.pickables = io.pickDrawables` at `:584` and `:624` with `packet.pickDrawables`. `graphFrame.pickView` (`:585`, `:625`) and `graphFrame.pickOutline` stay. Delete `graphFrame.selectedIds = io.pickSelectedIds` (`:586`). `--pick-probe`, after the move and only when `packet.pickDrawables` is non-empty, writes `io.packetRing.Latest().selectedIds = { 1u }`. It may still fill `io.pickSelectedIds` (`:579-581`) so that member is not stale; the outline does not read it. The `pick@` block leaves `selectedIds` empty and keeps `hoverPixel = (-1, -1)`. The comment at `:627-644` stays true and is not edited, and neither are the `CollectPickables` comments at `:554` and `:577`.

Append to `NriGraphPixelTest.cpp` after `CheckSelectionOutlineTracesOnlyTheSelection`. `MakeVehicle`, `RenderOne`, `At`, `WorldQuadAtPixel` (`:533`), `PixelIdentityView` (`:528`), and `DifferingTexels` (`:712`) are already in this TU's anonymous namespaces. `BindPacket` is cleared before `RenderFrameOffscreen` returns, so each capture binds immediately before its `RenderOne`. `NodeSet::pickOutline = true`. Both backends, tagged `[gpu][pixel][pick]`.

```cpp
std::vector<unsigned char> CaptureBoundSelection(
    PixelVehicle& v, const Arcane::RenderFramePacket* packet,
    std::span<const Arcane::PickDrawable> drawables,
    std::span<const std::uint32_t> frameSelected,
    std::uint32_t& w, std::uint32_t& h)
{
    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture     = true;
    frame.pickOutline = true;
    frame.pickables   = drawables;
    frame.pickView    = PixelIdentityView();
    frame.selectedIds = frameSelected;
    frame.hoverPixel  = glm::ivec2(-1, -1);
    if (packet)
        v.ctx->BindPacket(packet);
    RenderOne(*v.ctx, frame);
    std::vector<unsigned char> rgba;
    REQUIRE(v.ctx->ReadCapture(w, h, rgba));
    return rgba;
}

void CheckPacketSelectionDrawsTheOutline(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();
    Arcane::NriGraphContext::NodeSet nodes;
    nodes.pickOutline = true;

    const Arcane::PickDrawable quad = WorldQuadAtPixel({ 60.0f, 40.0f }, { 20.0f, 14.0f });
    Arcane::RenderFramePacket fromPacket;
    fromPacket.pickDrawables.push_back(quad);
    fromPacket.selectedIds = { 1u };
    Arcane::RenderFramePacket fromFrameIgnored;
    fromFrameIgnored.pickDrawables.push_back(quad);

    std::uint32_t wP = 0, hP = 0, wF = 0, hF = 0, wB = 0, hB = 0;
    std::vector<unsigned char> packetPx, framePx, baseline;
    {
        PixelVehicle v = MakeVehicle(backend, nodes);
        packetPx = CaptureBoundSelection(v, &fromPacket, fromPacket.pickDrawables, {}, wP, hP);
    }
    {
        PixelVehicle v = MakeVehicle(backend, nodes);
        const std::uint32_t selected[] = { 1u };
        framePx = CaptureBoundSelection(v, &fromFrameIgnored, fromFrameIgnored.pickDrawables, selected, wF, hF);
    }
    {
        PixelVehicle v = MakeVehicle(backend, nodes);
        const Arcane::PickDrawable drawables[] = { quad };
        baseline = CaptureBoundSelection(v, nullptr, drawables, {}, wB, hB);
    }
    REQUIRE(wP == wB && hP == hB && wF == wB && hF == hB);
    CHECK(DifferingTexels(packetPx, baseline, wB, hB) > 40u);
    CHECK(DifferingTexels(framePx, baseline, wB, hB) == 0u);
    CHECK(Arcane::RenderErrorCount() == before);
}
```

`fromPacket` binds a packet whose `selectedIds` is `{1}` and whose `FrameDesc::selectedIds` is empty. `fromFrameIgnored` binds the same drawable with an empty `packet.selectedIds` and `FrameDesc::selectedIds` of `{1}`. `baseline` does not bind a packet and selects nothing. The first check fails if the outline still reads only `FrameDesc`. The second fails if a bound packet still honours `FrameDesc::selectedIds`. The floor of 40 is the same floor `CheckSelectionOutlineTracesOnlyTheSelection` uses. Add the two `TEST_CASE`s beside that function's pair, named `"packet selection draws the outline (d3d12)"` and `"packet selection draws the outline (vulkan)"`, tagged `[gpu][pixel][pick][nri]` plus the backend tag.

- [ ] **Step 4: Run the pick case.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract publishes a pick drawable"
.\ArcaneTests.exe "[pick]"
.\ArcaneTests.exe "packet selection draws the outline"
```

Expected: PASS. The polarity case fails if the outline still reads only `FrameDesc::selectedIds`.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/PickEmit.cpp ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp ArcaneTests/src/ExtractFrameTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): extract publishes pick drawables" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 9: Overlays

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/Grid2D.hpp`
- Create: `ArcaneClient/src/Arcane/Render/Grid2D.cpp`
- Modify: `ArcaneEditor/src/Viewport/ViewportGrid.hpp` and `ViewportGrid.cpp` (`PlanGrid2D` at `:78`, `DrawGrid2D` at `:132`)
- Modify: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` (`DeclareGraphFrame`, the `wantOverlay` block Task 3 added after `AddTonemapNode`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.cpp` (`Record` reads `Packet()->overlay` when a packet is bound)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`SubmitSceneToBatcher` `:1857-1991`, the `vp.grid` block at `:2108-2119`, the gizmo draw read at `:3720`, and the Task 3 `FrameDesc::overlay` pointer)
- Modify: `ArcaneTests/src/NriGraphPixelTest.cpp` (append the extract-to-present case)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp`
- `ArcaneTests/src/ViewportGridTest.cpp` stays green without an edit

**Interfaces:**
- Consumes: `DrawGrid2D` (`ViewportGrid.cpp:132` through its closing brace), `PlanGrid2D` (`:78`), `PixelsPerMetre` (`:70`). `DrawPhysicsDebug` (`PhysicsDebugDraw.hpp:164`) calls only `Line`, `Circle`, and `Rect` (`:142-145`). `Batcher2D`'s pure virtuals, in declaration order: `Begin` (`:237`), `SetLayer` (`:250`), `Quad` (`:259`), `Glyph` (`:285`), `Rect` (`:291`), `Line` (`:293`), `Circle` (`:295`), `Triangle` (`:301`), `End` (`:321`), `Stats` (`:325`), `QuadWorld` (`:405`), `CircleWorld` (`:412`), `SetViewProjection` (`:421`). `RegisterMaterial`, `UpdateMaterial`, `SetGlobals`, `QuadMaterial`, `Drain`, `MaterialDesc`, and `QuadTextured` have defaults. `kCameraFrameColor` is `{ 0.45, 0.62, 0.78, 0.75 }` (`EditorThemeSettings.hpp:181`). `EditorGridSettings` fields read at `ViewportGrid.cpp:142-145` are `minorAlpha`, `majorAlpha`, `lineColor`, `lineThickness`, `maxLinesPerAxis`.
- Produces: `Arcane::PlanGrid2D(float ppm, float minorAlpha, float majorAlpha)`, `Arcane::EmitGrid2D(...)`, and the local collector below. `Editor::PlanGrid2D` and `Editor::DrawGrid2D` keep their signatures so `ViewportGridTest.cpp` stays green.

`DrawPhysicsDebug` submits pixels through `PhysicsDebugDrawOptions::view` (`EditorAppFrame.cpp:1911-1926`). The collector stores those calls as screen-space overlay primitives. Grid lines are the world endpoints `DrawGrid2D` already computes before `WorldToScreen` (`ViewportGrid.cpp:166-175`), stored with `screenSpace == false` so the overlay shader projects them.

- [ ] **Step 1: Write the failing test.**

```cpp
TEST_CASE("overlay tags: grid is depth-tested linear, physics camera and gizmo are always on top", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);

    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    Arcane::PhysicsResource& resource = *world.reg.EmplaceResource<Arcane::PhysicsResource>();
    resource.world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
    Manifold2D::Physics::BodyDef bd;
    bd.type = Manifold2D::Physics::BodyType::Static;
    bd.position = Manifold2D::Physics::Vec2(Manifold2D::Physics::Real(0), Manifold2D::Physics::Real(0));
    bd.shape = Manifold2D::Physics::MakeAabb(Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
    resource.world->AddBody(bd);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    req.showGrid2d = true;
    req.gridMinorAlpha = 0.35f;
    req.gridMajorAlpha = 0.55f;
    req.gridLineRgb = { 0.5f, 0.5f, 0.5f };
    req.gridLineThicknessPx = 1.0f;
    req.gridMaxLinesPerAxis = 4096;
    req.physicsDebug = true;
    req.physicsWholeWorld = true;
    req.cameraRect = true;
    Arcane::OverlayLine gizmo{};
    gizmo.a = { 0.0f, 0.0f, 0.0f };
    gizmo.b = { 10.0f, 0.0f, 0.0f };
    gizmo.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    gizmo.thickness = 1.0f;
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.lines.push_back(gizmo);

    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    bool sawGrid = false;
    bool sawGizmo = false;
    bool sawScreenLinear = false;
    for (const Arcane::OverlayLine& line : packet.overlay.lines)
    {
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::DepthTested &&
            line.screenSpace == false)
            sawGrid = true;
        if (line.space == Arcane::OverlayColorSpace::DisplayReferred)
        {
            sawGizmo = true;
            CHECK(line.depth == Arcane::OverlayDepthClass::AlwaysOnTop);
            CHECK(line.screenSpace == true);
        }
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::AlwaysOnTop &&
            line.screenSpace == true)
            sawScreenLinear = true;
    }
    CHECK(sawGrid);
    CHECK(sawGizmo);
    CHECK(sawScreenLinear);
}
```

Append to `NriGraphPixelTest.cpp`, after the helpers `MakeVehicle`, `RenderOne`, `At`, `Luma`, `kW`, and `kH` (`:139-234`). Add `#include <Arcane/Render/ExtractFrame.hpp>`, `#include <Arcane/Scene/PhysicsSystem.hpp>`, `#include <Arcane/Scene/SceneModule.hpp>`, `#include <Astra/Component/ComponentRegistry.hpp>`, and `<cmath>`. `PhysicsSystem.hpp:128` is `PhysicsResource`. That header includes `Ecs.hpp`; this TU is under `ArcaneTests`, not `ArcaneClient/src/Arcane/Render`, so the include gate does not see it. Do not set `frame.overlay` or `frame.grid` on either capture.

`Ortho2DView({0,0}, 16.0f, {kW, kH})` (`RenderViewSettings.hpp:33`) gives `halfW = 16 * (160/96) = 80/3`. `AsAffine2D` (`ViewTransform.hpp:113-127`) then has `scale.x = 3` px/m and `scale.y = -3`, and `offset = (80, 48)`. `WorldToScreen` (`ViewTransform.hpp:67`) maps world `(0,0,0)` to screen `(80, 48)`. +Y is up and screen Y is down. The Y axis is the world line `x = 0` and the X axis is `y = 0` (`ViewportGrid.cpp:224-231`). With `gridLineThicknessPx = 8` the screen thickness is 8 px, so the Y axis covers x `76-84` and the X axis covers y `44-52`. Sample the Y axis at `(80, 20)`.

`MakeAabb` takes half-extents (`Shapes.hpp:156`). A static body at `(10, 6)` with `MakeAabb(1, 1)` is the world box x `9..11`, y `5..7`. `Affine2D::Point` (`ViewTransform.hpp:44`) puts that box near screen x `107-113`, y `27-33`, clear of both axis quads and of the gizmo. `cameraRect` is false, so every screen-space linear always-on-top line is physics, not the camera rect. `MakePhysicsDebugDrawOptions` (`PhysicsDebugDraw.cpp:290`) copies the published `lineThickness`, whose default is `1.0f` (`PhysicsDebugSettings.hpp:23`). The test samples a neighbourhood because that line is one pixel wide.

```cpp
void CheckExtractedOverlayReachesTheBackbuffer(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{ components };
    Arcane::RegisterSceneComponents(reg);

    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    Arcane::PhysicsResource& resource = *reg.EmplaceResource<Arcane::PhysicsResource>();
    resource.world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
    Manifold2D::Physics::BodyDef bd;
    bd.type = Manifold2D::Physics::BodyType::Static;
    bd.position = Manifold2D::Physics::Vec2(
        Manifold2D::Physics::Real(10), Manifold2D::Physics::Real(6));
    bd.shape = Manifold2D::Physics::MakeAabb(
        Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
    resource.world->AddBody(bd);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { kW, kH };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 16.0f, req.viewport);
    req.playMode = false;
    req.showGrid2d = true;
    req.gridMinorAlpha = 0.35f;
    req.gridMajorAlpha = 0.55f;
    req.gridLineRgb = { 0.5f, 0.5f, 0.5f };
    req.gridLineThicknessPx = 8.0f;
    req.gridMaxLinesPerAxis = 4096;
    req.physicsDebug = true;
    req.physicsWholeWorld = true;
    req.cameraRect = false;
    Arcane::OverlayLine gizmo{};
    gizmo.a = { 4.0f, 10.0f, 0.0f };
    gizmo.b = { 16.0f, 10.0f, 0.0f };
    gizmo.color = { 0.0f, 0.0f, 1.0f, 1.0f };
    gizmo.thickness = 8.0f;
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.lines.push_back(gizmo);

    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    const glm::vec3 origin = packet.spriteView->view.WorldToScreen({ 0.0f, 0.0f, 0.0f });
    CHECK(std::abs(origin.x - 80.0f) < 0.5f);
    CHECK(std::abs(origin.y - 48.0f) < 0.5f);

    const Arcane::OverlayLine* physics = nullptr;
    float best = -1.0f;
    for (const Arcane::OverlayLine& line : packet.overlay.lines)
    {
        if (!line.screenSpace || line.space != Arcane::OverlayColorSpace::Linear
            || line.depth != Arcane::OverlayDepthClass::AlwaysOnTop)
            continue;
        const float len = glm::length(glm::vec2(line.b) - glm::vec2(line.a));
        if (len > best)
        {
            best = len;
            physics = &line;
        }
    }
    REQUIRE(physics != nullptr);
    const int px = (int)std::lround(0.5f * (physics->a.x + physics->b.x));
    const int py = (int)std::lround(0.5f * (physics->a.y + physics->b.y));
    REQUIRE(std::abs(px - 80) > 8 || std::abs(py - 20) > 8);
    REQUIRE(std::abs(px - 10) > 8 || std::abs(py - 10) > 8);

    Arcane::RenderFramePacket cleared = packet;
    cleared.overlay.lines.clear();
    cleared.overlay.fills.clear();
    cleared.overlay.hasGrid3d = false;

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->BindPacket(&packet);
    RenderOne(*v.ctx, frame);
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> drawn;
    REQUIRE(v.ctx->ReadCapture(w, h, drawn));

    v.ctx->BindPacket(&cleared);
    RenderOne(*v.ctx, frame);
    std::uint32_t w2 = 0, h2 = 0;
    std::vector<unsigned char> bare;
    REQUIRE(v.ctx->ReadCapture(w2, h2, bare));
    REQUIRE(w == kW && h == kH && w2 == w && h2 == h);

    const Rgba gridOn = At(drawn, w, 80, 20);
    const Rgba gridOff = At(bare, w, 80, 20);
    CHECK(gridOn.r != gridOff.r || gridOn.g != gridOff.g || gridOn.b != gridOff.b);

    const Rgba gizmoOn = At(drawn, w, 10, 10);
    CHECK(gizmoOn.r == 0);
    CHECK(gizmoOn.g == 0);
    CHECK(gizmoOn.b == 255);
    const Rgba gizmoOff = At(bare, w, 10, 10);
    CHECK(!(gizmoOff.r == 0 && gizmoOff.g == 0 && gizmoOff.b == 255));

    bool physicsDiff = false;
    const glm::ivec2 samples[5] = {
        { px, py }, { px - 1, py }, { px + 1, py }, { px, py - 1 }, { px, py + 1 } };
    for (const glm::ivec2 s : samples)
    {
        if (s.x < 0 || s.y < 0 || s.x >= (int)w || s.y >= (int)h)
            continue;
        const Rgba a = At(drawn, w, (std::uint32_t)s.x, (std::uint32_t)s.y);
        const Rgba b = At(bare, w, (std::uint32_t)s.x, (std::uint32_t)s.y);
        if (a.r != b.r || a.g != b.g || a.b != b.b)
            physicsDiff = true;
    }
    CHECK(physicsDiff);
    CHECK(Arcane::RenderErrorCount() == before);
}
```

The chebyshev test (`abs(dx) > 8 || abs(dy) > 8`) rejects a sample that is inside the 8 px square around `(80, 20)` or `(10, 10)` and accepts one that is outside it. The axis colour is `Grid2DStyle::axisY` (`{0.3, 0.8, 0.3, 0.9}`) after ACES and a source-over blend, so the grid check is "the texel changed", not an exact byte. The gizmo is display-referred, alpha 1, so `(10, 10)` is `(0, 0, 255)` and the cleared frame (clear `{0.02, 0.02, 0.04}` from `RenderDeviceSettings.hpp:82`, then ACES) is not. One `PixelVehicle`, two `RenderOne`s. `BindPacket` before each, because Task 5 nulls the pointer before return. Add `"extracted overlay reaches the backbuffer (d3d12)"` and `"extracted overlay reaches the backbuffer (vulkan)"`, tagged `[gpu][pixel][overlay][nri]` plus the backend tag.

- [ ] **Step 2: Run the cases.**

The GPU case is a new `TEST_CASE` in `NriGraphPixelTest.cpp`, so this step builds before it runs.

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "overlay tags"
.\ArcaneTests.exe "extracted overlay reaches the backbuffer"
```

Expected: FAIL. `packet.overlay.lines` is empty, and the backbuffer case finds no physics line or no overlay pixel. The pass still reads `FrameDesc`, and this case sets neither `frame.overlay` nor `frame.grid`.

- [ ] **Step 3: Move the grid, add the collector, and append the editor primitives.**

`Grid2D.hpp` (no editor settings, no `Registry.hpp`):

```cpp
struct Grid2DStyle
{
    float minorAlpha = 0.35f;
    float majorAlpha = 0.55f;
    glm::vec3 lineRgb{ 0.5f, 0.5f, 0.5f };
    float lineThicknessPx = 1.0f;
    std::int64_t maxLinesPerAxis = 4096;
    glm::vec4 axisX{ 0.85f, 0.25f, 0.25f, 0.9f };
    glm::vec4 axisY{ 0.3f, 0.8f, 0.3f, 0.9f };
};

Grid2DPlan PlanGrid2D(float pixelsPerMetre, float minorAlpha, float majorAlpha) noexcept;

void EmitGrid2D(const ViewTransform& view, const Grid2DPlan& plan, const Grid2DStyle& style,
                std::vector<OverlayLine>& out);
```

`GridLevel` and `Grid2DPlan` move with it from `ViewportGrid.hpp:85-97`. `Editor::PlanGrid2D(float)` reads `Settings<EditorGridSettings>()` and calls `Arcane::PlanGrid2D`. `EmitGrid2D` is the body of `DrawGrid2D` (`ViewportGrid.cpp:134` through its closing brace) with two substitutions. The `line` lambda pushes an `OverlayLine` instead of calling `b.Line`:

```cpp
OverlayLine line;
line.a = glm::vec3(a, 0.0f);
line.b = glm::vec3(wb, 0.0f);
const float ppm = PixelsPerMetre(view);
line.thickness = ppm > 0.0f ? style.lineThicknessPx / ppm : 0.0f;
line.color = colour;
line.depth = OverlayDepthClass::DepthTested;
line.space = OverlayColorSpace::Linear;
line.screenSpace = false;
if (line.thickness > 0.0f)
    out.push_back(line);
```

Axis lines use `style.axisX` and `style.axisY` and the same tags. Delete `b.SetLayer`. `Editor::DrawGrid2D` calls `EmitGrid2D`, then for each stored line calls `b.SetLayer(0, 0)` once and `b.Line(WorldToScreen(line.a), WorldToScreen(line.b), line.thickness * PixelsPerMetre(view), line.color)`. That keeps `ViewportGridTest.cpp` on screen pixels.

`PixelsPerMetre` moves to `Arcane::` as well. `Editor::PixelsPerMetre` forwards to it.

The collector, in `ExtractFrame.cpp`'s anonymous namespace. Every pure virtual is defined. New virtuals stay at the end of `Batcher2D`; this subclass does not change that class.

```cpp
class OverlayBatchCollector final : public Batcher2D
{
public:
    explicit OverlayBatchCollector(OverlayPrimitiveList& out) : m_out(out) {}

    void Begin(std::uint32_t, std::uint32_t) override {}
    void SetLayer(std::uint16_t, std::uint16_t) override {}
    void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override {}
    void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
    void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
    void End() override {}
    Batch2DStats Stats() const override { return {}; }
    void QuadWorld(std::uint16_t, const Guid&, const std::array<glm::vec3, 4>&,
                   glm::vec2, glm::vec2, glm::vec4) override {}
    void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
    void SetViewProjection(const glm::mat4&) override {}

    void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 color) override
    {
        OverlayLine line;
        line.a = glm::vec3(a, 0.0f);
        line.b = glm::vec3(b, 0.0f);
        line.thickness = thickness;
        line.color = color;
        line.depth = OverlayDepthClass::AlwaysOnTop;
        line.space = OverlayColorSpace::Linear;
        line.screenSpace = true;
        m_out.lines.push_back(line);
    }

    void Circle(glm::vec2 center, float radius, glm::vec4 color) override
    {
        OverlayFill fill;
        fill.kind = OverlayPrimKind::Circle;
        fill.a = glm::vec3(center, 0.0f);
        fill.radius = radius;
        fill.color = color;
        fill.depth = OverlayDepthClass::AlwaysOnTop;
        fill.space = OverlayColorSpace::Linear;
        fill.screenSpace = true;
        m_out.fills.push_back(fill);
    }

    void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color, float) override
    {
        OverlayFill fill;
        fill.kind = OverlayPrimKind::Rect;
        fill.a = glm::vec3(pos, 0.0f);
        fill.size = size;
        fill.color = color;
        fill.depth = OverlayDepthClass::AlwaysOnTop;
        fill.space = OverlayColorSpace::Linear;
        fill.screenSpace = true;
        m_out.fills.push_back(fill);
    }

private:
    OverlayPrimitiveList& m_out;
};
```

Extract, after the views exist:

```cpp
if (req.showGrid2d && packet.spriteView)
{
    const ViewTransform& view = packet.spriteView->view;
    const float ppm = PixelsPerMetre(view);
    Grid2DStyle style;
    style.minorAlpha = req.gridMinorAlpha;
    style.majorAlpha = req.gridMajorAlpha;
    style.lineRgb = req.gridLineRgb;
    style.lineThicknessPx = req.gridLineThicknessPx;
    style.maxLinesPerAxis = req.gridMaxLinesPerAxis;
    style.axisX = req.gridAxisX;
    style.axisY = req.gridAxisY;
    EmitGrid2D(view, PlanGrid2D(ppm, style.minorAlpha, style.majorAlpha), style, packet.overlay.lines);
}
if (req.showGrid3d)
{
    packet.overlay.hasGrid3d = true;
    packet.overlay.grid3d = req.grid3d;
}
if (req.physicsDebug && req.editorView)
{
    if (const PhysicsResource* phys = reg.GetResource<PhysicsResource>())
    {
        if (phys->world)
        {
            if (const std::optional<Affine2D> affine = req.editorView->AsAffine2D())
            {
                OverlayBatchCollector collector(packet.overlay);
                collector.Begin(req.viewport.x, req.viewport.y);
                PhysicsDebugDrawOptions opts = MakePhysicsDebugDrawOptions();
                opts.view = *affine;
                opts.alpha = 0.0f;
                opts.interp = reg.GetResource<PhysicsInterpBuffer>();
                opts.velocities = opts.comMarkers = opts.orientations = false;
                opts.contacts = req.physicsWholeWorld;
                DrawPhysicsDebug(*phys->world, collector, opts);
            }
        }
    }
}
if (req.cameraRect && req.editorView && !req.playMode)
{
    if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
    {
        const float aspect = req.viewport.y > 0u ? float(req.viewport.x) / float(req.viewport.y) : 1.0f;
        const float hw = cam->halfHeight * aspect;
        const float hh = cam->halfHeight;
        const glm::vec2 c = cam->worldCenter;
        const glm::vec3 world[4] = {
            { c.x - hw, c.y - hh, 0.0f }, { c.x + hw, c.y - hh, 0.0f },
            { c.x + hw, c.y + hh, 0.0f }, { c.x - hw, c.y + hh, 0.0f },
        };
        glm::vec3 screen[4];
        bool finite = true;
        for (int i = 0; i < 4; ++i)
        {
            screen[i] = req.editorView->WorldToScreen(world[i]);
            finite = finite && std::isfinite(screen[i].x) && std::isfinite(screen[i].y);
        }
        if (finite)
        {
            for (int i = 0; i < 4; ++i)
            {
                OverlayLine line;
                line.a = glm::vec3(glm::vec2(screen[i]), 0.0f);
                line.b = glm::vec3(glm::vec2(screen[(i + 1) % 4]), 0.0f);
                line.thickness = 1.0f;
                line.color = req.cameraFrameColor;
                line.depth = OverlayDepthClass::AlwaysOnTop;
                line.space = OverlayColorSpace::Linear;
                line.screenSpace = true;
                packet.overlay.lines.push_back(line);
            }
        }
    }
}
packet.overlay.lines.insert(packet.overlay.lines.end(), req.gizmoPrims.lines.begin(), req.gizmoPrims.lines.end());
packet.overlay.fills.insert(packet.overlay.fills.end(), req.gizmoPrims.fills.begin(), req.gizmoPrims.fills.end());
```

`SubmitSceneToBatcher` stops calling `DrawGrid2D` (`:1887`), `SubmitRender` (`:1891`), the physics `GetResource` block (`:1898-1927`), and `ActiveSceneCamera` (`:1953`). The Task 3 local-batcher conversion is deleted in the same edit, and so is the `FrameDesc::overlay` pointer `ArmGraphViewportFrame` stored for it. The editor call site that already calls `ExtractFrame` sets `showGrid2d`, `physicsDebug`, `physicsWholeWorld`, `cameraRect`, the grid style fields from `Settings<EditorGridSettings>()`, and `cameraFrameColor` from the theme value it reads today (`:1977-1979`). Gizmo lines are copied into `req.gizmoPrims` with the tags Task 3 already applied.

Delete the `vp.grid` assignment at `EditorAppFrame.cpp:2108-2119`. The same condition (not play mode, `ViewMode::Perspective`, `EditorViewportSettings::showGrid`, `MakeGridScene`, `view = m_runtime->View()`, `SetPlane` from `gridPlane`) moves onto the `ExtractRequest` inside `RenderSceneToViewport`, as `req.showGrid3d` and `req.grid3d`. Extract already copies those into `packet.overlay` in the block above. After this task the 3D grid reaches the node only as `packet.overlay.grid3d`.

`DeclareGraphFrame`, when `context->Packet()` is non-null, ignores `shape.overlay` and `shape.grid`. `wantOverlay` is then true when that packet's `overlay.lines` or `overlay.fills` is non-empty or `overlay.hasGrid3d` is set. The 3D-grid pointer passed to `AddOverlayNode` is `&packet->overlay.grid3d` only when `hasGrid3d` is set, and null otherwise. When `Packet()` is null, keep Task 3's `shape.overlay` / `shape.grid` test, so the GPU cases that set `frame.overlay` and never call `BindPacket` stay green. `BuildFrame` may still copy those two fields for that path. Do not delete `FrameDesc::overlay` or `FrameDesc::grid`.

`OverlayNode::Record` captures the `NriGraphContext*` the way `AddBatch2DNode` does (`Batch2DNode.cpp:1628-1641` in Task 5). When `Packet()` is non-null it draws `Packet()->overlay` (lines, fills, and `grid3d` when `hasGrid3d`) and does not read the shape pointers. When `Packet()` is null it draws the pointers `AddOverlayNode` was given. A host frame that binds a packet and leaves `FrameDesc::overlay` and `FrameDesc::grid` null still draws the packet's overlay. The backbuffer case fails if this branch is missing.

The `GetComponent` at `:3720` leaves whichever function contains it. Move that draw read into `EditorApp::BuildGizmoOverlayPrims`, a method that is not one of the seven host-gate functions, and call it before `RenderSceneToViewport`. It writes `m_gizmoPrims`. `RenderSceneToViewport` assigns `req.gizmoPrims = m_gizmoPrims` and does not call `GetComponent`. Interaction reads at `:1241`, `:1283`, and `:1339` stay in their current functions.

- [ ] **Step 4: Run the overlay case and the editor grid tests.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "overlay tags"
.\ArcaneTests.exe "extracted overlay reaches the backbuffer"
.\ArcaneTests.exe "[editor][grid]"
```

Expected: PASS. `[editor][grid]` is `ViewportGridTest.cpp`, still drawing through `Editor::DrawGrid2D`. The backbuffer case fails if the pass still reads only `FrameDesc`.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/Grid2D.hpp ArcaneClient/src/Arcane/Render/Grid2D.cpp ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.cpp ArcaneEditor/src/Viewport/ViewportGrid.hpp ArcaneEditor/src/Viewport/ViewportGrid.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/ExtractFrameTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): extract publishes overlay primitives" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 10: UI snapshot, post chain, and the default light

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/ImGuiNriNode.cpp:58-110`
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:676-681` and the `req` fill from Task 5
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`ApplyDefaultLight` at `:2089`, `RefreshSceneResolution` at `:1572`)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `CopyImDrawSnapshot` and `ImDrawSnapshotRebuild` from Task 4. `ImGuiNriNode::Record` (`ImGuiNriNode.cpp:58`) calls `RenderDrawData`. `AddImGuiNode` (`:63`) reads `CurrentImGuiDrawData()` / `CurrentGameUiDrawData()` at `:88` and `:110`. `ApplyDefaultLight` (`MeshNode.hpp:362`). `PostChainDesc` is copied by value; its bytecode members are `std::shared_ptr<const std::vector<std::uint8_t>>` (`PostChainDesc.hpp`, moved in Task 4 from `PostChainCache.hpp:55-78`).
- Produces: `packet.imgui`, `packet.gameUi`, `packet.post` / `packet.hasPost`, and the three light vectors.

- [ ] **Step 1: Write the failing test.**

`ImGui::Render` is what builds the source. Do not hand-fill a list and call `AddDrawList`: that asserts (`imgui_draw.cpp:2305-2308`). `VtxOffset > 0` is Task 4's case. This case checks two real windows, the raw user-texture ids, the texture-pointer list, the post bytecode, and a callback refusal through `AddCallback` on a second rendered frame. The host batcher is installed so the sprite copy from Task 5 does not depend on a null context. `World` has no sprites here.

```cpp
static void DummyImGuiCallback(const ImDrawList*, const ImDrawCmd*) {}

TEST_CASE("extract copies two ImGui lists and keeps the post bytecode", "[nri][extract]")
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(32.0f, 32.0f);
    // Before NewFrame, matching the live backend (ImGuiNri.cpp:119):
    // without RendererHasTextures NewFrame demands a built font atlas
    // (imgui_draw.cpp:2778).
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    ImGui::Begin("one");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(42), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::Begin("two");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(7), ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f));
    ImGui::End();
    ImGui::Render();
    ImDrawData* live = ImGui::GetDrawData();
    REQUIRE(live != nullptr);
    REQUIRE(live->CmdLists.Size >= 2);
    REQUIRE(live->Textures != nullptr);

    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    Arcane::PostChainDesc desc;
    desc.passes.push_back({});
    desc.passes.back().vsBytes = bytes;

    World world;
    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(32u, 32u);
    Arcane::RenderContext2D renderCtx;
    renderCtx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(renderCtx);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 32u, 32u };
    req.imgui = live;
    req.post = &desc;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.imgui.lists.size() == static_cast<std::size_t>(live->CmdLists.Size));
    REQUIRE(packet.imgui.lists.size() >= 2u);
    CHECK(packet.imgui.hasTextures);
    REQUIRE(packet.imgui.textures.size() == static_cast<std::size_t>(live->Textures->Size));
    bool saw42 = false;
    bool saw7 = false;
    for (std::size_t li = 0; li < packet.imgui.lists.size(); ++li)
    {
        CHECK(packet.imgui.lists[li].vertices.size() ==
              static_cast<std::size_t>(live->CmdLists[static_cast<int>(li)]->VtxBuffer.Size));
        for (const Arcane::ImDrawListSnapshot::Cmd& cmd : packet.imgui.lists[li].cmds)
        {
            if (cmd.managed == nullptr && cmd.texture == static_cast<ImTextureID>(42))
                saw42 = true;
            if (cmd.managed == nullptr && cmd.texture == static_cast<ImTextureID>(7))
                saw7 = true;
        }
    }
    CHECK(saw42);
    CHECK(saw7);
    CHECK(packet.hasPost);
    desc = {};
    bytes.reset();
    REQUIRE(packet.post.passes.size() == 1u);
    REQUIRE(packet.post.passes[0].vsBytes);
    CHECK(packet.post.passes[0].vsBytes->size() == 4u);

    ImGui::NewFrame();
    ImGui::Begin("callback");
    ImGui::GetWindowDrawList()->AddCallback(&DummyImGuiCallback, nullptr);
    ImGui::End();
    ImGui::Render();
    Arcane::ExtractRequest req2;
    req2.viewport = { 32u, 32u };
    req2.imgui = ImGui::GetDrawData();
    Arcane::PacketRing ring2;
    const Arcane::RenderFramePacket& refusedPacket = Arcane::ExtractFrame(world.reg, req2, ring2);
    CHECK(refusedPacket.imgui.lists.empty());
    CHECK_FALSE(refusedPacket.imgui.hasTextures);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
}
```

`PostChainPassDesc::vsBytes` is the `shared_ptr<const vector<uint8_t>>` (`PostChainCache.hpp:57`, moved to `PostChainDesc.hpp` in Task 4). The copy is the struct assignment `packet.post = *req.post`.

- [ ] **Step 2: Run the case.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract copies two ImGui lists"
```

Expected: FAIL. `packet.imgui.lists` is empty and `hasPost` is false.

- [ ] **Step 3: Copy the snapshot, the chain, and the light.**

Inside `ExtractFrame`, after the overlay block:

```cpp
if (!CopyImDrawSnapshot(req.imgui, packet.imgui))
    packet.imgui = {};
if (!CopyImDrawSnapshot(req.gameUi, packet.gameUi))
    packet.gameUi = {};
packet.hasPost = req.post != nullptr;
if (req.post)
    packet.post = *req.post;
packet.lightDirection = req.lightDirection;
packet.lightColor = req.lightColor;
packet.ambient = req.ambient;
```

`RuntimeFrame`, where it fills `req`, before `ExtractFrame`:

```cpp
Arcane::MeshSceneDesc light{};
Arcane::ApplyDefaultLight(light, Arcane::Settings<Arcane::RenderMeshDefaultLightSettings>());
req.lightDirection = light.lightDirection;
req.lightColor = light.lightColor;
req.ambient = light.ambient;
req.imgui = graphFrame.imgui;
req.gameUi = graphFrame.gameUi;
req.post = graphFrame.post;
```

`graphFrame.imgui` is assigned at `:508`. `graphFrame.post` is `const PostChainDesc*` (`NriGraphContext.hpp:457`), assigned at `RuntimeFrame.cpp:692`. `graphFrame.gameUi` is the `FrameDesc` field at `NriGraphContext.hpp:546`; `RuntimeFrame` leaves it null. Delete the `ApplyDefaultLight` call inside the mesh-scene block Task 6 moved to after `ExtractFrame`. That block copies `packet.lightDirection`, `packet.lightColor`, and `packet.ambient` onto the mesh desc. The editor deletes `ApplyDefaultLight` at `:2089` and fills the same three request fields at its `ExtractFrame` call.

`AddImGuiNode`'s record lambda (`ImGuiNriNode.cpp:105-114`) becomes:

```cpp
[context, target, gameUi](RenderGraphNodeContext& nodeContext)
{
    if (!context)
        return;
    if (ImGuiNriNode* node = gameUi ? context->ImGuiGame() : context->ImGuiHud())
    {
        if (context->Packet())
        {
            const ImDrawSnapshot& snap = gameUi ? context->Packet()->gameUi : context->Packet()->imgui;
            ImDrawSnapshotRebuild rebuilt(snap);
            node->Record(nodeContext, rebuilt.Data(), target);
        }
        else
        {
            node->Record(nodeContext,
                         gameUi ? context->CurrentGameUiDrawData()
                                : context->CurrentImGuiDrawData(),
                         target);
        }
    }
}
```

The declaration-time block at `:80-92` builds its own stack `ImDrawSnapshotRebuild` when `context->Packet()` is set, and keeps the live pointers when it is not. That object is not the record lambda's rebuild. Both name the same `ImTextureData` objects, so `NewFrameTexUpdates` (`ImGuiNri.cpp:772-779`) can `SetTexID` before `RenderDrawData` reads `GetTexID`. A null `Textures` makes `NewFrameTexUpdates` return immediately (`:775-776`).

```cpp
if (context)
{
    if (ImGuiNriNode* node = gameUi ? context->ImGuiGame() : context->ImGuiHud())
    {
        if (context->Packet())
        {
            const ImDrawSnapshot& snap = gameUi ? context->Packet()->gameUi : context->Packet()->imgui;
            ImDrawSnapshotRebuild rebuilt(snap);
            node->PrepareFrame(rebuilt.Data(), context->Graves(), context->Graph().DebugSubmitCount());
        }
        else
        {
            node->PrepareFrame(gameUi ? context->CurrentGameUiDrawData()
                                      : context->CurrentImGuiDrawData(),
                               context->Graves(),
                               context->Graph().DebugSubmitCount());
        }
    }
}
```

A preview graph has no packet and keeps the live pointers. `ImGuiNriNode.cpp` includes `RenderFramePacket.hpp` for `ImDrawSnapshotRebuild`. Service stays here, at declaration. Extract does not call `NewFrameTexUpdates` and does not wait on the graphics queue (`ImGuiNri.hpp:184-204`).

`RefreshSceneResolution` deletes the `ActiveSceneCamera` branch at `:1572-1577`. Play mode leaves the runtime view for extract: the editor's `ExtractFrame` call sets `req.playMode = InPlayMode()` and leaves `req.editorView` empty on that path, and immediately after `ExtractFrame` returns calls `SetView(packet.spriteView->view)` when `spriteView` has a value. Edit mode keeps `SetView(m_camera.Resolve(ViewportSize()))` at `:1570` and passes that same value as `req.editorView`. `EditorApp::RefreshSceneResolution(` is already a host-gate signature; after this edit its body contains none of the forbidden tokens.

Thumbnail `Impl::Harvest` and `RenderGraphPreview` do not gain `ExtractFrame` or a registry read.

- [ ] **Step 4: Run the UI case.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract copies two ImGui lists"
.\ArcaneTests.exe "[packet]"
```

Expected: PASS. `[packet]` still covers `ImDrawSnapshotRebuild` from Task 4, including the two `[gpu][packet]` reach cases, which stay red-dominant.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/ImGuiNriNode.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/ExtractFrameTest.cpp
git commit -m "feat(render): extract publishes the UI snapshot and the post chain" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 11: Behavioural extract-skip on the three hosts

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp` (one `fprintf` immediately before the `if (!m_config.reportPath.empty())` at `:1249`)
- Modify: `ArcaneServer/src/ServerReport.hpp` (one field; `kSchemaVersion` stays 1)
- Modify: `ArcaneServer/src/ServerReport.cpp:25` (`ToJson`)
- Modify: `ArcaneServer/src/ServerApp.cpp:225-230` (assign the field before `Finish`)
- Test: `ArcaneTests/src/ExtractHostTest.cpp`

**Interfaces:**
- Consumes: `ExtractFrameCallCount` / `ResetExtractFrameCallCount` from Task 5. `RunLoop::Advance(double)` (`RunLoop.hpp:104`). `Arcane::Runtime rt(Arcane::Test::Process())` (`ClientRuntimeTest.cpp:28`). `RuntimeApp --headless` still builds an offscreen vehicle (`RuntimeApp.cpp:447-457`). `WitnessScratch` / `RunWitness` (`HostWitness.hpp`, the pattern in `ServerWitnessTest.cpp:26-53`). `ServerApp::Run` ticks `Loop().Advance` (`ServerApp.cpp:191`) and sets `hasRenderSubmission = false` (`:230`). ArcaneServer links ArcaneCore only (`premake5.lua:785-792`).
- Produces: `ServerReport::extractFrameCalls`, the stdout line `extract-frames: N`, and three tests. No source scan for the token `ExtractFrame`.

- [ ] **Step 1: Write the failing tests.**

`ArcaneTests/src/ExtractHostTest.cpp`:

```cpp
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/ExtractAccounting.hpp>
#include <Arcane/Base/Runtime.hpp>
#include "Helpers/TestTypeContext.hpp"
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("a non-rendering runtime leaves the extract counter at 0", "[nri][extract]")
{
    Arcane::ResetExtractFrameCallCount();
    Arcane::Runtime rt(Arcane::Test::Process());
    for (int i = 0; i < 4; ++i)
        rt.Loop().Advance(1.0 / 60.0);
    CHECK(Arcane::ExtractFrameCallCount() == 0u);
}

static std::string Slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int CountExactLine(const std::string& text, const std::string& want)
{
    int n = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        std::size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == want) ++n;
        i = end + (end < text.size() ? 1 : 0);
        if (end == text.size()) break;
    }
    return n;
}

TEST_CASE("RuntimeApp --headless prints extract-frames equal to --frames", "[gpu][extract][witness]")
{
    const std::filesystem::path dir = std::filesystem::absolute("../ArcaneRuntime");
    REQUIRE(std::filesystem::exists(dir / "ArcaneRuntime.exe"));
    Arcane::Test::WitnessScratch scratch(dir, "extract-headless");
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneRuntime.exe";
    inv.workingDir = scratch.Dir();
    inv.args = { "--project", "ReferenceProject", "--frames", "4", "--headless" };
    inv.hardCapMs = 60000;
    Arcane::Test::WitnessRun run = Arcane::Test::RunWitness(inv);
    INFO(Slurp(run.stdoutPath));
    CHECK(run.exitCode == 0);
    CHECK(CountExactLine(Slurp(run.stdoutPath), "extract-frames: 4") == 1);
}

TEST_CASE("ArcaneServer ticks N frames with extractFrameCalls 0", "[witness][server][extract]")
{
    const std::filesystem::path dir = std::filesystem::absolute("../ArcaneServer");
    REQUIRE(std::filesystem::exists(dir / "ArcaneServer.exe"));
    Arcane::Test::WitnessScratch scratch(dir, "extract-server");
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = scratch.Dir() / "ArcaneServer.exe";
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "4", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    Arcane::Test::WitnessRun run = Arcane::Test::RunWitness(inv);
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("framesTicked") == 4);
    CHECK(run.report.at("extractFrameCalls") == 0);
    CHECK(run.report.at("systems").at("hasRenderSubmission") == false);
    CHECK(run.report.at("systems").at("render") == 0);
    CHECK(run.report.at("presentation").at("clientAttached") == false);
    CHECK(run.report.at("presentation").at("clientDllLoadedAtBoot") == false);
    CHECK(run.report.at("schemaVersion") == 1);
}
```

Use the real namespaces from `HostWitness.hpp` (`Arcane::Test` is what `ServerWitnessTest.cpp` imports via `using namespace Arcane::Test`). Match `WitnessInvocation::args` to that header's element type.

- [ ] **Step 2: Generate and run the three cases.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "leaves the extract counter"
.\ArcaneTests.exe "prints extract-frames"
.\ArcaneTests.exe "extractFrameCalls"
```

Expected: the in-process case PASSes (the counter exists and nothing in that process calls `ExtractFrame`). The two witness cases FAIL: stdout has no `extract-frames: 4` line, and the server JSON has no `extractFrameCalls` key.

- [ ] **Step 3: Report the counter from the rendering host and from the server.**

Immediately before `if (!m_config.reportPath.empty())` at `RuntimeApp.cpp:1249`:

```cpp
std::fprintf(stdout, "extract-frames: %llu\n",
             static_cast<unsigned long long>(Arcane::ExtractFrameCallCount()));
std::fflush(stdout);
```

This is `fprintf`, not `ARC_INFO`. It runs when `reportPath` is empty. `VerifyReport::schemaVersion` stays 14.

`ServerReport` gains `std::uint64_t extractFrameCalls = 0;`. `ToJson` writes `j["extractFrameCalls"] = extractFrameCalls;`. `kSchemaVersion` stays 1. `ServerApp::Run`, beside the other `rep.` assignments at `:225-230` and before `Finish`:

```cpp
rep.extractFrameCalls = Arcane::ExtractFrameCallCount();
```

A future `ExtractFrame` call inside the server tick would store N and fail the test. The existing named-key checks in `ServerWitnessTest.cpp:39-53` stay valid because the new key is additive.

- [ ] **Step 4: Re-run the three cases.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "leaves the extract counter"
.\ArcaneTests.exe "prints extract-frames"
.\ArcaneTests.exe "extractFrameCalls"
```

Expected: PASS. The headless runtime line equals `extract-frames: 4` once. The server report has `extractFrameCalls == 0` and `framesTicked == 4`.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneRuntime/src/RuntimeApp.cpp ArcaneServer/src/ServerReport.hpp ArcaneServer/src/ServerReport.cpp ArcaneServer/src/ServerApp.cpp ArcaneTests/src/ExtractHostTest.cpp
git commit -m "test(render): extract counter stays zero on non-rendering hosts" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 12: The include gate and the host frame-builder gate

**Files:**
- Create: `ArcaneCore/src/Arcane/Scene/SceneCacheRecords.hpp`
- Modify: `ArcaneCore/src/Arcane/Scene/SceneResources.hpp` (include the new header; delete the moved definitions)
- Modify: `ArcaneClient/src/Arcane/Render/MeshCache.hpp:53`, `MeshMaterialCache.hpp:58`, `SpriteCache.hpp:32`, `PhysicsDebugDraw.cpp:37` (include `SceneCacheRecords.hpp` instead of `SceneResources.hpp`)
- Modify: `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp:44` (the forward-declaration comment names the new header)
- Test: `ArcaneTests/src/RenderRegistryIncludeTest.cpp`
- Tasks 5–10 already removed the other registry includes this gate depends on, including Task 8's drops from `PickEmit.cpp` (`PhysicsComponents.hpp` at `:7` and `SceneResources.hpp` at `:9` among them). The four cache and debug-draw TUs are this task's split, named here, not a file the compiler output picks. If the gate fails on a path that is absent from the table below, stop and correct this table before calling the task done.

**Interfaces:**
- Consumes: `ExtractFrame.cpp` as the one Render `.cpp` that includes `Astra/Registry/Registry.hpp`. `DepthConvention.hpp` (no registry). `SceneCamera.hpp:20` (includes `Registry.hpp`). The closed set below. `scripts/settings-compile-fail.ps1:11-14` (vswhere, then `VC\Auxiliary\Build\vcvars64.bat`). `SceneResources.hpp` includes `Ecs.hpp` at `:7`, and `Ecs.hpp:23` includes `Astra/Registry/Registry.hpp`. `Ecs.hpp:34` is `using Entity = Astra::Entity`. `Astra::Entity` is `Astra/Entity/Entity.hpp:160`.
- Produces: `SceneCacheRecords.hpp` and `[registry-gate]`. The compiler is an absolute `cl.exe` resolved in the test, so the case passes when the process cwd is the ArcaneTests exe dir.

`MeshCache.hpp:53`, `MeshMaterialCache.hpp:58`, `SpriteCache.hpp:32`, and `PhysicsDebugDraw.cpp:37` include `SceneResources.hpp` today, so each of those TUs preprocesses `Registry.hpp`. Nothing in Tasks 5–10 edits those four files. Their other includes do not: `MeshCache.cpp` includes `MeshCache.hpp`, `Log.hpp`, and `MeshAsset.hpp`; `SpriteCache.cpp` includes `SpriteCache.hpp`, `Assets.hpp`, `Log.hpp`, and `SpriteAsset.hpp`; `MeshMaterialCache.cpp` includes `MeshMaterialCache.hpp`, `Log.hpp`, `MaterialAsset.hpp`, `MaterialSource.hpp`, and `MaterialTypes.hpp`; `PhysicsDebugDraw.cpp` includes `PhysicsDebugDraw.hpp`, the Manifold headers, and `Batcher2D.hpp`. None of those headers include `Ecs.hpp` or `Registry.hpp`. The types those TUs need from `SceneResources.hpp` are `MeshEntry`, `ResolvedMeshMaterial`, `SpriteEntry`, and `PhysicsInterpBuffer` plus `InterpPose`, `Lerp`, and `AngleLerp`. `SpriteEntry`, `MeshEntry`, and `ResolvedMeshMaterial` use `Guid`, `glm`, `MeshData`, `MeshBounds`, `MeshSlot`, `MeshSource`, `MaterialBlendMode`, and `std::tuple`. `MeshAsset.hpp` includes `MeshBuilder.hpp` (`MeshData` at `:68`, `using MeshBounds = Aabb` at `:114`) and `ArtifactReader.hpp`. `MaterialBlendMode.hpp` includes only `<cstdint>`. `Guid.hpp`, `Aabb.hpp`, `ArtifactReader.hpp`, and `Constant.hpp` do not include `Ecs.hpp` or `Registry.hpp`.

`PhysicsInterpBuffer::slotOf` is `Astra::FlatMap<Arcane::Entity, InterpSlot>` (`SceneResources.hpp:101`). `FlatMap.hpp` includes `Entity.hpp`, `Base.hpp`, `Simd.hpp`, `Memory.hpp`, and `Swiss.hpp` (`FlatMap.hpp:17-21`). Those headers, and the Mosaic headers they reach, do not include `Registry.hpp` or `Ecs.hpp`. The new header includes that one Astra container header and does not include `Ecs.hpp`. The moved field is spelled `Astra::FlatMap<Astra::Entity, InterpSlot>`. `Astra::Entity` is the same type as `Arcane::Entity`. Do not pimpl it: that would change the layout. This is not an ECS-registry include, and the per-TU check below asserts `Registry.hpp` and `Ecs.hpp` are both absent.

Move these definitions into `SceneCacheRecords.hpp` verbatim, including their comments: `Lerp` and `AngleLerp` (`SceneResources.hpp:44-61`), `InterpPose` (`:65-70`), `InterpSlot` (`:77-81`), `PhysicsInterpBuffer` (`:91-107`, with the `slotOf` spelling above), `SpriteEntry` (`:129-141`), `MeshEntry` (`:205-247`), `ResolvedMeshMaterial` (`:289-329`). Leave `SceneRoot`, `RenderContext2D`, `SpriteTable`, `MeshTable`, and `MeshMaterialTable` in `SceneResources.hpp`. That header includes `SceneCacheRecords.hpp` and keeps `Ecs.hpp`, `Guid.hpp`, `ViewTransform.hpp`, `<unordered_map>`, and `<cstdint>`. It drops the includes only the moved types used: `FlatMap.hpp`, `<tuple>`, `<cmath>`, `Constant.hpp`, `MeshAsset.hpp`, `MeshBuilder.hpp`, `MaterialBlendMode.hpp`, `glm/glm.hpp`, and `<vector>`.

The new header includes `Constant.hpp`, `Guid.hpp`, `MaterialBlendMode.hpp`, `MeshAsset.hpp`, `FlatMap.hpp`, `glm/glm.hpp`, `<cmath>`, `<cstdint>`, `<tuple>`, and `<vector>`. It does not include `Ecs.hpp`, `Registry.hpp`, or `SceneResources.hpp`.

On each of the four includes, replace `SceneResources.hpp` with `SceneCacheRecords.hpp` and point the include-line comment at the new header. Also update the comments that name `SceneResources.hpp` as the home of a moved type: `MeshCache.hpp:38`, `SpriteCache.cpp:48` (the `SpriteEntry` field defaults; the old `:85-92` line numbers do not survive the move), and `PhysicsDebugDraw.hpp:44-47`. `PhysicsDebugDraw.hpp:48` stays a forward declaration. Comments that name `SceneResources.hpp` as the home of `SpriteTable` or `MeshTable` stay, because those tables stay. Do not edit host-function comments to satisfy the scanner; that is the next section.

Closed set. These are the only files the gate is allowed to talk about:

| File | What it includes after its owning task | Owner |
|---|---|---|
| `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp` | `Astra/Registry/Registry.hpp`, and the headers that pull it in: `VisibilitySystem.hpp` (`:30`), `GpuSceneSync.hpp` (`:27`), `RenderSystems.hpp` (`:37`), `SceneCamera.hpp`, `PhysicsSystem.hpp` (via `Ecs.hpp:23`) | 5–10 |
| `ArcaneClient/src/Arcane/Render/PickEmit.cpp` | `PickEmit.hpp`, `Constant.hpp`, `glm/glm.hpp`, `<algorithm>`, `<cmath>`. Task 8 dropped `SpriteGeometry.hpp` (`:4`), `VisibilitySystem.hpp` (`:5`), `Components.hpp` (`:6`), `PhysicsComponents.hpp` (`:7`), `PhysicsSystem.hpp` (`:8`), `SceneResources.hpp` (`:9`), `Registry.hpp` (`:11`), and `<utility>` (`:17`). `PickPassId`, `PickSampleTexel`, `PickKindCode`, `PickBoundHalfExtents`, and `BuildPickIdGeometry` stayed. | 8 |
| `ArcaneClient/src/Arcane/Render/MeshCache.cpp` | `SceneCacheRecords.hpp` via `MeshCache.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| `ArcaneClient/src/Arcane/Render/MeshMaterialCache.cpp` | `SceneCacheRecords.hpp` via `MeshMaterialCache.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| `ArcaneClient/src/Arcane/Render/SpriteCache.cpp` | `SceneCacheRecords.hpp` via `SpriteCache.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.cpp` | `SceneCacheRecords.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| Every other `.cpp` under `ArcaneClient/src/Arcane/Render/` | None of `Registry.hpp`, `VisibilitySystem.hpp`, `GpuSceneSync.hpp`, `RenderSystems.hpp`, `SceneCamera.hpp`, `Ecs.hpp`, `BoundsSystem.hpp`, `PhysicsSystem.hpp`. `MeshNode.cpp` mentions `SceneCamera.hpp` in comments and does not include it. | already true before Task 5 |

`GpuSceneHost.hpp:28` includes `Registry.hpp` and lives under `Host/`, outside this compile. `ClientRuntime.cpp` includes `RenderSystems.hpp` and lives under `Client/`, outside this compile. `ComponentRegistry.hpp` and `CVarRegistry.hpp` are different paths; the match string is `Astra/Registry/Registry.hpp` with either slash. `Nri/` has no allow-list entry.

Host-gate functions, and the only token each one may strip from a line before the forbidden-token scan:

| Signature | File | After the comment strip, erase this token from the line, then whole-word scan |
|---|---|---|
| `RenderGraph(` | `ArcaneRuntime/src/RuntimeFrame.cpp:452` | `GpuSceneInvalidate` (the call at `:756`) |
| `EditorApp::RenderSceneToViewport(` | `ArcaneEditor/src/App/EditorAppFrame.cpp` | `GpuSceneInvalidate` (the call at `:1764`) |
| `EditorApp::ArmGraphViewportFrame(` | `ArcaneEditor/src/App/EditorAppFrame.cpp` | `PickPassIdOf` (the loop at `:2177-2181`) |
| `EditorApp::RefreshSceneResolution(` | `ArcaneEditor/src/App/EditorAppFrame.cpp:1506` | nothing |
| `EditorApp::SubmitSceneToBatcher(` | `ArcaneEditor/src/App/EditorAppFrame.cpp` | nothing |
| `MaterialPreviewHarvester::Impl::Harvest(` | `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:1138` | nothing |
| `RenderGraphPreview(` | `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:1662` | nothing |

Strip `//` and `/* */` comments on the whole function body before the line scan, then apply the allow-list `Strip` of `GpuSceneInvalidate` or `PickPassIdOf`, then match forbidden tokens as whole words. An identifier character is `[A-Za-z0-9_]`. `GpuSceneSync` does not match `GpuSceneSyncNode`. `ActivePerspectiveSceneCamera` does not contain `ActiveSceneCamera` as a substring. Replacing a comment with a space, and keeping the newlines inside a block comment, stops two tokens from joining across it. A `//` or `/*` inside `"..."` or `'...'` is not a comment (a `\` escapes the next character); a forbidden token that sits inside a string still counts. No host source comment is edited to make this pass. The comments the stripper has to ignore, and that stay in the source, are: `RuntimeFrame.cpp:554` and `:577` (`CollectPickables`, inside `RenderGraph` at `:452`), `:651` and `:752` (`PrepareSceneForRender`), `:664` and `:751` (`GpuSceneSyncNode`); `EditorAppFrame.cpp:1564` (`ActiveSceneCamera`, inside `RefreshSceneResolution` at `:1506`), `:1679` (`GpuSceneSync`, inside `RenderSceneToViewport`), `:1760` (`PrepareSceneForRender`), `:2067` and `:2075` (`PrepareSceneForRender` and `GpuSceneSyncNode`, inside `ArmGraphViewportFrame` at `:2042`).

A line that is only `GpuSceneInvalidate(reg);` inside `RenderGraph` has zero hits. A line `GpuSceneInvalidate(reg); reg.CreateView<const Transform>();` inside `RenderGraph` has one hit. A line `PickPassIdOf(e); reg.GetComponent<const Transform>(e);` inside `ArmGraphViewportFrame` has one hit. A body whose only tokens are inside `//` and `/* */`, plus a `GpuSceneSyncNode()` call, has zero hits. A body with `GpuSceneSync(reg)` has one hit. Forbidden tokens: `CreateView`, `GetComponent`, `GetResource`, `InspectEntity`, `ActiveSceneCamera`, `ActivePerspectiveSceneCamera`, `CollectPickables`, `PrepareSceneForRender`, `BuildVisibleSet`, `GpuSceneSync`. `CVarRegistry` and `Settings<` are not hits. Reads outside these seven functions stay legal, including the gizmo interaction reads at `:1241`, `:1283`, and `:1339`.

- [ ] **Step 1: Write the failing test.**

`ArcaneTests/src/RenderRegistryIncludeTest.cpp`. Resolve `cl.exe` inside the test. MSBuild's absolute path does not put `cl` on PATH for a later process whose cwd is the exe dir.

```cpp
#include <catch2/catch_test_macros.hpp>
#include "Helpers/ConstantScan.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstdio>

static std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string Capture(const std::string& command)
{
    FILE* pipe = _popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string output;
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        output += buf;
    _pclose(pipe);
    return output;
}

struct ClTool
{
    std::string vcvars;
    std::string cl;
};

static ClTool ResolveCl()
{
    const char* pfx86 = std::getenv("ProgramFiles(x86)");
    REQUIRE(pfx86 != nullptr);
    const std::filesystem::path vswhere = std::filesystem::path(pfx86)
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    REQUIRE(std::filesystem::exists(vswhere));
    const std::string vs = Capture(
        "\"" + vswhere.string() + "\" -latest -prerelease -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");
    std::string install = vs;
    while (!install.empty() && (install.back() == '\n' || install.back() == '\r'))
        install.pop_back();
    REQUIRE(!install.empty());
    ClTool tool;
    tool.vcvars = (std::filesystem::path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").string();
    REQUIRE(std::filesystem::exists(tool.vcvars));
    const std::filesystem::path msvc = std::filesystem::path(install) / "VC" / "Tools" / "MSVC";
    std::string best;
    for (const auto& entry : std::filesystem::directory_iterator(msvc))
        if (entry.is_directory() && entry.path().filename().string() > best)
            best = entry.path().filename().string();
    REQUIRE(!best.empty());
    tool.cl = (msvc / best / "bin" / "Hostx64" / "x64" / "cl.exe").string();
    REQUIRE(std::filesystem::exists(tool.cl));
    return tool;
}

static std::string GrabTag(const std::string& xml, const char* tag)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const std::size_t a = xml.find(open);
    const std::size_t b = xml.find(close, a);
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    return xml.substr(a + open.size(), b - (a + open.size()));
}

static int ShowIncludes(const ClTool& tool, const std::filesystem::path& tu, std::string& output)
{
    const auto vcx = Arcane::Test::RepoRoot() / "ArcaneClient" / "ArcaneClient.vcxproj";
    const std::filesystem::path projectDir = vcx.parent_path();
    const std::string xml = ReadAll(vcx);
    const std::string includes = GrabTag(xml, "AdditionalIncludeDirectories");
    const std::string defines = GrabTag(xml, "PreprocessorDefinitions");

    std::string cmd = "cmd /c \"call \"" + tool.vcvars + "\" >nul && \"" + tool.cl
        + "\" /nologo /showIncludes /Zs /std:c++latest /EHsc /MD /Zc:__cplusplus ";
    std::stringstream def(defines);
    std::string item;
    while (std::getline(def, item, ';'))
    {
        if (item.empty() || item.find('%') != std::string::npos)
            continue;
        cmd += "/D" + item + " ";
    }
    std::stringstream inc(includes);
    while (std::getline(inc, item, ';'))
    {
        if (item.empty())
            continue;
        const std::string projectToken = "$(ProjectDir)";
        const std::string solutionToken = "$(SolutionDir)";
        if (item.find(projectToken) != std::string::npos)
            item.replace(item.find(projectToken), projectToken.size(), projectDir.string() + "\\");
        if (item.find(solutionToken) != std::string::npos)
            item.replace(item.find(solutionToken), solutionToken.size(),
                         Arcane::Test::RepoRoot().string() + "\\");
        REQUIRE(item.find("$(") == std::string::npos);
        std::filesystem::path path(item);
        if (path.is_relative())
            path = projectDir / path;
        cmd += "/I\"" + path.string() + "\" ";
    }
    cmd += "\"" + tu.string() + "\" 2>&1\"";
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        output += buf;
    return _pclose(pipe);
}

static bool RegistryHit(const std::string& output)
{
    return output.find("Astra\\Registry\\Registry.hpp") != std::string::npos
        || output.find("Astra/Registry/Registry.hpp") != std::string::npos;
}

static bool EcsHit(const std::string& output)
{
    return output.find("Arcane\\Ecs.hpp") != std::string::npos
        || output.find("Arcane/Ecs.hpp") != std::string::npos;
}

TEST_CASE("render TUs preprocess Registry.hpp only from ExtractFrame.cpp", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto root = Arcane::Test::RepoRoot() / "ArcaneClient/src/Arcane/Render";
    bool sawExtract = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.path().extension() != ".cpp")
            continue;
        std::string output;
        const int code = ShowIncludes(tool, entry.path(), output);
        INFO(entry.path().string());
        INFO(output);
        CHECK(code == 0);
        const bool extract = entry.path().filename() == "ExtractFrame.cpp";
        if (extract)
            sawExtract = true;
        if (extract)
            CHECK(RegistryHit(output));
        else
            CHECK_FALSE(RegistryHit(output));
        if (!extract)
            CHECK(ReadAll(entry.path()).find("Astra::Registry") == std::string::npos);
    }
    CHECK(sawExtract);
}

TEST_CASE("a clean include of DepthConvention passes and SceneCamera fails the same check", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto dir = std::filesystem::temp_directory_path();
    const auto passTu = dir / "f5_depth_only.cpp";
    const auto failTu = dir / "f5_scene_camera.cpp";
    {
        std::ofstream o(passTu);
        o << "#include <Arcane/Scene/DepthConvention.hpp>\n";
    }
    {
        std::ofstream o(failTu);
        o << "#include <Arcane/Scene/SceneCamera.hpp>\n";
    }
    std::string passOut, failOut;
    CHECK(ShowIncludes(tool, passTu, passOut) == 0);
    CHECK_FALSE(RegistryHit(passOut));
    ShowIncludes(tool, failTu, failOut);
    CHECK(RegistryHit(failOut));
    std::filesystem::remove(passTu);
    std::filesystem::remove(failTu);
}

TEST_CASE("cache and debug-draw TUs preprocess neither Registry.hpp nor Ecs.hpp", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto root = Arcane::Test::RepoRoot() / "ArcaneClient/src/Arcane/Render";
    const char* names[] = {
        "MeshCache.cpp", "MeshMaterialCache.cpp", "SpriteCache.cpp", "PhysicsDebugDraw.cpp" };
    for (const char* name : names)
    {
        std::string output;
        const int code = ShowIncludes(tool, root / name, output);
        INFO(name);
        INFO(output);
        CHECK(code == 0);
        CHECK_FALSE(RegistryHit(output));
        CHECK_FALSE(EcsHit(output));
    }
}
```

Host gate, same file:

```cpp
static std::string FunctionBody(const std::string& text, const std::string& signature)
{
    const std::size_t sig = text.find(signature);
    if (sig == std::string::npos) return {};
    const std::size_t open = text.find('{', sig);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0)
            return text.substr(open, i - open + 1);
    }
    return {};
}

static std::string Strip(std::string row, const char* token)
{
    for (;;)
    {
        const std::size_t at = row.find(token);
        if (at == std::string::npos)
            return row;
        row.erase(at, std::char_traits<char>::length(token));
    }
}

static bool IdentChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
}

static std::string StripComments(const std::string& body)
{
    std::string out;
    out.reserve(body.size());
    enum class Mode { Code, Line, Block, String, Char };
    Mode mode = Mode::Code;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        const char n = (i + 1 < body.size()) ? body[i + 1] : '\0';
        if (mode == Mode::Code)
        {
            if (c == '/' && n == '/')      { mode = Mode::Line;  out.push_back(' '); ++i; }
            else if (c == '/' && n == '*') { mode = Mode::Block; out.push_back(' '); ++i; }
            else if (c == '"')             { mode = Mode::String; out.push_back(c); }
            else if (c == '\'')            { mode = Mode::Char;   out.push_back(c); }
            else out.push_back(c);
        }
        else if (mode == Mode::Line)
        {
            if (c == '\n') { mode = Mode::Code; out.push_back('\n'); }
            else out.push_back(' ');
        }
        else if (mode == Mode::Block)
        {
            if (c == '*' && n == '/') { mode = Mode::Code; out.push_back(' '); ++i; }
            else if (c == '\n') out.push_back('\n');
            else out.push_back(' ');
        }
        else
        {
            out.push_back(c);
            if (c == '\\' && n) { out.push_back(n); ++i; }
            else if ((mode == Mode::String && c == '"') || (mode == Mode::Char && c == '\''))
                mode = Mode::Code;
        }
    }
    return out;
}

static bool WholeWord(const std::string& row, const char* token)
{
    const std::size_t n = std::char_traits<char>::length(token);
    for (std::size_t at = 0; (at = row.find(token, at)) != std::string::npos; at += n)
    {
        const bool left  = at > 0 && IdentChar(row[at - 1]);
        const bool right = at + n < row.size() && IdentChar(row[at + n]);
        if (!left && !right)
            return true;
    }
    return false;
}

static int HostGateHits(const std::string& text, const std::string& signature)
{
    const std::string raw = FunctionBody(text, signature);
    if (raw.empty()) return -1;
    const std::string body = StripComments(raw);
    const char* tokens[] = {
        "CreateView", "GetComponent", "GetResource", "InspectEntity",
        "ActiveSceneCamera", "ActivePerspectiveSceneCamera", "CollectPickables",
        "PrepareSceneForRender", "BuildVisibleSet", "GpuSceneSync"
    };
    const bool stripInvalidate = signature == "RenderGraph("
        || signature == "EditorApp::RenderSceneToViewport(";
    const bool stripPick = signature == "EditorApp::ArmGraphViewportFrame(";
    int hits = 0;
    std::size_t line = 0;
    while (line < body.size())
    {
        const std::size_t end = body.find('\n', line);
        std::string row = body.substr(line, (end == std::string::npos ? body.size() : end) - line);
        if (stripInvalidate) row = Strip(row, "GpuSceneInvalidate");
        if (stripPick) row = Strip(row, "PickPassIdOf");
        for (const char* token : tokens)
            if (WholeWord(row, token))
                ++hits;
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return hits;
}

TEST_CASE("stripping a call name does not exempt the rest of the line", "[registry-gate]")
{
    const std::string mixed =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg); reg.CreateView<const Arcane::Transform>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(mixed, "RenderGraph(") == 1);
    const std::string only =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(only, "RenderGraph(") == 0);
    const std::string pick =
        "void EditorApp::ArmGraphViewportFrame()\n{\n    PickPassIdOf(e); reg.GetComponent<Arcane::Transform>(e);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(pick, "EditorApp::ArmGraphViewportFrame(") == 1);
}

TEST_CASE("comments and a longer identifier are not host-gate hits", "[registry-gate]")
{
    const std::string comments =
        "void RenderGraph()\n{\n"
        "    // CollectPickables\n"
        "    /* PrepareSceneForRender\n       GpuSceneSync */\n"
        "    GpuSceneSyncNode();\n"
        "}\nvoid Next(){}\n";
    CHECK(HostGateHits(comments, "RenderGraph(") == 0);
    const std::string inString =
        "void RenderGraph()\n{\n    const char* kept = \"// CreateView\";\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(inString, "RenderGraph(") == 1);
    const std::string call =
        "void RenderGraph()\n{\n    GpuSceneSync(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(call, "RenderGraph(") == 1);
    const std::string onlyNode =
        "void RenderGraph()\n{\n    GpuSceneSyncNode();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(onlyNode, "RenderGraph(") == 0);
}

TEST_CASE("named frame builders do not read the registry", "[registry-gate]")
{
    const std::string runtime = ReadAll(Arcane::Test::RepoRoot() / "ArcaneRuntime/src/RuntimeFrame.cpp");
    const std::string editor = ReadAll(Arcane::Test::RepoRoot() / "ArcaneEditor/src/App/EditorAppFrame.cpp");
    const std::string harvest = ReadAll(Arcane::Test::RepoRoot() / "ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp");
    const std::string preview = ReadAll(Arcane::Test::RepoRoot() / "ArcaneEditor/src/Documents/ShaderEditorDocument.cpp");
    CHECK(HostGateHits(runtime, "RenderGraph(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::RefreshSceneResolution(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::RenderSceneToViewport(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::SubmitSceneToBatcher(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::ArmGraphViewportFrame(") == 0);
    CHECK(HostGateHits(harvest, "MaterialPreviewHarvester::Impl::Harvest(") == 0);
    CHECK(HostGateHits(preview, "RenderGraphPreview(") == 0);
}
```

- [ ] **Step 2: Generate and run the gate.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[registry-gate]"
```

Expected: FAIL. `MeshCache.cpp`, `MeshMaterialCache.cpp`, `SpriteCache.cpp`, and `PhysicsDebugDraw.cpp` each report `Astra/Registry/Registry.hpp` (through `SceneResources.hpp` -> `Ecs.hpp:23`) and `Arcane/Ecs.hpp`. The comment-token case passes: it only exercises the scanner in this file. The planted `SceneCamera.hpp` TU still reports `Registry.hpp`. The planted mixed `GpuSceneInvalidate` + `CreateView` line reports one hit, the comment body reports zero, and `GpuSceneSyncNode()` reports zero. The cwd of this command is the exe dir; `ResolveCl` is what finds the compiler.

If a `.cpp` outside the table is the failure, stop. The table is the closed set. Do not add an allow-list entry, and do not add a step whose file is chosen by the compiler output. The four named TUs are in the table. Fixing them is the next step, not a stop.

- [ ] **Step 3: Split the cache records out of `SceneResources.hpp`.**

Do the move, the include retarget, and the three comment updates named in the Interfaces section. `SceneResources.hpp` includes `SceneCacheRecords.hpp`. The four TUs include `SceneCacheRecords.hpp` and do not include `SceneResources.hpp`. Header-only: no new `.cpp`, and no vcxproj source row. The `/showIncludes` check is what proves the split.

- [ ] **Step 4: Re-run the gate.**

```powershell
$env:_APH_NOPAUSE = '1'
cmd /c GenerateProjects.bat
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[registry-gate]"
```

Expected: PASS. `ExtractFrame.cpp` is the one `Astra/Registry/Registry.hpp` hit. Each of the four named TUs reports neither `Registry.hpp` nor `Ecs.hpp`, and each compile returns 0. The planted `SceneCamera.hpp` TU still reports that path.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneCore/src/Arcane/Scene/SceneCacheRecords.hpp ArcaneCore/src/Arcane/Scene/SceneResources.hpp ArcaneClient/src/Arcane/Render/MeshCache.hpp ArcaneClient/src/Arcane/Render/MeshMaterialCache.hpp ArcaneClient/src/Arcane/Render/SpriteCache.hpp ArcaneClient/src/Arcane/Render/SpriteCache.cpp ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.cpp ArcaneTests/src/RenderRegistryIncludeTest.cpp
git commit -m "test(render): showIncludes and host frame-builder gates" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

`SpriteCache.cpp` is in that list because the comment at `:48` names the header `SpriteEntry` moved out of. `MeshCache.cpp` is not: the comment that names that header is `MeshCache.hpp:38`, already in the list. If a file named in the table still had to change for the gate to pass, add that path too. Do not add an unrelated file. Do not add an allow-list entry.

---

### Task 13: Section 7.1 golden gate

**Files:**
- Modify: only the blessed PNGs the grid/overlay class actually moves, under `ReferenceProject/Verify/References/`. World lanes are not blessed.

**Interfaces:**
- Consumes: the frame from Tasks 1–12. `scripts/golden-gate.ps1` lanes (`:345-393`): `runtime-scene` and `f3-cull-blend` (backend-split; `--scene 7e5a0030-0030-4030-8030-000000000030` for f3), `editor-ui`, `editor-ui-perspective` (`--view-mode perspective`), `editor-input-doc`, `editor-asset-page`, `editor-material-page`. The gate does not rebuild the engine. Comparator success is `diffCount == 0`.
- Produces: a green Debug gate and a green Release gate, a Debug game DLL in the slot, and a commit whose message contains the re-bless note below.

- [ ] **Step 1: Run the Debug gate.**

`arcbuild.exe` exists from the Debug engine build in Task 1.

```powershell
$env:CL = '/FS'
& "D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe" build --project ReferenceProject --config Debug --sdk D:\dev\starworks\Arcane-f5
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneRuntime\imgui.ini
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneEditor\imgui.ini
Set-Location D:\dev\starworks\Arcane-f5
powershell -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: `runtime-scene` and `f3-cull-blend` report `diffCount == 0` on both backends. `editor-ui` and `editor-ui-perspective` report a diff on the grid, and on physics-debug or camera-rect pixels when the frame contains them. Open the diff images the gate writes and confirm the changed pixels are that class. Bless a pixel only when that primitive is present in the new frame and its colour changed because it composites after tonemap. A 2D grid, 3D grid, physics-debug line, or camera rect that is absent — scene or clear colour where that primitive used to draw — is a failure: stop, and leave the PNG blessed as it was. A world sprite or world mesh pixel in the diff stops the task: fix the earlier task, and leave the PNG blessed as it was. A gizmo pixel in the diff stops the task: a display-referred gizmo colour went through ACES. Fix that path and leave the gizmo PNG blessed as it was. An ImGui chrome or widget pixel in the diff stops the task: that class is not the grid. Fix the earlier task and leave the PNG blessed as it was.

`editor-input-doc` and `editor-material-page` cover the viewport with a document tab. They have no pre-authorized bless. If their `diffCount` is 0, leave them. If a pixel moved and the diff image is the same grid/overlay class, it may be blessed with the same note. Any other diff stops the task.

`editor-asset-page` leaves the viewport visible. A grid diff there is the same class. A mesh-thumbnail or HUD diff stops the task.

Do not bless `inprocess-lit-cube`. If `ARCANE_THUMBS` images moved, stop.

- [ ] **Step 2: Re-bless the grid/overlay class from the staged slot, then copy back.**

From the host exe directory so `--project` resolves to the staged tree. One dx12 bless covers a shared slot. `editor-ui` and `editor-ui-perspective` are shared (`golden-gate.ps1:347-354`).

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneEditor
.\ArcaneEditor.exe --backend dx12 --compare editor-ui --bless
.\ArcaneEditor.exe --backend dx12 --compare editor-ui-perspective --view-mode perspective --bless
```

Add the same `--bless` pair for `editor-asset-page` only if Step 1 showed a grid-class diff. Immediately copy the blessed PNGs back to source:

```powershell
$src = "D:\dev\starworks\Arcane-f5\ReferenceProject"
$staged = "D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneEditor"
Copy-Item "$staged\Verify\References\editor-ui.png" "$src\Verify\References\editor-ui.png" -Force
Copy-Item "$staged\Verify\References\editor-ui-perspective.png" "$src\Verify\References\editor-ui-perspective.png" -Force
git -C D:\dev\starworks\Arcane-f5 status --short -- ReferenceProject/Verify/References
```

`git status` lists those PNGs. A bless that wrote a shared lane under `vulkan\` is the wrong slot. Shared lanes bless `References\<name>.png` only. Leave that vulkan file out of the copy.

- [ ] **Step 3: Re-run Debug, then Release, then put the slot back on Debug.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5
powershell -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: every lane `diffCount == 0`, both backends.

Release, with no second bless:

```powershell
$env:CL = '/FS'
& "D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe" rebuild --project ReferenceProject --config Release --sdk D:\dev\starworks\Arcane-f5
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Release /m:8 /nodeReuse:false /v:m
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Release-windows-x86_64-md\ArcaneRuntime\imgui.ini
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Release-windows-x86_64-md\ArcaneEditor\imgui.ini
powershell -File scripts\golden-gate.ps1 -Configuration Release
```

Expected: PASS with no bless. World lanes still `diffCount == 0`. Editor lanes match the PNGs blessed in Debug.

End on a Debug game DLL. Use whichever `arcbuild.exe` exists after the Release engine build:

```powershell
& "D:\dev\starworks\Arcane-f5\bin\Release-windows-x86_64-md\arcbuild\arcbuild.exe" rebuild --project ReferenceProject --config Debug --sdk D:\dev\starworks\Arcane-f5
```

Do not msbuild Release again after this. The slot `ReferenceProject\Binaries\ReferenceGame.dll` is the Debug CRT.

Also run the witness that pins `f3-cull-blend` counts, from the Debug exe dir, after a Debug engine rebuild if the Debug test exe is older than the Release engine just built (game module first, then `Arcane.slnx` Debug):

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[witness]"
```

`W5` stays `diffCount == 0`.

- [ ] **Step 4: Commit the PNGs and the note.**

The commit message body is the note, verbatim:

```
F5 plan 1: the editor grid, physics-debug lines, and the camera rect leave the linear canvas and composite after the tonemap. A blend into linear colour before the nonlinear tonemap differs from running the tonemap operator on the primitive alone and blending after, including when the post chain is empty. editor-ui and editor-ui-perspective include the boot scene's post process (main.arcscene:27-31). The 2D grid also leaves batcher layer 0, so a grid line that crosses a sprite is in this class. World sprites and world meshes are not in this class. The gizmo was already outside the post chain and its pixels stay. ImGui chrome and widget pixels stay.
```

```powershell
git add ReferenceProject/Verify/References
git commit -m "test(render): F5 plan 1 golden gate, explained grid re-bless" -m "F5 plan 1: the editor grid, physics-debug lines, and the camera rect leave the linear canvas and composite after the tonemap. A blend into linear colour before the nonlinear tonemap differs from running the tonemap operator on the primitive alone and blending after, including when the post chain is empty. editor-ui and editor-ui-perspective include the boot scene's post process (main.arcscene:27-31). The 2D grid also leaves batcher layer 0, so a grid line that crosses a sprite is in this class. World sprites and world meshes are not in this class. The gizmo was already outside the post chain and its pixels stay. ImGui chrome and widget pixels stay." -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

If a lane did not change, it will not appear in `git status`. Add only the PNGs that changed. Do not add `imgui.ini`.

---

## Spec coverage

| Spec | Task |
|---|---|
| 8.1 declarative clear on `batch2d`, `mesh` loads colour, depth clears to the convention, `CmdClearAttachments` gone for those two planes | 1, 2 |
| 8.1 world-node order unchanged; sprites stay in `batch2d`; meshes stay in the mesh pass | 2, 5, 6, 13 |
| 8.1 overlay after tonemap; 3D grid folds in; the bound packet is what the pass reads; screen UI after the overlay | 3, 9 |
| 8.1 extract + packet; section 4.2 reads move; 4.3 and 4.4 gates | 4, 5, 6, 7, 8, 9, 10, 12 |
| 8.1 `Arcane::Runtime` and ArcaneServer skip extract; `RuntimeApp --headless` runs it | 11 |
| 3.1 depth table, ForwardZ selected, reverse-Z unselected | 1 |
| 3.1 overlay classes, exposure excluded, empty-chain re-bless is not identity | 3, 9, 13 |
| 3.1 chrome graph has no world pass and no overlay | 2 |
| 3.2 G-buffer, lighting, TAA declared and empty. Forward world stays the mesh node. Translucent-world list is plan 3 | 2 |
| 3.3 render origin field exists and is 0; no rebase | 4, 5 |
| 3.4 overlay samples world-pass depth; missing depth draws unoccluded; perspective grid depth reads `SV_Position.z` | 3 |
| 3.6 plans 1–2 keep today's camera split; hosts stop calling the two sweeps | 7, 12 |
| 3.7 server skip; rendering `--headless` runs extract | 11 |
| 4.1 packet fields, `FramesInFlight()` slots, GPU reads the upload ring; the host loop writes `selectedIds` and the outline reads it | 4, 5, 8, 10 |
| 4.3 / 4.4 include gate, cache-header split, and host frame-builder gate | 8, 12 |
| 7.1 world pixels identical; grid class re-blessed with a written note | 13 |
