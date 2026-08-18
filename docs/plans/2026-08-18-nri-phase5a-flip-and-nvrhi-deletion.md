# NRI Phase 5a: Default-Path Flip + NVRHI Deletion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the NRI frame graph the engine's only render path and delete the NVRHI implementation behind it, leaving one path, one device abstraction, and no `GraphMode()` flavor splits.

**Architecture:** Two movements with a desk checkpoint between them. **Movement 1 (Tasks 1-3)** flips the default so both hosts construct a graph context unconditionally; NVRHI code still compiles but becomes unreachable. That isolates "does everything work when the graph is the only path" from "did deletion break a shared file" — two independent gates instead of one ambiguous one. **Movement 2 (Tasks 4-11)** deletes, leaves-first: the NVRHI-only passes, then the ImGui and GpuContext flavor splits, then the NVRHI types embedded in shared classes, then the crash layer's NVRHI coupling, then the plugin ABI, then the build. Task 1 runs first and alone because it fixes a correctness hole the flip would otherwise open in Dist.

**Tech Stack:** C++23, MSVC v143 (VS 18), premake5 → `Arcane.slnx`, Catch2 (`ArcaneTests`), NRI v180, D3D12 + Vulkan.

## Global Constraints

Every task's requirements implicitly include this section.

- **THE 12 GOLDEN PNGs ARE FROZEN.** `ReferenceProject/Goldens/{main,editor}-{,batch-,post-}{dx12,vulkan}.png` are the contract. They must compare at `maxDelta 0` throughout. **NEVER re-capture a golden to make a comparison pass** — a delta is a defect until proven otherwise, and re-capturing destroys the only evidence that this phase moved no pixels.
- **Gate baseline: 47694 assertions / 1015 test cases**, both configs, at phase start (`@3c01db89`). Report the count at every task; it may only change when a task deliberately adds or deletes cases, and the task must say so.
- **THE FOUR PHASE-2 PLAN-INPUT CORRECTIONS STILL BIND.** `CAN_ALIAS` does not exist in NRI v180; root descriptors are unbuildable with all-space0 shaders; descriptor-set CB views take no dynamic offsets (per-frame-slot `HOST_UPLOAD` arenas instead); pool-handover barriers must carry the previous tenant's REAL before-layout. Full text at the tail of `docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md`.
- **NO GPU OR WINDOWED RUNS IN AN AGENT SESSION.** Windowed d3d12 SIGSEGVs under remote/locked sessions. Everything GPU-facing is a desk checkpoint (D5a-1, D5a-2) run by the user. The `~[gpu]` gate runs FROM the exe directory.
- **ONE BUILDER AT A TIME.** `Binaries\` is single-slot; on any config flip, `/t:Rebuild` the game module and probe its CRT imports rather than trusting "Build succeeded".
- **Build command:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" D:\dev\starworks\Arcane\Arcane.slnx /p:Configuration=<Debug|Release|Dist> /m`
- **All three configs must build at every task**, Dist included. Dist has its own `#if defined(ARCANE_DIST)` paths (Task 1 touches one) and a habit of breaking silently.
- **ASCII-only in comments.** House style.
- **Commit per task**, message body explaining WHY, not just what.

---

## File structure

**Deleted outright** (NVRHI-only, no graph-side consumer):

| File | Graph-side replacement that already exists |
|---|---|
| `Render/OffscreenCanvas.{hpp,cpp}` | `NriGraphContext` offscreen mode (Task 7, Phase 3) |
| `Render/PickBuffer.{hpp,cpp}` | `Nri/nodes/PickOutlineNodes` pick pass + readback |
| `Render/SelectionOutline.{hpp,cpp}` | `Nri/nodes/PickOutlineNodes` JFA outline |
| `Render/TonemapPass.{hpp,cpp}` | `Nri/nodes/FullscreenNodes` TonemapNode |
| `Render/FullscreenMaterialChain.{hpp,cpp}` | `Nri/nodes/FullscreenNodes` post chain |
| `Render/FullscreenMaterialPass.{hpp,cpp}` | `Nri/nodes/FullscreenNodes` |
| `Render/Canvas.{hpp,cpp}` | graph render targets |
| `Render/Swapchain.hpp` | `NriGraphContext` swapchain |
| `Render/NvrhiMessageCallback.hpp` | `NriCommon` message callback |
| `ImGui/ImGuiNvrhi.{hpp,cpp}` | `Nri/nodes/ImGuiNriNode` |
| `Render/Device.{hpp,cpp}`, `DeviceD3D12.cpp`, `DeviceVulkan.cpp`, `DeviceCreationD3D12.hpp`, `DeviceCreationVulkan.hpp`, `DeviceFactories.hpp` | `Nri/NriDevice` — **but see Task 8: DRED setup and InfoQueue arming must be relocated, not deleted** |
| `ThirdParty/nvrhi/` | — |

**Modified** (shared; NVRHI types removed, behaviour preserved):

