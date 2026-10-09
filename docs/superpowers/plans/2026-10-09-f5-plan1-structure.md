# F5 Plan 1 — Structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the frame a declarative clear, a post-tonemap overlay pass, and one extract stage that publishes an immutable `RenderFramePacket`, while world sprites and world meshes stay pixel-identical.

**Architecture:** Plan 1 does not merge the two world nodes. `batch2d` stays the first world node and clears the canvas with its colour load-op; `mesh` stays next and loads colour while its depth load-op clears to the one depth-convention value. Empty G-buffer, lighting, and TAA nodes are declared and record nothing. After tonemap, one overlay pass samples scene depth and draws the 2D grid, the folded 3D grid, physics debug, the camera rect, and the gizmo. Extract, on the game thread, is the only render-side registry reader; it fills a packet ring of `FramesInFlight()` slots and returns before any GPU work. The GPU never reads packet memory.

**Tech Stack:** C++23, premake5, MSBuild (VS 2026), Catch2 v3, NRI (D3D12 + Vulkan), HLSL via DXC, Astra ECS, PowerShell.

**Spec:** `docs/superpowers/specs/2026-10-09-f5-unified-world-pipeline-design.md`. This plan implements section 8.1 and every ruling that lands in plan 1 (sections 3.1–3.4, 3.7, 4.1–4.4, 7.1). Where this plan and the spec disagree, the spec wins; stop and report.

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

Pass `--sdk` explicitly. Do not rely on `ARCANE_SDK` pointing at `D:\dev\starworks\Arcane`. The first-ever build of this worktree has no `arcbuild.exe` until ArcaneClient has been built once: build the engine **Debug** once, then the per-config order above applies. After a config flip, `arcbuild rebuild` (not `build`). The last command of Task 7 is `arcbuild rebuild --config Debug`, so the slot ends as a Debug DLL.

