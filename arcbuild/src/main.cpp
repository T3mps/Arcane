// arcbuild -- the game-project build driver (spec docs/specs/
// 2026-09-13-arcbuild-driver-design.md). Unreal's Build.bat analogue for
// Arcane game projects: ONE entry point the editor (ModuleBuild), the Gacha
// scripts and CI all call. It DRIVES premake and msbuild; it never owns
// compilation. The pure core (every decision) is Driver.hpp; this file is
// the shell: manifest, SDK, tool resolution, the slot probe, and the
// spawn-and-stream of each child.
//
//   arcbuild <generate|build|rebuild|clean|probe> --project <dir|.arcproj>
//            [--config Debug|Release|Dist] [--sdk <root>] [--action vs2026]
//            [--force-rebuild] [--quiet]
//
// Output: every child line to stdout prefixed [premake] / [msbuild]; the
// driver's own lines [arcbuild] (refusals: "[arcbuild] error: ..."). Flushed
// per line -- the editor reads this through a pipe and streams it into its
// Console. Exit: the first failing child's status, 2 for a driver refusal
// (no SDK / no project / bad flags), 3 from `probe` when the slot would
// force /t:Rebuild, 0 otherwise.

#include "Driver.hpp"

#include <Arcane/Build/Toolchain.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    using namespace arcbuild;

    bool g_quiet = false;

    void Say(const std::string& line)
    {
        if (g_quiet)
            return;
        std::printf("[arcbuild] %s\n", line.c_str());
        std::fflush(stdout);
    }

    // Never quieted: a refusal is the one line the caller must see.
    int Refuse(const std::string& why)
    {
        std::printf("[arcbuild] error: %s\n", why.c_str());
        std::fflush(stdout);
        return kExitRefused;
    }

#ifdef _WIN32
    std::wstring Widen(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                            static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                              wide.data(), n);
        return wide;
    }
#endif

    // Point ARCANE_SDK at `sdkRoot` in THIS process's environment block, which
    // the spawned cmd children inherit (the project's premake5.lua consumes it
    // via build/arcane.lua). Deliberately overwrites any setx'd value: --sdk
    // means "build against THIS engine", not whichever one the machine-wide
    // variable last pointed at.
    void SetSdkEnv(const std::filesystem::path& sdkRoot)
    {
#ifdef _WIN32
        ::SetEnvironmentVariableW(L"ARCANE_SDK", sdkRoot.wstring().c_str());
#else
        ::setenv("ARCANE_SDK", sdkRoot.string().c_str(), 1);
#endif
    }

    // Run one composed ( ... ) 2>&1 line through cmd (_wpopen), re-emitting
    // every line with `prefix` as it arrives. nullopt when the pipe itself
    // could not open. The same shape as the editor's ModuleBuild::Runner
    // worker, on the calling thread -- the driver has nothing else to do.
    std::optional<int> Stream(const std::string& commandLine, const char* prefix)
    {
#ifdef _WIN32
        FILE* pipe = ::_wpopen(Widen(commandLine).c_str(), L"r");
        if (!pipe)
            return std::nullopt;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe))
        {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            std::printf("%s %s\n", prefix, line.c_str());
            std::fflush(stdout);
        }
        return ::_pclose(pipe);
#else
        (void)commandLine; (void)prefix;
        std::printf("[arcbuild] error: the vs2026 action is Windows-only today (cmd + msbuild)\n");
        return std::nullopt;
