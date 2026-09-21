#include "Compose.hpp"

#include <cctype>
#include <utility>

namespace arcbuild
{
    namespace
    {
        std::string PathArgument(
            const std::filesystem::path& path)
        {
            // generic_string() (forward slashes) rather than string(): these
            // are argv entries handed to a real process, never a shell, so
            // the separator style is cosmetic -- forward slashes read the
            // same on every host this driver targets.
            return path.generic_string();
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

        bool NeedsDisplayQuoting(
            const std::string& token)
        {
            if (token.empty())
                return true;

            for (const char c : token)
            {
                if (std::isspace(static_cast<unsigned char>(c)))
                    return true;
            }

            return false;
        }
    }

    ProcessSpec ComposeGenerate(
        const ProjectLayout& project,
        const std::filesystem::path& premake,
        std::string_view action)
    {
        ProcessSpec spec;
        spec.executable       = premake;
        spec.arguments        = { std::string(action) };
        spec.workingDirectory = project.root;
        return spec;
    }

    ProcessPlan ComposeBuild(
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
                config,
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

    ProcessPlan ComposeMsBuild(
        const std::filesystem::path& msbuild,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        ProcessSpec spec;
        spec.executable = msbuild;
        spec.arguments.push_back(PathArgument(context.path));
        spec.arguments.push_back("/p:Configuration=" + std::string(config));

        switch (operation)
        {
        case BuildOperation::Build:
            break;

        case BuildOperation::Rebuild:
            spec.arguments.push_back("/t:Rebuild");
            break;

        case BuildOperation::Clean:
            spec.arguments.push_back("/t:Clean");
            break;
        }

        spec.arguments.push_back("/m");
        spec.arguments.push_back("/nologo");

        ProcessPlan plan;
        plan.steps.push_back(std::move(spec));
        return plan;
    }

    ProcessPlan ComposeMake(
        const std::filesystem::path& make,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        const std::string makeConfig =
            LowerAscii(config);

        auto invocation =
            [&](bool clean)
            {
                ProcessSpec spec;
                spec.executable = make;
                spec.arguments  =
                {
                    "-C",
                    PathArgument(context.path),
                    "config=" + makeConfig
                };

                if (clean)
                    spec.arguments.push_back("clean");

                return spec;
            };

        ProcessPlan plan;

        switch (operation)
        {
        case BuildOperation::Build:
            plan.steps.push_back(invocation(false));
            break;

        case BuildOperation::Clean:
            plan.steps.push_back(invocation(true));
            break;

        case BuildOperation::Rebuild:
            plan.steps.push_back(invocation(true));
            plan.steps.push_back(invocation(false));
            break;
        }

        return plan;
    }

    ProcessPlan ComposeNinja(
        const std::filesystem::path& ninja,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        // The Ninja workspace target is <module-stem>_<Config> (beta8's
        // naming for a workspace's per-configuration aggregate target --
        // build/arcane.lua's `action:ninja` filter is the matching link-
        // location half of this). BackendResolver stashes the module stem
        // in BackendContext::scheme once it confirms <module-stem>.ninja
        // exists alongside build.ninja (Backend.cpp).
        const std::string stem =
            context.scheme
                ? *context.scheme
                : std::string();

        const std::string target =
            stem + "_" + std::string(config);

        auto invocation =
            [&](bool clean)
            {
                ProcessSpec spec;
                spec.executable = ninja;
                spec.arguments  =
                {
                    "-C",
                    PathArgument(context.path)
                };

                if (clean)
                {
                    spec.arguments.push_back("-t");
                    spec.arguments.push_back("clean");
                }

                spec.arguments.push_back(target);

                return spec;
            };

        ProcessPlan plan;

        switch (operation)
        {
        case BuildOperation::Build:
            plan.steps.push_back(invocation(false));
            break;

        case BuildOperation::Clean:
            plan.steps.push_back(invocation(true));
            break;

        case BuildOperation::Rebuild:
            plan.steps.push_back(invocation(true));
            plan.steps.push_back(invocation(false));
            break;
        }

        return plan;
    }

    ProcessPlan ComposeXcodeBuild(
        const std::filesystem::path& xcodebuild,
        const BackendContext& context,
        std::string_view config,
        BuildOperation operation)
    {
        // beta8's xcode4 generator creates a .xcworkspace + .xcodeproj but no
        // shared scheme (Global Constraints, multibackend hardening plan) --
        // BackendResolver stores the module stem in BackendContext::scheme
        // and it is composed here as `-target`, never `-scheme`.
        const std::string target =
            context.scheme
                ? *context.scheme
                : std::string();

        ProcessSpec spec;
        spec.executable = xcodebuild;
        spec.arguments  =
        {
            "-project",
            PathArgument(context.path),
            "-target",
            target,
            "-configuration",
            std::string(config)
        };

        switch (operation)
        {
        case BuildOperation::Build:
            spec.arguments.push_back("build");
            break;

        case BuildOperation::Rebuild:
            // ONE invocation, not two steps (unlike Make/Ninja above):
            // xcodebuild takes "clean" and "build" as two positional
            // actions in a single command line.
            spec.arguments.push_back("clean");
            spec.arguments.push_back("build");
            break;

        case BuildOperation::Clean:
            spec.arguments.push_back("clean");
            break;
        }

        ProcessPlan plan;
        plan.steps.push_back(std::move(spec));
        return plan;
    }

    std::string RenderProcess(
        const ProcessSpec& process)
    {
        std::string rendered;

        auto appendToken =
            [&](const std::string& token)
            {
                if (!rendered.empty())
                    rendered += ' ';

                if (NeedsDisplayQuoting(token))
                {
                    rendered += '"';
                    rendered += token;
                    rendered += '"';
                }
                else
                {
                    rendered += token;
                }
            };

        appendToken(PathArgument(process.executable));

        for (const std::string& argument : process.arguments)
            appendToken(argument);

        return rendered;
    }
}
