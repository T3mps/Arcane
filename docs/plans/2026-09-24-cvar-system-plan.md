# CVar system — registry, provenance, and the two consoles

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One typed registry of scalar knobs and commands, in `ArcaneCore` beside `Config`, with `SetBy` history, a frame publish barrier, default-deny, and `--set`. The editor Console tab and the runtime overlay are the two presentations of one console model. No window is required for the registry to work.

**Architecture:** A declaration (`ARC_CVAR`) is a handle into registry-owned storage, not a value living in the declaring module. Reads hit an immutable snapshot published at the frame boundary. `Set` writes a pending store and records who set it. Weaker sources cannot stomp stronger ones. Config layers (`engine → plugin → project → user`) are applied as `SetBy` rungs through the existing `Arcane::Config` documents. Commands share the registry. The console model (history, input line, autocomplete, permission context) is engine-owned; the editor draws it in the Console tab and the runtime draws it as an ImGui overlay.

**Tech Stack:** C++23, `ArcaneCore` (`Config`, `Arcane::Log`), nlohmann json, Catch2 (`ArcaneTests`), premake5 → `Arcane.slnx`. ImGui only for the two presentations (Tasks 10–11). No new thread.

**Spec:** `docs/specs/2026-09-02-cvar-system-design.md` — §3 types and flags (including decision 18, `UInt32`/`UInt64` in v1), §4 registry and module lifetime, §5 Config and `SetBy`, §6 publish barrier, §7 default-deny, §8 the two consoles, §9 tests, §10 out-of-scope list. Do not edit §10's sequencing sentence.

## Global Constraints

- **Values are typed, stored in the registry.** The declaration object holds a `CVarHandle` (index + generation) and nothing else a reader dereferences. Unloading a module must not leave a readable dangling value. This is the deliberate break from Source 2, whose `ConVar` owns its value and whose id still carries a raw pointer.
- **Types implemented in v1:** `Bool`, `Int32`, `UInt32`, `Int64`, `UInt64`, `Float32`, `Float64`, `String`. Reserved in the enum with no accessors: `Color`, `Vec2`, `Vec3`, `Vec4`. No `Int16` / `UInt16`. No `Qangle`.
- **`SetBy` is not serialized.** It is rebuilt from the layers on every boot. Ladder, weak → strong, numbered with gaps: `Default`, `EngineConfig`, `Plugin`, `Project`, `User`, `CommandLine`, `Code`, `Console`. A weaker `Set` is refused and the current value stays. `Plugin` is history-retaining: unregistering that module pops its contribution.
- **Archive writes only when `SetBy` is `User` or stronger.** A `Project` value must not be copied into the user layer. Writes go to the user Config dir, `<category>.json`, at the key path.
- **A `Set` is not visible until publish.** Outside the frame loop (boot, tests, tools) `Publish()` is immediate. There is no read-your-own-writes inside a frame. Callbacks run once, at publish, on the thread that publishes — never on the thread that called `Set`.
- **Default-deny.** A cvar is not settable from a `Player` or `Server` context unless it has `UserSettable`. `Archive` implies `UserSettable`. `Dev` is compiled out in Dist. `Hidden` is never compiled out and never appears in find or autocomplete. `Cheat` requires the cheats gate and reverts by popping `Console` and `Code`, not by snapping to the constructor default.
- **Names, not GUIDs.** First segment is the Config category, the rest is the key path: `diagnostics.drawMarkers` → `diagnostics.json` / `drawMarkers`. Duplicate registration is a hard error that names both modules. No implicit Source-style parent alias.
- **No plugin ABI bump.** `ARC_CVAR` registers through a new `ArcaneCore` export. Old game modules simply declare nothing. Do not add a field to the plugin descriptor. `kGamePluginABIVersion` stays 40 unless a task discovers it must change — stop and say so rather than bumping silently.
- **The publish barrier is not a new thread and not a fourth hand-rolled threading copy.** Hosts call `CVarRegistry::Publish()` at the existing frame boundary. Workers only read the snapshot.
- **`HostConfig` flags stay.** `--fixed-time`, `--frames`, `--settle`, and the rest are not cvars. `--set name=value` is an additional door. It carries a permission context (the editor's `--set` is `Editor`; the runtime's is `Player` unless a dev flag says otherwise — Task 7 pins which).
- **Out of this plan:** vector and colour accessors, `exec`, scalability, device profiles, a remote server console UI, forwarding the editor's live cvars into a standalone launch, `arcctl`, and turning thumbnail size or the mesh light triple into cvars (those re-bless the thumbnail golden set; they wait).
- **Build and tests:** `msbuild Arcane.slnx /p:Configuration=Debug /m` from the repo root. Run `ArcaneTests.exe` from `bin\Debug-windows-x86_64-md\ArcaneTests`. Capture the seed on failure. Tasks 1–9 do not touch a golden lane. Task 9's `diagnostics.drawMarkers` default stays `false`. Task 12's `kMeshCullEnabled` default stays `true`. If either default moves, stop — a golden diff is a bug, not a re-bless.
- **Commit per task.** Message style `feat(cvar):` / `test(cvar):`. Never `git add -A`. Do not stage the uncommitted crash-window files (`OffscreenVehicle`, `ReporterWindow`, `WarpImGui`, the crash spec) into a cvar commit.
- **Process:** implementers commit before writing their report. Do not terminate an editor you did not launch.

