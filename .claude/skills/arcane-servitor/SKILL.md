---
name: arcane-servitor
description: Use when a change must be checked in a RUNNING Arcane host (editor, runtime or server) rather than only in unit tests. Covers seeing what a scene renders now, stepping the simulation N ticks and inspecting entities or ECS state, reading or setting cvars, watching logs and Problems live, confirming a hot reload landed, and driving play/pause/stop. Also use it when a user asks what the engine is doing right now.
---

# Driving live hosts with Servitor

## Overview

`servitor` is a loopback CLI that attaches to running Arcane hosts and prints one JSON envelope per call. It is not an MCP server: run it with your shell tool, like `git`. Each host with remote armed listens on `127.0.0.1` and writes a descriptor (port + token) under `<project>\Saved\Remote\`. Servitor finds hosts through those descriptors.

A Servitor check is evidence for THIS change, not a regression net. If the behavior must stay fixed, write an ArcaneTests case or a golden lane (`arcane-tests`, `arcane-verify`).

**REQUIRED BACKGROUND:** `arcane-build` (hosts and the game module must be built for the config you launch).

## Setup

- **Binary:** `Binaries\<Cfg>-windows-x86_64-md\Servitor\servitor.exe`. It is built with `Arcane.slnx` and compiled out of Dist (there it exits 2).
- **Project:** `--project <dir|.arcproj>`, otherwise the nearest `.arcproj` above the working directory.
- **Engine root:** `--engine <root>` or `SERVITOR_ENGINE` when servitor cannot find the host exes beside itself.
- **Name yourself:** pass `--client claude-code` on every call, `launch` and `status` included, so the host's audit log (`Saved\Remote\audit.ndjson`) names the agent. `launch --client NAME` records NAME in the host's descriptor; `status --client NAME` lists only that client's hosts; `quit --client NAME` quits the hosts that client launched (several need `--yes`; none is exit 4), so launch with the same `--client` you quit with.
- **Read the output:** `--format json` (or `--json`) gives the full envelope. Branch on the exit code, never on HTTP status.

How each host gets remote:

| Host | How it arms remote | Startup |
|---|---|---|
| ArcaneEditor | cvar `editor.remote.enable` (default ON) or `--remote` | opens in Edit |
| ArcaneRuntime | `--remote` (`servitor launch` adds it) | open-ended run starts HELD at frame 0 (log line, `[held]` title); `play`/`resume` releases it; `--frames N` runs |
| ArcaneServer | `--remote` | same hold rule as the runtime |

## Recipes

```bat
set S=Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe
set L=--project ReferenceProject
set P=--project ReferenceProject --client claude-code

:: what is running
%S% %L% status

:: headless runtime, deterministic picture
%S% %P% launch runtime --headless -- --fixed-time 0
%S% %P% --host runtime wait settled
%S% %P% --host runtime screenshot window --out %TEMP%\shot.png
%S% %P% --host runtime compare runtime-scene --source window

:: step the sim exactly N ticks, then inspect state
%S% %P% --host runtime step 10
%S% %P% --host runtime entity get name:Player --component Transform
%S% %P% --host runtime ecs query --with Transform,RigidBody2D --fields Transform --limit 20

:: live diagnostics while reproducing something
%S% %P% --host editor problems --severity error
%S% %P% --host editor logs --level warn --follow --timeout 30
%S% %P% --host editor cvar explain render.vsync

:: after rebuilding the game module, confirm the editor reloaded it
%S% %P% --host editor wait reload

:: clean up what you launched
%S% %P% --host runtime quit
```

- `describe <command>` and `schema component <Name>` explain a command's or a component's arguments.
- `commands` prints the whole manifest and the exit codes with no host running.
- `call <command> name=value ...` reaches any registered command.

## Exit codes (branch on these)

| Code | Meaning | What to do |
|---|---|---|
| 0 | ok | |
| 2 | usage/refusal: unknown verb or flag, ambiguous target, a flag that does not apply (never silently ignored) | fix the command line |
| 3 | permission: read-only token, or the command's audience refused | use the drive path, or a different cvar |
| 4 | no host / stale descriptor / protocol mismatch | `launch`, or pass `--project` |
| 5 | busy or not ready; provably NOT run | safe to retry (servitor already retries) |
| 6 | the host ran it and it failed (`errors[]`) | read `errors[0].code` and the remediation |
| 7 | host died or unreachable mid-call | check for an `.arcdiag` path; relaunch |
| 8 | a check did not pass: `compare.failed`, a wait false at timeout, `call.timeout`, `launch.timeout` | the result is the finding |

## Rules

- **Determinism.**
  - Captures of time-driven materials need a `--headless` host launched with `--fixed-time`; `--fixed-time` is refused without `--headless`.
  - `wait settled` (two byte-equal captures, shaders idle, clock frozen while it captures) comes before any screenshot or compare that you judge.
- **Drive vs observe.**
  - `play`, `stop`, `pause`, `resume`, `step`, `cvar set`, `console`, `quit` and `compare --bless` change the host and are audited.
  - Use `--read-only` when you only mean to look.
  - `--all` fans out to every host, and a drive command under `--all` needs `--yes`.
  - Never `--bless` from servitor to "make a diff go away". Blessing follows `arcane-verify`'s re-bless procedure and needs the user's intent.
- **Do not fight the user's session.**
  - An editor the user has open is THEIR session. Do not `play`, `stop`, `quit` or `cvar set` it without asking.
  - Prefer launching your own `--headless` host.
  - Never `quit --force` or `--discard-changes` a host you did not launch.
- **Long steps.**
  - `step N` returns when `host.frame` moved N (a fixed tick each, or one loop pass each on a running windowed runtime).
  - Overlapping steps share one budget.
  - Give long steps a `--timeout`. A live host that is slow answers `call.timeout` (exit 8), not "relaunch".
- **Retry.** `--retry-on-death` resends only to a NEW replacement host of the same kind and project. It is refused with `--all` and with `--pid`.
- **Clean up.** `quit` every host you launched, and leave `Saved\Remote\audit.ndjson` alone. It is the audit trail.

## Common mistakes

- **Sandboxed agent CLIs.** Some block loopback by default (Codex does), so every call fails with exit 4 or 7. Allow localhost in that tool's sandbox settings.
- **Missing `wait`.** Calling a freshly launched host before `wait ready` gets exit 4/5. `launch` already waits for the descriptor; other spawns do not.
- **Held runtime.** A runtime launched open-ended is held at frame 0: `stats frame` stays put until `play`/`resume`/`step`.
- **Stale game module.** Judging pixels from a host whose game module is stale or the wrong config: see `arcane-build`'s single-slot trap.
