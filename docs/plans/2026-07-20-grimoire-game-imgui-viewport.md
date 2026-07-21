# Game Debug ImGui in the Grimoire Viewport — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the hosted plugin's debug ImGui its own ImGui context, rendered into the Grimoire Viewport texture (the "game view"), so it appears inside the viewport during Play instead of floating over the editor chrome.

**Architecture:** A second, editor-free `ARCANE_API` ImGui layer (`OffscreenImGuiLayer`) with no SDL3 backend and manually-injected input renders into the offscreen viewport framebuffer. Grimoire owns that layer, points the plugin at its context, runs it only in Play, and arbitrates viewport input three ways (editor → game → gameplay). Both ImGui contexts each own their own (already instance-scoped) `ImGuiNvrhiRenderer`.

**Tech Stack:** C++23, Dear ImGui (1.92, `RendererHasTextures` protocol), NVRHI, SDL3 (editor context only), glm, Catch2. Spec: `docs/superpowers/specs/2026-07-20-grimoire-game-imgui-viewport-design.md`.

## Global Constraints

- **`ARCANE_API`, editor-free, game-agnostic:** `OffscreenImGuiLayer` lives in Arcane and knows nothing about Grimoire or any game; it owns its own ImGui context + `ImGuiNvrhiRenderer` + font atlas (no dependency on editor-allocated resources). The plugin is NOT modified. Loom is NOT modified.
- **Two contexts must not stomp:** every ImGui entry point brackets with `ImGui::SetCurrentContext(...)` for the context it operates on. `ImGui_ImplSDL3_*` is called ONLY with the editor context current (the game context has no SDL3 backend).
- **/MD everywhere; no `/fp:fast`; UTF-8 without BOM; ASCII comments.**
- **Build (PowerShell, VS18 MSBuild):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /t:<Target> /p:Configuration=Debug /m /nologo /v:minimal`. New files → run `& "Arcane\GenerateProjects.bat"` once first. Targets: `ArcaneTests`, `Grimoire`. **Use PowerShell, not the Bash tool, for MSBuild** (Git Bash mangles `/p:` `/m` switches).
- **Run headless tests** from the exe dir: `cd "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"` then `.\ArcaneTests.exe "<filter>"`.
- **GPU/interactive behavior is DESK-ONLY** — the Parsec GPU-driver crash hazard means `[gpu]` tests + launching Grimoire.exe are done at the desk, never headless. Run only `~[gpu]` filters headless.
- **Clang/clangd diagnostics in this workspace are NOISE** (wrong toolchain — "expected Clang 20", "Api.hpp not found"). MSVC build is the sole source of truth.
- **Commits:** `type(scope): summary`, NO AI trailers.
- **Baseline (must not drop):** `~[gpu]` 27845/343, `[grimoire]` 80/11.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/ImGui/OffscreenImGuiLayer.hpp` | Create | `ARCANE_API` facade: second ImGui context, injected input, offscreen render target. |
| `Arcane/Arcane/src/Arcane/ImGui/OffscreenImGuiLayer.cpp` | Create | Impl over an owned `ImGuiContext` + `ImGuiNvrhiRenderer`, no SDL3. |
| `Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.cpp` | Modify | `BeginFrame`/`Render` set their own context current (two-context safety). |
| `Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.hpp` | Modify | Add `OutputFramebuffer()` accessor (render the game ImGui pass into the post-tonemap texture). |
| `Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.cpp` | Modify | Return the internal output framebuffer. |
| `Arcane/Grimoire/src/ViewportImGuiInput.hpp` | Create | Pure input-arbitration predicates + mouse remap (headless-testable). |
| `Arcane/Grimoire/src/GrimoireApp.hpp` | Modify | Own `m_gameImgui`; store game-context WantCapture. |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Create game context, redirect plugin, render game ImGui pass into the viewport, arbitrate input, remove the old editor-context `DrawUI`. |
| `Arcane/Tests/src/ViewportImGuiInputTest.cpp` | Create | `[grimoire]` headless tests for arbitration + remap. |

---

## Task 1: `Arcane::OffscreenImGuiLayer` + two-context safety

