# Crash Window Design

**Date:** 2026-09-22

**Status:** Draft -- brainstormed and approved section by section on
2026-09-22; audited line by line against UE 5.8.2's Windows crash path the
same day (§2.1 records every delta and what changed here because of it);
awaiting the written-spec review before the implementation plan.

**Plan 1 status:** Plan 1 (core crash path) implemented 2026-09-22 on branch
feat/crash-window-plan-1, commit c15d71d6 (+ this close-out); plans 2
(reporter + NativeWindow + monitor mode) and 3 (autosave) pending.

**Plan 1 measurements (2026-09-22, Debug, this desk):**
- Death fixture (`bin/Debug-windows-x86_64-md/death-fixture`), wall time to
  exit measured by hand, kind from the envelope: `av` exit 10, report
  written; `assert` exit 10, ~148 ms, kind `assert`; `terminate` exit 10,
  135 ms, kind `terminate`; `abort` exit 10, 141 ms, kind `terminate`;
  `invalid-parameter` exit 10, 137 ms, kind `crash`; `purecall` exit 10,
  152 ms, kind `crash`; `stack-overflow` exit 10, 207 ms, kind `crash`;
  `oom` exit 10, 156-168 ms, kind `out-of-memory` -- 135-207 ms is the
  spread across these seven rows; `ensure` exit 0, ~311 ms, kind `ensure`,
  no `.dmp`; `--hang-at-exit --exit-seconds 2` exit 12 at 2223 ms;
  `--hang 5 --hang-seconds 1` exit 0 at 5058 ms.
- Desk: `ArcaneEditor.exe --project ReferenceProject --backend dx12
  --frames 900` exit 0; log retargeted to `<exe dir>/ReferenceProject/
  Saved/Logs/ArcaneEditor.log`.
- Suite: `~[gpu]` 1987 cases / 1983 passed / 4 skipped; golden gate 8/8;
  `[witness][gpu]` 6/6; `[witness][server]` 3/3.

**Sequencing (binding order, 2026-09-22 amendment):** this arc runs now,
ahead of F5. arcbuild II follows F5. The general allocator, worker-thread
heartbeats and the slow-task scope are owed to the introspection arc (§13).

**Reference rule (2026-09-04):** every design states what Source 2 / Deadlock
and UE do and why we match or diverge. §2 carries the citations; every UE
claim below was verified against `D:\dev\_reference\UnrealEngine-5.8.2-release`
on 2026-09-22 (file:line), not recalled.

## 1. Purpose

Today a crash in an Arcane host is handled in the faulting thread by
`Diagnostics::WriteReportImpl` (`ArcaneCore/src/Arcane/Base/Diagnostics.cpp`):
minidump, then a DbgHelp symbol walk of EVERY thread with symbols loaded for
every module, then the GPU provider touching the device, then the text and
envelope writes, then a log echo. The window stops pumping the instant the
fault happens, so the OS paints "Not Responding" for however long that takes
-- seconds in a Debug editor with a game module and forty threads -- and then
the process dies. The hang watchdog does the same work from its own thread
while the main thread sits wedged, and offers the user nothing. The freeze
the user sees before a crash is ours.

Worse, a whole family of deaths produces NO report at all. A failed
`ARC_ASSERT` is logged and then Mosaic calls `abort()`; the runtime's abort
ends in a fail-fast that bypasses the unhandled-exception filter, and in
Debug it first shows the CRT's modal abort box, which wedges a headless run
until an external cap kills it (measured: `ArcaneEditor/src/main.cpp:483-493`).
`std::terminate`, CRT invalid parameters and pure-virtual calls die the same
way. Not one CRT or OS death-path handler is installed anywhere in the hosts
(audit 2026-09-22: `set_terminate`, `signal`, `_set_invalid_parameter_handler`,
`_set_purecall_handler`, `SetErrorMode`, `_set_abort_behavior`,
`_CrtSetReportMode`, `SetThreadStackGuarantee`, `SetConsoleCtrlHandler`,
`WM_QUERYENDSESSION`: zero files). The engine log has only a stderr sink
(`Log.cpp`), so a Hub-launched editor leaves no log file when it dies. The
editor has no autosave, so a crash loses everything since the last manual save.

This design gives the hosts UE's shape: the crashed process does the minimum
on a dedicated thread, hands off to a separate reporter process that does the
slow work and shows the window, and terminates with an exit code its parent
can decode. Hangs and GPU stalls keep the host alive and let the window
decide. Every death path above becomes a report. The editor autosaves
everything dirty on a timer and offers recovery on the next start. The
splash's native-window machinery is lifted into Core so the reporter (and any
future window that must exist before or without a renderer) reuses it.

Non-goals: sending reports anywhere (no backend yet), a native debugger
(Visual Studio is the debugger; verified 2026-09-22 that UE ships none either
-- its build tool generates IDE debug settings, natvis and LLDB formatters,
and nothing that steps machine code), and a general allocator (§13).

## 2. What UE and Source 2 do

