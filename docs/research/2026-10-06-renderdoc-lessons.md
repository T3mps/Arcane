# What Arcane can take from RenderDoc 1.46

**Date:** 2026-10-06
**Source:** `D:\dev\_reference\renderdoc-1.46`. RenderDoc is MIT-licensed, so we may borrow code with attribution.
**Method:** four read-only research passes (UI/UX, debug overlays and analysis, capture architecture, integration and automation), each compared against Arcane `feat/settings` (`D:\dev\starworks\Arcane-settings`), then combined by the controller.

**Status:** research only. Nothing here is scheduled. Every item is a candidate for the user to pick from.

## Summary

**What RenderDoc has to do that Arcane does not.** RenderDoc has to hook the graphics API and reconstruct the frame from API calls. Arcane owns its render graph: `RenderGraph` already records every node's reads and writes (`RenderGraph.hpp:864-878`), the compiled barriers and transient lifetimes (`RgCompiled`, `:429-490`), and named resources (`:161-187`, `:685-691`). It also emits per-node annotations (`RenderGraphExec.cpp:65-185`), and the pick pass has a readback ring with no stall (`PickOutlineNodes.hpp:40-63`). So most of RenderDoc's value is reachable natively and more cheaply, and it can show things RenderDoc cannot, such as the frame graph itself.

**What Arcane lacks today:**
- debug view modes (`ViewMode` in `EditorCamera.hpp:64` is only the camera);
- a texture viewer;
- a frame-graph view;
- any GPU query pools (timestamps, occlusion, statistics);
- any readback of DRED or Vulkan device-fault data after a GPU crash;
- any way to trigger a RenderDoc capture from the editor.

## Ranked adoption menu

Effort: S is under 2 days, M is under 1 week, L is more. "Home" names where the work would land.

### Tier 1: cheap and high value (any slot)

| # | Item | RenderDoc reference | Arcane home | Effort |
|---|---|---|---|---|
| 1 | **Read DRED back on device loss:** breadcrumb nodes with their context strings (our node names), the last-executed index, the page-fault VA, and the existing and freed allocations. On Vulkan, `vkGetDeviceFaultInfoEXT` into the same `fault.*` fields. | `driver/d3d12/d3d12_device.cpp` CheckHRESULT / DumpDRED / DumpDREDPageFault (~4017-4140) | `NriGraphCrashBackend::CollectFault` (`Nri/NriDiagnostics.cpp`); a `.gpudump` `dred` section. Today page-fault DRED is enabled (`GpuCrashD3D12.cpp:115-120`) but never read (`IGpuCrashBackend.hpp:339-350`). | S |
| 2 | **"Capture Frame (RenderDoc)"**, opt-in. Detail in **RenderDoc integration** below. | `renderdoc/api/app/renderdoc_app.h`; `docs/in_application_api.rst` | Boot (after `ApplyEarlyConfigRungs`, before device creation), an S4 action, the console | M (3-5 d) |
| 3 | **NaN / Inf / negative detector:** an overlay plus an `InterlockedAdd` counter read back, giving "NaN pixels: N" in the HUD and VerifyReport even when the overlay is off. | `data/hlsl/texdisplay.hlsl:186-195` | A fullscreen node on the RGBA16F canvas before tonemap (`FullscreenNodes.cpp:1477`) | S |
| 4 | **Lit / Unlit / Wireframe view modes**, plus a wireframe overlay | `driver/d3d12/d3d12_overlay.cpp:1287` | `MeshNode` pipeline variants (`FillMode::WIREFRAME`, NRI `NRIDescs.h:1231`), under a ViewMode preset layer above `FrameDesc` (already planned at `NriGraphContext.hpp:366-391`) | S |
| 5 | **Copy and CSV export on every table:** Ctrl+C gives aligned text | `qrenderdoc/Widgets/Extended/RDTreeView.cpp:457` | `EditorWidgets` table helper (22 `BeginTable` sites; today only Problems has "Copy path") | S |
| 6 | **Artifact integrity:** a CRC per section, a supported version range instead of exact equality, and a `headerLength` field | `serialise/rdcfile.cpp:323-346` | `ArtifactReader.cpp:15,169` (it rejects `version != kArtifactVersion`, so any bump invalidates the whole store). Copy Astra's `BinaryHeader` (`BinaryArchive.hpp:117-148`). | S |
| 7 | **Name every GPU resource:** `NriTextureCache.cpp:522` uses the generic name "content artifact" | (RenderDoc shows names everywhere) | Use the asset GUID and path as the debug name | S |

