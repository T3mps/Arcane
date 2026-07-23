# Arcane Project Format — Slice 1b: Host/Editor Wiring — Design

**Date:** 2026-07-22
**Status:** Design (approved in brainstorm; pending written-spec review)
**Parent spec:** `docs/superpowers/specs/2026-07-22-arcane-project-format-design.md` (Slice 1b of §11)
**Scope:** Make the Arcane Editor and the Loom runtime host actually **open a project** via
`Project::Open` — load its `gameModule` through the existing ABI-versioned plugin host and
resolve content through the project's mounts — instead of today's `--plugin <path>` +
`data/`-next-to-exe. This is the visible payoff of the project format built in Slice 1.

---

## 1. Motivation & starting point

Slice 1 (merged `@2e781d08`) built the engine-side foundation in `Arcane/Arcane/src/Arcane/Project/`:
`AssetId`, `MountTable`, `ProjectManifest`, and `Project` (`Open`/`Create`/`ResolveAsset`).
Nothing consumes it yet. Both hosts still boot the old way:

- `LoomConfig` from argv → `GpuContext::Create(cfg)` → emplace `Runtime` →
  `PluginHost(runtime, cfg.pluginPath)` (`"Sandbox.dll"`, resolved next to the exe) → `Load()`.
- `GpuContext::Create` loads `data/input_actions.json` (relative, before any project exists).
- Editor fonts load from `ExeDir()/data/font/...` (editor chrome).
- The `Assets` facade (owned by `Runtime`, `Runtime.cpp:105`) is path-keyed; **Sandbox loads
  nothing through it**.

Slice 1b wires the foundation into that boot path.

## 2. Decisions (locked in brainstorm)

| # | Decision | Choice |
|---|---|---|
| a | Who owns the `Project` | **`Runtime`** (`std::optional<Project>`); it also owns `Assets`, and a headless runtime host inherits it. |
| b | How to specify the project | **`--project <path>`** (optional) on `LoomConfig`, **and** editor **File → Open Project** (SDL folder dialog, in-session soft-restart). |
| c | Loading the game DLL | `manifest.gameModule` loaded through the **existing `PluginHost`**; ABI cross-checked against `manifest.engineAbi`. `--plugin` remains the fallback. |
| d | Content routing depth | **Thick**: `Assets` content-root set from the project's `game://` mount (plumbing + unit test); `input_actions.json` moves into the project's `Content/` and loads via the mount (the observable proof). Fonts stay exe-relative (editor chrome). |
| e | `engine://` content | **Reserved-empty** (no engine content to mount yet). |
| f | First Aphelyon-project location (§13.3) | **Deferred to Slice 5** (needs the SDK to build). |
| g | File → Open re-open model | **Soft-restart in-process** (reuse plugin Unload/Load + `Runtime::OpenProject`); window/device/session persist. |
| h | `Assets` facade depth | **Plumbing + test + input-proof**; no render-content authored, no new load site. |
| i | `Project` default ctor | Made **private** (Open/Create are the factories). |
| j | Backward compat | **Non-breaking**: no `--project` ⇒ identical to today (Sandbox + `data/`). |

## 3. Runtime API additions

`Runtime` becomes the project owner. It already owns `Assets`, so content-root wiring stays
in-module and a future headless runtime host shares the same path.

```cpp
// Arcane/Base/Runtime.hpp
bool                    OpenProject(const std::filesystem::path& pathOrFile);
const Arcane::Project*  CurrentProject() const;   // nullptr when none open
```

- **`OpenProject` is validate-then-commit.** It calls `Project::Open(...)` first; on `nullopt`
  it returns `false` and **leaves all state untouched** — the property the File→Open switch
  relies on to avoid tearing down a live session for a bad project. On success it moves the
  `Project` into the runtime and calls `Assets().SetContentRoot(root / "Content")`.