The reusable engine capability: a second ImGui context that renders into a caller framebuffer with injected input. Also hardens `ImGuiLayer` to set its own context current (so two contexts coexist). GPU-touching, so build-verified here + desk-verified in Task 4 (its render path is the already-`[gpu]`-tested `ImGuiNvrhiRenderer`).

**Files:** Create `OffscreenImGuiLayer.{hpp,cpp}`; modify `ImGuiLayer.cpp`.

**Interfaces:**
- Consumes: `Arcane::RenderDevice`, `Arcane::ShaderLibrary`, `Arcane::ImGuiNvrhiRenderer` (engine-internal), `nvrhi`.
- Produces: `class OffscreenImGuiLayer` with `Create(RenderDevice&, ShaderLibrary&) -> unique_ptr`, `Context()`, `SetInput(const Input&)`, `BeginFrame()`, `Render(nvrhi::ICommandList*, nvrhi::IFramebuffer*)`, `WantCaptureMouse()`, `WantCaptureKeyboard()`.

- [ ] **Step 0 (read-first):** Read `Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.cpp` (the sibling to mirror), `ImGuiNvrhi.hpp` (the renderer — already instance-scoped, member state only), and how `RenderDevice`/`ShaderLibrary` are obtained (`ImGuiLayer::Create(Window&, RenderDevice&, ShaderLibrary&)` and its `device.Nvrhi()`). Confirm `ImGui::CreateContext()` makes the new context current, and that `ImGuiNvrhiRenderer::Init` sets its backend flags on the CURRENT context. Confirm the glm include convention (`<glm/glm.hpp>`).

- [ ] **Step 1: Write `OffscreenImGuiLayer.hpp`.**

```cpp
#pragma once

// A self-contained Dear ImGui context that renders into a caller-provided
// framebuffer with MANUALLY INJECTED input (no OS window / SDL3 backend). For
// hosting a SECOND ImGui layer inside an offscreen render target -- e.g. a
// game/plugin's debug UI composited into an editor viewport, separate from the
// editor's own ImGui. Owns its ImGuiContext + ImGuiNvrhiRenderer + font atlas;
// editor- and game-agnostic. Sibling of ImGuiLayer (which owns the OS-window,
// SDL3-backed context).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

namespace Arcane
{
    class RenderDevice;
    class ShaderLibrary;

    class ARCANE_API OffscreenImGuiLayer
    {
    public:
        static std::unique_ptr<OffscreenImGuiLayer> Create(RenderDevice& device,
                                                           ShaderLibrary& shaders);
        virtual ~OffscreenImGuiLayer() = default;

        // The underlying ImGuiContext* (as void*, keeping this header imgui-free
        // like Runtime.hpp). Pass to a plugin via Runtime::SetImGui / EngineContext.
        virtual void* Context() const = 0;

        // Injected IO for the NEXT BeginFrame. Coordinates are target-local px.
        struct Input
        {
            glm::vec2 displaySize{0.0f, 0.0f};   // offscreen target size (px)
            glm::vec2 mousePos{0.0f, 0.0f};      // target-local px
            bool      mouseDown[5] = {};         // LMB,RMB,MMB,X1,X2
            float     wheel        = 0.0f;
            float     deltaTime    = 1.0f / 60.0f;
            bool      hasInput     = false;      // false => cursor off-target, no buttons
        };
        virtual void SetInput(const Input&) = 0;

        // SetCurrentContext(this) + inject IO + ImGui::NewFrame(). Caller then
        // issues ImGui draw calls (or a plugin's DrawUI). Caches WantCapture* for
        // this frame. Pair every BeginFrame with a Render.
        virtual void BeginFrame() = 0;

        // SetCurrentContext(this) + ImGui::Render() + RenderDrawData into `target`
        // on the OPEN command list (display-referred target; ImGui blends over it).
        virtual void Render(nvrhi::ICommandList*, nvrhi::IFramebuffer* target) = 0;

        // Valid after BeginFrame; reflects whether THIS context's UI wants the
        // pointer/keys this frame (i.e. the cursor is over its widgets).
        virtual bool WantCaptureMouse() const = 0;
        virtual bool WantCaptureKeyboard() const = 0;
    };
}
```

- [ ] **Step 2: Write `OffscreenImGuiLayer.cpp`.**

