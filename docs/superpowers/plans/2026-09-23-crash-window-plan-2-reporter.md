# Crash Window Plan 2: Reporter, NativeWindow, Monitor Mode -- Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The reporter process plan 1 hands off to exists: it symbolizes the minidump out of process, shows a plain Win32 window (or runs unattended under a deadline), decides a hang (Keep Waiting / Terminate and Collect), and, pre-launched as a monitor, turns the fail-fasts SEH never sees into an `abnormal-exit` report.

**Architecture:** `Arcane::NativeWindow` (ArcaneCore, `Platform/`) is the thread-owned Win32 window lifted out of the boot splash; the splash becomes a presenter on it. `ArcaneCrashReporter.exe` (new premake `WindowedApp`, links `ArcaneCore.dll` + `dbgeng`/`dbghelp`, never `ArcaneClient`) has pure halves -- argument parser, envelope-to-view model, hang session rule, monitor rule, symbolized-text formatter -- that source-compile into `ArcaneTests`, and thin Win32/dbgeng shells around them. The host side of `Diagnostics` gains the recovered event, the live-reporter guard, the `--host-created` identity, `Config::launchMonitor`, and closes plan 1's three timing seams. It is staged beside every host by each host's post-build, exactly like `ArcaneCore.dll`.

**Tech Stack:** C++23, MSVC v143 `/MD`, Win32 (`CreateWindowExW`, child controls, `CreateProcessW`, named events, `GetProcessTimes`), `dbgeng.h` (`DebugCreate` / `IDebugClient5::OpenDumpFileWide` / `IDebugControl4::GetStackTrace` / `IDebugSymbols3::GetNameByOffsetWide`), Catch2 `[diag]` / `[platform]` / `[reporter]` / `[host]` / `[witness][gpu]` tests, `HostWitness` for process-level tests, premake5.

**Spec:** `docs/specs/2026-09-22-crash-window-design.md` -- §4 (hand-off contract, exit codes), §5.4 (hang protocol), §5.8 (monitor mode), §6 (reporter), §7 (NativeWindow), §9 (edge cases), §10 (tests), §12 item 2, §13 (owed), plus every "(plan 1 as built)" marker. Plan 1's ledger (`.superpowers/sdd/2026-09-22-crash-window-plan-1-core-crash-path/progress.md`, rulings R1-R27) is an input; read R1, R2, R4, R5, R11, R16, R19-R26 before Task 1.

## Global Constraints

- Work in a fresh worktree on branch `feat/crash-window-plan-2` from `main` at `c8a28926` (plan 1 merged). The checkout carries the user's dirty `ArcaneHub/src-tauri/Cargo.toml`, `arccook/src/DdsDump.hpp` and untracked `ReferenceProject/Source/Game/TestComponent.*`, `out.txt`, `out/`, `docs/research/2026-09-17-isometric-survey.md`, `ArcaneAssetPipeline/ArcaneAs.*`, `ArcaneEditor/ArcaneEditor/` -- never `git add -A`, never clean them; name every file you add.
- Build from Git Bash with DASH msbuild switches: `"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m`. Regenerate with `./ThirdParty/premake5/premake5.exe vs2026` whenever a `.cpp` is ADDED or a project is added (premake globs at generate time). The first full Debug build in the worktree takes ~10 min; then `arcbuild build --project ReferenceProject --config Debug` with `ARCANE_SDK` pointed at the worktree, and copy `ReferenceProject/Binaries` into the three staged host slots (`bin/Debug-windows-x86_64-md/{ArcaneEditor,ArcaneRuntime,ArcaneServer}/ReferenceProject/Binaries`) before any witness lane -- the ABI bump in Task 11 requires doing this again.
- Tests run FROM the exe dir: `cd bin/Debug-windows-x86_64-md/ArcaneTests && rm -f imgui.ini && ./ArcaneTests.exe "[diag]"`. ALWAYS print the exit code (`echo "exit $?"`) -- a green summary with a non-zero exit slipped past an implementer in plan 1. Baseline at plan start (main `c8a28926`): `~[gpu]` 1988 cases / 1984 passed / 4 skipped, exit 0; `[diag]` 81; `[witness][gpu]` 6/6; `[witness][server]` 3/3; golden gate 8/8.
- Every new Core symbol is `ARCANE_CORE_API` (`Arcane/Core/Api.hpp`). New exports and the `Config` layout change are ONE ABI bump at the end (Task 11): `kGamePluginABIVersion` 39 -> 40 in `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:937` and `"abi": 40` in `ReferenceProject/ReferenceProject.arcproj:6`.
- Windows-only bodies under `#if defined(_WIN32)`, no-op elsewhere -- `Diagnostics.cpp`'s and `BootSplashWindow.cpp`'s convention. The reporter project itself is emitted only for a Windows target (the `death-fixture` gate).
- The crash-path rules from plan 1 still bind every line that runs on the crash thread: no heap, no loader lock, no spdlog for a FATAL report after this plan (Task 1 seam 3), fixed storage only. The reporter and the monitor are healthy processes and may allocate freely.
- Exit codes: hosts keep `10/11/12/13` (spec §4). The reporter's own: `0` done, `2` bad arguments, `3` envelope unreadable, `4` host identity mismatch (terminate refused), `5` unattended deadline expired with partial output written.
- Tests that need a SPAWNED reporter (`--reporter`, `--monitor`, the witness siblings) `SKIP` when `CI` or `ARCANE_BUILD_MACHINE` is set AND `ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE` is not: spec §6 forbids the spawn on a build machine and `Install` forces `spawnReporter=false` there. The override is UE's `-AllowCrashReportClientOnBuildMachine` (WindowsPlatformCrashContext.cpp:1053) as an environment variable; `Install`'s gate honours it (Task 4), so Jenkins can opt the reporter tests in once the deadline has proven itself. They run on every desk gate regardless.
- Commit after every task, house style: subject `feat(diagnostics|reporter|platform): ...`, body says WHY, ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi`.

## Decisions this plan takes where the spec is silent or plan 1 diverged

- **D1 relaunch line.** It comes from the envelope's `commandLine` (plan 1 as built; §4 "everything else comes from the envelope"). The reporter accepts `--relaunch` only as an override; the hosts never pass it. The monitor reads it from the session record (D15) -- UE keeps `RestartCommandLine` in its crash context file too (CrashReportClient.cpp:204), never on the client's argv -- so nothing is ever escaped for `CommandLineToArgvW`.
- **D2 envelope `reason`.** The reason text (exception code + address, assert expression/file/line, hang duration) is only in the `.txt` today. The envelope gains `reason` (additive, format version unchanged) so the reporter reads one file.
- **D3 program shape.** `ArcaneCrashReporter` is `kind "WindowedApp"` with `wWinMain` (argv from `CommandLineToArgvW`, UTF-16 -> UTF-8). Its pure files source-compile into `ArcaneTests` (the `ArcaneEditor/src/Panels/ConsoleBuffer.cpp` pattern); the dbgeng and Win32 files do not.
- **D4 monitor gate.** `Config::launchMonitor` (default `false`); the editor and the runtime set it to `!headless`, the server never, the death fixture on `--monitor`. `Install` launches the monitor iff `launchMonitor && spawnReporter && !IsDebuggerPresent()`, passing `--unattended` when `Config::unattended` -- an unattended monitor synthesizes the report and shows nothing, which is what makes it testable.
- **D5 symbol path seam.** `--symbol-path "<dirs>"` REPLACES the default search (the reporter's own directory + `_NT_SYMBOL_PATH`) and sets `SYMOPT_IGNORE_CVREC`, so the PDB the linker embedded an absolute path to is NOT consulted. That is what "PDBs hidden" means on the desk that built them.
- **D6 Keep Waiting.** Exits the reporter with 0; the report stays on disk; the host's once-per-stall rule re-arms on progress as before, so a later stall gets a new reporter. Nothing hides and lingers.
- **D7 hang identity.** Every spawn carries `--host-created <u64>` (the host's creation `FILETIME`); the reporter compares it against `GetProcessTimes` on the handle it opened at start before `TerminateProcess`. Pid reuse cannot make it kill a stranger (spec §9).
- **D8 monitor report directory.** The monitor writes into the report directory the session record names -- the `Install`-time default, `<exe dir>/diagnostics` (the record is rewritten on `RetargetDumpDir`, so after a project opens it names `<project>/Saved/Diagnostics`). "A fresh report exists" is checked against that same directory.
- **D9 fatal echo.** For a report with `exitCode != 0` the post-file echo (`Diagnostics: <reason> -- report written` + header + stack, and the hand-off/elision warnings) goes straight to the log file by `WriteFile` append and to the stderr HANDLE -- never through spdlog, whose sink mutex the dead thread may hold. Survivable reports keep `ARC_ERROR`/`ARC_WARN`.
- **D10 hang lever.** `--hang-main N` (dev, Dist-excluded, both hosts, stripped by `SanitizeRelaunchLine`): on frame N the main thread sleeps `kHangMainSeconds = 15` without beating, then continues. It is the witness hang lane's trigger.
- **D11 recovered event.** `Local\Arcane-Recovered-<pid>`, manual-reset, created by the host at `Install`; `ResetEvent` before every SURVIVABLE spawn, `SetEvent` when the main-thread beat resumes after a `hang` report or GPU progress resumes after a `gpu-stall` report; closed at `Shutdown`.
- **D12 one live reporter.** The host keeps the process handle of the reporter spawned for a survivable report; while `WaitForSingleObject(h, 0) == WAIT_TIMEOUT` no second reporter is spawned (the report itself is still written). Fatal spawns close the handle as today.
- **D13 reporter self-protection.** The reporter installs `Diagnostics` with `spawnReporter = false` (that is the mechanism behind "it never spawns itself"), `startHangWatchdog = false`, `dumpDir` = the report's own directory.
- **D14 foreground (UE audit).** The host calls `AllowSetForegroundWindow(reporterPid)` after every spawn (UE: WindowsPlatformCrashContext.cpp:913-915) and the reporter's window calls `SetForegroundWindow` after it shows (UE's `HACK_ForceToFront`, WindowsWindow.cpp:654-657; CrashReportClientApp.cpp:425-427). Without both, the window opens behind the dead host's.
- **D15 session record, not the exit code (UE audit).** UE's monitor does NOT decide "abnormal" from the exit code: the monitored app persists a session record (`%TEMP%/UECrashContext-<pid>.xml`, GenericPlatformCrashContext.cpp:877-880, 949-984) and a shutdown-type property it rewrites at a clean exit; the client treats "no clean shutdown recorded" as abnormal (CrashReportAnalyticsSessionSummary.cpp:238-257, 415-417) and uses the exit code only to NAME the reason (CrashReportClientApp.cpp:1090-1137). Ours: `Install` writes `<reportDir>/<app>-pid<pid>.session` (JSON: pid, app, product, logPath, reportDir, commandLine, hostCreated), `RetargetDumpDir` rewrites it, and `Shutdown()` and the atexit hook delete it. The monitor: record gone -> clean, silent; record present and no fresh report -> `abnormal-exit` named after the code. That is what makes `taskkill /F` (exit code 1, inside the hosts' "known" range) a report, as spec §5.8's "external kill" row requires; the exit-code table is decoration, as in UE.
- **D16 monitor respawn (UE audit).** The first monitor instance relaunches itself with `--respawned` and exits, so its parent is a process that no longer exists and Task Manager's "End process tree" on the host cannot take the monitor with it (UE: WindowsPlatformCrashContext.cpp:643-657). The host keeps no handle to either instance.

---

## File map

| File | Responsibility |
|---|---|
| `docs/specs/2026-09-22-crash-window-design.md` (modify) | Status, §5.6 lead sentence, the plan-2 as-built notes (Task 1, Task 11) |
| `ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp/.cpp` (modify) | `Envelope::reason` |
| `ArcaneCore/src/Arcane/Base/Diagnostics.hpp/.cpp` (modify) | `reason` in the arena JSON; orphan guard; shared submit deadline; fatal echo; recovered event; live-reporter guard; `--host-created`; `Config::launchMonitor` + `LaunchMonitor` |
| `ArcaneCore/src/Arcane/Base/Log.cpp` (modify) | The "EXACTLY ONCE" comment scoped to Log.cpp |
| `ArcaneCore/src/Arcane/Platform/NativeWindow.hpp/.cpp` (create) | Thread-owned Win32 window + `INativeWindowPresenter` |
| `ArcaneClient/src/Arcane/Host/BootSplashWindow.cpp` (modify) | The splash as a presenter on `NativeWindow`; header unchanged |
| `ArcaneCrashReporter/src/ReporterArgs.hpp/.cpp` (create, pure) | Argument parser + exit codes |
| `ArcaneCrashReporter/src/Win32Text.hpp` (create) | UTF-8 <-> UTF-16 helpers for the Win32 files |
| `ArcaneCrashReporter/src/SymbolizedText.hpp/.cpp` (create, pure) | `Symbolized` model, `FormatFrame`, `FormatSymbolized`, `ParseWalkedThreadId` |
| `ArcaneCrashReporter/src/Symbolizer.hpp/.cpp` (create) | dbgeng session: `SymbolizeDump` |
| `ArcaneCrashReporter/src/ReportView.hpp/.cpp` (create, pure) | Envelope -> view model, plain-words kinds, `LastLines`, `DetailsText` |
| `ArcaneCrashReporter/src/LogTail.hpp/.cpp` (create) | `ReadLogTail` (folder `.log.txt` first, else the live file) |
| `ArcaneCrashReporter/src/ReporterWindow.hpp/.cpp` (create) | The presenter: controls, layout, buttons |
| `ArcaneCrashReporter/src/HangSession.hpp/.cpp` (create, pure) | `DecideHang` |
| `ArcaneCrashReporter/src/MonitorRule.hpp/.cpp` (create, pure) | `ClassifyHostExit`, `NtStatusName`, `AbnormalExitReason` |
| `ArcaneCrashReporter/src/Monitor.hpp/.cpp` (create) | The monitor loop + the synthesized report |
| `ArcaneCrashReporter/src/ReporterMain.cpp` (create) | `wWinMain`, mode dispatch, worker + deadline, button actions |
| `premake5.lua` (modify) | `ArcaneCrashReporter` project; staging + `dependson` in ArcaneEditor/ArcaneRuntime/ArcaneServer/death-fixture; ArcaneTests compiles the pure files; ArcaneCore links `user32`/`gdi32` |
| `ArcaneTests/death-fixture/DeathFixtureMain.cpp` (modify) | `--reporter`, `--monitor`, `--attended`, `--die fastfail` |
| `ArcaneClient/src/Arcane/Host/HostConfig.hpp/.cpp` (modify) | `--hang-main`, `kHangMainSeconds`, strip set |
| `ArcaneRuntime/src/RuntimeFrame.hpp/.cpp`, `RuntimeApp.hpp/.cpp`, `ArcaneRuntime/src/main.cpp` (modify) | hang lever; `launchMonitor` |
| `ArcaneEditor/src/App/EditorApp.hpp`, `EditorAppFrame.cpp`, `ArcaneEditor/src/main.cpp` (modify) | hang lever; `launchMonitor` |
| `ArcaneTests/src/NativeWindowTest.cpp`, `ReporterArgsTest.cpp`, `SymbolizedTextTest.cpp`, `ReportViewTest.cpp`, `HangSessionTest.cpp`, `MonitorRuleTest.cpp`, `CrashWitnessTest.cpp` (create); `DiagEnvelopeTest.cpp`, `CrashPathTest.cpp`, `DiagnosticsTest.cpp`, `HostConfigTest.cpp` (modify) | Tests |
| `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`, `ReferenceProject/ReferenceProject.arcproj` (modify) | ABI 40 |

---

### Task 1: Spec pass, the envelope's `reason`, and plan 1's three timing seams

**Files:**
- Modify: `ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp` (struct `Envelope`, after `exitCode`), `DiagEnvelope.cpp` (`Serialize` after `doc["exitCode"]`, `Parse` after the `exitCode` block)
- Modify: `ArcaneCore/src/Arcane/Base/Diagnostics.cpp` (`EnvFields` ~:564, `BuildEnvelopeJson` ~:616, `RunReportOnCrashThread` ~:1010 and ~:1148-1176, `StopWatchdog` :1841, `StartWatchdog` :1824, `SubmitReport` :2461-2513)
- Modify: `ArcaneCore/src/Arcane/Base/Log.cpp:103-105`
- Modify: `docs/specs/2026-09-22-crash-window-design.md` (status block, §5.6 first sentence)
- Test: `ArcaneTests/src/DiagEnvelopeTest.cpp`, `ArcaneTests/src/CrashPathTest.cpp`

**Interfaces:**
- Produces: `Diag::Envelope::reason` (`std::string`, additive, empty when absent); the envelope JSON key `"reason"` written by the crash thread right after `"kind"`; `StopWatchdog` keeps an orphaned watchdog handle and `StartWatchdog` refuses to start beside it; `SubmitReport` spends ONE deadline of `crashHandlingTimeoutSeconds` across the lock and the wait; file-local `FatalEcho(std::initializer_list<std::string_view>)`.

- [ ] **Step 1: Write the failing tests**

`DiagEnvelopeTest.cpp`, after the `logPath/commandLine/exitCode` case:

```cpp
TEST_CASE("arcdiag envelope round-trips reason; an absent key parses as empty", "[diag]")
{
    Arcane::Diag::Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "assert";
    e.reason = "assert: x != nullptr -- boom (MeshCache.cpp:12)";
    const auto back = Arcane::Diag::Parse(Arcane::Diag::Serialize(e));
    REQUIRE(back.has_value());
    CHECK(back->reason == e.reason);

    const std::string legacy = "{\"formatVersion\":1,\"guid\":\"" + Arcane::Guid::Generate().ToString() + "\"}";
    const auto old = Arcane::Diag::Parse(legacy);
    REQUIRE(old.has_value());
    CHECK(old->reason.empty());
}
```

`CrashPathTest.cpp`, in the FIRST case ("a manual report runs on the crash thread ..."), after `CHECK(env->kind == "hang");`:

```cpp
    // Plan 2 (D2): the reason rides in the envelope so the reporter reads one file.
    CHECK(env->reason == "hang (test)");
```

and in "death fixture: an access violation ...", after the `.dmp` check:

```cpp
    // Plan 2 (D9): a FATAL report's echo reaches the log file WITHOUT spdlog.
    // The fixture's logDir is derived: <dumpDir>/../Logs/<appName>.log. This
    // line is green before the change too (spdlog used to carry it); it pins
    // that the direct append lands in the same file.
    const auto log = std::filesystem::temp_directory_path() / "Logs" / "DeathFixture.log";
    REQUIRE(std::filesystem::exists(log));
    CHECK(Slurp(log).find("-- report written") != std::string::npos);
```

- [ ] **Step 2: Run to verify they fail**

Build `ArcaneTests/ArcaneTests.vcxproj` with `-p:BuildProjectReferences=false` for a fast RED. Expected: `'reason': is not a member of 'Arcane::Diag::Envelope'`.

- [ ] **Step 3: The envelope field**

`DiagEnvelope.hpp`, after `int exitCode = 0;`:

```cpp
        // Plan 2 (D2): the report's reason text -- "crash (unhandled exception)",
        // "assert: <expr> -- <msg> (<file>:<line>)", "hang (main thread has not
        // ticked for 12.3s)" -- the same string the .txt header's `reason :`
        // line carries. Additive and optional, format version unchanged.
        std::string reason;
```

`DiagEnvelope.cpp` `Serialize`, after `doc["exitCode"] = envelope.exitCode;`: `doc["reason"] = envelope.reason;`. `Parse`, after the `exitCode` block: `e.reason = StrField(doc, "reason");`.

`Diagnostics.cpp` `EnvFields`: add `const char* reason = nullptr;` after `kind`. `BuildEnvelopeJson`, right after the `"kind"` line: `b.Append(",\n  \"reason\": ");           AppendJsonString(b, SV(f.reason));` (kept in the LEAN form too -- it is a few hundred bytes at most, `kReasonMax`). `RunReportOnCrashThread`, beside `fields.kind = kind;`: `fields.reason = p.reason[0] ? p.reason : "unspecified";`.

- [ ] **Step 4: Seam 1 -- the orphaned watchdog**

Replace `StopWatchdog` and the head of `StartWatchdog`:

```cpp
#if defined(_WIN32)
    // A watchdog that outlived StopWatchdog's bounded wait (parked mid-report
    // on the crash thread). Kept so StartWatchdog cannot start a SECOND thread
    // beside it -- two watchdogs on one g_watchdogThreadId was plan 1's
    // deferred minor -- and closed once it has actually exited.
    HANDLE g_watchdogOrphan = nullptr;
#endif

    void StopWatchdog() noexcept
    {
        g_watchdogStop.store(true, std::memory_order_release);
#if defined(_WIN32)
        if (g_watchdogThread)
        {
            // BOUNDED, unlike the old join (see the plan 1 comment this
            // replaces): five seconds covers a report that is actually
            // writing. Past it the thread is ORPHANED, not abandoned: the
            // handle moves to g_watchdogOrphan and StartWatchdog refuses to
            // run a second watchdog until it is gone.
            if (WaitForSingleObject(g_watchdogThread, 5000) == WAIT_OBJECT_0)
            {
                CloseHandle(g_watchdogThread);
            }
            else
            {
                if (g_watchdogOrphan) CloseHandle(g_watchdogOrphan);
                g_watchdogOrphan = g_watchdogThread;
                std::fprintf(stderr, "Diagnostics: the hang watchdog is still parked mid-report; "
                                     "it will finish on its own\n");
            }
            g_watchdogThread = nullptr;
        }
#else
        if (g_watchdog.joinable()) g_watchdog.join();
#endif
        g_watchdogThreadId.store(0, std::memory_order_release);
    }
```

In `StartWatchdog`, after `if (g_watchdogThread) return;`:

```cpp
        if (g_watchdogOrphan)
        {
            if (WaitForSingleObject(g_watchdogOrphan, 0) != WAIT_OBJECT_0)
            {
                std::fprintf(stderr, "Diagnostics: a previous hang watchdog is still parked; "
                                     "not starting another\n");
                return;
            }
            CloseHandle(g_watchdogOrphan);
            g_watchdogOrphan = nullptr;
        }
```

- [ ] **Step 5: Seam 2 -- one deadline in `SubmitReport`**

Replace the `timeoutMs` computation and its three uses (`:2462-2464`, `:2474`, `:2487`, `:2512`):

```cpp
    const bool fatal = (request.exitCode != 0);
    const std::uint32_t timeoutMs = g_cfg.crashHandlingTimeoutSeconds != 0
                                  ? g_cfg.crashHandlingTimeoutSeconds * 1000u
                                  : 60u * 1000u;
    // ONE deadline for the lock AND the wait (plan 2, seam 2): plan 1 spent
    // the timeout twice in the worst case -- 60 s behind another submitter's
    // lock, then 60 s more on the crash thread -- and the spec's number is 60.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto remainingMs = [&]() noexcept -> DWORD
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        return left > 0 ? static_cast<DWORD>(left) : 0u;
    };
```

then `g_submitMutex.try_lock_until(deadline)` at both lock sites and `WaitForSingleObject(g_handledEvent, remainingMs());` for the wait. `<chrono>` is already included.

- [ ] **Step 6: Seam 3 -- the bounded fatal echo**

Add, in the anonymous namespace beside `DumpBacklog`:

```cpp
    // Plan 2 (D9): a FATAL report's echo may not go through spdlog. The
    // faulting thread may have died holding a sink mutex, and the crash thread
    // would then wedge on ARC_ERROR until the submitter's deadline expired --
    // after every file was already on disk. Straight to the log file (append;
    // spdlog opens it _SH_DENYNO, so a second handle is fine) and to the
    // stderr HANDLE (not the CRT stream, whose lock is the same hazard).
    void FatalEcho(std::initializer_list<std::string_view> parts) noexcept
    {
        HANDLE log = INVALID_HANDLE_VALUE;
        if (g_logPathSnap[0]) log = OpenForWrite(g_logPathSnap, /*append*/true);
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        for (std::string_view part : parts)
        {
            if (log != INVALID_HANDLE_VALUE) WriteAll(log, part);
            if (err && err != INVALID_HANDLE_VALUE) WriteAll(err, part);
        }
        if (log != INVALID_HANDLE_VALUE) { WriteAll(log, "\n"); CloseHandle(log); }
        if (err && err != INVALID_HANDLE_VALUE) WriteAll(err, "\n");
    }
```

In `RunReportOnCrashThread`, wrap the echo block (`ARC_ERROR(... report written ...)` through the elision/withheld `ARC_WARN`s) so that a fatal report uses `FatalEcho` and a survivable one keeps the macros:

```cpp
        const char* const reasonText = p.reason[0] ? p.reason : "report";
        if (p.exitCode != 0)
        {
            FatalEcho({ "Diagnostics: ", reasonText, " -- report written\n", header.View(), section });
            if (!spawnOk)
                FatalEcho({ "Arcane: crash reporter hand-off failed; the report is at ", stem, ".arcdiag" });
            if (textTruncated)
                FatalEcho({ "Diagnostics: the crash arena was exhausted before the text report was built; '",
                            stem, ".txt' is truncated" });
            if (minimalWrite == EnvelopeWrite::WrittenElided || fullWrite == EnvelopeWrite::WrittenElided)
                FatalEcho({ "Diagnostics: '", stem, ".arcdiag' was ELIDED so it would still parse; '",
                            stem, ".txt' carries the full report" });
            if (fullWrite == EnvelopeWrite::NotWritten)
                FatalEcho({ "Diagnostics: the full envelope for '", stem, "' was WITHHELD; the earlier envelope stands" });
        }
        else
        {
            /* the existing ARC_ERROR / fprintf / ARC_WARN block, unchanged */
        }
