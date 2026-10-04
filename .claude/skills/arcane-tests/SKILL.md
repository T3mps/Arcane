---
name: arcane-tests
description: Use when running ArcaneTests, reproducing an intermittent or order-dependent test failure, investigating a test crash or hang, a [gpu] test failing on validation messages, or CI reporting a coverage or baseline drop.
---

# Running Arcane tests

## Overview

ArcaneTests is a Catch2 v3 suite that runs in RANDOM order by default and resolves data paths
relative to the exe. A green suite covers code paths, not the real hosts: for rendered output
use the `arcane-verify` skill.

## Quick reference

Always run from the exe directory, in the foreground:

```bat
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe                      :: full suite (needs a capable GPU)
ArcaneTests.exe "~[gpu]"             :: no GPU on this machine
ArcaneTests.exe "<test name>" --rng-seed 1234
```

| Need | Where |
|---|---|
| The seed of a failing run | console banner `Randomness seeded to: N`; JSON reporter metadata `rng-seed`; JUnit property `rng-seed` |
| Crash or hang evidence | `<exe dir>\diagnostics\` -- read the `.txt` (symbolized all-thread stacks) BEFORE theorizing; `.dmp` for WinDbg |
| Coverage guard | `scripts\check-baselines.ps1 -ReportPath <catch json> -Configuration <Cfg> -Invocation "<how it was run>"` vs `scripts\automation-baselines.json` |

## Which CI lane ran what

| Lane | Suite | Coverage check | Artifacts |
|---|---|---|---|
| Jenkins (`windows && gpu`) | unfiltered, incl. `[gpu]`, Debug + Release | `-Invocation "unfiltered"`: no committed baseline, reports "NO BASELINE -- not checked" | `%WORKSPACE%\test-results\arcane-<cfg>.{xml,json}`; archives only verify PNGs, NOT `diagnostics\` |
| GitHub Actions | `"~[gpu]"`, Debug + Release + Dist | `-Invocation "~[gpu]"` against the committed baselines | `test-results\arcane-<cfg>.json` |

So a `[gpu]` crash came from Jenkins, and its crash files exist only on that agent's workspace
until the next build cleans it. A coverage drop came from GitHub Actions. To check one locally:

```bat
cd bin\<Cfg>-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "~[gpu]" -r console -r json::out=%TEMP%\arcane.json
cd ..\..\..
powershell -ExecutionPolicy Bypass -File scripts\check-baselines.ps1 -ReportPath %TEMP%\arcane.json -Configuration <Cfg> -Invocation "~[gpu]"
```

Dist legitimately runs 6 cases / 68 assertions fewer than Debug and Release (`#if !defined(ARC_BUILD_DIST)`
guards); that is not a drop.

## What counts as a failure

- Assertions, crashes, hangs (the watchdog captures them).
- **Validation noise.** `[gpu]` tests assert `Arcane::RenderErrorCount() == 0`, which latches NRI
  diagnostics AND raw D3D12/Vulkan validation VUIDs. A clean assertion list with a validation
  message is still a failure.
- **A coverage drop.** `check-baselines.ps1` fails when assertion counts go DOWN. `-Invocation` is
  part of the key: a baseline measured under `"~[gpu]"` is not comparable with an unfiltered run,
  and a mismatch reports "NO BASELINE -- not checked" rather than passing.

## Reproducing an intermittent failure

1. Get the seed from the failing run (table above) and the configuration that failed.
2. Rerun the full suite with `--rng-seed N`. Reproduces -> order-dependent. Run the failing test
   alone; if it passes alone, list the run order with `--list-tests --order rand --rng-seed N` and
   rerun the failing test together with halves of the tests before it (comma-separated names)
   until the trigger is isolated.
3. Does not reproduce with the same seed -> timing (jobs, GPU), not order. Loop the single test.
4. Known order hazards: a bare `Arcane::Runtime rt;` in a test (never do this), and Astra
   TypeContext theft between tests.

## Common mistakes

- Running from the repo root: data paths break and failures look like missing assets.
- Starting the suite in the background from a subagent: the harness can kill background children.
- Treating a green `[gpu][golden]` Catch2 case as proof the hosts render correctly. It links
  neither RuntimeApp nor EditorApp.