**UE 5.8.2, crash path.** A dedicated crash thread is created at startup with
raw Win32 "in case the original thread's stack is corrupted (stack overflow
etc)" (`Runtime/Core/Private/Windows/WindowsPlatformCrashContext.cpp:1248`);
the crashed thread signals it and waits. That thread writes the minidump in
process with the crash context embedded as a user stream, records a
module+offset "portable" callstack (`GenericPlatformCrashContext.cpp:1783`),
and launches `CrashReportClient.exe` DETACHED without waiting
(`WindowsPlatformCrashContext.cpp:1130`, `CreateProc(..., bLaunchDetached=true, ...)`).
Human-readable symbolization in process is opt-in only. The editor also
pre-launches a monitor client and talks to it over a pipe. The client is a
standalone Slate program with no Engine dependency
(`Programs/CrashReportClient/CrashReportClient.Build.cs:13`), symbolizes from
the minidump with the Windows debug engine
(`Developer/CrashDebugHelper/Private/Windows/WindowsPlatformStackWalkExt.cpp:51`),
and offers Send and Close / Send and Restart / Close Without Sending / Copy
Files To Clipboard plus an `-Unattended` mode. A hang is converted into a
crash on purpose: after the heartbeat threshold it writes to address 3 so the
normal crash path runs, avoiding allocations (`HAL/ThreadHeartBeat.cpp:196`).
Nothing is saved at crash time. A crash-time pooled allocator exists
(`GenericPlatformMallocCrash.h:36`) but is swapped in only on Unix/Android;
the Windows crash path never performs the swap.

