// Loom -- the thin entry point. Parses argv into a HostConfig, constructs the
// Loom application object, and returns its Run() exit code. All engine boot,
// the frame loop, and the load-bearing teardown order now live in the Loom
// class (Loom.hpp/.cpp); main is just the wire-up.

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include "Loom.hpp"
#include <Arcane/Host/ProjectBoot.hpp>   // HostBoot::EngineInfoJson (the --print-engine-info probe)

#include <cstdio>

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    const Arcane::HostConfig::ParseOutcome parsed = Arcane::HostConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2

    // Same probe as the editor: identity to stdout, no window, no device. The
    // flag lives in the SHARED HostConfig, so a flag that parsed on both hosts
    // but only worked on one would be a trap. Loom does NOT get the editor's
    // no-project gate -- it hosts Sandbox.dll by default with no flags, by design.
    if (parsed.config->printEngineInfo)
    {
        // ExecutablePathUtf8, NOT argv[0]: argv[0] is whatever the launcher typed
        // (a bare relative name under the documented cd-then-run workflow) and is
        // ANSI-codepage bytes under MSVC, which a strict-UTF-8 dump() rejects.
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(Arcane::ExecutablePathUtf8()).c_str());
        return 0;
    }

    Loom loom(*parsed.config);
    return loom.Run();
}