```

- [ ] **Step 7: The two cosmetic parks**

`Log.cpp:103-105`: change "The engine logger's own sink vector is written EXACTLY ONCE, inside Init()'s call_once, and never again" to "...is written EXACTLY ONCE BY THIS FILE, inside Init()'s call_once: every later attach/detach here happens inside s_distSink ... (EditorApp's Console sink still mutates the vector directly -- owed: move it inside the dist sink.)"

Spec §5.6 first sentence: "`Log::Init` adds a file sink" -> "`Diagnostics::Install` attaches a file sink (through the dist sink `Log::Init` installs)". Spec status block: add "**Plan 2 status:** in progress on `feat/crash-window-plan-2` (reporter + NativeWindow + monitor mode); the envelope gains `reason` (D2); the fatal echo bypasses spdlog (D9); one submit deadline; orphaned-watchdog guard."

- [ ] **Step 8: Build, run, commit**

Full `Arcane.slnx` Debug build (Core changed). Run `[diag]` and `"death fixture*"` from the exe dir; print exit codes. Expected: all pass, `[diag]` count 82.

```bash
git add ArcaneCore/src/Arcane/Base/DiagEnvelope.hpp ArcaneCore/src/Arcane/Base/DiagEnvelope.cpp ArcaneCore/src/Arcane/Base/Diagnostics.cpp ArcaneCore/src/Arcane/Base/Log.cpp docs/specs/2026-09-22-crash-window-design.md ArcaneTests/src/DiagEnvelopeTest.cpp ArcaneTests/src/CrashPathTest.cpp
git commit -m "feat(diagnostics): the envelope carries the reason, a fatal report's echo bypasses spdlog, one submit deadline, and an orphaned watchdog is never doubled (crash window plan 2, task 1)"
```

---

### Task 2: `Arcane::NativeWindow` (spec §7)

**Files:**
- Create: `ArcaneCore/src/Arcane/Platform/NativeWindow.hpp`, `ArcaneCore/src/Arcane/Platform/NativeWindow.cpp` (new directory -- run premake)
- Modify: `premake5.lua` (ArcaneCore's `filter "system:windows"` links: `links { "dbghelp", "user32", "gdi32" }`)
- Test: `ArcaneTests/src/NativeWindowTest.cpp` (new -> run premake)

**Interfaces:**
- Produces (platform-neutral header; HWND/HDC cross as `void*` so no Core header pulls in `windows.h`):

```cpp
#pragma once
#include <Arcane/Core/Api.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace Arcane
{
    struct NativeWindowDesc
    {
        std::wstring  className = L"ArcaneNativeWindow";   // registered once per process per name
        std::wstring  title     = L"Arcane";
        int           width     = 480;
        int           height    = 270;
        bool          popup     = true;      // WS_POPUP (splash); false = WS_OVERLAPPEDWINDOW (reporter)
        bool          topmost   = false;     // WS_EX_TOPMOST
        bool          appWindow = true;      // WS_EX_APPWINDOW: taskbar button + Alt-Tab (else WS_EX_TOOLWINDOW)
        bool          foreground = false;    // SetForegroundWindow after ShowWindow -- the reporter's window may otherwise
                                             // open BEHIND the dead host's (UE's HACK_ForceToFront, WindowsWindow.cpp:654-657;
                                             // the spawning host must AllowSetForegroundWindow us first, see SpawnReporter)
        std::uint32_t backgroundRgb = 0x0D0D0F;   // 0xRRGGBB class brush
    };

    // Called on the WINDOW THREAD only. Every default is a no-op so a presenter
    // implements exactly what it draws.
    class ARCANE_CORE_API INativeWindowPresenter
    {
    public:
        virtual ~INativeWindowPresenter() = default;
        virtual void OnCreate(void* hwnd) { (void)hwnd; }            // create child controls; runs BEFORE the first paint
        virtual void OnPaint(void* hdc, int left, int top, int right, int bottom) { (void)hdc; (void)left; (void)top; (void)right; (void)bottom; }
        virtual void OnCommand(int id) { (void)id; }                  // WM_COMMAND's LOWORD(wParam)
        virtual void OnSize(int width, int height) { (void)width; (void)height; }
        virtual bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t l) { (void)msg; (void)w; (void)l; return false; }   // PostUser's msg (0..255)
        virtual void OnDestroy() {}
    };

    // A plain OS window owned by ITS OWN THREAD: class registration, creation
    // and the blocking GetMessageW loop all run there; every cross-thread call
    // is a posted message; an atomic handle plus an "ever opened" latch make
    // close-after-destroy and close-before-create both safe; Close() waits on
    // the thread. Lifted verbatim from the boot splash (two review rounds of
    // hardening). Windows-only; a no-op elsewhere. NEVER fails the caller: a
    // failed creation leaves IsOpen()/WasEverOpen() false and everything else
    // a silent no-op.
    class ARCANE_CORE_API NativeWindow
    {
    public:
        NativeWindow() noexcept;
        ~NativeWindow();   // Close()
        NativeWindow(const NativeWindow&)            = delete;
        NativeWindow& operator=(const NativeWindow&) = delete;

        // Starts the window thread and returns at once. `presenter` must
        // outlive the window (Close() first, then destroy the presenter).
        // A second Open on an open window is ignored.
        void Open(const NativeWindowDesc& desc, INativeWindowPresenter* presenter) noexcept;

        // Blocks until creation has been ATTEMPTED (succeeded or failed) or
        // the timeout passes; returns IsOpen().
        [[nodiscard]] bool WaitUntilReady(std::uint32_t timeoutMs) noexcept;

        // Posts WM_CLOSE if the window exists, then joins the thread. Never
        // call it FROM the window thread (it joins itself): a presenter that
        // wants to close posts WM_CLOSE to Hwnd() instead.
        void Close() noexcept;
        // Joins without closing: returns when the window is gone (the user
        // closed it, or Close() ran elsewhere).
        void Wait() noexcept;

        [[nodiscard]] bool  IsOpen() const noexcept;
        [[nodiscard]] bool  WasEverOpen() const noexcept;   // monotonic latch, see BootSplashWindow.hpp's contract
        [[nodiscard]] void* Hwnd() const noexcept;          // HWND, or nullptr
        [[nodiscard]] unsigned Dpi() const noexcept;        // GetDpiForWindow; 96 with no window
        [[nodiscard]] bool  OnWindowThread() const noexcept;

        void Invalidate() noexcept;
        void Invalidate(int left, int top, int right, int bottom) noexcept;   // client rect, bErase = FALSE
        void PostUser(unsigned msg, std::uintptr_t w = 0, std::intptr_t l = 0) noexcept;   // -> OnUser(msg, w, l), msg < 256
        void SetTitle(std::wstring title) noexcept;   // marshalled to the window thread

        struct Impl;   // public for the free WndProc in NativeWindow.cpp (same reason as BootSplashWindow::Impl)
    private:
        std::unique_ptr<Impl> m_impl;
    };
}
```

- [ ] **Step 1: Write the failing test**

`ArcaneTests/src/NativeWindowTest.cpp`:

```cpp
// NativeWindow (crash window plan 2, task 2): the thread-owned Win32 window
// the splash and the reporter present on. Behavioural, not visual: creation
// on its own thread, the presenter callbacks, the posted-message seam, and
// BOTH close paths (Close() and the user's Alt+F4).
#if defined(_WIN32)
#include <Arcane/Platform/NativeWindow.hpp>
#include <catch2/catch_test_macros.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace
{
    struct CountingPresenter final : Arcane::INativeWindowPresenter
    {
        std::atomic<int>      creates{0}, destroys{0}, paints{0}, sizes{0};
        std::atomic<unsigned> createThread{0};
        std::atomic<unsigned> lastUserMsg{0};
        std::atomic<std::uintptr_t> lastUserW{0};
        void OnCreate(void*) override { ++creates; createThread.store(GetCurrentThreadId()); }
        void OnPaint(void*, int, int, int, int) override { ++paints; }
        void OnSize(int, int) override { ++sizes; }
        bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t) override { lastUserMsg.store(msg); lastUserW.store(w); return true; }
        void OnDestroy() override { ++destroys; }
    };

    bool PollUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return pred();
    }

    HWND FindByClass(const wchar_t* cls)
    {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr)
        {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return h;
        }
        return nullptr;
    }
}

TEST_CASE("NativeWindow creates on its own thread, runs the presenter, and Close() destroys it once", "[platform]")
{
    CountingPresenter p;
    Arcane::NativeWindow w;
    CHECK_FALSE(w.IsOpen());
    CHECK_FALSE(w.WasEverOpen());
    CHECK(w.Dpi() == 96u);

    Arcane::NativeWindowDesc d;
    d.className = L"ArcaneNativeWindowTest";
    d.title     = L"NativeWindow test";
    w.Open(d, &p);
    REQUIRE(w.WaitUntilReady(5000));
    CHECK(w.IsOpen());
    CHECK(w.WasEverOpen());
    CHECK(w.Hwnd() != nullptr);
    CHECK(p.creates.load() == 1);
    CHECK(p.createThread.load() != GetCurrentThreadId());
    CHECK(w.Dpi() >= 96u);
    CHECK(PollUntil([&] { return p.paints.load() >= 1; }));

    w.PostUser(7, 42, 0);
    CHECK(PollUntil([&] { return p.lastUserMsg.load() == 7u && p.lastUserW.load() == 42u; }));

    w.SetTitle(L"renamed");
    CHECK(PollUntil([&]
    {
        wchar_t buf[64]{}; GetWindowTextW(static_cast<HWND>(w.Hwnd()), buf, 64);
        return std::wstring(buf) == L"renamed";
    }));

    w.Close();
    CHECK_FALSE(w.IsOpen());
    CHECK(w.WasEverOpen());
    CHECK(p.destroys.load() == 1);
    w.Close();   // idempotent
    CHECK(p.destroys.load() == 1);
}

TEST_CASE("NativeWindow: the user's Alt+F4 ends the window thread and Wait() returns", "[platform]")
{
    CountingPresenter p;
    Arcane::NativeWindow w;
    Arcane::NativeWindowDesc d;
    d.className = L"ArcaneNativeWindowTest";
    d.popup = false;   // an overlapped window has the system menu Alt+F4 drives
    w.Open(d, &p);
    REQUIRE(w.WaitUntilReady(5000));

    HWND h = FindByClass(L"ArcaneNativeWindowTest");
    REQUIRE(h != nullptr);
    PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);   // the exact message DefWindowProc synthesises from Alt+F4
    w.Wait();
    CHECK_FALSE(w.IsOpen());
    CHECK(w.WasEverOpen());
    CHECK(w.Hwnd() == nullptr);
    CHECK(p.destroys.load() == 1);
    w.Close();   // after an OS destroy: no join hang, no double destroy
    CHECK(p.destroys.load() == 1);
}

TEST_CASE("NativeWindow: Close() before creation completes and Close() on a never-opened window are both safe", "[platform]")
{
    {
        CountingPresenter p;
        Arcane::NativeWindow w;
        Arcane::NativeWindowDesc d;
        d.className = L"ArcaneNativeWindowTest";
        w.Open(d, &p);
        w.Close();   // no WaitUntilReady: Close must wait for the attempt itself
        CHECK_FALSE(w.IsOpen());
        CHECK(p.creates.load() == p.destroys.load());
    }
    {
        Arcane::NativeWindow w;
        w.Close();
        w.Wait();
        CHECK_FALSE(w.WasEverOpen());
    }
}
#endif
```

- [ ] **Step 2: Run to verify it fails**

Regenerate (`premake5.exe vs2026`), build `ArcaneTests.vcxproj` with `-p:BuildProjectReferences=false`. Expected: `Cannot open include file: 'Arcane/Platform/NativeWindow.hpp'`.

- [ ] **Step 3: Implement `NativeWindow.cpp`**

```cpp
#include <Arcane/Platform/NativeWindow.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <thread>
#endif

namespace Arcane
{
#if defined(_WIN32)
    struct NativeWindow::Impl
    {
        NativeWindowDesc        desc;
        INativeWindowPresenter* presenter = nullptr;
        std::thread             thread;
        std::atomic<HWND>       hwnd{nullptr};
        std::atomic<bool>       open{false};
        std::atomic<bool>       everOpen{false};   // monotonic; stored right after `open`, never cleared
        std::atomic<bool>       ready{false};      // creation ATTEMPTED (either way) -- Close() waits on this, not on hwnd
        std::atomic<DWORD>      threadId{0};
    };

    namespace
    {
        constexpr UINT kMsgUserBase = WM_APP;           // OnUser(msg) <-> WM_APP + msg, msg < 0x100
        constexpr UINT kMsgSetTitle = WM_APP + 0x100;   // lParam = wchar_t[] the window thread frees