## File structure

| Path | Responsibility |
|---|---|
| `ArcaneCore/src/Arcane/Config/CVarTypes.hpp` (new) | `CVarType`, `CVarFlags`, `SetBy`, `CVarValue` (typed union including `UInt32`/`UInt64`), `Permission` (`Editor`/`Player`/`Server`) |
| `ArcaneCore/src/Arcane/Config/CVarHandle.hpp` (new) | `CVarHandle` — index + generation; default is stale |
| `ArcaneCore/src/Arcane/Config/CVarRegistry.hpp` (new) | registry API: register, find, get, set, publish, unset-module, enumerate, explain |
| `ArcaneCore/src/Arcane/Config/CVarRegistry.cpp` (new) | storage, pending vs snapshot, history, callbacks-by-id, command dispatch |
| `ArcaneCore/src/Arcane/Config/CVarDecl.hpp` (new) | `ARC_CVAR` / `ARC_COMMAND` — intrusive registration, module tag |
| `ArcaneCore/src/Arcane/Config/CVarConfig.hpp` (new) | apply a `Config` layer as a `SetBy`; unknown-key list; archive write |
| `ArcaneCore/src/Arcane/Config/Config.hpp` | unchanged merge rules; cvars read it, they do not replace it |
| `ArcaneClient/src/Arcane/Host/HostConfig.hpp` | `--set` parsed into a list, applied after the registry exists |
| `ArcaneClient/src/Arcane/Render/GpuInstrumentation.hpp` | `drawMarkers` reads the published cvar instead of a private atomic |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.hpp` | `kMeshCullEnabled` becomes the default of `render.meshCull` (Task 12) |
| `ArcaneClient/src/Arcane/Host/ConsoleModel.hpp` (new) | input line, history, autocomplete, submit — no ImGui |
| `ArcaneEditor/src/Panels/EditorPanels.cpp` | Console tab gains the input line; Problems stays a separate tab |
| `ArcaneRuntime` frame | overlay presentation; `console_toggle` action, default `` ` `` / `~` |
| `ArcaneTests/src/CVarRegistryTest.cpp` (new) | §9 cases |
| `docs/specs/2026-09-02-cvar-system-design.md` | Status line at the close only. Do not touch §10's sequencing sentence |

---

### Task 1: Types and the value union

**Files:**
- Create: `ArcaneCore/src/Arcane/Config/CVarTypes.hpp`
- Test: `ArcaneTests/src/CVarRegistryTest.cpp` (started here, grown by later tasks)

