# Arcane M2a — Renderer Core (Pacing, Shaders, Canvas, Batcher, Tonemap) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace M1's clear-only scaffolding with the engine's canonical 2D render path: 2 frames in flight, HLSL→DXC shader pipeline with hot reload, a linear-HDR canvas, a quad+primitive batcher, and an ACES tonemap output pass — proven by GPU golden-readback tests and a Playground demo drawing real geometry on both backends.

**Architecture:** Everything lands in Arcane.dll's Render module behind the established pattern (exported pure-virtual interfaces + factories; concrete classes in anonymous namespaces; raw API calls confined to the backend TUs). The frame loop becomes: acquire (slot-gated by `nvrhi::EventQuery`) → batch geometry into a linear RGBA16F `Canvas` → `TonemapPass` (Narkowicz ACES + 2.2 encode, matching the client's landed pipeline) writes the display-referred backbuffer → present. Shaders compile at build time via DXC (DXIL + SPIR-V from one HLSL source) into loose artifact files that `ShaderLibrary` loads per backend and hot-reloads via mtime polling.

**Tech Stack:** NVRHI (pipelines, binding sets, EventQuery pacing), DXC (`ThirdParty/tools/dxc/dxc.exe`, dual-target), glm (public math types), Catch2 (GPU golden-readback tests).

**Spec:** `docs/superpowers/specs/2026-06-12-arcane-2d-renderer-architecture.md` (the renderer north star: NVRHI boundary rules, subsystem stack, milestone mapping — M2a row) + `docs/superpowers/specs/2026-06-11-engine-architecture-design.md` (M2 bullet; Render module responsibilities) + `docs/superpowers/specs/2026-06-10-engine-thirdparty-stack-design.md` (shader pipeline, `/fp:precise`, HLSL single-source). M2b (text, asset loaders, ImGui backends) is a separate follow-up plan — it consumes what this plan builds.

**User decisions (2026-06-12):** linear HDR + ACES now; 2 frames in flight; batcher covers quads + primitives; shader hot-reload in M2; sort-key system in batcher v1 (north-star directive).

---

## Decisions made by this plan (deviations and interpretations — flagged for review)

