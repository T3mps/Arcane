// ArcaneCrashReporter -- the out-of-process crash reporter (spec §6).
//
// Report mode: read the envelope the host wrote, symbolize its minidump out of
// process, write <stem>.symbolized.txt, then show the window (or exit,
// unattended). Monitor mode (task 9, Monitor.cpp): wait on the host's process
// handle and turn a death the crash path never saw into an abnormal-exit
// report.
//
// This process is deliberately MINIMAL: it links ArcaneCore.dll and the Win32
// debug libraries and NOTHING else -- never ArcaneClient, no GPU, no ImGui --
// because it has to be able to run when the host it reports on is already
// dead, and anything it shares with that host is something that can be broken
// in the same way. It is staged beside every host by that host's own postbuild
// (spec §12 item 2), which is how Diagnostics::ResolveReporterPath finds it at
// "<exe dir>/ArcaneCrashReporter.exe".
#include "FileText.hpp"
#include "HangSession.hpp"
#include "LogTail.hpp"
#include "Monitor.hpp"
#include "ReportView.hpp"
#include "ReporterArgs.hpp"
#include "ReporterShared.hpp"
#include "ReporterWindow.hpp"
#include "SymbolizedText.hpp"
#include "Symbolizer.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>

#include <shellapi.h>   // CommandLineToArgvW

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace Arcane::Reporter;
    using Arcane::NativeWindow;   // Arcane::Reporter is a child namespace of Arcane, not a re-export of it

    std::vector<std::string> ArgvUtf8()
    {
        int       argc = 0;
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

    // ArmedDiagnostics (R67/R80) lives in ReporterShared.hpp: monitor mode
    // installs this process's own Diagnostics the same way (task 9).

    // The path of the sibling this reporter writes beside <stem>.
    //
    // R64: built by APPENDING TO A PATH, never by `stem.string() + "..."`.
    // MSVC's path::string() narrows through CP_ACP and substitutes '?' for
    // anything unmappable, so on a machine whose profile path carries
    // non-ACP characters that concatenation produces a name NTFS rejects --
    // and the reporter would then "succeed" having written the only artifact
    // of the whole hand-off nowhere. operator+= appends without a separator
    // and keeps the native wide form, which the ofstream path overload then
    // opens as-is.
    std::filesystem::path SymbolizedSiblingPath(const std::filesystem::path& stem)
    {
        std::filesystem::path sibling = stem;
        sibling += ".symbolized.txt";
        return sibling;
    }
}

namespace Arcane::Reporter
{
    // The hang protocol's reporter half (plan 2, task 8; spec s5.4, D6/D7,
    // s9 "Reporter pid reuse"). Exists ONLY for an attended run whose view is
    // a hang: an unattended hang report waits on nothing (symbolize, write,
    // exit 0 -- nobody is there to choose, and the host's once-per-stall rule
    // stands), and a crash view has no live host to watch.
    //
    // `host` is opened ONCE, at start, and the identity check runs against
    // THAT handle: a process handle names one process object for its whole
    // life, so once the creation time matched, a later TerminateProcess on it
    // can never reach a stranger that inherited the pid. The check is repeated
    // before TerminateProcess anyway -- it is one syscall, and it is the line
    // D7 names.
    struct HangWatch
    {
        HANDLE           host      = nullptr;   // SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION
        HANDLE           recovered = nullptr;   // the host's Local\Arcane-Recovered-<pid>, or null (no --recovered-event)
        HANDLE           closing   = nullptr;   // R36: manual-reset, set by THIS process when its window is done
        std::uint64_t    hostCreated = 0;       // --host-created; 0 = an older host that did not send it
        std::thread      waiter;
        std::atomic<int> outcomeExit{ -1 };     // -1 = no hang outcome chose an exit code; else a Reporter::ExitCode

