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

        // Diagnostics for the reporter ITSELF (spec §6 failure modes): same
        // folder, never spawns a reporter (D13).
        Arcane::Diagnostics::Config diag;
        diag.appName           = "ArcaneCrashReporter";
        diag.productName       = "Arcane Crash Reporter";
        diag.dumpDir           = stem.parent_path().string();
        diag.unattended        = a.unattended;
        diag.spawnReporter     = false;
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
