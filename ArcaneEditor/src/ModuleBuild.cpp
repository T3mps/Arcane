#include "ModuleBuild.hpp"

#include <Arcane/Base/Log.hpp>   // ARC_ERROR (pipe-open failure)

#include <algorithm>
#include <cstdio>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane::Editor::ModuleBuild
{
    namespace
    {
        // ASCII-lowercased extension (the LowerExtension pattern from
        // EditorAppFrame.cpp): a hand-generated "Game.SLNX" is still the
        // workspace file.
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext;
        }

#ifdef _WIN32
        // UTF-8 -> UTF-16 for the _wpopen boundary. Project roots are user
        // paths and may be non-ANSI; composing in UTF-8 keeps the pure half
        // testable while the spawn stays wide.
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

        void Quote(std::string& out, const std::filesystem::path& p)
        {
            out += '"';
            out += p.string();
            out += '"';
        }
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

    std::filesystem::path SdkRootFromExeDir(const std::filesystem::path& exeDir)
    {
        // <sdk>/bin/<cfg>-<system>-<arch>-md/ArcaneEditor -> <sdk>. A
        // trailing separator ("...\ArcaneEditor\") survives lexically_normal
        // as an empty filename element and would spend one of the three
        // parent_path steps on it -- drop it first so the walk starts at the
        // real leaf.
        std::filesystem::path p = exeDir.lexically_normal();
        if (!p.has_filename())
            p = p.parent_path();
        return p.parent_path().parent_path().parent_path();
    }

    std::string ComposeRebuildCommands(const ComposeInputs& in)
    {
        std::string cmd = "( cd /d ";
        Quote(cmd, in.projectRoot);
        cmd += " && ";
        Quote(cmd, in.premakeExe);
        cmd += " vs2026 && ";
        Quote(cmd, in.msbuildExe);
        cmd += ' ';
        Quote(cmd, in.solution);
        cmd += " /p:Configuration=";
        cmd += in.configuration;
        cmd += " /m /nologo ) 2>&1";
        return cmd;
    }

    std::filesystem::path ExeDir()
    {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return std::filesystem::current_path();
        return std::filesystem::path(buf).parent_path();
#else
        return std::filesystem::current_path();
#endif
    }

    std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot)
    {
        const std::filesystem::path bundled =
            sdkRoot / ".." / "ThirdParty" / "premake5" / "premake5.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(bundled, ec))
            return bundled.lexically_normal();
        return "premake5";   // PATH fallback (cmd resolves it)
    }

    std::filesystem::path ResolveMsBuild()
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
                query += "\" -latest -requires Microsoft.Component.MSBuild"
                         " -find MSBuild\\**\\Bin\\MSBuild.exe";
                // A quoted exe at the head of a bare _popen line loses its
                // quotes to cmd's outer-quote stripping; the standard dodge is
                // one extra wrapping pair.
                query = "\"" + query + "\"";
                if (FILE* pipe = _wpopen(Widen(query).c_str(), L"r"))
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
#endif
        return "msbuild";   // PATH fallback (a Developer Command Prompt launch)
    }

    void SetSdkEnv(const std::filesystem::path& sdkRoot)
    {
#ifdef _WIN32
        // The process environment block is what a spawned cmd inherits (and
        // what the child's CRT/os.getenv is initialized from) -- the CRT-side
        // _putenv mirror is not needed for inheritance.
        ::SetEnvironmentVariableW(L"ARCANE_SDK", sdkRoot.wstring().c_str());
#else
        (void)sdkRoot;
#endif
    }

    bool Runner::Start(std::string commandLine)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_running)
                return false;
            m_running = true;
            m_done    = false;
            m_exit    = -1;
            m_lines.clear();
        }
        // The previous worker (if any) has already cleared m_running, so this
        // join returns immediately -- it only reclaims the finished thread.
        if (m_thread.joinable())
            m_thread.join();

        m_thread = std::thread([this, cmd = std::move(commandLine)]
        {
            int exit = -1;
#ifdef _WIN32
            // _wpopen runs the line through cmd.exe /c, which is exactly what
            // the composed ( ... && ... ) 2>&1 shape needs. Both editor exes
            // are ConsoleApp, so the child inherits a console (possibly the
            // Hub's hidden one) instead of flashing up a new window.
            if (FILE* pipe = ::_wpopen(Widen(cmd).c_str(), L"r"))
            {
                char buf[4096];
                // fgets returns per newline; a >4 KB line arrives as two queue
                // entries, which for build output is a cosmetic non-event.
                while (std::fgets(buf, sizeof(buf), pipe))
                {
                    std::string line(buf);
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                        line.pop_back();
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_lines.push_back(std::move(line));
                }
                exit = ::_pclose(pipe);
            }
            else
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_lines.push_back("could not start the build shell (_wpopen failed)");
            }
#else
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_lines.push_back("module rebuild is Windows-only today (cmd + msbuild)");
            }
            (void)cmd;
#endif
            std::lock_guard<std::mutex> lk(m_mutex);
            m_exit    = exit;
            m_running = false;
            m_done    = true;
        });
        return true;
    }

    bool Runner::Running() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_running;
    }

    std::vector<std::string> Runner::DrainLines()
    {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(m_mutex);
        out.swap(m_lines);
        return out;
    }

    std::optional<int> Runner::TakeExit()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_done)
            return std::nullopt;
        m_done = false;
        return m_exit;
    }

    void Runner::Join()
    {
        if (m_thread.joinable())
            m_thread.join();
    }
}
