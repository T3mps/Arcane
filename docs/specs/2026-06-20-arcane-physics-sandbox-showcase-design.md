# Arcane Physics Sandbox / Engine Showcase — Design

> **Context.** A living, interactive showcase of the Arcane engine *as it stands today*, hosted by
> the `Loom` plugin host. It demonstrates the Physics v2 rigid-body core (Phase A: rotation-aware,
> fixture-based, compound-COM) plus the engine substrate (Astra ECS, transform/render systems, debug
> draw, input, ImGui) running through their REAL production paths. It is the baseline we update as the
> engine grows — through the EPA/MPR narrowphase work, soft bodies, particles, destruction — until the
> editor and real game exist, at which point this code is their seed. It also consolidates the three
> current demo entry points into one clean host + showcase plugin + test fixture.

## Goal

Build a new hot-reloadable plugin `Sandbox.dll` (under `Arcane/Sandbox/`) that is the engine's living
demo: curated physics scenes you cycle through AND interact with (spawn / drag / throw bodies, tweak
sim params live via an ImGui panel), rendered with the real scene + physics-debug-draw paths. Extend
the plugin ABI (v1 -> v2) to carry input and ImGui through the plugin boundary — forward-useful
plumbing the editor and real game will stand on. Clean up the current demo sprawl: retire the
monolithic `Playground.exe`, keep `PlaygroundGame.dll` as the minimal hot-reload test fixture, make
`Loom.exe` the single showcase host.

## Locked decisions (from brainstorming)

1. **Character = full interactive showcase.** Curated scenes (gallery) AND interaction (spawn / drag /
   throw / live param tweak). Not a passive reel, not a bare gallery.
2. **Extend the plugin ABI for input + ImGui (v1 -> v2).** Making it interactive is not a boundary
   violation — plugins SHOULD use engine features; input and ImGui simply are not plumbed through
   `EngineContext`/`Runtime` yet. Extending them is legitimate, forward-useful engine work.
3. **New `Sandbox.dll`, not an evolution of `PlaygroundGame`.** `PlaygroundGame.dll` stays as the tiny,
   stable hot-reload test fixture (its two tests untouched in content, bumped to ABI v2). The showcase
   grows freely without churning a test fixture.
4. **Retire `Playground.exe`** (the monolithic M4 direct-engine demo). Its only unique value — the
   headless `--frames N` scripted-verify smoke — folds into `Loom` gaining a `--frames N` headless
   mode. (Confirm nothing in CI invokes `Playground.exe` directly before removal; repoint to Loom if so.)
5. **`Loom.exe` is the single showcase host:** loads `Sandbox.dll` by default, keeps F5/F6 hot-reload,
   gains headless `--frames`.

## Architecture

Three demo paths today (Playground monolith / Loom host / orbit fixture) collapse to: **one host
(`Loom`) + one showcase plugin (`Sandbox`) + one test fixture (`PlaygroundGame`)**.

```
Loom.exe (host)
  owns: Window, RenderDevice, Swapchain, Canvas, Batcher2D, TonemapPass,
        ImGuiLayer, input sampling, the Runtime, the RunLoop.
  per frame: sample input -> store on Runtime; BeginFrame(ImGui);
             RunLoop tick (FixedUpdate/Update/Render phases drive plugin + engine systems);
             plugin issues ImGui in Update; Render(ImGui) into backbuffer.
  loads: Sandbox.dll (default) via the GamePlugin ABI v2.
    |
    v
Sandbox.dll (the showcase, hot-reloadable)
  uses the REAL engine stack: Astra Registry + PhysicsSystem (-> v2 PhysicsWorld)
  + TransformPropagationSystem + RenderSubmissionSystem + PhysicsDebugDraw.
  owns: SceneRegistry (named demos), Camera, Interaction, Hud (ImGui).
```

The sandbox exercises PRODUCTION paths end to end — it is an integration check, not a parallel demo
harness (consistent with the one-canonical-path principle).

## Part 1 — Plugin ABI v2 (engine-side enabling work)

**`PluginABI.hpp`:** bump `kGamePluginABIVersion` 1 -> 2. `EngineContext` gains:
- **Input access:** the latest `InputSnapshot` (the existing POD), exposed via a new
  `Runtime::Input()` accessor. The host samples input each frame and stores the snapshot on the
  Runtime; the plugin reads it in `Update`/`FixedUpdate`.