1. **M2 splits into M2a (this plan: renderer core) and M2b (text + asset loaders + ImGui backends).** Each is independently shippable; M2b's plan is written after M2a lands so it targets as-built reality (the M0→M1 pattern).
2. **Direct DXC invocation now; ShaderMake when shader count grows.** The stack spec says "ShaderMake/DXC". With 4 entry points, explicit `dxc.exe` command lines in one batch script are fully deterministic and reviewable; ShaderMake (vendored, ready at `ThirdParty/tools/ShaderMake/`) earns its config format when we have dozens of shaders in M2b/M3. The script is the single place to swap.
3. **SPIR-V register shifts match NVRHI defaults**: `-fvk-t-shift 0`, `-fvk-s-shift 128`, `-fvk-b-shift 256`, `-fvk-u-shift 384` (space 0) — verified against `nvrhi::VulkanBindingOffsets` defaults (nvrhi.h:1971). Push constants ≤ 128 bytes (`c_MaxPushConstantSize`).
4. **Batcher public math types are glm** (`glm::vec2`/`glm::vec4`) — the stack spec's math pick; header-only, safe in exported interfaces (the no-STL-members rule applies to data members; these appear only as by-value parameters on pure-virtual methods).
5. **Vertex format: pos(float2) + uv(float2) + color(float4) = 32 bytes.** Float4 color (not RGBA8) because the linear-HDR pipeline allows vertex colors > 1.0 (bloom-driving brights) and avoids dark-end quantization in linear space.
6. **Straight (non-premultiplied) alpha blending** — matches the LOVE client's semantics so screen ports translate 1:1. Premultiplied alpha is a known better convention; migrating to it is a deliberate future decision once the client-porting oracle stops mattering (documented on the batcher).
7. **Tonemap = Narkowicz ACES + gamma-2.2 encode, byte-matching the client oracle** (`Client/data/shader/post/post_process.glsl:43-50`). The engine authors linear directly (no decode step — the client's `srgbDecode` exists only because LOVE hands it display-space input).
8. **`kSwapchainFramesInFlight = 2` lives in `Swapchain.hpp`.** Both backend swapchains implement slot gating with `nvrhi::EventQuery` (create/set/wait/reset — verified nvrhi.h:3710-3714); Vulkan gets per-slot acquire/present semaphore pairs. `waitForIdle` disappears from the Present path (it stays in Resize/Release/teardown, where it is correct). `runGarbageCollection()` remains once per Present — nvrhi's docs ask for frequent polling to recycle command-list instances.
9. **No `Renderer` facade yet.** Playground composes canvas→batcher→tonemap→present directly; that composition IS the canonical path. A facade arrives with the render-graph seam design (deferred by the spec) — wrapping it now would freeze an API we haven't earned. Flagged against the homogenized-rendering mandate: there is still exactly ONE path; it just isn't wrapped.
10. **Shader artifact dir resolution:** `ShaderLibrary::Create(device, backend, dir)` takes an explicit directory; the `ARCANE_SHADER_DIR` env var overrides it when set (dev loop: point the running app at `Arcane/shaders/generated/` so recompiles hot-reload without a build's postbuild copy). Default is `shaders/` next to the exe (postbuild-copied).
11. **Pipeline caches key on `(framebuffer info, ShaderLibrary generation)`** — hot reload bumps the generation; consumers (batcher, tonemap) lazily rebuild pipelines on mismatch. No callback web; one integer compare per frame.
12. **Sort keys ship in batcher v1** (north-star directive: they are API-shaping). Every draw carries the current `(layer, orderInLayer)`; `End()` stable-sorts by a 64-bit key — layer(16) | order(16) | pipelineKind(8) | textureSlot(16) — before building batch runs. Contract: draws sharing `(layer, order)` may be reordered for batching; overlapping content takes distinct orders.
13. **Batcher v2 (instancing + structured buffers + bindless) is a planned internals swap, not v1 scope.** Bindless wants its own allocator subsystem and pays off with materials (north-star milestone map). The v1 API is shaped for the swap: calls RECORD draws, `End()` COMPILES them — today into expanded vertex quads, later into per-instance data. Nothing in the public API names vertices.
14. **Render graph stays deferred** to its own milestone per the north-star map; M2a's canvas→tonemap composition is the two-pass seed the graph later absorbs. The post stack waits for the graph — no post effects sneak into M2a.

## Rendering-pipeline foundation contracts (carried from M1 + new for M2a)

Carried (still binding, reviews enforce): backend isolation absolute; `Present()` promises presentation not sync; backbuffer is display-referred output only; explicit `ResourceStates` + `keepInitialState(true)`; NVRHI validation silent — every GPU test asserts `Arcane::RenderErrorCount() == 0`; resize/zero-size/minimize first-class.

New for M2a:

1. **All scene rendering happens in linear space into the Canvas (RGBA16_FLOAT).** Nothing but `TonemapPass` writes the backbuffer. No gamma math anywhere except the tonemap shader's final encode.
2. **One vertex format, one submission path.** Every 2D primitive goes through `Batcher2D` — sprites, rects, lines, circles, and (M2b) text glyphs and ImGui all flow into the same vertex stream + pipeline family. No bespoke draw paths grow beside it.
3. **Shaders are data.** HLSL sources in `Arcane/shaders/`, compiled artifacts in `Arcane/shaders/generated/<dxil|spirv>/` (gitignored), loaded by name at runtime, hot-reloadable. No embedded shader byte arrays, no per-backend shader source.
4. **Frames-in-flight discipline:** CPU-visible resources written per frame go through NVRHI's upload manager (`writeBuffer`/`writeTexture` on a command list — internally versioned per submission) — never through persistently-mapped memory the GPU might still read. Slot gating lives ONLY inside the swapchains.
5. **Interfaces stay backend-blind:** `Canvas`, `Batcher2D`, `TonemapPass`, `ShaderLibrary` speak nvrhi types only. If a method needs a `VkXxx`/`ID3D12Xxx`, the design is wrong.
6. **The NVRHI boundary (north-star prime rule):** never wrap `nvrhi::ICommandList` in our own recording abstraction (systems take `ICommandList*` and record into it — losing the state tracker is the classic mistake); never write our own barrier/state tracking; no descriptor abstraction below the future bindless allocator. Arcane builds policy and orchestration on NVRHI's mechanism.
7. **The batcher API is retained, not immediate:** submission calls record draws; `End()` sorts (64-bit keys) and compiles. Internals are the only thing that changes when v2 (instanced + bindless) lands.

## File structure

```
Arcane/
├── premake5.lua                                MODIFIED — shader prebuild hook, shader postbuild copies, glm include for consumers
├── shaders/                                    NEW — HLSL sources (committed) + generated/ (gitignored)
│   ├── sprite.hlsl                             NEW — batch VS + textured-quad PS
│   ├── circle.hlsl                             NEW — SDF circle PS (shares the batch VS input)
│   ├── tonemap.hlsl                            NEW — fullscreen triangle VS + ACES PS
│   └── compile-shaders.bat                     NEW — DXC invocations (DXIL + SPIRV per entry)
├── Arcane/src/Arcane/Render/
│   ├── Swapchain.hpp                           MODIFIED — kSwapchainFramesInFlight, pacing contract comments
│   ├── DeviceD3D12.cpp                         MODIFIED — EventQuery slot gating replaces idle-per-present
│   ├── DeviceVulkan.cpp                        MODIFIED — per-slot semaphores + EventQuery gating
│   ├── ShaderLibrary.hpp / ShaderLibrary.cpp   NEW — artifact loading per backend, mtime hot reload, generation counter
│   ├── Canvas.hpp / Canvas.cpp                 NEW — linear RGBA16F render target + framebuffer + resize
│   ├── Batcher2D.hpp / Batcher2D.cpp           NEW — quads + rect/line/circle, binding-set + pipeline caches
│   └── TonemapPass.hpp / TonemapPass.cpp       NEW — canvas -> ACES -> target framebuffer
├── Playground/src/main.cpp                     REWRITTEN — bouncing quads/circles/lines + HDR swatch through the canonical path
└── Tests/src/
    ├── PacingTest.cpp                          NEW — [gpu] 20 frames, both backends, validation silent
    ├── ShaderLibraryTest.cpp                   NEW — [gpu] artifact load + hot-reload mechanism
    ├── TonemapTest.cpp                         NEW — [gpu] ACES golden readback vs CPU reference
    ├── BatcherTest.cpp                         NEW — [gpu] quad + circle readbacks
    └── SwapchainTest.cpp                       MODIFIED — comment updates only (pacing now real)
.gitignore                                      MODIFIED — Arcane/shaders/generated/
CLAUDE.md                                       MODIFIED — Arcane section M2a state (Task 8)
```

## Constraints carried into every task

- UTF-8 without BOM, ASCII-only comments. Use Write/Edit tools.
- No `/fp:fast`; don't override `/fp:precise`. NDEBUG stays defined in Release (vulkan.hpp dispatcher ABI — M1 lesson).
- **Never run `db-reset.bat`, `clean.bat --deep`, or `docker compose down -v`.**
- Re-run `Arcane/GenerateProjects.bat` after adding files (vcxproj globs) or premake edits.
- Commit per task, `type(scope):` convention.
- Build/test loop (from repo root):
  ```bat
  cd Arcane
  GenerateProjects.bat
  msbuild Arcane.slnx /p:Configuration=Debug /m
  bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
  ```
- Baseline entering this plan: 107 assertions / 25 test cases, all green, `RenderErrorCount() == 0` asserted in GPU tests.
- The dev box and CI agent both have an NVIDIA GPU (D3D12 + Vulkan + VK_LAYER_KHRONOS_validation). `[gpu]` failures are real failures.
- API-adaptation rule: if a verified-against-header claim in this plan turns out wrong, check the vendored header, adapt minimally, and record the deviation in your report.

---

### Task 1: Two frames in flight (both swapchains)

Replace the M1 idle-after-present pacing with slot-gated frames in flight behind the UNCHANGED `Swapchain` interface — the contract M1 wrote down. D3D12 gates on per-slot `EventQuery`; Vulkan additionally needs per-slot acquire/present semaphore pairs (the single pair was only safe because of the idle wait).

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Render/Swapchain.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/DeviceD3D12.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/DeviceVulkan.cpp`
- Create: `Arcane/Tests/src/PacingTest.cpp`

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/PacingTest.cpp`**

```cpp
// Frame pacing: with 2 frames in flight, 20 consecutive frames must run
// without any validation noise (semaphore reuse, premature resource reuse,
// and upload-buffer races all surface as [nvrhi]/VK validation errors,
// which RenderErrorCount() latches). This is the regression gate for
// removing waitForIdle from the Present path.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Swapchain.hpp>

namespace
{
    void RunPacedFrames(Arcane::GraphicsBackend backend)
    {
        Arcane::Window window;
        Arcane::WindowDesc windowDesc;
        windowDesc.title  = std::string("PacingTest ") + Arcane::ToString(backend);
        windowDesc.width  = 320;
        windowDesc.height = 180;
        windowDesc.hidden = true;
        windowDesc.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
        REQUIRE(window.Create(windowDesc));

        Arcane::RenderDeviceDesc deviceDesc;
        deviceDesc.backend = backend;
        auto device = Arcane::RenderDevice::Create(deviceDesc);
        REQUIRE(device != nullptr);
        auto swapchain = device->CreateSwapchain(window, /*vsync=*/false);
        REQUIRE(swapchain != nullptr);

        nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();
        for (int frame = 0; frame < 20; ++frame)
        {
            nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
            REQUIRE(backbuffer != nullptr);
            commandList->open();
            commandList->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                           nvrhi::Color(0.05f * (float)frame, 0.1f, 0.2f, 1.0f));
            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            swapchain->Present();
        }
        device->Nvrhi()->waitForIdle();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: 20 frames with 2 in flight, validation silent", "[gpu][d3d12]")
{
    RunPacedFrames(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: 20 frames with 2 in flight, validation silent", "[gpu][vulkan]")
{
    RunPacedFrames(Arcane::GraphicsBackend::Vulkan);
}
```

(This compiles and passes against the M1 code too — it becomes the regression gate while you change the internals. TDD here means: make it pass BEFORE and AFTER the rework; the rework is observable via the removed idle-wait, checked in Step 5.)

- [ ] **Step 2: Update `Swapchain.hpp` — constant + contract comment**

Replace the file-top comment block and add the constant inside `namespace Arcane`:

```cpp
// Render module: backbuffer presentation against a Window.
// Pacing (M2): kSwapchainFramesInFlight frames in flight, slot-gated with
// nvrhi::EventQuery inside the swapchain implementations. Present() still
// promises presentation only -- callers must NOT assume any GPU sync
// happened. Anything needing the GPU drained calls waitForIdle() itself.
```

```cpp
    // CPU may run this many frames ahead of the GPU. Slot gating lives
    // INSIDE the swapchains; nothing above this interface sees it.
    inline constexpr uint32_t kSwapchainFramesInFlight = 2;
```

Also update the `Present()` comment on the interface: delete the "waits for GPU idle (one frame in flight)" sentence; it now reads "Presents the acquired frame. Promises presentation, not synchronization."

- [ ] **Step 3: Rework `SwapchainD3D12` in `DeviceD3D12.cpp`**

Add members to `SwapchainD3D12`:

```cpp
            nvrhi::EventQueryHandle m_frameQueries[kSwapchainFramesInFlight];
            uint64_t m_frameCounter = 0;
```

At the end of `SwapchainD3D12::Init` (after `CreateBackbufferHandles()` returns true — restructure the final `return CreateBackbufferHandles();` into an `if (!CreateBackbufferHandles()) return false;`):

```cpp
            for (uint32_t i = 0; i < kSwapchainFramesInFlight; ++i)
                m_frameQueries[i] = m_device->Nvrhi()->createEventQuery();
            return true;
```

Replace `BeginFrame` and `Present`:

```cpp
        nvrhi::ITexture* SwapchainD3D12::BeginFrame()
        {
            if (m_width == 0 || m_height == 0 || m_backbuffers.empty())
                return nullptr;

            // Slot gating: before reusing this slot's per-frame resources,
            // wait until the frame that last used it (N - framesInFlight)
            // has retired on the GPU.
            if (m_frameCounter >= kSwapchainFramesInFlight)
            {
                nvrhi::IEventQuery* slotQuery =
                    m_frameQueries[m_frameCounter % kSwapchainFramesInFlight];
                m_device->Nvrhi()->waitEventQuery(slotQuery);
                m_device->Nvrhi()->resetEventQuery(slotQuery);
            }
            return m_backbuffers[m_swapchain->GetCurrentBackBufferIndex()];
        }

        void SwapchainD3D12::Present()
        {
            if (m_backbuffers.empty())
                return;  // nothing acquired (zero-size window); see BeginFrame

            const HRESULT hr = m_swapchain->Present(m_vsync ? 1 : 0, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                ARC_ERROR("Present failed: device removed/reset (0x{:08X}), reason 0x{:08X}",
                          (uint32_t)hr,
                          (uint32_t)m_device->D3D12Device()->GetDeviceRemovedReason());
            }

            // Mark this slot's GPU completion point; BeginFrame N+2 waits on it.
            m_device->Nvrhi()->setEventQuery(
                m_frameQueries[m_frameCounter % kSwapchainFramesInFlight],
                nvrhi::CommandQueue::Graphics);
            ++m_frameCounter;

            // Recycle retired command-list instances and upload regions.
            m_device->Nvrhi()->runGarbageCollection();
        }
```

(Keep the existing device-removed logging exactly as it is — only the `waitForIdle` line is removed from Present. `ReleaseBackbufferHandles`, `Resize`, and the destructor keep their `waitForIdle` — full drains are correct there.)

- [ ] **Step 4: Rework `SwapchainVulkan` in `DeviceVulkan.cpp`**

Replace the single semaphore pair with per-slot arrays. Members become:

```cpp
            vk::Semaphore m_acquireSemaphores[kSwapchainFramesInFlight];
            vk::Semaphore m_presentSemaphores[kSwapchainFramesInFlight];
            nvrhi::EventQueryHandle m_frameQueries[kSwapchainFramesInFlight];
            uint64_t m_frameCounter = 0;
```

In `Init`, replace the two `createSemaphore` lines with:

```cpp
            for (uint32_t i = 0; i < kSwapchainFramesInFlight; ++i)
            {
                m_acquireSemaphores[i] = device.Device().createSemaphore({});
                m_presentSemaphores[i] = device.Device().createSemaphore({});
                m_frameQueries[i] = device.Nvrhi()->createEventQuery();
            }
```

`BeginFrame` becomes (double-acquire guard and zero-size check stay first, unchanged):

```cpp
        nvrhi::ITexture* SwapchainVulkan::BeginFrame()
        {
            if (m_acquired)
            {
                // Double BeginFrame without Present: re-acquiring would reuse
                // the already-signaled binary semaphore (VUID 01286). Hand
                // back the image we already hold.
                return m_backbuffers[m_currentImage];
            }
            if (m_width == 0 || m_height == 0 || m_backbuffers.empty())
                return nullptr;

            const uint32_t slot = (uint32_t)(m_frameCounter % kSwapchainFramesInFlight);

            // Slot gating: frame N-2's submits (which consumed this slot's
            // acquire semaphore and queued its present signal) must have
            // retired before the slot's binary semaphores are reused.
            if (m_frameCounter >= kSwapchainFramesInFlight)
            {
                m_device->Nvrhi()->waitEventQuery(m_frameQueries[slot]);
                m_device->Nvrhi()->resetEventQuery(m_frameQueries[slot]);
            }

            try
            {
                auto acquired = m_device->Device().acquireNextImageKHR(
                    m_swapchain, UINT64_MAX, m_acquireSemaphores[slot], nullptr);
                if (acquired.result != vk::Result::eSuccess &&
                    acquired.result != vk::Result::eSuboptimalKHR)
                    return nullptr;
                m_currentImage = acquired.value;
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Surface changed under us: rebuild at the current size and
                // skip this frame. (A throwing acquire does NOT signal the
                // semaphore -- safe to reuse.)
                m_device->Nvrhi()->waitForIdle();
                ReleaseBackbufferHandles();
                CreateSwapchainObjects();
                return nullptr;
            }

            m_device->VulkanNvrhi()->queueWaitForSemaphore(
                nvrhi::CommandQueue::Graphics, m_acquireSemaphores[slot], 0);
            m_acquired = true;
            return m_backbuffers[m_currentImage];
        }
```

`Present` becomes:

```cpp
        void SwapchainVulkan::Present()
        {
            if (!m_acquired)
                return;
            m_acquired = false;

            const uint32_t slot = (uint32_t)(m_frameCounter % kSwapchainFramesInFlight);

            m_device->VulkanNvrhi()->queueSignalSemaphore(
                nvrhi::CommandQueue::Graphics, m_presentSemaphores[slot], 0);
            // Empty submit flushes the queued semaphore signal. Must go
            // through the UNWRAPPED device: the validation wrapper
            // short-circuits executeCommandLists when numCommandLists == 0
            // and would leave the signal un-submitted.
            m_device->VulkanNvrhi()->executeCommandLists(nullptr, 0);

            auto presentInfo = vk::PresentInfoKHR()
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&m_presentSemaphores[slot])
                .setSwapchainCount(1)
                .setPSwapchains(&m_swapchain)
                .setPImageIndices(&m_currentImage);
            try
            {
                (void)m_device->GraphicsQueue().presentKHR(presentInfo);
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Rebuilt on the next BeginFrame/Resize.
            }

            // Completion point for this slot: covers the flush submit above,
            // i.e. every queue operation of this frame.
            m_device->Nvrhi()->setEventQuery(m_frameQueries[slot],
                                             nvrhi::CommandQueue::Graphics);
            ++m_frameCounter;

            m_device->Nvrhi()->runGarbageCollection();
        }
```

Destructor: replace the two `destroySemaphore` calls with a loop over both arrays (queries are `nvrhi` handles — they release themselves):

```cpp
            for (uint32_t i = 0; i < kSwapchainFramesInFlight; ++i)
            {
                if (m_acquireSemaphores[i])
                    m_device->Device().destroySemaphore(m_acquireSemaphores[i]);
                if (m_presentSemaphores[i])
                    m_device->Device().destroySemaphore(m_presentSemaphores[i]);
            }
```

(`ReleaseBackbufferHandles` keeps its `waitForIdle` — resize/teardown full drains stay.)

- [ ] **Step 5: Verify the idle-wait is gone from the Present paths**

```bash
grep -n "waitForIdle" Arcane/Arcane/src/Arcane/Render/DeviceD3D12.cpp Arcane/Arcane/src/Arcane/Render/DeviceVulkan.cpp
```
Expected: hits ONLY in release/resize/teardown/OutOfDate-rebuild paths and device destructors — none inside `Present()`.

- [ ] **Step 6: Build + run the full suite (Debug and Release)**

Standard loop, then `msbuild /p:Configuration=Release` + Release test run. Expected: ALL existing tests still pass (SwapchainTest exercises resize mid-pacing) plus the two new PacingTest cases. Validation silent. If Vulkan validation reports semaphore reuse, the slot wait isn't covering the flush submit; if D3D12 flickers garbage in later Playground work, the slot wait is gating the wrong slot index — the slot for frame N is `N % kFramesInFlight`, waited at the TOP of frame N.

- [ ] **Step 7: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): 2 frames in flight - EventQuery slot gating replaces idle-per-present"
```

---

### Task 2: Shader build pipeline + ShaderLibrary

HLSL single-source → DXC → DXIL + SPIR-V artifacts at build time; `ShaderLibrary` loads the right flavor per backend at runtime. Hot reload is Task 6 — this task lands `Poll()`/`Generation()` as working mechanics but nothing consumes them yet.

**Files:**
- Create: `Arcane/shaders/sprite.hlsl`, `Arcane/shaders/circle.hlsl`, `Arcane/shaders/tonemap.hlsl`
- Create: `Arcane/shaders/compile-shaders.bat`
- Create: `Arcane/Arcane/src/Arcane/Render/ShaderLibrary.hpp`, `ShaderLibrary.cpp`
- Modify: `Arcane/premake5.lua`, `.gitignore` (root)
- Create: `Arcane/Tests/src/ShaderLibraryTest.cpp`

- [ ] **Step 1: Write `Arcane/shaders/sprite.hlsl`**

```hlsl
// Batch shader: textured/colored quads. Untextured primitives bind the
// 1x1 white texture. Positions arrive in canvas pixels (y down); the push
// constants carry 2/viewport to reach clip space. Colors are LINEAR and
// may exceed 1.0 (HDR canvas).

#if SPIRV
    #define VK_PUSH_CONSTANT [[vk::push_constant]]
#else
    #define VK_PUSH_CONSTANT
#endif

VK_PUSH_CONSTANT cbuffer BatchConstants : register(b0)
{
    float2 g_invHalfViewport;   // 2.0 / (canvasW, canvasH)
    float2 g_pad;
};

struct VSInput
{
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                        1.0 - input.pos.y * g_invHalfViewport.y,
                        0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 ps_main(VSOutput input) : SV_Target0
{
    return g_Texture.Sample(g_Sampler, input.uv) * input.color;
}
```

- [ ] **Step 2: Write `Arcane/shaders/circle.hlsl`**

```hlsl
// SDF circle: the batcher emits a quad whose uv spans [-1, 1]; the PS
// keeps the unit disc with fwidth-based antialiasing. Shares sprite.hlsl's
// vertex layout and vs_main (compiled separately so the artifacts stay
// self-contained per pipeline).

#if SPIRV
    #define VK_PUSH_CONSTANT [[vk::push_constant]]
#else
    #define VK_PUSH_CONSTANT
#endif

VK_PUSH_CONSTANT cbuffer BatchConstants : register(b0)
{
    float2 g_invHalfViewport;
    float2 g_pad;
};

struct VSInput
{
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                        1.0 - input.pos.y * g_invHalfViewport.y,
                        0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float dist = length(input.uv);
    float aa = fwidth(dist);
    float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, dist);
    if (alpha <= 0.0)
        discard;
    return float4(input.color.rgb, input.color.a * alpha);
}
```

- [ ] **Step 3: Write `Arcane/shaders/tonemap.hlsl`**

```hlsl
// Output pass: linear HDR canvas -> ACES filmic -> gamma 2.2 encode ->
// display-referred backbuffer. The ACES approximation and 2.2 transfer
// byte-match the LOVE client's post_process.glsl (the porting oracle).
// Fullscreen triangle from SV_VertexID -- no vertex buffer.

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}

