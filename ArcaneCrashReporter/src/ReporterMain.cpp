// ArcaneCrashReporter -- the out-of-process crash reporter (spec §6).
//
// Report mode: read the envelope the host wrote, symbolize its minidump out of
// process, write <stem>.symbolized.txt, then show the window (or exit,
// unattended). Monitor mode (task 9): wait on a host pid and turn an
// unrecognised exit code into an abnormal-exit report.
//
// This process is deliberately MINIMAL: it links ArcaneCore.dll and the Win32
// debug libraries and NOTHING else -- never ArcaneClient, no GPU, no ImGui --
// because it has to be able to run when the host it reports on is already
// dead, and anything it shares with that host is something that can be broken
// in the same way. It is staged beside every host by that host's own postbuild
// (spec §12 item 2), which is how Diagnostics::ResolveReporterPath finds it at
// "<exe dir>/ArcaneCrashReporter.exe".
#include "FileText.hpp"
#include "LogTail.hpp"
#include "ReportView.hpp"
#include "ReporterArgs.hpp"
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

#include <chrono>
#include <condition_variable>
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

    // R67: Install/Shutdown bound by SCOPE, not by control flow.
    // Diagnostics.hpp:109-112 is explicit that Install creates the crash thread
    // and its events even with installCrashHandler and startHangWatchdog both
    // off, and the death fixture's own comment records what skipping Shutdown
    // cost last time: a joinable thread at static destruction, whose destructor
    // calls std::terminate.
    //
    // R80 corrects what this comment used to claim. Task 5's kDeadline does NOT
    // return from inside the guarded window -- it ends the process with
    // TerminateProcess, so this guard never runs on that path. That is the
    // CORRECT behaviour, not an oversight: TerminateProcess runs no destructors
    // by design, and calling Shutdown() under an already-expired deadline could
    // itself block on the very worker the deadline just gave up on. The guard
    // stays because task 8's kHostMismatch IS a real return from this scope,
    // and because the ordinary success path still needs it.
    struct ArmedDiagnostics
    {
        explicit ArmedDiagnostics(const Arcane::Diagnostics::Config& cfg) { Arcane::Diagnostics::Install(cfg); }
        ~ArmedDiagnostics() { Arcane::Diagnostics::Shutdown(); }

        ArmedDiagnostics(const ArmedDiagnostics&)            = delete;
        ArmedDiagnostics& operator=(const ArmedDiagnostics&) = delete;
    };

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

    // Runs on the window thread (it IS `onCommand`, ReporterWindow's ctor
    // contract). The folder and the relaunch line are read from the window's
    // CURRENT view -- SetView may have replaced the initial one by the time a
    // button fires -- through ReporterWindow's mutex-guarded accessors
    // (R40), never captured once at window creation.
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
        default: break;   // task 8: kBtnKeepWaiting / kBtnTerminate / kHostExited / kHostRecovered
        }
    }

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
        std::unique_ptr<ReporterWindow> ui;
        NativeWindow                    window;
        std::string                     logTail;
        if (!a.unattended)
        {
            logTail = ReadLogTail(stem, std::filesystem::path(ToWide(envelope->logPath)), 200);
            const ReportView initial = BuildReportView(*envelope, a, nullptr, logTail);
            ui = std::make_unique<ReporterWindow>(initial, [&](int id) { OnButton(id, *ui, window); });
            ui->Show(window, a.product);
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

            const bool symbolizedInTime = [&]
            {
                std::unique_lock<std::mutex> lk(m);
                return cv.wait_for(lk, std::chrono::seconds(a.deadlineSeconds), [&] { return done; });
            }();

            if (!symbolizedInTime && a.unattended)
            {
                // R39: guarded on `a.unattended` -- an ATTENDED run lets the
                // worker keep going while the user looks at the window (which
                // already shows "Symbolizing..."); only a headless run is
                // bound by the deadline at all. When attended and the
                // deadline has passed, control falls straight through to the
                // unconditional `worker.join()` below and simply waits
                // longer, same as if no deadline had been set.
                //
                // TerminateProcess, not a return: the worker captured `result`,
                // `m`, `cv` and `done` BY REFERENCE off this frame. Returning
                // would unwind them under a thread still writing to them, and
                // ~std::thread on a joinable thread calls std::terminate
                // anyway. Ending the process here is the only exit that is
                // sound with a wedged worker -- and it is exactly what the
                // deadline promised.
                // The one place this task DISCARDS a write result, deliberately
                // (cf. R64 below): the process is about to end with kDeadline,
                // which already names a failure, and kWriteFailed cannot also
                // be returned. The expired deadline is the more informative
                // cause of the two, so it is the one the exit code carries.
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
            }
            worker.join();
        }

        // The engine's own verdict leads; the envelope's walked thread is the
        // fallback for a dump with no stored event (see PutFaultingFirst).
        PutFaultingFirst(result, ParseWalkedThreadId(envelope->cpuThreadSummary));

        // R64: the return value is ACTED ON, never discarded. This sibling is
        // the whole artifact of the hand-off; a silent kOk after a failed
        // write is a lie a parent (and the [diag] hand-off case) would believe.
        if (!WriteText(sibling, FormatSymbolized(result, Arcane::BuildInfo(), envelope->cpuThreadSummary)))
        {
            ARC_ERROR("reporter: cannot write '{}'", ToUtf8(sibling.wstring()));
            return ExitCode::kWriteFailed;
        }

        // The window (when attended) gets the finished view -- the same
        // `logTail` read before symbolization started, not a second read.
        if (ui) ui->SetView(BuildReportView(*envelope, a, &result, logTail));

        // Block here, not on ~NativeWindow: an attended run is done only once
        // the user closes the window (a button, Esc, or the system menu);
        // Shutdown() below must not run while that window is still up.
        if (ui) window.Wait();

        return ExitCode::kOk;
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
    if (parsed.args->mode == Args::Mode::Monitor)
        return ExitCode::kBadArgs;   // TASK 9: RunMonitor(*parsed.args)
    return RunReport(*parsed.args);
}
