# Game Debug ImGui in the Grimoire Viewport — Design

**Goal:** Render the hosted game/plugin's debug ImGui on its **own layer** — composited into the Grimoire Viewport panel (the "game view") — instead of into the shared editor ImGui context where it floats over the editor chrome. The game's debug UI appears inside the viewport during Play; Edit mode shows a clean scene + gizmo. This is the Unity/Unreal "game UI lives in the game view" model.

## Motivation / problem

Grimoire and the hosted plugin currently share **one** Dear ImGui context. `ImGuiLayer` creates it; the plugin draws into that same context via `ImGui::SetCurrentContext(ctx->imguiContext)` and raw `ImGui::Begin/End` (`Arcane/Sandbox/src/Sandbox.cpp:98-102`). The editor's dockspace/panels/toolbar and the plugin's HUD land in one draw-data buffer, rendered in one pass, so the game's debug windows float freely over the editor chrome with a shared z-order. An earlier stopgap (`GrimoireApp.cpp` commit `6d0f4f45`) simply gated the plugin's `DrawUI()` off in Edit — this removes the clutter but throws away the game's debug UI entirely instead of separating it. This design gives the game its own ImGui layer.

## Research grounding (2026-07-20)

Both industry engines confine game-side debug UI to the game viewport surface, never the editor's panel tree, and the exact scenario is a documented Dear ImGui pattern:
- **Unreal:** the community `UnrealImGui` plugin uses **one ImGui context per world** (editor context + one per PIE instance), attached via `GameViewport->AddViewportWidgetContent(...)` — scoped to that viewport ([README](https://github.com/segross/UnrealImGui/blob/master/README.md), [ImGuiModuleManager.cpp](https://github.com/segross/UnrealImGui/blob/master/Source/ImGui/Private/ImGuiModuleManager.cpp)). Epic's own Gameplay Debugger / `DrawDebug*` / UMG all draw only into the game `SViewport`.
- **Unity:** runtime `OnGUI`/uGUI render into the **Game view**; editor `EditorGUI` renders into EditorWindows via a separate `GUIView` pipeline — same API family, two disjoint hosting paths ([IMGUI intro](https://docs.unity3d.com/Manual/GUIScriptingGuide.html)).
- **Dear ImGui:** our exact case is [ocornut/imgui #5891](https://github.com/ocornut/imgui/issues/5891) (editor ImGui + game-to-texture + game's own ImGui in that texture), maintainer-tagged **multi-contexts**, confirmed working with **two contexts**. Input rule (from a backend contributor): feed the focused context first, check `WantCaptureMouse/Keyboard`, forward what it doesn't claim downstream. Explicit "don't": **multi-viewports is the wrong tool** ([discussion #7578](https://github.com/ocornut/imgui/discussions/7578)) — it solves windows escaping the OS window, not nesting a context in a texture.

## Context (what already exists)

- **`Arcane::ImGuiLayer`** (`Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.{hpp,cpp}`): the exported facade over one ImGui context. `Init` does `ImGui::CreateContext()` + `ImGui_ImplSDL3_InitForOther(window)` + `m_renderer.Init(device, shaders)`. `BeginFrame()` = `ImGui_ImplSDL3_NewFrame()` + `ImGui::NewFrame()`; `Render(cmdList, target)` = `ImGui::Render()` + `m_renderer.RenderDrawData(...)`. A native SDL event tap feeds `ImGui_ImplSDL3_ProcessEvent`.
- **`Arcane::ImGuiNvrhiRenderer`** (`ImGuiNvrhi.hpp`): the NVRHI backend. **Engine-internal (not exported).** Crucially, it is **already fully instance-scoped** — sampler, layouts, vertex/index buffers, pipeline cache, binding-set cache, and the `ImTextureData*`→texture (font atlas) map are all members, no global statics. Two live instances do not conflict. (This is the single risk the research flagged, and it is already handled.)
- **Plugin ImGui contract (ABI v2):** the host calls the plugin's `GamePlugin_DrawUI` **between** `ImGuiLayer::BeginFrame` and `Render`; the plugin does `ImGui::SetCurrentContext(ctx->imguiContext)` then `Begin/End` windows. `ctx->imguiContext` is `EngineContext::imguiContext`. The plugin never calls `NewFrame`/`Render` itself. Loom passes the single context; Grimoire currently passes the editor context.
- **Grimoire viewport render:** `GrimoireApp::MainLoop` renders the scene into an `OffscreenCanvas` (`m_viewport->Draw([](Batcher2D& b){ SetRenderContext(&b); SubmitRender(); }, clear)`), linear-HDR canvas → ACES tonemap → the panel texture; the Viewport panel then shows that texture via `ImGui::Image`. The editor context's `BeginFrame`/`Render` bracket the panels, and `vtUI->DrawUI()` is called between them (`GrimoireApp.cpp` ~527).
- **Existing Edit-mode input gate (kept):** in Edit mode the editor owns the viewport's left mouse button (pick + gizmo); `pluginSnap.mouseButtons &= ~LMB` when `!IsPlaying()`, RMB/wheel stay live for plugin camera (`GrimoireApp.cpp`, commit `6d0f4f45`).

## Architecture — two ImGui contexts

1. **Editor context** — today's `ImGuiLayer` / `m_gpu->Imgui()`. SDL3 platform backend + its own `ImGuiNvrhiRenderer`; renders the dockspace/panels/toolbar/Viewport into the backbuffer. **Unchanged.**
2. **Game context** — a **second** `ImGuiContext` + its own `ImGuiNvrhiRenderer`, with **no SDL3 backend**. Its IO is *injected* (DisplaySize = viewport texture size, MousePos = viewport-local cursor, mouse buttons/wheel, deltaTime), and it renders into the **offscreen viewport framebuffer** so its output composites into the game view. Owns its **own font atlas** (self-contained; no dependency on editor-allocated resources — required by the one-way layering rule).

## Arcane API — `Arcane::OffscreenImGuiLayer` (ARCANE_API, editor-free)

Because `ImGuiNvrhiRenderer` is engine-internal, Grimoire cannot build the second context from raw pieces — Arcane exposes a small facade (`Arcane/Arcane/src/Arcane/ImGui/OffscreenImGuiLayer.{hpp,cpp}`), a sibling of `ImGuiLayer` minus the SDL3/window binding and plus injected input + a caller-supplied render target:

```cpp
namespace Arcane
{
    // A self-contained ImGui context that renders into a caller-provided
    // framebuffer with MANUALLY INJECTED input (no OS window / SDL3 backend).
    // For hosting a second ImGui layer inside an offscreen render target (e.g.
    // the game's debug UI composited into an editor viewport). Owns its own
    // ImGuiContext + ImGuiNvrhiRenderer + font atlas. Editor-agnostic.
    class ARCANE_API OffscreenImGuiLayer
    {
    public:
        static std::unique_ptr<OffscreenImGuiLayer> Create(RenderDevice&, ShaderLibrary&);
        virtual ~OffscreenImGuiLayer() = default;

        // The underlying ImGuiContext* (pass to a plugin as EngineContext::imguiContext).
        virtual void* Context() const = 0;

        // Per-frame injected IO for the next BeginFrame (viewport-local px).
        struct Input {
            glm::vec2 displaySize{0,0};   // the offscreen target size (px)
            glm::vec2 mousePos{-1,-1};    // viewport-local; (-1,-1) = cursor outside
            bool      mouseDown[5] = {};  // LMB,RMB,MMB,X1,X2
            float     wheel = 0.0f;
            float     deltaTime = 1.0f/60.0f;
            bool      hasInput = false;   // false => feed no mouse (context not focused/hovered)
        };
        virtual void SetInput(const Input&) = 0;

        // SetCurrentContext(this) + NewFrame. Caller then issues ImGui draw calls
        // (or the plugin's DrawUI). Pair with Render.
        virtual void BeginFrame() = 0;
        // ImGui::Render() + RenderDrawData into `target` on the OPEN command list.
        virtual void Render(nvrhi::ICommandList*, nvrhi::IFramebuffer* target) = 0;

        virtual bool WantCaptureMouse() const = 0;
        virtual bool WantCaptureKeyboard() const = 0;
    };
}
```

`ImGuiLayer` and `OffscreenImGuiLayer` both wrap the (instance-scoped) `ImGuiNvrhiRenderer`; the shared context/renderer plumbing may be factored into a common internal helper, but that is an implementation detail, not a contract.

## Grimoire orchestration

Grimoire owns the game-context instance (`std::unique_ptr<Arcane::OffscreenImGuiLayer> m_gameImgui`), created in `Init` after the GPU/ImGui layer. It sets the plugin's `EngineContext::imguiContext = m_gameImgui->Context()` so the plugin's `DrawUI` draws into the game context. **The plugin code does not change**; Loom (no editor) keeps passing its single context and renders over the whole window.

Per-frame flow (Play mode only), integrated into the viewport render:
1. Scene renders into the offscreen canvas → ACES tonemap → panel texture (unchanged).
2. **After tonemap** (so the UI is display-referred, not tonemapped), if `m_play.IsPlaying()`: build `OffscreenImGuiLayer::Input` from the viewport-local cursor + buttons + the panel size; `m_gameImgui->SetInput(...)`; `BeginFrame()`; `vtUI->DrawUI()`; `Render(cmdList, <panel-texture framebuffer>)`.
3. The editor context proceeds as today; the Viewport panel `ImGui::Image` shows the composited texture.

In **Edit mode** the game context is not run at all — no game HUD in the viewport (scene + gizmo only). This **supersedes** the earlier "gate `DrawUI` off in Edit" stopgap; the LMB-for-editor gate stays.

## Input arbitration (the #5891 handoff)

Order per frame, extending today's editor-first model:
1. OS input → **editor context** (SDL3 backend), as today. `m_gpu->Imgui().WantCaptureMouse()` true ⇒ the cursor is over an editor panel (not the viewport image) ⇒ done.
2. Else if the **Viewport is hovered** and Play: remap the cursor to viewport-local (`cursor − viewportImageOrigin`, scaled if texture-res ≠ panel-display-size), feed it to the **game context** via `SetInput` (previous frame's values drive this frame's `WantCapture` since ImGui is one frame deferred — acceptable, matches the existing 1-frame-lagged viewport data).
3. `m_gameImgui->WantCaptureMouse()` true ⇒ the game's debug UI owns the click ⇒ **suppress** the editor's gizmo/pick and the plugin's gameplay input (spawn) for that click. Else the input falls through to gizmo/pick (Edit) or plugin gameplay (Play) as today.

Mouse-position remap is the single most common bug in this pattern and is called out as a read-first confirmation. Keyboard/text input **into the game context** is a documented follow-up (see Non-goals) — v1 injects mouse (pos/buttons/wheel) only, which covers buttons/sliders/checkboxes/drags/combos (all the Sandbox HUD uses).

## Layering compliance

- `OffscreenImGuiLayer` is `ARCANE_API`, editor-free, game-agnostic — a reusable engine capability (any consumer wanting a second ImGui layer into a texture). Grimoire is the only current consumer.
- The plugin is unchanged and stays host-agnostic (it draws into whatever `ctx->imguiContext` it is handed).
- Loom is unchanged (single context, full-window).
- The game context owns its own font atlas — no dependency on editor-allocated resources, so Arcane stays usable editor-free.

## Edge cases

- **Play/Stop transitions:** the game context persists across Play/Stop (it's a render layer, not per-play state); it is simply run-or-not per frame based on `IsPlaying()`. No context recreation on Play/Stop.
- **Cursor outside the viewport while Playing:** `Input.hasInput = false` (or `mousePos = (-1,-1)`) so the game context sees no hover; its windows stay put, no spurious hover/capture.
- **Font atlas / device loss / shader hot-reload:** the game context's `ImGuiNvrhiRenderer` handles its own pipeline-cache rebuild on shader reload exactly as the editor one does (same instance-scoped machinery).
- **Plugin without `DrawUI`:** no game-context frame is run (nothing to draw); harmless.
- **Loom:** untouched — `EngineContext::imguiContext` is Loom's single context; the plugin renders over the whole window as before.

## Testing

- **Headless (`[grimoire]` / a new tag, CPU-only):** the input-arbitration decision + mouse-position remap are pure functions over (cursor, viewport rect, editor-wants, game-wants, isPlaying) → (who gets the click). Extract them as testable predicates (mirroring `ViewportInput.hpp`) and unit-test the handoff table + the remap math. No GPU.
- **Build-verified:** `OffscreenImGuiLayer` compiles + links (its `RenderDrawData` path is the same instance-scoped renderer already GPU-tested via `ImGuiLayer`).
- **Desk-verify (GPU/interactive):** Play → the plugin HUD renders **inside the Viewport** (not over the editor); hovering/clicking a HUD widget drives it and does **not** spawn/gizmo; the HUD does not appear in Edit; the HUD is not tonemapped (correct colors); no NVRHI/validation noise; two contexts coexist without backend corruption across many frames + a shader hot-reload.

## Non-goals (v1)

- **Keyboard/text input into the game context** (text fields in game debug UI) — mouse only in v1; keyboard forwarding to the second context is a follow-up.
- **Multi-viewport / ImGui docking-branch viewports** — the wrong tool (windows escaping the OS window), explicitly not used.
- **Per-world multiple game contexts** (UE PIE-style N contexts) — Grimoire hosts one scene → one game context.
- **netImgui-style out-of-process streaming.**
- **An editor toggle to show game UI in Edit** — Play-only for v1 (an editor-owned debug panel is the future path for edit-time visualization, not the game's context).

## Implementation-time confirmations (pin at read-first, not deferred TODOs)

- How `OffscreenCanvas` exposes a **framebuffer for the final (post-tonemap) panel texture** + an open command list to render the game ImGui pass into, and where in `GrimoireApp::MainLoop` that pass hooks (after tonemap, before the editor `ImGui::Image` samples the texture). Confirm the texture is display-referred at that point.
- The exact **viewport-local cursor + button** values already computed in `GrimoireApp::MainLoop` (`lx`,`ly`,`inViewport`, `snap.mouseButtons`) to build `OffscreenImGuiLayer::Input`, and the panel-image rect (`m_viewportRect`) for the remap + scale.
- Whether `ImGuiNvrhiRenderer` (or `ImGui_ImplSDL3`) touches any **process-global** ImGui state that a second context perturbs (the header says instance-scoped; confirm no `static` in the `.cpp`, and that `SetCurrentContext` fully swaps IO/fonts).
- The `EngineContext::imguiContext` plumbing + where Grimoire sets it, and that setting it to the game context doesn't break the editor context's own frames.
- Whether the game context needs `ImGui_ImplSDL3` for **any** IO it can't get injected (it should not — mouse is injected; keyboard is a non-goal) — confirm a context with a null platform backend runs `NewFrame` without asserting (feed `io.DisplaySize` + `io.DeltaTime` manually).