Texture2D    g_Scene   : register(t0);
SamplerState g_Sampler : register(s0);

// Narkowicz ACES filmic approximation (linear in -> [0,1] out).
float3 ACESFilmic(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float3 linearColor = g_Scene.Sample(g_Sampler, input.uv).rgb;
    float3 display = pow(ACESFilmic(linearColor), 1.0 / 2.2);
    return float4(display, 1.0);
}
```

- [ ] **Step 4: Write `Arcane/shaders/compile-shaders.bat`**

```bat
@echo off
:: Compiles every engine shader entry point to DXIL + SPIR-V loose artifacts.
:: Invoked by the premake prebuild step on the Arcane project; also runnable
:: by hand for the hot-reload dev loop (the running app picks changes up via
:: ShaderLibrary::Poll when ARCANE_SHADER_DIR points at generated\).
::
:: SPIR-V register shifts MUST match nvrhi::VulkanBindingOffsets defaults
:: (t=0, s=128, b=256, u=384). ShaderMake (vendored) replaces this script
:: when the shader count outgrows explicit lines.
setlocal
set DXC=%~dp0..\..\ThirdParty\tools\dxc\dxc.exe
set SRC=%~dp0
set OUT=%~dp0generated
if not exist "%OUT%\dxil"  mkdir "%OUT%\dxil"
if not exist "%OUT%\spirv" mkdir "%OUT%\spirv"

set SPIRV_FLAGS=-spirv -D SPIRV=1 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0

call :compile sprite  vs_main vs_6_5 sprite_vs
call :compile sprite  ps_main ps_6_5 sprite_ps
call :compile circle  vs_main vs_6_5 circle_vs
call :compile circle  ps_main ps_6_5 circle_ps
call :compile tonemap vs_main vs_6_5 tonemap_vs
call :compile tonemap ps_main ps_6_5 tonemap_ps
echo Shaders compiled to %OUT%
exit /b 0

:compile
"%DXC%" -T %3 -E %2 -Fo "%OUT%\dxil\%4.bin" "%SRC%%1.hlsl" || exit /b 1
"%DXC%" -T %3 -E %2 %SPIRV_FLAGS% -Fo "%OUT%\spirv\%4.bin" "%SRC%%1.hlsl" || exit /b 1
exit /b 0
```

Run it once by hand and verify:

```bat
Arcane\shaders\compile-shaders.bat
dir Arcane\shaders\generated\dxil Arcane\shaders\generated\spirv
```
Expected: six `.bin` files in each dir. (`fwidth` in circle.hlsl and `vk::push_constant` are the likely first-error spots; dxc errors name the line.)

- [ ] **Step 5: Wire premake — prebuild, postbuild copies, gitignore**

In `Arcane/premake5.lua`, `project "Arcane"`, add (before the `filter` blocks):

```lua
    -- Shaders are data: compiled at build time, loaded by name at runtime,
    -- hot-reloadable. The script is the single swap point for ShaderMake.
    prebuildcommands {
        'call "%{wks.location}/shaders/compile-shaders.bat"',
    }
```

In BOTH `ArcaneTests` and `Playground` projects, extend the existing `postbuildcommands` block with:

```lua
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
```

Add to `ArcaneTests` and `Playground` `includedirs`: `"%{IncludeDir.glm}",` (the batcher's public API uses glm from Task 4; adding it once now avoids premake churn). The `Arcane` project also gets `"%{IncludeDir.glm}",`.

Root `.gitignore`: append `Arcane/shaders/generated/`.

- [ ] **Step 6: Write `Arcane/Arcane/src/Arcane/Render/ShaderLibrary.hpp`**

```cpp
#pragma once

// Render module: loads compiled shader artifacts (DXIL or SPIR-V chosen by
// the device's backend) by name from a directory of loose .bin files.
// Poll() re-stats the files and reloads changed ones (hot reload);
// Generation() bumps on every reload so pipeline caches can lazily rebuild.
// The ARCANE_SHADER_DIR environment variable overrides the directory --
// point it at Arcane/shaders/generated for the recompile-while-running loop.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Device.hpp>

#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace Arcane
{
    class ARCANE_API ShaderLibrary
    {
    public:
        // Returns null (with ARC_ERROR) when the directory is missing.
        static std::unique_ptr<ShaderLibrary> Create(
            nvrhi::IDevice* device, GraphicsBackend backend,
            const std::filesystem::path& shaderDir);

        virtual ~ShaderLibrary() = default;

        // Loads (and caches) "<dir>/<dxil|spirv>/<name>.bin". Returns null
        // with ARC_ERROR when the artifact is missing or unreadable.
        virtual nvrhi::ShaderHandle Get(std::string_view name,
                                        nvrhi::ShaderType type) = 0;

        // Re-stats every loaded artifact; reloads the changed ones.
        // Returns true when anything reloaded (Generation() bumped).
        virtual bool Poll() = 0;

        // Monotonic; starts at 1. Pipeline caches compare and rebuild.
        virtual uint64_t Generation() const = 0;
    };
}
```

- [ ] **Step 7: Write `Arcane/Arcane/src/Arcane/Render/ShaderLibrary.cpp`**

```cpp
#include <Arcane/Render/ShaderLibrary.hpp>