#endif
    }

    const char* FlavorName(Arcane::CrtFlavor f)
    {
        switch (f)
        {
            case Arcane::CrtFlavor::Debug:   return "debug";
            case Arcane::CrtFlavor::Release: return "release";
            case Arcane::CrtFlavor::Unknown: return "unknown";
        }
        return "?";
    }

    struct Probe
    {
        std::filesystem::path slot;
        bool                  exists = false;
        Arcane::CrtFlavor     flavor = Arcane::CrtFlavor::Unknown;
        std::string           matched;   // the import that decided the verdict
        SlotState             state  = SlotState::Absent;
    };

    // The s4.3 probe: Module::ScanFileCrtFlavor over the slot -- the SAME
    // verdict PluginHost uses to refuse a cross-CRT module, never a second
    // scanner. A content-only project (no gameModule) is Absent (ruling R7).
    Probe ProbeSlot(const Layout& layout, std::string_view config)
    {
        Probe p;
        p.slot = SlotPath(layout);
        std::error_code ec;
        p.exists = !p.slot.empty() && std::filesystem::is_regular_file(p.slot, ec);
        if (p.exists)
            p.flavor = Arcane::Module::ScanFileCrtFlavor(p.slot, &p.matched);
        p.state = ClassifySlot(p.exists, p.flavor, config);
        return p;
    }

    std::string ProbeRow(const Probe& p, std::string_view config, const Verdict& v)
    {
        std::string row = "probe: slot=";
        row += p.slot.empty() ? std::string("(no gameModule)") : p.slot.generic_string();
        row += " state=";
        row += SlotStateName(p.state);
        row += " flavor=";
        row += FlavorName(p.flavor);
        if (!p.matched.empty())
        {
            row += " (";
            row += p.matched;
            row += ')';
        }
        row += " config=";
        row += config;
        row += " -> ";
        row += v.rebuild ? "/t:Rebuild" : "plain build";
        return row;
    }

    void PrintUsage()
    {
        std::printf("usage: arcbuild <generate|build|rebuild|clean|probe> --project <dir|.arcproj>\n"
                    "                [--config Debug|Release|Dist] [--sdk <root>] [--action vs2026]\n"
                    "                [--force-rebuild] [--quiet]\n"
                    "  generate   premake <action> in the project root (writes <Name>.slnx + .vcxproj)\n"
                    "  build      generate, then msbuild; /t:Rebuild only when the Binaries/ slot's CRT\n"
                    "             flavor mismatches --config (or is unreadable)\n"
                    "  rebuild    generate, then msbuild /t:Rebuild unconditionally\n"
                    "  clean      msbuild /t:Clean, then remove Binaries/ and Intermediate/<config>/\n"
                    "  probe      print the slot verdict and exit 0 (plain build) or 3 (/t:Rebuild)\n");
        std::fflush(stdout);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")
    {
        PrintUsage();
        return argc < 2 ? kExitRefused : kExitOk;
    }

    const std::optional<Command> command = ParseCommand(argv[1]);
    if (!command)
    {
        PrintUsage();
        return Refuse(std::string("unknown command '") + argv[1] + "'");
    }

    // The command word is positional (Arcane::Cli has no subcommands): hand
    // Cli argv+1 so the word sits in the [0] slot Parse skips.
    const Arcane::Cli cli = MakeCli();
    const Arcane::Cli::Result parsed = cli.Parse(argc - 1, argv + 1);
    if (!parsed.ok)
        return parsed.exitCode;   // Cli printed the reason + usage (2), or --help (0)

    const Request req = RequestFromCli(*command, parsed);
    g_quiet = req.quiet;

    const std::optional<std::filesystem::path> sdk = ResolveSdk(req.sdk, std::getenv("ARCANE_SDK"));
    if (!sdk)
        return Refuse("no SDK: pass --sdk <root> or set ARCANE_SDK to an Arcane engine checkout");

    // The project: a directory (one .arcproj inside) or the .arcproj itself --
    // Project::ResolveManifestFile is the ONE rule the hosts use for the same
    // question; the manifest's name/gameModule drive the solution convention
    // and the slot. Root is absolutised here (ruling R3) so a relative
    // --project from a CI workspace composes correctly after premake's cd.
    const std::optional<std::filesystem::path> manifestFile = Arcane::Project::ResolveManifestFile(req.project);
    if (!manifestFile)
        return Refuse("no project: '" + req.project.string() + "' is not a project directory or .arcproj");
    const std::optional<Arcane::ProjectManifest> manifest = Arcane::ProjectManifest::LoadFile(*manifestFile);
    if (!manifest)
        return Refuse("'" + manifestFile->string() + "' is not a valid .arcproj");

    std::error_code ec;
    Layout layout;
    layout.manifest   = std::filesystem::absolute(*manifestFile, ec).lexically_normal();
    layout.root       = layout.manifest.parent_path();
    layout.name       = manifest->name;
    layout.gameModule = manifest->gameModule;

    SetSdkEnv(*sdk);
    Tools tools;
    tools.premake = Arcane::Toolchain::ResolvePremake(*sdk);
    tools.msbuild = Arcane::Toolchain::ResolveMsBuild();

    Say(std::string(CommandName(*command)) + " " + layout.name + " (" + req.config + ") in " +
        layout.root.generic_string() + " against SDK " + sdk->generic_string());

    switch (*command)
    {
        case Command::Probe:
        {
            const Probe   p = ProbeSlot(layout, req.config);
            const Verdict v = Decide(Command::Build, req.forceRebuild, p.state);
            // The row is the command's whole output -- never quieted.
            std::printf("[arcbuild] %s\n", ProbeRow(p, req.config, v).c_str());
            std::fflush(stdout);
            return ProbeExitCode(p.state);
        }

        case Command::Generate:
        {
            const std::string line = ComposeGenerate(layout, tools, req.action);
            Say(line);
            return ExitFromChild(Stream(line, "[premake]"));
        }

        case Command::Build:
        case Command::Rebuild:
        {
            // Premake FIRST, every build (idempotent; kills the stale-.sln
            // class of failure -- the editor's standing decision).
            const std::string gen = ComposeGenerate(layout, tools, req.action);
            Say(gen);
            const int genExit = ExitFromChild(Stream(gen, "[premake]"));
            if (genExit != kExitOk)
            {
                Say("premake exited with " + std::to_string(genExit) + " -- msbuild not run");
                return genExit;
            }

            const Probe   p = ProbeSlot(layout, req.config);
            const Verdict v = Decide(*command, req.forceRebuild, p.state);
            Say(ProbeRow(p, req.config, v));
            Say(v.reason);

            const std::filesystem::path solution =
                SolutionPath(layout, Arcane::Toolchain::DiscoverSolution(layout.root));
            const std::string build = ComposeMsBuild(tools, solution, req.config,
                                                     v.rebuild ? MsBuildTarget::Rebuild : MsBuildTarget::Build);
            Say(build);
            const int buildExit = ExitFromChild(Stream(build, "[msbuild]"));
            Say(buildExit == kExitOk ? "msbuild succeeded" : "msbuild exited with " + std::to_string(buildExit));
            return buildExit;
        }

        case Command::Clean:
        {
            int exit = kExitOk;
            const std::filesystem::path discovered = Arcane::Toolchain::DiscoverSolution(layout.root);
            if (discovered.empty())
                Say("no workspace file in " + layout.root.generic_string() + " -- skipping msbuild /t:Clean");
            else
            {
                const std::string clean = ComposeMsBuild(tools, discovered, req.config, MsBuildTarget::Clean);
                Say(clean);
                exit = ExitFromChild(Stream(clean, "[msbuild]"));
            }
            for (const std::filesystem::path& dir : CleanTargets(layout, req.config))
            {
                const auto removed = std::filesystem::remove_all(dir, ec);
                Say("removed " + dir.generic_string() + " (" + std::to_string(removed) + " entries)");
            }
            return exit;
        }
    }
    return Refuse("unreachable command");
}
