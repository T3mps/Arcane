// ArcaneRuntime -- the thin entry point. Parses argv into a HostConfig, constructs
// the RuntimeApp application object, and returns its Run() exit code. All engine
// boot, the frame loop, and the load-bearing teardown order now live in the
// RuntimeApp class (RuntimeApp.hpp/.cpp); main is just the wire-up.

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include "RuntimeApp.hpp"
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
    // but only worked on one would be a trap. ArcaneRuntime does NOT get the editor's
    // no-project gate -- it hosts Sandbox.dll by default with no flags, by design.
    if (parsed.config->printEngineInfo)
    {
        // ExecutablePathUtf8, NOT argv[0]: argv[0] is whatever the launcher typed
        // (a bare relative name under the documented cd-then-run workflow) and is
        // ANSI-codepage bytes under MSVC, which a strict-UTF-8 dump() rejects.
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(Arcane::ExecutablePathUtf8()).c_str());
        return 0;
    }

    // Same arming as the editor, same reasoning, same position relative to the
    // probe -- see ArcaneEditor/src/main.cpp. The two hosts must not diverge on
    // whether a crash or a hang leaves evidence behind.
    {
        Arcane::Diagnostics::Config diag;
        diag.appName = "ArcaneRuntime";
        Arcane::Diagnostics::Install(diag);
    }

    // Before ANY engine boot: something on screen within ~100ms. The probe
    // return above stays free of any window on purpose. Never fails boot --
    // BootSplashWindow's whole contract is "every error path degrades to no
    // splash, silently".
    Arcane::BootSplashWindow splash("data/images/arcane_logo.png");

    // Scoped so ~RuntimeApp runs while the watchdog is still armed, then joined
    // before main returns -- see ArcaneEditor/src/main.cpp for the full reason
    // (teardown is a suspect; a joinable std::thread at static destruction
    // calls std::terminate).
    int rc = 0;
    {
        RuntimeApp app(*parsed.config, &splash);
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("runtime teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