        HangWatch() = default;
        HangWatch(const HangWatch&)            = delete;
        HangWatch& operator=(const HangWatch&) = delete;
        ~HangWatch()
        {
            Stop();
            if (host)      CloseHandle(host);
            if (recovered) CloseHandle(recovered);
            if (closing)   CloseHandle(closing);
        }

        // D7: the handle we opened is the host we were told about. A host
        // that sent no creation time (0) is taken by pid, as before D7.
        [[nodiscard]] bool IsTheHost() const
        {
            if (!host) return false;
            if (hostCreated == 0) return true;
            FILETIME c{}, e{}, k{}, u{};
            return GetProcessTimes(host, &c, &e, &k, &u) &&
                   ((static_cast<std::uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime) == hostCreated;
        }

        // The waiter: ONE wait, ONE posted event, then done. Everything it
        // learns goes to the window thread as a PostUser -- never a direct
        // call into ReporterWindow -- so every hang decision is made on that
        // one thread, and a message that lands after the window died is
        // simply dropped (PostUser to a null/dead HWND).
        //
        // R36: `closing` is always in the array, which is what lets Stop()
        // end the wait without TerminateThread. The array is built COMPACTLY
        // -- with no recovered event, `closing` is index 1, not 2 -- so the
        // index each handle landed at is recorded and the result is mapped
        // back through those, never through fixed positions. Index order is
        // also priority order (WaitForMultipleObjects reports the LOWEST
        // signalled index): a host that exited outranks a beat that resumed.
        void Start(NativeWindow& window)
        {
            waiter = std::thread([this, &window]
            {
                HANDLE      handles[3]{};
                DWORD       n         = 0;
                const DWORD hostAt    = n; handles[n++] = host;
                DWORD       recoverAt = MAXDWORD;
                if (recovered) { recoverAt = n; handles[n++] = recovered; }
                handles[n++] = closing;

                const DWORD r = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
                if (r == WAIT_OBJECT_0 + hostAt)
                {
                    DWORD code = 0;
                    GetExitCodeProcess(host, &code);
                    window.PostUser(ReporterWindow::kUserHostExited, code);
                }
                else if (recoverAt != MAXDWORD && r == WAIT_OBJECT_0 + recoverAt)
                {
                    window.PostUser(ReporterWindow::kUserHostRecovered);
                }
                else if (r == WAIT_FAILED)
                {
                    // R97: post nothing -- but say so. Without this line the
                    // window would quietly stop reacting to a recovery or a
                    // host exit, with no trace of why.
                    ARC_WARN("reporter: waiting on the host failed ({}); recovery and host exit will not be noticed",
                             GetLastError());
                }
                // `closing` (this process is done): post nothing.
            });
        }

        // R36: set right after the attended wait returns, before the join --
        // the host may well still be alive (Keep Waiting, Close), and the
        // waiter would otherwise block this join forever. Idempotent.
        void Stop()
        {
            if (closing) SetEvent(closing);
            if (waiter.joinable()) waiter.join();
        }
    };
}

namespace
{
    using namespace Arcane::Reporter;
    using Arcane::NativeWindow;

    // "0xC0000005" for an NTSTATUS-shaped code, plain decimal for a small one
    // (an ordinary exit code reads better as "1" than "0x00000001").
    std::string HostCodeText(std::uint32_t code)
    {
        char buf[32];
        if (code > 0xFFFFu) std::snprintf(buf, sizeof(buf), "0x%08X", code);
        else                std::snprintf(buf, sizeof(buf), "%u", code);
        return buf;
    }