#include <Arcane/Base/Log.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    namespace
    {
        std::vector<char> ReadFileBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<char> bytes((size_t)size);
            if (!file.read(bytes.data(), size))
                return {};
            return bytes;
        }

        class ShaderLibraryImpl final : public ShaderLibrary
        {
        public:
            ShaderLibraryImpl(nvrhi::IDevice* device,
                              std::filesystem::path flavorDir)
                : m_device(device), m_flavorDir(std::move(flavorDir))
            {
            }

            nvrhi::ShaderHandle Get(std::string_view name,
                                    nvrhi::ShaderType type) override
            {
                const std::string key(name);
                auto it = m_entries.find(key);
                if (it != m_entries.end())
                    return it->second.handle;

                Entry entry;
                entry.path = m_flavorDir / (key + ".bin");
                entry.type = type;
                if (!LoadEntry(entry))
                    return nullptr;
                m_entries.emplace(key, entry);
                return entry.handle;
            }

            bool Poll() override
            {
                bool reloaded = false;
                for (auto& [name, entry] : m_entries)
                {
                    std::error_code ec;
                    const auto stamp =
                        std::filesystem::last_write_time(entry.path, ec);
                    if (ec || stamp == entry.stamp)
                        continue;
                    Entry fresh = entry;
                    if (LoadEntry(fresh))
                    {
                        entry = fresh;
                        reloaded = true;
                        ARC_INFO("Shader reloaded: {}", name);
                    }
                }
                if (reloaded)
                    ++m_generation;
                return reloaded;
            }

            uint64_t Generation() const override { return m_generation; }

        private:
            struct Entry
            {
                std::filesystem::path path;
                std::filesystem::file_time_type stamp{};
                nvrhi::ShaderType type = nvrhi::ShaderType::None;
                nvrhi::ShaderHandle handle;
            };

            bool LoadEntry(Entry& entry)
            {
                const std::vector<char> bytes = ReadFileBytes(entry.path);
                if (bytes.empty())
                {
                    ARC_ERROR("Shader artifact missing/unreadable: {}",
                              entry.path.string());
                    return false;
                }
                auto desc = nvrhi::ShaderDesc()
                    .setShaderType(entry.type)
                    .setDebugName(entry.path.filename().string());
                entry.handle =
                    m_device->createShader(desc, bytes.data(), bytes.size());
                if (!entry.handle)
                {
                    ARC_ERROR("createShader failed: {}", entry.path.string());
                    return false;
                }
                std::error_code ec;
                entry.stamp = std::filesystem::last_write_time(entry.path, ec);
                return true;
            }

            nvrhi::IDevice* m_device;
            std::filesystem::path m_flavorDir;
            std::unordered_map<std::string, Entry> m_entries;
            uint64_t m_generation = 1;
        };
    }

    std::unique_ptr<ShaderLibrary> ShaderLibrary::Create(
        nvrhi::IDevice* device, GraphicsBackend backend,
        const std::filesystem::path& shaderDir)
    {
        std::filesystem::path dir = shaderDir;
        if (const char* overrideDir = std::getenv("ARCANE_SHADER_DIR"))
            dir = overrideDir;
        dir /= (backend == GraphicsBackend::Vulkan) ? "spirv" : "dxil";

        if (!std::filesystem::is_directory(dir))
        {
            ARC_ERROR("Shader directory not found: {}", dir.string());
            return nullptr;
        }
        return std::make_unique<ShaderLibraryImpl>(device, dir);
    }
}
```

(Verify `nvrhi::ShaderDesc` setter names against nvrhi.h — `setShaderType`/`setDebugName` are the expected fluent setters; adapt minimally if the vendored version differs, noting it.)

- [ ] **Step 8: Write the test `Arcane/Tests/src/ShaderLibraryTest.cpp`**

```cpp
// Shader artifacts load on both backends. (The hot-reload mechanism test
// joins this file in the hot-reload task.)

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    void CheckShaderLoads(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "shaders");
        REQUIRE(shaders != nullptr);
        REQUIRE(shaders->Generation() == 1);

        REQUIRE(shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel) != nullptr);
        REQUIRE(shaders->Get("circle_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("circle_ps", nvrhi::ShaderType::Pixel) != nullptr);
        REQUIRE(shaders->Get("tonemap_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("tonemap_ps", nvrhi::ShaderType::Pixel) != nullptr);

        // Cached: same handle back.
        REQUIRE(shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex) ==
                shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex));

        // Missing artifact: null, not a crash. (Logs one ARC_ERROR -- that is
        // an engine log, not an NVRHI validation error; RenderErrorCount is
        // unaffected.)
        REQUIRE(shaders->Get("does_not_exist", nvrhi::ShaderType::Pixel) == nullptr);

        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: shader artifacts load", "[gpu][d3d12]")
{
    CheckShaderLoads(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: shader artifacts load", "[gpu][vulkan]")
{
    CheckShaderLoads(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 9: Build + run (the prebuild step compiles shaders; postbuild copies them next to the test exe)**

Standard loop. Expected: the two new cases pass on both backends. If `createShader` fails on Vulkan only, the SPIR-V register shifts or the `SPIRV` define didn't apply — check the `compile-shaders.bat` SPIRV_FLAGS line.

- [ ] **Step 10: Commit**

```bash
git add -A Arcane/ .gitignore && git commit -m "feat(arcane): shader build pipeline (DXC dual-target) + ShaderLibrary"
```

---

### Task 3: Canvas (linear HDR target) + TonemapPass + ACES golden readback

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Render/Canvas.hpp`, `Canvas.cpp`
- Create: `Arcane/Arcane/src/Arcane/Render/TonemapPass.hpp`, `TonemapPass.cpp`
- Create: `Arcane/Tests/src/TonemapTest.cpp`

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/TonemapTest.cpp`**

The strongest possible proof the color pipeline is right: clear the canvas to a known LINEAR color (including one channel > 1.0 to prove HDR rolloff), run the tonemap into an offscreen BGRA8 target (same format family as the backbuffer), read it back, and compare against the SAME math run on the CPU.

```cpp
// Color-pipeline golden test: linear HDR canvas -> ACES -> 2.2 encode must
// byte-match the CPU reference (the client post_process.glsl oracle math).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/TonemapPass.hpp>

namespace
{
    float AcesFilmic(float x)
    {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
    }

    uint8_t ExpectedByte(float linearChannel)
    {
        const float display = std::pow(AcesFilmic(linearChannel), 1.0f / 2.2f);
        return (uint8_t)std::lround(display * 255.0f);
    }

    void CheckTonemapGolden(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "shaders");
        REQUIRE(shaders != nullptr);

        auto canvas = Arcane::CreateCanvas(nv, 8, 8);
        REQUIRE(canvas != nullptr);
        auto tonemap = Arcane::TonemapPass::Create(nv, *shaders);
        REQUIRE(tonemap != nullptr);

        // Display-referred output target standing in for the backbuffer.
        auto outDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true)
            .setDebugName("TonemapOut");
        nvrhi::TextureHandle output = nv->createTexture(outDesc);
        REQUIRE(output != nullptr);
        nvrhi::FramebufferHandle outputFb = nv->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(output));
        REQUIRE(outputFb != nullptr);

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("TonemapReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);

        // 1.5 in red proves the HDR rolloff (would clip without ACES).
        const float lin[3] = { 1.5f, 0.5f, 0.1f };

        nvrhi::CommandListHandle commandList = nv->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(lin[0], lin[1], lin[2], 1.0f));
        tonemap->Run(commandList, canvas->Texture(), outputFb);
        commandList->copyTexture(staging, nvrhi::TextureSlice(),
                                 output, nvrhi::TextureSlice());
        commandList->close();
        nv->executeCommandList(commandList);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        // BGRA byte order; +-2 tolerance (fp16 canvas storage + UNORM rounding).
        CHECK(std::abs((int)pixels[2] - (int)ExpectedByte(lin[0])) <= 2);
        CHECK(std::abs((int)pixels[1] - (int)ExpectedByte(lin[1])) <= 2);
        CHECK(std::abs((int)pixels[0] - (int)ExpectedByte(lin[2])) <= 2);
        CHECK((int)pixels[3] == 255);
        nv->unmapStagingTexture(staging);
        nv->runGarbageCollection();

        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: ACES tonemap matches CPU reference", "[gpu][d3d12]")
{
    CheckTonemapGolden(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: ACES tonemap matches CPU reference", "[gpu][vulkan]")
{
    CheckTonemapGolden(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Build — confirm compile FAILURE** (Canvas.hpp / TonemapPass.hpp missing).

- [ ] **Step 3: Write `Arcane/Arcane/src/Arcane/Render/Canvas.hpp`**

```cpp
#pragma once

// Render module: the linear-HDR scene target. ALL scene rendering happens
// here in linear space (RGBA16_FLOAT); only TonemapPass writes the
// display-referred backbuffer. Resize recreates texture + framebuffer.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>

namespace Arcane
{
    class ARCANE_API Canvas
    {
    public:
        virtual ~Canvas() = default;

        virtual nvrhi::ITexture* Texture() const = 0;
        virtual nvrhi::IFramebuffer* Framebuffer() const = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
    };

    // Returns null (with ARC_ERROR) on creation failure.
    ARCANE_API std::unique_ptr<Canvas> CreateCanvas(nvrhi::IDevice* device,
                                                    uint32_t width,
                                                    uint32_t height);
}
```

- [ ] **Step 4: Write `Arcane/Arcane/src/Arcane/Render/Canvas.cpp`**

```cpp
#include <Arcane/Render/Canvas.hpp>

#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    namespace
    {
        constexpr nvrhi::Format kCanvasFormat = nvrhi::Format::RGBA16_FLOAT;

        class CanvasImpl final : public Canvas
        {
        public:
            explicit CanvasImpl(nvrhi::IDevice* device) : m_device(device) {}

            bool Init(uint32_t width, uint32_t height)
            {
                m_width = width;
                m_height = height;
                auto desc = nvrhi::TextureDesc()
                    .setWidth(width)
                    .setHeight(height)
                    .setFormat(kCanvasFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::RenderTarget)
                    .setKeepInitialState(true)
                    .setDebugName("Canvas");
                m_texture = m_device->createTexture(desc);
                if (!m_texture)
                {
                    ARC_ERROR("Canvas texture creation failed ({}x{})", width, height);
                    return false;
                }
                m_framebuffer = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_texture));
                if (!m_framebuffer)
                {
                    ARC_ERROR("Canvas framebuffer creation failed");
                    return false;
                }
                return true;
            }

            nvrhi::ITexture* Texture() const override { return m_texture; }
            nvrhi::IFramebuffer* Framebuffer() const override { return m_framebuffer; }
            uint32_t Width() const override { return m_width; }
            uint32_t Height() const override { return m_height; }

            void Resize(uint32_t width, uint32_t height) override
            {
                if ((width == m_width && height == m_height) ||
                    width == 0 || height == 0)
                    return;
                // The caller owns frame pacing; a resize mid-flight must not
                // free a texture the GPU still reads.
                m_device->waitForIdle();
                m_framebuffer = nullptr;
                m_texture = nullptr;
                m_device->runGarbageCollection();
                Init(width, height);
            }

        private:
            nvrhi::IDevice* m_device;
            nvrhi::TextureHandle m_texture;
            nvrhi::FramebufferHandle m_framebuffer;
            uint32_t m_width = 0;
            uint32_t m_height = 0;
        };
    }

    std::unique_ptr<Canvas> CreateCanvas(nvrhi::IDevice* device,
                                         uint32_t width, uint32_t height)
    {
        auto canvas = std::make_unique<CanvasImpl>(device);
        if (!canvas->Init(width, height))
            return nullptr;
        return canvas;
    }
}
```

- [ ] **Step 5: Write `Arcane/Arcane/src/Arcane/Render/TonemapPass.hpp`**

```cpp
#pragma once

// Render module: the ONLY writer of display-referred output. Samples a
// linear HDR texture, applies Narkowicz ACES + gamma 2.2 (the client
// oracle), draws a fullscreen triangle into the target framebuffer.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    class ARCANE_API TonemapPass
    {
    public:
        // Returns null (with ARC_ERROR) when the tonemap shaders are missing.
        static std::unique_ptr<TonemapPass> Create(nvrhi::IDevice* device,
                                                   ShaderLibrary& shaders);

        virtual ~TonemapPass() = default;

        // Records the fullscreen pass into an OPEN command list. The source
        // must be shader-readable; the target framebuffer is typically the
        // backbuffer. Pipelines rebuild lazily on shader Generation() bumps.
        virtual void Run(nvrhi::ICommandList* commandList,
                         nvrhi::ITexture* source,
                         nvrhi::IFramebuffer* target) = 0;
    };
}
```

- [ ] **Step 6: Write `Arcane/Arcane/src/Arcane/Render/TonemapPass.cpp`**

```cpp
#include <Arcane/Render/TonemapPass.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <unordered_map>

namespace Arcane
{
    namespace
    {
        class TonemapPassImpl final : public TonemapPass
        {
        public:
            TonemapPassImpl(nvrhi::IDevice* device, ShaderLibrary& shaders)
                : m_device(device), m_shaders(shaders)
            {
            }

            bool Init()
            {
                auto samplerDesc = nvrhi::SamplerDesc()
                    .setAllFilters(false)  // point: 1:1 canvas -> target
                    .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
                m_sampler = m_device->createSampler(samplerDesc);

                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);

                return m_sampler != nullptr && m_bindingLayout != nullptr &&
                       m_shaders.Get("tonemap_vs", nvrhi::ShaderType::Vertex) &&
                       m_shaders.Get("tonemap_ps", nvrhi::ShaderType::Pixel);
            }

            void Run(nvrhi::ICommandList* commandList, nvrhi::ITexture* source,
                     nvrhi::IFramebuffer* target) override
            {
                nvrhi::IGraphicsPipeline* pipeline = GetPipeline(target);
                if (!pipeline)
                    return;

                nvrhi::BindingSetHandle& bindingSet = m_bindingSets[source];
                if (!bindingSet)
                {
                    bindingSet = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
                            .addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler)),
                        m_bindingLayout);
                }

                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                auto state = nvrhi::GraphicsState()
                    .setPipeline(pipeline)
                    .setFramebuffer(target)
                    .addBindingSet(bindingSet);
                state.viewport.addViewportAndScissorRect(
                    nvrhi::Viewport((float)fbInfo.width, (float)fbInfo.height));
                commandList->setGraphicsState(state);
                commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
            }

        private:
            nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* target)
            {
                // Lazy rebuild on hot reload: a generation bump invalidates
                // the whole cache (cheap -- one pipeline per target format).
                if (m_pipelineGeneration != m_shaders.Generation())
                {
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders.Generation();
                }
                const size_t key = target->getFramebufferInfo().getHash();
                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    nvrhi::ShaderHandle vs =
                        m_shaders.Get("tonemap_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps =
                        m_shaders.Get("tonemap_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("Tonemap shaders unavailable");
                        return nullptr;
                    }
                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    pipeline = m_device->createGraphicsPipeline(
                        desc, target->getFramebufferInfo());
                }
                return pipeline;
            }

            nvrhi::IDevice* m_device;
            ShaderLibrary& m_shaders;
            nvrhi::SamplerHandle m_sampler;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_bindingSets;
            uint64_t m_pipelineGeneration = 0;
        };
    }

    std::unique_ptr<TonemapPass> TonemapPass::Create(nvrhi::IDevice* device,
                                                     ShaderLibrary& shaders)
    {
        auto pass = std::make_unique<TonemapPassImpl>(device, shaders);
        if (!pass->Init())
            return nullptr;
        return pass;
    }
}
```

API notes to verify against `nvrhi.h` while implementing (adapt minimally + report): `BindingLayoutItem::Texture_SRV(slot)` / `BindingSetItem::Texture_SRV(slot, tex)` / `BindingSetItem::Sampler(slot, sampler)` are the expected static constructors; `IFramebuffer::getFramebufferInfo()` returns `FramebufferInfoEx` with `width`/`height`/`getHash()`; `GraphicsState.viewport` is a `ViewportState` with `addViewportAndScissorRect`; `SamplerDesc::setAllFilters(bool)`; `createGraphicsPipeline(desc, FramebufferInfo const&)` (the non-deprecated overload). The binding-set map keyed on raw `ITexture*` holds strong handles in values — entries for destroyed textures leak one binding set until the pass dies; acceptable for M2a (one canvas), revisit when canvases multiply.

- [ ] **Step 7: Build + run** — standard loop. Expected: both golden tests pass within ±2. A wildly wrong red channel (e.g. 255) means tonemap didn't run (HDR clipped); a uniformly dark/bright image means the 2.2 encode is missing or doubled — compare against `ExpectedByte` values printed via `WARN()` if needed.

- [ ] **Step 8: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): linear HDR Canvas + ACES TonemapPass, CPU-reference golden tests"
```

