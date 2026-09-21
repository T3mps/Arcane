# arcbuild — the game-project build driver

**Status:** implemented 2026-09-13 (plan
`docs/plans/2026-09-13-arcbuild-driver-plan.md`; plan-time rulings R1–R10
there, with the measured one-compile-one-link proof in its Closeout). Follows
the editor<->IDE surface arc (Source/ in the browser, Open Visual Studio, the
C++ Class wizard).

**Amended 2026-09-20** by the multibackend hardening plan
(`docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md`,
**Implemented**): this document's Windows/MSBuild-only CLI resolution (§4.1),
shell-composed `generate` command (§4.2), and "not yet a spawn path" note on
`gmake2`/`ninja` (§6) are superseded. `generate`/`build`/`rebuild`/`clean` now
drive Make, Ninja, and (macOS-only) Xcode through structured, direct child
processes — no shell, on any platform — exactly as MSBuild always has. The
sections below are updated in place; §4.3's incremental rule, §4.4's clean
guarantees, and the CLI surface in §3 (commands, flags, exit codes) are
unchanged and still binding. See the hardening design for the full
correctness/robustness split, the process-execution contracts (Windows
`CreateProcessW`, POSIX `fork`/`execv`), and per-backend test evidence.

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
                   [--sdk <root>] [--action <platform-default>] [--force-rebuild] [--quiet]
commands:
  generate   premake <action> in the project root (writes the generated project files
             for that backend -- .slnx/.vcxproj, Makefile, build.ninja, or .xcodeproj)
  build      generate, then run the resolved backend's build; a full rebuild only
             when §4.3 says so (MSBuild `/t:Rebuild`; Make/Ninja `clean` then `build`
             as two process steps; Xcode `clean build` in one invocation)
  rebuild    generate, then run the resolved backend's full rebuild unconditionally
  clean      run the resolved backend's clean (soft-skipped, logged, if no backend/
             generated context resolves), then ALWAYS remove Binaries/ and
             Intermediate/<config>/ -- see §4.4's precedence rule
  probe      print the slot verdict of §4.3 and exit (diagnostics; used by tests)
             (exit 0 = the plain-build rows, 3 = the would-rebuild rows -- plan ruling R4)
             `--force-rebuild` is a refusal (exit 2): probe is the slot row,
             not a dry-run of `build`; `probe` alone needs no SDK (§4.1)
