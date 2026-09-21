// arcbuild-process-fixture: a tiny, dependency-free console app ArcaneTests
// spawns through the REAL arcbuild::ProcessRunner (arcbuild/src/Process.cpp)
// to prove Win32 process-launch behavior end to end -- exact argv
// reconstruction through CreateProcessW's quoting, merged stdout+stderr
// streaming, cwd (lpCurrentDirectory), a handle-inheritance boundary probe,
// and child exit-code propagation. It is its OWN premake project (NOT part
// of ArcaneTests' own file glob) -- a second wmain/main in the same binary
// would not link -- built as `arcbuild-process-fixture` and located by
// BuildDriverTest.cpp's [build] cases the same "../<project>/<project>.exe"
// way DeskDriverExe() locates arcbuild.exe.
//
// Protocol: every line below is UTF-8, LF-terminated, written to the ONE
// stream a real child inherits as both stdout AND stderr from ProcessRunner
// (see BuildDriverTest.cpp for the reading side):
//
//   ARG:<n>:<value>        the n-th ordinary (non-flag) argv entry, in the
//                          order it was received; <n> counts only ordinary
//                          entries, so a control flag never shifts it
//   CWD:<path>             GetCurrentDirectoryW -- proves lpCurrentDirectory
//   HANDLE:valid|invalid   only printed for --probe-handle
//
// Recognized flags (consumed whole, never themselves an ARG: line):
//   --stderr <text>        write <text> to stderr (still the one merged pipe)
//   --probe-handle <n>     interpret <n> (decimal) as a HANDLE and probe it
//   --exit <n>             exit with code n (default 0 if never given)
//
// wmain, not main: argv here is the real UTF-16 split CommandLineToArgvW
// (and the CRT's own wmain startup) produces -- converting each entry to
// UTF-8 ourselves sidesteps the active-code-page ambiguity a narrow main's
// argv would carry for non-ASCII input.

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace
{
    std::string ToUtf8(
        const wchar_t* wide)
    {
        if (!wide || !*wide)
            return {};

        const int count =
            ::WideCharToMultiByte(
                CP_UTF8, 0,
                wide, -1,
                nullptr, 0,
                nullptr, nullptr);

        if (count <= 0)
            return {};

        std::string narrow(
            static_cast<std::size_t>(count),
            '\0');

        ::WideCharToMultiByte(
            CP_UTF8, 0,
            wide, -1,
            narrow.data(), count,
            nullptr, nullptr);

        // -1 above asked WideCharToMultiByte to include the null terminator
        // in both the count and the write -- std::string owns its own.
        if (!narrow.empty() && narrow.back() == '\0')
            narrow.pop_back();

        return narrow;
    }

    // Writes with fwrite/fputc, never printf("%s", ...): an argv value under
    // test may itself contain a literal '%', and this fixture's whole job is
    // to reproduce what it received byte for byte, not reinterpret it as a
    // format string.
    void PrintLine(
        FILE* stream,
        const std::string& line)
    {
        std::fwrite(line.data(), 1, line.size(), stream);
        std::fputc('\n', stream);
        std::fflush(stream);
    }

    bool ProbeHandleValid(
        unsigned long long value)
    {
        const HANDLE handle =
            reinterpret_cast<HANDLE>(
                static_cast<UINT_PTR>(value));

        ::SetLastError(0);
        const DWORD type = ::GetFileType(handle);

        // The documented MSDN pattern: FILE_TYPE_UNKNOWN is ambiguous on its
        // own (a real handle CAN legitimately report an unknown type with
        // GetLastError() == NO_ERROR) -- only FILE_TYPE_UNKNOWN together
        // with a non-zero last error means the handle itself was invalid.
        return !(type == FILE_TYPE_UNKNOWN && ::GetLastError() != 0);
    }
}

int wmain(
    int argc,
    wchar_t* argv[])
{
    // Unbuffered: every line reaches the parent's pipe as soon as it is
    // written, rather than waiting on a full CRT buffer or process exit.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    int exitCode = 0;
    int argIndex = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring token = argv[i];

        if (token == L"--stderr" && i + 1 < argc)
        {
            PrintLine(stderr, ToUtf8(argv[++i]));
        }
        else if (token == L"--probe-handle" && i + 1 < argc)
        {
            const unsigned long long value =
                std::wcstoull(argv[++i], nullptr, 10);

            PrintLine(
                stdout,
                std::string("HANDLE:") +
                    (ProbeHandleValid(value) ? "valid" : "invalid"));
        }
        else if (token == L"--exit" && i + 1 < argc)
        {
            exitCode = std::wcstol(argv[++i], nullptr, 10);
        }
        else
        {
            PrintLine(
                stdout,
                "ARG:" + std::to_string(argIndex) + ":" + ToUtf8(argv[i]));
            ++argIndex;
        }
    }

    wchar_t cwd[MAX_PATH]{};
    ::GetCurrentDirectoryW(MAX_PATH, cwd);
    PrintLine(stdout, std::string("CWD:") + ToUtf8(cwd));

    return exitCode;
}
