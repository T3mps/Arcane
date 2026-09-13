#include "Project/ModuleBuild.hpp"

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

    std::vector<std::filesystem::path> DriverCandidates(const std::filesystem::path& editorExeDir)
    {
        return {
            editorExeDir / "arcbuild.exe",
            editorExeDir / ".." / "arcbuild" / "arcbuild.exe",
        };
    }

    std::filesystem::path ResolveDriver(const std::filesystem::path& editorExeDir)
    {
        std::error_code ec;
        for (const std::filesystem::path& candidate : DriverCandidates(editorExeDir))
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        return {};
    }

    std::string ComposeDriverCommand(const DriverInputs& in)
    {
        std::string cmd = "( ";
        Quote(cmd, in.driverExe);
        cmd += ' ';
        cmd += in.command;
        cmd += " --project ";
        Quote(cmd, in.projectRoot);
        cmd += " --config ";
        cmd += in.configuration;
        cmd += " --sdk ";
        Quote(cmd, in.sdkRoot);
        cmd += " ) 2>&1";
        return cmd;
    }

    CaptureResult RunCapture(const std::string& commandLine)
    {
        CaptureResult out;
#ifdef _WIN32
        // Same _wpopen shape as the Runner's worker (see Runner::Start), just
        // on the calling thread: the composed ( ... ) 2>&1 line runs through
        // cmd.exe /c and its merged output is read to EOF.
        if (FILE* pipe = ::_wpopen(Widen(commandLine).c_str(), L"r"))
        {
            char buf[4096];
            while (std::fgets(buf, sizeof(buf), pipe))
            {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                out.lines.push_back(std::move(line));
            }
            out.exit = ::_pclose(pipe);
        }
        else
            out.lines.push_back("could not start the shell (_wpopen failed)");
#else
        out.lines.push_back("project generation is Windows-only today (cmd + premake)");
        (void)commandLine;
#endif
        return out;
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
