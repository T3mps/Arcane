#pragma once

// ServerConfig: ArcaneServer's own typed CLI result over Arcane::Cli (Core-DLL
// split, spec docs/specs/2026-09-15-core-dll-split-design.md s6: "mirrors the
// other hosts"). HostConfig (ArcaneClient/src/Arcane/Host/HostConfig.hpp) is
// Client -- it names a GraphicsBackend and a dozen presentation-only flags
// (--headless, --screenshot, --settle, --compare, ...) this host has no use
// for -- so ArcaneServer owns its own, narrower vocabulary over the same
// Arcane::Cli rather than dragging Client in for one parse.

#include <cstdint>
#include <optional>
#include <string>

namespace Arcane::Server
{
    struct ServerConfig
    {
        std::string   projectPath;                  // --project (REQUIRED: a server with nothing to host refuses, like ArcaneRuntime)
        std::string   pluginPath;                    // --plugin override
        std::uint64_t frames         = 0;             // --frames N; 0 = run until terminated (P13)
        double        fixedDtSeconds = 1.0 / 60.0;    // --fixed-dt
        std::string   reportPath;                     // --report <json>
        bool          printEngineInfo = false;        // --print-engine-info (same probe as the other hosts, Core-side)

        // Forward-declared here so it names ServerConfig::ParseOutcome and can be
        // the return type of Parse; DEFINED below (after the class closes) because
        // its std::optional<ServerConfig> member needs ServerConfig to be a
        // complete type, which it is not yet inside this member specification
        // (MSVC enforces this) -- same shape as HostConfig::ParseOutcome
        // (ArcaneClient/src/Arcane/Host/HostConfig.hpp).
        struct ParseOutcome;

        // {help} => {nullopt, 0}; a parse/validation refusal => {nullopt, 2};
        // otherwise {config, 0}. See ServerConfig.cpp for the exact refusals.
        [[nodiscard]] static ParseOutcome Parse(int argc, char** argv);
    };

    struct ServerConfig::ParseOutcome { std::optional<ServerConfig> config; int exitCode = 0; };
}
