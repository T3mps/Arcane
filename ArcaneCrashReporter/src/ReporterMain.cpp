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
#include "ReporterArgs.hpp"
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
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace Arcane::Reporter;

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

    // R67: Install/Shutdown bound by SCOPE, not by control flow. Today the
    // only early return in RunReport sits before Install, so the raw pair
    // balanced -- but task 5's kDeadline and task 8's kHostMismatch both
    // return from INSIDE that window, and Diagnostics.hpp:109-112 is explicit
    // that Install creates the crash thread and its events even with
    // installCrashHandler and startHangWatchdog both off. The death fixture's
    // own comment records what skipping Shutdown cost last time: a joinable
    // thread at static destruction, whose destructor calls std::terminate.
    // The trap is removed here, before two later tasks can step into it.
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

            if (!symbolizedInTime)
            {
                // R39: UNGUARDED on purpose. Task 7 adds the window and wraps
                // this branch in an `unattended` check -- an ATTENDED run must
                // let the worker keep going while the user looks at the
                // report. At this point in the plan no window exists, so
                // unconditional is correct, and task 7 owns the guard.
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
                TerminateProcess(GetCurrentProcess(), static_cast<UINT>(ExitCode::kDeadline));
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

        // TASK 7: the window, unless unattended.
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