- **Tests run from the exe directory:**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\<Cfg>-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[tag]"
```

Record the `Randomness seeded to:` line. GPU tests are tagged `[gpu]`. Device-less graph tests are `[nri]`. `Arcane::Test::RepoRoot()` is `ArcaneTests/src/Helpers/ConstantScan.hpp:34`.

- **Goldens are Task 7 only.** Tasks 1–6 do not run `scripts\golden-gate.ps1`. After Task 3 the editor grid pixels differ from the blessed PNGs on purpose; `editor-ui` stays red until Task 7's explained re-bless. World lanes (`runtime-scene`, `f3-cull-blend`) must stay `diffCount == 0` with no re-bless. Before any golden run, delete `imgui.ini` in both host exe dirs (`bin\<Cfg>-windows-x86_64-md\ArcaneRuntime` and `ArcaneEditor`). Re-bless only the grid/overlay class, and only with the written note in Task 7. Bless from the host exe directory (the project resolves to the staged copy), then immediately copy the blessed PNGs from the staged `Verify\References` into source `ReferenceProject\Verify\References`. `git status` must show those PNGs. Gate Debug and Release. Do not bless again under Release. Do not bless `inprocess-lit-cube` (it has no bless mode). If mesh thumbnails move, stop: the tonemap include refactor must stay pixel-identical on world content.

- **Nothing on the frame path blocks the main thread.** Extract returns before `RenderFrame` / `RenderFrameOffscreen`. The `/showIncludes` compile is a Catch2 test, not a per-frame step. No `DeviceWaitIdle` is added.

- **Packets and the GPU.** `kMaxFramesInFlight` is 3 (`FramePacing.hpp:38`). `FramesInFlight()` is the latched depth (`FramePacing.hpp:42`). `render.framesInFlight` defaults to 2 and its range is 2..3 (`RenderDeviceSettings.hpp:80`, `:109`). The packet ring stores `kMaxFramesInFlight` slots and uses the live `FramesInFlight()` depth. The GPU reads `NriUploadRing` copies (`NriUploadRing.hpp:21`, `:135`, `NriUploadRing.cpp:102`), never packet memory.

## Review Focus

These are the five inputs most likely to bite that a task's other tests would miss. Each one has a pinning test in the task named, written in that task's step style.

1. **The chrome graph declares a world pass.** `PresentChromeFrame` (`EditorAppFrame.cpp:4016-4035`) sets only `frame.imgui` today, and `DeclareGraphFrame` always starts with `batch2d` (`NriGraphContext.cpp:1277`), so an imgui-only shape clears a canvas through an empty batcher and then tonemaps it (`RenderGraphTest.cpp:6177-6220`). Plan 1 adds `RgFrameShape::world` / `FrameDesc::world`, default `true`. Chrome sets `frame.world = false` and gets a letterbox raster (colour load-op CLEAR to `render.clearColor`, no draws), then tonemap, then imgui. Forbidden names on that shape: `batch2d`, `mesh`, `grid`, `overlay`, `gbuffer`, `lighting`, `taa`, `gpuscene-sync`, `mesh-cull`. An empty-batcher *world* frame still declares `batch2d`. Pinned in Task 2 (`"chrome world=false is letterbox, tonemap, imgui"` and `"PresentChromeFrame sets frame.world = false"`).
2. **A frame still in flight holds the previous packet while extract writes the slot `FramesInFlight()` extracts old.** The GPU never reads packet memory. After `FramesInFlight() - 1` further extracts the first packet's bytes are intact; the next extract overwrites that slot. A span taken from the packet lies inside the packet's own vector. Mutating the source component after extract leaves the packet unchanged. Render origin translation is `{0,0,0}`. The packet header does not include `Astra/Registry/Registry.hpp`. Pinned in Task 4 (`"a packet slot stays intact until FramesInFlight extracts have passed"`).
3. **An empty post chain is not pixel-identity for a blended overlay, and a post chain does not grade an opaque overlay primitive.** Blending a partial-alpha linear colour into scene colour and then running the tonemap differs from running the tonemap operator on the primitive and blending after, including when the chain is empty (C3). Separately, an opaque overlay primitive (alpha 1) reads the same bytes with a scene-tinting post chain and without one, while a world pixel in that same pair of frames differs. Pinned in Task 3 (`"tonemap-then-blend differs from blend-then-tonemap"` and `"an opaque overlay pixel is unchanged by a post chain"`). Task 7 carries the explained re-bless note; it does not claim the empty-chain grid matches today's golden.
4. **A headless runtime draws, so it runs extract; ArcaneServer records no graph, so it does not.** `RuntimeFrame.cpp:748` branches `IsOffscreen() ? RenderFrameOffscreen : RenderFrame`. The one `ExtractFrame` call sits above that branch. `ArcaneServer` links `ArcaneCore` only (`premake5.lua:785-792`). Pinned in Task 5 (`"runtime calls ExtractFrame once, above both render paths"` and `"ArcaneServer does not name ExtractFrame"`).
5. **Overlay class, colour space, and a missing depth target.** Each primitive is depth-tested or always-on-top (spec section 3.1). A depth-tested line behind an opaque mesh is hidden; the same line tagged always-on-top is visible. The 2D grid is depth-tested and, because sprites write no depth in this plan, draws over sprites (that crossing is part of the Task 7 note). The 3D grid uses nearer-or-equal and does not write depth. Physics debug does not call `SetLayer` today (`EditorAppFrame.cpp:1893-1981`) and must be tagged always-on-top explicitly. Gizmo colours are already display-referred (`GizmoOverlay.hpp:26-45`, `ColorConvertFloat4ToU32`); a display-referred red `(1,0,0)` reads back unscaled and is not `ACES(1,0,0)`. Linear primitives go through ACES then true sRGB with no exposure term (`tonemap.hlsl:36-56`). With no mesh pass, today's grid comment (`NriGraphContext.cpp:1374-1378`) draws unoccluded and does not attach the placeholder depth; the overlay does the same and does not sample a missing texture. Pinned in Task 3 (`"depth-tested hides, always-on-top shows, display-referred red is not ACES, missing depth draws unoccluded"`).

## Out of scope

Do not implement any of these in this plan. A task that starts one of them is wrong; stop.

- Plan 2: Surface domain, `.arcmat` migration, vertex factories, the six mesh shaders becoming one material, preview rows, checkerboard moving off the world quad. The checkerboard stays a world quad (spec section 5.7). `Impl::Harvest` and `ShaderEditorDocument::RenderGraphPreview` contain no forbidden registry calls today; they must not grow any, and they do not have to call `ExtractFrame` when they author a local quad.
- Plan 3: one world raster pass, the shared back-to-front sprite list, sprite depth writes, quantized sort keys, sprite texture/cutoff, the 50k budget, sprite-edge parity, `Camera::primary`, editing `main.arcscene` or `f3_cull_blend.arcscene`, the perspective occlusion witness, the view-space slab witness, the `runtime-scene` two-mesh re-bless.
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
| `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp` | 1 | Include the new header immediately above the DEPTH CONVENTION comment (`:120`). `ActiveSceneCamera` (`:57`) and `ActivePerspectiveSceneCamera` (`:206`) stay. |
| `ArcaneClient/src/Arcane/Render/DepthConventionMap.hpp` (new) | 1 | `DepthClearValue`, `NearerWinsOp`, `NearerOrEqualOp`. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp` | 1, 2 | Compare at `:832` reads `NearerWinsOp`. Depth clear sites `:83`, `:955`, `:1309` read `DepthClearValue` in Task 2. `CmdClearAttachments` at `:942-957` deleted in Task 2. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp` | 1 | Compare at `:582` reads `NearerWinsOp`. `:400` stays. |
| `ArcaneTests/src/DepthConventionTest.cpp` (new) | 1 | Mapping checks and the source scan of the two compare sites. |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp` | 2 | `RgLoadOp`, `RgAttachmentOp`, setters, queries. `NodeDesc` (`:852-866`). |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.cpp` | 2 | Store the ops beside `SetColorAttachments` (`:211` region). |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp` | 2 | Fill `nri::AttachmentDesc` (`:1368-1382`) from the ops. |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` | 2, 3 | `FrameDesc::world`, `RgFrameShape::world` (default true). Task 3 adds the overlay list pointer. |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp` | 2, 3 | `DeclareGraphFrame` (`:1231`) and `BuildFrame` (`:1632-1664`). |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp` | 2, 5 | Delete the colour `CmdClearAttachments` (`:1395-1408`). Task 5 drains the packet's owned copy. |
| `ArcaneTests/src/RenderGraphTest.cpp` | 2, 3 | Every `DeclareGraphFrame` node census. Task 3 moves the grid census to `overlay`. |
| `ArcaneEditor/src/App/EditorAppFrame.cpp` | 2, 3, 5 | `PresentChromeFrame` (`:4016`) sets `world = false` in Task 2. Task 3 retargets grid/physics/camera/gizmo submissions. Task 5 moves the registry reads into extract. |
| `ArcaneClient/src/Arcane/Render/GridSceneDesc.hpp` (new) | 3 | `GridSceneDesc` moved out of `GridNode.hpp:95-155`. No NRI, no Astra. |
| `ArcaneClient/src/Arcane/Render/OverlayPrimitive.hpp` (new) | 3 | Overlay enums and primitive records. |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/OverlayNode.hpp`, `.cpp` (new) | 3 | Node `"overlay"`. |
| `data/shaders/tonemap_operator.hlsl` (new), `data/shaders/tonemap.hlsl`, `data/shaders/overlay.hlsl` (new), `data/shaders/compile-shaders.bat` | 3 | Shared ACES + sRGB. Overlay entry points. |
| `ArcaneTests/src/OverlayWitnessTest.cpp` (new) | 3 | Review Focus 3 and 5. |
| `ArcaneClient/src/Arcane/Render/VisibleSet.hpp` (new) | 4 | `VisibleEntry`, `VisibleSet`. No `Registry.hpp`. |
| `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp` | 4 | Includes `VisibleSet.hpp`. `BuildVisibleSet` (`:119`) stays here. |
| `ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp`, `.cpp` (new) | 4 | Packet, `PacketRing`. |
| `ArcaneTests/src/RenderFramePacketTest.cpp` (new) | 4 | Review Focus 2. |
| `ArcaneClient/src/Arcane/Render/ExtractFrame.hpp`, `.cpp` (new) | 5 | The one allowlisted registry TU. |
| `ArcaneClient/src/Arcane/Render/Grid2D.hpp`, `.cpp` (new) | 5 | `PlanGrid2D` / line emission moved from the editor so extract can call it. |
| `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp` | 5 | `BuildGpuSceneFrame` (`:285`) drops `const MeshTable*`. |
| `ArcaneRuntime/src/RuntimeFrame.cpp`, `RuntimeApp.cpp` | 5 | One `ExtractFrame` above `:748`. `PushSceneCamera` (`:846`) asks extract for the view. |
| `ArcaneTests/src/ExtractFrameTest.cpp` (new) | 5 | Owned snapshot, camera split, Review Focus 4. |
| `ArcaneTests/src/RenderRegistryIncludeTest.cpp` (new) | 6 | `/showIncludes` gate and the host frame-builder gate. |
| Golden PNGs under `ReferenceProject/Verify/References` | 7 | Explained grid/overlay re-bless only. |

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

`BuildGpuSceneFrame` in this task still takes `const MeshTable*` (`GpuSceneSync.hpp:285`). Task 5 changes that signature and updates the call below in the same commit as the other call sites.

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
```

`compile-shaders.bat` is an explicit list. The test also reads the bat and requires both lines:

```cpp
CHECK(bat.find("call :compile overlay vs_main vs_6_5 overlay_vs") != std::string::npos);
CHECK(bat.find("call :compile overlay ps_main ps_6_5 overlay_ps") != std::string::npos);
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
        float fragD = input.clip.z / input.clip.w;
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

`grid_vs_main` writes `depthTest = 1`. `grid_ps_main` computes the same linear grid colour `grid.hlsl`'s `ps_main` computes today, then `return float4(LinearToSrgb(ACESFilmic(linear.rgb)), linear.a);`, and it runs the same discard guard as `ps_main` (`g_HasDepth > 0.5 && depthTest > 0.5`). Do not change `grid.hlsl` itself: `AddGridNode`'s direct test still loads `grid_vs` / `grid_ps` (`GridNode.cpp:37-38`) for a linear canvas, and that path is no longer on the frame.