- Accessor is `CurrentProject()` (not `Project()`) to avoid the member-name-vs-type clash.
- `Runtime` holds `std::optional<Arcane::Project>`. Because `Project`'s only construction paths
  are the static `Open`/`Create` factories, the optional move-constructs and never
  default-constructs — so `Project`'s default ctor can be private (decision i).

### `Assets::SetContentRoot`

```cpp
// Arcane/Assets/Assets.hpp
virtual void SetContentRoot(const std::filesystem::path& root) = 0;
```

A base directory prepended to **relative** paths in `GetTexture/GetBytes/GetJson`; absolute
paths pass through unchanged. This is the forward-plumbing the parent spec's resolver seam
wants (§6.4). No caller depends on it in 1b (Sandbox loads nothing through the facade); its
correctness is proven by a unit test. When Slice 2 lands `AssetId`→GUID→`AssetRegistry`,
resolution migrates behind the seam and this content-root becomes the fallback for legacy
path loads.

## 4. Host boot flow (shared Loom + EditorApp path)

- `LoomConfig` gains `std::string projectPath` and a `--project <path>` option (parsed in
  `LoomConfig.cpp` alongside `--plugin`). Precedence: with `--project`, the manifest's
  `gameModule` wins; `--plugin` is the no-project fallback.
- **Input-file load moves out of `GpuContext::Create`.** `GpuContext` still creates the
  `InputActions` object (it needs the input devices); the *file load* + `SetBaseContext("demo")`
  move to the app, after project open. `GpuContext::Create` no longer fails on a missing input
  file — the app owns that.

Revised `Init()` order (both hosts):

1. `GpuContext::Create(cfg)` — **no input-file load**.
2. emplace `Runtime`.
3. `if (!cfg.projectPath.empty()) runtime.OpenProject(cfg.projectPath)`.
4. **Load input through the project:** if a project is open, resolve `game://input_actions.json`
   via `CurrentProject()->Mounts()` → `Input().LoadFile(resolved)`; else fallback
   `data/input_actions.json`. Then `SetBaseContext("demo")`. *(This is the observable
   "content resolved through `game://`" proof — input working ⇒ the mount resolved.)*