```

**Supported backends** (multibackend hardening, 2026-09-20; full contracts in
`docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md` §4.3/§8):

| Premake action | Backend | Tool prerequisite | Live-validated (2026-09-21, `scripts/verify-arcbuild-backends.ps1` Debug + Release) |
|---|---|---|---|
| `vs2026`, `vs2022` | MSBuild | Visual Studio / `msbuild` on PATH (`vswhere`) | Windows: full build/rebuild/clean/probe lifecycle against a disposable ReferenceProject copy |
| `gmake`, `gmakelegacy` | Make | `mingw32-make`/`make` on PATH **and** a compiler the generated Makefile's toolset needs (beta8's `gmake` action defaults to GCC/G++ on every host, including Windows — a MinGW-w64 toolchain, never `cl.exe`) | Windows (MinGW-w64 GCC 16.1.0 / GNU Make 4.4.1): mechanics — generate, build, rebuild, clean, real child exit codes propagated unchanged, all four clean-precedence cases — **and the module now compiles** (GCC accepts the whole engine header closure the fixture pulls in); the **link fails**: the GCC object references Itanium-mangled `__imp_` symbols (`ImGui::SetAllocatorFunctions`, `Arcane::Runtime::Components`, `Arcane::Log::Engine`, …) that the MSVC-built `ArcaneCore`/`ArcaneClient` import libraries do not export. A MinGW-built module cannot link an MSVC-built engine; a Make-built `Fixture.dll` needs a GCC-built engine — i.e. the Linux port. Not an arcbuild or header defect. Linux: **not yet** — `verify-arcbuild-posix.sh` stage 2 has never run (no engine Linux port) |
| `ninja` | Ninja | `ninja` on PATH **and**, on Windows, a Visual Studio developer environment (`cl.exe`/`link.exe` — beta8's `ninja` action defaults to the MSVC toolset on a native Windows target) | Windows: **full build** — generate, build, rebuild and clean each produce/remove `Binaries\Fixture.dll` (Debug and Release; the `probe`-able single slot is the arcbuild-staged copy, see the Ninja note below). Linux: **not yet** — same stage-2 gap as Make |
| `xcode4` | xcodebuild | macOS only; `/usr/bin/xcodebuild` or `xcodebuild` on PATH | Nowhere live — resolution/context/argument composition are unit-tested on every platform against a real `--os=macosx xcode4` fixture, but **live `xcodebuild` execution needs macOS**, which no desk in this arc had; a documented live-validation limit |

What "Linux: not yet" covers: the POSIX process runner's own contract is
proven (its `fork`/`execv`/pipe branch compiles under GCC with
`-Wall -Wextra -Werror` — `verify-arcbuild-posix.sh --syntax-only`, stage 1 —
and its runtime behaviour was exercised through a throwaway harness on WSL2,
Task 6), but no Linux host has ever generated a project, built a module, or
run `ArcaneTests '[build]'`, because the engine itself has no Linux port yet.
Stage 2 of that script is the gate that flips this row.

**Verified Xcode target convention:** beta8's `xcode4` action emits a
`.xcworkspace` + `.xcodeproj` but **no shared scheme** — confirmed by
generating the committed `ArcaneTests/data/arcbuild-fixture/` fixture with
`premake5 --os=macosx xcode4` and inspecting the output. `ComposeXcodeBuild`
therefore drives `xcodebuild -project <name>.xcodeproj -target <module-stem>
-configuration <Config> build|clean|clean build`, never `-scheme` — a scheme
argument would name something beta8 never generates. Make and Ninja have the
same "characterize the real generator output first" provenance: Ninja's
target is `<module-stem>_<Config>` (`Fixture.ninja`'s own per-configuration
aggregate; beta8's naming, not an arcbuild convention), and Make's is `-C
<root> config=<lowercased Config>` against the generated `Makefile`.

**Ninja and the single slot (2026-09-21):** beta8's `ninja` action refuses
three configurations that link to one output ("multiple rules generate
`Binaries/X.dll`"), so `build/arcane.lua` links each configuration to its own
`Intermediate/<Config>/Ninja/Binaries/<gameModule>` — inside the
`Intermediate/<cfg>/` clean target of §4.4 — and **arcbuild copies the
result into `Binaries/<gameModule>` itself** after a successful Ninja
build/rebuild (`arcbuild/src/Stage.cpp`, `NinjaLinkOutput` in
`ProjectLayout.cpp`; the `.pdb` beside it when the toolset made one). A
Premake post-build step cannot do this on Windows: beta8's ninja module
wraps post-build commands in `cmd /C "…"` and escapes every inner quote as
`\"`, which cmd.exe reads as a bare backslash — a quoted relative path
becomes a drive-root path (`\"Binaries\"` → `C:\Binaries`), and the module's
own always-appended stamp touch fails the same way. Characterized live
(the first run created `C:\Binaries` and failed; every later run had its
`&&` chain swallowed into `IF NOT EXIST` and exited 0 having done nothing).
Consequences: a raw `ninja <stem>_<Config>` links and stops; `arcbuild
build`/`rebuild` is what fills the slot; a Ninja child that exits 0 without
producing the link output is an arcbuild refusal (exit 2), never a
fabricated success. MSBuild and Make link straight into the slot and need no
staging.

Any other valid Premake action is accepted by `generate` but has no backend:
`build`/`rebuild` refuse it with exit 2; `clean` still performs the filesystem
cleanup. When `--action` is omitted, the default is platform-specific and
pinned by tests: `vs2026` on Windows, `gmake` on Linux, `xcode4` on macOS.

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
- `--action` is a premake generator token (platform-default when omitted —
  see the backend table above). It must be a non-empty `[A-Za-z0-9_-]+`
  identifier; anything else is a refusal so the token cannot become a shell
  fragment — moot in practice since the token is always passed as one
  argv entry, never concatenated into a command string (§4.2). `BuildBackend`
  classifies the action (`BuildBackend::None` for any action with no backend,
  e.g. a bare `vs2022` — still a valid `generate` target, just not one
  `build`/`rebuild` can drive).