| File | What changes |
|---|---|
| `Render/GpuCrashD3D12.cpp` | Task 1: Dist DRED tier stops depending on nvrhi markers |
| `Host/HostConfig.{hpp,cpp}` | Task 2: `nriGraph` retired; `--nri-graph` accepted-and-ignored |
| `Host/GpuContext.{hpp,cpp}` | Task 6: `Create`/`CreateForGraph` collapse to one |
| `ImGui/ImGuiLayer.{hpp,cpp}`, `ImGui/OffscreenImGuiLayer.{hpp,cpp}` | Task 5: two flavors → one; `m_hasNvrhiRenderer` dies |
| `Render/Batcher2D.hpp` | Task 7: `nvrhi::ShaderHandle`/`TextureHandle` members → backend-neutral ids |
| `Render/ShaderLibrary.{hpp,cpp}` | Task 7: **deleted**; only the static `ResolveFlavorDir` is preserved, relocated |
| `Render/SpriteCache.cpp` | Task 7: nvrhi keep-alive refs → `NriTextureCache` |
| `Render/{GpuInstrumentation,GpuFaultInjector,IGpuCrashBackend,GpuCrashVulkan}.*` | Task 8: nvrhi marker channel + injector re-homed onto NRI |
| `Base/Runtime.hpp`, `Plugin/PluginABI.hpp` | Task 9: NVRHI-typed plugin surface removed; ABI 13 → 14 |
| `premake5.lua`, `build/arcane.lua` | Task 10: nvrhi include dirs + project reference dropped |
| `ArcaneEditor/src/App/*`, `ArcaneRuntime/src/*` | Task 11: `GraphMode()` branches collapsed |

---

## Task 1: Dist DRED tier stops depending on nvrhi markers

**Runs first and alone.** This is the one correctness hole the flip opens, it is independent of everything else, and it is cheap.

`GpuCrashD3D12.cpp:711-736` sets `UseMarkersOnlyAutoBreadcrumbs(TRUE)` in Dist. The header states the obligation as BINDING (`GpuInstrumentation.hpp:13-16`, F-2c-bis): *markers-only DRED with no `nvrhi::ICommandList::beginMarker` calls yields an EMPTY breadcrumb list, strictly worse than no DRED at all.* The NRI path emits no nvrhi markers and its own `WriteMarkerNative` is a stub (`NriDiagnostics.cpp:82` returns false), so **the moment NVRHI is unreachable, Dist crash reports lose their breadcrumbs entirely and say nothing about it.**

Fix: Dist falls back to full auto-breadcrumbs — strictly better than empty — until the native NRI marker arc lands and can re-earn the lighter tier.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/GpuCrashD3D12.cpp:711-736`
- Test: `ArcaneTests/src/DiagnosticsTest.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `Arcane::GpuCrash::DredTier()` — `const char*`, the process's DRED tier string, so a test can assert it. Add the accessor if it does not already exist (`g_dredTier` is a file-static atomic today).

- [ ] **Step 1: Expose the tier so a test can see it**

In `GpuCrashD3D12.cpp`, beside the existing `g_dredTier`:

```cpp
    // Exposed for DiagnosticsTest: the tier is otherwise only observable in a
    // log line, and F-2c-bis makes the Dist choice load-bearing enough to pin.
    const char* DredTier()
    {
        return g_dredTier.load(std::memory_order_acquire);
    }
```

Declare it in the matching header next to the other `GpuCrash` entry points.

- [ ] **Step 2: Write the failing test**

In `ArcaneTests/src/DiagnosticsTest.cpp`:

```cpp
TEST_CASE("dred tier never selects markers-only while WriteMarkerNative is a stub", "[diagnostics]")
{
    // F-2c-bis: markers-only auto-breadcrumbs with no marker producer yields an
    // EMPTY breadcrumb list -- strictly worse than no DRED. The NRI path is the
    // only path as of Phase 5a and its WriteMarkerNative returns false, so this
    // tier must not be selected in ANY config until the native marker arc lands.
    const std::string tier = Arcane::GpuCrash::DredTier();
    CHECK(tier.find("markers-only") == std::string::npos);
}
```

- [ ] **Step 3: Run it and watch it fail in Dist**

```
MSBuild Arcane.slnx /p:Configuration=Dist /m
cd bin\Dist-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "[diagnostics]"
```

Expected: FAIL in Dist (`dred:markers-only`). Debug/Release already pass (`dred:full`) — that asymmetry is the bug.

- [ ] **Step 4: Change the Dist tier**

Replace the `#if defined(ARCANE_DIST)` block's markers-only branch:

```cpp
#if defined(ARCANE_DIST)
            // F-2c (lightweight tier) is SUSPENDED as of Phase 5a. Its
            // markers-only auto-breadcrumbs are only worth anything if pass
            // scopes also emit nvrhi::ICommandList::beginMarker/endMarker
            // (F-2c-bis, GpuInstrumentation.hpp) -- and after the NVRHI
            // deletion there is no nvrhi command list to emit them on, while
            // the NRI path's WriteMarkerNative is still the stub at
            // NriDiagnostics.cpp:82. Selecting markers-only here would produce
            // an EMPTY breadcrumb list, which the header calls strictly worse
            // than no DRED at all -- and it would do it silently, in the one
            // config nobody runs interactively.
            //
            // So Dist takes full auto-breadcrumbs: heavier than F-2c intended,
            // but it actually records something. RESTORE the lighter tier when
            // the native NRI marker layer lands and GpuPassScope emits through
            // it -- that arc is what re-earns this branch.
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
            g_dredTier.store(hasSettings1 ? "dred:breadcrumbs" : "dred:breadcrumbs-nocontext",
                             std::memory_order_release);
            (void)hasSettings2;
#else
```

- [ ] **Step 5: Verify all three configs**