Draw order inside `OverlayNode::Record`: 3D grid (when `hasGrid3d` or `shape.grid`), then lines and fills whose `depth` is `DepthTested`, then lines and fills whose `depth` is `AlwaysOnTop`. Depth write stays off (the pass has no depth attachment). Blend is straight alpha, source-over, on the tonemap target.

`OverlayNode::Create` follows `GridNode::Create` (`GridNode.cpp:73`) for the pipeline layout: one cbuffer, one optional depth texture. `NriGraphContext`'s constructor (`:470`, `m_grid = GridNode::Create`) also constructs `OverlayNode`. `BuildFrame` copies `shape.overlay = frame.overlay` and still copies `shape.grid`. `DeclareGraphFrame` deletes the block at `:1392-1397` (`if (shape.grid) AddGridNode(...)`). After `AddTonemapNode` returns `handles.backbuffer` and before gameui (`:1474`):

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

**Editor submissions move in this same commit.** Otherwise the grid disappears until Task 5.

- `SubmitSceneToBatcher` (`:1857`): stop calling `DrawGrid2D` (`:1887`). Build `OverlayLine`s by walking the same world segments `DrawGrid2D` would have emitted (call `DrawGrid2D` into a local `Batcher2D`, `Drain()` it, and convert each line span into `OverlayLine` with `depth = DepthTested`, `space = Linear`, `screenSpace = false`). Store them on `EditorApp` and point `FrameDesc::overlay` at that list from `ArmGraphViewportFrame`. Task 5 replaces this drain with `Grid2D` inside extract; this task only has to keep the lines on screen.
- Physics debug (`:1898-1922`) and the camera rect (`:1953-1981`) stop submitting into the scene batcher. Convert their `Line`/`Circle`/`Rect` calls into `OverlayLine` / `OverlayFill` tagged `AlwaysOnTop`, `Linear`, `screenSpace = false`. Physics debug has no `SetLayer` today; the tag is what keeps it above the grid.
- The gizmo lambda (`DrawViewportPanelPhase`, `:3713`) stops constructing `ImGuiGizmoSink`. It fills `OverlayLine` and `OverlayFill` from the same `Line`/`Triangle`/`Rect`/`Circle` callbacks, tagged `AlwaysOnTop`, `DisplayReferred`, `screenSpace = true`, positions in viewport pixels. The interaction `GetComponent` calls at `:1241`, `:1283`, `:1339` stay. Removing the ImGui sink draw here is what prevents a double draw once Task 5 also emits those primitives.

`FrameDesc::grid` stays the 3D grid desc the editor already fills. The overlay node reads it. Runtime frames leave `overlay` null and `grid` null, so they do not declare the node and `runtime-scene` node order is Task 2's order.

Do not run the golden gate. `editor-ui` and `editor-ui-perspective` will differ on grid pixels. That is expected until Task 7.

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
- Create: `ArcaneClient/src/Arcane/Render/VisibleSet.hpp`
- Modify: `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp:48-84` (the two structs move; `BuildVisibleSet` at `:119` stays)
- Create: `ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp`
- Create: `ArcaneClient/src/Arcane/Render/RenderFramePacket.cpp`
- Test: `ArcaneTests/src/RenderFramePacketTest.cpp`

**Interfaces:**
- Consumes: `VisibleEntry` / `VisibleSet` fields (`VisibilitySystem.hpp:48-84`), `GpuSceneFrame` (`GpuSceneTypes.hpp:275-289`), `Batch2DVertex` (`Batcher2D.hpp:126-132`), `Batch2DDrawSpan` (`:139-157`), `Batch2DDrained` (`:163-188`, non-owning, valid until the next `Begin()`), `GlobalParams` (`ArcaneCore/src/Arcane/Material/GlobalParams.hpp:33-39`), `PickDrawable` (`PickEmit.hpp:46-70`, `Astra::Entity` by value), `OverlayPrimitiveList` from Task 3, `PostChainDesc` (`PostChainCache.hpp:66-78`), `ViewTransform`, `FramesInFlight()`, `kMaxFramesInFlight`.
- Produces:
  - `struct RenderOrigin { glm::vec3 translation{0.0f}; };`
  - `struct ImDrawSnapshot` with owned `std::vector<ImDrawVert> vertices`, `std::vector<ImDrawIdx> indices`, a `struct Cmd { ImVec4 clip; ImTextureID texture; std::uint32_t idxOffset, idxCount, vtxOffset; }`, `std::vector<Cmd> cmds`, and `glm::vec2 displayPos, displaySize, framebufferScale`.
  - `struct RenderFramePacket` with the fields listed in Step 3. No `Astra::Registry` member. The header does not include `Astra/Registry/Registry.hpp`, `SceneCamera.hpp`, `VisibilitySystem.hpp`, or `GridNode.hpp`.
  - `class PacketRing` with `RenderFramePacket& BeginExtract();` and `const RenderFramePacket& Slot(std::uint32_t extractsAgo) const;`.
  - `BeginExtract` returns the slot at `extractCount % FramesInFlight()`, clears that slot, then increments `extractCount`.

`VisibleSet.hpp` includes `Aabb.hpp`, `Frustum.hpp`, `ViewTransform.hpp`, `Astra/Entity/Entity.hpp`, and `<vector>`. It does not include `Registry.hpp`. `VisibilitySystem.hpp` includes `VisibleSet.hpp`, deletes the two struct definitions, and keeps `SceneVisibility`, `MainVisibleSet`, and `BuildVisibleSet`.

- [ ] **Step 1: Write the failing test.**

