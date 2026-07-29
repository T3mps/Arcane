# Arcane Runtime Host Fold — Design

**Date:** 2026-07-29
**Status:** Approved direction; spec for review
**Supersedes:** the "Loom" framing throughout CLAUDE.md and the workspace

## Goal

Retire Loom. The engine owns the runtime-host role: the shared host-boot layer
moves into the Arcane DLL, the Loom exe becomes `ArcaneRuntime` (the thin
standalone host that opens a project and runs its game), and the editor grows
a play-mode dropdown whose *Separate window* mode launches the game through
that runtime while the viewport stays in edit mode. Server launching is a
designed seam, not a feature of this arc.

Standing directive (2026-07-23): Loom was the host for Arcane-as-a-library;
Arcane is a full engine now, so the host belongs inside it — the same
fold-into-Arcane pattern applied to the servers.

## Decisions already made (user calls, recorded)

- The name **Loom is retired** everywhere except git history.
- Separate `ArcaneRuntime.exe`, **not** an editor `--headless-host` mode.
  (UE precedent, verified in vendored source: mode flags on the editor binary
  exist to run *uncooked* content standalone — a problem we don't have, since
  the runtime opens the same `.arcproj`. Unity ships a separate Player
  outright. Our engine-as-DLL makes the separate exe nearly free, and the
  editor's contracts — project lock, rival refusal, exit-code gate, Hub probe
  — must NOT apply to the runtime.)
- Separate-window play boots **the active editor scene, as saved** (option b):
  dirty scene prompts save-or-cancel; never a temp snapshot of unsaved state.
  Undo is unaffected by construction — the editor process only saves and
  spawns.
- **No modifier-click.** The play button plays whatever mode the dropdown
  last selected; the dropdown is the only mode switch.
- Standalone play is **fire-and-forget**: the editor does not track the
  spawned runtime; the game window closes like any window. Child tracking is
  earned later by server-set teardown, not before.

## Part A — the fold (ownership inversion)

Today the editor compiles Loom's sources directly (`GpuContext.cpp`,
`LoomConfig.cpp` in the ArcaneEditor project; `LoomConfig.cpp` again in
ArcaneTests for the `[loom]` tests) and Loom.exe links Core statically so
`LoomConfig.cpp` can reach the un-exported `Arcane::Cli`. That is the
inverted ownership this part fixes.

**Moves into the engine DLL** as `Arcane/Arcane/src/Arcane/Host/`,
`namespace Arcane`, `ARCANE_API`-exported:

| Today (Loom/src) | Becomes |
|---|---|
| `LoomConfig.hpp/.cpp` | `Arcane/Host/HostConfig.hpp/.cpp` (type `HostConfig`, `HostConfig::Parse`) |
| `GpuContext.hpp/.cpp` | `Arcane/Host/GpuContext.hpp/.cpp` |
| `FramePerf.hpp` | `Arcane/Host/FramePerf.hpp` |
| `ProjectBoot.hpp` (`HostBoot::*`) | `Arcane/Host/ProjectBoot.hpp` |

Consequences:
- `Arcane::Cli` usage becomes DLL-internal — no consumer needs a static Core
  copy for config parsing anymore.
- ArcaneEditor deletes the two source-share entries and the Loom includedir;
  it includes `<Arcane/Host/...>` like any engine module.
- ArcaneTests deletes its `LoomConfig.cpp` compile; the `[loom]` tag becomes
  `[host]` and the tests call the exported `HostConfig::Parse`.
- `IMGUI_API=dllimport` handling follows the existing editor/tests pattern;
  GpuContext's ImGui touchpoints compile inside the DLL where ImGui already
  builds.

**Stays exe-side, renamed:** `Loom.hpp/.cpp` (the host app class — Init,
frame loop, plugin hosting) and `main.cpp` move to
`Arcane/ArcaneRuntime/src/` as `RuntimeApp`; project `ArcaneRuntime`,
`kind ConsoleApp`, output `bin/<cfg>/ArcaneRuntime/ArcaneRuntime.exe`.
EditorApp keeps its own frame loop; unifying the two loops is explicitly out
of scope (noted as possible later convergence).

**Keeps working unchanged:**
- `--plugin` hosting: Sandbox.dll showcase (still the default hosted plugin),
  PlaygroundGame.dll, the HotReloadPluginV1/V2/Bad fixtures, F5/F6 reload.
