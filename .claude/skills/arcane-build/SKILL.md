---
name: arcane-build
description: Use when building Arcane or ReferenceProject, after adding/removing/renaming source files or editing any premake5.lua, when switching between Debug, Release and Dist, or when a host fails with "plugin: initial load failed", an access violation at module load, or unresolved symbols after a file was added.
---

# Building Arcane

## Overview

Two workspaces, one build order. The engine (`Arcane.slnx`, repo root) builds first; the game module
(`ReferenceProject.slnx`, built over the SDK) links the engine's import libs and builds second.
Most build pain here comes from three traps: an unregenerated file list, the single-slot game DLL,
and resolving `--project` against the wrong directory.

## Quick reference (repo root unless noted)

| Situation | Command |
|---|---|
| New/removed/renamed file, or any `premake5.lua` edited | `set _APH_NOPAUSE=1` then `GenerateProjects.bat` (cmd; in PowerShell `$env:_APH_NOPAUSE=1`) |
| Build the engine | `msbuild Arcane.slnx /p:Configuration=<Cfg> /m` (or `ci\msbuild.cmd` if msbuild is not on PATH) |
| Build the game module | `bin\<Cfg>-windows-x86_64-md\arcbuild\arcbuild.exe build --project ReferenceProject --config <Cfg>` |
| Game module after a config flip | the same command with `rebuild` in place of `build` |
| Manual game-module path | `cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=<Cfg> /m` |
| GPU smoke run | `bin\<Cfg>-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --frames 180` |

`arcbuild.exe` is built to `bin\<Cfg>-windows-x86_64-md\arcbuild\` and is not on PATH.
`arcbuild probe --project ReferenceProject` reports the slot verdict without building
(exit 3 = would rebuild).

## The traps

**Unregenerated file list.** A new `.cpp` is invisible to MSBuild until premake regenerates the
solution. Symptom: unresolved externals or code that silently never runs. Without `_APH_NOPAUSE`
the script blocks on `pause` in a non-interactive shell.

**Single-slot `ReferenceProject\Binaries\`.** Every configuration writes the same
`ReferenceGame.dll`. After Debug -> Release (or any flip), an incremental build can call itself
up to date and leave the other config's DLL in place; a Debug-CRT module in a Release host crashes.
After a flip, always `rebuild`, never `build`. Whichever config built last owns the slot.

**`--project` resolves against the working directory.** A relative `--project ReferenceProject`
opens the SOURCE tree when run from the repo root and the STAGED copy
(`bin\<Cfg>...\<Host>\ReferenceProject\`) when run from the exe directory. The staged copy is taken
by each host's post-build during the ENGINE build, before the game module is rebuilt, so in the
normal order its `Binaries\ReferenceGame.dll` is stale by construction. Run smoke tests from the
repo root, as CI does. (The golden gate is the exception: it restages and runs from the exe dir;
see `arcane-verify`.)

**Always build the solution, never a bare `.vcxproj`.** Project references and the shader prebuild
only run through `Arcane.slnx`.

**Stale objects after a layout change.** If a struct layout or ABI-visible header changed and
behavior is inexplicable, do a full rebuild before debugging.

**`ARCANE_SDK` can be stale in the process environment.** arcbuild resolves the engine through it
and fails with "no SDK" when it is unset. Pass `--sdk D:\dev\starworks\Arcane` (this repo's root)
explicitly rather than trusting the shell's value.

## Order for "everything, then verify"

1. Regenerate if the file list or premake changed.
2. Engine: `Arcane.slnx` for the target config.
3. Game module: `arcbuild rebuild` for the same config.
4. Run the host (`--frames 180`), or use the `arcane-verify` skill for image checks.

Shaders need no separate step: the ArcaneClient prebuild compiles `data/shaders/*.hlsl`.