- **ImGui handoff:** an `ImGuiContext*` plus the ImGui allocator function pointers. The plugin calls
  `ImGui::SetCurrentContext(...)` + `ImGui::SetAllocatorFunctions(...)` in its `Init` (the standard
  cross-DLL ImGui pattern; see the M2b dual-GImGui/`IMGUI_API`-export gotcha). The plugin then issues
  ImGui calls in `Update`; the host's existing `ImGuiLayer` renders them into the backbuffer.

**`InputSnapshot.hpp`:** add `float mouseX, mouseY` (the struct stays POD / trivially-copyable; the
replay/serialization contract is preserved) and have `InputDevices::Sample` fill it from SDL. Today
the snapshot carries mouse *buttons* but not *position* — a real gap that interactive picking needs.
This is an addressable-gap fix, not sandbox-only scaffolding.

**Host (`Loom`):** Loom already owns the window, input sampling, and the `ImGuiLayer`. Per frame it
samples input -> stores the snapshot on `Runtime`; hands its ImGui context + allocators into
`EngineContext` at plugin load; wraps the plugin's render-phase `Update` between `ImGuiLayer::BeginFrame`
and `Render` so plugin ImGui draws land on screen. Loom also gains the `--frames N` headless mode.

**`PlaygroundGame` -> ABI v2:** mechanical — returns 2 from `GamePlugin_ABIVersion`, installs the
ImGui context in `Init` (draws no UI), so the fixture still loads under the new `EngineContext`. Its
two tests (`PlaygroundGamePluginTest`, `LoomSliceTest`) get the same one-line v2 update. No behavior
change.

## Part 2 — The Sandbox plugin

**File structure (`Arcane/Sandbox/src/`, one responsibility per file):**

| File | Responsibility |
|---|---|
| `Sandbox.cpp` | The 7 `GamePlugin_*` entry points (thin, like `PlaygroundGame.cpp`): install TypeContext + ImGui context, re-register components, add systems, own a `SandboxApp`, route Save/LoadState. |
| `SandboxApp.{hpp,cpp}` | Orchestrator: holds the `SceneRegistry`, current scene index, `Camera`, `Interaction`, `Hud`; drives per-phase update; owns scene switch/reset. |
| `Scenes.{hpp,cpp}` | `SceneRegistry` = ordered list of `{ const char* name; void(*build)(Astra::Registry&); }`. One builder per demo. THE GROWTH SEAM. |
| `Interaction.{hpp,cpp}` | Mouse picking (`PhysicsWorld::OverlapShape`/`QueryAABB` at the cursor world point), spawn, mouse-spring drag (`SetVelocity` toward target), throw (release with cursor velocity). |
| `Camera.hpp` | Simple 2D camera: pan (render-bridge `cameraOffset`) + zoom; `ScreenToWorld`/`WorldToScreen`. |
| `Hud.{hpp,cpp}` | The ImGui control panel. |

**Initial scene roster** (each demonstrates a current capability):
1. **Playground** (default) — ground + walls + mixed bodies; the free-interaction canvas.
2. **Box stack** — stable vertical stacking (warm-start / stable feature IDs).
3. **Pyramid** — friction + stress pile.
4. **Joint chain** — revolute / distance / weld / prismatic hanging chain.
5. **Rotation drop** — boxes dropped at angles settle flat (Phase-A rotation headline).
6. **CCD bullet** — fast body vs thin wall (no-tunnel).
7. **Compound bodies** — multi-fixture bodies tipping about their COM (fixture model + compound-COM).
8. **Mixed shapes** — circles / capsules / polygons interacting (unified shape model).

**Interaction model:** left-click empty space -> spawn the selected shape at the cursor world point;
left-click a body -> grab (mouse-spring drag — body follows the cursor via a soft velocity target);
release -> throw with the cursor velocity; right-drag -> pan camera; wheel -> zoom. All built on the
existing `PhysicsWorld` API (`OverlapShape` to pick; `SetVelocity`/`ApplyImpulse` to drag/throw) — no
new physics API.

**Camera:** sandbox-owned 2D camera (pan via the render bridge's `cameraOffset`; zoom requires a small
`Runtime::SetRenderContext` extension to carry a scale, OR the plugin pre-scales positions — resolved
in planning, minor render-bridge touch).

**HUD (ImGui panel):**
- Scene selector (dropdown / next-prev) + Reset scene.
- Sim controls: pause, single-step, time-scale slider, gravity vector/toggle.
- Spawn settings: shape (box/circle/capsule/polygon), size, density.
- Debug-draw toggles (all via the existing `PhysicsDebugDraw`): colliders, AABBs, contacts, normals,
  COM, joints, sleep state.