    // Applies one hang decision (HangSession.hpp's pure table) to the live
    // window and host. Window thread only -- it is reached from OnButton,
    // i.e. from ReporterWindow's onCommand, for both the two hang buttons and
    // the three posted host events.
    void ApplyHang(HangEvent event, std::uint32_t hostExitCode, ReporterWindow& ui, HWND hwnd, HangWatch& hang)
    {
        const HangOutcome o = DecideHang(event, hostExitCode);
        if (o.terminateHost)
        {
            // D7 / spec s9: never terminate a process that is not the host we
            // were told about. A mismatch here is the IdentityMismatch row:
            // close, exit 4 (Reporter::ExitCode::kHostMismatch).
            if (!hang.IsTheHost())
            {
                ARC_ERROR("reporter: the process behind the pid is not the host that reported the hang; terminate refused");
                ApplyHang(HangEvent::IdentityMismatch, 0, ui, hwnd, hang);
                return;
            }
            if (!TerminateProcess(hang.host, static_cast<UINT>(Arcane::Diagnostics::ExitCode::kHangTerminated)))
            {
                // The host may have exited on its own a moment ago -- the
                // waiter's HostExited will say so. Either way the window
                // stays as it is rather than claiming a termination.
                ARC_ERROR("reporter: TerminateProcess on the host failed ({})", GetLastError());
                return;
            }
        }
        if (o.becomeCrashView)
            ui.BecomeCrashView(o.terminateHost ? std::string(" -- terminated and collected")
                                               : " -- the host exited (" + HostCodeText(hostExitCode) + ")");
        if (o.closeWindow)
        {
            hang.outcomeExit.store(o.exitCode);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    }
}

namespace Arcane::Reporter
{
    // Runs on the window thread (it IS `onCommand`, ReporterWindow's ctor
    // contract). The folder and the relaunch line are read from the window's
    // CURRENT view -- SetView may have replaced the initial one by the time a
    // button fires -- through ReporterWindow's mutex-guarded accessors
    // (R40), never captured once at window creation.
    //
    // R99 (task 9): external linkage, declared in ReporterShared.hpp, so the
    // monitor's window runs the same handler with `hang = nullptr`.
    void OnButton(int id, ReporterWindow& ui, NativeWindow& window, HangWatch* hang)
    {
        HWND hwnd = static_cast<HWND>(window.Hwnd());
        switch (id)
        {
        case ReporterWindow::kBtnOpenFolder:
        {
            // Fix round 1 (R91 minor 3): log a failed open (<= 32 is a real
            // SE_ERR_*/ERROR_* code from ShellExecuteW) instead of ignoring it.
            const INT_PTR rc = OpenFolder(ToWide(ui.ReportFolder()));
            if (rc <= 32) ARC_WARN("reporter: could not open the report folder (ShellExecute rc {})", static_cast<long long>(rc));
            break;
        }
        case ReporterWindow::kBtnCopy:
            // Fix round 1 (R91 minor 3): log a failed copy instead of ignoring it.
            if (!CopyToClipboard(hwnd, ui.CurrentDetails())) ARC_WARN("reporter: could not copy details to the clipboard");
            break;
        case ReporterWindow::kBtnRelaunch:
        {
            // Fix round 1 (R90): a failed relaunch must not be swallowed and
            // must not close the window -- the user still has Open Folder
            // and Copy Details to fall back on. On success,
            // AllowSetForegroundWindow lets the relaunched host come to the
            // front (D14 applies to it too, same as the reporter itself).
            const std::string line = ui.RelaunchLine();
            DWORD             childPid = 0;
            if (SpawnDetached(line, &childPid))
            {
                AllowSetForegroundWindow(childPid);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            else
            {
                ARC_ERROR("reporter: relaunch failed ({}): {}", GetLastError(), line);
            }
            break;
        }
        case ReporterWindow::kBtnClose:
            // ON the window thread: PostMessageW, never window.Close() here --
            // NativeWindow::Close() called from its own thread only posts
            // WM_CLOSE and returns without joining (R51), so posting directly
            // is the documented idiom with one less indirection, and calling
            // it from OnCreate specifically would block forever (Close()
            // waits on `ready`, which OnCreate itself has to return from
            // first) -- not the case here, but the rule is "never from inside
            // a presenter callback that runs before the window is ready".
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        // Task 8: the two hang buttons and the three host events. `hang` is
        // null outside an attended hang view -- the buttons are hidden there
        // and nothing posts host events -- so a stray id is ignored.
        case ReporterWindow::kBtnKeepWaiting: if (hang) ApplyHang(HangEvent::KeepWaitingChosen, 0, ui, hwnd, *hang); break;
        case ReporterWindow::kBtnTerminate:   if (hang) ApplyHang(HangEvent::TerminateChosen,   0, ui, hwnd, *hang); break;
        case ReporterWindow::kHostRecovered:  if (hang) ApplyHang(HangEvent::HostRecovered,     0, ui, hwnd, *hang); break;
        case ReporterWindow::kHostMismatch:   if (hang) ApplyHang(HangEvent::IdentityMismatch,  0, ui, hwnd, *hang); break;
        case ReporterWindow::kHostExited:
            if (hang) ApplyHang(HangEvent::HostExited, ui.LastHostExitCode(), ui, hwnd, *hang);
            break;
        default: break;
        }
    }
}

namespace
{
    using namespace Arcane::Reporter;
    using Arcane::NativeWindow;