5. `gameModule = project ? manifest.gameModule : cfg.pluginPath`. **ABI cross-check:**
   `manifest.engineAbi == Arcane::kGamePluginABIVersion` (belt-and-suspenders over the
   `PluginHost`'s own DLL-ABI gate); mismatch → refuse with a clear error.
6. emplace `PluginHost(runtime, gameModule)` + `Load()`.
7. Editor only: window title = `project ? "Arcane Editor — " + manifest.name : "Arcane Editor"`;
   the Assets panel shows the open project's name + root.

Nothing in the teardown contract changes: member declaration order (`m_gpu` first ⇒ destructs
last) is untouched; `Runtime` simply carries an extra `optional<Project>`.

## 5. File → Open Project (editor only, soft-restart)

- **Picker:** SDL3 `SDL_ShowOpenFolderDialog`, parented to `m_gpu->Win().SdlWindow()`. SDL
  dialogs are async (callback-driven off the event pump), so the callback stashes the picked
  path in a member; the switch executes at the **top of the next frame**, never mid-render.
  *(Confirm the vendored SDL3 headers export `SDL_ShowOpenFolderDialog` at plan time; SDL 3.1.2+.)*
- **Switch sequence (validate before teardown):**
  1. `Project::Open(picked)` → on `nullopt`, log + **abort with no teardown** (session intact).
  2. `Device().Nvrhi()->waitForIdle()`; `m_play.Stop(...)` if playing; `m_selection.Clear()`;
     `m_undo->Clear()`.
  3. `m_plugin.reset()` — dtor runs `Unload` (`ClearSystems + ResetRegistry`) while the DLL is
     still mapped (the existing teardown contract).
  4. `runtime.OpenProject(picked)` (commits project + content-root); reload input via the new
     mount; re-title the window.
  5. `m_plugin.emplace(runtime, newGameModule)` + `Load()`. On `Load` failure: log ERROR and
     leave the editor in a no-plugin state (the user can Open another project). Full
     rollback-to-previous-project is a deferred nicety.

## 6. Demo project artifact

A committed **`Arcane/SampleProject/`** so `--project` and the headless test have a real thing
to open (no `Arcane::Guid`/SDK needed):

```
Arcane/SampleProject/
├─ SampleProject.arcproj      # formatVersion 1, name "SampleProject", engine.abi 5,
│                             #   gameModule "Sandbox.dll", plugins [], bootScene ""
├─ Content/
│  └─ input_actions.json      # the game:// input-config proof
├─ Source/  Config/  Plugins/ # skeleton (empty)
└─ .gitignore                 # Binaries/ Intermediate/ Saved/
```

- `gameModule "Sandbox.dll"` resolves next-to-exe exactly like `--plugin` today — no
  `Binaries/`/SDK, which is Slice 5.
- **Post-build copy:** premake copies `SampleProject/` next to the exe (mirroring the existing
  `Loom/data/input_actions.json` copy step). The no-project fallback still needs
  `bin/data/input_actions.json`, so the input-config source is copied to both
  `bin/SampleProject/Content/` and `bin/data/` (single in-tree source of truth; exact wiring
  finalized in the plan).

## 7. Testing strategy

- **Headless `[project]` / `[assets]` (TDD, subagent-driven):**
  - `Runtime::OpenProject(valid)` → `CurrentProject()` non-null; mounts resolve;
    `Assets` content-root set.
  - `Runtime::OpenProject(invalid)` → `false`; state unchanged (no project, no content-root change).
  - Re-open A → B switches `gameModule` + mounts.
  - `Assets::SetContentRoot(tmp)` + relative `GetJson("foo.json")` loads `tmp/foo.json`;
    absolute paths bypass the root.
- **Desk-verify (`[gpu]`/interactive — human-in-the-loop, per the machine's Parsec GPU hazard):**
  - `ArcaneEditor --project SampleProject` → opens, window re-titles, Sandbox renders, input works.
  - `Loom --project SampleProject --frames N` → loads gameModule via the project, exits 0.
  - **File → Open Project** → pick `SampleProject` → editor re-titles + reloads the plugin.
  - No-arg `ArcaneEditor` / `Loom` → unchanged (Sandbox + `data/`).

**Baseline gates to hold:** `[project]` 76/18 (grows), `~[gpu]` 27951/366 (no regression).
Build: VS18 MSBuild on `Arcane\Arcane.slnx` (`/t:ArcaneTests|ArcaneEditor`); **new files ⇒
`Arcane\GenerateProjects.bat` first**; run `ArcaneTests.exe` from its bin dir.

## 8. Out of scope (later slices)

`Arcane::Guid` / `AssetRegistry` / `.meta` (Slice 2); layered JSON config — which later moves
`input_actions.json` → `Config/input.json` (Slice 3); `.arcplugin` + `plugin://` (Slice 4);
engine-as-SDK + the first external Aphelyon-client project (Slice 5); `bootScene` / scene
loading; binary content packages (future spec). The `input_actions`-in-`Content/` placement is
a deliberate 1b bridge that Slice 3 supersedes.

## 9. Risks & mitigations

- **Boot-order surgery on the shared `GpuContext`.** Moving the input-file load out is a
  behavior change to a helper both hosts use → covered by the no-project fallback test + the
  desk-verify that a no-arg run is unchanged.
- **File→Open mid-session teardown ordering.** `waitForIdle` before `m_plugin.reset()` (plugin
  GPU resources built via `SetRenderResources`); validate-before-teardown so a bad pick never
  strands the session. The pure re-open (`OpenProject` A→B) is unit-tested; the dialog path is
  desk-verified.
- **SDL folder-dialog availability.** Verify the symbol in the vendored SDL3 headers at plan
  time; if absent, fall back to a minimal Win32 `IFileDialog` behind the same app-level seam.