- Output: every child line to stdout, prefixed `[premake]` / `[msbuild]` /
  `[gmake]` / `[ninja]` / `[xcodebuild]` (`BuildBackendPrefix`); the driver's
  own lines `[arcbuild]`. Exit code = the first failing child's, or `2` for a
  driver refusal (no SDK, no project, empty `--sdk`, bad `--action`,
  `--force-rebuild` on probe, no resolvable backend for `build`/`rebuild`),
  `0` on success. A child that never launched at all (tool not found, launch
  failure) is also a `2` refusal — never mistaken for a child's own exit code
  (`ProcessError`, multibackend hardening design §6.2). The editor keeps
  colouring lines by `": error"` / `": warning"` as today.

## 4. Behaviour

### 4.1 Resolution
`Arcane::Toolchain` (ArcaneCore) is the single owner of tool discovery;
`arcbuild::BackendResolver` wraps it with project-context checks and turns an
empty/missing result into a descriptive refusal. Every resolver call is a
real filesystem/PATH probe — never an optimistic bare name that makes an
unavailable tool look installed.

- premake: `<sdk>/ThirdParty/premake5/premake5[.exe]`, else `premake5` on
  PATH. Arcane's bundled copy wins over a global installation.
- msbuild: `vswhere -latest -requires Microsoft.Component.MSBuild -find
  MSBuild\**\Bin\MSBuild.exe`, else `msbuild` on PATH.
- make: on Windows, `mingw32-make` then `make`; on POSIX, `make`.
- ninja: `ninja` on PATH.
- xcodebuild: `/usr/bin/xcodebuild`, then `xcodebuild` on PATH — macOS only;
  empty (refused) on Windows and Linux.
- **`probe` is the one exception:** it needs no SDK, resolves no premake, and
  resolves no backend tool — it only inspects the project manifest and the
  `Binaries/<gameModule>` slot (§4.3), so `arcbuild probe --project <dir>`
  succeeds with neither `--sdk` nor `ARCANE_SDK` set. Every other command
  (`generate`/`build`/`rebuild`/`clean`) still requires a resolvable SDK.
- No developer-specific absolute path (e.g. a local `D:\...\tools` layout) is
  ever compiled in. A bundled or PATH-discovered tool is the only source of
  truth; a locally installed tool participates by being added to `PATH`.
- Tools are resolved per command: `probe` needs neither (see above);
  `generate` needs premake; `build`/`rebuild` need premake plus the resolved
  backend's builder; `clean` needs the backend's builder only when a
  generated context resolves for it (a resolver failure is a soft skip,
  §4.4). Backend resolution never spawns a tool speculatively — skipping it
  on `probe` is exactly why a nothing-stale check never shells out to
  `vswhere`, `mingw32-make`, or anything else.
- The SDK root is never inferred from the driver's own exe location (the
  editor knows its root and passes `--sdk`; scripts/CI have the variable).
  `SdkRootFromExeDir` stays in the editor.
- (Plan ruling R1: the probes above, plus `DiscoverSolution` and a
  `ResolveDevenv`, live in ArcaneCore as `Arcane::Toolchain` -- shared by
  arcbuild.exe and the editor's IdeLaunch, which still needs devenv and the
  solution path after ModuleBuild lost them.)

