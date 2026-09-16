#include "Project/ServerLaunch.hpp"

#include "Project/RuntimeLaunch.hpp"   // QuoteArg -- ONE place gets Windows' escaping right

#include <Arcane/Base/Log.hpp>   // ARC_ERROR / ARC_INFO

#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane::Editor::ServerLaunch
{
    std::vector<std::filesystem::path> ExeCandidates(const std::filesystem::path& editorExeDir)
    {
        return {
            editorExeDir / "ArcaneServer.exe",
            editorExeDir / ".." / "ArcaneServer" / "ArcaneServer.exe",
        };
    }

    std::vector<std::wstring> BuildArgs(const std::filesystem::path& projectRoot)
    {
        std::vector<std::wstring> args;
        args.reserve(4);
        args.push_back(L"--project");
        args.push_back(projectRoot.wstring());
        // "Run until terminated" -- see the header. The editor's Stop is the end.
        args.push_back(L"--frames");
        args.push_back(L"0");
        return args;
    }

    ServerProcess::~ServerProcess()
    {
        Stop();
    }

    bool ServerProcess::Spawn(const std::filesystem::path& exe, const std::vector<std::wstring>& args)
    {
#ifdef _WIN32
        // Never leak a previous child: a second Play must not leave the first
        // server running and holding the project's files.
        Stop();

        std::error_code ec;
        if (!std::filesystem::is_regular_file(exe, ec))
        {
            ARC_ERROR("ServerLaunch: '{}' does not exist", exe.string());
            return false;
        }

        std::wstring cmdLine = RuntimeLaunch::QuoteArg(exe.wstring());
        for (const std::wstring& a : args)
        {
            cmdLine.push_back(L' ');
            cmdLine += RuntimeLaunch::QuoteArg(a);
        }

        // Same data-resolution rule every Arcane host relies on: paths resolve
        // relative to the PROCESS's own directory, not the caller's cwd.
        const std::wstring workDir = exe.parent_path().wstring();

        // CREATE_NO_WINDOW + a log file, for exactly the reasons
        // RuntimeLaunch::SpawnDetached's own comment states: ArcaneServer is a
        // console-subsystem exe, so without the flag Windows hands it a console
        // WINDOW that flashes black beside the editor, and without the redirect a
        // child that dies during Init takes the reason with it when that console
        // closes. Truncated per launch -- it answers "why did the server I just
        // started fail", not "what happened all week".
        HANDLE logHandle = INVALID_HANDLE_VALUE;
        const std::filesystem::path logPath = exe.parent_path() / "ArcaneServer.log";
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength        = sizeof(sa);
            sa.bInheritHandle = TRUE;   // the child can only receive an INHERITABLE handle
            logHandle = ::CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        const bool haveLog = (logHandle != INVALID_HANDLE_VALUE);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (haveLog)
        {
            si.dwFlags   |= STARTF_USESTDHANDLES;
            si.hStdOutput = logHandle;
            si.hStdError  = logHandle;
            si.hStdInput  = nullptr;   // the child never reads stdin
        }
        PROCESS_INFORMATION pi{};

        // lpCommandLine must be a MUTABLE buffer (CreateProcessW may rewrite it in
        // place); cmdLine is a local std::wstring, so .data() is safe.
        const BOOL ok = ::CreateProcessW(
            exe.c_str(),
            cmdLine.data(),
            nullptr, nullptr,
            haveLog ? TRUE : FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workDir.c_str(),
            &si, &pi);

        // Ours to close either way: the child holds its own duplicate, and leaving
        // this open would keep the file locked for the editor's life.
        if (haveLog)
            ::CloseHandle(logHandle);

        if (!ok)
        {
            ARC_ERROR("ServerLaunch: CreateProcessW failed for '{}' (error {})",
                      exe.string(), ::GetLastError());
            return false;
        }

        // The ONE difference from SpawnDetached: the PROCESS handle is KEPT (it is
        // what IsRunning/Stop act on); only the thread handle is closed here.
        ::CloseHandle(pi.hThread);
        m_handle = pi.hProcess;
        ARC_INFO("ServerLaunch: started ArcaneServer.exe (log: {})", logPath.string());
        return true;
#else
        (void)args;
        ARC_ERROR("ServerLaunch: Spawn('{}') is Windows-only (CreateProcessW)", exe.string());
        return false;
#endif
    }

    bool ServerProcess::IsRunning() const noexcept
    {
#ifdef _WIN32
        if (m_handle == nullptr) return false;
        return ::WaitForSingleObject(static_cast<HANDLE>(m_handle), 0) == WAIT_TIMEOUT;
#else
        return false;
#endif
    }

    void ServerProcess::Stop() noexcept
    {
#ifdef _WIN32
        // Silent no-op with nothing spawned -- the contract the editor's
        // unconditional post-Stop call depends on.
        if (m_handle == nullptr) return;
        HANDLE h = static_cast<HANDLE>(m_handle);
        m_handle = nullptr;
        ::TerminateProcess(h, 0);
        ::CloseHandle(h);
        ARC_INFO("ServerLaunch: stopped ArcaneServer.exe");
#endif
    }
}