    int RunReport(const Args& a)
    {
        const std::filesystem::path envelopePath = std::filesystem::path(ToWide(a.envelopePath));
        const auto envelope = Arcane::Diag::ReadFile(envelopePath);
        if (!envelope) { ARC_ERROR("reporter: cannot read envelope '{}'", a.envelopePath); return ExitCode::kNoEnvelope; }
        const std::filesystem::path stem = StemOf(envelopePath);

        // Diagnostics for the reporter ITSELF (spec §6 failure modes): same
        // folder, never spawns a reporter (D13).
        //
        // R70: THIS EXE is what Diagnostics::ResolveReporterPath defaults
        // `reporterPath` to ("<exe dir>/ArcaneCrashReporter.exe"), so the
        // `spawnReporter = false` below is the ONLY thing standing between a
        // reporter that crashes and a reporter that spawns itself, forever.
        // It must never be set true here, and it must survive tasks 7 and 9 --
        // both of which edit this function.
        Arcane::Diagnostics::Config diag;
        diag.appName           = "ArcaneCrashReporter";
        diag.productName       = "Arcane Crash Reporter";
        diag.dumpDir           = stem.parent_path().string();
        diag.unattended        = a.unattended;
        diag.spawnReporter     = false;   // R70: never true -- see above
        diag.startHangWatchdog = false;
        const ArmedDiagnostics armed(diag);

        const std::filesystem::path sibling = SymbolizedSiblingPath(stem);

        // TASK 7: the window goes up now, BEFORE symbolization starts, so an
        // attended run has it on screen within about a second saying
        // "Symbolizing..." rather than waiting on dbgeng. `logTail` is read
        // once here and reused for the final view below (R86: no host change
        // needed to bound this -- the reporter's own --deadline default is
        // spec §6's 60s) rather than a second file read the unattended path
        // would never need.
        //
        // R34: `ui` is declared BEFORE `window`, so `window` -- declared
        // second -- is destroyed FIRST at scope exit. NativeWindow's dtor
        // joins the window thread; only once that join returns can nothing
        // still be running the `[&]` lambda below, which dereferences `*ui`.
        // Declaring them the other way round would destroy `ui` while the
        // window thread could still be alive to call into it.
        //
        // Task 8: `hang` is declared before both, because the `[&]` lambda
        // reaches it too -- but it is STOPPED explicitly right after
        // window.Wait() below (R36), not left to its destructor, since its
        // waiter posts into `window` and `window` is destroyed first.
        std::unique_ptr<HangWatch>      hang;
        std::unique_ptr<ReporterWindow> ui;
        NativeWindow                    window;
        std::string                     logTail;

        // R97: the waiter posts into `window`, so it must be stopped and
        // joined before `window` is destroyed on EVERY path out of this
        // scope. The normal path does that explicitly after window.Wait();
        // this guard, declared AFTER `window` (so destroyed BEFORE it), covers
        // an exception unwinding RunReport in between. Armed right after
        // HangWatch::Start; Stop() is idempotent, so the normal path's
        // explicit call and this one never conflict.
        struct StopWaiterOnUnwind
        {
            HangWatch* watch = nullptr;
            ~StopWaiterOnUnwind() { if (watch) watch->Stop(); }
        } stopWaiterOnUnwind;
        if (!a.unattended)
        {
            logTail = ReadLogTail(stem, std::filesystem::path(ToWide(envelope->logPath)), 200);
            const ReportView initial = BuildReportView(*envelope, a, nullptr, logTail);
            ui = std::make_unique<ReporterWindow>(initial, [&](int id) { OnButton(id, *ui, window, hang.get()); });

            // The hang protocol (spec s5.4): the handles are opened BEFORE the
            // window exists, so a click can never find `hang` half-built; the
            // closing event exists before any waiter does (R36).
            if (initial.isHang)
            {
                hang = std::make_unique<HangWatch>();
                hang->host        = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a.pid);
                hang->recovered   = a.recoveredEvent.empty() ? nullptr : OpenEventW(SYNCHRONIZE, FALSE, ToWide(a.recoveredEvent).c_str());
                hang->closing     = CreateEventW(nullptr, /*manualReset*/TRUE, FALSE, nullptr);
                hang->hostCreated = a.hostCreated;
            }

            ui->Show(window, a.product);

            // Only once the window is genuinely up: before that there is no
            // one to post to, and a window that never opened is the
            // unattended case (R89's third bullet), which waits on nothing.
            if (hang && window.WasEverOpen())
            {
                if (!hang->host)
                {
                    // Gone already (or not ours to open). Nothing to wait on
                    // and nothing Terminate could reach: this is a crash view.
                    ARC_WARN("reporter: cannot open host pid {} ({}); showing the report as a crash view", a.pid, GetLastError());
                    ui->BecomeCrashView(" -- the host could not be reached");
                }
                else if (!hang->IsTheHost())
                {
                    // D7: the pid was recycled -- the host that hung is gone
                    // and this is a stranger. The IdentityMismatch row, on the
                    // window thread like every other hang decision.
                    ARC_ERROR("reporter: pid {} is not the host that reported the hang (creation time differs)", a.pid);
                    window.PostUser(ReporterWindow::kUserHostMismatch);
                }
                else if (hang->closing)
                {
                    hang->Start(window);
                    stopWaiterOnUnwind.watch = hang.get();
                }
                else
                {
                    // R36: without a closing event a waiter could only be
                    // ended by TerminateThread, which is never used. The
                    // buttons still work; recovery and a host exit are just
                    // not noticed.
                    ARC_WARN("reporter: cannot create the closing event ({}); not watching the host", GetLastError());
                }
            }
        }

