#include <Arcane/Build/Toolchain.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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

namespace Arcane::Toolchain
{
    namespace
    {
        // ASCII-lowercased extension: a hand-generated "Game.SLNX" is still
        // the workspace file.
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext;
        }

#ifdef _WIN32
        // UTF-8 -> UTF-16 for the _wpopen boundary (install paths may be
        // non-ANSI).
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
    }

    std::filesystem::path DiscoverSolution(const std::filesystem::path& projectRoot)
    {
        std::vector<std::filesystem::path> slnx, sln;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(projectRoot, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string ext = LowerExt(entry.path());
            if (ext == ".slnx")     slnx.push_back(entry.path());
            else if (ext == ".sln") sln.push_back(entry.path());
        }
        // Lexicographic within each bucket: directory_iterator order is
        // unspecified, and "first" must mean the same file every build.
        std::sort(slnx.begin(), slnx.end());
        std::sort(sln.begin(), sln.end());
        if (!slnx.empty()) return slnx.front();
        if (!sln.empty())  return sln.front();
        return {};
    }

    std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot)
    {
        const std::filesystem::path bundled =
            sdkRoot / "ThirdParty" / "premake5" / "premake5.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(bundled, ec))
            return bundled.lexically_normal();
        return "premake5";   // PATH fallback (cmd resolves it)
    }

    std::filesystem::path VsWhere(const std::string& arguments)
    {
#ifdef _WIN32
        // vswhere is the one install-location contract VS actually documents:
        // it always lives under %ProgramFiles(x86)%/Microsoft Visual Studio/
        // Installer once any VS >= 15.2 is present.
        const char* pf86 = std::getenv("ProgramFiles(x86)");
        if (pf86)
        {
            const std::filesystem::path vswhere =
                std::filesystem::path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
            std::error_code ec;
            if (std::filesystem::is_regular_file(vswhere, ec))
            {
                std::string query = "\"";
                query += vswhere.string();
                query += "\" ";
                query += arguments;
                // A quoted exe at the head of a bare _popen line loses its
                // quotes to cmd's outer-quote stripping; the standard dodge is
                // one extra wrapping pair.
                query = "\"" + query + "\"";
                if (FILE* pipe = ::_wpopen(Widen(query).c_str(), L"r"))
                {
                    char line[1024] = {};
                    std::string first;
                    if (std::fgets(line, sizeof(line), pipe))
                        first = line;
                    ::_pclose(pipe);
                    while (!first.empty() && (first.back() == '\n' || first.back() == '\r'))
                        first.pop_back();
                    if (!first.empty())
                        return std::filesystem::path(first);
                }
            }
        }
#else
        (void)arguments;
#endif
        return {};
    }

    std::filesystem::path ResolveMsBuild()
    {
        const std::filesystem::path found =
            VsWhere("-latest -requires Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe");
        if (!found.empty())
            return found;
        return "msbuild";   // PATH fallback (a Developer Command Prompt launch)
    }

    std::filesystem::path ResolveDevenv()
    {
        return VsWhere("-latest -find Common7\\IDE\\devenv.exe");
    }
}