**Interfaces:**
- Produces: `enum class CVarType : uint8_t { Bool, Int32, UInt32, Int64, UInt64, Float32, Float64, String, Color, Vec2, Vec3, Vec4 }`.
- Produces: `CVarFlags` as a bitmask — `Dev`, `Hidden`, `UserSettable`, `Archive`, `Cheat`, plus the reserved authority bits (`Replicated`, `ServerOnly`, `NotConnected`, `Protected`, `ServerCanExecute`, `ClientCmdCanExecute`) and `ReloadShaders`, `ReloadMaterials`, `Deterministic`. `Archive` implies `UserSettable` at registration, not at the call site.
- Produces: `enum class SetBy : uint8_t` with explicit gaps (`Default = 0`, `EngineConfig = 10`, `Plugin = 20`, `Project = 30`, `User = 40`, `CommandLine = 50`, `Code = 60`, `Console = 70`).
- Produces: `CVarValue` — a tagged union of the eight implemented types. `Color`/`Vec*` are not representable yet; constructing one does not compile.
- Produces: `enum class Permission : uint8_t { Editor, Player, Server }`.

- [ ] **Step 1: Write the failing round-trip tests** for all eight types, including a `UInt32` above `INT32_MAX` and a `UInt64` above `INT64_MAX`, plus bounds clamping (an int set above max becomes max, below min becomes min).
- [ ] **Step 2: Run the test, confirm it fails to compile.**
- [ ] **Step 3: Implement the header.** No registry yet.
- [ ] **Step 4: Run `CVarRegistryTest` and confirm the type cases pass.**
- [ ] **Step 5: Commit** `feat(cvar): typed values, flags, and SetBy rungs`.

### Task 2: Registry, handles, declaration, module teardown

**Files:**
- Create: `CVarHandle.hpp`, `CVarRegistry.hpp`, `CVarRegistry.cpp`, `CVarDecl.hpp`
- Modify: `ArcaneCore` premake file list only if the glob does not already pick `Config/*.cpp` (it does — confirm, do not add a second source list)
- Test: extend `CVarRegistryTest.cpp`

**Interfaces:**
- `CVarRegistry::Register(desc) -> CVarHandle`. `desc` carries name, type, default, min, max, flags, help, module id.
- `Find(name) -> CVarHandle` (stale if missing). `Get(handle) -> CVarValue` fails visibly on a stale handle (returns `std::optional`, does not throw, does not read freed memory).
- `UnregisterModule(moduleId)` drops that module's declarations and its callbacks. Outstanding handles to those cvars test stale.
- Duplicate name: registration fails, the error names both module ids. The first registration keeps the value.
- `ARC_CVAR(name, type, default, flags, help)` links a static descriptor. Tests register explicitly so they do not depend on static init order. The macro is what engine and game code will call; one engine cvar is declared with it in Task 9 to prove the macro.

- [ ] **Step 1: Tests** — round-trip get after register; stale handle after unregister; duplicate name names both modules; a handle to a different generation does not read the slot's new occupant.
- [ ] **Step 2: Run, confirm failure.**
- [ ] **Step 3: Implement.** Value storage is type-segregated arrays (a `Bool` read is one byte). Metadata (name, help, flags, history) is a parallel cold table. The snapshot and the pending store are the same layout; Task 5 wires the swap. Until then `Set` may publish immediately so Task 2's tests can read.
- [ ] **Step 4: Tests pass. Commit** `feat(cvar): registry owns values; handles die with their module`.

### Task 3: `SetBy` history

**Files:** `CVarRegistry.cpp`, tests

**Behaviour:**
- `Set(handle, value, SetBy)` refuses when `SetBy` is weaker than the rung that currently holds the value. The refused call is observable (a bool, or an enum result — pick one and test it) and does not change the value.
- Each successful `Set` pushes a history record `{SetBy, value, moduleId}`. `UnsetModule(moduleId)` removes every record tagged with that module, from declarations and from sets, and the strongest remaining record becomes current.
- `Explain(name)` returns the current value, the winning `SetBy`, and the history stack. This is the data `cvar_explain` will print in Task 6.

