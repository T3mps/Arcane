# Arcane Physics Sandbox / Engine Showcase — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task (fresh implementer + spec + quality review per task). Steps use
> checkbox (`- [ ]`) syntax.

**Goal:** Build `Sandbox.dll` — a hot-reloadable, interactive physics/engine showcase hosted by Loom
(8 curated scenes + spawn/drag/throw + an ImGui control panel + PhysicsDebugDraw overlays) — extend
the plugin ABI (v1->v2) to carry input + ImGui through the boundary, and retire the monolithic
`Playground.exe` so the demo surface is one host + one showcase plugin + one test fixture.

**Architecture:** `Loom.exe` (host) owns window/device/render/ImGui/input + the `Runtime` + `PluginHost`;
it loads `Sandbox.dll` via the GamePlugin ABI. The sandbox uses the REAL engine stack (Astra Registry,
`PhysicsSystem` -> v2 `PhysicsWorld`, `TransformPropagationSystem`, `RenderSubmissionSystem`,
`PhysicsDebugDraw`). `PlaygroundGame.dll` stays as the minimal hot-reload test fixture (bumped to v2).

**Tech Stack:** C++23 (Arcane /MD); Astra ECS; glm; Dear ImGui; NVRHI; SDL3; Catch2; premake5 vs2026;
msbuild. Spec: `docs/superpowers/specs/2026-06-20-arcane-physics-sandbox-showcase-design.md`.

---

## Design notes (read before Task 1)

**Read the spec first** for the locked decisions (full interactive showcase; extend ABI for
input+ImGui; new `Sandbox.dll` not an evolution of PlaygroundGame; retire Playground.exe; Loom is the
single host).

**Engine philosophy:** Arcane is a growing best-in-class engine. Correctness/integration first; the
sandbox drives PRODUCTION paths (it is an integration check, not a parallel demo harness). The
ABI/input/render-bridge extensions are real, forward-useful engine work the editor will stand on — not
throwaway scaffolding. Don't avoid improving a path because it changes numbers (see
`feedback_engine_evolves_not_frozen`).

**Verified current-code facts (cite these):**
- **Plugin ABI** (`Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp`): `kGamePluginABIVersion = 1`;
  `EngineContext { abiVersion, typeContext, workScheduler, engine }`; `PluginVTable` has 7 fn ptrs;
  `PluginEntry::k*` names. The DLL exports 7 `GamePlugin_*` functions; `PluginHost` resolves them.
- **Runtime** (`Arcane/Arcane/src/Arcane/Base/Runtime.hpp`): exposes `Registry()`, `Schedulers()`,
  `Loop()`, `TypeContext()`, `WorkScheduler()`, `Components()`, `SetRenderContext(Batcher2D*, glm::vec2
  cameraOffset)`, `SnapshotRegistry()`/`RestoreRegistry()`/`ResetRegistry()`/`ClearSystems()`. Uses a
  pImpl (`struct Impl`). ARCANE_API (host + plugin both call it).
- **InputSnapshot** (`Arcane/Arcane/src/Arcane/Input/InputSnapshot.hpp`): a POD
  (`static_assert(is_trivially_copyable)`); carries scancodes, keycodes, `mouseButtons`, gamepad,
  `wantCaptureKeyboard/Mouse`. **No mouse position.** Filled by `InputDevices::Sample(captureKb,
  captureMouse)`.
- **ImGuiLayer** (`Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.hpp`): owns the ImGui context + backends;
  `BeginFrame()` / `Render(cmdList, framebuffer)` / `WantCaptureKeyboard/Mouse()`.
- **Loom loop** (`Arcane/Loom/src/main.cpp`): already parses `--frames N` (line 65), `--plugin <path>`
  (default `"PlaygroundGame.dll"`, line 54). Per frame: sample input -> `input->Update(...,
  inputDevices->Sample(...))` (line 173, snapshot consumed inline — NOT stored); RunLoop `Advance`
  with the plugin's `FixedUpdate`/`Update` (lines 186-188) — **this is BEFORE `imgui->BeginFrame()`
  at line 191**; host draws its own ImGui panel (193-198); `SetRenderContext(batcher, vec2(0,0))` +
  `SubmitRender()` (228-229); `imgui->Render(...)` (238). So the plugin's ImGui must be drawn by a
  NEW hook the host calls between `BeginFrame` (191) and `Render` (238) — `Update` is too early.