```
MSBuild Arcane.slnx /p:Configuration=Debug /m ; MSBuild Arcane.slnx /p:Configuration=Release /m ; MSBuild Arcane.slnx /p:Configuration=Dist /m
```
Then from each exe dir: `.\ArcaneTests.exe "~[gpu]"`
Expected: PASS. Gate 47695/1016 (one case added — state this in the commit).

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Render/GpuCrashD3D12.cpp ArcaneTests/src/DiagnosticsTest.cpp
git commit -m "fix(diag): Dist DRED stops selecting markers-only -- the flip would empty its breadcrumbs"
```

---

## Task 2: Flip the default path

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp:182`, `HostConfig.cpp:36-40,77,155`
- Modify: `ArcaneEditor/src/App/EditorApp.cpp:307`, `ArcaneRuntime/src/RuntimeApp.cpp:130,481`
- Test: `ArcaneTests/src/HostConfigTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `HostConfig` no longer carries `nriGraph`. Every later task may assume the graph context is unconditional.

Only six sites read `nriGraph`. Keep `--nri-graph` **parsed and ignored** rather than rejected — scripts, the Hub's saved launch args, and the desk batteries all pass it, and a hard refusal turns a no-op into a boot failure at the worst moment.

- [ ] **Step 1: Write the failing test**

In `HostConfigTest.cpp`:

```cpp
TEST_CASE("--nri-graph is accepted and ignored after the Phase 5a flip", "[hostconfig]")
{
    // The flag is retired, not rejected: desk batteries, the Hub's saved
    // launch args and every script still pass it. Refusing would convert a
    // harmless no-op into a boot failure.
    const auto with    = Run({"--nri-graph", "--frames", "1"});
    const auto without = Run({"--frames", "1"});
    REQUIRE(with.config);
    REQUIRE(without.config);
    CHECK(with.exitCode == without.exitCode);
}
```

- [ ] **Step 2: Run it, expect a compile failure or a pass-for-the-wrong-reason**

`.\ArcaneTests.exe "[hostconfig]"` — it passes today because both paths parse. That is fine: this test's job is to hold the contract steady while step 3 removes the member. Re-run it after step 3 and it must still pass.

- [ ] **Step 3: Retire the member and the branches**

`HostConfig.hpp` — delete `bool nriGraph = false;` and its comment block.
`HostConfig.cpp:36` — keep the flag registered, change the help text:

```cpp
        cli.Flag  ("nri-graph",      "DEPRECATED, accepted and ignored: the NRI frame graph is "
                                     "the only render path as of Phase 5a. Kept so existing "
                                     "scripts and saved launch args do not fail to boot.");
```

`HostConfig.cpp:77` — delete `cfg.nriGraph = r.Flag("nri-graph");`.
`HostConfig.cpp:155` — the `if (!cfg.nriGraph)` refusal guards a flag that required graph mode; delete the condition, keep whatever it guarded unconditionally.

`EditorApp.cpp:307` and `RuntimeApp.cpp:130` — take the `CreateForGraph` arm unconditionally:

```cpp
        m_gpu = GpuContext::CreateForGraph(m_config);
```

`RuntimeApp.cpp:481` — same treatment; take the graph arm, delete the else.

- [ ] **Step 4: Build all three configs and run the gate**

Expected: PASS, 47695/1016. Dead-code warnings in the NVRHI files are EXPECTED at this task — they are what Tasks 4-10 delete.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(nri)!: the frame graph is the only render path -- --nri-graph retired to a no-op"
```

---

## Task 3: DESK CHECKPOINT D5a-1 (USER)

**Not an agent task.** The flip is live and NVRHI is unreachable but still present — the last moment where a regression can be attributed to the flip alone rather than to deletion.

- [ ] **Step 1: Write the battery**

> **AMENDED 2026-08-18 (user-approved): the battery covers THREE configs, and
> Dist is the point.** This plan never mentions Dist. It should have: until the
> Task 2b flip, `HostConfig::nriGraph` and all three call sites sat inside
> `#if !defined(ARCANE_DIST)`, so a Dist build ALWAYS took the NVRHI arm and
> `--nri-graph` was not even a registered flag there (exit 2). **The flip makes
> Dist render through the NRI graph for the first time ever** — unexercised code
> in the shipped configuration. Without a Dist pass here it stays unexercised
> until Task 12, the last task in the phase. Written and parse-checked:
> `Desktop\D5a-1-Battery.ps1` — goldens in Debug + Dist (12 each), Release gets
> clean latch-asserted runs, crash battery Debug-only (see the Task 12
> amendment), plus a drive item 9 that boots both Dist hosts.
>
> **The battery deliberately passes NO `--nri-graph`.** The flag is a no-op as
> of `e4ff6fed`; the default path IS the graph path, and the default path is
> what this checkpoint must exercise.

`Desktop\D5a-1-Battery.ps1`, modelled on `D3-Exit-Battery.ps1`. PREP both configs, then per backend per host:

```
ArcaneRuntime.exe --project <ReferenceProject> --backend <b> --golden-compare Goldens --golden-stage <batch|post|full> --frames 120
ArcaneEditor.exe  --project <ReferenceProject> --backend <b> --golden-compare Goldens --golden-stage <batch|post|full> --frames 120
```

Assert `maxDelta 0` on all 12. Then `--crash-gpu 60` per host per backend (expect exit 1 + device-removed + artifacts), and the latch invariant `RenderErrorCount B -> N`, `N == B`, on every clean run.

- [ ] **Step 2: User drives the interactive half**

The eight D3 drive items: click-pick select/reselect/clear, outline, Play + game HUD, shader-editor preview + node grid, two project switches, one Build → Rebuild Game Module, drag-resize, latch. **Clicking is the one to watch** — Phase 3's tap regression proves no automated signal covers it.

- [ ] **Step 3: Gate**

12/12 `maxDelta 0` and the drive clean → proceed to Task 4. **Any pixel delta stops the phase**: the flip must move zero pixels, because the graph path already produced these exact baselines. A delta means the default-path construction differs from what D3 exercised, and that is a bug to root-cause, never a golden to re-capture.