### Tier 2: the debugging toolkit (natural home: the introspection arc)

| # | Item | RenderDoc reference | Arcane home | Effort |
|---|---|---|---|---|
| 8 | **TextureView widget plus a Texture document:** channel toggles (right-click shows that channel alone), black/white points with a histogram, auto-fit, a pick readout with a swatch, arrow-key nudge, Ctrl+G go-to-pixel, mip/slice, fit/1:1 zoom, checker background, save with alpha options. Also golden review as an expected / actual / diff A/B (`ImageCompareResult::diffRgba` is only a PNG today). | `Windows/TextureViewer.cpp`, `Widgets/RangeHistogram.cpp`, `Dialogs/TextureSaveDialog.cpp` | New `Widgets/TextureView` and `Documents/TextureDocument` | M widget / L document |
| 9 | **"Show buffer" in the viewport:** any named graph texture (depth, pick IDs, outline/JFA, HDR canvas) through the TextureView. Transients share pool memory, so this needs a "keep" copy node. | TextureViewer overlay and texture list | Viewport (`EditorPanels.cpp:1384-1392`) + `RenderGraph.hpp:685-691` | M |
| 10 | **Frame Graph panel:** node rows; the selected resource's lifetime bar with read, write, read-write, barrier and clear marks and a legend; barrier layouts in tooltips; step, find (F3), breadcrumbs | `Windows/EventBrowser.cpp`, `Windows/TimelineBar.cpp:661-819` (`HighlightResourceUsage` :136) | New `Panels/RenderGraphPanel` over `RgCompiled` | M |
| 11 | **Pixel probe and inspector:** HDR and post-tonemap value, depth, and the entity at the cursor | `PickPixel` (`d3d12_replay.cpp:2896`) | Extend the pick readback ring (`PickOutlineNodes.cpp:295`). For an exact id, a `nointerpolation` row (`mesh.hlsl:120`) into an R32_UINT MRT debug permutation. | S-M |
| 12 | **Quad overdraw, plain overdraw and triangle density heatmaps.** Quad overdraw is Stephen Hill's `SV_Coverage` + `ddx_fine` + `InterlockedAdd` trick with a resolve of `sum(count[i]/(i+1))`. Triangle density uses `SV_Barycentrics` derivatives instead of a geometry shader. | `data/hlsl/quadoverdraw.hlsl`; `data/glsl/trisize.*`; `d3d12_overlay.cpp:1624-2021` | `MeshNode` debug permutations plus one `debug-resolve` fullscreen node | S-M each |
| 13 | **Depth visualisation:** linearised depth; a selection depth test (red where it fails, green where it passes). Stencil waits until deferred adds a stencil plane. | `d3d12_overlay.cpp:2022-2250` | A fullscreen node over the `"depth"` transient | S |
| 14 | **Per-texture min/max and histogram:** a tile reduction plus 256 buckets. It doubles as the auto-exposure basis in the Deadlock target. | `data/hlsl/histogram.hlsl`; `GetMinMax`/`GetHistogram` (`d3d12_replay.cpp:3008/3189`) | A compute node on any named transient (wave intrinsics), the readback ring, ImPlot (wishlist) | S-M |
| 15 | **GPU timestamps per graph node.** RenderDoc relies on vendor counters; we should time our own graph. | n/a (gap noted by every pass) | Query pools in `RenderGraphExec` (NRI supports them, `NRIDescs.h:1602`), feeding the Frame Graph panel and Tracy | M |
| 16 | **One shared filter grammar:** `+must -exclude (group) $fn(args)` with a live explanation of what the filter means | `EventBrowser.cpp:3098` (parse), `:5056` (explanations); `docs/how/how_filter_events.rst` | One parser for the asset browser, outliner, console, Problems and Settings, with `$kind(material)`, `$refs(<guid>)`, `$has(Camera)` | M |
| 17 | **Mesh document preview modes:** wireframe, normals, UV/attribute as colour, explode, plus a vertex table synced with clicks in the preview | `BufferViewer.cpp:5022, 6979, 7206`; `replay_enums.h:3182` | `Documents/MeshDocument.cpp`. This is the landing spot for the planned interactive material preview. | M |
| 18 | **Custom visualisation shaders:** a project `Content/debugviz/*.hlsl` that hot-reloads, with helper constants | `TextureViewer.cpp:4320, 4579, 4808`; `docs/how/how_custom_visualisation.rst` | TextureView "Custom" mode via `Render/ShaderCompiler` | M |

