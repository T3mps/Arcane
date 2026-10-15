#include "Settings/EditorRestart.hpp"

#include "Project/RuntimeLaunch.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Process.hpp>

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

namespace Arcane::Editor::EditorRestart
{
    std::vector<std::wstring> Args(const std::filesystem::path& projectRoot)
    {
        if (projectRoot.empty()) return {};
        return { L"--project", projectRoot.wstring() };
    }

    std::filesystem::path CurrentExe()
    {
        // GetModuleFileNameW / /proc/self/exe / _NSGetExecutablePath+realpath:
        // macOS has no /proc, so the old /proc read answered empty there.
        return Arcane::Platform::ExecutablePath();
    }

    bool Spawn(const std::filesystem::path& exe, const std::vector<std::wstring>& args)
    {
#ifdef _WIN32
        std::error_code ec;
        if (!std::filesystem::is_regular_file(exe, ec))
        {
            ARC_ERROR("Restart editor: '{}' does not exist", exe.string());
            return false;
        }
        std::wstring cmd = RuntimeLaunch::QuoteArg(exe.wstring());
        for (const std::wstring& a : args)
        {
            cmd.push_back(L' ');
            cmd += RuntimeLaunch::QuoteArg(a);
        }
        const std::wstring workDir = exe.parent_path().wstring();
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, workDir.c_str(), &si, &pi);
        if (!ok)
        {
            ARC_ERROR("Restart editor: CreateProcessW failed ({}) for '{}'", ::GetLastError(), exe.string());
            return false;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return true;
#else
        (void)args;
        ARC_ERROR("Restart editor: relaunch is Windows-only for now ('{}')", exe.string());
        return false;
#endif
    }
}