`ArcaneTests/src/RenderFramePacketTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Render/FramePacing.hpp>

#include "Helpers/ConstantScan.hpp"

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

    struct ImDrawSnapshot
    {
        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx>  indices;
        struct Cmd
        {
            ImVec4        clip{};
            ImTextureID   texture = nullptr;
            std::uint32_t idxOffset = 0;
            std::uint32_t idxCount  = 0;
            std::uint32_t vtxOffset = 0;
        };
        std::vector<Cmd> cmds;
        glm::vec2 displayPos{ 0.0f };
        glm::vec2 displaySize{ 0.0f };
        glm::vec2 framebufferScale{ 1.0f };
    };

    struct RenderFramePacket
    {
        std::optional<ViewTransform> spriteView;
        std::optional<ViewTransform> meshView;
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

Expected: PASS.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/VisibleSet.hpp ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp ArcaneClient/src/Arcane/Render/PostChainDesc.hpp ArcaneClient/src/Arcane/Render/PostChainCache.hpp ArcaneClient/src/Arcane/Render/RenderFramePacket.hpp ArcaneClient/src/Arcane/Render/RenderFramePacket.cpp ArcaneTests/src/RenderFramePacketTest.cpp
git commit -m "feat(render): RenderFramePacket ring sized to FramesInFlight" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 5: ExtractFrame is the only frame-side registry reader

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/ExtractFrame.hpp`
- Create: `ArcaneClient/src/Arcane/Render/ExtractFrame.cpp`
- Create: `ArcaneClient/src/Arcane/Render/Grid2D.hpp`
- Create: `ArcaneClient/src/Arcane/Render/Grid2D.cpp`
- Modify: `ArcaneEditor/src/Viewport/ViewportGrid.hpp` and `ViewportGrid.cpp` (forward `PlanGrid2D` / `DrawGrid2D` to the moved functions)
- Modify: `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp:285-376`
- Modify: `ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp:87-111`
- Modify: `ArcaneClient/src/Arcane/Render/PickEmit.cpp` (move `CollectPickables` at `:32`; keep `BuildPickIdGeometry` at `:243`)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:508-756`
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:846-874`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`RefreshSceneResolution` `:1506-1641`, `RenderSceneToViewport` `:1642`, `SubmitSceneToBatcher` `:1857-1991`, `ArmGraphViewportFrame` `:2042-2181`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1618-1641`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/ImGuiNriNode.cpp:58`
- Modify every `BuildGpuSceneFrame` call: `GpuSceneHost.hpp:110`, `ArcaneTests/src/GpuSceneSyncTest.cpp:499,536,561,617,667,668`, `GpuSceneCullTest.cpp:417,524,570,606`, `MeshCullNodeTest.cpp:67,110,116`, `NriGraphPixelTest.cpp:2221,2301,2358,2387`, and the call `CheckOverlayClasses` makes in `ArcaneTests/src/OverlayWitnessTest.cpp` (Task 3 wrote it against the `const MeshTable*` overload)
- Test: `ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: the packet and `PacketRing` from Task 4. `OverlayPrimitiveList` from Task 3. `ActiveSceneCamera` (`SceneCamera.hpp:57`), `ActivePerspectiveSceneCamera` (`:206`), `BuildVisibleSet` (`VisibilitySystem.hpp:119`), `GpuSceneSync` (`GpuSceneSync.hpp:92`), `BuildGpuSceneFrame` (`:285`), `PrepareSceneForRender` (`GpuSceneHost.hpp:87`), `CollectPickables` (`PickEmit.cpp:32`), `RenderSubmissionSystem` (`RenderSystems.hpp:50-250`), `DrawPhysicsDebug` (`PhysicsDebugDraw.hpp:164`), `ApplyDefaultLight` (`MeshNode.hpp:362`).
- Produces:

```cpp
struct ExtractRequest
{
    glm::uvec2                    viewport{ 0u, 0u };
    std::optional<ViewTransform>  editorView;
    bool                          playMode = false;
    bool                          showGrid2d = false;
    bool                          showGrid3d = false;
    bool                          perspective = false;
    GridSceneDesc                 grid3d{};
    bool                          physicsDebug = false;
    bool                          physicsWholeWorld = false;
    std::optional<std::uint32_t>  selectedEntity;
    OverlayPrimitiveList          gizmoPrims{};
    const PostChainDesc*          post = nullptr;
    ImDrawData*                   imgui = nullptr;
    ImDrawData*                   gameUi = nullptr;
    glm::vec3                     lightDirection{ 0.0f };
    glm::vec3                     lightColor{ 0.0f };
    glm::vec3                     ambient{ 0.0f };
    GpuScene*                     device = nullptr;
};

const RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring);
```

`ExtractFrame.hpp` forward-declares `Astra::Registry` and does not include `Registry.hpp`. `ExtractFrame.cpp` is the one file under `ArcaneClient/src/Arcane/Render/` that includes `Registry.hpp`.

`struct MeshSectionRange { Guid mesh{}; std::uint32_t section = 0; std::uint32_t indexOffset = 0; std::uint32_t indexCount = 0; };`

```cpp
std::vector<MeshSectionRange> MeshSectionRanges(const MeshTable* table);
void BuildGpuSceneFrame(const GpuSceneMirror& m, const VisibleSet* vis,
                        std::span<const MeshSectionRange> sections,
                        const ViewTransform& view, GpuSceneFrame& out);
```

The `const MeshTable*` overload is deleted. Lookup replaces `meshes->Resolve` (`GpuSceneSync.hpp:321-323` and `:360-363`): find the `MeshSectionRange` whose `mesh` and `section` match the batch key. Opaque/masked emit sort (`:349-357`) and the transparent `renderOrder`-then-depth sort (`:380-388`) stay as they are.

- [ ] **Step 1: Write the failing test.**

`ArcaneTests/src/ExtractFrameTest.cpp`. Copy `struct World` from `GpuSceneSyncTest.cpp:32-105` into this file's anonymous namespace (it is not visible outside that cpp). Add:

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
```

`WorldTransform` is a `glm::mat4` (`Components.hpp:87-91`), not a position. Mutate `Transform::position`. `GetComponent` returns a pointer (`CommandStackTest.cpp:231`).