---

### Task 4: Batcher2D — textured/colored quads

The single submission path for all 2D geometry. CPU-side vertex accumulation, batch breaks on texture/pipeline change, one `writeBuffer` + N draws at `End()`. NVRHI's upload manager versions the buffer writes per command-list instance, so frames-in-flight need no manual ring buffer.

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Render/Batcher2D.hpp`, `Batcher2D.cpp`
- Create: `Arcane/Tests/src/BatcherTest.cpp`

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/BatcherTest.cpp`**

```cpp
// Batcher GPU readbacks: a quad covering the whole canvas must write its
// exact color (alpha-blend with a == 1 is a passthrough); stats must count
// what was submitted.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    struct GpuFixture
    {
        std::unique_ptr<Arcane::RenderDevice> device;
        std::unique_ptr<Arcane::ShaderLibrary> shaders;
        std::unique_ptr<Arcane::Canvas> canvas;
        std::unique_ptr<Arcane::Batcher2D> batcher;
        nvrhi::StagingTextureHandle staging;
        nvrhi::CommandListHandle commandList;

        explicit GpuFixture(Arcane::GraphicsBackend backend, uint32_t size = 8)
        {
            Arcane::RenderDeviceDesc desc;
            desc.backend = backend;
            device = Arcane::RenderDevice::Create(desc);
            REQUIRE(device != nullptr);
            shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                    "shaders");
            REQUIRE(shaders != nullptr);
            canvas = Arcane::CreateCanvas(device->Nvrhi(), size, size);
            REQUIRE(canvas != nullptr);
            batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
            REQUIRE(batcher != nullptr);

            auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(size).setHeight(size)
                .setFormat(nvrhi::Format::RGBA16_FLOAT)
                .setDebugName("BatcherReadback");
            staging = device->Nvrhi()->createStagingTexture(
                stagingDesc, nvrhi::CpuAccessMode::Read);
            commandList = device->Nvrhi()->createCommandList();
        }

        // Renders whatever the caller batched, copies the canvas to staging.
        void Flush()
        {
            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            device->Nvrhi()->waitForIdle();
        }
    };

    // fp16 -> float for readback checks.
    float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = (uint32_t)(h >> 15) & 1;
        const uint32_t expo = (uint32_t)(h >> 10) & 0x1F;
        const uint32_t mant = (uint32_t)h & 0x3FF;
        if (expo == 0)
            return (sign ? -1.0f : 1.0f) * (float)mant * 5.9604645e-8f;
        if (expo == 31)
            return sign ? -65504.0f : 65504.0f;  // inf/nan clamped; unused here
        const uint32_t bits = (sign << 31) | ((expo - 15 + 127) << 23) | (mant << 13);
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    void CheckPixel(const uint8_t* base, size_t rowPitch, uint32_t x, uint32_t y,
                    float r, float g, float b, float tolerance = 0.02f)
    {
        const auto* texel = reinterpret_cast<const uint16_t*>(
            base + y * rowPitch + x * 8);  // RGBA16F = 8 bytes/texel
        CHECK(std::abs(HalfToFloat(texel[0]) - r) <= tolerance);
        CHECK(std::abs(HalfToFloat(texel[1]) - g) <= tolerance);
        CHECK(std::abs(HalfToFloat(texel[2]) - b) <= tolerance);
    }

    void CheckQuadFillsCanvas(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(),
                          fx.canvas->Width(), fx.canvas->Height());
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8),
                         glm::vec4(0.9f, 0.4f, 0.1f, 1.0f));
        fx.batcher->End();
        const Arcane::Batch2DStats stats = fx.batcher->Stats();
        CHECK(stats.quads == 1);
        CHECK(stats.drawCalls == 1);
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 0, 0, 0.9f, 0.4f, 0.1f);
        CheckPixel(pixels, rowPitch, 7, 7, 0.9f, 0.4f, 0.1f);
        CheckPixel(pixels, rowPitch, 4, 4, 0.9f, 0.4f, 0.1f);
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();

        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

namespace
{
    // Sort keys do two jobs at once: layer ordering wins over submission
    // order, and same-state draws coalesce ACROSS layer interleaves.
    void CheckSortKeys(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 8, 8);
        // Submitted top layer FIRST: without sorting, green would cover red.
        fx.batcher->SetLayer(1, 0);
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8),
                         glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        fx.batcher->SetLayer(0, 0);
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8),
                         glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
        fx.batcher->End();

        // Same kind + texture (white) on both layers: ONE coalesced draw.
        const Arcane::Batch2DStats stats = fx.batcher->Stats();
        CHECK(stats.quads == 2);
        CHECK(stats.drawCalls == 1);

        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 4, 4, 1.0f, 0.0f, 0.0f);  // layer 1 on top
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: batcher quad fills the canvas; sort keys order and coalesce", "[gpu][d3d12]")
{
    CheckQuadFillsCanvas(Arcane::GraphicsBackend::D3D12);
    CheckSortKeys(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: batcher quad fills the canvas; sort keys order and coalesce", "[gpu][vulkan]")
{
    CheckQuadFillsCanvas(Arcane::GraphicsBackend::Vulkan);
    CheckSortKeys(Arcane::GraphicsBackend::Vulkan);
}
```

(`<cstring>` for memcpy. The fixture and helpers are reused by Task 5's primitive tests — keep them in this file's anonymous namespace.)

- [ ] **Step 2: Build — confirm compile FAILURE** (Batcher2D.hpp missing).

- [ ] **Step 3: Write `Arcane/Arcane/src/Arcane/Render/Batcher2D.hpp`**