**UE 5.8.2, autosave.** Maps and content packages, every 10 minutes, a 10 s
warning toast with save-now/postpone, a 15 s interaction delay
(`Engine/Config/BaseEditorPerProjectUserSettings.ini:180-190`); gated on PIE,
open menus, interaction, slow tasks, shader/asset compiles, VR, Sequencer
(`Editor/UnrealEd/Private/PackageAutoSaver.cpp:1212`); writes to
`Saved/Autosaves/` mirroring the package path with `_Auto<N>`, 10 rotating
backups (`AutoSaveUtils.cpp:10`, `FileHelpers.cpp:1359`); the package is
re-marked dirty after the autosave ("autosaving it will have cleared the
dirty flag", `FileHelpers.cpp:3634`); it never touches the transaction buffer
(`PackageAutoSaver.cpp:169,769`); `PackageRestoreData.json` is the crash
marker, rewritten as disabled on clean shutdown (`UnrealEdEngine.cpp:521`),
disabled under a debugger because "programmers like to just kill
applications" (`PackageAutoSaver.cpp:140`); the next start shows a modal
checkbox list and restore copies the autosave over the original
(`PackageRestore.cpp:354,700`).

**Source 2 / Deadlock** (from memory, no source on this desk): Valve's games
load a crash-handler DLL and hand minidumps to a separate Steam error-reporter
process; there is no game-owned window. Same shape -- separate process,
minidump first, symbolization elsewhere. We diverge only by owning the window,
because we have no backend.

**Where we diverge from UE, deliberately:**
- Hangs and GPU stalls keep the host ALIVE and the window decides (Keep
  Waiting / Terminate and Collect). UE kills after its threshold. A slow shader
  compile or a large import must not die to a rule.
- Recovery never copies an autosave over the original. The scene or document
  loads from the autosave with its path kept as the original and marked dirty,
  so the user commits with an ordinary save.
- The crash arena is used on Windows (UE builds one and never swaps it there).
- No comment box, no send, no screenshot: nothing to send to yet.

### 2.1 Audit against UE's Windows crash path (2026-09-22)

Every choice this spec had made from reasoning rather than source was checked
against `Engine/Source/Runtime/Core/Private/Windows/WindowsPlatformCrashContext.cpp`,
`Runtime/Launch/Private/Windows/LaunchWindows.cpp`,
`Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp`,
`Runtime/Core/Private/Microsoft/MicrosoftPlatformStackWalk.cpp` and
`Programs/CrashReportClient/Private/CrashReportClientApp.cpp`. Outcome:

| Item | UE 5.8.2 does | This spec now |
|---|---|---|
| Handler set | Less: `_set_invalid_parameter_handler`, `_set_purecall_handler`, `signal(SIGABRT)`, Debug-only `_CrtSetReportMode`; no `set_terminate`, no `_set_abort_behavior`, no `SetThreadStackGuarantee` (`LaunchWindows.cpp:90-96`, `WindowsPlatformMisc.cpp:1042`, `WindowsPlatformCrashContext.cpp:1460`) | Keeps the superset (§5.1); `set_terminate` adds the exception's `what()` and names `bad_alloc` as `out-of-memory`, which UE classifies too |
| `SetErrorMode` | Only under `-unattended` (`LaunchWindows.cpp:211-213`); interactive runs leave WER as the backstop | CHANGED: `SEM_NOGPFAULTERRORBOX` only when unattended, so WER LocalDumps stay the interactive backstop for what SEH never sees (§5.1) |
| Debugger attached | Runs WITHOUT the SEH guard "to exactly trap the crash" (`LaunchWindows.cpp:262-267`) | Match: no reporter, the debugger takes it (§9) |
| Crash-thread order | Heartbeat stopped first (`:1573`), log put into panic mode BEFORE signalling (`:1836`), context XML written BEFORE the minidump (`:1034` then `:419`), then the log copied into the folder, then the client launched | CHANGED to that order: watchdog stopped, log backlog frozen, minimal envelope first, minidump, text, GPU provider, full envelope rewritten atomically, log backlog dumped, spawn (§5.2) |
| Crash inside the crash thread | Its own `__try/__except` terminates with a dedicated code (`:1342-1345`, `CrashReporterCrashed`) | CHANGED: exit code 13 for a crash inside the crash path (§4, §9) |
| Portable stack | `RtlLookupFunctionEntry` + `RtlVirtualUnwind`, no DbgHelp at all, in its own SEH guard (`MicrosoftPlatformStackWalk.cpp:108-134`) | CHANGED: same, `StackWalk64` dropped (§5.2) |
| Log at crash | Panic-mode redirector, log copied into the crash folder, `LogFilePath` in the context (`GenericPlatformCrashContext.cpp:1102,1658`) | CHANGED: file sink keeps a backlog ring; the crash path dumps it into the folder; flush is best-effort (§5.6) |
| Relaunch | Records the command line and a separate sanitized restart line; client relaunches with `CreateProc` (`CrashReportClient.cpp:204`) | CHANGED: the host passes a sanitized relaunch line with dev crash flags stripped (§6) |
| Build machines | Never spawns the client ("not okay to have lingering processes", `:1052-1056`); files still written; unattended client has no deadline | CHANGED: no reporter spawn on a build machine; the unattended reporter has a hard deadline (§6) |
| Shutdown hang | No exit watchdog; the heartbeat simply never stops through shutdown and `RequestExit(true)` is `TerminateProcess` after one flush | CHANGED: the exit sentinel IS the watchdog kept alive through shutdown with an exit deadline, no new thread (§5.7) |
| Console close / logoff | Ctrl-C two-step; close/shutdown/logoff hard-terminate with `0xC000013A` (`WindowsPlatformMisc.cpp:1160-1168`); `WM_ENDSESSION` saves nothing | Kept ours, more than UE: two-step Ctrl-C adopted; close and end-session write autosaves within the OS budget, then a clean exit (§5.7) |
| Fail-fasts SEH never sees | The pre-launched MONITOR client watches the editor pid and, on an exit code it does not recognise, synthesizes an "AbnormalShutdown - ExitCode: STATUS_STACK_OVERFLOW" report with the log (`CrashReportClientApp.cpp:1126-1140`; limits documented at `WindowsPlatformCrashContext.cpp:1798-1801`) | ADDED §5.8 monitor mode: the reporter is pre-launched for windowed hosts and turns an unrecognised exit code into an `abnormal-exit` report. The "cannot be caught" row is closed the way UE closes it |
| Ensures | A continuable report shape shares the crash pipeline, no dump, no all-thread capture, execution continues (`:1871-1887`) | ADDED: `ARC_ENSURE` failures write a lightweight `ensure` report once per site (§5.3) |
| Stack overflow | No guard-page reset; the crash thread plus the monitor | Ours adds the stack guarantee and keeps both |
| Exit codes | `3` on a crash via `RequestExit`, `777xxx` only for reporter diagnostics | Kept 10/11/12, added 13; the kind stays in the envelope |

## 3. Decisions taken in the brainstorm (2026-09-22)

1. Coverage: crashes, hangs and GPU stalls. (A)
2. "Graceful" = the minimum hand-off plus periodic autosave.
3. Autosave is keyed off the one command stack's `StateId()` and NEVER marks
   a document or the scene clean. Undo history is not persisted.
4. Hang policy: report, keep alive, the window decides.
5. Autosave scope: everything dirty, through a serialize-to-path seam every
   document type implements once. UE-style rotation, marker, startup prompt,
   debugger-disabled marker.
6. Reporter: a native `ArcaneCrashReporter.exe` on ArcaneCore only, no GPU.
   Rationale (user): no dependency in either direction between the Hub and
   the hosts -- the editor must work from a source build without the Hub,
   which is a commercial add-on; and the reporter ships beside a packaged
   game, as UE ships CrashReportClient.
7. Gaps folded in (coverage audit, §5.3): the fail-fast family; the exit
   sentinel with clean-exit handlers; a fixed report-path arena. Worker
   heartbeats and the slow-task scope are owed (§13).
8. The general allocator (mimalloc behind a Core allocation entry with
   per-module new/delete replacement and an ABI-gate check) goes to the
   introspection arc together with Tracy's memory hooks; Astra gets a
   slab-provider hook there, never a malloc hook (its chunk pool is TLSF over
   `VirtualAlloc` slabs and must stay that way).
9. The splash's native-window machinery is lifted into Core as
   `Arcane::NativeWindow` (§7).

## 4. Architecture

Four components, three plans (§12):

- **Core `Diagnostics`** (`ArcaneCore/src/Arcane/Base/`): the crash thread,
  the fail-fast family, the arena, the portable stack, the log file sink, the
  reporter hand-off, the exit sentinel and clean-exit handlers, the hang
  protocol. Windows-only bodies, no-op elsewhere, as today.
- **`ArcaneCrashReporter`** (new premake `WindowedApp` project beside
  `arcbuild`/`arccook`; links `ArcaneCore.dll` + `dbgeng`/`dbghelp`; NEVER
  `ArcaneClient`): symbolization, the window, unattended mode. Staged beside
  every host by each host's post-build, exactly like `ArcaneCore.dll`.
- **Core `NativeWindow`** (`ArcaneCore/src/Arcane/Platform/`, new directory):
  the thread-owned Win32 window base the splash and the reporter present on.
- **Editor autosave** (`ArcaneEditor/src/`): the `SaveTo` seam, the
  scheduler, the marker, the restore prompt.

**Data flow.** Host faults -> crash thread writes `<stem>.dmp`, `<stem>.txt`,
`<stem>.arcdiag` (existing sibling scheme, `ReportDir()`) -> spawns the
reporter detached -> host terminates with the crash exit code. Reporter reads
the envelope, opens the minidump, writes `<stem>.symbolized.txt`, shows the
window (or exits, unattended). For a hang the host stays alive; the reporter
holds the pid and a named event.

**Hand-off contract.** Command line, built into static storage at install so
nothing allocates in the filter:

```
ArcaneCrashReporter.exe <path.arcdiag> --pid <n> --kind <crash|hang|gpu-stall|gpu-crash|assert|terminate>
                        --product "<name>" [--unattended] [--recovered-event <name>] [--relaunch "<cmdline>"]
```

Everything else comes from the envelope. `Diag::Envelope` gains, additively
(format version unchanged, same rule as `foreignModules`): `logPath`,
`commandLine`, `exitCode`, and the kind vocabulary grows by `assert` and
`terminate` (`DeriveKind` matches those substrings ahead of "crash").

(plan 1 as built) Spawning the absent reporter is one stderr line plus one
`ARC_WARN` after the files exist -- plan 1 has no reporter to spawn yet, so
the hand-off contract's command line is built but unused; the minimal
envelope carries the portable stack in `cpuThreadSummary`, not a separate
field (R4); a full envelope that cannot fit the arena is elided (an 8 KiB
lean form) or withheld entirely, never truncated on disk.

**Exit codes** (a block clear of the hosts' existing 0-5, which the Hub and
the witness harness decode): `10` crashed with a report written, `11` hang
terminated by the reporter (the reporter's `TerminateProcess` argument), `12`
exit sentinel fired, `13` a crash inside the crash path itself (UE's
`CrashReporterCrashed` shape). The kind is always in the envelope; the codes
only tell a parent "a report exists". Fail-fasts SEH never sees keep their
NTSTATUS codes, and the monitor (§5.8) turns those into a report.

**Threads.** The crash thread (raw `CreateThread`, created in `Install`,
waits on an event); the watchdog thread (existing); the reporter's UI thread
and one symbolization worker. The filter runs on the faulting thread and only
signals and waits.

**ABI.** New Core exports -> an ABI bump (cheap by standing rule).
`VerifyReport` is unchanged.

## 5. The crash path

### 5.1 Install

`Diagnostics::Config` gains: `productName` (window title; the editor passes
"Arcane Editor", the runtime the project's name), `reporterPath` (empty =
`<exe dir>/ArcaneCrashReporter.exe`), `unattended` (set by every `--headless`
host), `commandLine` (for relaunch), `logDir` (empty = `<dump dir>/../Logs`,
retargeted with `RetargetDumpDir`).

Order inside `Install`, first to last, and the order is the point:
1. `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)`, plus
   `SEM_NOGPFAULTERRORBOX` ONLY when `unattended` -- UE's rule
   (`LaunchWindows.cpp:211`): an interactive run keeps WER reachable as the
   backstop for the fail-fasts SEH never sees, and our own filter terminates
   before WER's dialog could appear for everything else.
2. `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`;
   `_CrtSetReportMode(_CRT_ASSERT | _CRT_ERROR | _CRT_WARN, _CRTDBG_MODE_FILE)`
   to stderr in Debug. No CRT dialog can appear after this line.
3. `std::set_terminate`, `signal(SIGABRT)`, `_set_invalid_parameter_handler`,
   `_set_purecall_handler`, and the Mosaic assert handler (§5.3), all routing
   into the same report entry with their own kind.
4. `SetThreadStackGuarantee` on the calling (main) thread; the job system and
   `ServiceThread` call it for their workers (a one-line helper).
5. The arena (§5.5), the crash thread, the reporter command line, the log
   file sink, then `SetUnhandledExceptionFilter` and the watchdog as today.

The current rule that `Install` must come AFTER every flag refusal
(`main.cpp:347-362,443-448`) exists only because a `return` without
`Shutdown` left the watchdog's `std::thread` joinable at static destruction.
The watchdog thread becomes a raw thread with a stop flag joined from an
`atexit` hook registered by `Install`, so `Install` is safe as the first line
of `main` and the per-site `Shutdown()` calls before refusals go.

(plan 1 as built) `Install` runs first after argument parsing EXCEPT for
`--print-engine-info`, the Hub's probe, which runs before it so the probe
stays free of diagnostics artifacts (R25). `Shutdown()` still disarms the
watchdog with a bounded wait -- the sentinel window is
`RequestCleanExit()` -> `Shutdown()` (R1). The `atexit` hook registered
from ArcaneCore.dll runs at `DLL_PROCESS_DETACH` after other threads are
gone, so what actually makes Install-then-return safe is the raw thread
handle, not the hook itself (owed: fix the comment that implies otherwise).

### 5.2 On a crash

The filter (faulting thread): `g_inCrashHandler` guard as today (UE's
`ReportCrashCallCount`); freeze the log backlog (§5.6, UE's `GLog->Panic()`
before anything else); store the `EXCEPTION_POINTERS`, the thread id and the
kind in the arena; signal the crash thread; `WaitForSingleObject` up to 60 s
(UE's timeout); then `TerminateProcess(GetCurrentProcess(), 10)`. Never
chains to the previous filter for our own kinds (that chain is what let
WER's dialog appear).

The crash thread runs inside its own SEH guard whose handler terminates the
process with 13 (UE: `CrashReporterCrashed`, `:1342`). Its steps, in UE's
order (`HandleCrashInternal`, `:1570-1669`), each independently survivable:
1. Stop the watchdog so no hang report interleaves with this one (UE stops
   its heartbeat first, `:1573`).
2. The portable stack of the faulting thread from the captured `CONTEXT`:
   `RtlLookupFunctionEntry` + `RtlVirtualUnwind` per frame, addresses
   resolved to `module+offset` against the loaded-module list -- no DbgHelp
   at all (UE: `MicrosoftPlatformStackWalk.cpp:108-134`). Unloaded modules
   marked (the minidump carries `MiniDumpWithUnloadedModules`).
3. A MINIMAL envelope first: kind, reason, phase, portable stack, sibling
   paths, from the arena, so the reporter can start even if everything
   after this wedges (UE writes its context XML before the minidump,
   `:1034`).
4. Minidump (`MiniDumpWriteDump` with the faulting thread's exception
   pointers, same type flags as today).
5. The `.txt` header as today (reason, app, pid, phase, `injected :` line
   from `ForeignModules::LastScan`, exception) plus the portable stack. The
   all-thread symbolized walk is REMOVED from the in-process path; the
   reporter produces it from the minidump.
6. The GPU section provider (DRED, device fault), unchanged, bounded by the
   filter's timeout.
7. The FULL envelope, rewritten atomically over the minimal one.
8. The log backlog dumped into the crash folder as `<stem>.log.txt` (UE's
   `DumpLog`, `GenericPlatformCrashContext.cpp:1658`), then a best-effort
   file-sink flush, then the reporter spawn (`CreateProcessW`, detached, no
   handle kept, static command line + the envelope path), then return; the
   faulting thread wakes and terminates.

`ARC_ERROR`'s echo of the whole report into the log stays but moves after the
files are on disk.

### 5.3 The fail-fast family

Each handler builds a synthetic reason in the arena and enters the same
crash-thread path with `EXCEPTION_POINTERS` = null (the minidump is still
written with the current context):
- **Assert** (`Arcane::Assert::MosaicHandler`, `Base/Assert.cpp`): with a
  debugger attached the handler logs and returns `AssertAction::Break` as
  today, so Mosaic's `FailFatal` breaks into the debugger. Without one it
  writes a report of kind `assert` (the envelope carries expression, message,
  file and line) through the crash thread and terminates the process with
  code 10; it never returns and `abort()` is never reached.
- **terminate** (`std::set_terminate`): kind `terminate`; if
  `std::current_exception()` is set, its `what()` goes into the reason, and
  a `std::bad_alloc` makes the kind `out-of-memory` (UE classifies OOM as
  its own crash type, `:1582-1627`). (plan 1 as built) With our
  unhandled-exception filter installed, an uncaught C++ exception reaches
  the FILTER before `std::terminate`, and `std::current_exception()` is
  empty there -- measured, not assumed -- so the filter decodes the MSVC
  throw record (`ThrowInfo` -> catchable types) for kind `terminate` /
  `out-of-memory` and `what()` instead; `std::set_terminate` remains
  installed for the paths that DO reach it (noexcept violation, terminate
  during unwinding, an explicit call) (ruling R20).
- **ensure** (`ARC_ENSURE` / Mosaic `FailEnsure`, the recoverable path):
  kind `ensure`, a LIGHTWEIGHT report -- envelope and portable stack only,
  no minidump, no all-thread capture, no window, at most once per call site
  per session -- and execution continues. UE's continuable-report shape
  (`:1871-1887`, "don't capture all threads to report and resume quickly").
  (plan 1 as built) `ARC_ENSURE` evaluates its condition at the call site
  and raises the ensure scope only around the failure report, so a fatal
  assert reached while evaluating the condition stays fatal (R22); the
  discriminator is one exported thread-local accessor in Core, because a
  header-inline `thread_local` gives each module its own copy across DLLs
  (R21); a bare `MOSAIC_ENSURE` reaching the Arcane handler is fatal, not
  recoverable (owed: give Mosaic's `AssertContext` a recoverable flag).
- **SIGABRT** (`signal`): kind `terminate`, reason "abort() called" -- covers
  a third party's `abort()`.
- **Invalid parameter / pure call**: kind `crash`, reason names the CRT
  check and the expression/function the CRT passes.

What cannot be caught in process: `__fastfail` raised by ntdll (heap
corruption, `RtlReportCriticalFailure`), `/GS` cookie failures, and a stack
overflow with no room left to run SEH. UE documents exactly these limits
(`WindowsPlatformCrashContext.cpp:1798-1801`) and closes them OUT of process
with its monitor client; §5.8 does the same. WER LocalDumps remain the
last-resort backstop and stay reachable in interactive runs (§5.1 item 1).

### 5.4 Hangs and GPU stalls

The watchdog thread writes the report exactly as §5.2 steps 1-6 (no
exception pointers; the main thread's context captured by suspending it as
today), spawns the reporter with `--kind hang|gpu-stall --pid <n>
--recovered-event Local\Arcane-Recovered-<pid>`, and KEEPS THE HOST ALIVE.
The host remembers the reporter's pid; while it lives no second reporter is
spawned for this pid. The watchdog's once-per-stall rule is unchanged. If the
main thread's beat resumes, the watchdog signals the event and the reporter
closes itself. If the user chooses Terminate and Collect, the reporter
`TerminateProcess(host, 11)`es and becomes the crash view of the report it
already holds.

### 5.5 The arena

A fixed static block (256 KiB, hard cap) bump-allocated by the crash thread
for the reason text, paths, the portable stack and the envelope's JSON; reset
per report; on exhaustion the thread writes what it has and says so in the
header. It is NOT a general allocator and never becomes one (§13).

### 5.6 Log file sink and backlog

`Log::Init` adds a file sink at `<logDir>/<App>.log` (rotated per run,
keep 5) beside the stderr sink, retargeted when the dump dir retargets
(`Saved/Logs/` under a project), `flush_on(warn)`. The sink also keeps a
BACKLOG: a fixed ring of the last 512 formatted lines written without taking
the sink's mutex on the read side. The crash path freezes the ring on the
faulting thread (UE's panic mode) and the crash thread dumps it into the
crash folder, so the report folder is self-contained and no lock the dead
thread held can block it. The envelope's `logPath` names the file; the
reporter shows the folder's `.log.txt` first and the live file's tail when
the folder copy is missing.

(plan 1 as built) The file sink attaches in `Diagnostics::Install`, not
`Log::Init` -- the sink needs the resolved `logDir`, which `Install`
computes -- and re-attaches on `RetargetDumpDir` when `logDir` was derived
from the dump dir (R5). `AttachFileSink` called again on an already-attached
path rotates the existing file and replaces the sink rather than erroring or
duplicating it (R13).

### 5.7 Exit sentinel and clean-exit handlers

The exit sentinel is the WATCHDOG KEPT ALIVE THROUGH SHUTDOWN, UE's shape
(its heartbeat thread never stops during exit, `LaunchEngineLoop.cpp`), with
one change of rule: from `Diagnostics::RequestCleanExit()` onward the beat is
replaced by an exit deadline, `Config::exitSeconds` (default 30). If the
process has not exited by then, the watchdog writes a `hang` report ("hang
at exit") through the crash thread, spawns the reporter, and terminates with
12. No new thread; `Shutdown()` no longer joins the watchdog early. It is the
only thing that can name the Vulkan teardown hang and the module-build join.

`SetConsoleCtrlHandler` and the hosts' window procedures on
`WM_QUERYENDSESSION`/`WM_ENDSESSION` route to `RequestCleanExit()`, a hook
the host installs. Ctrl+C is two-step as in UE (`WindowsPlatformMisc.cpp:1113`):
the first requests the clean exit, the second terminates. Console close,
logoff and shutdown run the hook within the OS budget (five seconds for a
console handler): the editor writes autosaves for everything dirty first,
which is fast, then runs its ordinary exit. If the OS cuts it short the
autosave marker stays enabled (§8.5), which is the right answer. UE saves
nothing on either path; we do more here on purpose.

(plan 1 as built) `exitSeconds == 0` disables the sentinel outright, for a
host that wants no exit deadline. The console handler is gated by
`installCrashHandler`, the same gate as the fail-fast family, since it is a
process-wide handler (R24); a Ctrl-C that arrives with no clean-exit hook
installed is DECLINED -- the handler returns `FALSE` before touching the
press counter or arming the sentinel, so Windows' default termination runs
-- rather than being silently swallowed by an empty hook slot. Every host
installs a hook, so this only matters for a bare `Config`.

### 5.8 Monitor mode (windowed hosts)

Adopted from UE's monitor client (`WindowsPlatformCrashContext.cpp:512-640`,
`CrashReportClientApp.cpp:1126-1160`): at `Install`, a windowed host that is
not on a build machine launches `ArcaneCrashReporter.exe --monitor <pid>
--product "<name>" --log <path> --report-dir <dir>` detached and hidden. The
monitor waits on the host's process handle and inspects the exit code:
- a known code (0-5, 10-13) or a fresh report in the report directory: the
  monitor exits silently;
- anything else (an NTSTATUS such as `STATUS_STACK_BUFFER_OVERRUN`,
  `STATUS_HEAP_CORRUPTION`, `STATUS_FAIL_FAST_EXCEPTION`, a stack overflow
  that had no room for SEH, or an external kill): it synthesizes a report of
  kind `abnormal-exit` -- envelope with the exit code named, the host's log
  file tail copied as the backlog -- and shows the window as for a crash.

This closes the fail-fast row the in-process path cannot, exactly as UE
closes it. The monitor never symbolizes (there is no minidump) and never
spawns itself. Headless and build-machine runs launch no monitor. One monitor
per host process; the reporter spawned by a crash and the monitor never both
show a window, because the monitor sees the fresh report and exits.

## 6. The reporter

**Program.** `ArcaneCrashReporter` (`ArcaneCrashReporter/src/`): argument
parser, envelope-to-view model (pure), symbolizer, presenter over
`NativeWindow`. Links `ArcaneCore.dll` (envelope parser, `ForeignModules`
table for naming overlays, log), `dbgeng.lib`, `dbghelp.lib`. No
`ArcaneClient`, no GPU, no ImGui.

**Window** (plain Win32 controls, on screen within a second saying
"Symbolizing", filled from a worker thread):
- Header: product, plain-words kind (crashed / stopped responding / the GPU
  stopped responding / the GPU device was lost / an assertion failed /
  terminated), time, phase, build.
- Reason: exception code and address; or the assert's expression, message,
  file:line; or the hang duration.
- Injected modules, named with product and tier from the table.
- Stack: the faulting thread first, other threads from a selector; without
  PDBs, module+offset with unloaded modules marked.
- GPU section when present (queues, fault, layers) and the last 200 log lines.
- Buttons: Open Report Folder, Copy Details, Close. Hang: Keep Waiting,
  Terminate and Collect. Relaunch when `--relaunch` was passed (editor and
  runtime pass their own command line); disabled for a hang until terminated.
- No comment box, no send.

**Symbolization.** `DebugCreate` -> `IDebugClient::OpenDumpFile` ->
`WaitForEvent`; symbol path = the host's directory + `_NT_SYMBOL_PATH`;
`GetContextStackTrace`/`GetStackTrace` + `GetNameByOffset` +
`GetLineByOffset` per frame. Output written as `<stem>.symbolized.txt`
beside the report, unattended too. The editor's `CrashReportDocument`
showing that sibling is owed (§13).

**Unattended** (`--unattended`, every headless host): no window; symbolize,
write the sibling, exit 0, under a hard deadline of 60 s after which it
writes what it has and exits -- UE's unattended client has no deadline and
UE therefore never launches it on build machines ("not okay to have
lingering processes", `WindowsPlatformCrashContext.cpp:1052`). We keep the
spawn on a desk's headless runs because it is bounded, and skip it entirely
on a build machine: `Config::spawnReporter` is false when `ARCANE_BUILD_MACHINE`
or `CI` is set in the environment (Jenkins sets it); the files are still
written. Gate and CI logs on a desk gain readable stacks; the hand-off path
stays exercised there.

**Relaunch line.** The host passes `--relaunch` as a SANITIZED copy of its
command line: dev crash flags (`--crash-gpu`, the death-fixture flags) and
scripted-run flags stripped, so a relaunch never re-crashes on purpose or
re-runs a scripted capture. UE keeps a separate `RestartCommandLine` for the
same reason.

**Failure modes.** Reporter missing or `CreateProcessW` fails: one line to
log and stderr, same exit code, report on disk. Reporter crashes: its own
`Diagnostics::Install` writes into the same folder; it never spawns itself
(`reporterPath` empty). Debug engine unavailable: module+offset from the
portable stack. Debugger attached to the host: no reporter spawned (the
debugger has the crash), as UE.

## 7. `Arcane::NativeWindow`

Lifted from `ArcaneClient/src/Arcane/Host/BootSplashWindow.cpp`, which has
been hardened by two review rounds: a window owned by its own thread, class
registration and creation on that thread, a blocking `GetMessageW` loop,
cross-thread calls marshalled through posted messages, an atomic handle with
an "ever opened" flag so close-after-destroy and close-before-create are both
safe, a close that waits on the thread, and the never-fail degrade rules.

`ArcaneCore/src/Arcane/Platform/NativeWindow.hpp/.cpp` (Windows-only, no-op
elsewhere) owns exactly that machinery plus DPI, and calls a small
`INativeWindowPresenter` on the window thread: `OnCreate(HWND)` (create
child controls), `OnPaint(HDC, RECT)`, `OnCommand(id)`, `OnSize`, `OnUser(msg,
w, l)`, `OnDestroy`. `BootSplashWindow` becomes a presenter in
`ArcaneClient` keeping only its image, status line, progress bar and taskbar
progress; its public API and its device-free tests are unchanged. The reporter
is a presenter in its own exe using standard child controls. This is not a
UI toolkit and not a second window path for the hosts.

## 8. Autosave and recovery

### 8.1 The seam

`EditorDocument` gains `virtual bool SaveTo(const std::filesystem::path&) const`
(serialize only). `Save()` becomes `SaveTo(own path)` then mark clean. Every
existing type's `Save()` is one call to a path-taking Core primitive today
(`SaveSpriteAsset`, `SaveMaterialAsset`, `SaveMeshAsset`), so each
implementation changes one line. `SceneSession` gets `SaveTo(path)` over
`Scene::SaveSceneFile` without `MarkSaved`. `CrashReportDocument` is read-only
and never dirty. `DocumentHost::SaveAllDirty` is unchanged in behaviour.
New document types implement one method and are covered -- the scalability
requirement.

### 8.2 The change clock

One `CommandStack`, one `StateId()`. The autosaver keeps
`m_lastAutosavedStateId`; it is due only when the id moved since then and
something is dirty (`SceneSession::IsDirty` or `DocumentHost::AnyDirty`).
Undo back to that point writes nothing. It never calls `MarkSaved` or a
document's clean transition. Invariant stated by the spec: every edit path
goes through the one history (`Push`/`Commit`); an edit that bypassed the
stack would bypass autosave -- a named bug class
(`feedback_editor_only_plumbing_bug_class`'s cousin).

### 8.3 When

A timer in the editor frame tick. Settings in the editor ini `[Autosave]`,
defaults matching UE: `Enable=1`, `Minutes=10`, `WarningSeconds=10`,
`InteractionDelaySeconds=15`, `MaxBackups=10`. The warning is a status-bar
countdown with Save Now and Postpone. Gates (blocked -> retry in 3 s): play
mode active, `CommandStack::InTransaction()`, a pending unsaved-changes
confirm (scene or document host), a project switch in flight, a module build
running, a cook or import in flight, shader compiles in flight, mouse
captured. Headless hosts never arm the autosaver.

### 8.4 Where

`<project>/Saved/Autosaves/<content-relative path>_Auto<N>.<ext>`, `N`
rotating over `MaxBackups` per file, scene and documents alike, written
through the same atomic temp-and-replace primitives the real saves use.

### 8.5 The marker

`<project>/Saved/Autosaves/RestoreData.json`:
`{ "restoreEnabled": bool, "entries": [ { "assetGuid", "originalPath",
"autosavePath", "stateId", "writtenUtc" } ] }`. Rewritten after every
autosave and whenever the dirty set changes; written as `restoreEnabled:false`
with entries cleared on clean shutdown and when a manual save leaves
everything clean; NEVER written under a debugger (`IsDebuggerPresent`, UE's
rule) or under `--headless` (the witness harness copies the staged slot and
kills hosts on timeout; a marker there would prompt on the next lane and wedge
the gate).

### 8.6 Recovery

On project open, an enabled marker with entries shows a modal list
(`DialogSlot`): per entry a checkbox, name, autosave time, and a warning when
the original is newer; buttons Restore Selected and Skip. Restore loads the
scene from the autosave with the session's path kept as the original and the
scene marked dirty, and opens each selected document from its autosave with
its path retargeted to the original and marked dirty. Either button writes
the marker disabled. Autosave files are left for rotation.

### 8.7 Crash time

Nothing is saved at crash time (UE's rule and ours). The crash thread flushes
the log; the autosaves and the marker are already on disk.

## 9. Edge cases

- **Crash inside the crash path.** The crash thread's own SEH guard
  terminates the process with 13 at once (UE's `CrashReporterCrashed`); if
  the guard itself cannot run, the faulting thread's 60 s wait expires and
  terminates with 10. The minimal envelope was written first, so the monitor
  or the next start can still say what happened.
- **Logger deadlock.** The faulting thread may have died holding spdlog's
  mutex; the flush uses a bounded try and moves on.
- **Second faulting thread while a report is in flight.** It waits on the
  crash thread's event (serialized) and, on timeout, terminates like the
  first.
- **Debugger attached.** Watchdog suppressed as today; the debugger takes
  the crash; no reporter; no marker.
- **Headless / unattended.** No window anywhere: reporter unattended, no
  autosave, no marker, no prompt, OS dialogs off by error mode.
- **Console close / logoff / shutdown.** §5.7.
- **Killed externally.** Nothing runs; the marker stays enabled and the next
  start offers recovery (correct, UE same).
- **Static-init crash / third-party `ExitProcess`.** WER or silence, as
  today; `Install` moves to the first line of `main` to shrink the window.
- **Arena exhaustion.** §5.5.
- **Reporter pid reuse.** The hang protocol keys the event on the host pid
  and the reporter checks the host's creation time before terminating it.

## 10. Testing

- **Death fixture**: `ArcaneTests/death-fixture/DeathFixtureMain.cpp`, a
  program in the style of `process-fixture` that installs Diagnostics
  (unattended, reporter path = the staged reporter) and dies on request:
  `--die av|assert|terminate|abort|invalid-parameter|purecall|stack-overflow`,
  `--hang <seconds>` (with `hangSeconds` lowered), `--hang-at-exit`. `[diag]`
  tests spawn it through `HostWitness` and assert: the report triple exists,
  the envelope's kind, the exit code (10/11/12), the reporter's
  `.symbolized.txt`, and a bounded wall time -- the proof no dialog appeared.
- **Pure units** (`[diag]`, `[editor]`): portable stack formatter; arena
  (bump, reset, exhaustion); autosave scheduler rule (timer, gates, state id);
  marker state machine; restore-prompt model; reporter argument parser and
  envelope-to-view model; `NativeWindow` create-and-close on its thread.
- **Symbolization**: against a minidump the fixture produced, once with PDBs
  present (names resolve) and once with them hidden (module+offset).
- **`SaveTo`**: each document type through the existing asset round-trip
  tests, plus "autosave never marks clean" over `CommandStack::StateId`.
- **Witnesses** (`[witness][gpu]`): the existing GPU-crash lane gains the
  reporter sibling; a hang lane proves the report and sibling under the
  harness cap; a headless editor writes no marker.
- **Gate**: 8/8 unchanged; headless never arms autosave, so no chrome change.
- **Desk**: the overlay close-crash reproduction shows GPU Tweak III by name
  in the window; a deliberate assert in the Aphelyon module shows expression
  and location; the exit sentinel against the Vulkan teardown hang if it
  reproduces.

## 11. Compatibility

`Diag::Envelope` additions are optional (older envelopes parse). `DeriveKind`
gains two kinds ahead of "crash". The Hub keeps working unchanged and may
later decode 10/11/12. The witness harness's grading is unchanged. ABI bump
for the new Core exports.

## 12. Delivery sequence

Three plans, each independently mergeable, in this order:
1. **Core crash path**: crash thread, fail-fast family, arena, portable
   stack, log file sink, hand-off (spawning an absent reporter is a logged
   no-op), exit sentinel, clean-exit handlers, death fixture, `[diag]` tests.
   Removes the freeze on its own.
2. **Reporter**: `NativeWindow` lift (splash re-presented), the
   `ArcaneCrashReporter` program, symbolization, unattended mode with its
   deadline, monitor mode (§5.8), staging, witness lanes, desk proof.
3. **Autosave and recovery**: the `SaveTo` seam, scheduler, marker, prompt,
   settings, tests.

## 13. Out of scope, owed

- Worker-thread heartbeats and the slow-task scope (introspection arc).
- The general allocator: Core allocation entry, per-module new/delete
  replacement, ABI-gate check, Tracy memory hooks, mimalloc behind the entry
  once measured; Astra slab-provider hook (Astra repo first, then sync).
- A WER runtime-exception module: no longer needed once the monitor (§5.8)
  covers the fail-fast row; kept here only as the fallback if the monitor
  proves unreliable on a desk.
- `CrashReportDocument` reading `.symbolized.txt` and `foreignModules`
  products.
- The Hub decoding exit codes 10/11/12 into a "crashed, report at" row.
- Document autosave beyond the built-in types is covered by the seam; a
  document that cannot serialize to a path must say so in its own spec.

Owed from plan 1's build (2026-09-22):
- Mosaic `AssertContext` needs a recoverable flag, plus a grep gate banning
  raw `MOSAIC_ENSURE*` outside `Assert.hpp` (a bare `MOSAIC_ENSURE` through
  the Arcane handler is fatal today, §5.3).
- `DeriveKind` classifies by substring match; the assert/ensure reasons now
  carry a file path, so a guard in a path containing "gpu" or "assert" can
  be misclassified. Wants a prefix-match rule instead.
- `StopWatchdog`'s bounded 5 s wait can return while the watchdog is still
  parked mid-report; wants an orphaned-thread guard.
- `Diagnostics.cpp` has grown past one task's worth of concern (envelope
  writer, IO helpers, snapshot block) and wants extraction into its own
  files.
- The death fixture has no Release-config rows (`MOSAIC_ENABLE_ASSERTS` is
  defined there for Debug/Release/Dist alike, but only Debug is exercised
  by the gate).
- A second `g_handledEvent` correlation id, for the case where a hang report
  and a crash report could otherwise be confused as the same event.
- The `.txt` header has no `envelope : ELIDED` line for the case where a
  full envelope could not fit the arena and was withheld.
- The job-worker stack guarantee (`GuaranteeStackForThisThread`, R23) is
  wired into `JobSystem` and `ServiceThread` but has no test proving a
  worker thread actually survives a near-overflow.