        LRESULT CALLBACK NativeProc(HWND h, UINT msg, WPARAM w, LPARAM l)
        {
            // Set right after CreateWindowExW, before OnCreate/ShowWindow; still
            // null-checked because WM_NCCREATE/WM_CREATE arrive inside
            // CreateWindowExW, before the store.
            auto* impl = reinterpret_cast<NativeWindow::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            INativeWindowPresenter* p = impl ? impl->presenter : nullptr;
            switch (msg)
            {
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                if (p) p->OnPaint(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_COMMAND:
                if (p) p->OnCommand(static_cast<int>(LOWORD(w)));
                return 0;
            case WM_SIZE:
                if (p) p->OnSize(static_cast<int>(LOWORD(l)), static_cast<int>(HIWORD(l)));
                return 0;
            case WM_DPICHANGED:
            {
                // Take the suggested rect; the WM_SIZE that follows re-lays out.
                const RECT* r = reinterpret_cast<const RECT*>(l);
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case kMsgSetTitle:
            {
                wchar_t* text = reinterpret_cast<wchar_t*>(l);
                if (text) { SetWindowTextW(h, text); delete[] text; }
                return 0;
            }
            case WM_DESTROY:
                // ANY destroy path (Close() or the OS's Alt+F4 chain) lands
                // here: clear the handle so later posts address nothing.
                if (p) p->OnDestroy();
                if (impl) { impl->hwnd.store(nullptr); impl->open.store(false); }
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            if (msg >= kMsgUserBase && msg < kMsgSetTitle && p &&
                p->OnUser(msg - kMsgUserBase, static_cast<std::uintptr_t>(w), static_cast<std::intptr_t>(l)))
                return 0;
            return DefWindowProcW(h, msg, w, l);
        }

        void WindowThread(NativeWindow::Impl* impl) noexcept
        {
            // The whole body is one try/catch: an exception escaping a
            // std::thread entry is std::terminate, the one outcome strictly
            // worse than "no window".
            try
            {
                impl->threadId.store(GetCurrentThreadId());
                const NativeWindowDesc& d = impl->desc;

                WNDCLASSEXW wc{};
                wc.cbSize        = sizeof(wc);
                wc.lpfnWndProc   = &NativeProc;
                wc.hInstance     = GetModuleHandleW(nullptr);
                wc.hbrBackground = CreateSolidBrush(RGB((d.backgroundRgb >> 16) & 0xFF,
                                                        (d.backgroundRgb >> 8) & 0xFF,
                                                        d.backgroundRgb & 0xFF));
                wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
                wc.lpszClassName = d.className.c_str();
                // A second window of the same class fails registration with
                // ERROR_CLASS_ALREADY_EXISTS and creates fine; a real failure
                // fails CreateWindowExW below for the same reason. Not checked.
                RegisterClassExW(&wc);

                const DWORD style   = d.popup ? WS_POPUP : WS_OVERLAPPEDWINDOW;
                const DWORD exStyle = (d.appWindow ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW)
                                    | (d.topmost ? WS_EX_TOPMOST : 0);
                const int x = (GetSystemMetrics(SM_CXSCREEN) - d.width) / 2;
                const int y = (GetSystemMetrics(SM_CYSCREEN) - d.height) / 2;
                HWND h = CreateWindowExW(exStyle, d.className.c_str(), d.title.c_str(), style,
                                         x, y, d.width, d.height, nullptr, nullptr, wc.hInstance, nullptr);
                if (!h)
                {
                    impl->ready.store(true);
                    impl->ready.notify_all();
                    return;
                }
                SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
                impl->hwnd.store(h);
                impl->open.store(true);
                impl->everOpen.store(true);   // after open, before ready -- the ordering BootSplashPresenter relies on
                if (impl->presenter) impl->presenter->OnCreate(h);   // child controls exist before the first paint
                impl->ready.store(true);
                impl->ready.notify_all();
                ShowWindow(h, SW_SHOW);
                UpdateWindow(h);
                if (d.foreground) SetForegroundWindow(h);   // honoured only if the spawner granted it (AllowSetForegroundWindow)

                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                impl->open.store(false);
            }
            catch (...)
            {
                impl->open.store(false);
                impl->ready.store(true);
                impl->ready.notify_all();
            }
        }
    }

    NativeWindow::NativeWindow() noexcept : m_impl(nullptr) {}
    NativeWindow::~NativeWindow() { Close(); }

    void NativeWindow::Open(const NativeWindowDesc& desc, INativeWindowPresenter* presenter) noexcept
    {
        if (m_impl) return;
        try
        {
            m_impl = std::make_unique<Impl>();
            m_impl->desc      = desc;
            m_impl->presenter = presenter;
            m_impl->thread    = std::thread(&WindowThread, m_impl.get());
        }
        catch (...) { m_impl.reset(); }
    }

    bool NativeWindow::WaitUntilReady(std::uint32_t timeoutMs) noexcept
    {
        if (!m_impl) return false;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!m_impl->ready.load() && GetTickCount64() < deadline) Sleep(2);
        return m_impl->ready.load() && m_impl->open.load();
    }

    void NativeWindow::Close() noexcept
    {
        if (!m_impl) return;
        m_impl->ready.wait(false);   // creation attempted -- see BootSplashWindow's Impl::ready archaeology
        if (HWND h = m_impl->hwnd.exchange(nullptr))
            PostMessageW(h, WM_CLOSE, 0, 0);
        if (m_impl->thread.joinable()) m_impl->thread.join();
        m_impl->open.store(false);
    }

    void NativeWindow::Wait() noexcept
    {
        if (!m_impl) return;
        if (m_impl->thread.joinable()) m_impl->thread.join();
    }

    bool     NativeWindow::IsOpen() const noexcept      { return m_impl && m_impl->open.load(); }
    bool     NativeWindow::WasEverOpen() const noexcept { return m_impl && m_impl->everOpen.load(); }
    void*    NativeWindow::Hwnd() const noexcept        { return m_impl ? m_impl->hwnd.load() : nullptr; }
    bool     NativeWindow::OnWindowThread() const noexcept { return m_impl && m_impl->threadId.load() == GetCurrentThreadId(); }
    unsigned NativeWindow::Dpi() const noexcept
    {
        const HWND h = static_cast<HWND>(Hwnd());
        return h ? GetDpiForWindow(h) : 96u;
    }
    void NativeWindow::Invalidate() noexcept
    {
        if (HWND h = static_cast<HWND>(Hwnd())) InvalidateRect(h, nullptr, FALSE);
    }
    void NativeWindow::Invalidate(int left, int top, int right, int bottom) noexcept
    {
        if (HWND h = static_cast<HWND>(Hwnd())) { RECT r{ left, top, right, bottom }; InvalidateRect(h, &r, FALSE); }
    }
    void NativeWindow::PostUser(unsigned msg, std::uintptr_t w, std::intptr_t l) noexcept
    {
        if (msg >= 0x100) return;
        if (HWND h = static_cast<HWND>(Hwnd())) PostMessageW(h, kMsgUserBase + msg, static_cast<WPARAM>(w), static_cast<LPARAM>(l));
    }
    void NativeWindow::SetTitle(std::wstring title) noexcept
    {
        HWND h = static_cast<HWND>(Hwnd());
        if (!h) return;
        try
        {
            wchar_t* copy = new wchar_t[title.size() + 1];
            std::wmemcpy(copy, title.c_str(), title.size() + 1);
            if (!PostMessageW(h, kMsgSetTitle, 0, reinterpret_cast<LPARAM>(copy))) delete[] copy;
        }
        catch (...) {}
    }
#else
    struct NativeWindow::Impl {};
    NativeWindow::NativeWindow() noexcept : m_impl(nullptr) {}
    NativeWindow::~NativeWindow() = default;
    void NativeWindow::Open(const NativeWindowDesc&, INativeWindowPresenter*) noexcept {}
    bool NativeWindow::WaitUntilReady(std::uint32_t) noexcept { return false; }
    void NativeWindow::Close() noexcept {}
    void NativeWindow::Wait() noexcept {}
    bool NativeWindow::IsOpen() const noexcept { return false; }
    bool NativeWindow::WasEverOpen() const noexcept { return false; }
    void* NativeWindow::Hwnd() const noexcept { return nullptr; }
    unsigned NativeWindow::Dpi() const noexcept { return 96u; }
    bool NativeWindow::OnWindowThread() const noexcept { return false; }
    void NativeWindow::Invalidate() noexcept {}
    void NativeWindow::Invalidate(int, int, int, int) noexcept {}
    void NativeWindow::PostUser(unsigned, std::uintptr_t, std::intptr_t) noexcept {}
    void NativeWindow::SetTitle(std::wstring) noexcept {}
#endif
}
```

`premake5.lua`, ArcaneCore's Windows filter: `links { "dbghelp", "user32", "gdi32" }` with a comment "user32/gdi32: Platform/NativeWindow.cpp (crash window plan 2) -- explicit rather than inherited from the VS default list".

- [ ] **Step 4: Run to verify it passes**

Full Debug build (Core changed). `./ArcaneTests.exe "[platform]"` -> 3 cases pass, exit 0. Then `"[boot]"` (untouched, must still pass) and `"[diag]"`.

- [ ] **Step 5: Commit**

```bash
git add ArcaneCore/src/Arcane/Platform/NativeWindow.hpp ArcaneCore/src/Arcane/Platform/NativeWindow.cpp ArcaneTests/src/NativeWindowTest.cpp premake5.lua
git commit -m "feat(platform): Arcane::NativeWindow -- the thread-owned Win32 window lifted from the boot splash, with a presenter seam (crash window plan 2, task 2)"
```

---

### Task 3: The boot splash becomes a presenter on `NativeWindow` (spec §7)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/BootSplashWindow.cpp` (whole Windows body; the `#else` stubs stay)
- Unchanged: `ArcaneClient/src/Arcane/Host/BootSplashWindow.hpp` (public API, `struct Impl;` forward declaration, `BootSplashPresenter`)
- Test: `ArcaneTests/src/BootSplashPresenterTest.cpp` and `BootStageParityTest.cpp` are the proof and are NOT modified -- they must pass as they are, including `FindSplashHwnd`'s `L"ArcaneBootSplash"` class name and the Alt+F4 chain.

**Interfaces:**
- Consumes: `Arcane::NativeWindow`, `INativeWindowPresenter`, `NativeWindowDesc` (Task 2).
- Produces: nothing new. `BootSplashWindow::Impl` now derives from `INativeWindowPresenter` and owns a `NativeWindow`.

- [ ] **Step 1: Confirm the existing tests are the failing gate**

There is no new behaviour, so the RED is structural: after the rewrite, `"[boot]"` must be green with zero test edits. Run `./ArcaneTests.exe "[boot]"` on the Task 2 head first and record the count (expected: all pass) -- that is the number to match.

- [ ] **Step 2: Rewrite the Windows body**

Keep the file's includes, `Utf8ToWide`, `ResolveImagePathWide`, `kTextRowHeightPx` and the body of `PaintSplash` (its signature changes to take the presenter). Replace everything from `struct BootSplashWindow::Impl` through the end of the `#if defined(_WIN32)` section:

```cpp
#include <Arcane/Platform/NativeWindow.hpp>   // add to the include block, after Log.hpp

namespace Arcane
{
#if defined(_WIN32)
    namespace
    {
        // NativeWindow::PostUser ids (OnUser's `msg`), handled on the window thread.
        constexpr unsigned kUserSetProgress = 1;   // wParam = integer percent
        constexpr unsigned kUserLoadImage   = 2;   // posted from OnCreate so the decode runs AFTER the first paint

        constexpr LONG kTextRowHeightPx = 24;
        /* Utf8ToWide, ResolveImagePathWide: unchanged */
    }

    // The splash IS a presenter now (spec S7): NativeWindow owns the thread,
    // the class, the message loop and the close rules; this owns only what the
    // splash draws -- the image, the status line, the taskbar progress -- and
    // the window-thread-only resources behind them (GDI+, COM).
    struct BootSplashWindow::Impl final : INativeWindowPresenter
    {
        NativeWindow      window;
        std::string       imagePath;
        std::mutex        textMutex;     // statusText: written by any thread, read by OnPaint
        std::string       statusText;
        std::atomic<bool> showProgress{true};
        int               lastPercent = -1;   // SetProgress dedupe (boot/main thread only)

        // Window-thread-owned, acquired in OnCreate, released in OnDestroy.
        std::unique_ptr<Gdiplus::Bitmap>      bitmap;
        ULONG_PTR                             gdiplusToken   = 0;
        bool                                  gdiplusOk      = false;
        Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
        bool                                  comInitialized = false;

        void OnCreate(void*) override
        {
            Gdiplus::GdiplusStartupInput in;
            gdiplusOk      = Gdiplus::GdiplusStartup(&gdiplusToken, &in, nullptr) == Gdiplus::Ok;
            comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            // The image decodes AFTER the window is up and painted once:
            // NativeWindow shows the window right after OnCreate returns and
            // UpdateWindow paints synchronously, so this posted message is
            // dequeued only after that first paint -- the "~100 ms to
            // something on screen" promise is about the WINDOW, not the image.
            window.PostUser(kUserLoadImage);
        }

        void OnPaint(void* hdc, int left, int top, int right, int bottom) override
        {
            const RECT paint{ left, top, right, bottom };
            PaintSplash(static_cast<HWND>(window.Hwnd()), *this, static_cast<HDC>(hdc), paint);
        }

        bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t) override
        {
            if (msg == kUserLoadImage)
            {
                if (gdiplusOk && !imagePath.empty())
                {
                    const std::wstring wpath = ResolveImagePathWide(imagePath);
                    auto bmp = std::make_unique<Gdiplus::Bitmap>(wpath.c_str());
                    if (bmp->GetLastStatus() == Gdiplus::Ok)
                    {
                        bitmap = std::move(bmp);
                        window.Invalidate();
                    }
                    // else: missing/corrupt/unreadable -> the class brush stays the whole splash
                }
                return true;
            }
            if (msg == kUserSetProgress)
            {
                if (!taskbar && comInitialized)
                {
                    Microsoft::WRL::ComPtr<ITaskbarList3> tbl;
                    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tbl))) &&
                        SUCCEEDED(tbl->HrInit()))
                        taskbar = tbl;
                }
                if (taskbar)
                {
                    const HWND h = static_cast<HWND>(window.Hwnd());
                    const int percent = static_cast<int>(w);
                    if (percent >= 100) taskbar->SetProgressState(h, TBPF_NOPROGRESS);   // 100% clears the overlay (WindowsPlatformSplash.cpp:769-781)
                    else                taskbar->SetProgressValue(h, static_cast<ULONGLONG>(percent), 100ULL);
                }
                return true;
            }
            return false;
        }

        void OnDestroy() override
        {
            taskbar.Reset();
            if (comInitialized) CoUninitialize();
            bitmap.reset();
            if (gdiplusOk) Gdiplus::GdiplusShutdown(gdiplusToken);
        }
    };
```

`PaintSplash`'s signature becomes `void PaintSplash(HWND h, BootSplashWindow::Impl& impl, HDC hdc, const RECT& paintRect)` -- exactly what it was; it reads `impl.bitmap`, `impl.textMutex`, `impl.statusText` as before. It must be declared after `Impl` is complete (move it below the struct, or forward-declare `struct BootSplashWindow::Impl;` above it as today).

The public methods:

```cpp
    BootSplashWindow::BootSplashWindow(const char* imagePath) noexcept
        : m_impl(nullptr)
    {
        try
        {
            m_impl = std::make_unique<Impl>();
            m_impl->imagePath  = imagePath ? imagePath : "";
            m_impl->statusText = "Loading...";   // seeded BEFORE the thread exists (the comment archaeology from before stays)

            NativeWindowDesc d;
            d.className     = L"ArcaneBootSplash";   // BootSplashPresenterTest finds the window by this name
            d.title         = L"Arcane";
            d.width         = 480;
            d.height        = 270;
            d.popup         = true;
            d.topmost       = true;
            d.appWindow     = true;                  // taskbar button, so ITaskbarList3 has somewhere to draw (WindowsPlatformSplash.cpp:451-452)
            d.backgroundRgb = 0x0D0D0F;              // RGB(13, 13, 15), the brush PaintSplash reads back via GCLP_HBRBACKGROUND
            m_impl->window.Open(d, m_impl.get());
        }
        catch (...) { m_impl.reset(); }   // never fail boot for a splash
    }

    void BootSplashWindow::Close() noexcept          { if (m_impl) m_impl->window.Close(); }
    bool BootSplashWindow::IsOpen() const noexcept   { return m_impl && m_impl->window.IsOpen(); }
    bool BootSplashWindow::WasEverOpen() const noexcept { return m_impl && m_impl->window.WasEverOpen(); }

    void BootSplashWindow::SetStatusText(std::string text) noexcept
    {
        if (!m_impl) return;
        bool changed = false;
        try
        {
            std::lock_guard<std::mutex> lk(m_impl->textMutex);
            if (m_impl->statusText == text) return;   // dedupe: ~125 calls/s with the same stageId while a Worker stage overlaps
            m_impl->statusText = std::move(text);
            changed = true;
        }
        catch (...) { return; }
        if (!changed) return;
        if (HWND h = static_cast<HWND>(m_impl->window.Hwnd()))
        {
            RECT client{};
            GetClientRect(h, &client);
            m_impl->window.Invalidate(client.left, client.bottom - kTextRowHeightPx, client.right, client.bottom);   // the text row only
        }
    }

    void BootSplashWindow::SetProgress(float fraction01) noexcept
    {
        if (!m_impl || !m_impl->window.Hwnd()) return;
        const float clamped = fraction01 < 0.0f ? 0.0f : (fraction01 > 1.0f ? 1.0f : fraction01);
        const int percent = static_cast<int>(clamped * 100.0f + 0.5f);
        if (m_impl->lastPercent == percent) return;
        m_impl->lastPercent = percent;
        m_impl->window.PostUser(kUserSetProgress, static_cast<std::uintptr_t>(percent));   // COM belongs to the window thread
    }

    void BootSplashWindow::SetShowProgress(bool show) noexcept { if (m_impl) m_impl->showProgress.store(show); }
    bool BootSplashWindow::ShowProgress() const noexcept        { return !m_impl || m_impl->showProgress.load(); }
    BootSplashWindow::~BootSplashWindow() { Close(); }
```

Delete `SplashProc`, the old thread lambda and `kMsgSetProgress`. Carry the explanatory comments that still apply (the everOpen ordering note now lives in NativeWindow.cpp; leave a one-line pointer). Keep `#pragma comment(lib, "gdiplus.lib")`.

- [ ] **Step 3: Build and run**

Full Debug build (ArcaneClient changed). `./ArcaneTests.exe "[boot]"` -> same case count as Step 1, all pass, exit 0. Desk check: `bin/Debug-windows-x86_64-md/ArcaneEditor/ArcaneEditor.exe --project ReferenceProject --backend dx12` -- the splash appears with the logo and the status line, the taskbar shows progress, the editor reveals and the splash closes. Close the splash with Alt+F4 during boot: the editor exits (quit-during-boot contract).

- [ ] **Step 4: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/BootSplashWindow.cpp
git commit -m "refactor(host): the boot splash presents on Arcane::NativeWindow -- image, status line and taskbar progress only; thread, class and close rules live in Core (crash window plan 2, task 3)"
```

---

### Task 4: The reporter program -- project, argument parser, the fallback sibling, staging, `--reporter` (spec §6, §12 item 2)

**Files:**
- Create: `ArcaneCrashReporter/src/ReporterArgs.hpp`, `ReporterArgs.cpp` (pure), `ArcaneCrashReporter/src/Win32Text.hpp`, `ArcaneCrashReporter/src/ReporterMain.cpp`
- Modify: `premake5.lua` (new project after `death-fixture`; staging lines + gated `dependson` in ArcaneEditor/ArcaneRuntime/ArcaneServer/death-fixture; ArcaneTests `files` + `includedirs`)
- Modify: `ArcaneTests/death-fixture/DeathFixtureMain.cpp` (`--reporter`, `--attended`)
- Test: `ArcaneTests/src/ReporterArgsTest.cpp` (new), `ArcaneTests/src/CrashPathTest.cpp` (`--reporter` case + helpers)

**Interfaces:**
- Consumes: `Diag::ReadFile`, `Envelope::reason` (Task 1), `Diagnostics::Install`.
- Produces:

```cpp
// ArcaneCrashReporter/src/ReporterArgs.hpp -- pure, std-only, compiled into ArcaneTests.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Arcane::Reporter
{
    namespace ExitCode
    {
        inline constexpr int kOk           = 0;
        inline constexpr int kBadArgs      = 2;
        inline constexpr int kNoEnvelope   = 3;
        inline constexpr int kHostMismatch = 4;   // --host-created did not match: terminate refused
        inline constexpr int kDeadline     = 5;   // unattended deadline expired; partial sibling written
    }

    struct Args
    {
        enum class Mode { Report, Monitor };
        Mode          mode = Mode::Report;
        std::string   envelopePath;          // Report: the positional <path.arcdiag>
        std::uint32_t pid = 0;               // Report: --pid; Monitor: --monitor <pid>
        std::string   kind;                  // --kind (informational; the envelope decides)
        std::string   product;               // --product "<name>"  (window title)
        std::string   app;                   // --app "<appName>"   (monitor: report stem)
        bool          unattended = false;    // --unattended
        std::string   recoveredEvent;        // --recovered-event <name>   (hang protocol)
        std::string   relaunch;              // --relaunch "<line>"        (D1: overrides the envelope's commandLine)
        std::uint64_t hostCreated = 0;       // --host-created <u64>       (D7)
        std::string   sessionPath;           // --session <file>           (monitor, REQUIRED: the host's session record, D15)
        std::string   logPath;               // --log <path>               (monitor, optional override of the record)
        std::string   reportDir;             // --report-dir <dir>         (monitor, optional override of the record)
        std::string   symbolPath;            // --symbol-path "<a;b>"      (D5 test seam)
        std::uint32_t deadlineSeconds = 60;  // --deadline <s>             (unattended; tests lower it)
        bool          respawned = false;     // --respawned                (monitor: the second instance, D16)
    };

    struct ParseResult
    {
        std::optional<Args> args;
        std::string         error;   // one line, empty on success
    };

    // argv WITHOUT the program name. Report mode needs the positional envelope
    // path; Monitor mode needs --monitor <pid> and --report-dir. Unknown flags,
    // a missing value, or a non-numeric number are errors.
    [[nodiscard]] ParseResult ParseArgs(std::span<const std::string> argv);
    [[nodiscard]] std::string Usage();
}
```

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/ReporterArgsTest.cpp`:

```cpp
#include "ReporterArgs.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace Arcane::Reporter;

TEST_CASE("reporter args: the exact crash hand-off line plan 1 emits parses into Report mode", "[reporter]")
{
    const std::vector<std::string> argv = {
        "D:/p/diagnostics/ArcaneEditor-20260923-101112-pid4242.arcdiag",
        "--pid", "4242", "--kind", "crash", "--product", "Arcane Editor", "--unattended",
        "--host-created", "133700000000000000",
    };
    const ParseResult r = ParseArgs(argv);
    REQUIRE(r.error.empty());
    REQUIRE(r.args.has_value());
    CHECK(r.args->mode == Args::Mode::Report);
    CHECK(r.args->envelopePath == argv[0]);
    CHECK(r.args->pid == 4242u);
    CHECK(r.args->kind == "crash");
    CHECK(r.args->product == "Arcane Editor");
    CHECK(r.args->unattended);
    CHECK(r.args->hostCreated == 133700000000000000ull);
    CHECK(r.args->deadlineSeconds == 60u);
    CHECK(r.args->recoveredEvent.empty());
}

TEST_CASE("reporter args: the hang line carries the recovered event; the monitor line selects Monitor mode", "[reporter]")
{
    const ParseResult hang = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "7", "--kind", "hang", "--product", "P",
        "--recovered-event", "Local\\Arcane-Recovered-7", "--relaunch", "P.exe --project X" });
    REQUIRE(hang.args.has_value());
    CHECK(hang.args->recoveredEvent == "Local\\Arcane-Recovered-7");
    CHECK(hang.args->relaunch == "P.exe --project X");

    const ParseResult mon = ParseArgs(std::vector<std::string>{
        "--monitor", "99", "--session", "D:/x/diagnostics/ArcaneEditor-pid99.session", "--unattended" });
    REQUIRE(mon.error.empty());
    REQUIRE(mon.args.has_value());
    CHECK(mon.args->mode == Args::Mode::Monitor);
    CHECK(mon.args->pid == 99u);
    CHECK(mon.args->sessionPath == "D:/x/diagnostics/ArcaneEditor-pid99.session");
    CHECK(mon.args->unattended);
}

TEST_CASE("reporter args: test seams and refusals", "[reporter]")
{
    const ParseResult seams = ParseArgs(std::vector<std::string>{
        "r.arcdiag", "--pid", "0", "--symbol-path", "D:/a;D:/b", "--deadline", "10" });
    REQUIRE(seams.args.has_value());
    CHECK(seams.args->symbolPath == "D:/a;D:/b");
    CHECK(seams.args->deadlineSeconds == 10u);

    CHECK_FALSE(ParseArgs(std::vector<std::string>{}).args.has_value());                              // no envelope, no --monitor
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid" }).args.has_value());        // missing value
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "x" }).args.has_value());   // not a number
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--bogus" }).args.has_value());      // unknown flag
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "--monitor", "5" }).args.has_value());            // monitor without --session
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "a.arcdiag", "b.arcdiag" }).args.has_value());    // two positionals
    CHECK_FALSE(ParseArgs(std::vector<std::string>{ "r.arcdiag", "--pid", "x" }).error.empty());
    CHECK(Usage().find("--monitor") != std::string::npos);
}
```

`CrashPathTest.cpp`, replace the file-local helpers block (`RunFixture`) with this superset and add the case:

```cpp
#include <chrono>
#include <cstdlib>
#include <thread>

namespace
{
    // Spec s6: no reporter spawn on a build machine (Install forces
    // spawnReporter=false there), so every case that needs a SPAWNED reporter
    // skips -- the desk gate is where these run.
    void SkipIfBuildMachine()
    {
        if ((std::getenv("CI") || std::getenv("ARCANE_BUILD_MACHINE")) && !std::getenv("ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
            SKIP("CI/ARCANE_BUILD_MACHINE set -- the reporter is never spawned on a build machine (spec s6; "
                 "ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE overrides)");
    }

    // The newest .arcdiag stem in `dir`, polling up to `timeout` -- a
    // detached reporter or monitor writes AFTER the fixture has exited.
    std::filesystem::path WaitForStem(const std::filesystem::path& dir, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::filesystem::path found;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".arcdiag") found = e.path().parent_path() / e.path().stem();
            if (!found.empty() || std::chrono::steady_clock::now() >= deadline) return found;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool WaitForFile(const std::filesystem::path& p, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!std::filesystem::exists(p) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return std::filesystem::exists(p);
    }

    struct FixtureRun { Arcane::Test::WitnessRun run; std::filesystem::path stem; std::filesystem::path dir; };
    FixtureRun RunFixture(const char* mode, std::vector<std::string> extra = {})
    {
        const auto exe = std::filesystem::absolute("../death-fixture/death-fixture.exe");
        REQUIRE(std::filesystem::exists(exe));
        std::string tag = mode;
        for (const auto& e : extra) if (e.rfind("--", 0) == 0) tag += "-" + e.substr(2);
        const auto dir = std::filesystem::temp_directory_path() / ("arcane-death-" + tag);
        std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
        std::vector<std::string> args = { "--dir", dir.string(), "--die", mode };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 30000;
        FixtureRun out{ Arcane::Test::RunWitness(inv), {}, dir };
        out.stem = WaitForStem(dir, std::chrono::milliseconds(0));
        return out;
    }
}

// The hand-off, end to end (spec s6): the fixture crashes, the crash thread
// spawns the STAGED reporter beside the fixture, the reporter reads the
// envelope and writes <stem>.symbolized.txt after the host is already dead.
TEST_CASE("death fixture --reporter: a crash report gains a .symbolized.txt from the detached reporter", "[diag]")
{
    SkipIfBuildMachine();
    REQUIRE(std::filesystem::exists(std::filesystem::absolute("../death-fixture/ArcaneCrashReporter.exe")));
    const FixtureRun r = RunFixture("av", { "--reporter" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    const auto sibling = r.stem.string() + ".symbolized.txt";
    REQUIRE(WaitForFile(sibling, std::chrono::seconds(20)));
    const std::string text = Slurp(sibling);
    CHECK(text.find("symbolized by ArcaneCrashReporter") != std::string::npos);
    CHECK(text.find("thread") != std::string::npos);
}
```

The `tag` derivation keeps each variant in its own directory (`arcane-death-av-reporter`), so a `--reporter` run never reads a plain run's stem.

- [ ] **Step 2: Run to verify they fail**

Regenerate + build ArcaneTests only. Expected: `Cannot open include file: 'ReporterArgs.hpp'` (the premake edit in Step 3 adds the include dir).

- [ ] **Step 3: premake -- the project, the staging, the test compile**

After the `death-fixture` block (inside its own `if os.target() == "windows" then ... end`):

```lua
-- ============================================================================
-- ArcaneCrashReporter (crash window plan 2; spec s6): the out-of-process crash
-- reporter every host hands off to. WindowedApp (no console; wWinMain), links
-- ArcaneCore ONLY -- never ArcaneClient, no GPU, no ImGui -- plus dbgeng/
-- dbghelp for out-of-process symbolization. STAGED beside every host by that
-- host's own postbuild, exactly like ArcaneCore.dll (spec s12 item 2), which
-- is why each host `dependson` it. Its pure files (ReporterArgs, ReportView,
-- SymbolizedText, HangSession, MonitorRule) are ALSO source-compiled into
-- ArcaneTests ([reporter]); the dbgeng/Win32 files are not.
-- ============================================================================
if os.target() == "windows" then
project "ArcaneCrashReporter"
    location "ArcaneCrashReporter"
    kind "WindowedApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files { "%{prj.location}/src/**.hpp", "%{prj.location}/src/**.cpp" }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.spdlog}",     -- Log.hpp
        "%{IncludeDir.Mosaic}",     -- Assert.hpp
        "%{IncludeDir.nlohmann}",   -- Monitor.cpp reads the host's session record (task 9)
    }

    links { "ArcaneCore", "dbgeng", "dbghelp", "user32", "gdi32", "shell32", "ole32" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
    }

    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneCore/ArcaneCore.dll" "%{cfg.buildtarget.directory}/ArcaneCore.dll"',
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
        fatalwarnings { "4715" }
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
end   -- ArcaneCrashReporter: Windows target only
```

Staging -- add ONE line to each of ArcaneEditor's, ArcaneRuntime's, ArcaneServer's and death-fixture's `postbuildcommands`, right after their `ArcaneCore.dll` copy:

```lua
        -- Crash window plan 2 (spec s12 item 2): the reporter this host hands off to lives beside it.
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneCrashReporter/ArcaneCrashReporter.exe" "%{cfg.buildtarget.directory}/ArcaneCrashReporter.exe"',
```

and the build order, gated like `death-fixture`'s own dependency in ArcaneTests: in ArcaneEditor, ArcaneRuntime and ArcaneServer add

```lua
    if os.target() == "windows" then
        dependson { "ArcaneCrashReporter" }   -- staged by the postbuild above; emitted for a Windows target only
    end
```

and in `death-fixture` (already inside the gate) `dependson { "ArcaneCrashReporter" }`. ArcaneTests' gated line becomes `dependson { "arcbuild-process-fixture", "death-fixture", "ArcaneCrashReporter" }` -- its `reporter:` cases run `../ArcaneCrashReporter/ArcaneCrashReporter.exe` directly (Task 5).

ArcaneTests: `includedirs` gains `"%{wks.location}/ArcaneCrashReporter/src",` and `files` gains (with a comment in the file's own style: pure halves of the reporter, compiled here so `[reporter]` drives them; `ReporterMain.cpp`/`Symbolizer.cpp`/`ReporterWindow.cpp`/`Monitor.cpp` are NOT compiled -- they carry wWinMain, dbgeng and the window):

```lua
        "%{wks.location}/ArcaneCrashReporter/src/ReporterArgs.cpp",
```

(Tasks 5, 6, 8, 9 add `SymbolizedText.cpp`, `ReportView.cpp`, `HangSession.cpp`, `MonitorRule.cpp` to this list.)

- [ ] **Step 4: `ReporterArgs.cpp`**

```cpp
#include "ReporterArgs.hpp"
#include <charconv>

namespace Arcane::Reporter
{
    namespace
    {
        template <typename T>
        bool ParseNumber(const std::string& s, T& out)
        {
            const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc{} && r.ptr == s.data() + s.size();
        }
    }

    std::string Usage()
    {
        return "ArcaneCrashReporter <report.arcdiag> --pid <n> [--kind <k>] [--product \"<name>\"] [--app <name>]\n"
               "                    [--unattended] [--recovered-event <name>] [--relaunch \"<line>\"]\n"
               "                    [--host-created <u64>] [--symbol-path \"<dir;dir>\"] [--deadline <s>]\n"
               "ArcaneCrashReporter --monitor <pid> --session <file> [--unattended]\n"
               "                    [--product \"<name>\"] [--app <name>] [--log <path>] [--report-dir <dir>]\n";
    }

    ParseResult ParseArgs(std::span<const std::string> argv)
    {
        Args a;
        bool haveMonitor = false;
        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            const std::string& s = argv[i];
            auto value = [&](std::string& out) -> bool
            {
                if (i + 1 >= argv.size()) return false;
                out = argv[++i];
                return true;
            };
            std::string v;
            if (s.rfind("--", 0) != 0)
            {
                if (!a.envelopePath.empty()) return { std::nullopt, "two positional arguments: '" + a.envelopePath + "' and '" + s + "'" };
                a.envelopePath = s;
                continue;
            }
            if (s == "--unattended") { a.unattended = true; continue; }
            if (s == "--respawned")  { a.respawned = true; continue; }
            if (!value(v)) return { std::nullopt, s + " needs a value" };
            if      (s == "--pid")             { if (!ParseNumber(v, a.pid)) return { std::nullopt, "--pid is not a number: " + v }; }
            else if (s == "--monitor")         { if (!ParseNumber(v, a.pid)) return { std::nullopt, "--monitor is not a pid: " + v }; haveMonitor = true; }
            else if (s == "--kind")            a.kind = v;
            else if (s == "--product")         a.product = v;
            else if (s == "--app")             a.app = v;
            else if (s == "--recovered-event") a.recoveredEvent = v;
            else if (s == "--relaunch")        a.relaunch = v;
            else if (s == "--host-created")    { if (!ParseNumber(v, a.hostCreated)) return { std::nullopt, "--host-created is not a number: " + v }; }
            else if (s == "--session")         a.sessionPath = v;
            else if (s == "--log")             a.logPath = v;
            else if (s == "--report-dir")      a.reportDir = v;
            else if (s == "--symbol-path")     a.symbolPath = v;
            else if (s == "--deadline")        { if (!ParseNumber(v, a.deadlineSeconds)) return { std::nullopt, "--deadline is not a number: " + v }; }
            else return { std::nullopt, "unknown argument: " + s };
        }
        if (haveMonitor)
        {
            a.mode = Args::Mode::Monitor;
            if (a.sessionPath.empty()) return { std::nullopt, "--monitor needs --session" };
            if (!a.envelopePath.empty()) return { std::nullopt, "--monitor takes no envelope path" };
        }
        else if (a.envelopePath.empty())
        {
            return { std::nullopt, "missing <report.arcdiag> (or --monitor <pid>)" };
        }
        return { a, "" };
    }
}
```

- [ ] **Step 5: `Win32Text.hpp` and `ReporterMain.cpp`**

`Win32Text.hpp` (Windows-only inline helpers shared by every Win32 file of the reporter):

```cpp
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <string_view>

namespace Arcane::Reporter
{
    inline std::wstring ToWide(std::string_view s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n > 0 ? static_cast<std::size_t>(n) : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    inline std::string ToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
}
```

`ReporterMain.cpp` (this task's shape; Tasks 5, 7, 8, 9 extend the marked points):

```cpp
// ArcaneCrashReporter -- the out-of-process crash reporter (spec s6).
// Report mode: read the envelope the host wrote, symbolize its minidump out of
// process, write <stem>.symbolized.txt, then show the window (or exit,
// unattended). Monitor mode (task 9): wait on a host pid and turn an
// unrecognised exit code into an abnormal-exit report.
#include "ReporterArgs.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>

#include <shellapi.h>   // CommandLineToArgvW

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    using namespace Arcane::Reporter;

    std::vector<std::string> ArgvUtf8()
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::vector<std::string> out;
        for (int i = 1; argv && i < argc; ++i) out.push_back(ToUtf8(argv[i]));
        if (argv) LocalFree(argv);
        return out;
    }

    std::filesystem::path StemOf(const std::filesystem::path& envelopePath)
    {
        return envelopePath.parent_path() / envelopePath.stem();   // "<dir>/<name>" from "<dir>/<name>.arcdiag"
    }

    // Task 4's sibling: the portable stack the host already wrote, re-headed.
    // Task 5 keeps this as the fallback when the debug engine is unavailable.
    bool WriteFallbackSymbolized(const std::filesystem::path& stem, const Arcane::Diag::Envelope& e, std::string_view why)
    {
        std::ofstream out(stem.string() + ".symbolized.txt", std::ios::binary);
        if (!out) return false;
        out << "symbolized by ArcaneCrashReporter " << Arcane::BuildInfo() << "\n"
            << "engine      : unavailable (" << why << ") -- module+offset from the portable stack\n\n"
            << e.cpuThreadSummary << "\n";
        return static_cast<bool>(out);
    }

    int RunReport(const Args& a)
    {
        const std::filesystem::path envelopePath = std::filesystem::path(ToWide(a.envelopePath));
        const auto envelope = Arcane::Diag::ReadFile(envelopePath);
        if (!envelope) { ARC_ERROR("reporter: cannot read envelope '{}'", a.envelopePath); return ExitCode::kNoEnvelope; }
        const std::filesystem::path stem = StemOf(envelopePath);

        // Diagnostics for the reporter ITSELF (spec s6 failure modes): same
        // folder, never spawns a reporter (D13).
        Arcane::Diagnostics::Config diag;
        diag.appName = "ArcaneCrashReporter";
        diag.productName = "Arcane Crash Reporter";
        diag.dumpDir = stem.parent_path().string();
        diag.unattended = a.unattended;
        diag.spawnReporter = false;
        diag.startHangWatchdog = false;
        Arcane::Diagnostics::Install(diag);

        // TASK 5: the dbgeng worker + deadline replace this call.
        WriteFallbackSymbolized(stem, *envelope, "not built yet (plan 2 task 4)");

        // TASK 7: the window, unless unattended.
        Arcane::Diagnostics::Shutdown();
        return ExitCode::kOk;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();

    const std::vector<std::string> argv = ArgvUtf8();
    const ParseResult parsed = ParseArgs(argv);
    if (!parsed.args)
    {
        ARC_ERROR("reporter: {}\n{}", parsed.error, Usage());
        return ExitCode::kBadArgs;
    }
    if (parsed.args->mode == Args::Mode::Monitor)
        return ExitCode::kBadArgs;   // TASK 9: RunMonitor(*parsed.args)
    return RunReport(*parsed.args);
}
```

- [ ] **Step 6: The death fixture, and the build-machine override**

`Diagnostics.cpp` `Install` (:2108): the build-machine gate gains UE's override (`-AllowCrashReportClientOnBuildMachine`, WindowsPlatformCrashContext.cpp:1053):

```cpp
    // A build agent must never be left with an interactive process on it --
    // unless it says so: the unattended reporter is bounded by its deadline,
    // and a CI lane that wants the hand-off proven opts in explicitly.
    if ((EnvIsSet(L"ARCANE_BUILD_MACHINE") || EnvIsSet(L"CI")) && !EnvIsSet(L"ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE"))
        g_cfg.spawnReporter = false;
```

`DeathFixtureMain.cpp`: parse two new bare flags and thread them into the config:

```cpp
    bool reporter = false, attended = false;
    // in the loop:
        else if (a == "--reporter") reporter = true;     // spawn the STAGED reporter beside this exe (tests, desk)
        else if (a == "--attended") attended = true;     // desk only: let the reporter show its window
    // config:
    cfg.unattended = !attended; cfg.spawnReporter = reporter;
```

Update the file's header comment: "`--reporter` turns the hand-off on (the reporter is staged beside this exe by premake); `--attended` is the desk switch that lets it show a window."

- [ ] **Step 7: Build, run, commit**

Regenerate; FULL Debug build (new project; the hosts' postbuild stages the exe). Verify the staging: `ls bin/Debug-windows-x86_64-md/{ArcaneEditor,ArcaneRuntime,ArcaneServer,death-fixture}/ArcaneCrashReporter.exe`. Run `"[reporter]"` (3 cases), `"death fixture*"` (7 cases; the new one writes the fallback sibling), `"[diag]"`; print exit codes. By hand: `cd bin/Debug-windows-x86_64-md/death-fixture && ./death-fixture.exe --dir /tmp/dd --die av --reporter; echo $?` -> 10, and within a second `/tmp/dd/*.symbolized.txt` exists.

```bash
git add premake5.lua ArcaneCrashReporter/src/ReporterArgs.hpp ArcaneCrashReporter/src/ReporterArgs.cpp ArcaneCrashReporter/src/Win32Text.hpp ArcaneCrashReporter/src/ReporterMain.cpp ArcaneTests/src/ReporterArgsTest.cpp ArcaneTests/src/CrashPathTest.cpp ArcaneTests/death-fixture/DeathFixtureMain.cpp
git commit -m "feat(reporter): ArcaneCrashReporter.exe -- the hand-off target exists, parses the contract line, writes the portable-stack sibling, and is staged beside every host (crash window plan 2, task 4)"
```

---

### Task 5: Out-of-process symbolization with dbgeng, and the unattended deadline (spec §6 "Symbolization", "Unattended")

**Files:**
- Create: `ArcaneCrashReporter/src/SymbolizedText.hpp`, `SymbolizedText.cpp` (pure), `ArcaneCrashReporter/src/Symbolizer.hpp`, `Symbolizer.cpp` (dbgeng)
- Modify: `ArcaneCrashReporter/src/ReporterMain.cpp` (worker + deadline replace the Task 4 fallback call), `premake5.lua` (ArcaneTests compiles `SymbolizedText.cpp`)
- Test: `ArcaneTests/src/SymbolizedTextTest.cpp` (new), `ArcaneTests/src/CrashPathTest.cpp` (PDB present / hidden case)

**Interfaces:**
- Produces:

```cpp
// SymbolizedText.hpp -- pure model + text, compiled into ArcaneTests.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Reporter
{
    struct SymFrame
    {
        std::uint64_t address      = 0;
        std::string   module;        // "death_fixture" (dbgeng's name) or "death-fixture.exe" (image name, symbol-less fallback)
        std::string   function;      // empty when no symbol resolved
        std::uint64_t displacement = 0;   // from `function` when set, else from the module base
        std::string   file;          // empty when no line info
        std::uint32_t line         = 0;
    };
    struct SymThread
    {
        std::uint32_t         systemId = 0;
        bool                  faulting = false;
        std::vector<SymFrame> frames;
    };
    struct Symbolized
    {
        bool                   engineAvailable = false;   // DebugCreate + OpenDumpFile + WaitForEvent all succeeded
        std::string            engineError;               // why not, when not
        std::string            symbolPath;                // what the engine was actually given
        std::vector<SymThread> threads;                   // the faulting thread first
    };

    // "module!function+0x1a [file:line]"  |  "module!function+0x1a"  |  "module+0x1234"
    [[nodiscard]] std::string FormatFrame(const SymFrame& f);

    // The whole <stem>.symbolized.txt. `portableFallback` (the envelope's
    // cpuThreadSummary) is the body when the engine was unavailable.
    [[nodiscard]] std::string FormatSymbolized(const Symbolized& s, std::string_view buildInfo,
                                               std::string_view portableFallback);

    // The walked thread's id from the envelope's stack text ("--- thread 1234 (MAIN)"
    // is the first line plan 1 writes); 0 when absent. Used to put that thread
    // first when the dump carries no exception event (a hang report).
    [[nodiscard]] std::uint32_t ParseWalkedThreadId(std::string_view cpuThreadSummary);

    // Moves the thread whose systemId matches (or the one flagged faulting) to the front.
    void PutFaultingFirst(Symbolized& s, std::uint32_t walkedThreadId);
}
```

```cpp
// Symbolizer.hpp -- the dbgeng session. Blocking; run it on a worker.
#pragma once
#include "SymbolizedText.hpp"
#include <filesystem>
#include <string>

namespace Arcane::Reporter
{
    struct SymbolizeOptions
    {
        std::string   symbolPath;          // empty = "<reporter dir>;%_NT_SYMBOL_PATH%"
        bool          ignoreCvRecord = false;   // D5: set iff symbolPath was given -- the embedded PDB path is NOT consulted
        std::uint32_t maxFramesPerThread = 64;
        std::uint32_t maxThreads = 64;
        std::uint32_t waitForEventMs = 30000;
    };
    [[nodiscard]] Symbolized SymbolizeDump(const std::filesystem::path& dmp, const SymbolizeOptions& opt);
}
```

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/SymbolizedTextTest.cpp`:

```cpp
#include "SymbolizedText.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Reporter;

TEST_CASE("symbolized text: frames format as module!function+0xoff [file:line], degrading to module+0xoff", "[reporter]")
{
    SymFrame full{ 0x7ff6'1234'0000ull, "death_fixture", "main", 0x1a4, "D:\\a\\DeathFixtureMain.cpp", 52 };
    CHECK(FormatFrame(full) == "death_fixture!main+0x1a4 [D:\\a\\DeathFixtureMain.cpp:52]");
    SymFrame noLine{ 0x1, "KERNEL32", "BaseThreadInitThunk", 0x14, "", 0 };
    CHECK(FormatFrame(noLine) == "KERNEL32!BaseThreadInitThunk+0x14");
    SymFrame bare{ 0x1, "death-fixture.exe", "", 0x1234, "", 0 };
    CHECK(FormatFrame(bare) == "death-fixture.exe+0x1234");
}

TEST_CASE("symbolized text: the faulting thread leads, the fallback carries the portable stack", "[reporter]")
{
    Symbolized s;
    s.engineAvailable = true;
    s.symbolPath = "D:\\bin";
    s.threads.push_back({ 11, false, { { 0x1, "ntdll", "NtWaitForSingleObject", 0x14, "", 0 } } });
    s.threads.push_back({ 22, true,  { { 0x2, "death_fixture", "main", 0x1a4, "f.cpp", 52 } } });
    PutFaultingFirst(s, 0);
    REQUIRE(s.threads.front().systemId == 22u);

    const std::string text = FormatSymbolized(s, "Arcane 0.1 Debug", "");
    CHECK(text.find("symbolized by ArcaneCrashReporter Arcane 0.1 Debug") == 0u);
    CHECK(text.find("engine      : dbgeng") != std::string::npos);
    CHECK(text.find("--- thread 22 (faulting)") < text.find("--- thread 11"));
    CHECK(text.find("00 death_fixture!main+0x1a4 [f.cpp:52]") != std::string::npos);

    Symbolized none;
    none.engineError = "DebugCreate failed: 0x80004005";
    const std::string fb = FormatSymbolized(none, "b", "--- thread 5 (MAIN)\n00 x.dll + 0x10\n");
    CHECK(fb.find("engine      : unavailable (DebugCreate failed: 0x80004005)") != std::string::npos);
    CHECK(fb.find("--- thread 5 (MAIN)") != std::string::npos);
}

TEST_CASE("symbolized text: the walked thread id is read from the envelope's stack header and put first", "[reporter]")
{
    CHECK(ParseWalkedThreadId("--- thread 4242 (MAIN)\n00 a + 0x1\n") == 4242u);
    CHECK(ParseWalkedThreadId("--- thread 7\n") == 7u);
    CHECK(ParseWalkedThreadId("") == 0u);
    CHECK(ParseWalkedThreadId("garbage") == 0u);

    Symbolized s;
    s.threads.push_back({ 1, false, {} });
    s.threads.push_back({ 4242, false, {} });
    PutFaultingFirst(s, 4242);
    CHECK(s.threads.front().systemId == 4242u);
    CHECK(s.threads.front().faulting);
}
```

`CrashPathTest.cpp` -- the process-level proof against a REAL minidump:

```cpp
namespace
{
    // Runs the staged reporter directly on a report the fixture already wrote.
    Arcane::Test::WitnessRun RunReporter(const std::filesystem::path& stem, std::vector<std::string> extra)
    {
        const auto exe = std::filesystem::absolute("../ArcaneCrashReporter/ArcaneCrashReporter.exe");
        REQUIRE(std::filesystem::exists(exe));
        std::vector<std::string> args = { stem.string() + ".arcdiag", "--pid", "0", "--kind", "crash",
                                          "--product", "DeathFixture", "--unattended", "--deadline", "40" };
        args.insert(args.end(), extra.begin(), extra.end());
        Arcane::Test::WitnessInvocation inv; inv.exePath = exe; inv.args = args; inv.hardCapMs = 60000;
        return Arcane::Test::RunWitness(inv);
    }
}

// Spec s10 "Symbolization": once with PDBs present (names resolve) and once with
// them hidden (module+offset). --symbol-path REPLACES the search and ignores
// the PDB path the linker embedded (D5), which is the only way to hide a PDB
// on the desk that built it. `--pid 0` = the host is already gone, the crash case.
TEST_CASE("reporter: symbolizes the death fixture's minidump -- names with PDBs, module+offset with them hidden", "[diag]")
{
    const FixtureRun crash = RunFixture("av");
    REQUIRE(crash.run.exitCode == 10);
    REQUIRE_FALSE(crash.stem.empty());
    const std::string sibling = crash.stem.string() + ".symbolized.txt";

    const std::string fixtureDir = std::filesystem::absolute("../death-fixture").string();
    const std::string coreDir    = std::filesystem::absolute("../ArcaneCore").string();
    {
        const auto run = RunReporter(crash.stem, { "--symbol-path", fixtureDir + ";" + coreDir });
        INFO("reporter stderr: " << run.stderrPath.string());
        CHECK_FALSE(run.timedOut);
        CHECK(run.exitCode == 0);
        REQUIRE(std::filesystem::exists(sibling));
        const std::string text = Slurp(sibling);
        CHECK(text.find("engine      : dbgeng") != std::string::npos);
        CHECK(text.find("(faulting)") != std::string::npos);
        CHECK(text.find("!main") != std::string::npos);            // death-fixture.pdb resolved
        CHECK(text.find("DeathFixtureMain.cpp") != std::string::npos);
    }
    std::filesystem::remove(sibling);
    {
        const auto hidden = std::filesystem::temp_directory_path() / "arcane-no-symbols";
        std::filesystem::create_directories(hidden);
        const auto run = RunReporter(crash.stem, { "--symbol-path", hidden.string() });
        CHECK(run.exitCode == 0);
        REQUIRE(std::filesystem::exists(sibling));
        const std::string text = Slurp(sibling);
        CHECK(text.find("engine      : dbgeng") != std::string::npos);
        CHECK(text.find("!main") == std::string::npos);
        CHECK(text.find("death-fixture.exe+0x") != std::string::npos);   // the symbol-less form names the IMAGE
    }
}
```

- [ ] **Step 2: Run to verify they fail**

Regenerate; build ArcaneTests. Expected: `Cannot open include file: 'SymbolizedText.hpp'`; the process case fails on `engine      : dbgeng` (Task 4's exe writes "unavailable").

- [ ] **Step 3: `SymbolizedText.cpp`**

```cpp
#include "SymbolizedText.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>

namespace Arcane::Reporter
{
    std::string FormatFrame(const SymFrame& f)
    {
        char off[32];
        std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(f.displacement));
        std::string s = f.module;
        if (!f.function.empty()) s += "!" + f.function;
        s += off;
        if (!f.file.empty()) s += " [" + f.file + ":" + std::to_string(f.line) + "]";
        return s;
    }

    std::string FormatSymbolized(const Symbolized& s, std::string_view buildInfo, std::string_view portableFallback)
    {
        std::string out = "symbolized by ArcaneCrashReporter ";
        out += buildInfo;
        out += "\n";
        if (s.engineAvailable)
        {
            out += "engine      : dbgeng\nsymbol path : " + s.symbolPath + "\n\n";
            for (const SymThread& t : s.threads)
            {
                out += "--- thread " + std::to_string(t.systemId) + (t.faulting ? " (faulting)\n" : "\n");
                char idx[8];
                for (std::size_t i = 0; i < t.frames.size(); ++i)
                {
                    std::snprintf(idx, sizeof(idx), "%02zu ", i);
                    out += idx;
                    out += FormatFrame(t.frames[i]);
                    out += "\n";
                }
                out += "\n";
            }
        }
        else
        {
            out += "engine      : unavailable (" + s.engineError + ") -- module+offset from the portable stack\n\n";
            out += portableFallback;
            if (out.empty() || out.back() != '\n') out += "\n";
        }
        return out;
    }

    std::uint32_t ParseWalkedThreadId(std::string_view text)
    {
        constexpr std::string_view kPrefix = "--- thread ";
        const std::size_t at = text.find(kPrefix);
        if (at == std::string_view::npos) return 0;
        const char* b = text.data() + at + kPrefix.size();
        const char* e = text.data() + text.size();
        std::uint32_t id = 0;
        const auto r = std::from_chars(b, e, id);
        return r.ec == std::errc{} ? id : 0;
    }

    void PutFaultingFirst(Symbolized& s, std::uint32_t walkedThreadId)
    {
        auto it = std::find_if(s.threads.begin(), s.threads.end(), [](const SymThread& t) { return t.faulting; });
        if (it == s.threads.end() && walkedThreadId != 0)
        {
            it = std::find_if(s.threads.begin(), s.threads.end(),
                              [&](const SymThread& t) { return t.systemId == walkedThreadId; });
            if (it != s.threads.end()) it->faulting = true;
        }
        if (it != s.threads.end() && it != s.threads.begin())
            std::rotate(s.threads.begin(), it, it + 1);
    }
}
```

- [ ] **Step 4: `Symbolizer.cpp` (dbgeng)**

```cpp
#include "Symbolizer.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/Engine.hpp>

#include <dbgeng.h>
#include <DbgHelp.h>    // the SYMOPT_* constants; UE includes it after dbgeng.h the same way (WindowsPlatformStackWalkExt.cpp:16-17)
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>

namespace Arcane::Reporter
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::string Hex(HRESULT hr)
        {
            char b[16]; std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr)); return b;
        }