### Tier 3: the native frame debugger (its own spec)

| # | Item | RenderDoc reference | Arcane home | Effort |
|---|---|---|---|---|
| 19 | **`.arccap` native frame capture.** A recorder armed for one `RenderGraph::Execute`. It writes these sections: Topology (nodes, accesses, compiled barriers, resource descs and lifetimes), Events (a capture-mode `RenderGraphNodeContext` wrapping draws, dispatches and copies; asset GUIDs, never handles), Snapshots (LZ4 per image, chosen by RenderDoc's frame-reference composition), Thumbnail, Log and Meta. One `eventId` space is shared by the capture, the breadcrumb ring and the DRED context strings. Ship inspection first and replay second. | `ActionDescription` (`api/replay/data_types.h:2350`), `ActionFlags` (`replay_enums.h:5126`), `FrameRefType`/`InitReq` (`core/resource_manager.h:74-260`), `EndFrameCapture` (`d3d12_device.cpp:2995`) | New `Render/Nri/FrameCapture` plus a Frame Debugger pane | M (inspect) / L (replay) |
| 20 | **One versioned section container for all binary formats:** `headerLength`, per-section type, version, flags and compressed/uncompressed lengths; unknown sections are skipped; thumbnail and embedded log tail | `serialise/rdcfile.cpp:45-140`; `core.cpp:1666-1705, 2188, 2213` | `ArcaneCore/Base/SectionFile.hpp` for `.gpudump`, `.arcart` and `.arccap` (mesh `.arcart` reserves a `Thumbnail` tag but never writes it) | M |
| 21 | **GPU virtual-address map,** so a faulting VA names a resource | `core/gpu_address_range_tracker.h` | `Render/Nri/GpuAddressMap`, fed by the caches and transient realisation | M |
| 22 | **Pixel history, pass level:** a 1-texel copy after each node that writes, when armed ("which nodes changed this pixel"). Fragment level, via a pixel-shader append permutation, comes later. | `d3d12_pixelhistory.cpp` (algorithm block at lines 27-80) | `RenderGraphExec` | M / L |
| 23 | **Post-VS mesh output:** a `vs_capture` permutation writing to a RWStructuredBuffer. CPU recompute is enough until world-position offset (WPO) exists. | `d3d12_postvs.cpp:3037`, `vk_postvs.cpp` `ConvertToMeshOutputCompute` | `mesh.hlsl` permutation; the material preview | M |
| 24 | **Shader "debugging" without an interpreter:** a `DBG_PRINT` at the cursor pixel into a ring, plus per-node value probes and thumbnails in the shader graph (absent today, `ShaderEditorDocument.hpp:856`). Do NOT port RenderDoc's DXIL/SPIR-V interpreters. | `driver/d3d12/d3d12_dxil_debug.cpp`, `driver/shaders/spirv/spirv_debug.cpp` (concept only) | Shader graph codegen plus ShaderCompiler | M |

**Recommended order:** 1, 2, 3, 4, 5, 6 and 7 first. Then 8, 10, 11, 12 and 15 in the introspection arc. 19 gets its own spec once 10 and 15 exist.

**Render-foundation (T1) input:** give the deferred G-buffer an instance-id or visibility channel. That makes the pixel inspector (11) and pixel history (22) nearly free, and lets the debug overlays work against deferred.

## RenderDoc integration (item 2): design sketch

**Opt-in only.** Loading `renderdoc.dll` opens an unauthenticated target-control listener on `0.0.0.0`, on the first free port in 38920-38927 (`core/core.cpp:657-680`). It must never load by default.

**Detect or load:**
- **Passive:** `GetModuleHandleA("renderdoc.dll")`, or `dlopen(..., RTLD_NOLOAD)` on Linux. This covers a host launched from qrenderdoc with no cvar.
- **Active:** `LoadLibraryW(render.renderdoc.dllPath)`.
- **Boot order:** after `HostConfig::Parse` and `ApplyEarlyConfigRungs`, before `Diagnostics::Install` and device creation (`ArcaneRuntime/src/main.cpp:47-101`; `DeviceCreationD3D12.cpp:300-452`). On Vulkan it must also come before `vkCreateInstance`.
- **Agility SDK:** compatible. RenderDoc embeds `D3D12Core.dll` in the `.rdc`, which costs several MB per capture.

**At load:**
- `UnloadCrashHandler()`, so `.arcdiag` keeps crashes.
- `SetCaptureKeys(NULL,0)` and `SetFocusToggleKeys(NULL,0)`; the trigger is an S4 action instead.
- `MaskOverlayBits(0, None)`, because the overlay would pollute screenshots and goldens.
- `HookIntoChildren=0`, so the crash monitor and arcbuild are never hooked.
- The capture path template is `<project>/Saved/Captures/RenderDoc/<host>_<scene>`.

**Per capture:**
- Use `StartFrameCapture(nativeDevice, hwnd|NULL)` and `EndFrameCapture`. `TriggerCapture` needs a presenting app, so it fails headless.
- Guard with `IsFrameCapturing()`.
- Call `SetCaptureTitle` with project / scene / frame / backend.
- Call `SetCaptureFileComments` with a JSON of engine sha, ABI, backend, adapter and cvar diff.
- Use `DiscardFrameCapture` when the frame errors.
- **Opening the result:** call `ShowReplayUI()` if connected, otherwise `LaunchReplayUI(1, path)`, which starts the matching qrenderdoc.

**PIX:**
- NRI annotations go through WinPixEventRuntime or `PIXBeginEvent`, and RenderDoc decodes them. Our `NodeScope` labels therefore show in both tools.
- One enum cvar makes RenderDoc and PIX mutually exclusive. That rule is engineering judgment, not documented by RenderDoc.

**Validation:** RenderDoc controls validation layers while attached.
- Map our debug layer and Vulkan validation onto `APIValidation`.
- Stamp the verify report `gpuTool:"renderdoc"`, so a 0/0 `RenderErrorCount` is graded Indeterminate, not green.

**Cvars** (`CommandLineOnly` comes from S6-2, so a project file can never load an unauthenticated network tool):
- `render.gpuCapture.tool` = none | renderdoc | pix (Dev, CommandLineOnly; `--renderdoc` is sugar);
- `render.renderdoc.dllPath` (Dev, CommandLineOnly);
- `.captureDir`, `.overlay`, `.captureCallstacks`, `.refAllResources`, `.saveAllInitials`, `.frames`;
- `.openAfterCapture` (editor preference).

**Pitfalls to design in:**
- Add `renderdoc.dll` and `WinPixGpuCapturer.dll` as an "expected capture tool" tier in `ForeignModules.cpp`, so they are not reported as injected.
- Golden bless refuses while a capture tool is attached.
- Always pass the device explicitly.
- `RemoveHooks` is safe only right after load.
- Keep DXC debug info when the tool is on, or reflection shows `cbuffer0`.
- Desk-check that the device-reference armor tolerates a wrapped refcount.

## Changes this suggests for the arc CLI spec (2026-10-05, draft)

**What arc already does better than RenderDoc:**
- a per-process descriptor with port 0, instead of scanning 8 fixed ports;
- a token plus loopback-only binding, instead of an open listener;
- no `eval`.

**Proposed additions** (each cites the spec section it touches):
1. **Frozen bootstrap (s4.4, s6.1, 0.28).** Make the unversioned `GET /arc/status` and the `protocol.mismatch{hostProtocol, protocolMin, engineVersion}` shape permanent across majors. Add `protocolMin` to the descriptor. Source: `core/remote_server.cpp:53-60, 223-240`.
2. **Minor-version field gating (s6.3).** The client sends `X-Arc-Protocol`, and the host omits newer fields. `describe` gets `since` per command and field, plus `stability: stable|experimental`. Source: `target_control.cpp:36-70, 228-258`; `renderdoc_app.h:720-833`.
3. **Advisory drive lease (s4.5, 0.6).** Observe stays lock-free. `arc lease acquire|release [--force]`; a non-holder drive call gets `host.busy{holder}`. Sanitise `X-Arc-Client` before it reaches the audit log. Source: `target_control.cpp:440-520`.
4. **Stream keepalive and replay (s6.1, s4.3).** An initial snapshot per topic, `: ping` every second (silence over 5 s gives exit 7), and `seq` with `Last-Event-ID` resume. Source: `target_control.cpp:157-160, 320-326`.
5. **`options.atFrame` on every job (s7.4, s7.7),** so CI never races settle. Source: `QueueCapture` (`renderdoc_replay.h:1189-1199`).
6. **A `gpu {backend, adapter, validation, presenting, toolsAttached[]}` block in status and the descriptor (s4.3).** Source: `RegisterAPI` (`target_control.cpp:206-222`).
7. **Child-process following (s4.4, s7.1).** Spawned arc-capable children get their own descriptor, `state{child}` is emitted, `children[]` is listed, and `arc status --tree` shows the hierarchy. Tokens are never inherited. Source: `NewChild` (`target_control.cpp:259-268`).
8. **Wire trace (s4.6).** `arc --trace-wire <file>` plus the Dev cvar `arc.server.traceWire`, with the token redacted. Source: `remote_server.cpp:41-45, 300-320`.
9. **A host-side deny list (s10.3).** `arc.server.denyCommands` (Dev, CommandLineOnly). Source: `remoteserver.conf` `noexec`.
10. **CLI structure (s4.6, s7.1).** Trait-attached flag groups (every long command gets `--detach` and `--timeout`; capture commands get `--at-frame`), hidden internal verbs, an `arc doctor` verb, and "data never rides in exit codes" (s6.6). Source: `renderdoccmd.cpp:129-1576, 1485-1492, 248-258`.
11. **Documentation enforced by a test (s5.1, s12.1).** Every args and result field has a Tooltip and follows the naming rules. Source: `interface_check.h`.
12. **`minProtocol` for game-module commands and `.arcscenario` (s5.2.3).** Source: qrenderdoc extension `minimum_renderdoc` (`CaptureContext.cpp:425-530`).
13. **A future `gpu.capture {frames, atFrame}` job** returning the `.rdc` as an artifact. It is currently out of scope in s15, and becomes cheap once item 2 exists.
14. **s10.7 must state the exception** that attaching RenderDoc opens a non-loopback listener.

## What rdtest offers the witness harness

- **One tiny demo per test, with an availability listing.** The demos binary prints `--list-raw` (Name, Available, AvailMessage). This maps onto `Verdict::Skipped` with a reason, or a host `--list-capabilities` (`util/test/rdtest/runner.py:180-190, 320-339`).
- **A fresh child per test with a timeout,** so a crash is a recorded failure; `--in-process` is for debugging. HostWitness already spawns per scenario.
- **Semantic GPU assertions:** `check_pixel_value(tex, x, y, value, eps)` at chosen points, `find_action("<marker>")` (our `NodeScope` labels become test addresses), and post-VS tables (`testcase.py:456, 510-537`). These complement whole-image goldens.
- **Fail a test whose log contains asserts** (`check_renderdoc_log_asserts`, `testcase.py:1058-1067`).
- **Artifacts:** a self-contained HTML log with diffs; a first run with no reference writes the candidate.
- **A later `[gpu][replay]` tier** could open a RenderDoc `.rdc` through RenderDoc's Python module and assert per-pass pixels and markers.

## Naming and explanation lessons for editor UI

- One fixed vocabulary with defined terms.
- IDs render as clickable links carrying names (`QRDUtils.cpp` `RichResourceText*`). Arcane's `LocatorRoute` does this only for Problems; extend it to the Console.
- Instant tooltips on dense tables.
- Status text that teaches the gesture ("Right click - x, y: values").
- Tooltips that say why a control is disabled, as an editor-wide rule (26 `AllowWhenDisabled` sites today).
- One documentation page per window.

## Not worth taking

- **Shader step-debugging and pixel-history UI:** both need a replay engine. Get them by opening the capture in RenderDoc (item 2).
- **Vendor performance counters:** use our own timestamps (item 15) plus Tracy.
- **Bind and API-call statistics:** meaningless with bindless, GPU-driven rendering. Show `GpuSceneFrame::Stats` instead.
- **Per-API pipeline viewers and the API inspector:** NRI and the render graph make one graph view enough.
- **Python shell and extension manager:** the arc CLI is the chosen automation path.
- **Remote/Android, capture comments UI, YUV decoding, Qt styling.**
