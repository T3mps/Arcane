#pragma once

// Host-boot helpers shared by Loom and the Arcane Editor (source-shared, like
// GpuContext/LoomConfig). They turn the engine's layered config + an open project
// into the two boot decisions a host makes: which input map to load, and which game
// module to host.

#include <Arcane/Config/Config.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Project/Project.hpp>

#include <string>

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
    inline std::string GameModule(const Arcane::Project* project, const std::string& fallback)
    {
        if (project && !project->Manifest().gameModule.empty())
            return project->Manifest().gameModule;
        return fallback;
    }
}