- Live stats: body count, contact count, FPS, step ms.

## Part 3 — Cleanup

- **Delete `Arcane/Playground/`** (project + `main.cpp`); remove it from the workspace
  (`Arcane/premake5.lua`) and regenerate. Fold its `--frames N` scripted-verify into `Loom --frames N`.
  Verify CI/scripts/CLAUDE.md references and repoint them to Loom.
- **`PlaygroundGame.dll`** kept as the minimal hot-reload test fixture (orbit scene), bumped to ABI v2
  (mechanical).
- **`Loom.exe`** becomes the default showcase host (loads `Sandbox.dll`), keeps F5/F6, gains `--frames`.
- Update `CLAUDE.md` (the Arcane build/run section) to describe the new host + sandbox + the retired
  Playground.

## Testing

The sandbox drives production paths, so its tests are real integration coverage:
- **Headless scene smoke** (in `ArcaneTests`, LoomSlice-style, `[gpu]` where it renders): load
  `Sandbox.dll`, build EACH scene in the roster, step N fixed frames headless, assert no crash and
  `Arcane::RenderErrorCount() == 0`. Catches a scene-builder or ABI-v2 regression.
- **ImGui-through-plugin smoke:** the plugin installs the context and issues ImGui calls headless
  without tripping ImGui asserts or render errors.
- **`Loom --frames N`:** the CI scripted-verify path (replaces `Playground.exe --frames`): default
  scene, N frames, exit 0.
- **`PlaygroundGame` tests** (`PlaygroundGamePluginTest`, `LoomSliceTest`): updated mechanically for ABI v2.
- **Not a determinism gate** (interactive). Physics determinism stays gated by the existing physics
  suite; the sandbox adds no determinism obligations.

## Determinism / contracts

- The sandbox is /MD (engine flavor), like `PlaygroundGame`. It uses header-only engine scene
  components that instantiate in the plugin module; the Registry/schedulers/RunLoop live in Arcane.dll
  via the Runtime (same model as `PlaygroundGame`).
- The ABI-v2 additions (input accessor, ImGui handoff, `InputSnapshot.mouseX/Y`) are additive; the
  `InputSnapshot` POD/trivially-copyable contract is preserved.
- No presentation/dialect leak into Core — the sandbox lives in the engine layer (Arcane workspace),
  never in `Core/`.

## Non-goals (YAGNI / deferred)

- **No editor features** (gizmos, property inspectors, scene save-to-disk authoring) — this is a
  showcase, not the editor. The sandbox is the editor's SEED, not the editor.
- **No new physics capabilities** — it showcases what EXISTS. New scenes/overlays are added as the
  engine grows (EPA deep-overlap viz, soft bodies, particles, destruction each add a scene/toggle later).
- **No networking / serialization-to-disk** of sandbox scenes (scenes are code builders). Hot-reload
  Save/LoadState round-trips the live registry, same as `PlaygroundGame`.
- **No gamepad interaction** in v1 (keyboard + mouse only); the snapshot already carries gamepad, add later.
- **Camera zoom polish / multi-viewport** — a single 2D pan+zoom camera; richer camera is later.

## Success criteria

- `Loom.exe` loads `Sandbox.dll` and shows an interactive physics showcase: cycle the 8 scenes,
  spawn/drag/throw bodies with the mouse, tweak gravity/time-scale/debug-draw via an ImGui panel, all
  rendered through the real scene + `PhysicsDebugDraw` paths.
- Plugin ABI v2 carries input + ImGui; `InputSnapshot` carries cursor position; `PlaygroundGame` loads
  under v2; the two fixture tests pass.
- `Playground.exe` is gone; `Loom --frames N` provides the headless scripted-verify; CI/docs repointed.
- Headless scene-smoke + ImGui-through-plugin tests green; full `ArcaneTests` green Debug + Release both
  backends; ArcaneCore still builds static-CRT (Core untouched).
- Hot-reload works: rebuild `Sandbox.dll` while Loom runs -> reload preserves the live scene (F5) or
  rebuilds fresh (F6).

## Next step

On approval: `writing-plans` -> a phased implementation plan (ABI v2 input+ImGui handoff + InputSnapshot
cursor -> Loom host wiring + `--frames` + Playground retire -> Sandbox plugin skeleton (entry points +
SandboxApp + one scene) -> scene roster -> camera + interaction -> ImGui HUD -> headless smoke tests +
docs), executed via subagent-driven-development.
