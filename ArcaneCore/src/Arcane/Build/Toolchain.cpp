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

        // The native PATH-list separator: ';' on Windows, ':' on POSIX.
        // pathExt (always ';'-separated, PATHEXT's own convention) uses the
        // Windows separator on every platform since it is only ever
        // meaningful there -- POSIX callers pass an empty pathExt.
#ifdef _WIN32
        constexpr char kPathListSep = ';';
#else
        constexpr char kPathListSep = ':';
#endif

        // Splits on `sep`, keeping empty leading/trailing/interior fields out
        // (an empty PATH entry is not a directory worth stat-ing).
        std::vector<std::string_view> SplitNonEmpty(std::string_view s, char sep)
        {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (start <= s.size())
            {
                const std::size_t pos = s.find(sep, start);
                const std::string_view field = (pos == std::string_view::npos)
                    ? s.substr(start)
                    : s.substr(start, pos - start);
                if (!field.empty())
                    parts.push_back(field);
                if (pos == std::string_view::npos)
                    break;
                start = pos + 1;
            }
            return parts;
        }

        // Windows: presence as a regular file is the whole contract -- NTFS
        // has no executable bit, and PATHEXT is what decides "this is a
        // program" upstream of us. POSIX: a regular file with at least one
        // executable bit set, so a readable-but-not-executable script or
        // data file with a matching name is correctly ignored.
        bool IsRunnableCandidate(const std::filesystem::path& candidate)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(candidate, ec) || ec)
                return false;
#ifdef _WIN32
            return true;
#else
            const std::filesystem::perms permissions =
                std::filesystem::status(candidate, ec).permissions();
            if (ec)
                return false;
            using std::filesystem::perms;
            return (permissions & (perms::owner_exec | perms::group_exec | perms::others_exec))
                != perms::none;
#endif
        }

#ifdef _WIN32
        // NTFS is case-insensitive but case-PRESERVING: IsRunnableCandidate
        // (is_regular_file) already matches a caller-cased "ninja.EXE"
        // against an on-disk "ninja.exe", but handing that fabricated
        // casing back as THE path would be a lie about what's really there.
        // FindFirstFileW reports the actual on-disk filename for whatever
        // case you probe it with; only the last path component needs
        // correcting; the directory part is left exactly as given.
        std::filesystem::path RealCasing(const std::filesystem::path& candidate)
        {
            WIN32_FIND_DATAW findData{};
            const HANDLE handle = ::FindFirstFileW(candidate.c_str(), &findData);
            if (handle == INVALID_HANDLE_VALUE)
                return candidate;
            ::FindClose(handle);
            return candidate.parent_path() / std::filesystem::path(findData.cFileName);
        }
#endif

        // Absolute + lexically_normal'd, for a candidate already proven to
        // exist (on Windows, corrected to its real on-disk casing first);
        // empty on the (essentially theoretical, since we just stat'd it)
        // failure to absolutise.
        std::filesystem::path AbsoluteNormal(const std::filesystem::path& p)
        {
#ifdef _WIN32
            const std::filesystem::path corrected = RealCasing(p);
#else
            const std::filesystem::path& corrected = p;
#endif
            std::error_code ec;
            const std::filesystem::path absolute = std::filesystem::absolute(corrected, ec);
            if (ec)
                return {};
            return absolute.lexically_normal();
        }

        // std::getenv wrapped as a value, never a dangling/nullptr string_view.
        std::string EnvOrEmpty(const char* name)
        {
            const char* value = std::getenv(name);
            return value ? std::string(value) : std::string();
        }
    }

    std::filesystem::path FindOnPath(
        std::string_view command,
        std::string_view searchPath,
        std::string_view pathExt)
    {
        if (command.empty())
            return {};

        const std::vector<std::string_view> directories = SplitNonEmpty(searchPath, kPathListSep);
        const std::vector<std::string_view> extensions   = SplitNonEmpty(pathExt, ';');

        for (const std::string_view directory : directories)
        {
            const std::filesystem::path dir(directory);

            // The command verbatim first -- covers a caller-supplied name
            // that already carries its extension (or a POSIX tool, which
            // never has one).
            const std::filesystem::path bare = dir / std::filesystem::path(command);
            if (IsRunnableCandidate(bare))
            {
                if (const std::filesystem::path found = AbsoluteNormal(bare); !found.empty())
                    return found;
            }

            for (const std::string_view extension : extensions)
            {
                std::string name(command);
                name.append(extension);
                const std::filesystem::path candidate = dir / name;
                if (IsRunnableCandidate(candidate))
                {
                    if (const std::filesystem::path found = AbsoluteNormal(candidate); !found.empty())
                        return found;
                }
            }
        }

        return {};
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
#ifdef _WIN32
        const std::filesystem::path bundled =
            sdkRoot / "ThirdParty" / "premake5" / "premake5.exe";
#else
        const std::filesystem::path bundled =
            sdkRoot / "ThirdParty" / "premake5" / "premake5";
#endif
        std::error_code ec;
        if (std::filesystem::is_regular_file(bundled, ec))
        {
            // Through AbsoluteNormal, same as every PATH hit below: this
            // file's contract is "absolute or empty", and a caller-relative
            // sdkRoot (Bootstrap absolutises its own, but nothing forces the
            // next caller to) would otherwise leak out as a relative path.
            if (const std::filesystem::path found = AbsoluteNormal(bundled); !found.empty())
                return found;
        }
        return FindOnPath("premake5", EnvOrEmpty("PATH"), EnvOrEmpty("PATHEXT"));
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
        const std::filesystem::path found = VsWhere("-latest -requires Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe");
        if (!found.empty())
            return found;
        // A Developer Command Prompt launch: "msbuild" resolves case-
        // insensitively to MSBuild.exe via PATHEXT on Windows.
        return FindOnPath("msbuild", EnvOrEmpty("PATH"), EnvOrEmpty("PATHEXT"));
    }

    std::filesystem::path ResolveMake()
    {
#ifdef _WIN32
        const std::string path    = EnvOrEmpty("PATH");
        const std::string pathExt = EnvOrEmpty("PATHEXT");
        // MSYS2/MinGW installs carry "mingw32-make" beside (or instead of)
        // "make"; prefer it when both are on PATH since a bare "make" there
        // is sometimes a stub that expects an MSYS shell.
        if (const std::filesystem::path mingw = FindOnPath("mingw32-make", path, pathExt); !mingw.empty())
            return mingw;
        return FindOnPath("make", path, pathExt);
#else
        return FindOnPath("make", EnvOrEmpty("PATH"), {});
#endif
    }

    std::filesystem::path ResolveNinja()
    {
#ifdef _WIN32
        return FindOnPath("ninja", EnvOrEmpty("PATH"), EnvOrEmpty("PATHEXT"));
#else
        return FindOnPath("ninja", EnvOrEmpty("PATH"), {});
#endif
    }

    std::filesystem::path ResolveXcodeBuild()
    {
#ifdef __APPLE__
        const std::filesystem::path fixed = "/usr/bin/xcodebuild";
        std::error_code ec;
        if (std::filesystem::is_regular_file(fixed, ec))
            return fixed;
        return FindOnPath("xcodebuild", EnvOrEmpty("PATH"), {});
#else
        // No Xcode toolchain exists off macOS -- there is no PATH worth
        // searching, and a match there would be a false positive (some
        // other tool happening to share the name).
        return {};
#endif
    }

    std::filesystem::path ResolveDevenv()
    {
        return VsWhere("-latest -find Common7\\IDE\\devenv.exe");
    }
}
