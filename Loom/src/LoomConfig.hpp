#pragma once
// LoomConfig: the typed result of parsing Loom's command line. Owns the Loom
// option vocabulary; delegates the parsing to the generic Arcane::Cli (Core).
// PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <optional>
#include <string>
#include <Arcane/Render/Device.hpp>   // Arcane::GraphicsBackend
struct LoomConfig
{
    Arcane::GraphicsBackend backend   = Arcane::GraphicsBackend::D3D12;
    std::uint64_t           maxFrames = 0;             // 0 = run until quit
    bool                    vsync     = true;
    bool                    perf      = false;
    // Empty = "no explicit --plugin"; each host supplies its own fallback (Loom ->
    // Sandbox.dll, the editor -> no game loaded). A project's gameModule overrides both.
    std::string             pluginPath = "";
    std::string             projectPath = "";   // .arcproj or project folder; "" = data/-next-to-exe

    // Forward-declared here so it names LoomConfig::ParseOutcome and can be the
    // return type of Parse; DEFINED below (after the class closes) because its
    // std::optional<LoomConfig> member needs LoomConfig to be a complete type,
    // which it is not yet inside this member specification (MSVC enforces this).
    struct ParseOutcome;

    // Builds the Cli spec, parses argv, maps Result -> LoomConfig.
    // --help / bad args -> { nullopt, exitCode }; success -> { config, 0 }.
    static ParseOutcome Parse(int argc, char** argv);
};

struct LoomConfig::ParseOutcome { std::optional<LoomConfig> config; int exitCode = 0; };