```cpp
#pragma once

// Render module: THE 2D submission path. Sprites, rects, lines, circles --
// and, in M2b, text glyphs and ImGui -- all flow through this batcher into
// the same vertex stream and pipeline family. No bespoke draw paths grow
// beside it (homogenized-rendering mandate).
//
// Coordinates are canvas pixels, y down. Colors are LINEAR floats and may
// exceed 1.0 (HDR canvas). Blending is straight (non-premultiplied) alpha
// to match the LOVE client's semantics for 1:1 screen ports; migrating to
// premultiplied is a deliberate future decision.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    struct Batch2DStats
    {
        uint32_t drawCalls = 0;
        uint32_t quads = 0;  // every primitive is quads under the hood
    };

    class ARCANE_API Batcher2D
    {
    public:
        // Returns null (with ARC_ERROR) when the batch shaders are missing.
        static std::unique_ptr<Batcher2D> Create(nvrhi::IDevice* device,
                                                 ShaderLibrary& shaders);

        virtual ~Batcher2D() = default;

        // Begin/End bracket one target per command list recording. The
        // command list must be open; End() records the draws.
        virtual void Begin(nvrhi::ICommandList* commandList,
                           nvrhi::IFramebuffer* target,
                           uint32_t viewportWidth,
                           uint32_t viewportHeight) = 0;

        // Sorting: every draw carries the current (layer, orderInLayer).
        // End() stable-sorts draws by a 64-bit key -- layer(16) | order(16)
        // | pipelineKind(8) | textureSlot(16) -- giving correct transparency
        // ordering AND minimal state changes in one pass. Draws sharing
        // (layer, order) may be reordered for batching; give overlapping
        // content distinct orders. Resets to (0, 0) at Begin().
        virtual void SetLayer(uint16_t layer, uint16_t orderInLayer) = 0;

        // Textured quad: dstPos/dstSize in pixels, uvMin/uvMax in [0,1].
        virtual void Quad(glm::vec2 dstPos, glm::vec2 dstSize,
                          nvrhi::ITexture* texture,
                          glm::vec2 uvMin, glm::vec2 uvMax,
                          glm::vec4 color) = 0;

        // Untextured primitives (white-texture quads / SDF circle quads).
        virtual void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color) = 0;
        virtual void Line(glm::vec2 a, glm::vec2 b, float thickness,
                          glm::vec4 color) = 0;
        virtual void Circle(glm::vec2 center, float radius, glm::vec4 color) = 0;

        virtual void End() = 0;

        // Stats for the most recently End()ed batch.
        virtual Batch2DStats Stats() const = 0;
    };
}
```

- [ ] **Step 4: Write `Arcane/Arcane/src/Arcane/Render/Batcher2D.cpp`** (quads only this task; `Line`/`Circle` land in Task 5 — this task they assert-log and no-op so the interface stays honest)

```cpp
#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct Vertex
        {
            glm::vec2 pos;
            glm::vec2 uv;
            glm::vec4 color;
        };
        static_assert(sizeof(Vertex) == 32, "vertex layout is the wire format");

        struct PushConstants
        {
            glm::vec2 invHalfViewport;
            glm::vec2 pad;
        };

        enum class BatchKind : uint8_t { Sprite, Circle };

        // One recorded draw (a quad: 4 vertices already in m_vertices).
        // End() stable-sorts records by key, then builds index data and
        // batch runs in sorted order. This is the v1 "compile" -- batcher
        // v2 compiles the same records into per-instance data instead.
        struct DrawRecord
        {
            uint64_t key = 0;
            uint32_t firstVertex = 0;
            BatchKind kind = BatchKind::Sprite;
            nvrhi::ITexture* texture = nullptr;
        };

        // One contiguous run of sorted records sharing kind + texture.
        struct BatchRun
        {
            BatchKind kind = BatchKind::Sprite;
            nvrhi::ITexture* texture = nullptr;
            uint32_t firstIndex = 0;
            uint32_t indexCount = 0;
        };

        class Batcher2DImpl final : public Batcher2D
        {
        public:
            Batcher2DImpl(nvrhi::IDevice* device, ShaderLibrary& shaders)
                : m_device(device), m_shaders(shaders)
            {
            }

            bool Init()
            {
                // 1x1 white texture: the untextured path.
                auto whiteDesc = nvrhi::TextureDesc()
                    .setWidth(1).setHeight(1)
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true)
                    .setDebugName("BatcherWhite");
                m_whiteTexture = m_device->createTexture(whiteDesc);

                auto samplerDesc = nvrhi::SamplerDesc()
                    .setAllFilters(true)  // linear: sprites scale smoothly
                    .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
                m_sampler = m_device->createSampler(samplerDesc);

                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::All)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(
                        0, sizeof(PushConstants)))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);

                const nvrhi::VertexAttributeDesc attributes[] = {
                    nvrhi::VertexAttributeDesc()
                        .setName("POSITION")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(Vertex, pos))
                        .setElementStride(sizeof(Vertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("TEXCOORD")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(Vertex, uv))
                        .setElementStride(sizeof(Vertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("COLOR")
                        .setFormat(nvrhi::Format::RGBA32_FLOAT)
                        .setOffset(offsetof(Vertex, color))
                        .setElementStride(sizeof(Vertex)),
                };
                nvrhi::ShaderHandle spriteVs =
                    m_shaders.Get("sprite_vs", nvrhi::ShaderType::Vertex);
                if (!spriteVs)
                    return false;
                m_inputLayout = m_device->createInputLayout(
                    attributes, (uint32_t)std::size(attributes), spriteVs);

                return m_whiteTexture && m_sampler && m_bindingLayout &&
                       m_inputLayout &&
                       m_shaders.Get("sprite_ps", nvrhi::ShaderType::Pixel) &&
                       m_shaders.Get("circle_vs", nvrhi::ShaderType::Vertex) &&
                       m_shaders.Get("circle_ps", nvrhi::ShaderType::Pixel);
            }

            void Begin(nvrhi::ICommandList* commandList,
                       nvrhi::IFramebuffer* target,
                       uint32_t viewportWidth, uint32_t viewportHeight) override
            {
                m_commandList = commandList;
                m_target = target;
                m_viewport = glm::vec2((float)viewportWidth, (float)viewportHeight);
                m_vertices.clear();
                m_indices.clear();
                m_runs.clear();
                m_records.clear();
                m_textureSlots.clear();
                m_textureSlotLookup.clear();
                m_layer = 0;
                m_order = 0;
                if (!m_whiteUploaded)
                    UploadWhiteTexture();
            }

            void SetLayer(uint16_t layer, uint16_t orderInLayer) override
            {
                m_layer = layer;
                m_order = orderInLayer;
            }

            void Quad(glm::vec2 dstPos, glm::vec2 dstSize,
                      nvrhi::ITexture* texture, glm::vec2 uvMin, glm::vec2 uvMax,
                      glm::vec4 color) override
            {
                PushQuad(BatchKind::Sprite, texture ? texture : m_whiteTexture.Get(),
                         dstPos, dstSize, uvMin, uvMax, color);
            }

            void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color) override
            {
                PushQuad(BatchKind::Sprite, m_whiteTexture.Get(),
                         pos, size, glm::vec2(0), glm::vec2(1), color);
            }

            void Line(glm::vec2, glm::vec2, float, glm::vec4) override
            {
                ARC_ERROR("Batcher2D::Line lands with the primitives task");
            }

            void Circle(glm::vec2, float, glm::vec4) override
            {
                ARC_ERROR("Batcher2D::Circle lands with the primitives task");
            }

            void End() override
            {
                m_stats = {};
                if (m_records.empty() || !m_commandList)
                {
                    m_commandList = nullptr;
                    return;
                }

                // The sort-key pass: correct transparency ordering (layer,
                // order) AND minimal state changes (kind, texture) in one
                // sort. stable_sort keeps submission order on identical keys.
                std::stable_sort(m_records.begin(), m_records.end(),
                                 [](const DrawRecord& a, const DrawRecord& b)
                                 { return a.key < b.key; });

                m_indices.reserve(m_records.size() * 6);
                for (const DrawRecord& record : m_records)
                {
                    if (m_runs.empty() || m_runs.back().kind != record.kind ||
                        m_runs.back().texture != record.texture)
                    {
                        BatchRun run;
                        run.kind = record.kind;
                        run.texture = record.texture;
                        run.firstIndex = (uint32_t)m_indices.size();
                        m_runs.push_back(run);
                    }
                    const uint32_t base = record.firstVertex;
                    const uint32_t quadIndices[6] = { base, base + 1, base + 2,
                                                      base, base + 2, base + 3 };
                    m_indices.insert(m_indices.end(), quadIndices,
                                     quadIndices + 6);
                    m_runs.back().indexCount += 6;
                }

                EnsureBuffers();
                m_commandList->writeBuffer(m_vertexBuffer, m_vertices.data(),
                                           m_vertices.size() * sizeof(Vertex));
                m_commandList->writeBuffer(m_indexBuffer, m_indices.data(),
                                           m_indices.size() * sizeof(uint32_t));

                const PushConstants push{
                    glm::vec2(2.0f / m_viewport.x, 2.0f / m_viewport.y),
                    glm::vec2(0.0f) };

                for (const BatchRun& run : m_runs)
                {
                    nvrhi::IGraphicsPipeline* pipeline = GetPipeline(run.kind);
                    if (!pipeline)
                        continue;
                    auto state = nvrhi::GraphicsState()
                        .setPipeline(pipeline)
                        .setFramebuffer(m_target)
                        .addBindingSet(GetBindingSet(run.texture))
                        .setIndexBuffer({ m_indexBuffer, nvrhi::Format::R32_UINT, 0 })
                        .addVertexBuffer({ m_vertexBuffer, 0, 0 });
                    state.viewport.addViewportAndScissorRect(
                        nvrhi::Viewport(m_viewport.x, m_viewport.y));
                    m_commandList->setGraphicsState(state);
                    m_commandList->setPushConstants(&push, sizeof(push));
                    m_commandList->drawIndexed(nvrhi::DrawArguments()
                        .setVertexCount(run.indexCount)
                        .setStartIndexLocation(run.firstIndex));
                    ++m_stats.drawCalls;
                }
                m_stats.quads = (uint32_t)m_records.size();
                m_commandList = nullptr;
            }

            Batch2DStats Stats() const override { return m_stats; }

        private:
            void UploadWhiteTexture()
            {
                const uint32_t white = 0xFFFFFFFFu;
                m_commandList->writeTexture(m_whiteTexture, 0, 0, &white, 4);
                m_whiteUploaded = true;
            }

            uint16_t TextureSlot(nvrhi::ITexture* texture)
            {
                auto [it, inserted] = m_textureSlotLookup.try_emplace(
                    texture, (uint16_t)m_textureSlots.size());
                if (inserted)
                    m_textureSlots.push_back(texture);
                return it->second;
            }

            void PushQuadVertices(BatchKind kind, nvrhi::ITexture* texture,
                                  const Vertex& v0, const Vertex& v1,
                                  const Vertex& v2, const Vertex& v3)
            {
                DrawRecord record;
                record.key = ((uint64_t)m_layer << 48) |
                             ((uint64_t)m_order << 32) |
                             ((uint64_t)kind << 24) |
                             (uint64_t)TextureSlot(texture);
                record.firstVertex = (uint32_t)m_vertices.size();
                record.kind = kind;
                record.texture = texture;
                m_records.push_back(record);
                m_vertices.push_back(v0);
                m_vertices.push_back(v1);
                m_vertices.push_back(v2);
                m_vertices.push_back(v3);
            }

            void PushQuad(BatchKind kind, nvrhi::ITexture* texture,
                          glm::vec2 pos, glm::vec2 size,
                          glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color)
            {
                PushQuadVertices(kind, texture,
                    { pos, uvMin, color },
                    { { pos.x + size.x, pos.y }, { uvMax.x, uvMin.y }, color },
                    { pos + size, uvMax, color },
                    { { pos.x, pos.y + size.y }, { uvMin.x, uvMax.y }, color });
            }

            void EnsureBuffers()
            {
                const size_t vertexBytes = m_vertices.size() * sizeof(Vertex);
                const size_t indexBytes = m_indices.size() * sizeof(uint32_t);
                if (!m_vertexBuffer ||
                    m_vertexBuffer->getDesc().byteSize < vertexBytes)
                {
                    m_vertexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                        .setByteSize(std::max<size_t>(vertexBytes, 64 * 1024))
                        .setIsVertexBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::VertexBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("Batcher2D.VB"));
                }
                if (!m_indexBuffer ||
                    m_indexBuffer->getDesc().byteSize < indexBytes)
                {
                    m_indexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                        .setByteSize(std::max<size_t>(indexBytes, 32 * 1024))
                        .setIsIndexBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::IndexBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("Batcher2D.IB"));
                }
            }

            nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture)
            {
                nvrhi::BindingSetHandle& set = m_bindingSets[texture];
                if (!set)
                {
                    set = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::PushConstants(
                                0, sizeof(PushConstants)))
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
                            .addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler)),
                        m_bindingLayout);
                }
                return set;
            }

            nvrhi::IGraphicsPipeline* GetPipeline(BatchKind kind)
            {
                if (m_pipelineGeneration != m_shaders.Generation())
                {
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders.Generation();
                }
                const size_t key =
                    m_target->getFramebufferInfo().getHash() * 2 + (size_t)kind;
                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    const bool circle = (kind == BatchKind::Circle);
                    nvrhi::ShaderHandle vs = m_shaders.Get(
                        circle ? "circle_vs" : "sprite_vs",
                        nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps = m_shaders.Get(
                        circle ? "circle_ps" : "sprite_ps",
                        nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                        return nullptr;

                    nvrhi::BlendState::RenderTarget blend;
                    blend.enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .setInputLayout(m_inputLayout)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    desc.renderState.blendState.setRenderTarget(0, blend);
                    pipeline = m_device->createGraphicsPipeline(
                        desc, m_target->getFramebufferInfo());
                }
                return pipeline;
            }

            nvrhi::IDevice* m_device;
            ShaderLibrary& m_shaders;
            nvrhi::TextureHandle m_whiteTexture;
            nvrhi::SamplerHandle m_sampler;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            nvrhi::InputLayoutHandle m_inputLayout;
            nvrhi::BufferHandle m_vertexBuffer;
            nvrhi::BufferHandle m_indexBuffer;
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_bindingSets;
            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            uint64_t m_pipelineGeneration = 0;

            nvrhi::ICommandList* m_commandList = nullptr;
            nvrhi::IFramebuffer* m_target = nullptr;
            glm::vec2 m_viewport{ 0.0f };
            std::vector<Vertex> m_vertices;
            std::vector<uint32_t> m_indices;
            std::vector<DrawRecord> m_records;
            std::vector<BatchRun> m_runs;
            std::vector<nvrhi::ITexture*> m_textureSlots;
            std::unordered_map<nvrhi::ITexture*, uint16_t> m_textureSlotLookup;
            uint16_t m_layer = 0;
            uint16_t m_order = 0;
            Batch2DStats m_stats;
            bool m_whiteUploaded = false;
        };
    }

    std::unique_ptr<Batcher2D> Batcher2D::Create(nvrhi::IDevice* device,
                                                 ShaderLibrary& shaders)
    {
        auto batcher = std::make_unique<Batcher2DImpl>(device, shaders);
        if (!batcher->Init())
            return nullptr;
        return batcher;
    }
}
```

