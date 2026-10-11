#include "Application.hpp"

#include "Exit.hpp"
#include "Request.hpp"

#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Platform/Paths.hpp>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arcbuild
{
    namespace
    {
        // The editor's Tools -> Rebuild Game Module spawns this exe, so it
        // reads the same build.* tool preferences (BuildToolSettings) the
        // editor does: the machine-wide EditorUser rung, then --set (the
        // CommandLine rung), published before anything resolves a tool.
        // Editor context: build.* is Editor-audience, and arcbuild is an
        // editor tool in every configuration.
        void ApplyToolPreferences(const std::vector<std::string>& sets)
        {
            Arcane::CVarRegistry& cvars = Arcane::CVarRegistry::Get();
            const std::filesystem::path editorUser =
                Arcane::Paths::Get(Arcane::Paths::Location::EditorUserDir);
            if (!editorUser.empty())
                Arcane::ApplyCVarDirectory(cvars, editorUser / "Config",
                                           Arcane::SetBy::EditorUser, "editor-user");
            Arcane::ApplyCVarCommandLine(cvars, sets, Arcane::CVarContext::Editor);
            cvars.PublishImmediate();
        }
    }

    void Application::PrintUsage() const
    {
        std::printf(
            "usage: arcbuild <generate|build|rebuild|clean|probe> "
            "--project <dir|.arcproj>\n"
            "                [--config Debug|Release|Dist] "
            "[--sdk <root>] [--action <premake-action>]\n"
            "                [--force-rebuild] [--quiet] [--set name=value]...\n"
            "  generate   run Premake <action> in the project root\n"
            "  build      generate, then invoke the action's build backend;\n"
            "             perform a full rebuild only when required by the slot\n"
            "             probe or --force-rebuild\n"
            "  rebuild    generate, then perform a full backend-native rebuild\n"
            "  clean      clean the generated build backend, then remove "
            "Binaries/ and\n"
            "             Intermediate/<config>/; filesystem cleanup still runs "
            "if backend clean fails\n"
            "  probe      print the slot state and exit 0 for absent/match or "
            "3 for mismatch/unreadable\n");

        std::fflush(stdout);
    }

    int Application::Run(
        int argc,
        char** argv)
    {
        if (argc < 2 ||
            std::string_view(argv[1]) == "--help" ||
            std::string_view(argv[1]) == "-h")
        {
            PrintUsage();

            return argc < 2
                ? kExitRefused
                : kExitOk;
        }

        const std::optional<Command> command =
            ParseCommand(argv[1]);

        if (!command)
        {
            PrintUsage();

            output_.Error(
                std::string("unknown command '") +
                argv[1] +
                "'");

            return kExitRefused;
        }

        const Arcane::Cli cli =
            MakeCli();

        const Arcane::Cli::Result parsed =
            cli.Parse(
                argc - 1,
                argv + 1);

        if (!parsed.ok)
            return parsed.exitCode;

        Request request =
            RequestFromCli(
                *command,
                parsed);

        if (const auto error =
            ValidateRequest(request))
        {
            output_.Error(*error);
            return kExitRefused;
        }

        output_.SetQuiet(
            request.quiet);

        ApplyToolPreferences(
            request.cvarSets);

        BootstrapResult prepared =
            bootstrap_.Prepare(
                std::move(request));

        if (!prepared)
        {
            output_.Error(
                prepared.error());

            return kExitRefused;
        }

        return pipeline_.Run(
            *prepared);
    }
}
