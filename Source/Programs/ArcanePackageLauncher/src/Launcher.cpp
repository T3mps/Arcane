// The Windows package launcher (engine modularization s3.3, Unreal's package
// layout -- user decision 2026-10-12). Staged as <Pkg>/<Project>.exe by
// `arcbuild package`, it runs <Pkg>/<Project>/Binaries/Win64/<Project>.exe --
// the name comes from this exe's own file name -- with the same arguments and
// standard handles, waits, and returns its exit code. build/arcane.lua
// (arcane_package_launcher) builds it into each package workspace; Linux and
// macOS packages get a generated <Project>.sh instead.
#include <windows.h>

#include <string>

namespace
{
    // The command line after the program name, with its leading whitespace,
    // per the rules CommandLineToArgvW applies to argv[0]: a quoted name ends
    // at the next quote, a bare one at the first space or tab.
    std::wstring ArgumentsAfterProgramName(const wchar_t* line)
    {
        const wchar_t* p = line;
        if (*p == L'"')
        {
            ++p;
            while (*p && *p != L'"') ++p;
            if (*p == L'"') ++p;
        }
        else
        {
            while (*p && *p != L' ' && *p != L'\t') ++p;
        }
        return std::wstring(p);
    }

    bool Valid(HANDLE h) { return h != nullptr && h != INVALID_HANDLE_VALUE; }
}

int wmain()
{
    std::wstring self(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n == 0) return 1;
        if (n < self.size()) { self.resize(n); break; }
        self.resize(self.size() * 2);
    }
    const std::size_t slash = self.find_last_of(L"\\/");
    const std::wstring dir = self.substr(0, slash + 1);
    std::wstring stem = self.substr(slash + 1);
    if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".exe") == 0) stem.resize(stem.size() - 4);

    const std::wstring target = dir + stem + L"\\Binaries\\Win64\\" + stem + L".exe";
    std::wstring command = L"\"" + target + L"\"" + ArgumentsAfterProgramName(GetCommandLineW());

    // Hand the game our standard handles when we have any (a console, or a
    // caller's redirection); otherwise let it get its own (a Debug game opens
    // a console of its own when launched from Explorer).
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    const BOOL inherit = (Valid(in) || Valid(out) || Valid(err)) ? TRUE : FALSE;
    if (inherit)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in;
        si.hStdOutput = out;
        si.hStdError = err;
    }

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, inherit, 0, nullptr, nullptr, &si, &pi))
    {
        const std::wstring message = L"Cannot start " + target;
        if (Valid(err))
        {
            DWORD written = 0;
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
                                                  nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
            if (bytes > 0)
                WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
                                    utf8.data(), bytes, nullptr, nullptr);
            WriteFile(err, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            WriteFile(err, "\r\n", 2, &written, nullptr);
        }
        else
        {
            MessageBoxW(nullptr, message.c_str(), stem.c_str(), MB_ICONERROR | MB_OK);
        }
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
