# Arcane Hub: project launcher + the no-project gate — design

Status: **DRAFT, awaiting review.** Nothing is built. Written 2026-07-26 from a
brainstorming session; the user was heading out, so the design was written in one
pass for async review rather than section-by-section approval.

## Problem

`ArcaneEditor.exe` opens fine with no project. Without `--project` it warns and
falls back to a "legacy `data/`-next-to-exe" boot, giving a half-configured
editor with no asset registry, no mounts, and no identity — a state nothing
downstream expects. There is also no way to discover or create a project: you
type a path on the command line or use `File -> Open Project`, which presumes you
already know where it is.

## Goals

1. The editor never runs a project-less session by accident.
2. A first-run user can find, create, and open a project without a terminal.
3. The launcher is the seed of a real hub (engine versions, installs, templates),
   not a dead-end dialog.

## Non-goals (v1)

- Engine version management, installs, downloads, accounts. **The architecture
  must not preclude them** — see "Growth path" — but v1 ships none of them.
- Replacing `File -> Open Project` inside the editor. It stays.
- Any change to `.arcproj` schema, mounts, or the asset registry.

## Decision: a separate Tauri 2 + Rust app (`ArcaneHub`)

**This reverses the recommendation made during brainstorming**, and the reversal
is worth recording. The initial recommendation was an in-process ImGui screen,
because `EditorApp::SwitchProject` (EditorApp.cpp:583) already performs a
validated-first in-process soft restart — it probes the project, refuses to tear
down a live session for a bad pick, and swaps at a safe point at the top of the
frame. `File -> Open Project` already drives it. By that reasoning the launcher
is a *screen*, not an application, and a second process buys nothing.

That reasoning holds only for a project picker. The user's answer was **"a real
hub, eventually"** — engine-version management, installs, a templates gallery,
possibly accounts. That is a product surface with its own lifecycle, and it
inverts the key constraint:

- A hub that manages *engine versions* cannot live inside one engine build's
  `bin/`. It has to outlive and sit above any single `ArcaneEditor.exe`.
- Version/install/download/account UI is exactly the kind of chrome ImGui is
  worst at and a web stack is best at.
- Building the ImGui picker first would be knowingly disposable work.

The precedent already exists in-repo: `Setup.exe` is Tauri 2 + SvelteKit at
`Tools/setup-wizard/`, a raw portable binary run in place at the repo root and
CI-maintained by `.github/workflows/build-setup-wizard.yml`. `ArcaneHub` is its
sibling and should reuse that packaging and CI shape wholesale.

**Recorded so it is not re-litigated:** if the hub ambition is ever dropped back
to "just a picker", the in-process ImGui screen becomes the right answer again,
and `SwitchProject` means it is a small change.

## Architecture

Three processes, no IPC:

```
ArcaneHub.exe  (Tauri 2 + Rust + SvelteKit)
     |  reads/writes  %APPDATA%/Arcane/hub/*.json
     |  probes        ArcaneEditor.exe --print-engine-info   (stdout JSON)
     |  spawns        ArcaneEditor.exe --project <path>
     v
ArcaneEditor.exe  (unchanged hosting path)
```

Handoff is **process spawn with an existing flag**, not IPC. `--project` already
exists and is already the CI/headless entry point; the Hub is simply another
caller. This is the cheapest possible coupling and it keeps the editor testable
without the Hub.

The Hub **stays running** after spawning an editor (Unity Hub behaviour).
Multiple editors may run at once; each is an independent process. Closing the Hub
does not close editors.

## The editor-side gate

`ArcaneEditor.exe` boot becomes:

| Invocation | Behaviour |
|---|---|
| `--project <path>` | Unchanged. Opens it. **CI, the `--frames N` headless harness, and desk repro all keep working.** |
| `--plugin <dll>` (no project) | Unchanged. The expert/engine-dev path (hosting Sandbox.dll without a project). |
| neither | **New:** does not boot a project-less session. Exits with a clear message naming the Hub, and — if `ArcaneHub.exe` is found next to it — offers to launch it. |

The `data/`-next-to-exe fallback is **removed** from the no-flags path. It stays
reachable only via explicit `--plugin`, where it is a deliberate choice rather
than a silent degradation.

Keeping `--project` and `--plugin` as bypasses is deliberate and load-bearing:
the headless `ArcaneEditor.exe --project <path> --frames N` harness is what found
the Add Component roster bug on 2026-07-26, and hard-gating would have cost us
that tool.

`File -> Close Project` is **not** added in v1. With the launcher out-of-process
there is nothing for the editor to return *to*, and a project-less editor is
precisely the state this design removes. Revisit only if the Hub ever becomes
in-process.

## Engine discovery and the ABI probe

This is the sharpest correctness issue in the design.

A `.arcproj` manifest **requires** `engine.abi` (ProjectManifest.hpp: required
fields are `formatVersion` > 0, `name`, `engine.abi`). If the Hub hardcodes an
ABI number, `New Project` will silently mint projects with a stale ABI the moment
the engine bumps — reproducing exactly the failure recorded in
`project_arcane_shader_editor_arc`, where a stale plugin ABI loaded and crashed
on project open.