```cpp
TEST_CASE("extract copies one sprite quad and one mesh row, then ignores later edits", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    const Arcane::Guid mesh = world.Mesh(1);
    world.Spawn({ 0.0f, 0.0f, 0.0f }, mesh);
    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    CHECK(packet.origin.translation == glm::vec3(0.0f));
    CHECK(packet.batchVertices.size() == 4u);
    CHECK(packet.mesh.stats.total >= 1u);
    const glm::vec3 copied = packet.batchVertices[0].pos;

    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 50.0f, 50.0f, 0.0f };
    CHECK(packet.batchVertices[0].pos == copied);
    CHECK(packet.spriteView.has_value());
    CHECK(packet.meshView.has_value());
}

TEST_CASE("runtime extract keeps the ortho sprite view and the perspective mesh view", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 200u, 100u };
    // editorView left empty: runtime / Play path.
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    const auto ortho = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    const auto persp = Arcane::ActivePerspectiveSceneCamera(world.reg, req.viewport);
    REQUIRE(ortho.has_value());
    REQUIRE(persp.has_value());
    CHECK(packet.spriteView->view == ortho->view);
    CHECK(packet.meshView->projection == persp->projection);
}

TEST_CASE("an editorView is the world view and is not replaced by a scene camera", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    Arcane::ViewTransform editor = *Arcane::ActiveSceneCamera(world.reg, { 64u, 64u });
    editor.view[3][0] += 3.0f;   // a translation the scene cameras do not have
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    req.editorView = editor;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.spriteView->view == editor.view);
    CHECK(packet.meshView->view == editor.view);
}

TEST_CASE("overlay tags: grid is depth-tested linear, physics camera and gizmo are always on top", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    req.showGrid2d = true;
    req.physicsDebug = true;
    Arcane::OverlayLine gizmo{};
    gizmo.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.lines.push_back(gizmo);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    bool sawGrid = false;
    bool sawGizmo = false;
    for (const Arcane::OverlayLine& line : packet.overlay.lines)
    {
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::DepthTested)
            sawGrid = true;
        if (line.space == Arcane::OverlayColorSpace::DisplayReferred)
        {
            sawGizmo = true;
            CHECK(line.depth == Arcane::OverlayDepthClass::AlwaysOnTop);
        }
    }
    CHECK(sawGrid);
    CHECK(sawGizmo);
    for (const Arcane::OverlayLine& line : packet.overlay.lines)
    {
        if (line.space == Arcane::OverlayColorSpace::Linear && line.screenSpace == false &&
            line.depth != Arcane::OverlayDepthClass::DepthTested)
            CHECK(line.depth == Arcane::OverlayDepthClass::AlwaysOnTop);   // physics / camera rect
    }
}

// Review Focus 4.
TEST_CASE("runtime calls ExtractFrame once, above both render paths", "[nri][extract]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "ArcaneRuntime/src/RuntimeFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t call = text.find("ExtractFrame(");
    REQUIRE(call != std::string::npos);
    CHECK(text.find("ExtractFrame(", call + 1) == std::string::npos);
    const std::size_t presented = text.find("RenderFrame(", call);
    const std::size_t offscreen = text.find("RenderFrameOffscreen(", call);
    CHECK(presented != std::string::npos);
    CHECK(offscreen != std::string::npos);
}

TEST_CASE("ArcaneServer does not name ExtractFrame", "[nri][extract]")
{
    const std::filesystem::path dir = Arcane::Test::RepoRoot() / "ArcaneServer/src";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path());
        std::ostringstream ss;
        ss << in.rdbuf();
        CHECK(ss.str().find("ExtractFrame") == std::string::npos);
    }
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

Expected: FAIL. `ExtractFrame` does not exist.

- [ ] **Step 3: Implement extract and move the reads.**

`ExtractFrame.cpp` includes `Registry.hpp`, `SceneCamera.hpp`, `VisibilitySystem.hpp`, `GpuSceneSync.hpp`, `RenderSystems.hpp`, `PickEmit.hpp`, `Grid2D.hpp`, `PhysicsDebugDraw.hpp`. The function:

1. `RenderFramePacket& packet = ring.BeginExtract();`
2. Views. If `req.editorView` is set, `packet.spriteView` and `packet.meshView` are both that value. Extract does not call `ActiveSceneCamera` to choose the world view on that path. If `req.editorView` is empty, `packet.spriteView = ActiveSceneCamera(reg, req.viewport)` and `packet.meshView = ActivePerspectiveSceneCamera(reg, req.viewport)`. The no-camera warning that `PushSceneCamera` builds with `CreateView<const Camera>` (`RuntimeApp.cpp:870-874`) moves here, still once, still only on the empty-editorView path.
3. `packet.viewport = req.viewport` and `packet.origin.translation = {0, 0, 0}`. Do not add the origin into any row.
4. If `packet.spriteView`, `BuildVisibleSet(reg, *packet.spriteView, packet.visible)`.
5. `GpuSceneSync` into the registry's `GpuSceneMirror` (the body of `PrepareSceneForRender`, `GpuSceneHost.hpp:87-111`). Then `MeshSectionRanges(reg.GetResource<MeshTable>())` and `BuildGpuSceneFrame(..., sections, meshView, packet.mesh)`. The `MeshTable*` does not leave this function.
6. Sprite submission. Run the body of `RenderSubmissionSystem::operator()` (`RenderSystems.hpp:50-250`) into a `Batcher2D` local to this function, `Drain()` it, and copy `vertices`, `indices`, `spans`, `*globals`, and `viewProjection` into the packet. Remove `RenderSubmissionSystem` from the client scheduler registration (`ClientRuntime.cpp`'s include of `RenderSystems.hpp` and the scheduler add) so the host's later `SubmitRender` does not submit those sprites again.
7. `CollectPickables` into `packet.pickDrawables`. Move the function body from `PickEmit.cpp:32` up to but not including `BuildPickIdGeometry` (`:243`) into this cpp. `PickEmit.cpp` keeps `BuildPickIdGeometry` and drops `#include <Astra/Registry/Registry.hpp>` (`:11`) and `#include <Arcane/Render/VisibilitySystem.hpp>` (`:5`). `PickEmit.hpp` already forward-declares the registry.
8. Overlay. If `req.showGrid2d` and `packet.spriteView`, call the moved `PlanGrid2D` / line emitter and append `OverlayLine`s tagged `DepthTested`, `Linear`, `screenSpace = false`. If `req.showGrid3d`, `packet.overlay.hasGrid3d = true` and `packet.overlay.grid3d = req.grid3d`. If `req.physicsDebug`, call `DrawPhysicsDebug` with a collector that writes `OverlayLine`s tagged `AlwaysOnTop`, `Linear`, instead of a `Batcher2D`. The collector is a `Batcher2D` subclass whose `Line`/`Circle`/`Rect` push overlay primitives; it is local to this cpp. Camera-rect corners: call `ActiveSceneCamera` here (this TU is the allowed caller) and append the rect lines tagged `AlwaysOnTop`, `Linear`. Append `req.gizmoPrims` unchanged.
9. `packet.hasPost = req.post != nullptr`, and if so `packet.post = *req.post` (the `shared_ptr<const vector<uint8_t>>` members keep the bytecode alive; do not invent a second bytecode type). Copy `req.imgui` and `req.gameUi` into `packet.imgui` and `packet.gameUi` by walking `ImDrawData`'s lists into `ImDrawSnapshot`. Copy `req.lightDirection/lightColor/ambient`.

Return `const RenderFramePacket&` to the slot. After return the caller treats it as immutable.

`MeshSectionRanges` walks `table->meshes` (`SceneResources.hpp:259`) and each `MeshEntry::data.sections`, copying `indexOffset` and `indexCount`. Tests that called `BuildGpuSceneFrame(..., meshTable, ...)` become `BuildGpuSceneFrame(..., MeshSectionRanges(meshTable), ...)`, including `CheckOverlayClasses` in `OverlayWitnessTest.cpp`.

`Grid2D.hpp` declares `Grid2DPlan`, `PlanGrid2D`, `PixelsPerMetre`, and `EmitGrid2D(const ViewTransform&, const Grid2DPlan&, std::vector<OverlayLine>&)` — the line loop copied from `ViewportGrid.cpp`'s `DrawGrid2D` (`:132`), writing overlay lines instead of batcher lines. `ArcaneEditor`'s `DrawGrid2D` becomes a loop that emits those lines into the batcher, so `ViewportGridTest.cpp` stays green. Extract calls `EmitGrid2D`. The editor stops calling `DrawGrid2D` from `SubmitSceneToBatcher`.

**Runtime.** Replace the region `RuntimeFrame.cpp:521-548` (Begin, SetGlobals, `PushSceneCamera`, `SetViewProjection`, `ActivePerspectiveSceneCamera`, `PrepareSceneForRender`, `SubmitRender`) with one `ExtractFrame` call. The batcher's `Begin` stays, because screen-space host draws that are not world sprites still use it; world sprites come from the packet, and `AddBatch2DNode` drains `packet.batchVertices/Indices/Spans` instead of `context->CurrentBatch()->Drain()` when the packet is bound. Bind the packet on `NriGraphContext` for the duration of `RenderFrame` (a `const RenderFramePacket*` set by the caller, cleared before return). `PushSceneCamera` becomes: call `ExtractFrame` (or, if extract already ran, read `packet.spriteView`) and `SetView` from `packet.spriteView->view` when it has a value. Do not call `ActiveSceneCamera` in `PushSceneCamera`. The `CreateView<const Camera>` census leaves `RuntimeApp.cpp`.

There is exactly one `ExtractFrame(` in `RuntimeFrame.cpp`, textually above both `RenderFrame(` and `RenderFrameOffscreen(` at `:748`. Both branches therefore run extract. `GpuSceneInvalidate` at `:756` stays.