```cpp
#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/ImGui/ImGuiNvrhi.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <imgui.h>

#include <cfloat>

namespace Arcane
{
    namespace
    {
        class OffscreenImGuiLayerImpl final : public OffscreenImGuiLayer
        {
        public:
            ~OffscreenImGuiLayerImpl() override
            {
                if (!m_context)
                    return;
                ImGuiContext* prev = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(m_context);
                m_renderer.Shutdown();
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            bool Init(RenderDevice& device, ShaderLibrary& shaders)
            {
                IMGUI_CHECKVERSION();
                m_context = ImGui::CreateContext();   // own atlas; sets itself current
                if (!m_context)
                {
                    ARC_ERROR("OffscreenImGuiLayer: ImGui::CreateContext failed");
                    return false;
                }
                ImGuiContext* prev = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(m_context);
                ImGuiIO& io = ImGui::GetIO();
                io.BackendPlatformName = "arcane_offscreen";
                io.IniFilename = nullptr;             // no imgui.ini for the game context
                io.DisplaySize = ImVec2(1.0f, 1.0f);  // non-zero so a stray NewFrame is safe
                const bool ok = m_renderer.Init(device.Nvrhi(), shaders);   // backend flags on THIS ctx
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                if (!ok)
                {
                    ARC_ERROR("OffscreenImGuiLayer: ImGuiNvrhiRenderer::Init failed");
                    ImGui::DestroyContext(m_context);
                    m_context = nullptr;
                    return false;
                }
                return true;
            }

            void* Context() const override { return m_context; }
            void  SetInput(const Input& in) override { m_input = in; }

            void BeginFrame() override
            {
                ImGui::SetCurrentContext(m_context);
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(m_input.displaySize.x <= 0.0f ? 1.0f : m_input.displaySize.x,
                                        m_input.displaySize.y <= 0.0f ? 1.0f : m_input.displaySize.y);
                io.DeltaTime = m_input.deltaTime > 0.0f ? m_input.deltaTime : 1.0f / 60.0f;
                if (m_input.hasInput)
                {
                    io.AddMousePosEvent(m_input.mousePos.x, m_input.mousePos.y);
                    for (int i = 0; i < 5; ++i) io.AddMouseButtonEvent(i, m_input.mouseDown[i]);
                    if (m_input.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, m_input.wheel);
                }
                else
                {
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);   // cursor off-target
                    for (int i = 0; i < 5; ++i) io.AddMouseButtonEvent(i, false);
                }
                ImGui::NewFrame();
                // WantCapture* is finalized at NewFrame (from last frame's hover/active).
                m_wantMouse    = io.WantCaptureMouse;
                m_wantKeyboard = io.WantCaptureKeyboard;
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* target) override
            {
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                m_renderer.RenderDrawData(ImGui::GetDrawData(), cmd, target);
            }

            bool WantCaptureMouse()    const override { return m_wantMouse; }
            bool WantCaptureKeyboard() const override { return m_wantKeyboard; }

        private:
            ImGuiContext*      m_context = nullptr;
            ImGuiNvrhiRenderer m_renderer;
            Input              m_input;
            bool               m_wantMouse    = false;
            bool               m_wantKeyboard = false;
        };
    }

    std::unique_ptr<OffscreenImGuiLayer> OffscreenImGuiLayer::Create(RenderDevice& device,
                                                                    ShaderLibrary& shaders)
    {
        auto layer = std::make_unique<OffscreenImGuiLayerImpl>();
        if (!layer->Init(device, shaders))
            return nullptr;
        return layer;
    }
}
```

(Adapt `device.Nvrhi()` / the `RenderDevice`/`ShaderLibrary` accessors to the exact signatures found in Step 0 — mirror what `ImGuiLayer.cpp` does.)

- [ ] **Step 3: Make `ImGuiLayer` two-context safe.** In `ImGuiLayer.cpp`, at the TOP of `BeginFrame()` and `Render()`, add `ImGui::SetCurrentContext(m_context);` (before `ImGui_ImplSDL3_NewFrame`/`ImGui::Render`). The SDL3 tap (`ImGui_ImplSDL3_ProcessEvent`) already targets whatever context is current at event time; leave it, but note it processes into the editor context (correct — the editor owns OS input). Rationale: once a second context exists, `BeginFrame` must not run on whatever context another layer last left current.

