#include "Driver.hpp"

namespace arcbuild
{
    namespace
    {
        void Quote(std::string& out, const std::filesystem::path& p)
        {
            out += '"';
            out += p.string();
            out += '"';
        }
    }

    // ---- the CLI ----------------------------------------------------------------

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
        // --sdk "" would otherwise be indistinguishable from unsupplied.
        if (r.Supplied("sdk") && !r.Get("sdk").empty())
            req.sdk = std::filesystem::path(r.Get("sdk"));
        return req;
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

    // ---- the incremental rule ------------------------------------------------------

    bool ConfigWantsDebugCrt(std::string_view config)
    {
        return config == "Debug";
    }

    const char* SlotStateName(SlotState s)
    {
        switch (s)
        {
            case SlotState::Absent:     return "absent";
            case SlotState::Match:      return "match";
            case SlotState::Mismatch:   return "mismatch";
            case SlotState::Unreadable: return "unreadable";
        }
        return "?";
    }

    SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config)
    {
        if (!exists)
            return SlotState::Absent;
        if (flavor == Arcane::CrtFlavor::Unknown)
            return SlotState::Unreadable;
        const bool slotIsDebug = (flavor == Arcane::CrtFlavor::Debug);
        return slotIsDebug == ConfigWantsDebugCrt(config) ? SlotState::Match : SlotState::Mismatch;
    }

    Verdict Decide(Command command, bool forceRebuild, SlotState slot)
    {
        if (command == Command::Rebuild)
            return { true, "rebuild command: msbuild /t:Rebuild unconditionally" };
        if (forceRebuild)
            return { true, "--force-rebuild: msbuild /t:Rebuild, slot probe bypassed" };
        switch (slot)
        {
            case SlotState::Absent:
                return { false, "slot absent: plain build (nothing to be wrong about)" };
            case SlotState::Match:
                return { false, "slot CRT flavor matches --config: plain build (msbuild's incremental view is trustworthy)" };
            case SlotState::Mismatch:
                // THE SINGLE-SLOT HAZARD: Binaries\ holds one DLL for every
                // configuration while the object trees are per-config, so an
                // incremental build would compare this config's objects
                // against this config's link stamp, find both current, relink
                // nothing, and leave the OTHER config's DLL in place for the
                // host to refuse.
                return { true, "slot CRT flavor mismatches --config: msbuild /t:Rebuild (single-slot Binaries/ hazard)" };
            case SlotState::Unreadable:
                return { true, "slot CRT flavor unreadable: msbuild /t:Rebuild (unknown => the safe choice)" };
        }
        return { true, "slot state unknown: msbuild /t:Rebuild" };
    }

    // ---- exit codes ------------------------------------------------------------------

    int ProbeExitCode(SlotState s)
    {
        return (s == SlotState::Absent || s == SlotState::Match) ? kExitOk : kExitProbeRebuild;
    }

    int ExitFromChild(std::optional<int> childExit)
    {
        return childExit ? *childExit : kExitRefused;
    }

    // ---- paths -----------------------------------------------------------------------

    std::filesystem::path SlotPath(const Layout& l)
    {
        if (l.gameModule.empty())
            return {};
        return l.root / "Binaries" / l.gameModule;
    }

    std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered)
    {
        if (!discovered.empty())
            return discovered;
        return l.root / (l.name + ".slnx");
    }

    // ---- composition -------------------------------------------------------------------

    std::string ComposeGenerate(const Layout& l, const Tools& t, std::string_view action)
    {
        std::string cmd = "( cd /d ";
        Quote(cmd, l.root);
        cmd += " && ";
        Quote(cmd, t.premake);
        cmd += ' ';
        cmd += action;
        cmd += " ) 2>&1";
        return cmd;
    }

    std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution,
                               std::string_view config, MsBuildTarget target)
    {
        std::string cmd = "( ";
        Quote(cmd, t.msbuild);
        cmd += ' ';
        Quote(cmd, solution);
        cmd += " /p:Configuration=";
        cmd += config;
        switch (target)
        {
            case MsBuildTarget::Build:   break;
            case MsBuildTarget::Rebuild: cmd += " /t:Rebuild"; break;
            case MsBuildTarget::Clean:   cmd += " /t:Clean";   break;
        }
        cmd += " /m /nologo ) 2>&1";
        return cmd;
    }

    std::vector<std::filesystem::path> CleanTargets(const Layout& l, std::string_view config)
    {
        return { l.root / "Binaries", l.root / "Intermediate" / std::string(config) };
    }
}