Delete the second `CollectPickables` at `:573` and `:621`. `packet.pickDrawables` is the list. The light `ApplyDefaultLight` at `:676-681` writes `req.lightDirection/lightColor/ambient` from `Settings<RenderMeshDefaultLightSettings>()` before the call. That read is a setting, not a component.

**Editor.** `RenderSceneToViewport` (`:1642`) stops calling `ActivePerspectiveSceneCamera` (`:1688`) and `PrepareSceneForRender` (`:1689`). It fills `ExtractRequest::editorView` from the editor camera it already computed (`:1685-1688` shares one editor camera in Edit) and calls `ExtractFrame`. `GpuSceneInvalidate` at `:1764` stays.

`RefreshSceneResolution` (`:1506`) contains `ActiveSceneCamera` at `:1572`. That call moves into the extract request (Play sets `playMode` and leaves `editorView` empty; Edit sets `editorView`). Add `EditorApp::RefreshSceneResolution` to the host-gate table in Task 6 in this same change, because after the move the function must not grow a replacement call.

`SubmitSceneToBatcher` stops calling `DrawGrid2D`, `SubmitRender`, `GetResource` for physics (`:1898-1922`), and `ActiveSceneCamera` (`:1953`). Those flags go on the request (`showGrid2d`, `physicsDebug`, `playMode`). The Task 3 local-batcher conversion is deleted here so the lines are not produced twice.

`ArmGraphViewportFrame` stops calling `CollectPickables` (`:2165`). The selection loop at `:2177-2181` stays and reads pick ids off the packet (`PickPassIdOf` against `packet.pickDrawables`). That loop is the allow-list entry.

The gizmo draw `GetComponent` at `:3720` moves into extract (the editor copies the resulting lines into `req.gizmoPrims` before the named builders, or extract reads `Transform` itself when `editorView` is set and a gizmo is armed). Interaction reads at `:1241`, `:1283`, `:1339` stay. The ImGui sink draw was already removed in Task 3.

`AddBatch2DNode`, when the context has a bound packet, copies from `packet.batchVertices` rather than calling `Drain()` on the live batcher. `ImGuiNriNode::Record` (`ImGuiNriNode.cpp:58`) builds a stack `ImDrawData` whose `CmdLists` point at the packet's `ImDrawSnapshot` vectors and passes that to `RenderDrawData`. The snapshot outlives the frame because the ring retains the slot for `FramesInFlight()` extracts.

`PrepareSceneForRender` remains as a function that `ExtractFrame.cpp` calls, or its body is inlined there. No named frame builder calls it after this task.

Thumbnail harvest and shader preview are not given an `ExtractFrame` call. They do not read the scene registry.

- [ ] **Step 4: Run extract, packet, and the GPU-scene tests.**

```powershell
$env:CL = '/FS'
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
.\ArcaneTests.exe "[packet]"
.\ArcaneTests.exe "[gpuscene]"
.\ArcaneTests.exe "[overlay]"
```

Expected: PASS. `GpuSceneCullTest.cpp`, `MeshCullNodeTest.cpp`, and `NriGraphPixelTest.cpp` compile as part of the exe; a failed assertion names itself. `[overlay]` is in this list because `CheckOverlayClasses` calls `BuildGpuSceneFrame`.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneClient/src/Arcane/Render/ExtractFrame.hpp ArcaneClient/src/Arcane/Render/ExtractFrame.cpp ArcaneClient/src/Arcane/Render/Grid2D.hpp ArcaneClient/src/Arcane/Render/Grid2D.cpp ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp ArcaneClient/src/Arcane/Render/PickEmit.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/ImGuiNriNode.cpp ArcaneEditor/src/Viewport/ViewportGrid.hpp ArcaneEditor/src/Viewport/ViewportGrid.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneRuntime/src/RuntimeApp.cpp ArcaneTests/src/ExtractFrameTest.cpp ArcaneTests/src/GpuSceneSyncTest.cpp ArcaneTests/src/GpuSceneCullTest.cpp ArcaneTests/src/MeshCullNodeTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp ArcaneTests/src/OverlayWitnessTest.cpp
git commit -m "feat(render): extract is the only frame reader of the registry" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 6: The include gate and the host frame-builder gate

**Files:**
- Test: `ArcaneTests/src/RenderRegistryIncludeTest.cpp`
- Modify: whichever render TU the first `/showIncludes` run names, other than `ExtractFrame.cpp`. The known offender before Task 5 was `PickEmit.cpp:11`. Task 5 already dropped that include. If this task's run names another TU, split that include in this task before the gate is allowed to pass. The done condition is: the only preprocessed `Astra/Registry/Registry.hpp` under `ArcaneClient/src/Arcane/Render/**/*.cpp` is `ExtractFrame.cpp`.

**Interfaces:**
- Consumes: `ExtractFrame`, `DepthConvention.hpp` (no registry), `SceneCamera.hpp:20` (includes `Registry.hpp`). The named builders are spec section 4.4, plus `EditorApp::RefreshSceneResolution` added in Task 5.
- Produces: two Catch2 cases, `[registry-gate]`. Neither runs on a frame.

- [ ] **Step 1: Write the failing test.**

`ArcaneTests/src/RenderRegistryIncludeTest.cpp`. The compiler invocation is built from the generated vcxproj so the include set cannot drift from `premake5.lua:692-744`.