---

## Task 4: Delete the NVRHI-only render passes

**Files:**
- Delete: `Render/{OffscreenCanvas,PickBuffer,SelectionOutline,TonemapPass,FullscreenMaterialChain,FullscreenMaterialPass,Canvas}.{hpp,cpp}`, `Render/Swapchain.hpp`, `Render/NvrhiMessageCallback.hpp`
- Modify: every `#include` of the above; `ArcaneEditor/src/App/EditorApp.hpp` (member declarations), `EditorAppFrame.cpp` (the NVRHI arms)
- Modify: `ArcaneTests/src/{MaterialChainGpuTest,MaterialGpuTest}.cpp`

**Interfaces:**
- Consumes: Task 2's unconditional graph context.
- Produces: `Render/` contains no NVRHI pass implementations. Later tasks may assume `PickBuffer`/`SelectionOutline`/`OffscreenCanvas` names do not resolve.

- [ ] **Step 1: Find every consumer before deleting anything**

```bash
git grep -n "OffscreenCanvas\|PickBuffer\|SelectionOutline\|TonemapPass\|FullscreenMaterialChain\|FullscreenMaterialPass\|Render/Canvas.hpp\|Render/Swapchain.hpp\|NvrhiMessageCallback"
```

Every hit is either a deletion site or an edit site. Record the list in the commit body — the count is how a reviewer checks nothing was missed.

- [ ] **Step 2: Delete the files**

```bash
git rm ArcaneClient/src/Arcane/Render/OffscreenCanvas.hpp ArcaneClient/src/Arcane/Render/OffscreenCanvas.cpp \
       ArcaneClient/src/Arcane/Render/PickBuffer.hpp ArcaneClient/src/Arcane/Render/PickBuffer.cpp \
       ArcaneClient/src/Arcane/Render/SelectionOutline.hpp ArcaneClient/src/Arcane/Render/SelectionOutline.cpp \
       ArcaneClient/src/Arcane/Render/TonemapPass.hpp ArcaneClient/src/Arcane/Render/TonemapPass.cpp \
       ArcaneClient/src/Arcane/Render/FullscreenMaterialChain.hpp ArcaneClient/src/Arcane/Render/FullscreenMaterialChain.cpp \
       ArcaneClient/src/Arcane/Render/FullscreenMaterialPass.hpp ArcaneClient/src/Arcane/Render/FullscreenMaterialPass.cpp \
       ArcaneClient/src/Arcane/Render/Canvas.hpp ArcaneClient/src/Arcane/Render/Canvas.cpp \
       ArcaneClient/src/Arcane/Render/Swapchain.hpp \
       ArcaneClient/src/Arcane/Render/NvrhiMessageCallback.hpp
```

- [ ] **Step 3: Follow the compiler**

Build Debug. For each error, delete the member/branch rather than stubbing it. In `EditorApp.hpp` the NVRHI viewport members (`m_viewportTargets.pick`, the NVRHI canvas) go; in `EditorAppFrame.cpp` the `if (GraphMode())` blocks' **else arms** go — keep the graph arm, unindent it. Do **not** collapse the `GraphMode()` calls themselves yet; that is Task 11, and mixing the two makes the diff unreviewable.

- [ ] **Step 4: Retire the tests that tested deleted code**

`MaterialChainGpuTest.cpp` and `MaterialGpuTest.cpp` exercise the NVRHI chain. If a case has a graph-side equivalent already covered by the Nri node tests, delete it and say so; if a case pins behaviour with NO graph equivalent, that is a **coverage gap** — name it in the commit body and add it to the Phase 5b carry list rather than deleting silently.

- [ ] **Step 5: Build all three configs, run the gate**

Expected: PASS. The count WILL drop — state the new number and the deleted-case count in the commit.

- [ ] **Step 6: Commit**

```bash
git commit -m "refactor(nri)!: delete the NVRHI render passes -- the graph nodes are the implementation now"
```

---

## Task 5: Collapse the ImGui layer flavors

**Files:**
- Delete: `ImGui/ImGuiNvrhi.{hpp,cpp}`
- Modify: `ImGui/ImGuiLayer.{hpp,cpp}`, `ImGui/OffscreenImGuiLayer.{hpp,cpp}`
- Test: `ArcaneTests/src/{ImGuiTest,NriHostFlavorTest}.cpp`

**Interfaces:**
- Consumes: Task 4's deletions.
- Produces: `ImGuiLayer::Create(Window&)` — single factory, no device/shaders parameters. `CreateForGraph` is GONE; callers use `Create`. Same for `OffscreenImGuiLayer::Create()`.

**This task retires the flavor split that caused Phase 3's unclickable-editor defect.** `Init(device, shaders)` and `InitForGraph()` differed by `installTap`, and a time-boxed stance applied to one flavor rotted there unseen. One factory means no such divergence is expressible.

- [ ] **Step 1: Write the failing test**

In `NriHostFlavorTest.cpp`, replace the two `CreateForGraph` cases' factory calls with `Create`, and add:

```cpp
TEST_CASE("the ImGui layer has exactly one flavor and it installs the event tap", "[nri]")
{
    // Phase 3's unclickable-editor defect (@f7001ad9) was a stance applied to
    // one of two flavors. One factory makes that class of bug inexpressible.
    // The tap is what carries mouse BUTTON events (imgui_impl_sdl3 turns
    // SDL_EVENT_MOUSE_BUTTON_* into AddMouseButtonEvent; nothing polls them),
    // so a layer without it is a layer nothing can be clicked in.
    Arcane::Window window;
    Arcane::WindowDesc wd; wd.title = "ArcaneTests imgui"; wd.width = 320; wd.height = 200; wd.hidden = true;
    REQUIRE(window.Create(wd));

    auto layer = Arcane::ImGuiLayer::Create(window);
    REQUIRE(layer != nullptr);
    CHECK(window.HasNativeEventTap());

    layer.reset();
    CHECK_FALSE(window.HasNativeEventTap());   // teardown uninstalls
    window.Destroy();
}
```

