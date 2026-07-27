#pragma once

// Host-boot helpers shared by Loom and the Arcane Editor (source-shared, like
// GpuContext/LoomConfig). They turn the engine's layered config + an open project
// into the two boot decisions a host makes: which input map to load, and which game
// module to host.

#include <Arcane/Base/Engine.hpp>        // BuildInfo (engine identity probe)
#include <Arcane/Base/Log.hpp>           // ARC_WARN/ARC_ERROR/ARC_INFO (not pulled in transitively by any of the below)
#include <Arcane/Base/Runtime.hpp>       // Runtime::ResetRegistry/Registry (BootScene)
#include <Arcane/Config/Config.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (engine identity probe)
#include <Arcane/Project/AssetId.hpp>            // AssetId::FromGuid (BootSceneFile)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>    // ReadSceneFile/ApplySceneDocument/CreateEmpty (BootScene)

#include <Json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
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
    // Single line on purpose: the caller reads one line from stdout.
    //
    // `exePathUtf8` is UTF-8 BYTES, not a std::filesystem::path: callers pass
    // Arcane::ExecutablePathUtf8(). Taking a path here and calling
    // generic_string() would re-encode through the implementation's native narrow
    // encoding -- the ANSI-codepage round-trip that made this throw on non-ASCII
    // install paths in the first place. Backslashes are normalised here since we
    // no longer get generic_string()'s normalisation for free.
    inline std::string EngineInfoJson(std::string exePathUtf8)
    {
        std::replace(exePathUtf8.begin(), exePathUtf8.end(), '\\', '/');
        nlohmann::json j;
        // PluginABIVersion(), NOT the kGamePluginABIVersion header constant: the
        // constant is whatever THIS module was compiled against, while the gate
        // that rejects a plugin lives in Arcane.dll. A partially-updated install
        // would otherwise publish a number the runtime refuses.
        j["engineAbi"] = Arcane::PluginABIVersion();
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = std::move(exePathUtf8);
        // error_handler_t::replace, not the default throw: this probe is the ONE
        // thing the Hub relies on to learn the ABI, so a malformed byte must
        // degrade to U+FFFD in the output, never an exception escaping main()
        // into terminate(). ExecutablePathUtf8 already yields well-formed UTF-8;
        // this is the belt for every other caller.
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
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

    // The project's boot scene as a physical file, or empty when it has none /
    // the id names nothing this project contains.
    //
    // Split out from BootScene so the RESOLUTION is unit-testable without a
    // Runtime: it is the part with the interesting failure modes.
    inline std::filesystem::path BootSceneFile(const Arcane::Project& project)
    {
        const std::string& text = project.Manifest().bootScene;
        if (text.empty()) return {};

        const std::optional<Arcane::Guid> id = Arcane::Guid::FromString(text);
        if (!id || !id->IsValid())
        {
            ARC_WARN("bootScene '{}' is not a valid asset id", text);
            return {};
        }

        const std::optional<std::filesystem::path> file =
            project.ResolveAsset(Arcane::AssetId::FromGuid(*id));
        if (!file)
        {
            ARC_WARN("bootScene {} does not resolve to a file in this project", text);
            return {};
        }
        return *file;
    }

    // What BootScene loaded, handed back so the caller (the editor's
    // SceneSession::Adopt) can record the session's file + id WITHOUT a second
    // ReadSceneFile of the same path just to recover the Guid, which is what
    // the original plan for this function would have made every caller do.
    struct BootSceneResult
    {
        std::filesystem::path file;   // the .arcscene BootScene just applied
        Arcane::Guid          id;     // its asset id, straight from the parsed document
    };

    // Load the project's boot scene into `runtime`, replacing whatever the
    // registry holds. nullopt when there is no boot scene or it could not be
    // loaded -- the reason is logged HERE, and callers simply continue with
    // whatever the registry already held (a project with no boot scene, or one
    // whose boot scene fails to
    // resolve/parse, is left exactly as it was -- nothing is reset until a
    // valid document is in hand) rather than refusing to open the project,
    // because the editor is how a broken boot scene gets fixed.
    //
    // Call AFTER the plugin loads: a scene naming a component the game module
    // registers would otherwise silently drop it.
    inline std::optional<BootSceneResult> BootScene(Arcane::Runtime& runtime, const Arcane::Project& project)
    {
        const std::filesystem::path file = BootSceneFile(project);
        if (file.empty()) return std::nullopt;

        // Read before reset, same ordering rule as the editor's Open Scene: a
        // boot scene that fails to parse must not leave the host holding a
        // half-built registry. There is less to protect at boot than Open
        // Scene protects (no prior authored scene, only whatever the plugin's
        // Init happened to spawn), but the order is the same regardless.
        std::string err;
        const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
        if (!doc)
        {
            ARC_ERROR("bootScene: {}", err);
            return std::nullopt;
        }

        runtime.ResetRegistry();
        if (!Arcane::Scene::ApplySceneDocument(*doc, runtime.Registry()))
        {
            // Validated but unloadable -- the failure mode ReadSceneFile's
            // structural gate cannot see (a component whose reflected field
            // type is unsupported; SceneAsset.hpp's E02-3 note). The registry
            // is already reset by contract, so leave a well-formed empty scene
            // rather than an empty-but-rootless one.
            ARC_ERROR("bootScene: {} parsed but could not be loaded", file.generic_string());
            Arcane::Scene::CreateEmpty(runtime.Registry());
            return std::nullopt;
        }

        ARC_INFO("Loaded boot scene {}", file.generic_string());
        return BootSceneResult{file, doc->id};
    }
}
