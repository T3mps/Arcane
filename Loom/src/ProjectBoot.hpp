#pragma once

// Host-boot helpers shared by Loom and the Arcane Editor (source-shared, like
// GpuContext/LoomConfig). They turn the engine's layered config + an open project
// into the two boot decisions a host makes: which input map to load, and which game
// module to host.

#include <Arcane/Base/Engine.hpp>        // BuildInfo (engine identity probe)
#include <Arcane/Config/Config.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (engine identity probe)
#include <Arcane/Project/Project.hpp>

#include <Json.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Arcane::HostBoot
{
    // One-line JSON describing this engine build, for `--print-engine-info`.
    //
    // This exists so the Arcane Hub never HARDCODES a plugin ABI. A .arcproj
    // requires `engine.abi` (ProjectManifest.hpp), so a hub that guessed it
    // would mint stale-ABI projects the moment the engine bumps, and those
    // crash on open. The Hub probes, then stamps whatever the engine reports.
    //
    // Single line on purpose: the caller reads one line from stdout. Paths use
    // generic_string() so the Hub never has to unescape backslashes.
    inline std::string EngineInfoJson(const std::filesystem::path& exePath)
    {
        nlohmann::json j;
        j["engineAbi"] = Arcane::kGamePluginABIVersion;
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = exePath.generic_string();
        return j.dump();   // compact: no indent argument
    }

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

    // The secondary plugin modules a host should load: each enabled manifest plugin that
    // has built a DLL at <root>/Plugins/<name>/Binaries/<name>.dll. A content-only plugin
    // (no Source/ -> no DLL) contributes only its plugin:// content mount (added at
    // Project::Open) and is skipped here. Feed each to PluginHost::AddPlugin before Load().
    inline std::vector<std::filesystem::path> PluginModules(const Arcane::Project* project)
    {
        std::vector<std::filesystem::path> out;
        if (!project)
            return out;
        std::error_code ec;
        for (const auto& ref : project->Manifest().plugins)
        {
            if (!ref.enabled)
                continue;
            std::filesystem::path dll = project->Root() / "Plugins" / ref.name / "Binaries" / (ref.name + ".dll");
            if (std::filesystem::exists(dll, ec))
                out.push_back(std::move(dll));
        }
        return out;
    }
}