**`Window::HasNativeEventTap()` DOES NOT EXIST YET** — `Window.hpp:74-75` declares
`NativeEventTap` and `SetNativeEventTap`, and `m_tap` is private (`:109`). Add the
accessor as part of this step, beside `SetNativeEventTap`:

```cpp
        // Observability for the one-flavor invariant: the tap is the only route
        // by which mouse BUTTON events reach ImGui, so "is it installed" is a
        // property worth asserting rather than trusting.
        [[nodiscard]] bool HasNativeEventTap() const noexcept { return m_tap != nullptr; }
```

- [ ] **Step 2: Run it, expect a compile failure**

Expected: FAIL to compile — `ImGuiLayer::Create` still takes `(Window&, RenderDevice&, ShaderLibrary&)`.

- [ ] **Step 3: Collapse the implementation**

In `ImGuiLayer.cpp`: delete `Init(RenderDevice&, ShaderLibrary&)`, delete `InitForGraph()`, rename `InitCommon(bool installTap)` to `Init()` with the tap unconditional. Delete `ImGuiNvrhiRenderer m_renderer`, `bool m_hasNvrhiRenderer`, and the `Render(nvrhi::ICommandList*, nvrhi::IFramebuffer*)` override entirely — `RenderToDrawData()` is the only renderer entry point now. Delete the `ARC_ASSERT(m_hasNvrhiRenderer, ...)`. In `ImGuiLayer.hpp`, delete `CreateForGraph` and the two-flavor prose at the top, replacing it with a one-flavor description.

Apply the same collapse to `OffscreenImGuiLayer`.

- [ ] **Step 4: Run the test and the gate**

Expected: PASS all three configs.

- [ ] **Step 5: Commit**

```bash
git commit -m "refactor(imgui)!: one layer flavor -- the split that hid the unclickable-editor bug is gone"
```

---

## Task 6: Collapse GpuContext