- The postbuild set (Arcane.dll, shaders, EngineConfig, SampleProject, dxc
  trio, plugin DLL copies) — retargeted to the ArcaneRuntime output dir.
- CI's scripted GPU-verify: `Loom --frames N` → `ArcaneRuntime --frames N`,
  same flags, same exit-code contract. All Jenkinsfile / script / docs
  references rename in the same change.

Part A is behavior-preserving: same boot, same flags, new names and owners.
It lands and gates alone before Part C touches the editor.

## Part B — the runtime's project mode

`ArcaneRuntime --project <root-or-.arcproj>` exists today (slice-1b). Added:

- `--scene <guid>` — overrides the manifest's `bootScene` for this run.
  Resolved through the AssetRegistry after `OpenProject`; an unresolvable
  guid is a config error (the existing bad-argument exit path, message
  naming the guid). Absent flag = manifest `bootScene`, exactly today's
  behavior.
- **Contract, stated in code comments:** the runtime takes NO editor lock,
  refuses no rivals, and answers no engine probe. Running the game while the
  editor holds the project is the normal loop. `--print-engine-info` remains
  editor-only.

## Part C — editor play modes

The toolbar play control becomes a split button:

- **Play (main button):** runs the current default mode.
- **Dropdown:** picks the default. Entries are *process sets*, initially:
  1. `In viewport` — play-in-editor, exactly today's path, untouched.
  2. `Separate window` — the new mode.
  The selected default persists with the editor's other UI state (the same
  imgui.ini-backed mechanism the splitter ratios use).

**Separate window flow:**
1. Active scene dirty? Modal prompt: *Save and Play* / *Cancel*. No
   play-with-unsaved, no snapshot.
2. Resolve the runtime exe: try the editor exe's own directory first (the
   packaged layout), then `../ArcaneRuntime/ArcaneRuntime.exe` (the dev bin
   layout) — a pure candidate-list helper, unit-tested, same pattern as the
   Hub's `suggest_engine`.
3. Spawn detached: `ArcaneRuntime --project <root> --scene <active scene
   guid>` plus the editor's current backend flag; working directory = the
   runtime exe's directory (shader resolution rule). No pipe, no wait, no
   tracking. Spawn failure surfaces in the editor's existing error/notice
   path with the attempted exe path.
4. Editor stays fully interactive in edit mode throughout.

**Not in this arc:** stop button for standalone, PIE changes, Hub "Run"
button, packaging.

## Part D — the server seam (shape only)

The dropdown entries and the spawn path are list-shaped from day one: an
entry describes a set of processes to spawn (the game now; game + N servers
later). Server entries, when servers fold into Arcane, spawn with
`CREATE_NEW_CONSOLE` into their own cmd windows. Set teardown (and with it,
child tracking) is designed when the second entry type exists. Nothing else
is built now.

## Testing

- **Part A:** the full existing gate proves behavior preservation (the
  editor boots through the moved layer; ArcaneTests' `[host]` config tests
  exercise the exported Parse). CI's renamed `ArcaneRuntime --frames N`
  GPU-verify proves the runtime end to end.
- **Part B:** `[host]` unit tests for `--scene` parse; a runtime-side
  resolution test (bad guid → config-error exit) at the level the existing
  flag tests live.
- **Part C:** pure unit tests for the exe-candidate resolution and the
  spawn argv construction (scene guid, backend flag, no lock side effects to
  assert by design). Desk: dirty-scene prompt; game window opens on the
  active scene while the viewport stays editable; Sandbox showcase and F5/F6
  still work via ArcaneRuntime.
- Consolidated desk item: one pass over CLAUDE.md's Loom references
  (rewritten in the same change, not left stale).

## Phasing

1. **Slice 1 — the fold** (Part A): pure move/rename, behavior-preserving,
   gated alone. CI/scripts/docs rename with it.
2. **Slice 2 — runtime scene override** (Part B): small, gated by `[host]`
   tests.
3. **Slice 3 — play modes** (Part C): editor UX riding the clean layer.
4. Part D exists only as the shape of Slice 3's data structures.

## Non-goals

Server launching, standalone-play tracking/stop, Hub Run button, sharing the
frame loop between RuntimeApp and EditorApp, packaging/distribution, any
change to play-in-editor.