        // UE's rule (WindowsPlatformStackWalkExt.cpp:183-243): the symbol path is
        // the directory of EVERY module the dump names, plus _NT_SYMBOL_PATH, and
        // the image path is the same set. For our layout that puts the host's
        // directory (ArcaneCore.dll/ArcaneClient.dll beside the exe) and the
        // project's Binaries/ (the game module, whose PDB arcbuild leaves beside
        // it) on the path without anyone naming them. The reporter's own
        // directory joins too: it is staged beside the host (spec s12 item 2).
        std::wstring ModuleDirectories(IDebugSymbols3* symbols)
        {
            std::wstring path;
            ULONG loaded = 0, unloaded = 0;
            symbols->GetNumberModules(&loaded, &unloaded);
            for (ULONG i = 0; i < loaded; ++i)
            {
                ULONG64 base = 0;
                if (FAILED(symbols->GetModuleByIndex(i, &base))) continue;
                wchar_t image[1024]; ULONG size = 0;
                if (FAILED(symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, i, base, image, 1024, &size)) || size <= 1) continue;
                std::wstring dir(image);
                const std::size_t slash = dir.find_last_of(L"/\\");
                if (slash == std::wstring::npos) continue;
                dir.resize(slash);
                if (path.find(dir + L";") == std::wstring::npos) { path += dir; path += L";"; }
            }
            return path;
        }

        std::wstring DefaultSymbolPath(IDebugSymbols3* symbols)
        {
            std::wstring path = ModuleDirectories(symbols);
            std::wstring self = ToWide(Arcane::ExecutablePathUtf8());
            const std::size_t slash = self.find_last_of(L"/\\");
            if (slash != std::wstring::npos) { self.resize(slash); path += self; path += L";"; }
            wchar_t env[4096];
            if (GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", env, 4096) > 0) path += env;
            return path;
        }

        // One frame: dbgeng's "module!function" name + displacement, line info
        // when the PDB has it, else the IMAGE file name + offset from its base.
        SymFrame Resolve(IDebugSymbols3* symbols, ULONG64 address)
        {
            SymFrame f;
            f.address = address;
            wchar_t name[1024]; ULONG size = 0; ULONG64 disp = 0;
            if (SUCCEEDED(symbols->GetNameByOffsetWide(address, name, 1024, &size, &disp)) && size > 1)
            {
                const std::string full = ToUtf8(name);
                const std::size_t bang = full.find('!');
                if (bang != std::string::npos)
                {
                    f.module = full.substr(0, bang);
                    f.function = full.substr(bang + 1);
                    f.displacement = disp;
                    wchar_t file[1024]; ULONG line = 0; ULONG fileSize = 0; ULONG64 lineDisp = 0;
                    if (SUCCEEDED(symbols->GetLineByOffsetWide(address, &line, file, 1024, &fileSize, &lineDisp)) && fileSize > 1)
                    {
                        f.file = ToUtf8(file);
                        f.line = line;
                    }
                    return f;
                }
            }
            ULONG index = 0; ULONG64 base = 0;
            if (SUCCEEDED(symbols->GetModuleByOffset(address, 0, &index, &base)))
            {
                wchar_t image[1024]; ULONG imageSize = 0;
                if (SUCCEEDED(symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, index, base, image, 1024, &imageSize)) && imageSize > 1)
                {
                    const std::string full = ToUtf8(image);
                    const std::size_t slash = full.find_last_of("/\\");
                    f.module = slash == std::string::npos ? full : full.substr(slash + 1);
                }
                else
                {
                    f.module = "<module>";
                }
                f.displacement = address - base;
                return f;
            }
            f.module = "<unknown>";
            f.displacement = address;
            return f;
        }
    }

    Symbolized SymbolizeDump(const std::filesystem::path& dmp, const SymbolizeOptions& opt)
    {
        Symbolized out;
        // dbgeng.h declares every interface DECLSPEC_UUID, so __uuidof is the
        // idiom (UE: WindowsPlatformStackWalkExt.cpp:51-55); no IID_ linkage.
        ComPtr<IDebugClient5> client;
        HRESULT hr = DebugCreate(__uuidof(IDebugClient5), reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { out.engineError = "DebugCreate failed: " + Hex(hr); return out; }

        ComPtr<IDebugControl4>       control;
        ComPtr<IDebugSymbols3>       symbols;
        ComPtr<IDebugSystemObjects4> sys;
        if (FAILED(client.As(&control)) || FAILED(client.As(&symbols)) || FAILED(client.As(&sys)))
        { out.engineError = "dbgeng interfaces unavailable"; return out; }

        // UE's option set (WindowsPlatformStackWalkExt.cpp:74-95), set BEFORE the
        // dump opens: line info, nearest OMAP, fail on critical errors, deferred
        // loads, EXACT symbols (a GUID/age mismatch is "no symbols", never a
        // wrong name), undecorated names. IGNORE_CVREC is the D5 seam on top:
        // do not follow the PDB path the linker embedded in the image.
        ULONG so = SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST | SYMOPT_FAIL_CRITICAL_ERRORS
                 | SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS | SYMOPT_UNDNAME;
        if (opt.ignoreCvRecord) so |= SYMOPT_IGNORE_CVREC;
        symbols->SetSymbolOptions(so);

        hr = client->OpenDumpFileWide(dmp.c_str(), 0);
        if (FAILED(hr)) { out.engineError = "OpenDumpFile failed: " + Hex(hr); return out; }
        hr = control->WaitForEvent(0, opt.waitForEventMs);   // UE: WaitForEvent(0, INFINITE), :568 -- ours is bounded
        if (FAILED(hr)) { out.engineError = "WaitForEvent failed: " + Hex(hr); return out; }
        out.engineAvailable = true;

        // The symbol path needs the module list, so it is set AFTER the dump
        // opens (UE: CrashDebugHelperWindows.cpp:30-33 -- InitSymbols, open,
        // SetSymbolPathsFromModules). --symbol-path REPLACES the whole search.
        const std::wstring symPath = opt.symbolPath.empty() ? DefaultSymbolPath(symbols.Get()) : ToWide(opt.symbolPath);
        symbols->SetSymbolPathWide(symPath.c_str());
        symbols->SetImagePathWide(symPath.c_str());
        out.symbolPath = ToUtf8(symPath);

        // The faulting thread, UE's way (:463-490): the STORED event's CONTEXT is
        // the exception context the host put in the dump, and
        // GetContextStackTrace walks from it. Every Arcane dump carries one --
        // Step 4b below synthesizes an exception record for hang and manual
        // reports, exactly as UE does for a suspended thread (:1925-1966).
        std::vector<std::uint8_t> ctx(4096);
        ULONG eventType = 0, eventPid = 0, eventTid = 0, ctxUsed = 0;
        const bool haveEvent = SUCCEEDED(control->GetStoredEventInformation(&eventType, &eventPid, &eventTid,
                                                                             ctx.data(), static_cast<ULONG>(ctx.size()), &ctxUsed,
                                                                             nullptr, 0, nullptr)) && ctxUsed > 0;
        if (haveEvent)
        {
            SymThread t;
            ULONG sysId = 0;
            sys->SetCurrentThreadId(eventTid);
            sys->GetCurrentThreadSystemId(&sysId);
            t.systemId = sysId;
            t.faulting = true;
            std::vector<DEBUG_STACK_FRAME> frames(opt.maxFramesPerThread);
            ULONG filled = 0;
            if (SUCCEEDED(control->GetContextStackTrace(ctx.data(), ctxUsed, frames.data(), static_cast<ULONG>(frames.size()),
                                                        nullptr, 0, 0, &filled)))
                for (ULONG k = 0; k < filled; ++k)
                    t.frames.push_back(Resolve(symbols.Get(), frames[k].InstructionOffset));
            out.threads.push_back(std::move(t));
        }

        // Every OTHER thread from its own saved context (what windbg's ~*k does).
        // This is beyond what UE's client walks -- its other threads come from
        // the in-process portable capture -- so it is best-effort: a thread that
        // fails to walk is listed with no frames rather than failing the report.
        ULONG count = 0;
        sys->GetNumberThreads(&count);
        for (ULONG i = 0; i < count && i < opt.maxThreads; ++i)
        {
            ULONG engineId = 0, systemId = 0;
            if (FAILED(sys->GetThreadIdsByIndex(i, 1, &engineId, &systemId))) continue;
            if (haveEvent && engineId == eventTid) continue;
            if (FAILED(sys->SetCurrentThreadId(engineId))) continue;
            SymThread t;
            t.systemId = systemId;
            std::vector<DEBUG_STACK_FRAME> frames(opt.maxFramesPerThread);
            ULONG filled = 0;
            if (SUCCEEDED(control->GetStackTrace(0, 0, 0, frames.data(), static_cast<ULONG>(frames.size()), &filled)))
                for (ULONG k = 0; k < filled; ++k)
                    t.frames.push_back(Resolve(symbols.Get(), frames[k].InstructionOffset));
            out.threads.push_back(std::move(t));
        }
        client->EndSession(DEBUG_END_ACTIVE_DETACH);
        return out;
    }
}
```

- [ ] **Step 4b: Every dump carries an exception stream (host side, `Diagnostics.cpp`)**

Plan 1 passes `ep ? &mei : nullptr` to `MiniDumpWriteDump` (Diagnostics.cpp:857), so a hang or manual report's dump has NO exception stream: dbgeng's stored event is then not an exception, `GetStoredEventInformation` fails, and UE's own `GetCallstacks` returns early on exactly that (WindowsPlatformStackWalkExt.cpp:464-468). UE never hits it because it turns a hang into a real exception and, for a suspended thread, builds a synthetic `EXCEPTION_POINTERS` with `ExceptionCode = STILL_ACTIVE` around the captured `CONTEXT` (WindowsPlatformCrashContext.cpp:1925-1966). Do the same, on the crash thread, from the context `CaptureWalkedStack` already captures:

```cpp
    // Crash-thread scratch (plan 2, task 5): the walked thread's context, kept
    // so the minidump of a hang/manual report carries an EXCEPTION stream. UE
    // builds the same synthetic EXCEPTION_POINTERS for a suspended thread
    // (WindowsPlatformCrashContext.cpp:1925-1966, ExceptionCode = STILL_ACTIVE):
    // "not a fault, a snapshot" -- and dbgeng's stored-event walk needs it.
    CONTEXT          g_walkedContext{};
    EXCEPTION_RECORD g_walkedRecord{};
    bool             g_walkedContextValid = false;