- [ ] **Step 4: Regenerate + build, verify clean.** `& "Arcane\GenerateProjects.bat"`; build `ArcaneTests` (compiles the Arcane lib incl. the new layer). Expect zero errors. There is no headless test for a GPU layer; the render path is `ImGuiNvrhiRenderer`, already `[gpu]`-verified via `ImGuiLayer`.

- [ ] **Step 5: Run the CPU gate**, confirm no regression: from the exe dir `.\ArcaneTests.exe "~[gpu]"` → 27845/343 unchanged (nothing new is exercised yet).

- [ ] **Step 6: Commit** — `feat(arcane): OffscreenImGuiLayer -- second ImGui context into an offscreen target`.

---

## Task 2: `OffscreenCanvas::OutputFramebuffer()`

Expose the canvas's post-tonemap output framebuffer so the game ImGui pass can render INTO the display-referred viewport texture (over the scene). Tiny, build-verified.

**Files:** Modify `OffscreenCanvas.hpp`, `OffscreenCanvas.cpp`.

**Interfaces:**
- Produces: `nvrhi::IFramebuffer* OffscreenCanvas::OutputFramebuffer() const` — the framebuffer wrapping the same output texture `TextureId()` reports; valid until `Resize()`.

- [ ] **Step 0 (read-first):** Read `OffscreenCanvas.cpp` — find the member holding the output texture's framebuffer (the one the `TonemapPass` renders into). Confirm the output texture has render-target usage (it does — the tonemap writes it) and is display-referred (BGRA8_UNORM, gamma-encoded) after `Draw()` returns. Confirm it is a distinct handle from the linear-HDR canvas framebuffer.

- [ ] **Step 1: Declare the accessor** in `OffscreenCanvas.hpp`, after `TextureId()`:

```cpp
        // The output texture's framebuffer (post-tonemap, display-referred), for
        // rendering an extra overlay pass (e.g. a second ImGui context) OVER the
        // tonemapped scene. Valid until Resize(). Do not use for the scene pass --
        // that is Draw()'s job.
        virtual nvrhi::IFramebuffer* OutputFramebuffer() const = 0;
```

- [ ] **Step 2: Implement it** in `OffscreenCanvas.cpp` — return the existing output framebuffer member (the one the tonemap targets). One line: `nvrhi::IFramebuffer* OutputFramebuffer() const override { return m_outputFramebuffer.Get(); }` (use the real member name found in Step 0).

- [ ] **Step 3: Build, verify clean.** Build `ArcaneTests`; zero errors. `.\ArcaneTests.exe "~[gpu]"` → 27845/343 unchanged.

- [ ] **Step 4: Commit** — `feat(arcane): OffscreenCanvas::OutputFramebuffer -- expose the post-tonemap target for overlay passes`.

---

## Task 3: Grimoire orchestration — own the game context, redirect the plugin, render the game ImGui into the viewport

Grimoire creates the game context, points the plugin at it, and runs+composites the plugin's DrawUI into the viewport texture only in Play. Build + desk verified (ImGui + GPU viewport).

**Files:** Modify `GrimoireApp.hpp`, `GrimoireApp.cpp`.

**Interfaces:**
- Consumes: `Arcane::OffscreenImGuiLayer` (Task 1), `OffscreenCanvas::OutputFramebuffer()` (Task 2), `Runtime::SetImGui`, the plugin vtable's `DrawUI`.

