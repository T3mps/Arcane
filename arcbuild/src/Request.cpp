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

    const char* CommandName(Command c)
    {
        switch (c)
        {
            case Command::Generate: return "generate";
            case Command::Build:    return "build";
            case Command::Rebuild:  return "rebuild";
            case Command::Clean:    return "clean";
            case Command::Probe:    return "probe";
        }
        return "?";
    }

    Arcane::Cli MakeCli()
    {
        Arcane::Cli cli{ "arcbuild <generate|build|rebuild|clean|probe>",
                         "Arcane game-project build driver: premake, then msbuild, with the "
                         "single-slot incremental rule (spec 2026-09-13)" };
        cli.Option("project", "", "project directory or .arcproj (required)").Required();
        cli.Option("config", "Debug", "msbuild configuration").Choices({ "Debug", "Release", "Dist" });
        cli.Option("sdk", "", "Arcane SDK root (else ARCANE_SDK from the environment)");
        cli.Option("action", "vs2026", "premake action");
        cli.Flag("force-rebuild", "msbuild /t:Rebuild regardless of the slot probe");
        cli.Flag("quiet", "suppress the driver's own [arcbuild] info lines (child output still streams)");
        return cli;
    }

    Request RequestFromCli(Command command, const Arcane::Cli::Result& r)
    {
        Request req;
        req.command      = command;
        req.project      = r.Get("project");
        req.config       = r.Get("config");
        req.action       = r.Get("action");
        req.forceRebuild = r.Flag("force-rebuild");
        req.quiet        = r.Flag("quiet");
        // Supplied(), not a compare against the "" default: an explicit
        // --sdk "" is engaged-but-empty. ValidateRequest refuses it so it
        // cannot silently fall through to ARCANE_SDK.
        if (r.Supplied("sdk"))
            req.sdk = std::filesystem::path(r.Get("sdk"));
        return req;
    }

    bool IsValidAction(std::string_view action)
    {
        if (action.empty())
            return false;
        for (const char c : action)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    std::optional<std::string> ValidateRequest(const Request& req)
    {
        if (req.sdk && req.sdk->empty())
            return "--sdk requires a non-empty path (an empty value is not ARCANE_SDK)";
        if (!IsValidAction(req.action))
            return "invalid --action '" + req.action +
                   "' (expected a premake action identifier: letters, digits, '_' or '-')";
        if (req.command == Command::Probe && req.forceRebuild)
            return "--force-rebuild is not valid on probe (probe reports the slot row; pass it to build, or use rebuild)";
        return std::nullopt;
    }

    std::optional<std::filesystem::path> ResolveSdk(const std::optional<std::filesystem::path>& flag,
                                                    const char* envValue)
    {
        if (flag && !flag->empty())
            return *flag;
        if (envValue && *envValue)
            return std::filesystem::path(envValue);
        return std::nullopt;
    }
}