- [ ] **Step 1: Tests** — `Project` then `EngineConfig` leaves `Project` in place; `Console` then `Code` leaves `Console` in place (`Console` is stronger); popping the module that set `Plugin` reveals the `EngineConfig` value under it.
- [ ] **Step 2: Implement. Commit** `feat(cvar): weaker SetBy cannot stomp; module unload pops its sets`.

### Task 4: Config seam and archive

**Files:**
- Create: `CVarConfig.hpp` (+ `.cpp` if it is more than a header)
- Test: extend `CVarRegistryTest.cpp`

**Behaviour:**
- A category is tagged cvar-shaped or document-shaped. `diagnostics` and `render` are cvar-shaped. `input` is document-shaped.
- Applying a `Config` category at a `SetBy` walks keys. A known cvar is `Set` at that rung (so precedence still applies). An unknown key in a cvar-shaped category is recorded in an enumerable list and logged once. An unknown key in a document-shaped category is ignored.
- `WriteArchive(userDir)` writes an `Archive` cvar only when its winning `SetBy` is `User` or stronger. The file is `<category>.json`. A `Project`-sourced value produces no user file entry.

- [ ] **Step 1: Tests** from spec §9 — unknown key warns for `diagnostics` and not for `input`; a `Project` value is not archived; a `Console` value is.
- [ ] **Step 2: Implement against `Config::Category`. Do not change `DeepMerge`. Commit** `feat(cvar): Config layers are SetBy rungs; archive skips project defaults`.

### Task 5: Publish barrier

**Files:** `CVarRegistry.hpp/.cpp`, tests

**Behaviour:**
- `Set` writes pending. `Get` reads the snapshot. `Publish()` swaps them and runs callbacks once, in registration order, on the calling thread.
- A callback that calls `Set` affects the next publish, not this one.
- `Publish()` with no pending changes is a no-op (no callbacks).
- Boot helper: `PublishImmediate()` used by tests and by config apply before the frame loop starts, so the snapshot is valid before the first frame. Document that hosts call `Publish()` once per frame at the same point they already start the frame. Wire that call in the editor and runtime frame drivers in this task — one line each, no console yet.

- [ ] **Step 1: Tests** — `Set` then `Get` returns the old value; after `Publish`, the new value; a callback fires once; a `Set` inside the callback is invisible until the next `Publish`.
- [ ] **Step 2: Implement and call `Publish()` from both frame drivers. Commit** `feat(cvar): sets land at the frame publish, not mid-frame`.

### Task 6: Commands, `cvarlist`, `cvar_explain`

**Files:** `CVarDecl.hpp`, `CVarRegistry.cpp`, tests

**Behaviour:**
- `ARC_COMMAND(name, flags, help, fn)` registers a command: name, callback id, flags, help, no value. Same namespace as cvars. Duplicate name is the same hard error.
- Built-in commands: `cvarlist` (skips `Hidden`; skips `Dev` when the build compiled `Dev` out), `cvar_explain <name>` (prints `Explain()`).
- Dispatch is `Execute(line, Permission)`. Parsing is `name arg`. Unknown name is a visible error, not a silent no-op.

- [ ] **Step 1: Tests** — `cvar_explain` reports the winning layer; `cvarlist` omits a `Hidden` cvar; a command and a cvar cannot share a name.
- [ ] **Step 2: Implement. Commit** `feat(cvar): commands share the registry; cvarlist and cvar_explain`.

### Task 7: `--set` and default-deny

**Files:**
- Modify: `HostConfig`
- `CVarRegistry::Set` gains a `Permission` argument
- Test: `CVarRegistryTest.cpp` and the existing host-config test if there is one

