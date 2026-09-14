#pragma once

// CLI shape (spec s3): the positional command, the flag set, --sdk / ARCANE_SDK
// precedence, and the flag/command refusals. Shared by both target kinds --
// `--project` (game, v1) and `--engine` (Arcane.slnx, spec §6, not built).
// Adding `--engine` must not change the game-project CLI; it grows Request,
// it does not fork MakeCli.

#include <Arcane/Cli/Cli.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace arcbuild
{
    enum class Command : std::uint8_t { Generate, Build, Rebuild, Clean, Probe };

    // The exact lower-case word, or nullopt. The command is positional (argv[1])
    // because Arcane::Cli has no subcommands; main.cpp peels it and hands the
    // rest to MakeCli().Parse.
    [[nodiscard]] std::optional<Command> ParseCommand(std::string_view word);
    [[nodiscard]] const char* CommandName(Command c);

    struct Request
    {
        Command                              command = Command::Build;
        std::filesystem::path                project;            // --project, as given (dir or .arcproj)
        std::string                          config = "Debug";   // --config Debug|Release|Dist
        std::optional<std::filesystem::path> sdk;                // --sdk, when supplied (empty path = explicit --sdk "")
        std::string                          action = "vs2026";  // --action (premake generator token; not the Linux seam yet)
        bool                                 forceRebuild = false;
        bool                                 quiet = false;      // suppress the driver's own info lines (ruling R5)
    };

    // The option/flag set of s3, registered on an Arcane::Cli. --project is
    // Required(); --config is Choices'd to the three configurations.
    [[nodiscard]] Arcane::Cli MakeCli();
    [[nodiscard]] Request RequestFromCli(Command command, const Arcane::Cli::Result& r);

    // Premake's generator token, not a shell fragment: non-empty
    // [A-Za-z0-9_-]+. Kept a string (not an enum) until a non-msbuild spawn
    // path exists -- vs2026 / gmake2 / ninja are then a closed table, not a
    // pass-through. Dots, spaces, and cmd metacharacters are refusals.
    [[nodiscard]] bool IsValidAction(std::string_view action);

    // Flag/command combinations the shell must refuse (exit 2) before it
    // touches the filesystem: empty --sdk, a non-identifier --action,
    // --force-rebuild on probe (R4: probe reports the slot, it is not a
    // dry-run of build). nullopt = ok. `--engine` refusals land here too.
    [[nodiscard]] std::optional<std::string> ValidateRequest(const Request& req);

    // --sdk > ARCANE_SDK > nullopt (the driver refuses). A set-but-empty
    // variable counts as unset. An engaged-but-empty flag never reaches
    // here -- ValidateRequest refuses --sdk "" first so it cannot silently
    // fall through to ARCANE_SDK.
    [[nodiscard]] std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag, const char* envValue);
}