```

In `CaptureWalkedStack`: `g_walkedContextValid = false;` first; in the suspend branch, after `GetThreadContext(th, &ctx)` succeeds: `g_walkedContext = ctx; g_walkedContextValid = true;` (and `ctx.ContextFlags = CONTEXT_ALL;` -- UE :1935 -- so the dump's context is complete). In `RunReportOnCrashThread`, step 4 becomes:

```cpp
        // Step 4: the minidump. A hang/manual report has no fault, so the walked
        // thread's captured context rides in a synthetic record (STILL_ACTIVE):
        // every Arcane dump then opens with a stored event the reporter walks
        // from, uniformly. FillHeader still prints `exception :` only for a
        // real fault (it reads p.ep, untouched here).
        EXCEPTION_POINTERS  synthetic{};
        EXCEPTION_POINTERS* dumpEp = p.ep;
        if (!dumpEp && g_walkedContextValid)
        {
            g_walkedRecord = {};
            g_walkedRecord.ExceptionCode    = STILL_ACTIVE;
            g_walkedRecord.ExceptionAddress = reinterpret_cast<void*>(g_walkedContext.Rip);
            synthetic.ExceptionRecord = &g_walkedRecord;
            synthetic.ContextRecord   = &g_walkedContext;
            dumpEp = &synthetic;
        }
        const bool dumpOk = !p.lightweight && WriteMiniDump(dmpPath, dumpEp, p.walkThreadId);
```

Proof: Task 8's hang-sibling test asserts `(faulting)` in the hang report's `.symbolized.txt`, and `ParseWalkedThreadId` / `PutFaultingFirst` remain only as the fallback for dumps older than this change.

- [ ] **Step 5: The worker and the deadline in `ReporterMain.cpp`**

Replace the Task 4 `WriteFallbackSymbolized(...)` call in `RunReport`:

```cpp
        // Symbolize on a worker under the unattended deadline (spec s6): the
        // engine may block inside symbol loading, and a headless gate must
        // never inherit an open-ended child. On expiry the sibling is written
        // from what exists (the portable stack) and the process ENDS with
        // TerminateProcess -- a wedged worker must not turn into
        // std::terminate at exit.
        Symbolized result;
        std::mutex m; std::condition_variable cv; bool done = false;
        SymbolizeOptions opt;
        opt.symbolPath = a.symbolPath;
        opt.ignoreCvRecord = !a.symbolPath.empty();
        std::thread worker([&]
        {
            Symbolized r = SymbolizeDump(std::filesystem::path(ToWide(envelope->siblingDmp)), opt);
            std::lock_guard<std::mutex> lk(m);
            result = std::move(r);
            done = true;
            cv.notify_all();
        });
        const bool symbolizedInTime = [&]
        {
            std::unique_lock<std::mutex> lk(m);
            return cv.wait_for(lk, std::chrono::seconds(a.deadlineSeconds), [&] { return done; });
        }();
        const std::filesystem::path sibling = stem.string() + ".symbolized.txt";
        if (!symbolizedInTime)
        {
            Symbolized partial;
            partial.engineError = "deadline of " + std::to_string(a.deadlineSeconds) + " s expired";
            WriteText(sibling, FormatSymbolized(partial, Arcane::BuildInfo(), envelope->cpuThreadSummary));
            ARC_WARN("reporter: symbolization did not finish within {} s; wrote the portable stack", a.deadlineSeconds);
            Arcane::Log::FlushFileSinkBounded(1000);
            TerminateProcess(GetCurrentProcess(), ExitCode::kDeadline);
        }
        worker.join();
        if (envelope->siblingDmp.empty()) { result = Symbolized{}; result.engineError = "no minidump (lightweight report)"; }
        PutFaultingFirst(result, ParseWalkedThreadId(envelope->cpuThreadSummary));
        WriteText(sibling, FormatSymbolized(result, Arcane::BuildInfo(), envelope->cpuThreadSummary));
```

with a file-local `bool WriteText(const std::filesystem::path&, std::string_view)` (ofstream binary) replacing `WriteFallbackSymbolized`, and includes `<condition_variable>`, `<mutex>`, `<thread>`, `"Symbolizer.hpp"`. The unattended path then returns `ExitCode::kOk` as before (Task 7 inserts the window between the write and the return).

`premake5.lua`: ArcaneTests `files` gains `"%{wks.location}/ArcaneCrashReporter/src/SymbolizedText.cpp",`.

- [ ] **Step 6: Build, run, commit**

Regenerate; full Debug build. `"[reporter]"` (6 cases), `"reporter:*"` + `"death fixture*"` + `"[diag]"`; print exit codes. By hand, the deadline: `ArcaneCrashReporter.exe <stem>.arcdiag --pid 0 --unattended --deadline 0` -> exit 5 and a sibling headed "deadline of 0 s expired".

```bash
git add ArcaneCrashReporter/src/SymbolizedText.hpp ArcaneCrashReporter/src/SymbolizedText.cpp ArcaneCrashReporter/src/Symbolizer.hpp ArcaneCrashReporter/src/Symbolizer.cpp ArcaneCrashReporter/src/ReporterMain.cpp premake5.lua ArcaneTests/src/SymbolizedTextTest.cpp ArcaneTests/src/CrashPathTest.cpp
git commit -m "feat(reporter): out-of-process symbolization through dbgeng with the faulting thread first, module+offset without PDBs, and a hard unattended deadline (crash window plan 2, task 5)"
```

---

### Task 6: The envelope-to-view model and the log tail (spec §6 "Window" content, pure)

**Files:**
- Create: `ArcaneCrashReporter/src/ReportView.hpp`, `ReportView.cpp` (pure), `ArcaneCrashReporter/src/LogTail.hpp`, `LogTail.cpp` (filesystem, not compiled into tests)
- Modify: `premake5.lua` (ArcaneTests compiles `ReportView.cpp`)
- Test: `ArcaneTests/src/ReportViewTest.cpp` (new)

**Interfaces:**
- Consumes: `Diag::Envelope` (+ `reason`, Task 1), `ForeignModules::Classify`, `Symbolized` / `FormatFrame` (Task 5), `Args` (Task 4).
- Produces:

```cpp
// ReportView.hpp -- pure. What the window shows and what Copy Details copies.
#pragma once
#include "ReporterArgs.hpp"
#include "SymbolizedText.hpp"
#include <Arcane/Base/DiagEnvelope.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Reporter
{
    struct ThreadView { std::string label; std::string text; };   // "thread 4242 (faulting)" + its frames

    struct ReportView
    {
        std::string title;          // window title: "<product> -- crashed"
        std::string headline;       // "Arcane Editor crashed"
        std::string whenLine;       // "2026-09-23T10:11:12Z | phase: switch_plugin_load | build: Arcane 0.1 Debug"
        std::string reasonText;     // the envelope's reason, or "(no reason recorded)"
        std::string injectedText;   // one line per module; "" when none; "(not scanned)" is never claimed here
        std::vector<ThreadView> threads;   // faulting first; one thread from the portable stack when no symbolization
        std::string gpuText;        // queues / fault / layers when the envelope has any; "" otherwise
        std::string logTail;        // the last 200 lines
        std::string reportFolder;   // parent of the envelope
        std::string relaunchLine;   // Args::relaunch, else envelope.commandLine
        bool        isHang = false;         // hang | gpu-stall: Keep Waiting + Terminate and Collect, Relaunch disabled
        bool        canRelaunch = false;    // !relaunchLine.empty() && !isHang
        bool        isAbnormalExit = false; // monitor-synthesized: no stack, no threads selector
    };

    // "crashed" | "stopped responding" | "the GPU stopped responding" | "the GPU device was lost"
    // | "an assertion failed" | "terminated" | "ran out of memory" | "hit a recoverable check" | "exited abnormally"
    [[nodiscard]] std::string PlainWordsKind(std::string_view kind);

    [[nodiscard]] ReportView BuildReportView(const Diag::Envelope& e, const Args& a,
                                             const Symbolized* symbolized, std::string_view logTail);

    // The last `n` lines of `text` (pure; used by the log tail and the monitor).
    [[nodiscard]] std::string LastLines(std::string_view text, std::size_t n);

    // Everything, as text: header, reason, injected, the selected thread, GPU, log tail.
    [[nodiscard]] std::string DetailsText(const ReportView& v, std::size_t threadIndex);
}
```

```cpp
// LogTail.hpp
#pragma once
#include <filesystem>
#include <string>
namespace Arcane::Reporter
{
    // The folder's <stem>.log.txt first (self-contained report, spec s5.6);
    // the live log file's tail when the folder copy is missing; "" when neither.
    [[nodiscard]] std::string ReadLogTail(const std::filesystem::path& stem, const std::filesystem::path& livePath, std::size_t lines);
}
```

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/ReportViewTest.cpp`:

```cpp
#include "ReportView.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Reporter;

namespace
{
    Arcane::Diag::Envelope Crash()
    {
        Arcane::Diag::Envelope e;
        e.guid = Arcane::Guid::Generate();
        e.kind = "crash";
        e.reason = "crash (unhandled exception)";
        e.timestampUtc = "2026-09-23T10:11:12Z";
        e.appName = "ArcaneEditor";
        e.phase = "switch_plugin_load";
        e.buildInfo = "Arcane 0.1 Debug";
        e.cpuThreadSummary = "--- thread 4242 (MAIN)\n00 ArcaneCore.dll + 0x1234\n";
        e.foreignModules = { "GTIII-OSD64.dll", "SomethingElse.dll" };
        e.commandLine = "D:/bin/ArcaneEditor.exe --project D:/p";
        e.siblingTxt = "D:/p/Saved/Diagnostics/ArcaneEditor-20260923-101112-pid4242.txt";
        return e;
    }
    Args Attended() { Args a; a.envelopePath = "D:/p/Saved/Diagnostics/ArcaneEditor-20260923-101112-pid4242.arcdiag"; a.product = "Arcane Editor"; return a; }
}

TEST_CASE("report view: plain-words kinds", "[reporter]")
{
    CHECK(PlainWordsKind("crash") == "crashed");
    CHECK(PlainWordsKind("hang") == "stopped responding");
    CHECK(PlainWordsKind("gpu-stall") == "the GPU stopped responding");
    CHECK(PlainWordsKind("gpu-crash") == "the GPU device was lost");
    CHECK(PlainWordsKind("assert") == "an assertion failed");
    CHECK(PlainWordsKind("terminate") == "terminated");
    CHECK(PlainWordsKind("out-of-memory") == "ran out of memory");
    CHECK(PlainWordsKind("ensure") == "hit a recoverable check");
    CHECK(PlainWordsKind("abnormal-exit") == "exited abnormally");
    CHECK(PlainWordsKind("whatever") == "stopped (whatever)");
}

TEST_CASE("report view: a crash envelope becomes header, reason, named injected modules, one portable thread, relaunch", "[reporter]")
{
    const ReportView v = BuildReportView(Crash(), Attended(), nullptr, "line1\nline2\n");
    CHECK(v.title == "Arcane Editor -- crashed");
    CHECK(v.headline == "Arcane Editor crashed");
    CHECK(v.whenLine.find("2026-09-23T10:11:12Z") != std::string::npos);
    CHECK(v.whenLine.find("switch_plugin_load") != std::string::npos);
    CHECK(v.reasonText == "crash (unhandled exception)");
    CHECK(v.injectedText.find("GTIII-OSD64.dll") != std::string::npos);
    CHECK(v.injectedText.find("GPU Tweak III") != std::string::npos);     // the table names the product
    CHECK(v.injectedText.find("tier 1") != std::string::npos);
    CHECK(v.injectedText.find("SomethingElse.dll -- injected, uncatalogued") != std::string::npos);
    REQUIRE(v.threads.size() == 1);
    CHECK(v.threads[0].label == "thread 4242 (MAIN)");
    CHECK(v.threads[0].text.find("ArcaneCore.dll + 0x1234") != std::string::npos);
    CHECK(v.logTail == "line1\nline2\n");
    CHECK(v.reportFolder == "D:/p/Saved/Diagnostics");
    CHECK(v.relaunchLine == "D:/bin/ArcaneEditor.exe --project D:/p");
    CHECK(v.canRelaunch);
    CHECK_FALSE(v.isHang);
    CHECK(v.gpuText.empty());
}

TEST_CASE("report view: symbolized threads replace the portable one, faulting first; a hang disables relaunch; --relaunch overrides", "[reporter]")
{
    Symbolized s;
    s.engineAvailable = true;
    s.threads.push_back({ 4242, true,  { { 0x1, "ArcaneCore", "Arcane::Diagnostics::SubmitReport", 0x10, "D.cpp", 9 } } });
    s.threads.push_back({ 7,    false, { { 0x2, "ntdll", "NtWaitForSingleObject", 0x14, "", 0 } } });
    Args a = Attended();
    a.relaunch = "override.exe";
    const ReportView v = BuildReportView(Crash(), a, &s, "");
    REQUIRE(v.threads.size() == 2);
    CHECK(v.threads[0].label == "thread 4242 (faulting)");
    CHECK(v.threads[0].text.find("ArcaneCore!Arcane::Diagnostics::SubmitReport+0x10 [D.cpp:9]") != std::string::npos);
    CHECK(v.threads[1].label == "thread 7");
    CHECK(v.relaunchLine == "override.exe");

    Arcane::Diag::Envelope hang = Crash();
    hang.kind = "hang";
    hang.reason = "hang (main thread has not ticked for 12.3s)";
    const ReportView h = BuildReportView(hang, Attended(), nullptr, "");
    CHECK(h.isHang);
    CHECK_FALSE(h.canRelaunch);
    CHECK(h.headline == "Arcane Editor stopped responding");
}

TEST_CASE("report view: the GPU section and the abnormal-exit shape", "[reporter]")
{
    Arcane::Diag::Envelope g = Crash();
    g.kind = "gpu-crash";
    g.queues.push_back({ "graphics", "pass:tonemap", { "pass:gpu-fault" } });
    g.fault = { "page-fault", "0xDEADBEEF0000", "SceneColor" };
    g.activeLayers = { "DRED", "markers" };
    const ReportView v = BuildReportView(g, Attended(), nullptr, "");
    CHECK(v.gpuText.find("graphics: last completed pass:tonemap, in flight pass:gpu-fault") != std::string::npos);
    CHECK(v.gpuText.find("fault: page-fault at 0xDEADBEEF0000 (SceneColor)") != std::string::npos);
    CHECK(v.gpuText.find("layers: DRED, markers") != std::string::npos);

    Arcane::Diag::Envelope ab = Crash();
    ab.kind = "abnormal-exit";
    ab.reason = "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN";
    ab.cpuThreadSummary.clear();
    const ReportView x = BuildReportView(ab, Attended(), nullptr, "tail\n");
    CHECK(x.isAbnormalExit);
    CHECK(x.threads.empty());
    CHECK(x.headline == "Arcane Editor exited abnormally");
}

TEST_CASE("report view: LastLines and DetailsText", "[reporter]")
{
    CHECK(LastLines("a\nb\nc\nd\n", 2) == "c\nd\n");
    CHECK(LastLines("a\nb", 5) == "a\nb");
    CHECK(LastLines("", 3).empty());
    const ReportView v = BuildReportView(Crash(), Attended(), nullptr, "L1\n");
    const std::string d = DetailsText(v, 0);
    CHECK(d.find("Arcane Editor crashed") == 0u);
    CHECK(d.find("crash (unhandled exception)") != std::string::npos);
    CHECK(d.find("thread 4242 (MAIN)") != std::string::npos);
    CHECK(d.find("L1") != std::string::npos);
}
```

- [ ] **Step 2: Run to verify they fail**

Regenerate; build ArcaneTests. Expected: `Cannot open include file: 'ReportView.hpp'`.

- [ ] **Step 3: `ReportView.cpp`**

```cpp
#include "ReportView.hpp"
#include <Arcane/Base/ForeignModules.hpp>
#include <filesystem>

namespace Arcane::Reporter
{
    std::string PlainWordsKind(std::string_view kind)
    {
        if (kind == "crash")         return "crashed";
        if (kind == "hang")          return "stopped responding";
        if (kind == "gpu-stall")     return "the GPU stopped responding";
        if (kind == "gpu-crash")     return "the GPU device was lost";
        if (kind == "assert")        return "an assertion failed";
        if (kind == "terminate")     return "terminated";
        if (kind == "out-of-memory") return "ran out of memory";
        if (kind == "ensure")        return "hit a recoverable check";
        if (kind == "abnormal-exit") return "exited abnormally";
        return "stopped (" + std::string(kind) + ")";
    }

    std::string LastLines(std::string_view text, std::size_t n)
    {
        if (text.empty() || n == 0) return {};
        std::size_t end = text.size();
        if (text.back() == '\n') --end;             // a trailing newline does not count as a line
        std::size_t pos = end;
        for (std::size_t lines = 0; pos > 0; --pos)
        {
            if (text[pos - 1] == '\n' && ++lines == n) break;
        }
        return std::string(text.substr(pos));
    }

    namespace
    {
        std::string InjectedLines(const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& name : names)
            {
                out += name;
                if (const auto m = ForeignModules::Classify(name))
                {
                    out += " -- " + m->product + " (tier " + std::to_string(m->tier) + ")";
                    if (!m->consequence.empty()) out += ": " + m->consequence;
                    if (!m->remedy.empty())      out += " -- " + m->remedy;
                }
                else
                {
                    out += " -- injected, uncatalogued";
                }
                out += "\n";
            }
            return out;
        }

        std::string GpuLines(const Diag::Envelope& e)
        {
            std::string out;
            for (const auto& q : e.queues)
            {
                out += q.name + ": last completed " + (q.lastCompleted.empty() ? "<none>" : q.lastCompleted);
                if (!q.inFlight.empty())
                {
                    out += ", in flight ";
                    for (std::size_t i = 0; i < q.inFlight.size(); ++i) out += (i ? ", " : "") + q.inFlight[i];
                }
                out += "\n";
            }
            if (!e.fault.type.empty())
                out += "fault: " + e.fault.type + " at " + e.fault.address + (e.fault.resource.empty() ? "" : " (" + e.fault.resource + ")") + "\n";
            if (!e.activeLayers.empty())
            {
                out += "layers: ";
                for (std::size_t i = 0; i < e.activeLayers.size(); ++i) out += (i ? ", " : "") + e.activeLayers[i];
                out += "\n";
            }
            return out;
        }

        // The portable stack's first line is "--- thread <id> (MAIN)"; the label is the rest of that line.
        ThreadView PortableThread(std::string_view summary)
        {
            ThreadView t;
            const std::size_t nl = summary.find('\n');
            const std::string_view first = summary.substr(0, nl);
            t.label = first.rfind("--- ", 0) == 0 ? std::string(first.substr(4)) : "thread";
            t.text  = nl == std::string_view::npos ? "" : std::string(summary.substr(nl + 1));
            return t;
        }
    }

    ReportView BuildReportView(const Diag::Envelope& e, const Args& a, const Symbolized* sym, std::string_view logTail)
    {
        ReportView v;
        const std::string product = a.product.empty() ? (e.appName.empty() ? "Arcane" : e.appName) : a.product;
        const std::string words = PlainWordsKind(e.kind);
        v.title     = product + " -- " + words;
        v.headline  = product + " " + words;
        v.whenLine  = e.timestampUtc + (e.phase.empty() ? "" : " | phase: " + e.phase) + (e.buildInfo.empty() ? "" : " | build: " + e.buildInfo);
        v.reasonText = e.reason.empty() ? "(no reason recorded)" : e.reason;
        v.injectedText = InjectedLines(e.foreignModules);
        v.gpuText   = GpuLines(e);
        v.logTail   = std::string(logTail);
        v.reportFolder = std::filesystem::path(a.envelopePath).parent_path().generic_string();
        v.relaunchLine = a.relaunch.empty() ? e.commandLine : a.relaunch;
        v.isHang    = (e.kind == "hang" || e.kind == "gpu-stall");
        v.isAbnormalExit = (e.kind == "abnormal-exit");
        v.canRelaunch = !v.relaunchLine.empty() && !v.isHang;

        if (sym && sym->engineAvailable && !sym->threads.empty())
        {
            for (const SymThread& t : sym->threads)
            {
                ThreadView tv;
                tv.label = "thread " + std::to_string(t.systemId) + (t.faulting ? " (faulting)" : "");
                char idx[8];
                for (std::size_t i = 0; i < t.frames.size(); ++i)
                {
                    std::snprintf(idx, sizeof(idx), "%02zu ", i);
                    tv.text += idx + FormatFrame(t.frames[i]) + "\n";
                }
                v.threads.push_back(std::move(tv));
            }
        }
        else if (!e.cpuThreadSummary.empty())
        {
            v.threads.push_back(PortableThread(e.cpuThreadSummary));
        }
        return v;
    }

    std::string DetailsText(const ReportView& v, std::size_t threadIndex)
    {
        std::string d = v.headline + "\n" + v.whenLine + "\n\nreason: " + v.reasonText + "\n";
        if (!v.injectedText.empty()) d += "\ninjected modules:\n" + v.injectedText;
        if (threadIndex < v.threads.size()) d += "\n--- " + v.threads[threadIndex].label + "\n" + v.threads[threadIndex].text;
        if (!v.gpuText.empty()) d += "\n=== GPU ===\n" + v.gpuText;
        if (!v.logTail.empty()) d += "\n=== log (tail) ===\n" + v.logTail;
        d += "\nreport folder: " + v.reportFolder + "\n";
        return d;
    }
}
```

`LogTail.cpp`: read `<stem>.log.txt` fully if it exists, else `livePath` fully if it exists (both `std::ifstream` binary), return `LastLines(contents, lines)`.

`premake5.lua`: ArcaneTests `files` gains `"%{wks.location}/ArcaneCrashReporter/src/ReportView.cpp",`.

- [ ] **Step 4: Run to verify they pass, commit**

Regenerate; full build (the reporter project compiles the new files too). `"[reporter]"` -> 11 cases pass, exit 0.

```bash
git add ArcaneCrashReporter/src/ReportView.hpp ArcaneCrashReporter/src/ReportView.cpp ArcaneCrashReporter/src/LogTail.hpp ArcaneCrashReporter/src/LogTail.cpp premake5.lua ArcaneTests/src/ReportViewTest.cpp
git commit -m "feat(reporter): the envelope-to-view model -- plain-words kinds, named injected modules, faulting thread first, GPU section, log tail, relaunch rule (crash window plan 2, task 6)"
```

---

### Task 7: The window -- `ReporterWindow` presenter and the attended crash flow (spec §6 "Window")