**Behaviour:**
- `--set name=value` may repeat. Applied at `SetBy::CommandLine` after config layers and before the first frame, then `PublishImmediate()`.
- `Editor` permission may set any cvar that is not compiled out. `Player` and `Server` may set a cvar only when it has `UserSettable` (or `Archive`). `Cheat` is refused unless the cheats gate is on. The cheats gate is a single `Bool` cvar, `cheats`, itself `UserSettable` only from `Editor` — a player cannot turn cheats on. Pin that in a test.
- Reverting cheats pops `Console` and `Code` contributions on every `Cheat` cvar and republishes. Config layers remain.

- [ ] **Step 1: Tests** — an unflagged cvar is refused for `Player` and accepted for `Editor`; `--set` beats `User` and loses to `Code`; cheat revert restores the `Project` value rather than the constructor default.
- [ ] **Step 2: Implement. Commit** `feat(cvar): --set and default-deny`.

### Task 8: `diagnostics.drawMarkers` is a real cvar

**Files:**
- `GpuInstrumentation.hpp/.cpp` — delete the private atomic; read `CVarRegistry` snapshot
- Declare `ARC_CVAR(diagnostics.drawMarkers, Bool, false, Dev, ...)`
- `ProjectBoot` stops pushing the JSON bool into the atomic; Task 4's apply does it

**Behaviour:** default stays `false`. `Dev` means Dist builds compile the declaration out and the GPU markers stay off. Debug/Release still honour `diagnostics.json`.

- [ ] **Step 1: A test sets the cvar, publishes, and reads the instrumentation flag.**
- [ ] **Step 2: Implement. Confirm no second `drawMarkers` literal drives the flag. Commit** `feat(cvar): diagnostics.drawMarkers reads the registry`.

### Task 9: Console model, no widgets yet

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/ConsoleModel.hpp` (+ `.cpp`)
- Test: `ArcaneTests` can link it if it sits in `ArcaneCore` instead — prefer `ArcaneCore` next to the registry so the test does not need a host. Move it there if the client link makes the test awkward. The spec's "model lives in ArcaneClient" yields to testability: the model is presentation-free, so `ArcaneCore` is the right dll. Say so in the commit message. The spec's §8 sentence is the one line this plan is allowed to correct, in the close, because the core split happened after the spec.

**Behaviour:**
- Holds the input line, a ring of submitted lines, and autocomplete against `cvarlist`'s walk.
- `Submit(line, Permission)` calls `Execute` and stores the reply text.
- No ImGui, no input system, no window.

- [ ] **Step 1: Tests** — submit `cvar_explain diagnostics.drawMarkers` returns the layer; autocomplete of `diag` offers `diagnostics.drawMarkers`; a `Player` submit that default-deny refuses comes back as an error string, not an applied set.
- [ ] **Step 2: Implement. Commit** `feat(cvar): console model with no presentation`.

### Task 10: Editor Console tab

**Files:** `ArcaneEditor` Console panel (find the existing tab before adding a second one — Problems must stay separate)

**Behaviour:**
- The tab shows the model's history and an input line. Submit uses `Permission::Editor`.
- Autocomplete is the model's. Focus goes to the input line when the tab is shown.
- While the input line is active, ImGui `WantCaptureKeyboard` already eats keys. Do not add a second capture path.
- No overlay in the editor viewport.

- [ ] **Step 1: Wire the panel to `ConsoleModel`. Do not change Problems.**
- [ ] **Step 2: Build the editor. A manual check is enough; this task does not run the golden gate unless the panel's idle layout moves a captured editor-ui lane. If it does, stop and say so — do not re-bless in this task.**
- [ ] **Step 3: Commit** `feat(cvar): editor console tab submits through the registry`.

### Task 11: Runtime overlay and `console_toggle`

**Files:**
- Runtime frame, input action map (`input.json` default binding `` ` ``, the same key the spec calls `~` by convention — bind the key that `input.json` already uses for the tilde/backquote, do not hardcode a scan code in the frame)
- ImGui overlay drawn last