**Files:**
- Modify: `Host/GpuContext.{hpp,cpp}` (`Create`/`CreateForGraph` → one; delete NVRHI members)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp`, `ArcaneRuntime/src/RuntimeApp.cpp`
- Test: `ArcaneTests/src/{NriHostFlavorTest,HostSliceTest}.cpp`

**Interfaces:**
- Consumes: Tasks 4-5.
- Produces: `GpuContext::Create(const HostConfig&)` — the only factory, builds window + NRI device + ImGui layer. `GpuContext::Device()` and `Nvrhi()` are GONE.

- [ ] **Step 1: Update the tests to the single factory**

Replace every `GpuContext::CreateForGraph(cfg)` with `GpuContext::Create(cfg)` in `NriHostFlavorTest.cpp` and `HostSliceTest.cpp`. Run: expect compile failure.

- [ ] **Step 2: Collapse**

Delete the NVRHI `Create`; rename `CreateForGraph` to `Create`. Delete the NVRHI device member, `Device()`, `Nvrhi()`, and the `ShaderLibrary` NVRHI half if `GpuContext` owns it. Update the header's two-flavor prose (`GpuContext.hpp:19,33`) — `:33` explicitly says "The split is cleaned up at Phase 5", so it is discharged here; say so.

- [ ] **Step 3: Build all three, run the gate. Commit.**

```bash
git commit -m "refactor(host)!: GpuContext has one flavor -- the Phase 5 split cleanup its header promised"
```

---

## Task 7: De-NVRHI the shared render types

**The subtlest task.** These classes are used by the graph path and merely *typed* in NVRHI. Behaviour must not change; only the types.

**Files:**
- Modify: `Render/Batcher2D.hpp` (`nvrhi::ShaderHandle vs/ps`, `std::vector<nvrhi::TextureHandle> paramTextures`)
- Modify: `Render/ShaderLibrary.{hpp,cpp}` (delete the `nvrhi::ShaderHandle Get(...)` half; keep source/bytecode)
- Modify: `Render/SpriteCache.cpp` (`unordered_map<Guid, nvrhi::TextureHandle>` keep-alive refs)
- Test: `ArcaneTests/src/{BatcherTest,AssetsTest}.cpp`

**Interfaces:**
- Consumes: Tasks 4-6.
- Produces: `Batcher2D`'s span/material struct carries no NVRHI types. `Arcane::ShaderPaths::ResolveFlavorDir(GraphicsBackend, std::string_view) -> std::filesystem::path` replaces the `ShaderLibrary` static of the same name. Residency is `NriTextureCache`'s job exclusively.

**The graph's shader accessor already exists and is NOT on `ShaderLibrary`:**
`NriGraphContext::ShaderBytecode(const char* name) -> std::span<const std::uint8_t>`
(`NriGraphContext.hpp:1013`). Note `std::uint8_t`, not `std::byte`.

- [ ] **Step 1: Pin current behaviour before touching types**

`BatcherTest.cpp` already covers drain order, run splitting and `Stats().drawCalls`. **Run it and record the exact numbers** — those are the invariant. If drain order or `drawCalls` changes in this task, the type swap changed behaviour and is wrong.

Note the Phase-2 latent recorded in the milestone record: two materials sharing `(layer, orderInLayer)` can reorder run-to-run. Do **not** let a run-order change hide behind that known latent — if order shifts, prove it is the latent and not this edit.

- [ ] **Step 2: Decide each handle member's fate BY EVIDENCE, not by assumption**

`Batcher2D.hpp:42-46` carries `nvrhi::ShaderHandle vs/ps` and
`std::vector<nvrhi::TextureHandle> paramTextures`. These existed so the **NVRHI
batcher** could bind them; the graph's `Batch2DNode` resolves its own shaders
(`Batch2DNode.cpp:120`) and its own textures. So some of these fields are dead
on the graph path and some are still read. **Do not guess — determine it:**

```bash
git grep -n "\.vs\b\|\.ps\b\|paramTextures" -- ArcaneClient/src/Arcane/Render/Nri/
```

Per field: **no hit in `Nri/` → DELETE the field** (it was NVRHI binding state
that outlived its binder). **A hit → RETYPE** to the backend-neutral key the
graph already resolves by — `Guid` for textures (`NriTextureCache`'s key), a
stable shader name for shaders. **Do not use `std::string_view` for a stored
name**: these structs outlive the call that fills them, and a view into a
temporary is a use-after-free. Use `std::string`, or an interned id if one
exists.

State the per-field verdict in the commit body. That table is what a reviewer
checks, and it is the whole risk of this task.

- [ ] **Step 3: Delete ShaderLibrary, preserving only ResolveFlavorDir**

`ShaderLibrary` is NVRHI top to bottom: `Create(nvrhi::IDevice*, ...)` and
`Get(...) -> nvrhi::ShaderHandle`. **The graph uses exactly ONE thing from it —
the static `ResolveFlavorDir`** (`NriGraphContext.cpp:447`,
`NriDiagnostics.cpp:498`), a pure path helper with no device in sight.

Move that static into a new `Render/ShaderPaths.hpp` (namespace
`Arcane::ShaderPaths`), repoint those two call sites, then `git rm` the class.

**Before deleting, confirm `Poll()` and `Generation()` have no graph-side
consumer** — they are the NVRHI shader hot-reload pair, and the graph's live
reload is believed to run through `ShaderCompiler` plus the material compile
service instead:

```bash
git grep -n "Poll()\|Generation()" -- ArcaneClient/src/Arcane/Render/Nri/ ArcaneEditor/src/
```

If either has a live consumer, that capability must be re-homed before the
class dies — say so and re-home it rather than dropping shader hot reload.

- [ ] **Step 4: Strip SpriteCache's keep-alive refs**

The `nvrhi::TextureHandle` map existed to hold GPU textures alive for the NVRHI binder. `NriTextureCache` owns residency now. Delete the map; keep the pixel-data cache and its budget. **Re-read the final review's C1**: `PixelsFor` dropping its pin while an intervening `GetTexture` evicts cross-cache was a real use-after-free, fixed by a block-scoped hoist. Do not reintroduce it — the discriminating test in `AssetsTest.cpp` must still pass.

- [ ] **Step 5: Run BatcherTest and compare against Step 1's numbers, then the full gate, all three configs.**

- [ ] **Step 6: Commit**

```bash
git commit -m "refactor(render)!: shared batcher/shader/sprite types drop their NVRHI handles"
```

---

## Task 8: Re-home the crash layer, PRESERVING DRED

**Highest-risk task.** `Render/Device*.{hpp,cpp}` is NVRHI device creation **and** the home of DRED configuration and D3D12 InfoQueue arming. Deleting the files without relocating that logic silently removes GPU crash diagnostics — the exact class of silent loss Task 1 exists to prevent.

**Files:**
- Modify: `Render/{IGpuCrashBackend,GpuInstrumentation,GpuFaultInjector}.hpp` + `.cpp`, `Render/GpuCrashVulkan.cpp`
- Relocate then delete: `Render/{Device,DeviceD3D12,DeviceVulkan,DeviceCreationD3D12,DeviceCreationVulkan,DeviceFactories}.*`
- Modify: `Render/Nri/NriDevice.*`, `Render/Nri/NriDiagnostics.cpp`
- Test: `ArcaneTests/src/DiagnosticsTest.cpp`

**Interfaces:**
- Consumes: Tasks 4-7.
- Produces: DRED setup and InfoQueue arming live on the NRI device-creation path. `GpuPassScope` emits the first-party breadcrumb channel only. `GpuFaultInjector::Create(NriDevice&)` / `Fire(nri::CommandBuffer*)`.

- [ ] **Step 1: Inventory what is NOT NVRHI in the device files**

```bash
git grep -n "DRED\|InfoQueue\|SetBreakOnSeverity\|D3D12GetDebugInterface" -- ArcaneClient/src/Arcane/Render/Device*.cpp
```

Every hit is logic to RELOCATE, not delete. `NriCommon.cpp:121` already notes that `DeviceD3D12.cpp` arms the InfoQueue — that dependency must survive the move.

- [ ] **Step 2: Write the failing test**

```cpp
TEST_CASE("DRED is still configured after the NVRHI device layer is gone", "[diagnostics]")
{
    // The DRED policy tier used to be set during NVRHI D3D12 device creation.
    // It is process-global state configured before any device exists, so it
    // must survive the deletion of the file that happened to own it.
    const std::string tier = Arcane::GpuCrash::DredTier();
    CHECK(tier != "dred:off");
    CHECK(tier.find("markers-only") == std::string::npos);   // Task 1's invariant
}
```

- [ ] **Step 3: Relocate DRED + InfoQueue arming onto the NRI device path**

Move the `std::call_once` DRED block into the NRI D3D12 device-creation path, keeping the F-2 tier comments verbatim — they encode reasoning no reader can reconstruct.

- [ ] **Step 4: Sever the nvrhi marker channel in GpuInstrumentation**

`GpuPassScope` currently writes BOTH channels. Delete the `nvrhi::ICommandList::beginMarker`/`endMarker` half; keep the `GpuBreadcrumbs` scope and the backend `WriteMarkerNative` call. **Rewrite the F-2c-bis paragraph in the header** (`GpuInstrumentation.hpp:9-16`) to record that the nvrhi channel is gone, that Task 1 suspended the tier depending on it, and that the native NRI marker layer is what restores both.

- [ ] **Step 5: Re-home GpuFaultInjector onto NRI**

`--crash-gpu` is the desk gate for the whole diagnostics stack — it must keep working. `NriDiagnostics::FireFault` already fires the same `gpu_fault.hlsl` loop as an NRI compute dispatch, so the NVRHI injector is redundant: delete it and route `GpuFaultInjector::Create/Fire` to the NRI implementation. **Keep the 256 B CBV alignment fix and the non-null `AbortExecution` callback** (`@21759d73`) — both are load-bearing and both were found the hard way.

- [ ] **Step 6: Delete the device files, build all three, run the gate.**

- [ ] **Step 7: Commit**

```bash
git commit -m "refactor(diag)!: DRED and the fault injector move to the NRI device; the NVRHI device layer is deleted"
```

---

## Task 9: Remove NVRHI from the plugin ABI, bump to 14

**Files:**
- Modify: `Base/Runtime.hpp:173-181` (`SetRenderResources`, `Device()`, `Shaders()`), `Base/Runtime.cpp`
- Modify: `Plugin/PluginABI.hpp:141` (`kGamePluginABIVersion` 13 → 14), `:137` comment
- Modify: `ReferenceProject/ReferenceProject.arcproj`, `D:\dev\starworks\Gacha\Game\Aphelyon.arcproj`
- Test: the ABI constant is pinned across **eight** files — `ProjectManifestTest.cpp` (the abi-refusal path), `RuntimeModulePluginTest.cpp`, `PluginLoadDiagnosticsTest.cpp`, `HostBootTest.cpp`, `HostSliceTest.cpp`, `ProjectTest.cpp`, `RuntimeProjectTest.cpp`, `BootStageParityTest.cpp`. Any that hardcode `13` must move to `14`; prefer referencing `kGamePluginABIVersion` so the next bump is free.

**Interfaces:**
- Consumes: Tasks 4-8.
- Produces: ABI 14. `Runtime` exposes no render-backend types.

Verified: **neither ReferenceProject nor Aphelyon calls `Device()`, `Shaders()`, `SetRenderResources` or `OffscreenCanvas`** — grep returned nothing in either. The removal is safe; the bump is required because the vtable shape changes.

- [ ] **Step 1: Delete the three members and their backing storage.** The doc comment describes "a plugin that needs to build its OWN engine render objects (e.g. an OffscreenCanvas...)" — that capability is gone with the class; if a plugin ever needs graph-side resources, that is a new, deliberate API, not a resurrected NVRHI pointer.

- [ ] **Step 2: Bump the ABI**

```cpp
    inline constexpr uint32_t kGamePluginABIVersion = 14;
