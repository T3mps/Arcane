# arcbuild — the game-project build driver

**Status:** implemented 2026-09-13 (plan
`docs/plans/2026-09-13-arcbuild-driver-plan.md`; plan-time rulings R1–R10
there, with the measured one-compile-one-link proof in its Closeout). Follows
the editor<->IDE surface arc (Source/ in the browser, Open Visual Studio, the
C++ Class wizard).

## 1. Why

The "build a game module against this SDK" logic exists in three places and
is drifting:

| Copy | Where | What it knows |
|---|---|---|
| Editor | `ArcaneEditor/src/Project/ModuleBuild.{hpp,cpp}` | premake-first, `/t:Rebuild` always, SDK = the running editor's root, vswhere |
| Scripts | `Game/premake5.lua` header comment, `scripts/setup.ps1` (Gacha), `scripts/generate.bat` | premake + msbuild by hand, `ARCANE_SDK` from the environment |
| CI | `Jenkinsfile` (Game stages, gated on the agent's SDK), `scripts/golden-gate.ps1` (rebuild ReferenceProject first) | the same commands again, plus the config-ordering ritual |

Unreal's answer is `Build.bat`: one entry point the editor, the IDE's NMake
shim, CI and the class wizard all call. `arcbuild` is that entry point for
Arcane game projects. It **drives** premake and msbuild; it never owns
compilation (see the 2026-09-13 "own build system?" ruling: premake stays the
generator, a homegrown UBT is not worth it, CMake only if Linux+WASM together
demand it).

The first feature it must carry is the one that makes the class wizard feel
instant: **the incremental rule** (§4.3). Today every Rebuild Game Module is a
full `/t:Rebuild` because `Binaries/` is a single slot shared by every
configuration; an incremental build could report "up to date" over the wrong
config's DLL. The driver probes the slot and forces the rebuild only when it
must, so a wizard-made component costs one TU compile and a link.

## 2. Goals / non-goals

**Goals**
- One executable, `arcbuild.exe`, staged beside `ArcaneEditor.exe` /
  `arccook.exe` (premake project like `arccook`, ConsoleApp, links
  ArcaneClient for `Module::ScanFileCrtFlavor` — never a second PE scanner).
- Commands: `generate`, `build`, `rebuild`, `clean` for a game project
  (`--project <dir | .arcproj>`), `--config Debug|Release|Dist`, `--sdk <root>`
  (else `ARCANE_SDK`, else refuse), `--action vs2026` (the premake action;
  default vs2026, the seam for gmake2/ninja on Linux).
- The editor's `ModuleBuild` becomes a thin spawn-and-stream of `arcbuild`
  (its Runner + Console drain stay; its composition moves into the driver).
- The scripts and the Jenkinsfile's Game stages call the same exe.
- The pure core is testable without spawning: composition, the probe rule,
  the SDK walk, exit-code mapping.

**Non-goals (v1)**
- The engine workspace (`Arcane.slnx`, the ReferenceProject-before-Arcane
  ordering, staging post-builds, golden-gate's rebuild step). Deferred to the
  Linux/CI milestone, where a second toolchain makes it earn its keep. The CLI
  is shaped for it now (§6): `--engine <root>` is a second target kind, not a
  redesign.
- Live Coding / patching a running module. Hot reload (PluginHost's mtime
  watch + Save/LoadState) stays the reload mechanism.
- Auto-build after Create in the editor. Opt-in setting for the future Tools
  → Settings pane (user ruling 2026-09-13); not wired until that pane exists.
- MSVC diagnostic parsing into per-line locators (ModuleBuild's standing
  non-goal). The driver passes msbuild's lines through verbatim.

## 3. CLI

```
arcbuild <command> --project <dir|.arcproj> [--config Debug|Release|Dist]
                   [--sdk <root>] [--action vs2026] [--force-rebuild] [--quiet]
commands:
  generate   premake <action> in the project root (writes <Name>.slnx + .vcxproj)
  build      generate, then msbuild the solution; /t:Rebuild only when §4.3 says so
  rebuild    generate, then msbuild /t:Rebuild unconditionally
  clean      msbuild /t:Clean, then remove Binaries/ and Intermediate/<config>/
  probe      print the slot verdict of §4.3 and exit (diagnostics; used by tests)
             (exit 0 = the plain-build rows, 3 = the would-rebuild rows -- plan ruling R4)
             `--force-rebuild` is a refusal (exit 2): probe is the slot row,
             not a dry-run of `build`
```

- `--project` accepts the directory or the `.arcproj`; the manifest's `name`
  names the solution (`<Name>.slnx`) exactly as `EditorApp::StartModuleRebuild`
  assumes today; a solution discovered on disk (`DiscoverSolution`'s rule:
  first `*.slnx`, else first `*.sln`, lexicographic) wins over the convention.
- `--config` defaults to `Debug`. The editor passes its own build flavor
  (`ModuleBuild::Configuration()`), CI passes each of its two.
- `--sdk` overrides `ARCANE_SDK` for the child premake (the driver sets the
  variable in its own environment before spawning, exactly `SetSdkEnv`); the
  editor always passes the running editor's root ("rebuild against the engine
  you are looking at"). An explicit `--sdk ""` is a refusal (exit 2) -- it
  does not fall through to `ARCANE_SDK`.
- `--action` is a premake generator token (`vs2026` default; `gmake2`/`ninja`
  are the documented Linux seam, not yet a spawn path). It must be a non-empty
  `[A-Za-z0-9_-]+` identifier; anything else is a refusal so the token cannot
  become a `cmd.exe` fragment. It is not an enum until a non-msbuild build
  step exists.
- Output: every child line to stdout, prefixed `[premake]` / `[msbuild]`; the
  driver's own lines `[arcbuild]`. Exit code = the first failing child's, or
  `2` for a driver refusal (no SDK, no project, empty `--sdk`, bad `--action`,
  `--force-rebuild` on probe), `0` on success.
  The editor keeps colouring lines by `": error"` / `": warning"` as today.

## 4. Behaviour

### 4.1 Resolution
- premake: `<sdk>/ThirdParty/premake5/premake5.exe`, else `premake5` on PATH
  (`ModuleBuild::ResolvePremake`, moved).
- msbuild: `vswhere -latest -requires Microsoft.Component.MSBuild -find
  MSBuild\**\Bin\MSBuild.exe`, else `msbuild` on PATH (`ModuleBuild::VsWhere`
  + `ResolveMsBuild`, moved).
- Tools are resolved per command: `probe` needs neither; `generate` needs
  premake; `build`/`rebuild` need both; `clean` needs msbuild only when a
  workspace file exists. `ResolveMsBuild` never fails (PATH fallback, ruling
  R8) -- skipping it on `probe` is so a nothing-stale check does not spawn
  vswhere.
- The SDK root is never inferred from the driver's own exe location in v1
  (the editor knows its root and passes `--sdk`; scripts/CI have the
  variable). `SdkRootFromExeDir` stays in the editor.
- (Plan ruling R1: the probes above, plus `DiscoverSolution` and a
  `ResolveDevenv`, live in ArcaneCore as `Arcane::Toolchain` -- shared by
  arcbuild.exe and the editor's IdeLaunch, which still needs devenv and the
  solution path after ModuleBuild lost them.)

### 4.2 generate
`( cd /d "<root>" && "<premake>" <action> ) 2>&1` — `ComposeGenerateCommand`,
moved verbatim. Always runs before `build`/`rebuild` (the stale-.sln decision
stands).

### 4.3 The incremental rule (the reason this exists now)
Before `build`, probe `<root>/Binaries/<gameModule>` (the manifest's
`gameModule`):

| Slot state | Decision | Why |
|---|---|---|
| absent | plain build | nothing to be wrong about |
| present, CRT flavor matches `--config` (Debug ⇔ `ucrtbased`) | plain build | msbuild's incremental view is trustworthy for this config |
| present, CRT flavor mismatches | `/t:Rebuild` | the single-slot hazard: a plain build would report "up to date" and leave the other config's DLL in place |
| present, flavor unreadable | `/t:Rebuild` | unknown ⇒ the safe choice; say so in the log |

`Module::ScanFileCrtFlavor` (ArcaneClient) is the probe — the same verdict
PluginHost uses to refuse a cross-CRT module, so the driver and the host can
never disagree about what "matches" means. `--force-rebuild` and the `rebuild`
command bypass the probe on `build`. `probe` prints the slot row it landed on
and exits from that row (R4); `--force-rebuild` on `probe` is a refusal so
the printed `-> /t:Rebuild` cannot disagree with exit 0 on a matching slot.

Dist maps to Release for the probe (both are release-CRT), matching
`ModuleBuild::Configuration()`'s Dist caveat.

### 4.4 clean
`msbuild <sln> /t:Clean /p:Configuration=<cfg>`, then delete `Binaries/`
(whole slot — it is one slot) and `Intermediate/<cfg>/`. The filesystem
deletes still run if `/t:Clean` failed -- a broken generated Clean target
must not leave the slot behind. `remove_all` errors are reported; if msbuild
already succeeded, a delete failure becomes a driver refusal (exit 2). Never
touches `Source/`, `Content/`, `Saved/`, or the `.slnx` (generate rewrites
that).

## 5. Consumers, in order of adoption

1. **Editor (`ModuleBuild`)** — `ComposeRebuildCommands` becomes "the arcbuild
   command line"; the Runner spawns `arcbuild build --project <root> --config
   <cfg> --sdk <sdkRoot>` (a future "Force Rebuild" menu item would pass
   `rebuild`) and drains lines exactly as now. `RegenerateSolution` (OpenInIde, the
   class wizard) becomes `arcbuild generate`. The vswhere/premake resolution
   leaves the editor. `ModuleBuildTest`'s composition tests move to the
   driver's core tests; the editor keeps a test that the spawned line names
   arcbuild + the three flags.
2. **Gacha** — `Game/premake5.lua`'s header comment and `scripts/setup.ps1`'s
   Game step call `arcbuild generate` / `build`.
3. **Jenkinsfile** — the Game stages (when the agent carries an SDK) run
   `arcbuild build --project Game --config <each>` and `arcbuild probe` as a
   nothing-stale check, beside `arccook --check`.
4. **Hub** — later; the Hub's "Build" affordance, if it grows one, spawns the
   same exe.

The editor resolves `arcbuild.exe` beside its own exe (packaged layout) then
`../arcbuild/` (dev bin layout) — the `RuntimeLaunch::ExeCandidates` rule.

## 6. Extensibility (not built in v1)
- `--engine <root>` as a second target kind: the same commands over
  `Arcane.slnx` with the ReferenceProject-first ordering and the staging
  post-build awareness golden-gate.ps1 carries today. Adding it must not
  change the game-project CLI. The source split is the seam:
  `Request.hpp` grows the flag, `Compose.hpp`'s spawn lines are reused,
  `Slot.hpp` (the game-module CRT table) is not, and a new engine-layout
  unit owns ReferenceProject-first / staging. `main.cpp` dispatches.
- `--action gmake2|ninja` with a non-msbuild build step is the Linux seam;
  §4.3's probe generalises to "the slot's flavor" per platform.

## 7. Testing
- Core (compiled into ArcaneTests, `[build]` tag): command composition for
  all four commands; the §4.3 decision table as a pure function over
  `(slotExists, slotFlavor, config, forceRebuild)`; solution discovery vs.
  the naming convention; exit-code mapping; the `--sdk`/`ARCANE_SDK`/refuse
  precedence.
- Desk (opt-in, `[build-desk]`, SKIPs unless `ARCANE_BUILD_DESK=<project>`):
  `arcbuild probe` and `arcbuild generate` against a real project — the same
  shape as `[ide-desk]`.
- Editor: the spawned command line is pinned; the Runner/stream path is the
  existing desk-verified one.
- Product: Rebuild Game Module on Aphelyon after a wizard-made component
  must show ONE compile + link in the Console, not a full recompile; the
  golden gate stays 4/4 both configs (nothing renders differently).

## 8. Rollout
1. Driver core + exe + tests, staged beside the editor (premake project,
   post-build copy like arccook).
2. Editor shim (ModuleBuild spawns it) — desk-verify Rebuild + Open Visual
   Studio's generate path + the wizard.
3. Gacha scripts + Jenkinsfile Game stages.
4. Memory/CLAUDE.md: the "Build" section points at `arcbuild`.