```cpp
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>

static std::string VcxprojIncludeAndDefines()
{
    const auto vcx = Arcane::Test::RepoRoot() / "ArcaneClient/ArcaneClient.vcxproj";
    std::ifstream in(vcx);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string xml = ss.str();
    // The Debug|x64 ItemDefinitionGroup holds one AdditionalIncludeDirectories
    // and one PreprocessorDefinitions. Take the first occurrence of each;
    // Debug and Release share the engine include set.
    auto grab = [&](const char* tag) {
        const std::string open = std::string("<") + tag + ">";
        const std::string close = std::string("</") + tag + ">";
        const std::size_t a = xml.find(open);
        const std::size_t b = xml.find(close, a);
        REQUIRE(a != std::string::npos);
        REQUIRE(b != std::string::npos);
        return xml.substr(a + open.size(), b - (a + open.size()));
    };
    return grab("AdditionalIncludeDirectories") + "\n" + grab("PreprocessorDefinitions");
}

static int ShowIncludes(const std::filesystem::path& tu, const std::string& includes,
                        const std::string& defines, std::string& output)
{
    // cl is already on PATH inside the shell that built the exe, because the
    // test is only required to pass in the environment that just ran MSBuild.
    // The command is one translation unit, syntax-only (/Zs), dynamic CRT (/MD).
    std::string cmd = "cl /nologo /showIncludes /Zs /std:c++latest /EHsc /MD /Zc:__cplusplus ";
    std::stringstream def(defines);
    std::string item;
    while (std::getline(def, item, ';'))
        if (!item.empty() && item.find("%") == std::string::npos)
            cmd += "/D" + item + " ";
    std::stringstream inc(includes);
    while (std::getline(inc, item, ';'))
        if (!item.empty())
            cmd += "/I\"" + item + "\" ";
    cmd += "\"" + tu.string() + "\" 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (fgets(buf, sizeof buf, pipe))
        output += buf;
    return _pclose(pipe);
}

TEST_CASE("render TUs preprocess Registry.hpp only from ExtractFrame.cpp", "[registry-gate]")
{
    const std::string block = VcxprojIncludeAndDefines();
    const std::size_t split = block.find('\n');
    const std::string includes = block.substr(0, split);
    const std::string defines = block.substr(split + 1);
    const auto root = Arcane::Test::RepoRoot() / "ArcaneClient/src/Arcane/Render";
    bool sawExtract = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.path().extension() != ".cpp") continue;
        std::string output;
        const int code = ShowIncludes(entry.path(), includes, defines, output);
        INFO(entry.path().string());
        CHECK(code == 0);
        const bool hit = output.find("Astra\\Registry\\Registry.hpp") != std::string::npos
                      || output.find("Astra/Registry/Registry.hpp") != std::string::npos;
        const bool extract = entry.path().filename() == "ExtractFrame.cpp";
        if (extract) sawExtract = true;
        if (extract)
            CHECK(hit);          // the allowlisted TU really does include it
        else
            CHECK_FALSE(hit);

        std::ifstream src(entry.path());
        std::ostringstream body;
        body << src.rdbuf();
        if (!extract)
            CHECK(body.str().find("Astra::Registry") == std::string::npos);
    }
    CHECK(sawExtract);
}

TEST_CASE("a clean include of DepthConvention passes and SceneCamera fails the same check", "[registry-gate]")
{
    const std::string block = VcxprojIncludeAndDefines();
    const std::size_t split = block.find('\n');
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
    CHECK(ShowIncludes(passTu, block.substr(0, split), block.substr(split + 1), passOut) == 0);
    CHECK(passOut.find("Registry.hpp") == std::string::npos);
    ShowIncludes(failTu, block.substr(0, split), block.substr(split + 1), failOut);
    CHECK(failOut.find("Registry.hpp") != std::string::npos);
    std::filesystem::remove(passTu);
    std::filesystem::remove(failTu);
}
```

Host gate, same file. Parse each named function by brace depth from its signature to the next function at the same depth. Fail if the body contains `CreateView`, `GetComponent`, `GetResource`, `InspectEntity`, `ActiveSceneCamera`, `ActivePerspectiveSceneCamera`, `CollectPickables`, `PrepareSceneForRender`, `BuildVisibleSet`, or `GpuSceneSync`.

Allow-list, and only these two sites: a line that calls `GpuSceneInvalidate` inside `RuntimeFrame.cpp`'s `RenderGraph` (the call at `:756`), and the `PickPassIdOf` loop inside `ArmGraphViewportFrame` (the loop that was at `:2177-2181`). Exempt the line, not the whole function. `CVarRegistry` is not a hit: the scanner matches `GetResource<` / `CreateView<` / `Astra::` call tokens, and `Settings<` is not one of them.

Named functions and files:

| Function | File |
|---|---|
| `RenderGraph` | `ArcaneRuntime/src/RuntimeFrame.cpp` |
| `EditorApp::RefreshSceneResolution` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `EditorApp::RenderSceneToViewport` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `EditorApp::SubmitSceneToBatcher` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `EditorApp::ArmGraphViewportFrame` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `MaterialPreviewHarvester::Impl::Harvest` | `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp` |
| `ShaderEditorDocument::RenderGraphPreview` | `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp` |

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

static int HostGateHits(const std::string& text, const std::string& signature)
{
    const std::string body = FunctionBody(text, signature);
    if (body.empty()) return -1;
    const char* tokens[] = {
        "CreateView", "GetComponent", "GetResource", "InspectEntity",
        "ActiveSceneCamera", "ActivePerspectiveSceneCamera", "CollectPickables",
        "PrepareSceneForRender", "BuildVisibleSet", "GpuSceneSync"
    };
    int hits = 0;
    std::size_t line = 0;
    while (line < body.size())
    {
        const std::size_t end = body.find('\n', line);
        const std::string row = body.substr(line, (end == std::string::npos ? body.size() : end) - line);
        const bool exempt = row.find("GpuSceneInvalidate") != std::string::npos
                         || row.find("PickPassIdOf") != std::string::npos;
        if (!exempt)
            for (const char* token : tokens)
                if (row.find(token) != std::string::npos)
                    ++hits;
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return hits;
}

TEST_CASE("a planted CreateView in a builder body fails the host gate", "[registry-gate]")
{
    const std::string sample =
        "void EditorApp::SubmitSceneToBatcher(Arcane::Batcher2D& b)\n"
        "{\n"
        "    reg.CreateView<const Arcane::Transform>();\n"
        "}\n"
        "void EditorApp::Next()\n{\n}\n";
    CHECK(HostGateHits(sample, "EditorApp::SubmitSceneToBatcher") == 1);
    const std::string allowed =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(allowed, "RenderGraph") == 0);
}