**The Hub must never hardcode the ABI.** New flag:

```
ArcaneEditor.exe --print-engine-info
```

writes a single JSON object to stdout and exits 0 without creating a window,
device, or registry:

```json
{ "engineAbi": 7, "version": "...", "buildConfig": "Debug", "exePath": "..." }
```

The Hub probes this before creating a project and stamps the returned `engineAbi`
into the manifest. When multiple engine installs arrive, the same probe is how
the Hub enumerates and validates them — so this flag is the seam that makes
version management possible later, not throwaway plumbing.

Engine location in v1: a single configured path, stored in
`%APPDATA%/Arcane/hub/engines.json` as a **list with one entry**, defaulting to
an `ArcaneEditor.exe` found next to `ArcaneHub.exe` or at the known repo-relative
`bin/` path. It is a list from day one specifically so v2 does not migrate a
scalar.

## User-scope state

New directory `%APPDATA%/Arcane/hub/` (per-user, survives rebuilds and clean
`bin/` wipes, and is shareable across two checkouts — the reasons the alternative
"next to the exe" was rejected):

- `recents.json` — ordered list of `{ path, name, lastOpenedUtc, engineAbi }`.
- `engines.json` — the engine list described above.

No existing user-scope config seam exists (`Arcane/Config/Config.hpp` is
project-layered), so this is new. It stays **Hub-owned**: the engine does not
read it. Keeping user-scope state out of the engine preserves the rule that Core
and the runtime carry no host/user vocabulary.

Recents entries whose path no longer resolves render greyed with *Locate…* /
*Remove*, and are never silently dropped — a missing project is usually a moved
folder, not an abandoned one.

## New Project (v1)

Scaffolds a **content-only project**: no game DLL, no premake, no SDK recipe.
This is legitimate and already supported — `ProjectManifest` documents
`gameModule` as optional, and EditorApp already handles the plugins-only case
("an empty gameModule makes a plugins-only host (open a plugin-only project to
work on it before its game DLL exists)", EditorApp.cpp:239-241).

Creating `MyGame` at a chosen directory writes:

```
MyGame/
  MyGame.arcproj      { formatVersion, name, description, engine: { abi: <probed> } }
  Content/            (empty; the editor's asset registry scans it)
```

then adds it to `recents.json` and spawns the editor on it.

v1 ships exactly **one template, "Empty 2D"**, defined by the Hub itself rather
than a templates directory. A real template *gallery* — engine-relative
`templates/` so templates version with the engine — is deferred to the hub phase.
Shipping a directory-scanning template system for a single built-in template
would be structure without payload.

## Hub UI (v1)

Two screens.

**Projects** (default): a list of recents (name, path, last opened, engine ABI),
a prominent *Open…* (native folder dialog) and *New Project…*, per-row
*Open / Locate / Remove from list*. Empty state explains what a project is and
offers *New Project…*.

**Settings**: engine path (with the probe result shown, so a bad path is visible
immediately) and the recents list location.

The visual design should follow `Tools/setup-wizard/`'s existing look so the two
first-run surfaces feel like one product.

## Growth path (explicitly not built)

The v1 shapes chosen to make these additive rather than migrations:
`engines.json` is a list; `--print-engine-info` is the probe that enumerates and
validates installs; the Hub owns its own user-scope state; handoff is
process-spawn, which already works for N engine versions.

## Testing

- **Rust/Hub unit tests** (`cargo test`, mirroring the setup-wizard's test
  layout): recents add/dedupe/ordering/missing-path classification; manifest
  generation; engine-probe JSON parsing including a malformed/missing-field
  response.
- **Engine-side, in ArcaneTests:** `--print-engine-info` emits parseable JSON
  whose `engineAbi` equals `kGamePluginABIVersion` — a tripwire that fails if the
  ABI bumps without the probe following it. This is the single most valuable test
  in the design, because it is what stops silently-stale generated manifests.
- **Gate behaviour:** launching with no flags exits non-zero with the Hub message
  and creates no window.
- The Hub's UI itself is desk-verified, consistent with every other UI surface in
  this repo.

## Slices

1. **Engine seam** — `--print-engine-info` + the no-flags gate + removing the
   silent `data/` fallback. Pure C++, ships alone, testable in ArcaneTests, and
   immediately delivers the thing that was actually asked for ("no editor without
   a project").
2. **Hub shell** — Tauri app scaffold + packaging + CI, Projects screen over
   `recents.json`, Open… and spawn.
3. **New Project** — probe-driven manifest generation + scaffold + open.

Slice 1 is independently valuable: it closes the hole even if the Hub slips.

## Open questions for review

1. **Where does `ArcaneHub.exe` live and ship?** `Setup.exe` is a raw portable
   binary at the repo root. Same treatment, or does the Hub belong in an
   installed location once engine installs are real?
2. **Does the Hub replace `Setup.exe`'s role over time**, or stay strictly
   separate? They overlap conceptually (both are first-run surfaces) and it would
   be worth deciding before both grow.
3. **Should slice 1's gate exit non-zero, or pop a native message box?** Exiting
   non-zero is script-friendly; a message box is friendlier to a double-click
   user who has no console.