API notes to verify against `nvrhi.h` (adapt minimally + report): `BindingLayoutItem::PushConstants(slot, size)` and `BindingSetItem::PushConstants(slot, size)`; `VertexAttributeDesc` fluent setters incl. `setElementStride`; `VertexBufferBinding{ buffer, slot, offset }` / `IndexBufferBinding{ buffer, format, offset }` aggregate shapes used by `addVertexBuffer`/`setIndexBuffer`; `BlendState::RenderTarget::enableBlend()` fluent chain; `writeTexture(tex, arraySlice, mipLevel, data, rowPitch)`; `DrawArguments.setVertexCount(n)` is the index count for `drawIndexed` (nvrhi convention — vertexCount doubles as indexCount).

- [ ] **Step 5: Build + run** — standard loop. Expected: the two quad tests pass; the full suite is green; validation silent (state/blend mistakes show as `[nvrhi]` errors and fail via `RenderErrorCount`).

- [ ] **Step 6: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Batcher2D - quad batching with binding-set and pipeline caches"
```

---

### Task 5: Batcher2D primitives — Rect is already real; add Line + Circle

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Render/Batcher2D.cpp`
- Modify: `Arcane/Tests/src/BatcherTest.cpp`

- [ ] **Step 1: Add the failing tests to `Arcane/Tests/src/BatcherTest.cpp`**

```cpp
namespace
{
    void CheckCircleCoverage(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend, /*size=*/16);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 16, 16);
        fx.batcher->Circle(glm::vec2(8, 8), 6.0f, glm::vec4(0, 1, 0, 1));
        fx.batcher->End();
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 8, 8, 0.0f, 1.0f, 0.0f);   // center: solid
        CheckPixel(pixels, rowPitch, 0, 0, 0.0f, 0.0f, 0.0f);   // corner: untouched
        CheckPixel(pixels, rowPitch, 15, 0, 0.0f, 0.0f, 0.0f);
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }

    void CheckLineCoverage(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend, /*size=*/16);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 16, 16);
        // Horizontal line through y=8, 4px thick.
        fx.batcher->Line(glm::vec2(0, 8), glm::vec2(16, 8), 4.0f,
                         glm::vec4(1, 0, 1, 1));
        fx.batcher->End();
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 8, 8, 1.0f, 0.0f, 1.0f);   // on the line
        CheckPixel(pixels, rowPitch, 8, 1, 0.0f, 0.0f, 0.0f);   // above it
        CheckPixel(pixels, rowPitch, 8, 14, 0.0f, 0.0f, 0.0f);  // below it
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: batcher circle and line coverage", "[gpu][d3d12]")
{
    CheckCircleCoverage(Arcane::GraphicsBackend::D3D12);
    CheckLineCoverage(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: batcher circle and line coverage", "[gpu][vulkan]")
{
    CheckCircleCoverage(Arcane::GraphicsBackend::Vulkan);
    CheckLineCoverage(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Build + run — the new cases FAIL** (Line/Circle log errors and draw nothing; center-pixel checks fail).

- [ ] **Step 3: Implement `Line` and `Circle` in `Batcher2D.cpp`**

Replace the two stub overrides:

```cpp
            void Line(glm::vec2 a, glm::vec2 b, float thickness,
                      glm::vec4 color) override
            {
                const glm::vec2 delta = b - a;
                const float length = glm::length(delta);
                if (length <= 0.0f || thickness <= 0.0f)
                    return;
                const glm::vec2 normal =
                    glm::vec2(-delta.y, delta.x) * (0.5f * thickness / length);
                // Oriented quad through the shared record path; reuses the
                // sprite pipeline with the white texture (uv constant).
                const glm::vec2 uv(0.5f);
                PushQuadVertices(BatchKind::Sprite, m_whiteTexture.Get(),
                                 { a - normal, uv, color },
                                 { b - normal, uv, color },
                                 { b + normal, uv, color },
                                 { a + normal, uv, color });
            }

            void Circle(glm::vec2 center, float radius, glm::vec4 color) override
            {
                if (radius <= 0.0f)
                    return;
                // SDF quad: uv spans [-1,1]; circle.hlsl keeps the unit disc.
                PushQuad(BatchKind::Circle, m_whiteTexture.Get(),
                         center - glm::vec2(radius), glm::vec2(radius * 2.0f),
                         glm::vec2(-1.0f), glm::vec2(1.0f), color);
            }
```

(`PushQuad` already breaks the batch when `kind` changes, so circles interleaved with sprites produce separate runs — the pipeline switch is what the run split exists for.)

- [ ] **Step 4: Build + run** — standard loop; all batcher tests pass on both backends, validation silent.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Batcher2D primitives - oriented-quad lines, SDF circles"
```

---

### Task 6: Shader hot reload — mechanism test + consumer wiring proof

`ShaderLibrary::Poll()`/`Generation()` and the consumer-side lazy pipeline rebuild already exist (Tasks 2-4). This task proves the loop end-to-end and gives Playground a poll site.

**Files:**
- Modify: `Arcane/Tests/src/ShaderLibraryTest.cpp`

- [ ] **Step 1: Add the failing test to `Arcane/Tests/src/ShaderLibraryTest.cpp`**

```cpp
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    // Copies an artifact tree to a temp dir so the test can touch files
    // without disturbing the build output other tests read.
    std::filesystem::path CopyShaderTree()
    {
        const auto dst = std::filesystem::temp_directory_path() / "arcane-hotreload";
        std::filesystem::remove_all(dst);
        std::filesystem::copy("shaders", dst,
                              std::filesystem::copy_options::recursive);
        return dst;
    }
}

TEST_CASE("shader hot reload: mtime change reloads and bumps generation", "[gpu][d3d12]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);

    const auto dir = CopyShaderTree();
    auto shaders = Arcane::ShaderLibrary::Create(
        device->Nvrhi(), Arcane::GraphicsBackend::D3D12, dir);
    REQUIRE(shaders != nullptr);

    nvrhi::ShaderHandle before =
        shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel);
    REQUIRE(before != nullptr);
    REQUIRE(shaders->Generation() == 1);

    // No change: Poll is a no-op.
    REQUIRE_FALSE(shaders->Poll());
    REQUIRE(shaders->Generation() == 1);

    // Bump the artifact's mtime past filesystem timestamp granularity.
    const auto artifact = dir / "dxil" / "sprite_ps.bin";
    std::filesystem::last_write_time(
        artifact, std::filesystem::file_time_type::clock::now() +
                      std::chrono::seconds(2));

    REQUIRE(shaders->Poll());
    REQUIRE(shaders->Generation() == 2);
    nvrhi::ShaderHandle after =
        shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel);
    REQUIRE(after != nullptr);
    REQUIRE(after != before);  // fresh handle, old one still validly held

    CHECK(Arcane::RenderErrorCount() == 0);
}
```

- [ ] **Step 2: Build + run** — expected: PASSES immediately if Tasks 2-4 were implemented to spec (the mechanism already exists; this is its characterization). If `Poll()` misses the change, the stored stamp wasn't refreshed in `LoadEntry`; if `Generation()` over-counts, `Poll` bumps outside the `reloaded` branch.

- [ ] **Step 3: Confirm consumer invalidation is wired (no new code — verification step)**

```bash
grep -n "m_pipelineGeneration != m_shaders.Generation()" Arcane/Arcane/src/Arcane/Render/Batcher2D.cpp Arcane/Arcane/src/Arcane/Render/TonemapPass.cpp
```
Expected: one hit in each consumer (the lazy cache-clear). This is the whole invalidation story — no callbacks, one integer compare per pipeline lookup.

- [ ] **Step 4: Commit**

```bash
git add Arcane/Tests && git commit -m "test(arcane): shader hot-reload mechanism characterization"
```

