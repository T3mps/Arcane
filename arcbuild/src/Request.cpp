#include "Request.hpp"

#include <cctype>

namespace arcbuild
{
    std::optional<Command> ParseCommand(std::string_view word)
    {
        if (word == "generate") return Command::Generate;
        if (word == "build")    return Command::Build;
        if (word == "rebuild")  return Command::Rebuild;
        if (word == "clean")    return Command::Clean;
        if (word == "probe")    return Command::Probe;

        return std::nullopt;
    }

    const char* CommandName(Command command)
    {
        switch (command)
        {
        case Command::Generate: return "generate";
        case Command::Build:    return "build";
        case Command::Rebuild:  return "rebuild";
        case Command::Clean:    return "clean";
        case Command::Probe:    return "probe";
        }

        return "?";
    }

    Arcane::Cli MakeCli(
        std::string_view defaultAction)
    {
        Arcane::Cli cli
        {
            "arcbuild <generate|build|rebuild|clean|probe>",
            "Arcane game-project build driver: Premake generation plus "
            "backend-native builds with the single-slot incremental rule "
            "(spec 2026-09-13)"
        };

        cli.Option(
            "project",
            "",
            "project directory or .arcproj (required)")
            .Required();

        cli.Option(
            "config",
            "Debug",
            "build configuration")
            .Choices({ "Debug", "Release", "Dist" });

        cli.Option(
            "sdk",
            "",
            "Arcane SDK root (else ARCANE_SDK from the environment)");

        cli.Option(
            "action",
            std::string(defaultAction),
            "Premake action");

        cli.Flag(
            "force-rebuild",
            "force a full rebuild regardless of the slot probe");

        cli.Flag(
            "quiet",
            "suppress the driver's own [arcbuild] info lines "
            "(child output still streams)");

        return cli;
    }

    Request RequestFromCli(
        Command command,
        const Arcane::Cli::Result& result)
    {
        Request request;

        request.command      = command;
        request.project      = result.Get("project");
        request.config       = result.Get("config");
        request.action       = result.Get("action");
        request.forceRebuild = result.Flag("force-rebuild");
        request.quiet        = result.Flag("quiet");

        if (result.Supplied("sdk"))
            request.sdk = std::filesystem::path(result.Get("sdk"));

        return request;
    }

    bool IsValidAction(std::string_view action)
    {
        if (action.empty())
            return false;

        for (const char c : action)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                c == '_' ||
                c == '-'))
            {
                return false;
            }
        }

        return true;
    }

    std::optional<std::string> ValidateRequest(
        const Request& request)
    {
        if (request.sdk && request.sdk->empty())
        {
            return "--sdk requires a non-empty path "
                "(an empty value is not ARCANE_SDK)";
        }

        if (!IsValidAction(request.action))
        {
            return
                "invalid --action '" +
                request.action +
                "' (expected a premake action identifier: "
                "letters, digits, '_' or '-')";
        }

        if (request.forceRebuild &&
            request.command != Command::Build)
        {
            return "--force-rebuild is only valid with build";
        }

        return std::nullopt;
    }

    std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag,
        const std::optional<std::filesystem::path>& environment)
    {
        if (flag && !flag->empty())
            return *flag;

        if (environment && !environment->empty())
            return *environment;

        return std::nullopt;
    }

    std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag,
        const char* environment)
    {
        if (environment && *environment)
        {
            return ResolveSdk(
                flag,
                std::optional<std::filesystem::path>
            {
                std::filesystem::path(environment)
            });
        }

        return ResolveSdk(
            flag,
            std::optional<std::filesystem::path>{});
    }
}