- **PlaygroundGame** (`Arcane/PlaygroundGame/src/PlaygroundGame.cpp`): the orbit fixture; 7 entry
  points; `GamePlugin_Init` installs the TypeContext, re-registers components, adds systems, builds
  the scene. Referenced by tests `PlaygroundGamePluginTest.cpp` + `LoomSliceTest.cpp`.
- **Physics scene layer**: `Arcane/Arcane/src/Arcane/Scene/PhysicsComponents.hpp` (`RigidBody2D`,
  `Collider2D` w/ fixtures), `PhysicsSystem.hpp`. `PhysicsDebugDraw`
  (`Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp`).

**Build/run (the Arcane toolchain):**
- Branch: `feature/arcane-physics-v2` (current).
- Regenerate after adding files/projects: `cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026` (GenerateProjects.bat hangs on a `pause`).
- Build: `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (+ `Release`).
- Tests FROM the exe dir: `cd "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[sandbox]~[gpu]"` (tag new sandbox tests `[sandbox]`; render-touching ones also `[gpu]`).
- **GOTCHA:** never run the bare `[gpu]` tag during dev (Vulkan-validation hang risk); use a filtered subset. If ArcaneTests hangs: `taskkill //F //IM ArcaneTests.exe` + rebuild.
- Loom manual run: `bin/Debug-windows-x86_64-md/Loom/Loom.exe --backend vulkan --plugin Sandbox.dll` (F5/F6 reload).
- Server-flavor gate (Core untouched, but confirm): msbuild `Server/Aphelyon.slnx -t:ArcaneCore` Debug+Release.
- clangd shows false positives — MSVC is the source of truth.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Plugin-load test pattern:** mirror `LoomSliceTest.cpp` (headless `Runtime` + `PluginHost::Load` a
DLL + drive `RunLoop`). Render-touching smokes ([gpu]) mirror how `PhysicsDebugDrawTest`/`BatcherTest`
build a headless `RenderDevice`; assert `Arcane::RenderErrorCount() == 0`. New plugin DLLs need a
post-build copy next to `ArcaneTests.exe` + `Loom.exe` (mirror PlaygroundGame's premake copy wiring).

---

## Task 1 — InputSnapshot cursor position + Runtime input accessor

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Input/InputSnapshot.hpp`, `Arcane/Arcane/src/Arcane/Input/InputDevices.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp` / `Runtime.cpp`
- Test: `Arcane/Tests/src/SandboxInputTest.cpp` (new; `[sandbox]`)

**What:** Add the cursor position to the input snapshot (interactive picking needs it) and a Runtime
accessor so a plugin can read the current snapshot. Both additive; no ABI-layout change yet
(`Runtime::Input()` is a method; `InputSnapshot` is passed by value/ref, not embedded in `EngineContext`).

Add to `InputSnapshot` (after `mouseButtons`):
```cpp
        // Mouse cursor position in WINDOW pixels (top-left origin, +y down).
        float mouseX = 0.0f;
        float mouseY = 0.0f;
```
Fill it in `InputDevices::Sample` from SDL (`SDL_GetMouseState(&x,&y)` -> store as float). Keep the
`static_assert(is_trivially_copyable_v<InputSnapshot>)` passing.

Add to `Runtime` (store the host's latest snapshot; plugin reads it):
```cpp
        // Latest per-frame input snapshot. The host (Loom) stores it each frame
        // via SetInputSnapshot; plugins read it via Input() in their update hooks.
        void                 SetInputSnapshot(const InputSnapshot& snap) noexcept;
        const InputSnapshot& Input() const noexcept;
```
(Store a member `InputSnapshot m_input{}` in `Runtime::Impl`; `#include <Arcane/Input/InputSnapshot.hpp>`.)

- [ ] **Step 1: Write the failing test.** In `SandboxInputTest.cpp`:
```cpp
TEST_CASE("input snapshot carries cursor position and stays POD", "[sandbox]")
{
    static_assert(std::is_trivially_copyable_v<Arcane::InputSnapshot>);
    Arcane::InputSnapshot s{};
    s.mouseX = 12.5f; s.mouseY = -3.0f;       // fields exist + assignable
    CHECK(s.mouseX == 12.5f);
    CHECK(s.mouseY == -3.0f);
}
TEST_CASE("Runtime stores and returns the latest input snapshot", "[sandbox]")
{
    Arcane::Runtime rt;                         // owns its own TypeContext
    Arcane::InputSnapshot s{}; s.mouseX = 7.0f; s.mouseButtons = 0x1;
    rt.SetInputSnapshot(s);
    CHECK(rt.Input().mouseX == 7.0f);
    CHECK(rt.Input().mouseButtons == 0x1);
}
```
- [ ] **Step 2: Run, verify it fails** (`mouseX`/`SetInputSnapshot` undeclared): `... "[sandbox] *input*"` → FAIL.
- [ ] **Step 3: Implement** the two `InputSnapshot` fields + `Sample` fill + `Runtime::SetInputSnapshot`/`Input`.
- [ ] **Step 4: Regenerate + build + run** → PASS; full `[sandbox]~[gpu]` green; existing input tests
  (`InputActionsTest`) still pass (the snapshot change is additive — if any fails, it leaked).
- [ ] **Step 5: Commit** (`feat(arcane): InputSnapshot cursor position + Runtime input accessor`).

---

## Task 2 — Plugin ABI v2: ImGui handoff + DrawUI hook + version bump

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp`, `Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp` (+ `.hpp` if needed)
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp` / `Runtime.cpp` (store the ImGui handoff)
- Modify: `Arcane/PlaygroundGame/src/PlaygroundGame.cpp` (-> v2)
- Modify: `Arcane/Tests/src/PlaygroundGamePluginTest.cpp`, `Arcane/Tests/src/LoomSliceTest.cpp`
- Test: the two updated fixture tests are the gate.

**What:** Extend the plugin ABI so a plugin can draw ImGui. Bump `kGamePluginABIVersion` 1 -> 2.
`EngineContext` gains the ImGui handoff; `PluginVTable`/`PluginEntry` gain a `DrawUI` hook (the host
calls it between ImGui BeginFrame and Render — `Update` is too early per the loop facts above). Carry
the ImGui context through `Runtime` (host sets it; `PluginHost` reads it into `EngineContext`), so
`PluginHost`'s constructor signature stays unchanged.

`PluginABI.hpp` changes:
```cpp
inline constexpr uint32_t kGamePluginABIVersion = 2;   // was 1

struct EngineContext
{
    uint32_t               abiVersion;
    Astra::TypeContext*    typeContext;
    Astra::IWorkScheduler* workScheduler;
    Arcane::Runtime*       engine;
    // v2: ImGui cross-DLL handoff. The plugin calls ImGui::SetCurrentContext(imguiContext)
    // + ImGui::SetAllocatorFunctions(imguiAlloc, imguiFree, imguiUserData) in its Init so its
    // ImGui:: calls target the host's single context. Null when the host has no ImGui (headless).
    void* imguiContext  = nullptr;   // ImGuiContext*  (opaque here to avoid an imgui include in the ABI)
    void* imguiAlloc    = nullptr;   // ImGuiMemAllocFunc
    void* imguiFree     = nullptr;   // ImGuiMemFreeFunc
    void* imguiUserData = nullptr;
};

// v2: DrawUI -- the host calls this between ImGuiLayer::BeginFrame and Render so the plugin
// can issue ImGui:: calls. (Update runs in the sim phase, before BeginFrame -- too early.)
namespace PluginEntry { inline constexpr const char* kDrawUI = "GamePlugin_DrawUI"; }

struct PluginVTable
{
    // ... existing 7 ...
    void (*DrawUI)() = nullptr;      // v2
};
```
`Runtime` gains (host sets; `PluginHost` reads when building `EngineContext`):
```cpp
        // v2 ImGui handoff carried to plugins via EngineContext (host installs it).
        void  SetImGui(void* context, void* alloc, void* free, void* userData) noexcept;
        void* ImGuiContext()  const noexcept;
        void* ImGuiAlloc()    const noexcept;
        void* ImGuiFree()     const noexcept;
        void* ImGuiUserData() const noexcept;
```
`PluginHost`: resolve `GamePlugin_DrawUI` into `vtable.DrawUI`; when building `EngineContext`, set
`abiVersion = kGamePluginABIVersion` and copy the four imgui fields from `runtime`. Keep the ABI
mismatch check (`ABIVersion() == kGamePluginABIVersion`).

`PlaygroundGame.cpp` -> v2: `GamePlugin_ABIVersion` returns `kGamePluginABIVersion` (now 2; it already
returns the constant). In `GamePlugin_Init`, after `Astra::SetTypeContext`, if `ctx->imguiContext`,
call `ImGui::SetCurrentContext((ImGuiContext*)ctx->imguiContext)` +
`ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)ctx->imguiAlloc, (ImGuiMemFreeFunc)ctx->imguiFree,
ctx->imguiUserData)` (include `<imgui.h>`; link ImGui). Export a no-op `GamePlugin_DrawUI() {}`.

- [ ] **Step 1: Write/extend the failing test.** In `PlaygroundGamePluginTest.cpp` add assertions:
  the loaded plugin's `ABIVersion()` returns `Arcane::kGamePluginABIVersion` (2), and the host
  resolves a non-null `DrawUI` in the vtable. In `LoomSliceTest.cpp` confirm the plugin loads under a
  v2 `EngineContext` (with null imgui fields in the headless test) and a frame slice still runs (no
  crash). (Read both files for their existing harness; extend, don't rewrite.)
- [ ] **Step 2: Run, verify it fails** (version still 1 / `DrawUI` unresolved).
- [ ] **Step 3: Implement** the `PluginABI.hpp` v2 changes + `Runtime` ImGui storage + `PluginHost`
  resolve/populate + `PlaygroundGame` v2 (context install + no-op DrawUI).
- [ ] **Step 4: Regenerate + build + run** → the two fixture tests PASS; full `[sandbox]~[gpu]` +
  the plugin/loom tests green.
- [ ] **Step 5: Commit** (`feat(arcane): plugin ABI v2 -- ImGui handoff + DrawUI hook`).

---

## Task 3 — Loom host wiring: store input + call DrawUI between BeginFrame and Render

**Files:**
- Modify: `Arcane/Loom/src/main.cpp`
- Test: extend `Arcane/Tests/src/LoomSliceTest.cpp` (headless DrawUI + input store path).

**What:** Wire the host side of ABI v2. (1) Capture the per-frame `InputSnapshot` into a local and
store it on the Runtime before consuming it. (2) Install the host's ImGui context on the Runtime once
(before/at plugin load) so `PluginHost` hands it to the plugin. (3) Call `vt->DrawUI()` between
`imgui->BeginFrame()` and `imgui->Render()`.

In `main.cpp`:
- At the input block (~line 173): split the inline `Sample(...)` into a local and store it:
```cpp
const Arcane::InputSnapshot snap =
    inputDevices->Sample(imgui->WantCaptureKeyboard(), imgui->WantCaptureMouse());
runtime.SetInputSnapshot(snap);
input->Update(frameDt, snap);
```
- After `imgui` is created and before `plugin.Load()` (the Runtime is constructed at line 135, plugin
  at 136 — move the imgui install before `Load` so the plugin's `Init` sees the context): set
  `runtime.SetImGui(ImGui::GetCurrentContext(), /*alloc*/..., /*free*/..., /*ud*/...)` using
  `ImGui::GetAllocatorFunctions(&alloc, &free, &ud)`. NOTE: the `Runtime` + `PluginHost` are
  constructed at lines 135-136 inside the inner scope; the `imgui` layer at line 105 (outer). Install
  the imgui handoff on `runtime` immediately after it is constructed (line ~135) and BEFORE
  `plugin.Load()` (line 137).
- After the host's own ImGui panel (~line 198), call the plugin's UI hook:
```cpp
const Arcane::PluginVTable* vtUI = plugin.Vtable();
if (vtUI && vtUI->DrawUI) vtUI->DrawUI();
```

- [ ] **Step 1: Write/extend the failing test.** In `LoomSliceTest.cpp`, drive a headless slice that:
  stores a fabricated `InputSnapshot` via `runtime.SetInputSnapshot`, asserts `runtime.Input()`
  reflects it; and (with the loaded v2 PlaygroundGame) calls `vt->DrawUI()` once without crashing
  (DrawUI is a no-op there, so the gate is "resolves + callable + no crash"). If the slice harness
  can't open a window, assert the wiring via the Runtime accessors + a direct `vt->DrawUI()` call.
- [ ] **Step 2: Run, verify it fails** (wiring absent).
- [ ] **Step 3: Implement** the three `main.cpp` edits.
- [ ] **Step 4: Build + run** → LoomSlice green; build Loom; a manual smoke (`Loom.exe --frames 60`)
  exits 0. Full `[sandbox]~[gpu]` green.
- [ ] **Step 5: Commit** (`feat(arcane): Loom wires v2 input store + plugin DrawUI hook`).

---

## Task 4 — Sandbox plugin skeleton (project + entry points + SandboxApp + one scene + render)

**Files:**
- Create: `Arcane/Sandbox/Sandbox.vcxproj` wiring via `Arcane/premake5.lua` (new `project "Sandbox"`,
  /MD DLL, mirror `project "PlaygroundGame"`: `GAME_BUILD_DLL`, links Arcane.dll, post-build copy of
  `Sandbox.dll` next to `Loom.exe` + `ArcaneTests.exe`).
- Create: `Arcane/Sandbox/src/GameApi.hpp` (copy PlaygroundGame's `GAME_API` macro),
  `Arcane/Sandbox/src/Sandbox.cpp` (the 8 entry points), `Arcane/Sandbox/src/SandboxApp.hpp` / `.cpp`,
  `Arcane/Sandbox/src/Scenes.hpp` / `.cpp` (registry + ONE builder for now).
- Test: `Arcane/Tests/src/SandboxSmokeTest.cpp` (new; `[sandbox][gpu]`).

**What:** Stand up the plugin behind ABI v2 with ONE scene (Playground: a static ground + walls + a
few dynamic bodies), running the real physics + render systems. `Sandbox.cpp` mirrors
`PlaygroundGame.cpp`'s entry points but delegates scene/update/UI to a `SandboxApp` object.
`SandboxApp` owns the current scene index + (later) camera/interaction/HUD; for now it builds scene 0
and steps it. Add systems: `PhysicsSystem` (fixedUpdate), `TransformPropagationSystem` (fixedUpdate),
`RenderSubmissionSystem` (render), plus a render-phase physics-debug submission via the free function
`Arcane::DrawPhysicsDebug(Batcher2D&, const PhysicsWorld&, const PhysicsDebugDrawOptions&)`
(`Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp`) — the sandbox needs the `PhysicsWorld&` the
`PhysicsSystem` owns; expose it from the scene layer (read `PhysicsSystem.hpp` for the accessor, or
add one) so the plugin can pass it to `DrawPhysicsDebug` between `batcher->Begin/End`.

`Scenes.hpp`:
```cpp
namespace Sandbox
{
    struct SceneDef { const char* name; void (*build)(Astra::Registry&); };
    // Ordered registry; index 0 is the default scene.
    std::span<const SceneDef> SceneRegistry();
}
```
Scene 0 "Playground": a static floor body + two side walls (static `Collider2D` boxes) and ~5 dynamic
bodies (mixed `MakeCircle`/`MakeAabb` via the scene-layer `Collider2D` fixtures), via the same
`AddBody`/component pattern `PhysicsSystem` consumes (read `PhysicsComponentsTest`/`PhysicsSystemTest`
for the component-build idiom).

- [ ] **Step 1: Write the failing smoke test.** In `SandboxSmokeTest.cpp` (mirror `LoomSliceTest` +
  the headless-device pattern from `PhysicsDebugDrawTest`): load `Sandbox.dll` via `PluginHost` into a
  Runtime with a headless `RenderDevice`/batcher set as the render context, `Init` the plugin, step
  ~30 fixed frames + submit render, assert no crash and `Arcane::RenderErrorCount() == 0`.
- [ ] **Step 2: Run, verify it fails** (`Sandbox.dll` doesn't exist).
- [ ] **Step 3: Implement** the premake project + `Sandbox.cpp` (8 entry points; Init installs
  TypeContext + ImGui context + re-registers physics/transform/sprite components + adds systems +
  builds scene 0 via `SandboxApp`) + `SandboxApp` + `Scenes` (registry + scene 0). SaveState/LoadState
  mirror PlaygroundGame (snapshot the registry + the current scene index).
- [ ] **Step 4: Regenerate + build + run** → `SandboxSmoke` PASS; `Loom.exe --plugin Sandbox.dll
  --frames 120` shows the scene + debug draw, exits 0.
- [ ] **Step 5: Commit** (`feat(arcane): Sandbox plugin skeleton -- ABI v2 + physics scene + render`).

---

## Task 5 — Scene roster (registry + the remaining 7 builders + switch/reset)

**Files:**
- Modify: `Arcane/Sandbox/src/Scenes.cpp` (add 7 builders), `Arcane/Sandbox/src/SandboxApp.{hpp,cpp}`
  (scene switch + reset).
- Test: extend `Arcane/Tests/src/SandboxSmokeTest.cpp` (build EACH scene).

**What:** Fill the roster: (2) Box stack, (3) Pyramid, (4) Joint chain (revolute/distance/weld/
prismatic — use the existing `Joints` API the way `PhysicsJointsTest` does), (5) Rotation drop (boxes
at nonzero angle settle flat — `MakePolygon`, NOT `MakeAabb`, per the dynamic-AABB-fixedRotation
assert), (6) CCD bullet (a fast `bullet=true` body vs a thin static wall), (7) Compound bodies
(multi-fixture `Collider2D`, off-COM tipping), (8) Mixed shapes (circles/capsules/polygons).
`SandboxApp::SetScene(i)` rebuilds the registry into a FRESH registry region (clear entities / rebuild)
and `Reset()` rebuilds the current scene. Each builder is self-contained (no cross-scene state).

- [ ] **Step 1: Extend the failing smoke test.** Loop over `SceneRegistry()`: for each scene, build it
  (via `SandboxApp::SetScene(i)`), step ~30 frames + render, assert no crash + `RenderErrorCount()==0`.
  Assert `SceneRegistry().size() == 8`.
- [ ] **Step 2: Run, verify it fails** (only 1 scene exists).
- [ ] **Step 3: Implement** the 7 builders + `SetScene`/`Reset`.
- [ ] **Step 4: Regenerate + build + run** → all-scene smoke PASS; manually cycle scenes in Loom
  (scene switch will be HUD/key-driven in Task 8; for now `SetScene` is exercised by the test).
- [ ] **Step 5: Commit** (`feat(arcane): Sandbox scene roster -- 8 physics demo scenes`).

---

## Task 6 — Camera + render-bridge zoom (pan/zoom, screen<->world)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp` (`RenderContext2D`, line 20 — add `zoom`
  beside `cameraOffset` line 23); `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp` (line 36 applies
  `worldPos + cameraOffset` — change to apply offset+zoom); `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp`/`.cpp`
  (thread camera offset+zoom in — via `PhysicsDebugDrawOptions` or explicit params — so overlays match sprites).
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp`/`.cpp` (`SetCamera`/getters; `SetRenderContext` reads the stored camera),
  `Arcane/Loom/src/main.cpp` (the line-228 `SetRenderContext(batcher, vec2(0,0))` call-site).
- Create: `Arcane/Sandbox/src/Camera.hpp`.
- Modify: `Arcane/Sandbox/src/SandboxApp.{hpp,cpp}` (own a `Camera`; push it via `Runtime::SetCamera`).
- Test: `Arcane/Tests/src/SandboxCameraTest.cpp` (new; `[sandbox]`, CPU-only).

**What:** Give the showcase a navigable view. `RenderContext2D` currently carries only `cameraOffset`
(SceneResources.hpp:23), hardcoded to `(0,0)` by Loom (main.cpp:228); `RenderSystems.hpp:36` applies
`worldPos + cameraOffset`. Add `float zoom = 1.0f` to `RenderContext2D`; apply offset+zoom in
RenderSystems + DrawPhysicsDebug so sprites + overlays move together. The PLUGIN owns the camera:
`Camera` (sandbox) holds `offset`/`zoom` with `ScreenToWorld(px)`/`WorldToScreen(world)` (pure math —
the test gate); `SandboxApp` calls `Runtime::SetCamera(offset, zoom)` in its update (runs at
main.cpp:188, before render). `Runtime::SetRenderContext(batcher)` then writes `RenderContext2D` using
the STORED camera (drop the hardcoded offset param; update the single Loom call-site to
`SetRenderContext(batcher.get())`). This keeps Loom camera-agnostic and the plugin in control — the
model the editor wants.

- [ ] **Step 1: Write the failing test.** In `SandboxCameraTest.cpp` (pure math, no device):
```cpp
// offset (100,50), zoom 2: world (10,10) -> screen (100 + 10*2, 50 + 10*2) = (120,70); round-trips.
Sandbox::Camera cam; cam.offset = {100,50}; cam.zoom = 2.0f;
CHECK(cam.WorldToScreen({10,10}) == Approx2(120,70));
CHECK(cam.ScreenToWorld({120,70}) == Approx2(10,10));   // inverse
```
- [ ] **Step 2: Run, verify it fails** (`Camera` undeclared).
- [ ] **Step 3: Implement** `Camera` + the render-context zoom extension + apply in
  RenderSubmission/PhysicsDebugDraw + `SandboxApp` pushes the camera.
- [ ] **Step 4: Regenerate + build + run** → camera test PASS; `[sandbox]~[gpu]` green; Loom shows the
  scene at the camera's offset/zoom (manually nudge defaults to verify pan/zoom render).
- [ ] **Step 5: Commit** (`feat(arcane): Sandbox 2D camera + render-bridge zoom`).

---

## Task 7 — Interaction (pick / spawn / drag / throw)

**Files:**
- Create: `Arcane/Sandbox/src/Interaction.hpp` / `.cpp`.
- Modify: `Arcane/Sandbox/src/SandboxApp.{hpp,cpp}` (own `Interaction`; feed it the input snapshot + camera).
- Test: `Arcane/Tests/src/SandboxInteractionTest.cpp` (new; `[sandbox]`, CPU-only — fabricated input).

**What:** The mouse layer, built on `Runtime::Input()` (mouse pos/buttons from Task 1) + the camera
(screen->world) + the `PhysicsWorld` API. Behaviors: LMB on empty -> spawn the selected shape at the
cursor world point (create an entity with `RigidBody2D`+`Collider2D`); LMB on a body -> grab (pick via
`PhysicsWorld::OverlapShape` at a tiny query at the cursor; while held, drive the body toward the
cursor with a soft velocity target — `SetVelocity` each step); release -> throw (leave the cursor-derived
velocity); RMB drag -> pan camera; wheel -> zoom. Track edge transitions (press/release) from the
per-frame snapshot (`Interaction` keeps the previous frame's buttons).

- [ ] **Step 1: Write the failing test.** In `SandboxInteractionTest.cpp` (headless; build a Runtime +
  Sandbox scene OR drive `Interaction` directly against a `PhysicsWorld`): fabricate an `InputSnapshot`
  with `mouseButtons=LMB` at a known cursor over empty space -> tick interaction -> assert the body
  count increased by 1 at the expected world position (within 1e-3 via the camera). Then fabricate LMB
  over that body -> drag the cursor -> assert the body's position tracks toward the new cursor world
  point. Release -> assert it keeps a nonzero velocity (throw).
- [ ] **Step 2: Run, verify it fails** (`Interaction` undeclared).
- [ ] **Step 3: Implement** `Interaction` (pick/spawn/drag/throw) + wire into `SandboxApp::FixedUpdate`
  (read `Runtime::Input()`; map through `Camera`).
- [ ] **Step 4: Regenerate + build + run** → interaction test PASS; in Loom, click to spawn / drag /
  throw works; `[sandbox]~[gpu]` green.
- [ ] **Step 5: Commit** (`feat(arcane): Sandbox interaction -- spawn / drag / throw / pan / zoom`).

---

## Task 8 — ImGui HUD panel (DrawUI)

**Files:**
- Create: `Arcane/Sandbox/src/Hud.hpp` / `.cpp`.
- Modify: `Arcane/Sandbox/src/Sandbox.cpp` (`GamePlugin_DrawUI` -> `SandboxApp::DrawUI`),
  `Arcane/Sandbox/src/SandboxApp.{hpp,cpp}`.
- Test: `Arcane/Tests/src/SandboxHudTest.cpp` (new; `[sandbox]` — ImGui-through-plugin smoke).

**What:** The control panel, issued from the plugin's `GamePlugin_DrawUI` (host calls it between
BeginFrame/Render per Task 3). Panel: scene selector (dropdown + next/prev) + Reset; sim controls
(pause, single-step, time-scale slider, gravity vector + on/off); spawn settings (shape combo, size,
density); debug-draw toggles (colliders, AABBs, contacts, normals, COM, joints, sleep — flags consumed
by the `PhysicsDebugDraw` submission); live stats (body count, contact count via
`PhysicsWorld::ActiveContactCount`, FPS, step ms). The HUD reads/writes `SandboxApp` state (selected
scene, paused, spawn shape, debug flags); `SandboxApp` applies them (pause gates the physics step;
time-scale scales dt; debug flags gate the debug-draw submission).

- [ ] **Step 1: Write the failing test.** In `SandboxHudTest.cpp` (headless ImGui — create an
  `ImGuiContext` in the test, `SetCurrentContext`, `ImGui::NewFrame()`): call `SandboxApp::DrawUI()`
  (or the `Hud::Draw(app)` it delegates to), `ImGui::EndFrame()`/`Render()`; assert no crash + no ImGui
  assert, and that toggling a HUD field (drive the bound state directly) changes `SandboxApp`'s flag
  (e.g. set `paused=true` -> `SandboxApp::Step` is a no-op for the physics advance). Keep it as a
  no-window ImGui smoke (the real visual check is manual in Loom).
- [ ] **Step 2: Run, verify it fails** (`Hud`/`DrawUI` content absent).
- [ ] **Step 3: Implement** `Hud` + `SandboxApp::DrawUI` + the state it drives (pause/time-scale/
  gravity/spawn/debug-flags/scene-switch).
- [ ] **Step 4: Regenerate + build + run** → HUD test PASS; in Loom the panel drives scenes, sim,
  spawn, and debug overlays live; `[sandbox]~[gpu]` green.
- [ ] **Step 5: Commit** (`feat(arcane): Sandbox ImGui HUD -- scene/sim/spawn/debug controls`).

---

## Task 9 — Cleanup (retire Playground.exe) + docs + full-suite / dual-flavor gate

**Files:**
- Delete: `Arcane/Playground/` (project + `main.cpp`); remove `project "Playground"` from `Arcane/premake5.lua`.
- Modify: `Arcane/Loom/src/main.cpp` (default `--plugin` -> `"Sandbox.dll"`).
- Modify: `CLAUDE.md` (the "Build System (Arcane engine)" section: Loom host + Sandbox showcase; Playground retired).
- Verify: CI (`Jenkinsfile`, `ci/`, `Arcane/scripts/`) + CLAUDE.md for `Playground.exe` references; repoint to Loom.

**What:** Remove the retired monolith, make Sandbox the default, document the new layout, and gate the
whole feature green both flavors.

- [ ] **Step 1:** `git grep -n "Playground\.exe\|project \"Playground\"\|Playground/" -- Jenkinsfile ci
  Arcane/scripts CLAUDE.md docs` — list every reference; repoint each to Loom (`Loom.exe --frames N`
  is the headless scripted-verify replacement). If a CI stage ran `Playground.exe`, switch it to Loom.
- [ ] **Step 2:** Delete `Arcane/Playground/`; remove its premake `project`; flip Loom's default plugin
  to `Sandbox.dll`; update `CLAUDE.md`. Regenerate the workspace.
- [ ] **Step 3: Full suite, both configs:** build + run the ENTIRE `ArcaneTests` (no filter) Debug AND
  Release, both backends (exercises `[gpu]` D3D12+Vulkan), exit 0 (run from the exe dir; if the Debug
  `[gpu]` runner hangs on the Vulkan layer: `taskkill //F //IM ArcaneTests.exe`, rebuild, retry).
- [ ] **Step 4: Dual-flavor gate:** msbuild `Server/Aphelyon.slnx -t:ArcaneCore` Debug+Release -> 0
  errors (Core is untouched by this feature; confirm no accidental coupling).
- [ ] **Step 5:** Manual acceptance in Loom: `Loom.exe --backend vulkan` (default Sandbox) — cycle all
  8 scenes, spawn/drag/throw, toggle debug overlays + sim controls via the HUD, F5/F6 hot-reload
  preserves/rebuilds the scene.
- [ ] **Step 6: Commit** (`refactor(arcane): retire Playground.exe; Loom+Sandbox is the showcase`),
  then dispatch the final holistic review, then STOP — defer push/merge to the user.

---

## Notes on plan altitude (honest)

The ABI/host/render-bridge tasks (1-3, 6) are concrete engine changes with exact signatures + analytic
tests. The plugin tasks (4-8) describe scene/interaction/HUD CONTENT at structure altitude (the exact
body counts / pixel layout are not analytic) with the CONCRETE gate being the headless smoke
(no-crash + `RenderErrorCount()==0`), the camera math test, and the fabricated-input interaction test.
The real visual acceptance is the manual Loom run in Task 9 Step 5. The TDD micro-steps (failing test
-> impl -> pass -> commit) are executed by the subagent-driven-development implementers.

## Self-review checklist (run before executing)
- **Spec coverage:** ABI v2 input+ImGui (T1-T2), Loom host wiring + DrawUI (T3), Sandbox plugin
  skeleton (T4), 8-scene roster (T5), camera/pan/zoom (T6), interaction spawn/drag/throw (T7), ImGui
  HUD with sim/spawn/debug controls (T8), retire Playground.exe + Loom default + docs + full/dual gate
  (T9). InputSnapshot cursor gap closed (T1). PlaygroundGame kept as fixture, bumped v2 (T2). All spec
  parts covered. ✓
- **Engine philosophy:** integration-first; additive ABI; untouched-path checks are tripwires, not
  freeze constraints. ✓
- **Type consistency:** `InputSnapshot.mouseX/Y` + `Runtime::SetInputSnapshot/Input` (T1) ->
  `EngineContext` imgui fields + `PluginVTable::DrawUI` + `Runtime::SetImGui` (T2) -> Loom wiring (T3)
  -> `SceneDef`/`SceneRegistry`/`SandboxApp` (T4-5) -> `Camera`/`SetCamera` (T6) -> `Interaction`
  (T7) -> `Hud`/`DrawUI` (T8). Named consistently. ✓
- **Each task keeps the build green:** PlaygroundGame stays the loaded default until T9 flips it;
  ABI v2 lands with PlaygroundGame+tests updated in the same task (T2). ✓
- **No placeholders / real commands + paths throughout.** ✓
