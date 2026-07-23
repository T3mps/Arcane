#pragma once

// Host-boot helpers shared by Loom and the Arcane Editor (source-shared, like
// GpuContext/LoomConfig). They turn the engine's layered config + an open project
// into the two boot decisions a host makes: which input map to load, and which game
// module to host.

#include <Arcane/Config/Config.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Project/Project.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace Arcane::HostBoot
{
    // Load the input action maps from the layered config's "input" category (engine
    // default EngineConfig/input.json, deep-merged with the project's Config/input.json).
    // Sets the "demo" base context on success. Returns false if the category is
    // absent/malformed (the host logs and continues -- input stays inert).
    inline bool LoadInputConfig(Arcane::InputActions& input, const Arcane::Config& config)
    {
        if (!input.LoadJson(config.Category("input")))
            return false;
        input.SetBaseContext("demo");
        return true;
    }

    // The game module to host: the project's gameModule when a project is open and it
    // names one, else the fallback --plugin path.
    //
    // A project builds its own game DLL into <project>/Binaries/ (engine-as-SDK model),
    // so when that built copy exists we return its absolute path -- the host loads the
    // project's OWN module rather than a same-named DLL sitting beside the exe. If the
    // Binaries/ copy isn't there (a demo project that BORROWS a host-adjacent DLL, e.g.
    // SampleProject -> Sandbox.dll), we fall through to the bare name resolved beside the
    // host exe -- keeping the borrowing path working.
    inline std::string GameModule(const Arcane::Project* project, const std::string& fallback)
    {
        if (project && !project->Manifest().gameModule.empty())
        {
            const std::string& mod = project->Manifest().gameModule;
            std::error_code ec;
            const std::filesystem::path built = project->Root() / "Binaries" / mod;
            if (std::filesystem::exists(built, ec))
                return built.string();
            return mod;
        }
        return fallback;
    }
}
