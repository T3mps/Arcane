#include "Compose.hpp"

namespace arcbuild
{
    namespace
    {
        // UTF-8, not path::string() (ACP on Windows). Stream() Widen()s with
        // CP_UTF8, so a narrow ACP round-trip would mangle non-ASCII SDK paths.
        void Quote(std::string& out, const std::filesystem::path& p)
        {
            const auto u8 = p.u8string();
            out += '"';
            out.append(reinterpret_cast<const char*>(u8.data()), u8.size());
            out += '"';
        }
    }

    std::filesystem::path SlotPath(const Layout& l)
    {
        if (l.gameModule.empty())
            return {};
        return l.root / "Binaries" / l.gameModule;
    }

    std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered)
    {
        if (!discovered.empty())
        {
            if (discovered.is_absolute())
                return discovered;
            return (l.root / discovered).lexically_normal();
        }
        return l.root / (l.name + ".slnx");
    }

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

    std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution, std::string_view config, MsBuildTarget target)
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