### 4.2 generate
A single structured process spec — executable = the resolved premake,
arguments = `[<action>]`, working directory = the project root
(`ComposeGenerate`, `arcbuild/src/Compose.cpp`). No shell: no `cmd.exe /c`,
no `2>&1` folding, no `cd /d` — the working directory is set on the child
process directly, and the validated action token is one argv entry, never
concatenated into a command string. This is the same direct-process shape
every backend uses (multibackend hardening design §6.1); the original
`( cd /d "<root>" && "<premake>" <action> ) 2>&1` shell form this line used
to describe was retired with the Windows-only `_wpopen` runner. Always runs
before `build`/`rebuild` (the stale-generated-project decision stands, now
for any backend's generated files, not just `.slnx`).

### 4.3 The incremental rule (the reason this exists now)
Before `build`, probe `<root>/Binaries/<gameModule>` (the manifest's
`gameModule`):

| Slot state | Decision | Why |
|---|---|---|
| absent | plain build | nothing to be wrong about |
| present, CRT flavor matches `--config` (Debug ⇔ `ucrtbased`) | plain build | the backend's own incremental view is trustworthy for this config |
| present, CRT flavor mismatches | full rebuild | the single-slot hazard: a plain build would report "up to date" and leave the other config's DLL in place |
| present, flavor unreadable | full rebuild | unknown ⇒ the safe choice; say so in the log |

`Module::ScanFileCrtFlavor` (ArcaneClient) is the probe — the same verdict
PluginHost uses to refuse a cross-CRT module, so the driver and the host can
never disagree about what "matches" means. `--force-rebuild` and the `rebuild`
command bypass the probe on `build`. `probe` prints the slot row it landed on
and exits from that row (R4); `--force-rebuild` on `probe` is a refusal so
the printed `-> full rebuild` cannot disagree with exit 0 on a matching slot.
This table's verdict (`OperationForBuild`) is backend-agnostic — `Compose`
is what turns "full rebuild" into MSBuild's `/t:Rebuild`, Make/Ninja's
two-step clean-then-build, or Xcode's one-invocation `clean build` (§8 of the
multibackend hardening design).

Dist maps to Release for the probe (both are release-CRT), matching
`ModuleBuild::Configuration()`'s Dist caveat.

### 4.4 clean
Resolve the backend's builder and generated context (soft-skipped, logged,
if either fails to resolve — e.g. `clean` run before any `generate`), run its
clean if resolved (`msbuild <sln> /t:Clean /p:Configuration=<cfg>`;
`make`/`ninja -C <root> ... clean`; `xcodebuild ... clean`), then ALWAYS
delete `Binaries/` (whole slot — it is one slot) and `Intermediate/<cfg>/`.
The filesystem deletes still run even if the backend clean failed or was
skipped -- a broken generated Clean target, or no generated context at all,
must not leave the slot behind. Precedence when both halves can fail
(`MergeCleanResults`): a filesystem-delete failure is a driver refusal (exit
2) **regardless of the backend's result**; otherwise the backend's own exit
code (0 on a soft skip or a real success, or its real nonzero code on a
genuine child failure) is returned unchanged. Never touches `Source/`,
`Content/`, `Saved/`, or the generated project files (`generate` rewrites
those). The game module's source root is the manifest's `sourceDir` (default
`Source/`; `Source/Game/` for the `Source/<Module>/` layout, 2026-09-16) --
premake reads it, the driver never needs to.

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

## 6. Extensibility

**Built, 2026-09-20 (multibackend hardening):** `--action gmake`/`gmakelegacy`
(Make), `ninja` (Ninja), and `xcode4` (Xcode, macOS-only resolution/execution)
each have a real resolve/compose/execute/exit-code contract, unit-tested on
every platform against real generated fixtures — see the Supported backends
table in §3 and
`docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md` §7/§8 for
tool resolution and the exact composed command per backend/operation. How
much of each is proven LIVE is exactly what the table's last column says and
no more: Ninja on Windows is a complete, linked, slot-filling build; Make on
Windows is the full driver mechanics plus a GCC compile that stops at the
MSVC-vs-MinGW link boundary; Linux and macOS have not run a build yet. §4.3's
probe stays the single-slot CRT-flavor check as written; it did not need to
"generalise per platform" — `Module::ScanFileCrtFlavor` already reads the
built module's own PE import table, which is backend-independent (the
Ninja-built, arcbuild-staged `Fixture.dll` carries the same CRT-flavor signal
an MSBuild-built one does).

**Not built (still a future seam):**
- `--engine <root>` as a second target kind: the same commands over
  `Arcane.slnx` with the ReferenceProject-first ordering and the staging
  post-build awareness golden-gate.ps1 carries today. Adding it must not
  change the game-project CLI. The source split is the seam:
  `Request.hpp` grows the flag, `Compose.hpp`'s spawn lines are reused,
  `Slot.hpp` (the game-module CRT table) is not, and a new engine-layout
  unit owns ReferenceProject-first / staging. `main.cpp` dispatches.

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
