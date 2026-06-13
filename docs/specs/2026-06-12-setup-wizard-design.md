# Setup Wizard — Design

**Date:** 2026-06-12
**Status:** Approved (brainstorm 2026-06-12)
**Scope:** A graphical first-time-setup wizard (Tauri + Svelte) that drives the
existing setup scripts. Keeps all leaf scripts as the canonical units; adds a
single headless orchestrator + a GUI front-end over it.

## Context

First-time setup today is a sequence of `.bat` scripts (`scripts/doctor.bat`,
`Server/scripts/setup-vcpkg-deps.bat`, `Server/GenerateProjects.bat`,
`Server/scripts/db-setup.bat`, the Arcane equivalents) loosely chained by
`scripts/setup.bat`. Two problems: (1) `setup.bat` is stale — `doctor.bat`
fails on a `Server/Account/migrations/` directory that no longer exists (the DB
workflow is single-file `schema.sql` + `seed.sql`), so setup aborts at step 1;
and (2) the Arcane engine — now the active workspace — was never wired into the
orchestrator. The goal is a low-friction onboarding artifact: **clone the repo
(e.g. via GitKraken) and run one thing; it automates the rest.** A graphical
wizard (doctor checklist with green/red, validated inputs, live log streaming)
is a materially better first-run experience than `cmd` prompts, and doubles as
a polished onboarding surface for future contributors.

The existing scripts are kept and remain the source of truth for *what* each
step does. The wizard only sequences them and forwards inputs.

## Goal

`clone -> double-click Setup.exe -> wizard automates setup`, with zero
toolchain required to *run* the wizard (it ships prebuilt). Host prerequisites
that the wizard cannot silently install (Visual Studio, Docker Desktop) are
detected and the user is guided to install them.

### In scope

- Tauri (Rust core + OS WebView2) + Svelte GUI wizard.
- A single headless orchestrator script that both the GUI and CI call.
- Workspaces: **Server** (vcpkg deps -> generate -> Postgres + schema/seed),
  **Arcane** (SDL3 vcpkg deps -> generate), **Client** (stub step, nothing to
  build yet).
- Parameters: workspace selection, `VCPKG_ROOT`, DB host port, skip-vcpkg,
  build config (for an optional post-generate build), and an optional
  "build after generating" toggle.
- Committed prebuilt `Setup.exe`, kept current by a CI rebuild-and-commit job.
- Fixes folded in: repair `doctor.bat`'s migrations check; add Arcane; drop
  Tools.

### Out of scope

- **Tools editor** — being retired in favor of the engine-native editor
  (Grimoire); removed from the orchestrator entirely, not just the wizard.
- Auto-installing Visual Studio / Docker Desktop (detect-and-guide only).
- A Linux setup path (the leaf scripts are Windows `.bat`; revisit with the
  engine's Linux-port milestone — the Tauri/Svelte shell is cross-platform,
  but what it drives is not yet).
- Client build/setup beyond a placeholder (Love2D runs via `Client/run.bat`;
  no build step exists yet).

## Decisions (brainstorm 2026-06-12)

- **Bundler:** Tauri (not Electron) — ~3-10 MB binary makes a committed
  prebuilt exe practical; OS webview (WebView2 ships on Win 10/11); aligns with
  the eventual Linux direction. Electron's 100 MB+ binary makes the
  commit-in-checkout model impractical.
- **Frontend:** Svelte — lightest for a small few-screen wizard.
- **Distribution:** prebuilt `Setup.exe` committed in the checkout (clone +
  run, no download step). CI rebuilds and commits it when the wizard source
  changes, so it is never stale.
- **Orchestration:** one headless script is the single source of truth; the GUI
  and CI are both thin clients of it. No setup logic is duplicated in Rust.
- **Host prereqs:** detect + guide (green/red doctor rows with install links),
  not auto-install.

## Architecture (three layers)

### 1. Orchestrator script (the brain) — `scripts/setup.ps1` + `scripts/setup.bat` shim

The only place the setup sequence lives. PowerShell (real prompting/validation,
native on Windows). `scripts/setup.bat` becomes a 2-line shim that launches the
`.ps1` with `-ExecutionPolicy Bypass` so the existing entry point keeps working.

Responsibilities:
- Run `doctor` (prereq check), then, per selected workspace:
  - **Server:** `setup-vcpkg-deps` (unless skipped) -> `GenerateProjects` ->
    `db-setup`.
  - **Arcane:** `setup-vcpkg-deps` (unless skipped) -> `GenerateProjects`.
  - **Client:** stub step that prints "nothing to build yet."
