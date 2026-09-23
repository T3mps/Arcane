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
#include <string_view>
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

    // Task 4's sibling: the portable stack the host already wrote, re-headed.
    // Task 5 keeps this as the fallback when the debug engine is unavailable.
    bool WriteFallbackSymbolized(const std::filesystem::path& stem, const Arcane::Diag::Envelope& e, std::string_view why)
    {
        std::ofstream out(SymbolizedSiblingPath(stem), std::ios::binary);
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

        // TASK 5: the dbgeng worker + deadline replace this call.
        //
        // R64: the return value is ACTED ON, never discarded. Writing this
        // sibling is the reporter's whole job in task 4; a silent kOk after a
        // failed write is a lie a parent (and the [diag] hand-off case) would
        // believe.
        if (!WriteFallbackSymbolized(stem, *envelope, "not built yet (plan 2 task 4)"))
        {
            ARC_ERROR("reporter: cannot write '{}'", ToUtf8(SymbolizedSiblingPath(stem).wstring()));
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