TEST_CASE("named frame builders do not read the registry", "[registry-gate]")
{
    auto read = [](const char* rel) {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string runtime = read("ArcaneRuntime/src/RuntimeFrame.cpp");
    const std::string editor = read("ArcaneEditor/src/App/EditorAppFrame.cpp");
    const std::string harvest = read("ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp");
    const std::string preview = read("ArcaneEditor/src/Documents/ShaderEditorDocument.cpp");
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

Expected on the first run, if any non-extract TU still includes the registry: FAIL, and the `INFO` names the file. A raw `#include` grep is not the assertion; the planted `SceneCamera.hpp` case is what proves a transitive include fails.

- [ ] **Step 3: Split any TU the gate names.**

For each failing TU other than `ExtractFrame.cpp`: move the registry read into `ExtractFrame.cpp` (it is one of the section 4.2 reads Task 5 missed) or, when the include is only transitive, replace it with the registry-free header (`VisibleSet.hpp`, `DepthConvention.hpp`, `GridSceneDesc.hpp`, `PostChainDesc.hpp`). Do not add an allowlist entry. `Nri/` has no allowlist entry. Then re-run the gate. The case is green only when `ExtractFrame.cpp` is the one hit and the planted `SceneCamera.hpp` TU still fails.

- [ ] **Step 4: Run `[registry-gate]` again.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[registry-gate]"
```

Expected: PASS.

- [ ] **Step 5: Commit.**

```powershell
git add ArcaneTests/src/RenderRegistryIncludeTest.cpp
git commit -m "test(render): showIncludes and host frame-builder gates" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

If Step 3 edited product files, add those paths too. Do not add an unrelated file.

---

### Task 7: Section 7.1 golden gate

**Files:**
- Modify: only the blessed PNGs the grid/overlay class actually moves, under `ReferenceProject/Verify/References/` (and `vulkan/` for a backend-split lane, which these editor lanes are not). World lanes are not blessed.

**Interfaces:**
- Consumes: the frame from Tasks 1–6. `scripts/golden-gate.ps1` lanes (`:345-394`): `runtime-scene` and `f3-cull-blend` (backend-split; `--scene 7e5a0030-0030-4030-8030-000000000030` for f3), `editor-ui`, `editor-ui-perspective` (`--view-mode perspective`), `editor-input-doc`, `editor-asset-page`, `editor-material-page`. The gate does not rebuild the engine. Comparator success is `diffCount == 0`.
- Produces: a green Debug gate and a green Release gate, a Debug game DLL in the slot, and a commit whose message contains the re-bless note below.

- [ ] **Step 1: Write the failing check as the gate itself.**

There is no new Catch2 case. The gate is the check, and it fails on `editor-ui` / `editor-ui-perspective` until the explained bless. World lanes must already pass.

Build Debug in the required order. `arcbuild.exe` exists from Task 1's engine build.

```powershell
$env:CL = '/FS'
& "D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe" build --project ReferenceProject --config Debug --sdk D:\dev\starworks\Arcane-f5
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Debug /m:8 /nodeReuse:false /v:m
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneRuntime\imgui.ini
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneEditor\imgui.ini
Set-Location D:\dev\starworks\Arcane-f5
powershell -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: `runtime-scene` and `f3-cull-blend` report `diffCount == 0` on both backends. `editor-ui` and `editor-ui-perspective` report a diff on the grid (and on physics-debug or camera-rect pixels if the frame contains them). Open the diff images the gate writes and confirm the changed pixels are that class. A world sprite or world mesh pixel in the diff is a stop: fix the earlier task, do not bless it. A gizmo pixel in the diff is a stop: Task 3 ran ACES on a display-referred colour; fix Task 3, do not bless the gizmo away.

`editor-input-doc` and `editor-material-page` cover the viewport with a document tab. They are not pre-authorized for a bless. If their `diffCount` is 0, leave them. If a pixel moved and the diff image is the same grid/overlay class, it may be blessed with the same note. If the diff is anything else, stop.

`editor-asset-page` leaves the viewport visible. A grid diff there is the same class. A mesh-thumbnail or HUD diff is a stop.

Do not bless `inprocess-lit-cube`. If `ARCANE_THUMBS` images moved, stop.

- [ ] **Step 2: Re-bless the grid/overlay class from the staged slot, then copy back.**

From the host exe directory so `--project` resolves to the staged tree (the gate's own working directory for that host). One dx12 bless covers a shared slot. `editor-ui` and `editor-ui-perspective` are shared (`golden-gate.ps1:347-354`).

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

`git status` must list those PNGs. If the bless wrote under a `vulkan\` directory for a shared lane, that write is the wrong slot; shared lanes bless the file at `References\<name>.png` only. Do not copy a vulkan overwrite onto the shared file.

- [ ] **Step 3: Re-run the Debug gate, then Release, then put the slot back on Debug.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5
powershell -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: every lane `diffCount == 0`, both backends.

Release, with no second bless. `rebuild` because the slot was Debug:

```powershell
$env:CL = '/FS'
& "D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe" rebuild --project ReferenceProject --config Release --sdk D:\dev\starworks\Arcane-f5
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane-f5\Arcane.slnx /p:Configuration=Release /m:8 /nodeReuse:false /v:m
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Release-windows-x86_64-md\ArcaneRuntime\imgui.ini
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\bin\Release-windows-x86_64-md\ArcaneEditor\imgui.ini
powershell -File scripts\golden-gate.ps1 -Configuration Release
```

Expected: PASS with no bless. World lanes still `diffCount == 0`. Editor lanes match the PNGs blessed in Debug.

End on a Debug game DLL:

```powershell
& "D:\dev\starworks\Arcane-f5\bin\Release-windows-x86_64-md\arcbuild\arcbuild.exe" rebuild --project ReferenceProject --config Debug --sdk D:\dev\starworks\Arcane-f5
```

Use whichever `arcbuild.exe` exists; the Release one is present after the Release engine build. Do not msbuild Release again after this. The slot `ReferenceProject\Binaries\ReferenceGame.dll` is the Debug CRT.

Also run the witness that pins `f3-cull-blend` counts, from the Debug exe dir:

```powershell
Set-Location D:\dev\starworks\Arcane-f5\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[witness]"
```

`W5` stays `diffCount == 0` (`WitnessScenariosTest.cpp`, the f3 case). If the Debug test exe is stale relative to the Release engine just built, rebuild the Debug engine *after* the Debug `arcbuild rebuild` (game module first, then `Arcane.slnx` Debug) and re-run `[witness]`. That restage copies the Debug DLL beside the Debug hosts and is the configuration this task ends on.

- [ ] **Step 4: Commit the PNGs and the note.**

The commit message body is the note, verbatim:

```
F5 plan 1: the editor grid, physics-debug lines, and the camera rect leave the linear canvas and composite after the tonemap. A blend into linear colour before the nonlinear tonemap differs from running the tonemap operator on the primitive alone and blending after, including when the post chain is empty. editor-ui and editor-ui-perspective include the boot scene's post process (main.arcscene:27-31). The 2D grid also leaves batcher layer 0, so a grid line that crosses a sprite is in this class. World sprites and world meshes are not in this class. The gizmo was already outside the post chain and its pixels stay.
```

```powershell
git add ReferenceProject/Verify/References
git commit -m "test(render): F5 plan 1 golden gate, explained grid re-bless" -m "F5 plan 1: the editor grid, physics-debug lines, and the camera rect leave the linear canvas and composite after the tonemap. A blend into linear colour before the nonlinear tonemap differs from running the tonemap operator on the primitive alone and blending after, including when the post chain is empty. editor-ui and editor-ui-perspective include the boot scene's post process (main.arcscene:27-31). The 2D grid also leaves batcher layer 0, so a grid line that crosses a sprite is in this class. World sprites and world meshes are not in this class. The gizmo was already outside the post chain and its pixels stay." -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

If a lane did not change, it will not appear in `git status`. Add only the PNGs that changed. Do not add `imgui.ini`.

---

## Spec coverage

| Spec | Task |
|---|---|
| 8.1 declarative clear on `batch2d`, `mesh` loads colour, depth clears to the convention, `CmdClearAttachments` gone for those two planes | 1, 2 |
| 8.1 world-node order unchanged; sprites stay in `batch2d`; meshes stay in the mesh pass | 2, 5, 7 |
| 8.1 overlay after tonemap; 3D grid folds in; screen UI after the overlay | 3 |
| 8.1 extract + packet; section 4.2 reads move; 4.3 and 4.4 gates | 4, 5, 6 |
| 8.1 non-rendering hosts skip extract; headless runtime runs it | 5 |
| 3.1 depth table, ForwardZ selected, reverse-Z unselected | 1 |
| 3.1 overlay classes, exposure excluded, empty-chain re-bless is not identity | 3, 7 |
| 3.1 chrome graph has no world pass and no overlay | 2 |
| 3.2 G-buffer, lighting, TAA declared and empty. Forward world stays the mesh node. Translucent-world list is plan 3 | 2 |
| 3.3 render origin field exists and is 0; no rebase | 4, 5 |
| 3.4 overlay samples world-pass depth; no jitter; missing depth draws unoccluded | 3 |
| 3.6 plans 1–2 keep today's camera split; hosts stop calling the two sweeps | 5, 6 |
| 3.7 server skip, headless extract | 5 |
| 4.1 packet fields, `FramesInFlight()` slots, GPU reads the upload ring | 4, 5 |
| 7.1 world pixels identical; grid class re-blessed with a written note | 7 |