- Optionally build after generating (if `-Build`, using the chosen config).
- Print a final summary + next steps.
- Set `_APH_NOPAUSE=1` for every child script (this is what neutralizes the
  `generate.bat` `pause` that hangs non-interactive shells).
- Forward parameters via environment variables the existing scripts/compose
  already read:
  - `VCPKG_ROOT` (scripts read it; default detected or `$env:VCPKG_ROOT`)
  - `POSTGRES_PORT` (compose already reads `${POSTGRES_PORT:-5432}`)
- Honor flags so it runs unattended (CI): `-Workspaces server,arcane`,
  `-VcpkgRoot <path>`, `-DbPort <n>`, `-SkipVcpkg`, `-Config Debug|Release|Dist`,
  `-Build`, `-SkipDoctor`, `-NonInteractive` (take defaults, never prompt).

**GUI/CLI output contract.** The script prints normal human-readable lines
*and* lightweight machine markers on their own lines so a GUI can render a live
checklist while humans/CI see plain logs. Marker grammar (illustrative):

```
@@WIZ step=<id> status=start
@@WIZ step=<id> status=ok
@@WIZ step=<id> status=fail msg="<reason>"
@@WIZ doctor item=<name> status=pass|warn|fail msg="<...>"
```

Step ids: `doctor`, `server-vcpkg`, `server-generate`, `server-db`,
`arcane-vcpkg`, `arcane-generate`, `client`, `build`. The Rust core filters
`@@WIZ ` lines into structured events; everything else is raw log. The markers
are innocuous to a human reading the console.

### 2. Tauri Rust core (the bridge)

Spawns the orchestrator (`powershell -ExecutionPolicy Bypass -File
scripts/setup.ps1 <flags>`), streams stdout/stderr to the webview as Tauri
events, parses `@@WIZ` markers into typed step events, and exposes commands:
`run_doctor()`, `run_setup(params)`, `cancel()`. Holds **no** setup logic — just
process spawn + stream + marker parsing. Uses Tauri's shell/command capability.

### 3. Svelte UI (the face)

- **Doctor screen:** prereq rows (VS, Docker engine, vcpkg + overlay triplet,
  vendored deps, ports) each green/red; red rows show an install link/button.
  Runs on launch. "Continue" enabled when no hard failures (warnings allowed).
- **Options screen:** workspace checkboxes (Server / Arcane / Client, default
  all), `VCPKG_ROOT` text + folder picker (detected default), DB port (default
  5432), skip-vcpkg toggle, build-config dropdown + "build after generating?"
  toggle.
- **Run screen:** live log pane + a step checklist driven by the `@@WIZ`
  markers (spinner -> check/X per step), then a success/fail summary with next
  steps (open `Server/Aphelyon.slnx`, `start-all.bat`, `Client/run.bat`).

## Distribution + CI

- The built `Setup.exe` is committed at the **repo root** (most discoverable
  for clone-and-run).
- A CI job rebuilds it when `tools/setup-wizard/**` changes and commits the new
  `Setup.exe` back, so the checkout is always current. The build agent needs
  Rust + Node (the wizard's build-time toolchain); contributors need neither.
- CI's unattended setup path is the **same** `scripts/setup.ps1` invoked with
  flags — not a separate code path.

## Repo layout

```
Setup.exe                              committed prebuilt wizard (CI-maintained)
tools/setup-wizard/                    wizard SOURCE (lowercase; distinct from C++ Tools/)
  src/                                 Svelte UI
  src-tauri/                           Rust core (spawn + stream + marker parse)
  package.json, ...
scripts/setup.ps1                      headless orchestrator (single source of truth)
scripts/setup.bat                      2-line shim -> setup.ps1
scripts/doctor.bat                     prereq check (migrations check fixed)
```

## Folded-in fixes

1. `scripts/doctor.bat`: replace the `Server\Account\migrations\` FAIL check
   (directory does not exist) with a `Server\Account\schema.sql` +
   `seed.sql` presence check, matching what `db-setup.bat` actually applies.
2. Orchestrator: add the Arcane workspace steps; remove the Tools step.

## Open planning-time items (settle in the plan, not blockers)

- **Which CI builds + commits `Setup.exe`.** Reusing Jenkins/StarworksBuilder
  (the bot already has push rights) is consistent with existing infra, but the
  `windows-1` agent may lack Rust + Node; a GitHub Action is simpler for
  "build a Windows binary + commit back" but adds a second CI surface. Decide
  during planning, including the commit-loop guard (don't trigger the job on
  its own binary commit).
- **Doctor reuse:** whether the GUI doctor calls `doctor.bat` (parsing its
  output) or the orchestrator exposes a `-Doctor`/`run_doctor` mode that emits
  `@@WIZ doctor` markers directly. Prefer the latter for clean structured rows.
