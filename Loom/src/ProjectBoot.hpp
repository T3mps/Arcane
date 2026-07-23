#pragma once

// Host-boot helpers shared by Loom and the Arcane Editor (source-shared, like
// GpuContext/LoomConfig). They translate an open Project into the two boot
// decisions a host makes: which input config to load, and which game module to
// host. No project => the legacy data/-next-to-exe + --plugin behavior.

#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <filesystem>
#include <string>

namespace Arcane::HostBoot
{
    // Load the input-actions config: through the open project's game:// mount when a
    // project is open, else data/input_actions.json (exe-relative, the legacy path).
    // Sets the "demo" base context on success. Returns false if the file is
    // missing/unreadable/malformed (the host logs and continues -- input stays inert).
    inline bool LoadInputConfig(Arcane::InputActions& input, const Arcane::Project* project)
    {
        std::filesystem::path file = "data/input_actions.json";
        if (project)
        {
            auto resolved = project->ResolveAsset(
                Arcane::AssetId::FromMountPath("game://input_actions.json"));
            if (resolved)
                file = *resolved;
        }
        if (!input.LoadFile(file))
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