        // Symbolize on a WORKER under the unattended deadline (spec §6). The
        // engine can block for an unbounded time inside symbol loading -- a
        // symbol server, a dead UNC path, a huge PDB -- and a headless gate
        // must never inherit an open-ended child. This deadline is the ONLY
        // bound on SymbolizeDump; the 30 s WaitForEvent inside it covers the
        // open, not the walk.
        Symbolized result;
        if (envelope->siblingDmp.empty())
        {
            // A lightweight (ensure) report writes no dump on purpose, so
            // there is nothing to open and no reason to pay for a thread: the
            // portable stack IS the answer, and the header says why.
            result.engineError = "no minidump (lightweight report)";
        }
        else
        {
            SymbolizeOptions opt;
            opt.symbolPath = a.symbolPath;
            // D5: an explicit --symbol-path REPLACES the search, which only
            // means anything if the PDB path the linker embedded in the image
            // is also ignored. Coupled deliberately -- the two are one seam.
            opt.ignoreCvRecord = !a.symbolPath.empty();

            std::mutex              m;
            std::condition_variable cv;
            bool                    done = false;
            std::thread             worker([&]
            {
                Symbolized r = SymbolizeDump(std::filesystem::path(ToWide(envelope->siblingDmp)), opt);
                std::lock_guard<std::mutex> lk(m);
                result = std::move(r);
                done   = true;
                cv.notify_all();
            });

            // Fix round 1 (R89, Important 1): R39 exempted an attended run
            // from the deadline entirely, then fell into an unconditional
            // worker.join() -- if the worker wedges and the user closes the
            // window, the process was left running forever with no UI (and,
            // under D12, blocking every later report from this host: the
            // host will not spawn a second reporter while this one's handle
            // still reports WAIT_TIMEOUT).
            //
            // The deadline is measured from HERE (worker start), not from
            // whenever the window happens to close -- an attended run that
            // spent 55 of its 60 seconds with the window open does not get a
            // fresh 60 when the window closes.
            const auto symbolizeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(a.deadlineSeconds);

            // TerminateProcess(kDeadline) after writing whatever the worker
            // has NOT produced -- the ONE place both the plain-unattended
            // path and the "attended, window closed, worker still wedged"
            // path end, so neither can drift out of step with the other
            // (R89 asks for exactly this factoring).
            //
            // TerminateProcess, not a return: the worker captured `result`,
            // `m`, `cv` and `done` BY REFERENCE off this frame. Returning
            // would unwind them under a thread still writing to them, and
            // ~std::thread on a joinable thread calls std::terminate anyway.
            // Ending the process here is the only exit that is sound with a
            // wedged worker -- and it is exactly what the deadline promised.
            // The one place this task DISCARDS a write result, deliberately
            // (cf. R64 below): the process is about to end with kDeadline,
            // which already names a failure, and kWriteFailed cannot also be
            // returned. The expired deadline is the more informative cause
            // of the two, so it is the one the exit code carries.
            auto expireDeadline = [&]
            {
                Symbolized partial;
                partial.engineError = "deadline of " + std::to_string(a.deadlineSeconds) + " s expired";
                (void)WriteText(sibling, FormatSymbolized(partial, Arcane::BuildInfo(), envelope->cpuThreadSummary));
                ARC_WARN("reporter: symbolization did not finish within {} s; wrote the portable stack", a.deadlineSeconds);
                Arcane::Log::FlushFileSinkBounded(1000);

                // R79: unreachable BY CONSTRUCTION, not by argument. The
                // argument -- TerminateProcess on the current process cannot
                // fail, because the pseudo-handle always carries
                // PROCESS_TERMINATE -- is true, but if it ever DID return,
                // control fell straight into worker.join() and blocked forever
                // on the wedged worker: precisely the outcome this deadline
                // exists to prevent, arrived at through the code that
                // implements it.
                //
                // A loop with no exit removes the fall-through from the
                // grammar. Retrying is also the only sensible response to a
                // failure that cannot happen: the alternatives all end in
                // waiting on the worker, and there is nothing else this
                // process should be doing.
                for (;;) TerminateProcess(GetCurrentProcess(), static_cast<UINT>(ExitCode::kDeadline));
            };

            bool symbolizedInTime = false;
            if (ui && window.WasEverOpen())
            {
                // Attended, and the window genuinely opened (R89's third
                // bullet: a window that never opened behaves exactly like
                // unattended, handled by the `else` below). Poll in short
                // slices ONLY so this loop can notice the window closing --
                // not to impose a bound: while the window stays open the
                // wait is UNBOUNDED, deadline or not, because the user is
                // looking at "Symbolizing..." and may be waiting on a slow
                // symbol server for perfectly good reasons.
                for (;;)
                {
                    {
                        std::unique_lock<std::mutex> lk(m);
                        symbolizedInTime = cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return done; });
                    }
                    if (symbolizedInTime) break;
                    if (window.IsOpen()) continue;

                    // The window just closed and the worker is still
                    // running: from here this is the unattended case,
                    // except bounded by whatever remains of the deadline
                    // computed above (measured from worker start), not a
                    // fresh window's worth of time.
                    const auto now = std::chrono::steady_clock::now();
                    if (now < symbolizeDeadline)
                    {
                        std::unique_lock<std::mutex> lk(m);
                        symbolizedInTime = cv.wait_for(lk, symbolizeDeadline - now, [&] { return done; });
                    }
                    break;
                }
            }
            else
            {
                // Unattended, OR the window never actually opened
                // (WasEverOpen() == false: R89's third bullet).
                std::unique_lock<std::mutex> lk(m);
                symbolizedInTime = cv.wait_for(lk, std::chrono::seconds(a.deadlineSeconds), [&] { return done; });
            }