**Behaviour:**
- The overlay is the same `ConsoleModel` with `Permission::Player`.
- `console_toggle` opens and closes it. While open, game action maps are suppressed for the overlay's lifetime.
- Headless does not create the overlay and does not require it. `--set` still works.
- Dist still builds the overlay. `Dev` cvars are already compiled out, so the overlay cannot reach them.

- [ ] **Step 1: Add the action to the default input map. Toggle from the action, not from a key test in the frame.**
- [ ] **Step 2: Draw the overlay last. Commit** `feat(cvar): runtime console overlay, player permission`.

### Task 12: `render.meshCull`

**Files:** `MeshCullNode.hpp` and the cull dispatch

**Behaviour:**
- `ARC_CVAR(render.meshCull, Bool, true, Dev, "Frustum-cull mesh instances on the GPU.")`.
- The compile-time `kMeshCullEnabled` becomes the declaration's default and is no longer the thing the dispatch reads. The dispatch reads the published snapshot.
- Default `true` keeps every golden lane still. A test publishes `false` and observes the bypass the comment at `MeshCullNode.hpp:26` already describes (compute still runs, the frustum predicate does not).

- [ ] **Step 1: Write that test. Step 2: Implement. Step 3: Commit** `feat(cvar): render.meshCull replaces kMeshCullEnabled`.

---

## Close

- Spec status line: implemented, with the date and the plan path. Leave §10's "build nothing until Arc A and F2b" sentence as history.
- Correct spec §8's "model lives in ArcaneClient" only if Task 9 put the model in `ArcaneCore`. One sentence, naming the core split.
- `CLAUDE.md` gains a short cvar bullet: registry in `ArcaneCore`, names not GUIDs, publish barrier, default-deny, ImGui only for the two consoles.
- Do not migrate thumbnail constants. Do not build `arcctl`. Do not bump the plugin ABI.

## What this plan deliberately does not decide again

Source 2's typed union and callback id, Unreal's `SetBy` ladder and history pop, our registry-owned storage, our publish barrier, and default-deny are the spec. Task code that "simplifies" any of those — a stringly value, a raw pointer in the handle, a render-thread shadow, a default-allow console — is a bug, not an implementation detail.

## Hygiene pass 2026-09-28: what the review found owed

Fixed by the pass: `<Keyboard>/grave` resolves (the console toggle was dead);
the editor publishes once per frame (it published once at loop entry); the
console and `SetGpuDrawMarkersEnabled` no longer publish mid-frame (the setter,
caller-less, is gone); the editor Console tab draws the model's replies.

Still owed, in the order they bite:
- Module lifetime (spec 4.4, decision 7): `ARC_CVAR`/`ARC_COMMAND` hard-code
  module "engine"; nothing calls `UnregisterModule` on plugin unload; callbacks
  and commands are raw function pointers, so a rebuilt game module's statics
  are refused as duplicates and stale pointers stay live.
- Game-module cvars never receive config layers: layers apply in `project_open`,
  `--set` in `input_config`, the module's statics run in `plugin_load`, and the
  boot DAG orders none of them; re-apply on module reload too.
- History is append-only (equal-rung repeats push; `OpenProject` re-pushes every
  layer; `CloseProject` never pops), against the one-record-per-source model.
- `Register` does not check `defaultValue.type == type`; cheat revert bypasses
  callbacks; callback dispatch iterates a reference a callback can invalidate;
  config parse failures and unknown keys are silent (the spec promises a
  warning); command success is sniffed from the reply text.
- The spec's "workers read the immutable snapshot" contract has no
  implementation (no double buffer, no atomic index): either state main-thread-
  only or build it.
- Drift: storage is one `Slot` with a `variant`, not type-segregated arrays;
  reads go by name because the `ARC_CVAR` handle is unnameable; the config seam
  re-parses JSON instead of reading `Config::Category`; console focus and
  autocomplete are absent; `PublishImmediate()` does not exist; the plan's
  eight-type round-trip and `GpuDrawMarkersEnabled` tests are missing.