**Files:**
- Create: `ArcaneCrashReporter/src/ReporterWindow.hpp`, `ReporterWindow.cpp`
- Modify: `ArcaneCrashReporter/src/ReporterMain.cpp` (attended path: window up within a second saying "Symbolizing...", filled by the worker; button actions)
- Test: desk only (spec §10: window appearance is desk-verify). The pure content is Task 6's; the window machinery is Task 2's `[platform]` case.

**Interfaces:**
- Consumes: `NativeWindow` (Task 2), `ReportView` / `DetailsText` (Task 6).
- Produces:

```cpp
// ReporterWindow.hpp -- the presenter. Plain Win32 controls, no toolkit.
#pragma once
#include "ReportView.hpp"
#include <Arcane/Platform/NativeWindow.hpp>
#include <functional>
#include <mutex>
#include <string>

namespace Arcane::Reporter
{
    class ReporterWindow final : public INativeWindowPresenter
    {
    public:
        enum Command : int
        {
            kBtnOpenFolder = 100, kBtnCopy = 101, kBtnClose = 102, kBtnRelaunch = 103,
            kBtnKeepWaiting = 104, kBtnTerminate = 105,
            kThreadCombo = 200, kDetails = 300, kHeader = 301, kWhen = 302, kReason = 303,
        };
        enum User : unsigned { kUserViewChanged = 1, kUserHostRecovered = 2, kUserHostExited = 3 };

        // `onCommand` runs ON THE WINDOW THREAD with a Command id. The reporter's
        // main decides what a button means (relaunch, terminate); the presenter
        // only draws and reports.
        ReporterWindow(ReportView initial, std::function<void(int)> onCommand);

        // Open the window on its own thread and wait for it. The initial view
        // is drawn with "Symbolizing..." in the details until SetView arrives.
        void Show(NativeWindow& window, const std::string& productForTitle);
        // From ANY thread: replace the view and repaint (posts kUserViewChanged).
        void SetView(ReportView v);
        // From any thread: the hang was terminated / the host exited -- the
        // window becomes the crash view (hang buttons hidden, Relaunch enabled
        // when a line exists, title updated).
        void BecomeCrashView(const std::string& headlineSuffix);

        [[nodiscard]] std::string CurrentDetails();   // DetailsText of the selected thread (Copy Details)

    private:
        void OnCreate(void* hwnd) override;
        void OnSize(int w, int h) override;
        void OnCommand(int id) override;
        bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t l) override;
        void OnDestroy() override;
        void ApplyView();          // window thread: pushes m_view into the controls
        void Layout(int w, int h);

        NativeWindow*           m_window = nullptr;
        std::mutex              m_mutex;
        ReportView              m_view;
        bool                    m_symbolizing = true;
        std::function<void(int)> m_onCommand;
        void* m_hwnd = nullptr; void* m_header = nullptr; void* m_when = nullptr; void* m_reason = nullptr;
        void* m_combo = nullptr; void* m_details = nullptr; void* m_buttons[6] = {};
        void* m_uiFont = nullptr; void* m_monoFont = nullptr;
    };
}
```

- [ ] **Step 1: `ReporterWindow.cpp`**

```cpp
#include "ReporterWindow.hpp"
#include "Win32Text.hpp"
#include <windowsx.h>

namespace Arcane::Reporter
{
    namespace
    {
        HFONT MessageFont(unsigned dpi)
        {
            NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
            return CreateFontIndirectW(&ncm.lfMessageFont);
        }
        HFONT MonoFont(unsigned dpi)
        {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
            lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
            wcscpy_s(lf.lfFaceName, L"Consolas");
            return CreateFontIndirectW(&lf);
        }
        HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font, DWORD exStyle = 0)
        {
            HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            if (h && font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return h;
        }
        // Multi-line EDIT controls want CRLF.
        std::wstring Crlf(std::string_view utf8)
        {
            std::wstring w = ToWide(utf8), out;
            out.reserve(w.size() + w.size() / 16);
            for (wchar_t c : w) { if (c == L'\n') out += L'\r'; out += c; }
            return out;
        }
    }

    ReporterWindow::ReporterWindow(ReportView initial, std::function<void(int)> onCommand)
        : m_view(std::move(initial)), m_onCommand(std::move(onCommand)) {}

    void ReporterWindow::Show(NativeWindow& window, const std::string& productForTitle)
    {
        m_window = &window;
        NativeWindowDesc d;
        d.className = L"ArcaneCrashReporter";
        d.title     = ToWide(m_view.title.empty() ? productForTitle : m_view.title);
        d.width = 900; d.height = 640;
        d.popup = false; d.topmost = false; d.appWindow = true;
        d.foreground = true;   // UE's CRC forces itself to front (CrashReportClientApp.cpp:425-427)
        d.backgroundRgb = 0xF0F0F0;   // the system button face: plain Win32 controls draw on it
        window.Open(d, this);
        (void)window.WaitUntilReady(5000);
    }

    void ReporterWindow::SetView(ReportView v)
    {
        { std::lock_guard<std::mutex> lk(m_mutex); m_view = std::move(v); m_symbolizing = false; }
        if (m_window) m_window->PostUser(kUserViewChanged);
    }

    void ReporterWindow::BecomeCrashView(const std::string& headlineSuffix)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_view.isHang = false;
            m_view.canRelaunch = !m_view.relaunchLine.empty();
            m_view.headline += headlineSuffix;
            m_view.title += headlineSuffix;
        }
        if (m_window) { m_window->SetTitle(ToWide(m_view.title)); m_window->PostUser(kUserViewChanged); }
    }

    std::string ReporterWindow::CurrentDetails()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const int sel = m_combo ? ComboBox_GetCurSel(static_cast<HWND>(m_combo)) : 0;
        return DetailsText(m_view, sel < 0 ? 0 : static_cast<std::size_t>(sel));
    }

    void ReporterWindow::OnCreate(void* hwnd)
    {
        m_hwnd = hwnd;
        HWND h = static_cast<HWND>(hwnd);
        const unsigned dpi = GetDpiForWindow(h);
        m_uiFont   = MessageFont(dpi);
        m_monoFont = MonoFont(dpi);
        HFONT ui = static_cast<HFONT>(m_uiFont), mono = static_cast<HFONT>(m_monoFont);
        m_header  = Child(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kHeader, ui);
        m_when    = Child(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kWhen, ui);
        m_reason  = Child(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kReason, ui);
        m_combo   = Child(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kThreadCombo, ui);
        m_details = Child(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                          kDetails, mono, WS_EX_CLIENTEDGE);
        const wchar_t* labels[6] = { L"Open Report Folder", L"Copy Details", L"Close", L"Relaunch", L"Keep Waiting", L"Terminate and Collect" };
        for (int i = 0; i < 6; ++i)
            m_buttons[i] = Child(h, L"BUTTON", labels[i], BS_PUSHBUTTON, kBtnOpenFolder + i, ui);
        ApplyView();
    }

    void ReporterWindow::ApplyView()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        SetWindowTextW(static_cast<HWND>(m_header), ToWide(m_view.headline).c_str());
        SetWindowTextW(static_cast<HWND>(m_when),   ToWide(m_view.whenLine).c_str());
        SetWindowTextW(static_cast<HWND>(m_reason), ToWide(m_view.reasonText).c_str());
        HWND combo = static_cast<HWND>(m_combo);
        const int prev = ComboBox_GetCurSel(combo);
        ComboBox_ResetContent(combo);
        for (const ThreadView& t : m_view.threads) ComboBox_AddString(combo, ToWide(t.label).c_str());
        ComboBox_SetCurSel(combo, m_view.threads.empty() ? -1 : (prev >= 0 && prev < static_cast<int>(m_view.threads.size()) ? prev : 0));
        ShowWindow(combo, m_view.threads.size() > 1 ? SW_SHOW : SW_HIDE);
        const std::size_t sel = ComboBox_GetCurSel(combo) < 0 ? 0 : static_cast<std::size_t>(ComboBox_GetCurSel(combo));
        const std::string body = m_symbolizing ? "Symbolizing...\n\n" + DetailsText(m_view, sel) : DetailsText(m_view, sel);
        SetWindowTextW(static_cast<HWND>(m_details), Crlf(body).c_str());
        // Buttons: [0] folder [1] copy [2] close [3] relaunch [4] keep waiting [5] terminate
        ShowWindow(static_cast<HWND>(m_buttons[4]), m_view.isHang ? SW_SHOW : SW_HIDE);
        ShowWindow(static_cast<HWND>(m_buttons[5]), m_view.isHang ? SW_SHOW : SW_HIDE);
        ShowWindow(static_cast<HWND>(m_buttons[3]), m_view.relaunchLine.empty() ? SW_HIDE : SW_SHOW);
        EnableWindow(static_cast<HWND>(m_buttons[3]), m_view.canRelaunch ? TRUE : FALSE);
        RECT rc{}; GetClientRect(static_cast<HWND>(m_hwnd), &rc);
        Layout(rc.right, rc.bottom);
    }

    void ReporterWindow::Layout(int w, int h)
    {
        const int dpi = static_cast<int>(GetDpiForWindow(static_cast<HWND>(m_hwnd)));
        auto px = [&](int v) { return MulDiv(v, dpi, 96); };
        const int m = px(12), line = px(20), btnW = px(150), btnH = px(28);
        int y = m;
        MoveWindow(static_cast<HWND>(m_header), m, y, w - 2 * m, line, TRUE); y += line + px(2);
        MoveWindow(static_cast<HWND>(m_when),   m, y, w - 2 * m, line, TRUE); y += line + px(2);
        MoveWindow(static_cast<HWND>(m_reason), m, y, w - 2 * m, line, TRUE); y += line + px(6);
        MoveWindow(static_cast<HWND>(m_combo),  m, y, px(320), px(200), TRUE);
        if (IsWindowVisible(static_cast<HWND>(m_combo))) y += px(26) + px(4);
        const int detailsBottom = h - m - btnH - px(8);
        MoveWindow(static_cast<HWND>(m_details), m, y, w - 2 * m, detailsBottom - y, TRUE);
        int x = m;
        for (int i = 0; i < 6; ++i)
        {
            HWND b = static_cast<HWND>(m_buttons[i]);
            if (!IsWindowVisible(b)) continue;
            MoveWindow(b, x, h - m - btnH, btnW, btnH, TRUE);
            x += btnW + px(8);
        }
    }

    void ReporterWindow::OnSize(int w, int h) { if (m_hwnd) Layout(w, h); }

    void ReporterWindow::OnCommand(int id)
    {
        if (id == kThreadCombo) { ApplyView(); return; }   // any notification: refresh the details for the selection
        if (id >= kBtnOpenFolder && id <= kBtnTerminate && m_onCommand) m_onCommand(id);
    }

    bool ReporterWindow::OnUser(unsigned msg, std::uintptr_t, std::intptr_t)
    {
        if (msg == kUserViewChanged) { ApplyView(); return true; }
        return false;
    }

    void ReporterWindow::OnDestroy()
    {
        if (m_uiFont)   DeleteObject(static_cast<HFONT>(m_uiFont));
        if (m_monoFont) DeleteObject(static_cast<HFONT>(m_monoFont));
        m_uiFont = m_monoFont = nullptr;
        m_hwnd = nullptr;
    }
}
```