---

### Task 7: Playground M2a — real geometry through the canonical path

Replace the M1 direct-clear scaffolding with the full path: canvas → batcher (bouncing shapes + an HDR swatch) → tonemap → present. `ShaderLibrary::Poll()` runs once per second — edit an HLSL file, run `compile-shaders.bat`, watch the app pick it up (with `ARCANE_SHADER_DIR` pointed at the source `generated/` dir).

**Files:**
- Modify: `Arcane/Playground/src/main.cpp`

- [ ] **Step 1: Rewrite the render loop in `Arcane/Playground/src/main.cpp`**

Keep: the arg parsing (`--backend`, `--frames`, `--no-vsync`), Log init/banner, window+device+swapchain creation, the minimized-sleep, the title-update cadence, the maxFrames exit, and declaration order. Replace the includes block additions, the resource setup after `CreateSwapchain`, and the loop body:

Additional includes:

```cpp
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/TonemapPass.hpp>

#include <glm/glm.hpp>
```

Setup after the swapchain (replaces `createCommandList` alone):

```cpp
    auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                 "shaders");
    if (!shaders)
        return 1;
    auto canvas = Arcane::CreateCanvas(device->Nvrhi(),
                                       swapchain->Width(), swapchain->Height());
    if (!canvas)
        return 1;
    auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
    if (!batcher)
        return 1;
    auto tonemap = Arcane::TonemapPass::Create(device->Nvrhi(), *shaders);
    if (!tonemap)
        return 1;

    nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();
    auto lastShaderPoll = std::chrono::steady_clock::now();
```

Resize handling inside the loop gains the canvas:

```cpp
        if (events.resized)
        {
            swapchain->Resize(events.width, events.height);
            canvas->Resize(swapchain->Width(), swapchain->Height());
        }
```

The frame body (replaces the clear-color computation, clear, execute):

```cpp
        // Hot reload: poll once a second; pipeline caches rebuild lazily.
        const auto now0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now0 - lastShaderPoll).count() >= 1.0)
        {
            shaders->Poll();
            lastShaderPoll = now0;
        }

        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const float w = (float)canvas->Width();
        const float h = (float)canvas->Height();

        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

        batcher->Begin(commandList, canvas->Framebuffer(),
                       canvas->Width(), canvas->Height());

        // Three bouncing rects (linear colors).
        for (int i = 0; i < 3; ++i)
        {
            const float phase = (float)i * 2.1f;
            const float x = (0.5f + 0.4f * (float)std::sin(t * 0.8 + phase)) * w;
            const float y = (0.5f + 0.4f * (float)std::cos(t * 1.1 + phase)) * h;
            const glm::vec4 colors[3] = { { 0.9f, 0.2f, 0.2f, 1.0f },
                                          { 0.2f, 0.9f, 0.3f, 1.0f },
                                          { 0.2f, 0.4f, 0.9f, 1.0f } };
            batcher->Rect(glm::vec2(x - 40.0f, y - 40.0f), glm::vec2(80.0f),
                          colors[i]);
        }

        // Orbiting circle + a connecting line.
        const glm::vec2 middle(w * 0.5f, h * 0.5f);
        const glm::vec2 orbit = middle +
            glm::vec2((float)std::cos(t) * 0.3f * w, (float)std::sin(t) * 0.3f * h);
        batcher->Line(middle, orbit, 3.0f, glm::vec4(0.7f, 0.7f, 0.8f, 0.8f));
        batcher->Circle(orbit, 24.0f, glm::vec4(1.0f, 0.8f, 0.2f, 1.0f));

        // HDR swatch on a high sorting layer (always on top): linear up to
        // 4.0 -- visibly rolls off white through ACES instead of clipping.
        batcher->SetLayer(10, 0);
        const float hdr = 2.0f + 2.0f * (float)std::sin(t * 2.0);
        batcher->Rect(glm::vec2(20.0f, 20.0f), glm::vec2(120.0f, 60.0f),
                      glm::vec4(hdr, hdr, hdr, 1.0f));

        batcher->End();

        // The ONLY writer of the display-referred backbuffer. Framebuffers
        // are cached per backbuffer texture (cleared on resize below).
        nvrhi::FramebufferHandle& backbufferFb = backbufferFramebuffers[backbuffer];
        if (!backbufferFb)
            backbufferFb = device->Nvrhi()->createFramebuffer(
                nvrhi::FramebufferDesc().addColorAttachment(backbuffer));
        tonemap->Run(commandList, canvas->Texture(), backbufferFb);

        commandList->close();
        device->Nvrhi()->executeCommandList(commandList);
        swapchain->Present();
```

The cache is a local in `main`, declared next to `commandList`:

```cpp
    std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle>
        backbufferFramebuffers;  // swapchain backbuffer views; reset on resize
```

(add `#include <unordered_map>`), and the resize branch clears it — the full resize handling is:

```cpp
        if (events.resized)
        {
            backbufferFramebuffers.clear();
            swapchain->Resize(events.width, events.height);
            canvas->Resize(swapchain->Width(), swapchain->Height());
        }
```

The title line gains batch stats:

```cpp
            const Arcane::Batch2DStats stats = batcher->Stats();
            char title[200];
            std::snprintf(title, sizeof(title),
                          "Arcane Playground -- %s -- %s -- %.2f ms -- %u quads / %u draws",
                          Arcane::ToString(device->Backend()),
                          device->AdapterName().c_str(), frameMs,
                          stats.quads, stats.drawCalls);
```

NOTE on the per-frame `createFramebuffer`: nvrhi framebuffers are cheap view objects, but per-frame creation is still churn — cache them per backbuffer texture in a small `std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle>` local to main (clear it on `events.resized`). Show the cache in the final code; the snippet above shows the data flow.

- [ ] **Step 2: Build + scripted runs on both backends**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\Playground\Playground.exe --backend dx12 --frames 240 --no-vsync
echo EXIT=%ERRORLEVEL%
bin\Debug-windows-x86_64-md\Playground\Playground.exe --backend vulkan --frames 240 --no-vsync
echo EXIT=%ERRORLEVEL%
```
Expected: exit 0 on both; title shows `6 quads / N draws` (3 rects + line + circle + HDR swatch; N is 3 when the circle splits the sprite runs); zero `[nvrhi]` lines.

- [ ] **Step 3: Manual visual gate (record what you see in the report)**

Run each backend without `--frames`:
- three rects orbiting smoothly, a yellow circle on a gray line leash, dark blue background;
- the HDR swatch pulses TO white but never hard-clips (ACES shoulder);
- drag-resize: content rescales, no validation output, no flicker;
- vsync frame time ~16.7 ms; `--no-vsync` runs uncapped — confirm frame time drops well below 5 ms (2-frames-in-flight actually pipelining; the M1 idle-pacing would have pinned it near the GPU frame cost).

- [ ] **Step 4: Hot-reload dev-loop smoke (manual)**

```bat
set ARCANE_SHADER_DIR=%CD%\shaders\generated
bin\Debug-windows-x86_64-md\Playground\Playground.exe --backend vulkan
```
While it runs: edit `Arcane/shaders/tonemap.hlsl` (e.g. change `1.0 / 2.2` to `1.0 / 1.2`), run `shaders\compile-shaders.bat` in a second terminal. Expected: within ~1 s the image visibly brightens; "Shader reloaded: tonemap_ps" in the log. Revert the edit, recompile, confirm it returns. (Then `set ARCANE_SHADER_DIR=` to clear.)

- [ ] **Step 5: Release + Dist builds, Release scripted run, full test suite**

Standard loop + Release/Dist builds + `ArcaneTests.exe` both configs. Expected: green everywhere.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ && git commit -m "feat(playground): M2a demo - batched geometry through canvas/ACES path, hot-reload poll"
```

---

### Task 8: Docs + final sweep

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md's Arcane section**

(a) The "M1 state:" sentence becomes:

> M2a state: `Core` (static lib), `Arcane` (engine DLL — Base/Platform/Render: SDL3 window, NVRHI device on D3D12 + Vulkan, 2 frames in flight, linear-HDR Canvas → Batcher2D (quads/rects/lines/SDF circles) → ACES TonemapPass → backbuffer, ShaderLibrary with mtime hot reload), `Playground` (demo: `--backend dx12|vulkan`, `--frames N`, `--no-vsync`), `ArcaneTests`. Loom/Grimoire/Game arrive in later milestones; M2b adds text/assets/ImGui.

(b) Add to the workspace rules list:

> - **Shaders are data:** HLSL sources in `Arcane/shaders/`, compiled by `compile-shaders.bat` (DXC, DXIL+SPIR-V; SPIR-V register shifts match `nvrhi::VulkanBindingOffsets`: t=0 s=128 b=256 u=384) via the Arcane project's prebuild step into the gitignored `Arcane/shaders/generated/`. `ShaderLibrary` loads by name per backend; `ARCANE_SHADER_DIR` env var points a running app at a different artifact dir (hot-reload dev loop). All scene rendering is linear into the RGBA16F Canvas; only TonemapPass (Narkowicz ACES + 2.2, byte-matching the client's post_process.glsl) writes the backbuffer.

- [ ] **Step 2: Full verification sweep**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe -r junit::out=%TEMP%\arcane-m2a.xml
bin\Release-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Verify the JUnit XML has `failures="0" errors="0"`. No Server-workspace rebuild needed — nothing under `Arcane/Core/` changed in this plan (verify: `git diff main --stat -- Arcane/Core` is empty; if it is not, run the Server build + CommonTests like M1's Task 8 did).

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md && git commit -m "docs: CLAUDE.md Arcane section - M2a renderer core state"
```

---

## M2a exit criteria

- [ ] 2 frames in flight on both backends; `waitForIdle` absent from Present paths; PacingTest green.
- [ ] HLSL→DXC dual-target build runs as a prebuild step; six artifacts load by name on both backends.
- [ ] Linear RGBA16F Canvas is the only scene target; TonemapPass is the only backbuffer writer; ACES golden readbacks match the CPU reference (the client-oracle math) within ±2 bytes.
- [ ] Batcher2D is the single 2D submission path: quads, rects, lines, SDF circles; 64-bit sort keys (layer/order/kind/texture) give layer-correct ordering AND cross-layer batch coalescing, GPU-verified; the retained record→compile API is ready for the instanced/bindless v2 swap.
- [ ] Shader hot reload works end-to-end (mechanism test + manual Playground loop) via Generation-keyed lazy pipeline rebuild.
- [ ] Playground renders real geometry incl. an HDR swatch on both backends, exits 0 under `--frames`, uncapped frame time proves pipelining.
- [ ] All `[gpu]` tests assert `RenderErrorCount() == 0`; full suite green Debug + Release.

Out of scope (M2b plan, written after this lands): FreeType + skyline-atlas port (`Client/src/services/assets/skyline.lua` is the oracle), asset loaders (`services.Assets` semantics), `imgui_impl_nvrhi` + vendoring upstream `imgui_impl_sdl3` (only dx11/win32 backends are vendored today), ShaderMake adoption if shader count warrants. M3 takes the runtime backend swap, Tracy zones, and the full Playground scene.

