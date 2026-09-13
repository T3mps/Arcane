#pragma once

// arcbuild::Driver -- the PURE core of the game-project build driver (spec
// docs/specs/2026-09-13-arcbuild-driver-design.md). Everything a build
// DECIDES lives here, testable without a process or a PE file ([build],
// ArcaneTests/src/BuildDriverTest.cpp): the CLI shape (s3), the --sdk /
// ARCANE_SDK precedence, the s4.3 incremental rule as a function over
// (slot exists, slot CRT flavor, --config, --force-rebuild), the exit-code
// table, the path conventions, and every child command line. main.cpp is
// the thin shell that loads the manifest, scans the slot, spawns each line
// and re-emits its output.
//
// NOT a second PE scanner: the slot's CrtFlavor comes from
// Arcane::Module::ScanFileCrtFlavor (main.cpp), the same verdict PluginHost
// uses to refuse a cross-CRT module, so the driver and the host can never
// disagree about what "matches" means. This header only names the enum.

#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Plugin/Module.hpp>   // Arcane::CrtFlavor (enum only; no link)

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arcbuild
{
    // ---- the CLI (spec s3) ----------------------------------------------------

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
        std::optional<std::filesystem::path> sdk;                // --sdk, when supplied
        std::string                          action = "vs2026";  // --action (the premake action; Linux seam)
        bool                                 forceRebuild = false;
        bool                                 quiet = false;      // suppress the driver's own info lines (ruling R5)
    };

    // The option/flag set of s3, registered on an Arcane::Cli. --project is
    // Required(); --config is Choices'd to the three configurations.
    [[nodiscard]] Arcane::Cli MakeCli();
    [[nodiscard]] Request RequestFromCli(Command command, const Arcane::Cli::Result& r);

    // --sdk > ARCANE_SDK > nullopt (the driver refuses). A set-but-empty
    // variable counts as unset.
    [[nodiscard]] std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag, const char* envValue);

    // ---- the incremental rule (spec s4.3) -------------------------------------

    // Which CRT family a configuration's DLL links: Debug <=> the debug CRT
    // (ucrtbased & co); Release AND Dist are both release-CRT, so Dist maps
    // onto Release for the probe -- the same caveat the editor's
    // ModuleBuild::Configuration() documents.
    [[nodiscard]] bool ConfigWantsDebugCrt(std::string_view config);

    // The row of the s4.3 table the slot lands on.
    enum class SlotState : std::uint8_t { Absent, Match, Mismatch, Unreadable };
    [[nodiscard]] const char* SlotStateName(SlotState s);
    [[nodiscard]] SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config);

    // The decision: plain build, or msbuild /t:Rebuild. `reason` is a static
    // string naming the row (printed as the driver's own log line).
    struct Verdict
    {
        bool        rebuild;
        const char* reason;
    };
    [[nodiscard]] Verdict Decide(Command command, bool forceRebuild, SlotState slot);

    // ---- exit codes -------------------------------------------------------------

    constexpr int kExitOk           = 0;
    constexpr int kExitRefused      = 2;   // no SDK / no project / bad flags (s3)
    constexpr int kExitProbeRebuild = 3;   // `probe`: the slot would force /t:Rebuild (ruling R4)

    [[nodiscard]] int ProbeExitCode(SlotState s);
    // A child's own status passes through (premake/msbuild exit 1 on failure,
    // cmd 9009 when the exe is not found); a pipe that could not open is a
    // driver refusal.
    [[nodiscard]] int ExitFromChild(std::optional<int> childExit);

    // ---- paths ------------------------------------------------------------------

    struct Layout
    {
        std::filesystem::path root;        // ABSOLUTE project directory (main.cpp absolutises; ruling R3)
        std::filesystem::path manifest;    // the .arcproj
        std::string           name;        // manifest `name` -> the <name>.slnx convention
        std::string           gameModule;  // manifest `gameModule`; empty = content-only project
    };

    // <root>/Binaries/<gameModule> -- the single slot HostBoot loads from
    // (Arcane/Host/ProjectBoot.hpp). Empty when the project has no module.
    [[nodiscard]] std::filesystem::path SlotPath(const Layout& l);

    // The workspace file msbuild drives: `discovered` (Toolchain::
    // DiscoverSolution's answer) when non-empty, else <root>/<name>.slnx --
    // the committed convention (a project's premake workspace is named after
    // the project), exactly as EditorApp::StartModuleRebuild assumed.
    [[nodiscard]] std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered);

    // ---- composition --------------------------------------------------------------
    // Every line runs through cmd.exe /c (_wpopen). Parenthesised so the
    // trailing 2>&1 folds the member's stderr into the captured stdout. Paths
    // are wrapped in plain quotes -- good for spaces, which real install
    // paths contain; an embedded quote is not defended against.

    struct Tools
    {
        std::filesystem::path premake;
        std::filesystem::path msbuild;
    };

    // ( cd /d "<root>" && "<premake>" <action> ) 2>&1 -- premake reads
    // ./premake5.lua from the cwd, hence the cd.
    [[nodiscard]] std::string ComposeGenerate(const Layout& l, const Tools& t, std::string_view action);

    enum class MsBuildTarget : std::uint8_t { Build, Rebuild, Clean };

    // ( "<msbuild>" "<solution>" /p:Configuration=<cfg> [/t:Rebuild|/t:Clean] /m /nologo ) 2>&1
    // No cd (the solution path is absolute); /t: only when the target is not
    // the default Build.
    [[nodiscard]] std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution,
                                             std::string_view config, MsBuildTarget target);

    // What `clean` removes after msbuild /t:Clean: <root>/Binaries (the whole
    // slot -- it is one slot) and <root>/Intermediate/<config>. Never Source/,
    // Content/, Saved/, the .slnx (generate rewrites it), nor
    // Intermediate/Artifacts (arccook's).
    [[nodiscard]] std::vector<std::filesystem::path> CleanTargets(const Layout& l, std::string_view config);
}