(`kThreadCombo`'s WM_COMMAND carries the notification code in HIWORD, which NativeWindow drops; refreshing on every notification is fine for a read-only view.)

- [ ] **Step 2: The attended flow in `ReporterMain.cpp`**

In `RunReport`, BEFORE the worker starts (so the window is on screen within a second saying "Symbolizing..."), when `!a.unattended`:

```cpp
        const std::string logTail = ReadLogTail(stem, std::filesystem::path(ToWide(envelope->logPath)), 200);
        ReportView initial = BuildReportView(*envelope, a, nullptr, logTail);
        NativeWindow window;
        std::unique_ptr<ReporterWindow> ui;
        if (!a.unattended)
        {
            ui = std::make_unique<ReporterWindow>(initial, [&](int id) { OnButton(id, *ui, window); });
            ui->Show(window, a.product);
        }
```

after the sibling is written: `if (ui) ui->SetView(BuildReportView(*envelope, a, &result, logTail));` then, for the attended path, `window.Wait();` before `Shutdown()` and `return kOk`. The deadline branch (Task 5) applies only when `a.unattended`; an attended run lets the worker finish (the window already shows the portable stack).

The button actions (file-local; Task 8 adds the two hang buttons):

```cpp
    // Runs on the window thread. The folder and the relaunch line are read
    // from the window's CURRENT view (SetView may have replaced the initial
    // one), through two mutex-guarded accessors on ReporterWindow:
    //   [[nodiscard]] std::string ReportFolder();   // m_view.reportFolder
    //   [[nodiscard]] std::string RelaunchLine();   // m_view.relaunchLine
    void OnButton(int id, ReporterWindow& ui, NativeWindow& window)
    {
        HWND hwnd = static_cast<HWND>(window.Hwnd());
        switch (id)
        {
        case ReporterWindow::kBtnOpenFolder: OpenFolder(ToWide(ui.ReportFolder())); break;
        case ReporterWindow::kBtnCopy:       CopyToClipboard(hwnd, ui.CurrentDetails()); break;
        case ReporterWindow::kBtnRelaunch:
            SpawnDetached(ui.RelaunchLine());
            [[fallthrough]];
        case ReporterWindow::kBtnClose:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);   // ON the window thread: never Close() here
            break;
        default: break;   // task 8: kBtnKeepWaiting / kBtnTerminate / kHostExited / kHostRecovered
        }
    }
```

`OpenFolder`, `CopyToClipboard` and `SpawnDetached` are the `Win32Text.hpp` helpers Task 9 lists in full -- add them to that header in THIS task (Task 9 only adds a second caller). Add the two accessors to `ReporterWindow` beside `CurrentDetails()`.

- [ ] **Step 3: Build and desk-verify**

Full build. By hand from `bin/Debug-windows-x86_64-md/death-fixture/`: `./death-fixture.exe --dir /tmp/dd --die assert --reporter --attended` -> the window appears within a second titled "DeathFixture -- an assertion failed", details begin "Symbolizing...", then fill with `death_fixture!main` frames and the assert's expression, file and line in the reason. Copy Details pastes the same text into Notepad; Open Report Folder opens `/tmp/dd`; Relaunch is hidden (the fixture passes no command line); Close exits 0. Repeat with `--die av`. Resize the window: the details edit follows; move it to a monitor with a different scale: fonts and layout rescale (WM_DPICHANGED).

- [ ] **Step 4: Commit**

```bash
git add ArcaneCrashReporter/src/ReporterWindow.hpp ArcaneCrashReporter/src/ReporterWindow.cpp ArcaneCrashReporter/src/ReporterMain.cpp
git commit -m "feat(reporter): the crash window -- plain Win32 controls on NativeWindow, up within a second and filled by the symbolization worker; Open Folder, Copy Details, Relaunch, Close (crash window plan 2, task 7)"
```

---

### Task 8: The hang protocol -- recovered event, one live reporter, Keep Waiting / Terminate and Collect (spec §5.4, §9 "Reporter pid reuse")

**Files:**
- Modify: `ArcaneCore/src/Arcane/Base/Diagnostics.hpp` (`ProgressStallRule::WasReported`), `Diagnostics.cpp` (globals ~:289, `SpawnReporter` :913, the `spawnOk =` call :1136, `WatchdogMain` :1662 and :1715, `Install` after `ResolveReporterPath()`, `Shutdown` handle block)
- Create: `ArcaneCrashReporter/src/HangSession.hpp`, `HangSession.cpp` (pure)
- Modify: `ArcaneCrashReporter/src/ReporterMain.cpp` (the waiter thread, the two hang buttons), `premake5.lua` (ArcaneTests compiles `HangSession.cpp`)
- Test: `ArcaneTests/src/HangSessionTest.cpp` (new), `ArcaneTests/src/DiagnosticsTest.cpp` (recovered-event case), `ArcaneTests/src/CrashPathTest.cpp` (hang `--reporter` sibling)

**Interfaces:**
- Produces (host): the named event `Local\Arcane-Recovered-<pid>` (D11); spawn line gains ` --host-created <u64>` always and ` --recovered-event Local\Arcane-Recovered-<pid>` for survivable reports; `SpawnReporter(const char* stem, const char* kind, bool survivable)`; `[[nodiscard]] bool ProgressStallRule::WasReported() const noexcept`.
- Produces (reporter, pure):

```cpp
// HangSession.hpp -- what the reporter does when something happens to a hung host.
#pragma once
#include <cstdint>
namespace Arcane::Reporter
{
    enum class HangEvent { HostRecovered, HostExited, TerminateChosen, KeepWaitingChosen, IdentityMismatch };
    struct HangOutcome
    {
        bool closeWindow    = false;   // close silently and exit with `exitCode`
        bool terminateHost  = false;   // TerminateProcess(host, 11) first
        bool becomeCrashView = false;  // stay up as the crash view of the report already held
        int  exitCode       = 0;
    };
    // hostExitCode is meaningful for HostExited only.
    [[nodiscard]] HangOutcome DecideHang(HangEvent e, std::uint32_t hostExitCode);
}
```

Rules: `HostRecovered` -> close, 0. `HostExited` with 10/12/13 -> close, 0 (another reporter owns the crash view); with 11 -> becomeCrashView (we terminated it); any other code -> becomeCrashView (the host died under the window: say so). `TerminateChosen` -> terminateHost + becomeCrashView. `KeepWaitingChosen` -> close, 0 (D6). `IdentityMismatch` -> close, 4.

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/HangSessionTest.cpp`:

```cpp
#include "HangSession.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace Arcane::Reporter;

TEST_CASE("hang session: recovery and Keep Waiting close silently; a crash-exit yields to the crash reporter", "[reporter]")
{
    auto r = DecideHang(HangEvent::HostRecovered, 0);
    CHECK(r.closeWindow); CHECK_FALSE(r.terminateHost); CHECK_FALSE(r.becomeCrashView); CHECK(r.exitCode == 0);
    auto k = DecideHang(HangEvent::KeepWaitingChosen, 0);
    CHECK(k.closeWindow); CHECK(k.exitCode == 0);
    for (std::uint32_t code : { 10u, 12u, 13u })
    {
        auto e = DecideHang(HangEvent::HostExited, code);
        CHECK(e.closeWindow); CHECK_FALSE(e.becomeCrashView);
    }
}

TEST_CASE("hang session: Terminate and Collect kills the host and keeps the window as the crash view; other exits keep it too", "[reporter]")
{
    auto t = DecideHang(HangEvent::TerminateChosen, 0);
    CHECK(t.terminateHost); CHECK(t.becomeCrashView); CHECK_FALSE(t.closeWindow);
    auto own = DecideHang(HangEvent::HostExited, 11);
    CHECK(own.becomeCrashView); CHECK_FALSE(own.closeWindow);
    auto other = DecideHang(HangEvent::HostExited, 0xC0000005u);
    CHECK(other.becomeCrashView); CHECK_FALSE(other.terminateHost);
    auto bad = DecideHang(HangEvent::IdentityMismatch, 0);
    CHECK(bad.closeWindow); CHECK(bad.exitCode == 4);
}
```

`DiagnosticsTest.cpp` (beside the existing hang-watchdog cases; uses their short-threshold arming pattern):

```cpp
#if defined(_WIN32)
// Spec s5.4 / D11: the host creates Local\Arcane-Recovered-<pid> at Install,
// RESETS it before a survivable report's hand-off (spawnReporter is off here,
// so the reset is the only observable) and SETS it when the beat resumes.
TEST_CASE("diagnostics: the recovered event is reset by a hang report and set when the main thread beats again", "[diag]")
{
    const auto dir = std::filesystem::temp_directory_path() / "arcane-recovered-event-test";
    std::filesystem::create_directories(dir);
    Arcane::Log::Init();
    Arcane::Diagnostics::Config cfg;
    cfg.appName = "RecoveredEventTest"; cfg.dumpDir = dir.string();
    cfg.unattended = true; cfg.spawnReporter = false; cfg.installCrashHandler = false;
    cfg.startHangWatchdog = true; cfg.hangSeconds = 1;
    Arcane::Diagnostics::Install(cfg);
    struct Disarm { ~Disarm() { Arcane::Diagnostics::Shutdown(); } } disarm;

    const std::wstring name = L"Local\\Arcane-Recovered-" + std::to_wstring(GetCurrentProcessId());
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    REQUIRE(ev != nullptr);

    const std::uint32_t before = Arcane::Diagnostics::ReportCount();
    Arcane::Diagnostics::Heartbeat();   // arms the trigger; then silence
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (Arcane::Diagnostics::ReportCount() == before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(Arcane::Diagnostics::ReportCount() == before + 1);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);   // reset for the reporter that would be waiting

    Arcane::Diagnostics::Heartbeat();                    // the main thread moves again
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);
    CloseHandle(ev);
}
#endif
```

`CrashPathTest.cpp`:

```cpp
// The hang hand-off as a whole process: the watchdog spawns the reporter with
// the recovered event; unattended, it symbolizes the hang's minidump and exits
// while the host is STILL ALIVE, and the host then exits 0 on its own.
TEST_CASE("death fixture --reporter: a hang report gains its .symbolized.txt while the host lives", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("none", { "--hang", "8", "--hang-seconds", "1", "--reporter" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(r.run.exitCode == 0);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(Arcane::Diag::ReadFile(r.stem.string() + ".arcdiag")->kind == "hang");
    REQUIRE(WaitForFile(r.stem.string() + ".symbolized.txt", std::chrono::seconds(20)));
    CHECK(Slurp(r.stem.string() + ".symbolized.txt").find("thread") != std::string::npos);
}
```

- [ ] **Step 2: Run to verify they fail**

Regenerate; build. Expected: `HangSession.hpp` missing; the `[diag]` event case fails at `OpenEventW` (`ev == nullptr`).

- [ ] **Step 3: Host side in `Diagnostics.cpp`**

Globals (beside `g_spawnCmd`):

```cpp
    // The hang protocol (plan 2, D11/D12/D7). The event is created at Install
    // so the reporter can OpenEventW it by name; the reporter handle is kept
    // for SURVIVABLE spawns only; the creation time rides on every spawn so
    // the reporter never terminates a pid that was recycled (spec s9).
    wchar_t            g_recoveredName[64]{};
    HANDLE             g_recoveredEvent  = nullptr;
    HANDLE             g_reporterProcess = nullptr;
    unsigned long long g_hostCreated     = 0;
```

`SpawnReporter` becomes:

```cpp
    [[nodiscard]] bool SpawnReporter(const char* stemUtf8, const char* kind, bool survivable) noexcept
    {
        // The event is reset BEFORE the spawn gate so a test with the spawn
        // disabled still observes the protocol (DiagnosticsTest).
        if (survivable && g_recoveredEvent) ResetEvent(g_recoveredEvent);
        if (!g_cfg.spawnReporter) return true;
        if (!g_reporterExe[0])    return false;

        if (survivable && g_reporterProcess)
        {
            // One window per host (spec s5.4): a reporter is already up for
            // this pid. The report was still written; it just has no new window.
            if (WaitForSingleObject(g_reporterProcess, 0) == WAIT_TIMEOUT) return true;
            CloseHandle(g_reporterProcess);
            g_reporterProcess = nullptr;
        }

        wchar_t wideStem[kPathMax];
        wchar_t wideKind[64];
        ToWide(stemUtf8, wideStem, static_cast<int>(kPathMax));
        ToWide(kind, wideKind, 64);
        _snwprintf_s(g_spawnCmd, sizeof(g_spawnCmd) / sizeof(g_spawnCmd[0]), _TRUNCATE,
                     L"\"%s\" \"%s.arcdiag\" --pid %lu --kind %s --product \"%s\" --host-created %llu%s%s%s",
                     g_reporterExe, wideStem,
                     static_cast<unsigned long>(GetCurrentProcessId()),
                     wideKind, g_productWide, g_hostCreated,
                     g_cfg.unattended ? L" --unattended" : L"",
                     survivable ? L" --recovered-event " : L"",
                     survivable ? g_recoveredName : L"");

        STARTUPINFOW si{}; PROCESS_INFORMATION pi{}; si.cb = sizeof(si);
        if (!CreateProcessW(nullptr, g_spawnCmd, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
            return false;
        CloseHandle(pi.hThread);
        // UE :913-915: Windows lets a background process take focus only when
        // the foreground one grants it. Without this the reporter's window can
        // open BEHIND the dead host's. A user32 call, no heap; UE makes it from
        // its crash thread too.
        AllowSetForegroundWindow(pi.dwProcessId);
        if (survivable) g_reporterProcess = pi.hProcess;   // kept: "while it lives no second reporter"
        else            CloseHandle(pi.hProcess);          // the host is about to die; the reporter outlives it on purpose
        return true;
    }
```

Call site: `spawnOk = SpawnReporter(stem, kind, p.exitCode == 0);`.

`Install`, after `ResolveReporterPath();`:

```cpp
    _snwprintf_s(g_recoveredName, std::size(g_recoveredName), _TRUNCATE,
                 L"Local\\Arcane-Recovered-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    g_recoveredEvent = CreateEventW(nullptr, /*manualReset*/TRUE, FALSE, g_recoveredName);
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            g_hostCreated = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
```

`Shutdown`, in the Windows handle block: `if (g_reporterProcess) { CloseHandle(g_reporterProcess); g_reporterProcess = nullptr; }` and `if (g_recoveredEvent) { CloseHandle(g_recoveredEvent); g_recoveredEvent = nullptr; }` (the handle only -- a reporter mid-symbolization keeps running by design).

`WatchdogMain`: a file-local `void SignalRecovered() noexcept { if (g_recoveredEvent) SetEvent(g_recoveredEvent); }`. In `checkMainThreadBeat`: `if (reported && beat != reportedBeat) { reported = false; SignalRecovered(); }`. In `checkGpuProgress`, replace `if (!gpuRule.Poll(fence, now)) return;` with:

```cpp
            const bool wasReported = gpuRule.WasReported();
            if (!gpuRule.Poll(fence, now))
            {
                if (wasReported && !gpuRule.WasReported()) SignalRecovered();   // progress resumed after a gpu-stall report
                return;
            }
```

`Diagnostics.hpp` `ProgressStallRule`: add `[[nodiscard]] bool WasReported() const noexcept { return m_reported; }` after `Reset()`.

- [ ] **Step 4: Reporter side**

`HangSession.cpp`:

```cpp
#include "HangSession.hpp"
namespace Arcane::Reporter
{
    HangOutcome DecideHang(HangEvent e, std::uint32_t hostExitCode)
    {
        HangOutcome o;
        switch (e)
        {
        case HangEvent::HostRecovered:     o.closeWindow = true; break;
        case HangEvent::KeepWaitingChosen: o.closeWindow = true; break;
        case HangEvent::IdentityMismatch:  o.closeWindow = true; o.exitCode = 4; break;
        case HangEvent::TerminateChosen:   o.terminateHost = true; o.becomeCrashView = true; break;
        case HangEvent::HostExited:
            if (hostExitCode == 10 || hostExitCode == 12 || hostExitCode == 13) o.closeWindow = true;   // the crash reporter owns that view
            else o.becomeCrashView = true;
            break;
        }
        return o;
    }
}
```

`ReporterWindow.hpp` gains two pseudo-command ids and the exit code they carry, so the host events reach the SAME callback the buttons do (all on the window thread):

```cpp
        enum HostEvent : int { kHostExited = -1, kHostRecovered = -2 };   // delivered through the onCommand callback
        [[nodiscard]] std::uint32_t LastHostExitCode() const noexcept { return m_hostExitCode.load(); }
    private:
        std::atomic<std::uint32_t> m_hostExitCode{0};
```

and `OnUser` handles them:

```cpp
    bool ReporterWindow::OnUser(unsigned msg, std::uintptr_t w, std::intptr_t)
    {
        if (msg == kUserViewChanged)   { ApplyView(); return true; }
        if (msg == kUserHostExited)    { m_hostExitCode.store(static_cast<std::uint32_t>(w)); if (m_onCommand) m_onCommand(kHostExited); return true; }
        if (msg == kUserHostRecovered) { if (m_onCommand) m_onCommand(kHostRecovered); return true; }
        return false;
    }
```

`ReporterMain.cpp`, attended + `view.isHang`: open the host and the event, verify identity once, and start the waiter:

```cpp
        HANDLE host = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a.pid);
        HANDLE recovered = a.recoveredEvent.empty() ? nullptr : OpenEventW(SYNCHRONIZE, FALSE, ToWide(a.recoveredEvent).c_str());
        auto hostIsTheOneWeWereToldAbout = [&]() -> bool
        {
            if (!host || a.hostCreated == 0) return host != nullptr;
            FILETIME c{}, e{}, k{}, u{};
            return GetProcessTimes(host, &c, &e, &k, &u) &&
                   ((static_cast<unsigned long long>(c.dwHighDateTime) << 32) | c.dwLowDateTime) == a.hostCreated;
        };
        std::thread waiter;
        if (host)
        {
            waiter = std::thread([&]
            {
                HANDLE handles[2] = { host, recovered };
                const DWORD n = recovered ? 2u : 1u;
                const DWORD r = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
                if (r == WAIT_OBJECT_0)      { DWORD code = 0; GetExitCodeProcess(host, &code); window.PostUser(ReporterWindow::kUserHostExited, code); }
                else if (r == WAIT_OBJECT_0 + 1) window.PostUser(ReporterWindow::kUserHostRecovered);
            });
        }
```

The callback maps ids to events -- `kBtnKeepWaiting` -> `KeepWaitingChosen`, `kBtnTerminate` -> `TerminateChosen`, `kHostExited` -> `HostExited` (code = `ui.LastHostExitCode()`), `kHostRecovered` -> `HostRecovered`, and an identity check that fails -> `IdentityMismatch` -- and applies `DecideHang`:

```cpp
        const HangOutcome o = DecideHang(event, hostExitCode);
        if (o.terminateHost)
        {
            if (!hostIsTheOneWeWereToldAbout())
            { g_exitCode = ExitCode::kHostMismatch; PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
            TerminateProcess(host, Arcane::Diagnostics::ExitCode::kHangTerminated);   // 11
        }
        if (o.becomeCrashView) ui.BecomeCrashView(o.terminateHost ? " -- terminated and collected"
                                                                   : " -- the host exited (" + HexCode(hostExitCode) + ")");
        if (o.closeWindow) { g_exitCode = o.exitCode; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
```

After `window.Wait()`, close `host`/`recovered`, join the waiter (post nothing: `WaitForMultipleObjects` returns once the host exits; if the window was closed by the user while the host still lives, `TerminateThread` is NOT used -- instead the waiter also waits on a third handle, a manual-reset "reporter closing" event the main thread sets before joining), and return `g_exitCode`. Unattended hang reports do not wait on anything: symbolize, write, exit 0 (nobody to decide; the host's once-per-stall rule stands).

`premake5.lua`: ArcaneTests `files` gains `"%{wks.location}/ArcaneCrashReporter/src/HangSession.cpp",`.

- [ ] **Step 5: Build, run, desk, commit**

Full build. `"[reporter]"` (13), `"[diag]"` (incl. the event case), `"death fixture*"`; exit codes printed. Desk: `./death-fixture.exe --dir /tmp/dd --hang 60 --hang-seconds 2 --reporter --attended`: the window says "DeathFixture stopped responding" with Keep Waiting / Terminate and Collect; Terminate -> the fixture exits 11 (`echo $?` in its shell), the window retitles "-- terminated and collected", Relaunch stays hidden (no command line), Close exits 0. Second run, Keep Waiting -> reporter exits 0, the fixture keeps sleeping and exits 0 at 60 s. Third run with `--hang 4`: recovery before any click -> the window closes on its own (the fixture never beats again, so use the `[diag]` event case as the recovery proof; note this in the report).

```bash
git add ArcaneCore/src/Arcane/Base/Diagnostics.hpp ArcaneCore/src/Arcane/Base/Diagnostics.cpp ArcaneCrashReporter/src/HangSession.hpp ArcaneCrashReporter/src/HangSession.cpp ArcaneCrashReporter/src/ReporterMain.cpp ArcaneCrashReporter/src/ReporterWindow.hpp ArcaneCrashReporter/src/ReporterWindow.cpp premake5.lua ArcaneTests/src/HangSessionTest.cpp ArcaneTests/src/DiagnosticsTest.cpp ArcaneTests/src/CrashPathTest.cpp
git commit -m "feat(diagnostics,reporter): the hang protocol -- recovered event, one live reporter per host, host identity on every spawn, Keep Waiting / Terminate and Collect with exit 11 (crash window plan 2, task 8)"
```

---

### Task 9: Monitor mode -- the fail-fasts SEH never sees (spec §5.8)

**Files:**
- Create: `ArcaneCrashReporter/src/MonitorRule.hpp`, `MonitorRule.cpp` (pure), `ArcaneCrashReporter/src/Monitor.hpp`, `Monitor.cpp`
- Modify: `ArcaneCrashReporter/src/ReporterMain.cpp` (Monitor dispatch), `ArcaneCore/src/Arcane/Base/Diagnostics.hpp` (`Config::launchMonitor`), `Diagnostics.cpp` (`LaunchMonitor` at `Install`), `ArcaneEditor/src/main.cpp:361-383`, `ArcaneRuntime/src/main.cpp:84-97`, `ArcaneTests/death-fixture/DeathFixtureMain.cpp` (`--monitor`, `--die fastfail`), `premake5.lua` (ArcaneTests compiles `MonitorRule.cpp`)
- Test: `ArcaneTests/src/MonitorRuleTest.cpp` (new), `ArcaneTests/src/CrashPathTest.cpp` (fastfail + monitor)

**Interfaces:**
- Produces (host): `bool Config::launchMonitor = false;` -- "Pre-launch the reporter in monitor mode (spec S5.8) so a death the crash path never saw becomes an abnormal-exit report. Windowed hosts set `!headless`; headless and server hosts leave it off. Ignored when spawnReporter is false or a debugger is attached." The SESSION RECORD (D15, UE's `UECrashContext-<pid>.xml`): `<reportDir>/<app>-pid<pid>.session`, JSON `{ "pid", "app", "product", "logPath", "reportDir", "commandLine", "hostCreated", "launchedUtc" }`, written at `Install`, rewritten by `RetargetDumpDir`, deleted by `Shutdown()` and by the atexit hook. The monitor line: `"<reporter>" --monitor <pid> --session "<record>" [--unattended]` -- a path and a pid, nothing that needs escaping.
- Produces (reporter, pure):

```cpp
// MonitorRule.hpp
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace Arcane::Reporter
{
    enum class MonitorVerdict { Silent, Synthesize };
    // UE's rule (CrashReportAnalyticsSessionSummary.cpp:238-257, 415-417): the
    // host RECORDS its clean exit by deleting its session record; a record
    // still present when the process is gone is an abnormal exit, whatever the
    // exit code -- an external kill's code 1 included. A fresh report in the
    // report directory means the crash path already spoke (a crash reporter
    // owns that window). The exit code only NAMES the reason (UE :1090-1137).
    [[nodiscard]] MonitorVerdict ClassifyHostExit(bool sessionRecordPresent, bool freshReportExists);
    // "STATUS_STACK_BUFFER_OVERRUN" for 0xC0000409, ... ; "" when unknown. The
    // table is UE's (CrashReportClientApp.cpp:1093-1112) plus a few.
    [[nodiscard]] std::string_view NtStatusName(std::uint32_t code);
    // "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN" (name omitted when unknown)
    [[nodiscard]] std::string AbnormalExitReason(std::uint32_t code);
}
```

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/MonitorRuleTest.cpp`:

```cpp
#include "MonitorRule.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace Arcane::Reporter;

TEST_CASE("monitor rule: a clean exit deletes the session record; a record left behind is abnormal unless the crash path spoke", "[reporter]")
{
    CHECK(ClassifyHostExit(/*record*/false, /*fresh*/false) == MonitorVerdict::Silent);      // clean exit, whatever the code
    CHECK(ClassifyHostExit(/*record*/false, /*fresh*/true)  == MonitorVerdict::Silent);
    CHECK(ClassifyHostExit(/*record*/true,  /*fresh*/true)  == MonitorVerdict::Silent);      // exit 10/12/13: a crash reporter owns it
    CHECK(ClassifyHostExit(/*record*/true,  /*fresh*/false) == MonitorVerdict::Synthesize);  // fail-fast, external kill, /GS, heap
}

TEST_CASE("monitor rule: NTSTATUS names (UE's table and ours) and the reason line", "[reporter]")
{
    CHECK(NtStatusName(0xC0000409u) == "STATUS_STACK_BUFFER_OVERRUN");
    CHECK(NtStatusName(0xC0000374u) == "STATUS_HEAP_CORRUPTION");
    CHECK(NtStatusName(0xC00000FDu) == "STATUS_STACK_OVERFLOW");
    CHECK(NtStatusName(0xC0000005u) == "STATUS_ACCESS_VIOLATION");
    CHECK(NtStatusName(0xC0000602u) == "STATUS_FAIL_FAST_EXCEPTION");
    CHECK(NtStatusName(0xC000041Du) == "STATUS_FATAL_USER_CALLBACK_EXCEPTION");   // UE :1100
    CHECK(NtStatusName(0xC000012Du) == "STATUS_FATAL_MEMORY_EXHAUSTION");         // UE :1103
    CHECK(NtStatusName(0xC000000Du) == "STATUS_INVALID_PARAMETER");               // UE :1108
    CHECK(NtStatusName(0xC0000006u) == "STATUS_IN_PAGE_ERROR");                   // UE :1109
    CHECK(NtStatusName(0xC000013Au) == "STATUS_CONTROL_C_EXIT");
    CHECK(NtStatusName(0xE06D7363u) == "MSVC_CPP_EXCEPTION");
    CHECK(NtStatusName(42u).empty());
    CHECK(AbnormalExitReason(0xC0000409u) == "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN");
    CHECK(AbnormalExitReason(1u) == "abnormal-exit: 0x00000001");   // an external kill: named by number only
}
```

`CrashPathTest.cpp`:

```cpp
// Spec s5.8, the row the in-process path cannot close: __fastfail never
// reaches the filter, so the host writes NOTHING and dies 0xC0000409. The
// pre-launched monitor sees the code, finds no fresh report, and synthesizes
// an abnormal-exit report with the log tail -- exactly UE's monitor shape.
TEST_CASE("death fixture --monitor: a fail-fast the host never sees becomes an abnormal-exit report from the monitor", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("fastfail", { "--monitor" });
    CHECK_FALSE(r.run.timedOut);
    CHECK(static_cast<std::uint32_t>(r.run.exitCode) == 0xC0000409u);
    CHECK(r.stem.empty());   // the host wrote nothing at exit time
    const auto stem = WaitForStem(r.dir, std::chrono::seconds(10));
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "abnormal-exit");
    CHECK(env->appName == "DeathFixture");
    CHECK(static_cast<std::uint32_t>(env->exitCode) == 0xC0000409u);
    CHECK(env->reason.find("STATUS_STACK_BUFFER_OVERRUN") != std::string::npos);
    CHECK(std::filesystem::exists(stem.string() + ".txt"));
    CHECK(std::filesystem::exists(stem.string() + ".log.txt"));
    CHECK(Slurp(stem.string() + ".log.txt").find("death fixture: mode fastfail") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(stem.string() + ".dmp"));   // no minidump: the monitor never symbolizes
    CHECK(WaitForNoSessionRecord(r.dir, std::chrono::seconds(5)));   // the monitor reaps the record (UE :1408)
}

// The other half of s5.8: a host that died THROUGH the crash path (exit 10,
// report on disk) leaves the monitor silent -- one window per incident. The
// session record is still present (a crash never reaches Shutdown), so this is
// the "fresh report" clause of the rule doing its job.
TEST_CASE("death fixture --monitor: a crash the host reported itself leaves the monitor silent", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("av", { "--monitor" });
    CHECK(r.run.exitCode == 10);
    REQUIRE_FALSE(r.stem.empty());
    CHECK(WaitForNoSessionRecord(r.dir, std::chrono::seconds(5)));   // the monitor ran, decided, and reaped
    std::size_t envelopes = 0;
    for (const auto& e : std::filesystem::directory_iterator(r.dir))
        if (e.path().extension() == ".arcdiag") ++envelopes;
    CHECK(envelopes == 1);
}

// D15's reason for existing: an external kill. The harness kills the sleeping
// fixture at its cap (TerminateProcess, a code in the hosts' "known" range) --
// no report, no Shutdown, record left behind -> the monitor names it.
TEST_CASE("death fixture --monitor: an external kill leaves the session record behind and the monitor reports it", "[diag]")
{
    SkipIfBuildMachine();
    const auto exe = std::filesystem::absolute("../death-fixture/death-fixture.exe");
    const auto dir = std::filesystem::temp_directory_path() / "arcane-death-killed-monitor";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = exe;
    inv.args = { "--dir", dir.string(), "--die", "none", "--monitor", "--hang", "60", "--hang-seconds", "100" };
    inv.hardCapMs = 3000;   // killed mid-sleep; hang-seconds 100 keeps the watchdog quiet
    const auto run = Arcane::Test::RunWitness(inv);
    REQUIRE(run.timedOut);
    const auto stem = WaitForStem(dir, std::chrono::seconds(10));
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "abnormal-exit");
    CHECK(env->reason.rfind("abnormal-exit: 0x", 0) == 0);
    CHECK(WaitForNoSessionRecord(dir, std::chrono::seconds(5)));
}

// A clean run deletes its record in Shutdown, so the monitor has nothing to say.
TEST_CASE("death fixture --monitor: a clean exit deletes the session record and the monitor stays silent", "[diag]")
{
    SkipIfBuildMachine();
    const FixtureRun r = RunFixture("none", { "--monitor" });
    CHECK(r.run.exitCode == 0);
    CHECK(WaitForNoSessionRecord(r.dir, std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(WaitForStem(r.dir, std::chrono::milliseconds(0)).empty());
}
```

with one more helper beside `WaitForStem`:

```cpp
    // True once no "*.session" file remains in `dir` (the host deleted it on a
    // clean exit, or the monitor reaped it after deciding).
    bool WaitForNoSessionRecord(const std::filesystem::path& dir, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            bool any = false;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".session") any = true;
            if (!any) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
```

- [ ] **Step 2: Run to verify they fail**

Regenerate; build. Expected: `MonitorRule.hpp` missing; `'launchMonitor': is not a member` once the fixture edit is in.

- [ ] **Step 3: `MonitorRule.cpp`**

```cpp
#include "MonitorRule.hpp"
#include <cstdio>
namespace Arcane::Reporter
{
    MonitorVerdict ClassifyHostExit(bool sessionRecordPresent, bool freshReportExists)
    {
        if (!sessionRecordPresent) return MonitorVerdict::Silent;   // the host recorded its clean exit
        if (freshReportExists)     return MonitorVerdict::Silent;   // the crash path spoke; its reporter owns the window
        return MonitorVerdict::Synthesize;
    }

    std::string_view NtStatusName(std::uint32_t code)
    {
        switch (code)
        {
        case 0xC0000005u: return "STATUS_ACCESS_VIOLATION";
        case 0xC0000006u: return "STATUS_IN_PAGE_ERROR";
        case 0xC0000008u: return "STATUS_INVALID_HANDLE";
        case 0xC000000Du: return "STATUS_INVALID_PARAMETER";
        case 0xC0000017u: return "STATUS_NO_MEMORY";
        case 0xC000001Du: return "STATUS_ILLEGAL_INSTRUCTION";
        case 0xC0000094u: return "STATUS_INTEGER_DIVIDE_BY_ZERO";
        case 0xC00000FDu: return "STATUS_STACK_OVERFLOW";
        case 0xC000012Du: return "STATUS_FATAL_MEMORY_EXHAUSTION";
        case 0xC0000135u: return "STATUS_DLL_NOT_FOUND";
        case 0xC000013Au: return "STATUS_CONTROL_C_EXIT";
        case 0xC0000142u: return "STATUS_DLL_INIT_FAILED";
        case 0xC0000374u: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN";   // also every __fastfail
        case 0xC000041Du: return "STATUS_FATAL_USER_CALLBACK_EXCEPTION";
        case 0xC0000420u: return "STATUS_ASSERTION_FAILURE";
        case 0xC0000602u: return "STATUS_FAIL_FAST_EXCEPTION";
        case 0x80000003u: return "STATUS_BREAKPOINT";
        case 0xE06D7363u: return "MSVC_CPP_EXCEPTION";
        default:          return {};
        }
    }

    std::string AbnormalExitReason(std::uint32_t code)
    {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08X", code);
        std::string s = std::string("abnormal-exit: ") + hex;
        const std::string_view name = NtStatusName(code);
        if (!name.empty()) { s += " "; s += name; }
        return s;
    }
}
```

- [ ] **Step 4: `Monitor.cpp`**

```cpp
// Monitor.hpp
#pragma once
#include "ReporterArgs.hpp"
namespace Arcane::Reporter { [[nodiscard]] int RunMonitor(const Args& a); }
```

```cpp
#include "Monitor.hpp"
#include "MonitorRule.hpp"
#include "ReportView.hpp"
#include "ReporterWindow.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Platform/NativeWindow.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace Arcane::Reporter
{
    namespace
    {
        std::string Slurp(const std::filesystem::path& p)
        {
            std::ifstream in(p, std::ios::binary);
            return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        }
        bool WriteText(const std::filesystem::path& p, std::string_view s)
        {
            std::ofstream out(p, std::ios::binary); out << s; return static_cast<bool>(out);
        }
        // "20260923-101112" and "2026-09-23T10:11:12Z" from the same instant.
        void Stamps(std::string& compact, std::string& iso)
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{}; gmtime_s(&tm, &t);
            char a[32], b[32];
            std::strftime(a, sizeof(a), "%Y%m%d-%H%M%S", &tm);
            std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
            compact = a; iso = b;
        }
        bool FreshReportIn(const std::filesystem::path& dir, std::filesystem::file_time_type since)
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".arcdiag" && e.last_write_time(ec) >= since) return true;
            return false;
        }

        // The host's session record (D15). Absent/unreadable = the host is
        // already cleanly gone (or never wrote one): nothing to say.
        struct Session
        {
            std::string app, product, logPath, reportDir, commandLine;
            std::uint64_t hostCreated = 0;
            std::filesystem::file_time_type written{};
        };
        std::optional<Session> ReadSession(const std::filesystem::path& file)
        {
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) return std::nullopt;
            const auto doc = nlohmann::json::parse(Slurp(file), nullptr, /*allow_exceptions*/false);
            if (!doc.is_object()) return std::nullopt;
            Session s;
            auto str = [&](const char* k) { return doc.contains(k) && doc[k].is_string() ? doc[k].get<std::string>() : std::string{}; };
            s.app = str("app"); s.product = str("product"); s.logPath = str("logPath");
            s.reportDir = str("reportDir"); s.commandLine = str("commandLine");
            if (doc.contains("hostCreated") && doc["hostCreated"].is_number_unsigned()) s.hostCreated = doc["hostCreated"].get<std::uint64_t>();
            s.written = std::filesystem::last_write_time(file, ec);
            return s;
        }
    }

    int RunMonitor(const Args& a)
    {
        // D16 (UE :643-657): the FIRST instance relaunches itself and exits, so
        // the watcher's parent is a process that no longer exists and "End
        // process tree" on the host cannot take it along. GetCommandLineW is
        // the exact line we were started with; one flag is appended.
        if (!a.respawned)
        {
            std::wstring cmd = GetCommandLineW();
            cmd += L" --respawned";
            STARTUPINFOW si{}; PROCESS_INFORMATION pi{}; si.cb = sizeof(si);
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                               nullptr, nullptr, &si, &pi))
            { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return ExitCode::kOk; }
            // Respawn failed: watch from here rather than not at all.
        }

        const std::filesystem::path recordPath = std::filesystem::path(ToWide(a.sessionPath));
        // Same rights UE's client opens the monitored process with
        // (CrashReportAnalyticsSessionSummary.cpp:46-50), minus DUP_HANDLE/TERMINATE.
        HANDLE host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a.pid);
        if (!host) return ExitCode::kOk;   // already gone before we could watch: nothing to say
        WaitForSingleObject(host, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(host, &code);
        CloseHandle(host);

        // Re-read AFTER the exit: RetargetDumpDir rewrites the record when a
        // project opens, and Shutdown deletes it -- "present" is judged now.
        const std::optional<Session> session = ReadSession(recordPath);
        const std::filesystem::path reportDir = std::filesystem::path(ToWide(
            !a.reportDir.empty() ? a.reportDir : (session ? session->reportDir : std::string{})));
        const bool fresh = session && !reportDir.empty() && FreshReportIn(reportDir, session->written);
        if (ClassifyHostExit(session.has_value(), fresh) == MonitorVerdict::Silent)
        {
            std::error_code ec;
            std::filesystem::remove(recordPath, ec);   // reap (UE :1408); a no-op when the host already did
            return ExitCode::kOk;
        }

        // Synthesize (spec s5.8): envelope with the code named, the .txt
        // header, and the host's log tail as the backlog. No minidump, no
        // stack -- this process never saw the fault.
        std::error_code ec;
        std::filesystem::create_directories(reportDir, ec);
        std::string compact, iso;
        Stamps(compact, iso);
        const std::string app     = !a.app.empty() ? a.app : (session->app.empty() ? "Arcane" : session->app);
        const std::string logPath = !a.logPath.empty() ? a.logPath : session->logPath;
        const std::string product = !a.product.empty() ? a.product : session->product;
        const std::string relaunch = !a.relaunch.empty() ? a.relaunch : session->commandLine;
        const std::filesystem::path stem = reportDir / (app + "-" + compact + "-pid" + std::to_string(a.pid));

        Diag::Envelope e;
        e.guid = Guid::Generate();
        e.kind = "abnormal-exit";
        e.reason = AbnormalExitReason(code);
        e.timestampUtc = iso;
        e.appName = app;
        e.buildInfo = BuildInfo();
        e.logPath = logPath;
        e.commandLine = relaunch;
        e.exitCode = static_cast<int>(code);
        e.siblingTxt = (stem.string() + ".txt");

        const std::string logTail = logPath.empty() ? "" : LastLines(Slurp(std::filesystem::path(ToWide(logPath))), 512);
        WriteText(stem.string() + ".log.txt", logTail);
        WriteText(stem.string() + ".txt",
                  "=== Arcane diagnostic report (monitor) ===\nreason      : " + e.reason + "\napp         : " + app +
                  "\npid         : " + std::to_string(a.pid) + "\nexit code   : " + std::to_string(code) +
                  "\nlog         : " + logPath + "\nminidump    : <none -- the monitor never saw the fault>\n\n" +
                  "The host exited without recording a clean shutdown and without a report of its own: a\n"
                  "__fastfail, a /GS cookie failure, heap corruption, a stack overflow with no room for SEH,\n"
                  "or an external kill (spec S5.8).\n");
        Diag::WriteFile(e, stem.string() + ".arcdiag");
        std::filesystem::remove(recordPath, ec);   // reap the record now that its story is told (UE :1408)
        ARC_WARN("monitor: {} exited abnormally ({}); report at {}.arcdiag", app, e.reason, stem.string());
        if (a.unattended) return ExitCode::kOk;

        Args view = a;
        view.envelopePath = stem.string() + ".arcdiag";
        view.product = product;
        NativeWindow window;
        ReporterWindow ui(BuildReportView(e, view, nullptr, logTail), [&](int id)
        {
            if (id == ReporterWindow::kBtnOpenFolder) OpenFolder(reportDir.wstring());
            else if (id == ReporterWindow::kBtnCopy)  CopyToClipboard(static_cast<HWND>(window.Hwnd()), ui.CurrentDetails());
            else if (id == ReporterWindow::kBtnRelaunch || id == ReporterWindow::kBtnClose)
            {
                if (id == ReporterWindow::kBtnRelaunch && !relaunch.empty()) SpawnDetached(relaunch);
                PostMessageW(static_cast<HWND>(window.Hwnd()), WM_CLOSE, 0, 0);
            }
        });
        ui.Show(window, product);
        ui.SetView(BuildReportView(e, view, nullptr, logTail));   // clears "Symbolizing..."
        window.Wait();
        return ExitCode::kOk;
    }
}
```

The three button helpers in `Win32Text.hpp` (added in Task 7; repeated here so this task reads on its own):

```cpp
    inline void OpenFolder(const std::wstring& dir)
    {
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    inline void CopyToClipboard(HWND owner, std::string_view utf8)
    {
        const std::wstring text = ToWide(utf8);
        if (!OpenClipboard(owner)) return;
        EmptyClipboard();
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)))
        {
            if (void* p = GlobalLock(mem)) { std::wmemcpy(static_cast<wchar_t*>(p), text.c_str(), text.size() + 1); GlobalUnlock(mem); }
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }
    inline bool SpawnDetached(std::string_view lineUtf8)
    {
        std::wstring line = ToWide(lineUtf8);   // CreateProcessW wants a MUTABLE buffer
        STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return true;
    }
```

(`Win32Text.hpp` then also includes `<shellapi.h>` and `<cwchar>`.)

`Monitor.cpp` also includes `<Json.hpp>` and `<optional>`. `ReporterMain.cpp`: `if (parsed.args->mode == Args::Mode::Monitor) return RunMonitor(*parsed.args);`. The monitor installs no `Diagnostics` of its own before the wait (it is a sleeping process); install it with `dumpDir = reportDir`, `spawnReporter = false`, `launchMonitor = false` right before synthesizing. Because the host process is opened once at start and waited on by HANDLE, pid reuse cannot fool the wait; `hostCreated` in the record is kept for the log line only.

- [ ] **Step 5: Host side**

`Diagnostics.hpp` `Config`, after `spawnReporter`:

```cpp
        // Pre-launch the reporter in MONITOR mode (spec S5.8): it waits on this
        // process and turns an exit code the crash path never produced (a
        // __fastfail, /GS, heap corruption, a stack overflow with no room for
        // SEH, an external kill) into an abnormal-exit report. Windowed hosts
        // set it to !headless; headless and server hosts leave it off. Ignored
        // when spawnReporter is false (build machine) or a debugger is attached.
        bool launchMonitor = false;
```

`Diagnostics.cpp`: the session record (D15) and the launch, both at `Install` time (heap allowed), plus the heap-free deletion on the two exit paths. `#include <Json.hpp>` joins the file's includes.

```cpp
    // The session record (D15; UE's UECrashContext-<pid>.xml,
    // GenericPlatformCrashContext.cpp:877-880, 949-984). Its PRESENCE after
    // the process is gone is what the monitor reads as "died without a clean
    // shutdown"; its contents are what the synthesized report needs. Written
    // here and on RetargetDumpDir (the report dir moves with the project);
    // deleted by Shutdown() and by the atexit hook -- the two clean exits --
    // through the static wide path below, so neither deletion touches the heap.
    wchar_t g_sessionPathWide[kPathMax]{};

    void WriteSessionRecord()
    {
        if (!g_cfg.launchMonitor) return;
        char pathUtf8[kPathMax];
        std::snprintf(pathUtf8, sizeof(pathUtf8), "%s\\%s-pid%lu.session",
                      g_reportDirSnap, g_appNameSnap, static_cast<unsigned long>(GetCurrentProcessId()));
        ToWide(pathUtf8, g_sessionPathWide, static_cast<int>(kPathMax));

        nlohmann::json doc;
        doc["pid"]         = GetCurrentProcessId();
        doc["app"]         = g_appNameSnap;
        doc["product"]     = g_productSnap;
        doc["logPath"]     = g_logPathSnap;
        doc["reportDir"]   = g_reportDirSnap;
        doc["commandLine"] = g_commandLineSnap;
        doc["hostCreated"] = g_hostCreated;
        char stamp[48]; TimestampUtcIso8601(stamp, sizeof(stamp));
        doc["launchedUtc"] = stamp;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_sessionPathWide).parent_path(), ec);
        const HANDLE h = OpenForWrite(pathUtf8, /*append*/false);
        if (h == INVALID_HANDLE_VALUE) return;
        const std::string text = doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        WriteAll(h, text);
        CloseHandle(h);
    }

    void DeleteSessionRecord() noexcept
    {
        if (g_sessionPathWide[0]) { DeleteFileW(g_sessionPathWide); g_sessionPathWide[0] = L'\0'; }
    }

    void LaunchMonitor()
    {
        if (!g_cfg.launchMonitor || !g_cfg.spawnReporter || IsDebuggerPresent() || !g_reporterExe[0] || !g_sessionPathWide[0]) return;
        std::wstring cmd = L"\"" + std::wstring(g_reporterExe) + L"\" --monitor " + std::to_wstring(GetCurrentProcessId())
                         + L" --session \"" + std::wstring(g_sessionPathWide) + L"\"";   // a path: no quotes inside on Windows
        if (g_cfg.unattended) cmd += L" --unattended";
        STARTUPINFOW si{}; PROCESS_INFORMATION pi{}; si.cb = sizeof(si);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                           nullptr, nullptr, &si, &pi))
        {
            AllowSetForegroundWindow(pi.dwProcessId);   // D14; the first instance passes it on to its respawn (see below)
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);   // one monitor per host, detached, never waited on
            ARC_INFO("Diagnostics: monitor launched (pid {})", pi.dwProcessId);
        }
        else
        {
            ARC_WARN("Diagnostics: could not launch the crash monitor ({}); abnormal exits will go unreported", GetLastError());
        }
    }
```

Foreground and the respawn (D14 x D16): `AllowSetForegroundWindow` names ONE pid, and the watcher that shows the window is the respawned grandchild, so the grant above reaches only the first instance. Windows lets a process that HOLDS the grant pass it on: the first instance calls `AllowSetForegroundWindow(pi.dwProcessId)` for its respawn before exiting (add that line to `RunMonitor`'s respawn block, after `CreateProcessW`). The grant expires once the granting process no longer has the foreground, which is fine here: it is the HOST that must have it when it dies, and the dead host's last foreground state is exactly what the OS consults.

Call order in `Install`: `SnapshotReportDir(); ToWide(...); ResolveReporterPath(); AttachLogSink(); ... WriteSessionRecord(); LaunchMonitor();` -- the record must exist before the monitor starts. `RetargetDumpDir`: after `SnapshotReportDir()` and the sink re-attach, `DeleteSessionRecord(); WriteSessionRecord();` (the path moves with the report dir). `Shutdown()`: `DeleteSessionRecord();` right after `StopWatchdog()`. `AtExitStopWatchdog()`: `DeleteSessionRecord();` before the `g_exitedCleanly` store. The "Diagnostics armed" line gains `monitor {}`.

Hosts: `ArcaneEditor/src/main.cpp` after `diag.unattended = parsed.config->headless;` add `diag.launchMonitor = !parsed.config->headless;   // spec S5.8: windowed runs pre-launch the monitor` and the same in `ArcaneRuntime/src/main.cpp`. The server sets nothing (stays false).

Death fixture: `--monitor` -> `cfg.launchMonitor = true; cfg.spawnReporter = true;` (the monitor is the reporter exe; `--monitor` implies the spawn is allowed), and the death mode:

```cpp
#include <intrin.h>
    else if (die == "fastfail")     { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }   // 0xC0000409, invisible to SEH -- the monitor's row
```

`premake5.lua`: ArcaneTests `files` gains `"%{wks.location}/ArcaneCrashReporter/src/MonitorRule.cpp",`.

- [ ] **Step 6: Build, run, desk, commit**

Full build. `"[reporter]"` (15), `"death fixture*"` (both monitor cases), `"[diag]"`, `"[host]"`; exit codes printed. Desk: `ArcaneEditor.exe --project ReferenceProject --backend dx12` windowed, close it normally -> Task Manager shows no lingering `ArcaneCrashReporter.exe` a few seconds later. Then `./death-fixture.exe --dir /tmp/dd --die fastfail --monitor --attended` -> the monitor's window: "DeathFixture exited abnormally", reason `abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN`, the log tail ends in "death fixture: mode fastfail", no thread selector.

```bash
git add ArcaneCrashReporter/src/MonitorRule.hpp ArcaneCrashReporter/src/MonitorRule.cpp ArcaneCrashReporter/src/Monitor.hpp ArcaneCrashReporter/src/Monitor.cpp ArcaneCrashReporter/src/ReporterMain.cpp ArcaneCrashReporter/src/Win32Text.hpp ArcaneCore/src/Arcane/Base/Diagnostics.hpp ArcaneCore/src/Arcane/Base/Diagnostics.cpp ArcaneEditor/src/main.cpp ArcaneRuntime/src/main.cpp ArcaneTests/death-fixture/DeathFixtureMain.cpp premake5.lua ArcaneTests/src/MonitorRuleTest.cpp ArcaneTests/src/CrashPathTest.cpp
git commit -m "feat(diagnostics,reporter): monitor mode -- windowed hosts pre-launch the reporter, and an exit code the crash path never produced becomes an abnormal-exit report with the log tail (crash window plan 2, task 9)"
```

---

### Task 10: `--hang-main` and the witness lanes (spec §10 "Witnesses")

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp` (after `crashGpuFrame`, ~:290), `HostConfig.cpp` (`kStripWithValue` :22, the `crash-gpu` Option :165, the `crashGpuFrame` store :240)
- Modify: `ArcaneRuntime/src/RuntimeFrame.hpp` (`FrameIo`, after `gpuFaultFired` :260), `RuntimeFrame.cpp` (after the `--crash-gpu` block, :426), `RuntimeApp.hpp` (:289), `RuntimeApp.cpp` (:749)
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (:2070), `ArcaneEditor/src/App/EditorAppFrame.cpp` (after the `--crash-gpu` block, :397)
- Test: `ArcaneTests/src/HostConfigTest.cpp` (`[host]`), `ArcaneTests/src/CrashWitnessTest.cpp` (new, `[witness][gpu]`)

**Interfaces:**
- Produces: `std::uint64_t HostConfig::hangMainFrame = 0;` and `inline constexpr std::uint32_t kHangMainSeconds = 15;` (Dist-excluded); `--hang-main N` on both hosts; `"hang-main"` in the strip set.

- [ ] **Step 1: Write the failing tests**

`HostConfigTest.cpp`, beside the `--crash-gpu` parse case (:130-136):

```cpp
    // --hang-main N (crash window plan 2, D10): the witness hang lane's trigger.
    const auto hangRun = Arcane::HostConfig::Parse({"ArcaneRuntime.exe", "--project", "P", "--headless",
                                                    "--hang-main", "30", "--frames", "90"});
    REQUIRE(hangRun.config);
    CHECK(hangRun.config->hangMainFrame == 30u);
    CHECK(Arcane::kHangMainSeconds == 15u);
```

and in "SanitizeRelaunchLine strips the harness flags", add `"--hang-main", "30"` to `argv` and `"--hang-main"` to the `gone` list.

`ArcaneTests/src/CrashWitnessTest.cpp`:

```cpp
// Crash-window witness lanes (spec s10): the REAL staged ArcaneRuntime, headless,
// with the reporter staged beside it -- the copy WitnessScratch makes carries
// ArcaneCrashReporter.exe, so the default reporterPath resolves inside the copy.
#include "Helpers/HostWitness.hpp"
#include <Arcane/Base/DiagEnvelope.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace Arcane::Test;

namespace
{
    std::filesystem::path StagedRuntimeDir()
    {
        const std::filesystem::path p = std::filesystem::absolute("../ArcaneRuntime");
        REQUIRE(std::filesystem::exists(p / "ArcaneRuntime.exe"));
        REQUIRE(std::filesystem::exists(p / "ArcaneCrashReporter.exe"));   // staged by the host's postbuild (task 4)
        return p;
    }
    WitnessInvocation HostInv(const WitnessScratch& scratch, std::vector<std::string> extraArgs)
    {
        WitnessInvocation inv;
        inv.exePath    = scratch.Dir() / "ArcaneRuntime.exe";
        inv.workingDir = scratch.Dir();
        inv.reportPath = scratch.Dir() / "witness-report.json";
        inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan",
                     "--report", inv.reportPath.generic_string() };
        inv.args.insert(inv.args.end(), extraArgs.begin(), extraArgs.end());
        inv.hardCapMs = 120000;
        return inv;
    }
    std::filesystem::path NewestStem(const std::filesystem::path& dir)
    {
        std::filesystem::path found;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".arcdiag") found = e.path().parent_path() / e.path().stem();
        return found;
    }
    bool WaitForFile(const std::filesystem::path& p, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!std::filesystem::exists(p) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return std::filesystem::exists(p);
    }
    bool OnBuildMachine() { return std::getenv("CI") || std::getenv("ARCANE_BUILD_MACHINE"); }
}

// H1: the main thread stops beating for kHangMainSeconds (15 s) at frame 30 --
// past the 12 s default threshold -- so the watchdog writes a SURVIVABLE hang
// report into the project's Saved/Diagnostics, hands off to the reporter, and
// the host then finishes its frames and exits 0. The reporter's sibling lands
// while the host is still alive (unattended: symbolize and exit).
TEST_CASE("H1: a scripted main-thread hang yields a hang report, the reporter sibling, and a clean exit", "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "h1-hang-main");
    WitnessRun run = RunWitness(HostInv(scratch, { "--frames", "90", "--hang-main", "30" }));
    INFO("host stderr: " << run.stderrPath.string());
    CHECK_FALSE(run.timedOut);
    CHECK(run.exitCode == 0);
    CHECK(run.wallMs >= 15000);   // the sleep really happened

    const auto diagDir = scratch.Dir() / "ReferenceProject" / "Saved" / "Diagnostics";   // RuntimeApp retargets here on project open
    const auto stem = NewestStem(diagDir);
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "hang");
    CHECK(env->exitCode == 0);
    CHECK(std::filesystem::exists(stem.string() + ".dmp"));
    if (OnBuildMachine())
        SKIP("CI/ARCANE_BUILD_MACHINE set -- the reporter is never spawned on a build machine (spec s6)");
    REQUIRE(WaitForFile(stem.string() + ".symbolized.txt", std::chrono::seconds(20)));
}

// G1: the deliberate GPU fault. Desk-only (a TDR resets the driver): gated on
// ARCANE_DIAG_DESK like the arcbuild desk probes. Exit 1 is the hosts'
// device-loss code (Diagnostics.cpp's gpu-crash submit), not 10.
TEST_CASE("G1: a deliberate GPU fault yields a gpu-crash report with the reporter sibling", "[witness][gpu]")
{
    if (!std::getenv("ARCANE_DIAG_DESK")) SKIP("ARCANE_DIAG_DESK not set -- desk-only TDR lane");
    WitnessScratch scratch(StagedRuntimeDir(), "g1-crash-gpu");
    WitnessRun run = RunWitness(HostInv(scratch, { "--frames", "60", "--crash-gpu", "30" }));
    CHECK_FALSE(run.timedOut);
    const auto stem = NewestStem(scratch.Dir() / "ReferenceProject" / "Saved" / "Diagnostics");
    REQUIRE_FALSE(stem.empty());
    const auto env = Arcane::Diag::ReadFile(stem.string() + ".arcdiag");
    REQUIRE(env.has_value());
    CHECK(env->kind == "gpu-crash");
    REQUIRE(WaitForFile(stem.string() + ".symbolized.txt", std::chrono::seconds(20)));
}
```

- [ ] **Step 2: Run to verify they fail**

Regenerate; build ArcaneTests. Expected: `'hangMainFrame': is not a member`, `'kHangMainSeconds'` undeclared.

- [ ] **Step 3: HostConfig**

`HostConfig.hpp`, inside the same `#if !defined(ARCANE_DIST)` block after `crashGpuFrame`:

```cpp
        // DEV ONLY (crash window plan 2, D10): on frame N the MAIN thread
        // sleeps kHangMainSeconds without beating, then carries on -- the
        // scripted trigger for the hang report + reporter hand-off (spec
        // s5.4), the way --crash-gpu is the trigger for gpu-crash. Fires once.
        // 15 s clears Diagnostics::Config::hangSeconds' 12 s default by 3 s;
        // honoured by BOTH hosts (a flag one host silently ignores is a trap).
        std::uint64_t   hangMainFrame = 0;
```

and, at namespace scope in the same header (outside the struct, Dist-guarded): `inline constexpr std::uint32_t kHangMainSeconds = 15;`.

`HostConfig.cpp`: `kStripWithValue` gains `"hang-main"`; beside the `crash-gpu` Option: `cli.Option("hang-main", "0", "DEV: on frame N block the main thread for 15 s without beating (0 = off) -- the hang-report desk trigger").Type(CliType::Uint);`; beside the store: `cfg.hangMainFrame = r.GetAs<std::uint64_t>("hang-main");`.

- [ ] **Step 4: The two hosts**

`RuntimeFrame.hpp` `FrameIo`, after `gpuFaultFired`: `bool& hangMainFired;` (same `#if !defined(ARCANE_DIST)` block). `RuntimeApp.hpp` after `m_gpuFaultFired`: `bool m_hangMainFired = false;`; `RuntimeApp.cpp:749` binding gains `.hangMainFired = m_hangMainFired,`. `RuntimeFrame.cpp`, right after the `--crash-gpu` block inside the same `#if`:

```cpp
    // --hang-main N (crash window plan 2, D10): stop beating on purpose. The
    // sleep sits here, after the beat PumpAndResize published for this frame,
    // so the watchdog sees a beat that then goes stale for kHangMainSeconds.
    if (io.config.hangMainFrame != 0 && !io.hangMainFired && io.frameCount >= io.config.hangMainFrame)
    {
        io.hangMainFired = true;   // set FIRST: exactly once
        ARC_WARN("--hang-main: blocking the main thread for {} s without a heartbeat", Arcane::kHangMainSeconds);
        std::this_thread::sleep_for(std::chrono::seconds(Arcane::kHangMainSeconds));
    }
```

`EditorApp.hpp` after `m_gpuFaultFired`: `bool m_hangMainFired = false;`; `EditorAppFrame.cpp`, after the `--crash-gpu` block inside the same `#if`, the same four lines over `m_config.hangMainFrame` / `m_hangMainFired` / `m_frameCount`. Both files include `<thread>` and `<chrono>` if not already.

- [ ] **Step 5: Build, run, commit**

Regenerate; full build. `"[host]"` (exit 0), `"H1*"` (~20 s, exit 0), `"[witness][gpu]"` (7 cases: 6 + H1; G1 skips without the env), then `ARCANE_DIAG_DESK=1 ./ArcaneTests.exe "G1*"` on the desk (a TDR: expect the display to blink; exit 0). Print exit codes.

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.hpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneRuntime/src/RuntimeFrame.hpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneRuntime/src/RuntimeApp.hpp ArcaneRuntime/src/RuntimeApp.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/HostConfigTest.cpp ArcaneTests/src/CrashWitnessTest.cpp
git commit -m "test(diagnostics): --hang-main on both hosts and the H1/G1 witness lanes prove the hang and gpu-crash hand-offs through the staged reporter (crash window plan 2, task 10)"
```

---

### Task 11: ABI bump, desk proof, close-out

**Files:**
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:937` (`kGamePluginABIVersion = 40`), `ReferenceProject/ReferenceProject.arcproj:6` (`"abi": 40`)
- Modify: `docs/specs/2026-09-22-crash-window-design.md` (status, plan 2 measurements, as-built notes)
- Modify: the memory file `project_crash_window_arc.md` (the controller does this at the end, not the implementer)

- [ ] **Step 1: ABI**

Bump both files. Rebuild everything, then `arcbuild build --project ReferenceProject --config Debug` with `ARCANE_SDK` = the worktree, and copy `ReferenceProject/Binaries` into the three staged host slots. The ABI gate refuses a stale module; `[witness][server]` 3/3 is the proof the restage landed.

- [ ] **Step 2: The whole suite on the head**

From the exe dir, exit codes printed: `~[gpu]` (expect the plan-1 baseline plus every case this plan added: `[platform]` 3, `[reporter]` 15, `[diag]` +5, `[host]` +1 -- derive the number from the run, never recall it), `[witness][gpu]` (7 incl. H1), `[witness][server]` 3/3, `scripts/golden-gate.ps1` 8/8, and `"death fixture*"` alone. Any red stops here.

- [ ] **Step 3: Desk proof (spec §10 "Desk")**

Record wall times and outcomes in the task report:

1. The monitor's real case on this desk: `ArcaneEditor.exe --project ReferenceProject --backend vulkan` WINDOWED (GPU Tweak III's `GTIII-OSD64-VK.dll` hook aborts the loader with 0xC0000409 -- memory `project_gtiii_osd_vulkan_windowed_crash`). Expected: the editor dies with no report of its own; within a second the monitor's window says "Arcane Editor exited abnormally", reason `abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN`, the log tail names the injected module from the host's `ForeignModules::Report` line if the device got that far; the report lands in `<exe dir>/diagnostics` (D8). Relaunch re-runs the same command line. If the desk no longer reproduces the crash (driver/overlay changed), say so and fall back to `death-fixture --die fastfail --monitor --attended`.
2. The assert: `death-fixture --die assert --reporter --attended` -> window shows `assert: false -- fixture assert (DeathFixtureMain.cpp:53)` (the exact line from the envelope's `reason`) and `death_fixture!main` in the faulting thread.
3. The hang: `death-fixture --hang 60 --hang-seconds 2 --reporter --attended` -> Terminate and Collect -> fixture exit 11; then Keep Waiting on a second run -> fixture exit 0 at 60 s, reporter exit 0.
4. Windowed editor, dx12, normal close -> no `ArcaneCrashReporter.exe` lingers (Task Manager / `tasklist | findstr ArcaneCrashReporter` empty after 5 s).
5. Headless editor `--backend dx12 --frames 900` -> exit 0, no monitor launched (the log's "Diagnostics armed" line says `monitor off`).

- [ ] **Step 4: Spec close-out**

In `docs/specs/2026-09-22-crash-window-design.md`: status "Plan 2 (reporter + NativeWindow + monitor mode) implemented <date> on `feat/crash-window-plan-2`, commit <sha>; plan 3 (autosave) pending"; a "Plan 2 measurements" block mirroring plan 1's (the desk items above, the H1 wall time, the symbolization time of the fixture's dump with and without PDBs, the suite counts); and "(plan 2 as built)" notes at §4 (envelope `reason`; `--host-created`; `--relaunch` only on the monitor line), §5.4 (D6, D11, D12), §5.8 (D4, D8), §6 (the reporter's exit codes; `--symbol-path`/`--deadline` seams; the fatal echo D9), §7 (the presenter interface as shipped), §10 (H1 in the gate, G1 desk-gated), §13 (owed: `CrashReportDocument` showing `.symbolized.txt`; the Hub decoding 10-13; the editor's Console sink into the dist sink; Release-config fixture rows; the monitor's pre-retarget directory).

- [ ] **Step 5: Commit**

```bash
git add ArcaneCore/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj docs/specs/2026-09-22-crash-window-design.md
git commit -m "docs(diagnostics): crash window plan 2 closed -- ABI 40, measurements, and the as-built amendments for the reporter, NativeWindow and monitor mode"
```

Then `superpowers:finishing-a-development-branch`: the user merges (`--ff-only` onto main, as plan 1) and pushes; the controller updates `project_crash_window_arc.md` (plan 2 done, plan 3 next: autosave §8, via writing-plans) and rebuilds main's stale `bin/`.
