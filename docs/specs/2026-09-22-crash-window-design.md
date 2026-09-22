# Crash Window Design

**Date:** 2026-09-22

**Status:** Draft -- brainstormed and approved section by section on
2026-09-22; awaiting the written-spec review before the implementation plan.

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

**Exit codes** (a block clear of the hosts' existing 0-5, which the Hub and
the witness harness decode): `10` crashed with a report written, `11` hang
terminated by the reporter (the reporter's `TerminateProcess` argument), `12`
exit sentinel fired. The kind is always in the envelope; the codes only tell
a parent "a report exists". Fail-fasts we cannot catch (§5.3) keep their
NTSTATUS codes.

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
1. `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)`.
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

### 5.2 On a crash

The filter (faulting thread): `g_inCrashHandler` guard as today; store the
`EXCEPTION_POINTERS`, the thread id and the kind in the arena; signal the
crash thread; `WaitForSingleObject` up to 60 s (UE's timeout); then
`TerminateProcess(GetCurrentProcess(), 10)`. Never chains to the previous
filter for our own kinds (that chain is what let WER's dialog appear).

The crash thread, in order, each step independently survivable:
1. Minidump (`MiniDumpWriteDump` with the faulting thread's exception
   pointers, same type flags as today). FIRST, so it survives whatever fails
   next.
2. The portable stack of the faulting thread: `RtlCaptureStackBackTrace` /
   `StackWalk64` addresses resolved to `module+offset` with the module base,
   using the loaded-module list only -- no DbgHelp symbol loading, no
   `SymInitialize`. Unloaded modules marked as such (the minidump carries
   `MiniDumpWithUnloadedModules`).
3. The `.txt` header as today (reason, app, pid, phase, `injected :` line
   from `ForeignModules::LastScan`, exception) plus the portable stack. The
   all-thread symbolized walk is REMOVED from the in-process path; the
   reporter produces it from the minidump.
4. The GPU section provider (DRED, device fault), unchanged, bounded by the
   filter's timeout.
5. The envelope (`.arcdiag`) with the new fields.
6. Log flush with a bounded attempt (§9), then the reporter spawn
   (`CreateProcessW`, detached, no handle kept, static command line + the
   envelope path), then return; the faulting thread wakes and terminates.

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
  `std::current_exception()` is set, its `what()` goes into the reason.
- **SIGABRT** (`signal`): kind `terminate`, reason "abort() called" -- covers
  a third party's `abort()`.
- **Invalid parameter / pure call**: kind `crash`, reason names the CRT
  check and the expression/function the CRT passes.

What cannot be caught in process and is documented as the WER backstop:
`__fastfail` raised by ntdll (heap corruption, `RtlReportCriticalFailure`)
and `/GS` cookie failures. UE cannot either. The dev-setup note enables WER
LocalDumps for the Arcane hosts.

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

### 5.6 Log file sink

`Log::Init` adds a file sink at `<logDir>/<App>.log` (rotated per run,
keep 5) beside the stderr sink, retargeted when the dump dir retargets
(`Saved/Logs/` under a project), `flush_on(warn)`. The envelope's `logPath`
names it; the reporter shows its tail.

### 5.7 Exit sentinel and clean-exit handlers

`Diagnostics::Shutdown` arms an exit sentinel: a detached raw thread that,
if the process has not exited within `Config::exitSeconds` (default 30) of
the host's exit request, writes a `hang` report ("hang at exit") through the
crash thread, spawns the reporter, and terminates with 12. It is the only
thing that can name the Vulkan teardown hang and the module-build join.

`SetConsoleCtrlHandler` (Ctrl+C, console close) and the hosts' window
procedures on `WM_QUERYENDSESSION`/`WM_ENDSESSION` call
`Diagnostics::RequestCleanExit()`, a hook the host installs: the editor
writes autosaves for everything dirty (fast), then runs its ordinary exit.
If the OS cuts it short the autosave marker stays enabled (§8.5), which is
the right answer.

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
write the sibling, exit 0. Gate and CI logs gain readable stacks; the
hand-off path stays exercised on every headless run.

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

- **Crash inside the crash path.** The crash thread's own fault writes
  nothing more; the faulting thread's 60 s wait expires and terminates with
  10. The minidump was written first.
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
   `ArcaneCrashReporter` program, symbolization, unattended mode, staging,
   witness lanes, desk proof.
3. **Autosave and recovery**: the `SaveTo` seam, scheduler, marker, prompt,
   settings, tests.

## 13. Out of scope, owed

- Worker-thread heartbeats and the slow-task scope (introspection arc).
- The general allocator: Core allocation entry, per-module new/delete
  replacement, ABI-gate check, Tracy memory hooks, mimalloc behind the entry
  once measured; Astra slab-provider hook (Astra repo first, then sync).
- A WER runtime-exception module for fail-fasts we cannot catch.
- `CrashReportDocument` reading `.symbolized.txt` and `foreignModules`
  products.
- The Hub decoding exit codes 10/11/12 into a "crashed, report at" row.
- Document autosave beyond the built-in types is covered by the seam; a
  document that cannot serialize to a path must say so in its own spec.
