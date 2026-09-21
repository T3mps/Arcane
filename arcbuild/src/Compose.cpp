#include "Compose.hpp"

#include <cctype>

namespace arcbuild
{
    namespace
    {
        void Quote(
            std::string& output,
            const std::filesystem::path& path)
        {
            const auto utf8 = path.u8string();

            output += '"';
            output.append(
                reinterpret_cast<const char*>(utf8.data()),
                utf8.size());
            output += '"';
        }

        std::string LowerAscii(
            std::string_view value)
        {
            std::string result(value);

            for (char& c : result)
            {
                c = static_cast<char>(
                    std::tolower(
                        static_cast<unsigned char>(c)));
            }

            return result;
        }
    }

    std::string ComposeGenerate(
        const ProjectLayout& project,
        const std::filesystem::path& premake,
        std::string_view action)
    {
        std::string command = "( cd /d ";

        Quote(command, project.root);

        command += " && ";

        Quote(command, premake);

        command += ' ';
        command += action;
        command += " ) 2>&1";

        return command;
    }

    std::string ComposeBuild(
        BuildBackend backend,
        const std::filesystem::path& builder,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        switch (backend)
        {
        case BuildBackend::MsBuild:
            return ComposeMsBuild(
                builder,
                context,
                config,
                operation);

        case BuildBackend::Make:
            return ComposeMake(
                builder,
                context,
                config,
                operation);

        case BuildBackend::Ninja:
            return ComposeNinja(
                builder,
                context,
                operation);

        case BuildBackend::XcodeBuild:
            return ComposeXcodeBuild(
                builder,
                context,
                config,
                operation);

        case BuildBackend::None:
            return {};
        }

        return {};
    }

    std::string ComposeMsBuild(
        const std::filesystem::path& msbuild,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        std::string command = "( ";

        Quote(command, msbuild);

        command += ' ';

        Quote(command, context.path);

        command += " /p:Configuration=";
        command += config;

        switch (operation)
        {
        case BuildOperation::Build:
            break;

        case BuildOperation::Rebuild:
            command += " /t:Rebuild";
            break;

        case BuildOperation::Clean:
            command += " /t:Clean";
            break;
        }

        command += " /m /nologo ) 2>&1";

        return command;
    }

    std::string ComposeMake(
        const std::filesystem::path& make,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        const std::string makeConfig =
            LowerAscii(config);

        auto appendInvocation =
            [&](std::string& command, bool clean)
            {
                Quote(command, make);

                command += " -C ";

                Quote(command, context.path);

                command += " config=";
                command += makeConfig;

                if (clean)
                    command += " clean";
            };

        std::string command = "( ";

        switch (operation)
        {
        case BuildOperation::Build:
            appendInvocation(command, false);
            break;

        case BuildOperation::Clean:
            appendInvocation(command, true);
            break;

        case BuildOperation::Rebuild:
            appendInvocation(command, true);
            command += " && ";
            appendInvocation(command, false);
            break;
        }

        command += " ) 2>&1";

        return command;
    }

    std::string ComposeNinja(
        const std::filesystem::path& ninja,
        const BackendContext& context,
        BuildOperation operation)
    {
        auto appendInvocation =
            [&](std::string& command, bool clean)
            {
                Quote(command, ninja);

                command += " -C ";

                Quote(command, context.path);

                if (clean)
                    command += " -t clean";
            };

        std::string command = "( ";

        switch (operation)
        {
        case BuildOperation::Build:
            appendInvocation(command, false);
            break;

        case BuildOperation::Clean:
            appendInvocation(command, true);
            break;

        case BuildOperation::Rebuild:
            appendInvocation(command, true);
            command += " && ";
            appendInvocation(command, false);
            break;
        }

        command += " ) 2>&1";

        return command;
    }

    std::string ComposeXcodeBuild(
        const std::filesystem::path& xcodebuild,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        std::string command = "( ";

        Quote(command, xcodebuild);

        command += " -project ";

        Quote(command, context.path);

        if (context.scheme)
        {
            command += " -scheme \"";
            command += *context.scheme;
            command += '"';
        }

        command += " -configuration \"";
        command += config;
        command += '"';

        switch (operation)
        {
        case BuildOperation::Build:
            command += " build";
            break;

        case BuildOperation::Rebuild:
            command += " clean build";
            break;

        case BuildOperation::Clean:
            command += " clean";
            break;
        }

        command += " ) 2>&1";

        return command;
    }
}