- [ ] **Step 0 (read-first):** In `GrimoireApp.cpp` confirm: (a) `m_runtime->SetImGui(ImGui::GetCurrentContext(), ...)` at ~line 142 (the current editor-context redirect — you'll change the FIRST arg); capture the exact `alloc`/`free`/`userData` args passed there. (b) The GPU/shaders handles used to create the editor ImGui layer (for `OffscreenImGuiLayer::Create(device, shaders)`) — e.g. `m_gpu->Device()` / the `ShaderLibrary` the editor layer was built from. (c) The viewport render site `m_viewport->Draw([](Batcher2D&){...}, clear)` (~line 455) and that it runs BEFORE `m_gpu->Imgui().BeginFrame()` (~494). (d) The current `vtUI->DrawUI()` call (~527, editor context — to be REMOVED). (e) How to open/submit a one-off command list (mirror the backbuffer pass: `m_gpu->Cmd()->open()` ... `close()` ... `Device().Nvrhi()->executeCommandList(...)`, ~534-543). (f) The viewport-local cursor + buttons already computed (`lx`,`ly`,`inViewport`,`snap.mouseButtons`, `m_viewportRect`) and `m_play.IsPlaying()`.

- [ ] **Step 1: Add state in `GrimoireApp.hpp`.** Add `#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>` and a member near `m_viewport`:

```cpp
        std::unique_ptr<Arcane::OffscreenImGuiLayer> m_gameImgui;  // plugin debug UI, into the viewport
```

- [ ] **Step 2: Create the game context in `Init`, redirect the plugin.** After the GPU/editor-ImGui layer is up and BEFORE the plugin is loaded/`SetImGui` is called (~line 142), create the game layer and point the plugin at it:

```cpp
        m_gameImgui = Arcane::OffscreenImGuiLayer::Create(m_gpu->Device(), /*ShaderLibrary&*/ shaders);
        if (!m_gameImgui) { ARC_ERROR("Grimoire: OffscreenImGuiLayer creation failed"); return false; }
        // The hosted plugin draws its debug UI into the GAME context (composited
        // into the viewport), not the editor context. Same ImGui allocators.
        m_runtime->SetImGui(m_gameImgui->Context(), /*alloc*/..., /*free*/..., /*userData*/...);
```

Use the exact `device`/`shaders` handles + `alloc`/`free`/`userData` values from Step 0 (the `alloc`/`free` are the SAME process ImGui allocators the editor redirect used — only the context pointer changes).

- [ ] **Step 3: Render the game ImGui pass into the viewport, in Play.** Immediately AFTER `m_viewport->Draw(...)` (scene → tonemap → output texture) and BEFORE `m_gpu->Imgui().BeginFrame()`:

```cpp
            // Game debug UI -> the viewport's own layer (Play only). Runs the plugin's
            // DrawUI into the GAME ImGui context and composites it OVER the tonemapped
            // scene, in the offscreen output texture -- so it lives inside the Viewport
            // panel, never over the editor chrome. Edit mode: not run (clean viewport).
            if (!m_play.IsPlaying())
                m_gameImgui->SetInput({});   // no input; keeps its IO sane if ever drawn
            const Arcane::PluginVTable* vtGame = m_plugin->Vtable();
            if (m_play.IsPlaying() && vtGame && vtGame->DrawUI)
            {
                Arcane::OffscreenImGuiLayer::Input gi;
                gi.displaySize = glm::vec2((float)m_viewport->Width(), (float)m_viewport->Height());
                gi.deltaTime   = (float)frameDt;   // the same per-frame dt used above
                gi.hasInput    = inViewport;       // viewport-local cursor computed in the input block
                gi.mousePos    = glm::vec2(lx, ly);
                gi.mouseDown[0] = (snap.mouseButtons & 0x1u) != 0;
                gi.mouseDown[1] = (snap.mouseButtons & 0x2u) != 0;
                gi.mouseDown[2] = (snap.mouseButtons & 0x4u) != 0;
                gi.wheel        = snap.wheelY;
                m_gameImgui->SetInput(gi);
                m_gameImgui->BeginFrame();
                vtGame->DrawUI();
                m_gpu->Cmd()->open();
                m_gameImgui->Render(m_gpu->Cmd(), m_viewport->OutputFramebuffer());
                m_gpu->Cmd()->close();
                m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            }
```

Adapt `lx`/`ly`/`inViewport`/`snap`/`frameDt`/the command-list API to the real locals/APIs from Step 0. If `lx`/`ly`/`snap` are scoped to the input `{}` block, hoist the few values you need to members set in the input block (e.g. `m_lastViewportMouse`, `m_lastMouseButtons`, `m_lastInViewport`, `m_lastWheel`) and read them here — do NOT widen the input block's scope.

- [ ] **Step 4: Remove the old editor-context DrawUI.** Delete the `if (... vtUI->DrawUI) vtUI->DrawUI();` at ~line 527 (the plugin now draws into the game context in Step 3). This also removes the earlier Play-gate-on-DrawUI stopgap. (Keep the Edit-mode LMB gate in the input block — unrelated, still correct.)

- [ ] **Step 5: Regenerate + build `ArcaneTests;Grimoire`, verify clean.** Build both; zero errors. `.\ArcaneTests.exe "~[gpu]"` → 27845/343; `.\ArcaneTests.exe "[grimoire]"` → 80/11.

- [ ] **Step 6: Commit** — `feat(grimoire): render the hosted plugin's debug ImGui into the viewport (own context)`.

---

## Task 4: Viewport ImGui input arbitration (headless `[grimoire]` + wiring)

The game UI, being over the viewport, must claim the click when the cursor is over its widgets — suppressing the editor gizmo/pick and the plugin's gameplay spawn. Extract the decision as pure predicates and unit-test them, then wire.

**Files:** Create `Arcane/Grimoire/src/ViewportImGuiInput.hpp`, `Arcane/Tests/src/ViewportImGuiInputTest.cpp`; modify `GrimoireApp.cpp`.

**Interfaces:**
- Produces: `Grimoire::GameUiClaimsPointer(bool isPlaying, bool inViewport, bool gameWantCaptureMouse) -> bool`.

- [ ] **Step 1: Write the failing test** — `Arcane/Tests/src/ViewportImGuiInputTest.cpp`:

```cpp
// Grimoire viewport ImGui input arbitration ([grimoire], CPU-only, no ImGui/GPU).
#include <catch2/catch_test_macros.hpp>
#include "ViewportImGuiInput.hpp"

TEST_CASE("GameUiClaimsPointer: only in Play, in viewport, when game UI wants the mouse", "[grimoire]")
{
    // Play + cursor over a game widget -> game UI owns the click.
    CHECK(Grimoire::GameUiClaimsPointer(/*play*/true,  /*inVp*/true,  /*gameWant*/true));
    // Play but cursor not over a game widget -> falls through to gizmo/gameplay.
    CHECK_FALSE(Grimoire::GameUiClaimsPointer(true,  true,  false));
    // Cursor outside the viewport -> editor panels own it, not the game UI.
    CHECK_FALSE(Grimoire::GameUiClaimsPointer(true,  false, true));
    // Edit mode -> the game UI is not even running.
    CHECK_FALSE(Grimoire::GameUiClaimsPointer(false, true,  true));
}
```

- [ ] **Step 2: Run headless, verify fail.** `& "Arcane\GenerateProjects.bat"` (new files), build `ArcaneTests`, `.\ArcaneTests.exe "[grimoire]"` → compile/link error (no `ViewportImGuiInput.hpp`).

- [ ] **Step 3: Write `ViewportImGuiInput.hpp`:**

```cpp
#pragma once

// Pure predicates for arbitrating viewport pointer input between the editor
// (gizmo/pick), the game's in-viewport debug ImGui, and the plugin's gameplay.
// No ImGui/GPU dependency -> headlessly unit-testable (see
// Tests/src/ViewportImGuiInputTest.cpp). Consumed by GrimoireApp.

namespace Grimoire
{
    // True when the game's viewport debug UI owns the pointer this frame, so the
    // editor gizmo/pick AND the plugin's gameplay input must be suppressed for it.
    // Only in Play (the game UI runs only then), only when the cursor is inside the
    // viewport, and only when the game context reported WantCaptureMouse.
    inline bool GameUiClaimsPointer(bool isPlaying, bool inViewport,
                                    bool gameWantCaptureMouse) noexcept
    {
        return isPlaying && inViewport && gameWantCaptureMouse;
    }
}
```

- [ ] **Step 4: Build + run, verify PASS.** Build `ArcaneTests`, `.\ArcaneTests.exe "[grimoire]"` → all pass (80/11 + this case).

- [ ] **Step 5: Wire into `GrimoireApp.cpp`.** Add `#include "ViewportImGuiInput.hpp"`. Compute once per frame in the input block, using the game context's LAST-frame WantCapture (1-frame lag is fine — it matches the existing last-frame viewport data):

```cpp
                const bool gameUiClaims = Grimoire::GameUiClaimsPointer(
                    m_play.IsPlaying(), inViewport, m_gameImgui->WantCaptureMouse());
```

Then use `gameUiClaims`:
- **Plugin gameplay input:** when `gameUiClaims`, also clear LMB from `pluginSnap.mouseButtons` (extend the existing Edit-mode LMB clear) so a click on the game HUD does not spawn/drag through to gameplay: `if (!m_play.IsPlaying() || gameUiClaims) pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x1u);`
- **Editor gizmo/pick (Play only — they are already Edit-gated, but the click-pick + gizmo run guards should also honor it):** gate the gizmo/click-pick start on `!gameUiClaims` (they are already `!IsPlaying()`-gated in Edit, and `gameUiClaims` is Play-only, so in practice this only matters if a future change lets the gizmo run in Play — add `&& !gameUiClaims` to the gizmo/pick start conditions defensively, matching the spec's handoff).

- [ ] **Step 6: Build `ArcaneTests;Grimoire`, verify clean + no regression.** `.\ArcaneTests.exe "~[gpu]"` → 27845/343 + the new `[grimoire]` case; `[grimoire]` count grows by 1 case.

- [ ] **Step 7: Commit** — `feat(grimoire): arbitrate viewport input to the game's in-viewport debug UI`.

---

## Task 5: Desk-verify

- [ ] **Step 1: Headless gate** (here): `.\ArcaneTests.exe "~[gpu]"` passes at/above 27845/343 + the new `[grimoire]` case; record the count. `[grimoire]` still green.
- [ ] **Step 2: Desk interactive** (Grimoire, at the desk — GPU hazard headless): **Play** → the plugin HUD renders **inside the Viewport** (not floating over the editor), correct colors (not tonemapped); **hover/click a HUD widget** drives it and does **NOT** spawn a box or move the gizmo; RMB-drag / wheel still pan+zoom the plugin camera; the HUD is **absent in Edit** (clean scene+gizmo); **Stop** returns to the clean editor with no lingering HUD; run for a while + trigger a **shader hot-reload** (`F5`/edit a shader) → no NVRHI/VK-validation noise, no corruption between the two contexts; the editor panels/dockspace/inspector are unaffected.
- [ ] **Step 3:** Append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** two contexts (editor unchanged + new game context) → Task 1; the `OffscreenImGuiLayer` ARCANE_API facade (own context/renderer/atlas, injected input, offscreen render) → Task 1; two-context safety (SetCurrentContext) → Task 1 Step 3; render post-tonemap into the viewport texture → Task 2 (`OutputFramebuffer`) + Task 3 Step 3; Grimoire owns the game context + redirects the plugin via `SetImGui` → Task 3 Steps 2; Play-only visibility + removing the old stopgap → Task 3 Steps 3-4; input arbitration (editor → game → gameplay) + mouse-claim suppression → Task 4; headless testing of the arbitration → Task 4; layering (ARCANE_API, plugin/Loom unchanged, own atlas) → Task 1 + Global Constraints; non-goals (keyboard, multi-viewport, per-world, netImgui, edit toggle) untouched; implementation-time confirmations → Task-N Step-0 read-firsts. Covered.

**Placeholder scan:** the `/*...*/` tokens in Task 3 Step 2 (`device`/`shaders`/`alloc`/`free`/`userData`) and Task 2 Step 2 (member name) are enumerated read-first adaptations against named files/lines with the intended shape shown, not deferred TODOs — same convention as this repo's prior plans. No "TBD"/"add error handling"/uncoded steps.

**Type consistency:** `OffscreenImGuiLayer` / `Create(RenderDevice&, ShaderLibrary&)` / `Context()` / `SetInput(const Input&)` / `Input{displaySize,mousePos,mouseDown[5],wheel,deltaTime,hasInput}` / `BeginFrame()` / `Render(nvrhi::ICommandList*, nvrhi::IFramebuffer*)` / `WantCaptureMouse()` / `WantCaptureKeyboard()`; `OffscreenCanvas::OutputFramebuffer()`; `Grimoire::GameUiClaimsPointer(bool,bool,bool)` — consistent across tasks and matching the spec.