```

- [ ] **Step 3: Restamp both projects and rebuild their modules**

```bat
:: ReferenceProject
cd /d D:\dev\starworks\Arcane\ReferenceProject
MSBuild ReferenceProject.slnx /p:Configuration=Debug /m /t:Rebuild

:: Aphelyon (separate repo)
cd /d D:\dev\starworks\Gacha\Game
%ARCANE_SDK%\ThirdParty\premake5\premake5.exe vs2026
MSBuild Aphelyon.slnx /p:Configuration=Debug /m /t:Rebuild
```

Edit the `"abi"` field in both `.arcproj` files 13 → 14. **Auto-restamp does NOT heal a stale stamp — it is a refusal.** Single-slot `Binaries\`: a Release host session needs its own `/t:Rebuild`, and the CRT imports must be probed (`ucrtbased.dll` present = Debug module) rather than trusting the build log.

- [ ] **Step 4: Build all three, run the gate, launch both hosts headlessly (`--frames 1`) to prove the modules load at ABI 14.**

- [ ] **Step 5: Commit** (the Aphelyon `.arcproj` edit is a **separate commit in the Gacha repo** — note it in the body so it is not lost)

```bash
git commit -m "feat(abi)!: plugin ABI 14 -- the render surface no longer exposes NVRHI types"
```

---

## Task 10: Drop NVRHI from the build

**Files:**
- Modify: `premake5.lua:53,79,98`, `build/arcane.lua:65,75`
- Delete: `ThirdParty/nvrhi/`
- Modify: `ThirdParty/README.md` (the vendored-dep index)

- [ ] **Step 1:** `git grep -n nvrhi -- '*.lua'` — every hit is a deletion.
- [ ] **Step 2:** Remove `IncludeDir["nvrhi"]`, `include "ThirdParty/nvrhi"`, and the include-dir entries in `build/arcane.lua`. Fix `premake5.lua:53`'s comment: imgui lives in ArcaneClient.dll with `imgui_impl_nvrhi` — that impl is gone as of Task 5.
- [ ] **Step 3:** `git rm -r ThirdParty/nvrhi` and drop its README row.
- [ ] **Step 4:** Regenerate and rebuild from clean:

```
ThirdParty\premake5\premake5.exe vs2026
MSBuild Arcane.slnx /p:Configuration=Debug /m /t:Rebuild
```

A **clean** rebuild is required — stale incremental artifacts hide missing-include errors, and a vendored-layout change is precisely the case that produces phantom failures.

- [ ] **Step 5:** Build all three, gate, commit.

```bash
git commit -m "build!: NVRHI is no longer a dependency"
```

---

## Task 11: Collapse the GraphMode() branches and retag

**Files:**
- Modify: `ArcaneEditor/src/App/{EditorApp.hpp,EditorApp.cpp,EditorAppFrame.cpp,EditorAppProject.cpp}`, `ArcaneRuntime/src/{RuntimeApp.hpp,RuntimeFrame.cpp}`
- Modify: every surviving `[nvrhi]`-labelled diagnostic producer

- [ ] **Step 1:** `git grep -n "GraphMode()"` — after Tasks 4-10 every call returns true. Delete the predicate and unindent the surviving arms, file by file, building between files. `EditorApp.hpp:662` declares it; `EditorApp.hpp:660` already notes it is "Always false in a Dist build (CreateForGraph is..." — that comment dies with it.
- [ ] **Step 2:** Retag the remaining `[nvrhi]`-labelled message producers (`DeviceVulkan` message routing and the one `NoteError` caller, per the Phase 2 carry list) to `[nri]` — the label named a callback that no longer exists.
- [ ] **Step 3:** Sweep for prose that still describes two paths:

```bash
git grep -in "nvrhi\|nri-graph\|graph mode\|graph arm\|graph flavor" -- '*.hpp' '*.cpp' '*.md'
```

Every surviving mention must be either **historical and marked as such** (milestone records, phase plans — leave those alone) or **wrong and fixed**. This sweep is the task's real deliverable: Phase 3 proved that prose describing a temporary state hardens into prose describing current fact.

- [ ] **Step 4:** Build all three, gate, commit.

```bash
git commit -m "refactor!: one render path -- GraphMode() and the last two-path prose are gone"
```

---

## Task 12: DESK CHECKPOINT D5a-2 (USER) + milestone record

- [ ] **Step 1:** `Desktop\D5a-2-Battery.ps1` — the D5a-1 battery, re-run whole: 12 golden compares at `maxDelta 0`, `--crash-gpu` both hosts both backends, latch invariant, drag-storms, ~~plus a **Dist** `--crash-gpu` to prove Task 1's tier change produces a non-empty breadcrumb list~~.

> **AMENDED 2026-08-18: the Dist `--crash-gpu` step is DROPPED — it is
> impossible as written.** `--crash-gpu` is registered inside `HostConfig.cpp`'s
> `#if !defined(ARCANE_DIST)` block, so **the flag does not exist in a Dist
> build**. Un-guarding it would put a deliberate GPU-fault trigger in the
> shipped binary, which is not worth the one-time proof. The step is replaced by
> this equivalence argument, which the user delegated and the controller ruled:
>
> 1. **What Task 1 changed is the tier SELECTION**, and that is already proven
>    in Dist by the automated gate — the `[diag]` case *"dred tier never selects
>    markers-only while WriteMarkerNative is a stub"* compiles and runs in the
>    Dist build and asserts `DredTier()` is not markers-only.
> 2. **What produces breadcrumb CONTENT** is D3D12's auto-breadcrumb recording,
>    driven by `SetAutoBreadcrumbsEnablement(FORCED_ON)` at
>    `GpuCrashD3D12.cpp:680` — **unconditional, above the `#if`, byte-identical
>    in all three configs**. It is runtime-generated, not app-marker-generated;
>    that independence is the entire premise of Task 1's fix.
> 3. **The only Dist-unique delta left** is `SetPageFaultEnablement(FORCED_OFF)`,
>    which Task 1 did not touch.
>
> So the Debug `--crash-gpu` runs, which exercise full auto-breadcrumb capture
> end to end, carry the evidence across. If the direct proof is wanted instead,
> un-guarding `--crash-gpu` becomes a small task of its own — a deliberate
> change to the shipped CLI surface, not a silent one.
- [ ] **Step 2:** The eight-item drive checklist, both backends.
- [ ] **Step 3:** Append the Phase 5a milestone record to the tail of this plan: final commit range, gate count, ABI 14, what the deletion removed (file and line counts), the carry list for Phase 4, and any amendment this plan's text owes. Delete the SDD workspace **only after** the record is written and pushed — it is gitignored and unrecoverable.

---

## Verification ladder (Phase 5a exit criteria)

1. `~[gpu]` gate green in **all three configs** at every task.
2. Desk D5a-1: 12/12 goldens `maxDelta 0` with the flip live and NVRHI unreachable.
3. Desk D5a-2: 12/12 goldens `maxDelta 0` with NVRHI deleted, plus crash battery on both hosts, both backends, **and Dist**.
4. Both hosts boot and load an ABI-14 module.
5. `git grep -in nvrhi -- '*.cpp' '*.hpp' '*.lua'` returns only historical prose.

## Carried into Phase 4 / 5b

- **The native NRI marker layer** — now carries a second obligation: it restores the Dist markers-only DRED tier Task 1 suspended (F-2c-bis).
- **Any coverage gap Task 4 names** when retiring `MaterialChainGpuTest` / `MaterialGpuTest` cases with no graph-side equivalent.
- **The boundary doc (Phase 5b)** — rewrite and re-ratify after Phase 4, once bindless and mesh resources have exercised the parts of the RHI boundary 2D never touches.
- **Plugin-owned offscreen render targets** — `OffscreenCanvas` was the plugin-facing way to get one and it is gone with no replacement. No current consumer, so this is a capability to design deliberately if something ever needs it, not a regression to patch.