            if (!symbolizedInTime) expireDeadline();   // never returns
            worker.join();
        }

        // The engine's own verdict leads; the envelope's walked thread is the
        // fallback for a dump with no stored event (see PutFaultingFirst).
        PutFaultingFirst(result, ParseWalkedThreadId(envelope->cpuThreadSummary));

        // R64: the return value is ACTED ON, never discarded. This sibling is
        // the whole artifact of the hand-off; a silent kOk after a failed
        // write is a lie a parent (and the [diag] hand-off case) would believe.
        const bool writeOk = WriteText(sibling, FormatSymbolized(result, Arcane::BuildInfo(), envelope->cpuThreadSummary));
        if (!writeOk) ARC_ERROR("reporter: cannot write '{}'", ToUtf8(sibling.wstring()));

        // Fix round 1 (R91 minor 1): the window (when attended) gets the
        // finished view and stays open regardless of whether the sibling
        // write succeeded -- the symbolized result is already in memory, and
        // a write failure is no reason to vanish the window out from under a
        // user still reading "Symbolizing..." (the ORIGINAL code returned
        // kWriteFailed straight from inside the `if`, before SetView/Wait
        // ever ran, so ~NativeWindow closed a window that still said
        // "Symbolizing..."). Same `logTail` read before symbolization
        // started, not a second read.
        if (ui) ui->SetView(BuildReportView(*envelope, a, &result, logTail));

