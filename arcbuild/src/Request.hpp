#pragma once

#include "Action.hpp"

#include <Arcane/Cli/Cli.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace arcbuild
{
    enum class Command : std::uint8_t
    {
        Generate,
        Build,
        Rebuild,
        Clean,
        Probe
    };

    [[nodiscard]] std::optional<Command> ParseCommand(std::string_view word);
    [[nodiscard]] const char* CommandName(Command command);

    struct Request
    {
        Command                              command = Command::Build;
        std::filesystem::path                project;
        std::string                          config = "Debug";
        std::optional<std::filesystem::path> sdk;
        std::string                          action = std::string(DefaultActionFor(CurrentHostPlatform()));
        bool                                 forceRebuild = false;
        bool                                 quiet = false;
    };

    [[nodiscard]] Arcane::Cli MakeCli(
        std::string_view defaultAction = DefaultActionFor(CurrentHostPlatform()));

    [[nodiscard]]
    Request RequestFromCli(
        Command command,
        const Arcane::Cli::Result& result);

    [[nodiscard]]
    bool IsValidAction(std::string_view action);

    [[nodiscard]]
    std::optional<std::string> ValidateRequest(
        const Request& request);

    [[nodiscard]]
    std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag,
        const std::optional<std::filesystem::path>& environment);

    // Compatibility overload for existing pure-core tests/callers.
    [[nodiscard]]
    std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag,
        const char* environment);
}
