#pragma once
// HostConfig: the typed result of parsing a runtime host's command line. Owns
// the host option vocabulary; delegates the parsing to the generic Arcane::Cli
// (Core). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <optional>
#include <string>
#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Device.hpp>   // Arcane::GraphicsBackend
namespace Arcane
{
    struct ARCANE_API HostConfig
    {
        GraphicsBackend backend   = GraphicsBackend::D3D12;
        std::uint64_t   maxFrames = 0;             // 0 = run until quit
        bool            vsync     = true;
        bool            perf      = false;
        // Empty = "no explicit --plugin"; each host supplies its own fallback (Loom ->
        // Sandbox.dll, the editor -> no game loaded). A project's gameModule overrides both.
        std::string     pluginPath = "";
        std::string     projectPath = "";   // .arcproj or project folder; "" = data/-next-to-exe

        // Print one line of engine-identity JSON to stdout and exit, without creating
        // a window or device. The Arcane Hub probes this to learn the plugin ABI it
        // must stamp into a new .arcproj -- see HostBoot::EngineInfoJson.
        bool            printEngineInfo = false;

        // Forward-declared here so it names HostConfig::ParseOutcome and can be the
        // return type of Parse; DEFINED below (after the class closes) because its
        // std::optional<HostConfig> member needs HostConfig to be a complete type,
        // which it is not yet inside this member specification (MSVC enforces this).
        struct ParseOutcome;

        // Builds the Cli spec, parses argv, maps Result -> HostConfig.
        // --help / bad args -> { nullopt, exitCode }; success -> { config, 0 }.
        static ParseOutcome Parse(int argc, char** argv);
    };

    struct HostConfig::ParseOutcome { std::optional<HostConfig> config; int exitCode = 0; };
}