        // Block here, not on ~NativeWindow: an attended run is done only once
        // the user closes the window (a button, Esc, or the system menu);
        // Shutdown() below must not run while that window is still up.
        if (ui) window.Wait();

        // R36: the waiter is released and joined HERE -- after the window is
        // gone, before anything it captured (`window` above all) goes out of
        // scope.
        if (hang) hang->Stop();

        // Task 8: the exit code. Precedence, in order:
        //   4 (kHostMismatch) -- a genuine identity mismatch is the one hang
        //     outcome that names a failure, and the more specific one: it says
        //     this reporter REFUSED an action, which a parent checking the
        //     code needs to know even if the sibling write also failed;
        //   6 (kWriteFailed)  -- otherwise a failed sibling write still
        //     surfaces (R64), whatever the hang outcome was: every other
        //     outcome carries 0, and 0 must not paper over a missing artifact;
        //   0 (kOk).
        // Returning here (never exiting) is also what routes the mismatch
        // through ArmedDiagnostics' scope guard (R67).
        if (hang && hang->outcomeExit.load() == Arcane::Reporter::ExitCode::kHostMismatch)
            return Arcane::Reporter::ExitCode::kHostMismatch;
        return writeOk ? ExitCode::kOk : ExitCode::kWriteFailed;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();

    const std::vector<std::string> argv   = ArgvUtf8();
    const ParseResult              parsed = ParseArgs(argv);
    if (!parsed.args)
    {
        ARC_ERROR("reporter: {}\n{}", parsed.error, Usage());
        return ExitCode::kBadArgs;
    }
    // A monitor line that parsed is a monitor that runs: RunMonitor returns
    // Reporter::ExitCode::kOk whether or not it had anything to say.
    if (parsed.args->mode == Args::Mode::Monitor)
        return RunMonitor(*parsed.args);
    return RunReport(*parsed.args);
}
