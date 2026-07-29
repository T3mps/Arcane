#include "RuntimeLaunch.hpp"

#include <Arcane/Base/Log.hpp>   // ARC_ERROR

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

namespace Arcane::Editor::RuntimeLaunch
{
    namespace
    {
        // Windows command-line quoting (CreateProcessW takes ONE string, not
        // an argv array, so something has to reproduce CommandLineToArgvW's
        // own escaping rules): wrap in quotes when the token is empty or has
        // whitespace/a quote, doubling backslashes that immediately precede a
        // literal quote or the closing wrapper. Kept private to this TU --
        // BuildArgs deliberately returns UNQUOTED tokens (see its header
        // comment), so this is the one place that has to get Windows'
        // escaping right.
        std::wstring QuoteArg(const std::wstring& arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
                return arg;

            std::wstring out = L"\"";
            std::size_t backslashes = 0;
            for (wchar_t c : arg)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                    backslashes = 0;
                    out.push_back(L'"');
                    continue;
                }
                out.append(backslashes, L'\\');
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        }
    }

    std::vector<std::filesystem::path> ExeCandidates(const std::filesystem::path& editorExeDir)
    {
        return {
            editorExeDir / "ArcaneRuntime.exe",
            editorExeDir / ".." / "ArcaneRuntime" / "ArcaneRuntime.exe",
        };
    }

    std::vector<std::wstring> BuildArgs(const std::filesystem::path& projectRoot,
                                         const Arcane::Guid& scene,
                                         Arcane::GraphicsBackend backend)
    {
        std::vector<std::wstring> args;
        args.reserve(6);

        args.push_back(L"--project");
        args.push_back(projectRoot.wstring());

        if (scene.IsValid())
        {
            // Guid::ToString() is canonical lowercase hex + dashes -- pure
            // ASCII, so a plain widen is exact (no MultiByteToWideChar needed).
            const std::string guidText = scene.ToString();
            args.push_back(L"--scene");
            args.emplace_back(guidText.begin(), guidText.end());
        }

        args.push_back(L"--backend");
        args.push_back(backend == Arcane::GraphicsBackend::Vulkan ? L"vulkan" : L"dx12");

        return args;
    }

    bool SpawnDetached(const std::filesystem::path& exe, const std::vector<std::wstring>& args)
    {
#ifdef _WIN32
        std::error_code ec;
        if (!std::filesystem::is_regular_file(exe, ec))
        {
            ARC_ERROR("RuntimeLaunch: '{}' does not exist", exe.string());
            return false;
        }

        std::wstring cmdLine = QuoteArg(exe.wstring());
        for (const std::wstring& a : args)
        {
            cmdLine.push_back(L' ');
            cmdLine += QuoteArg(a);
        }

        // Same shader-resolution rule ArcaneRuntime/ArcaneEditor already rely
        // on: ShaderLibrary/ShaderCompiler resolve "shaders/" and friends
        // relative to the PROCESS's own directory, not the caller's cwd.
        const std::wstring workDir = exe.parent_path().wstring();

        // The child is a CONSOLE-subsystem exe, and the editor is typically
        // launched from the Hub with no console of its own -- so Windows would
        // hand the child a brand new console WINDOW, which flashes up black
        // beside the game window and vanishes with it. CREATE_NO_WINDOW
        // suppresses it, and its output goes to a log file instead: a
        // fire-and-forget child that dies during Init would otherwise take the
        // only record of WHY with it (the console closes with the process, far
        // too fast to read). The log is truncated per launch -- it answers
        // "why did the run I just started fail", not "what happened all week".
        //
        // (dwCreationFlags is also where CREATE_NEW_CONSOLE would go if the
        // future server-set play modes want a visible per-server console.)
        HANDLE logHandle = INVALID_HANDLE_VALUE;
        const std::filesystem::path logPath = exe.parent_path() / "ArcaneRuntime.log";
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

        // lpCommandLine must be a MUTABLE buffer (CreateProcessW may rewrite
        // it in place); cmdLine is a local std::wstring, so .data() is safe.
        // bInheritHandles must be TRUE for the redirect above to reach the
        // child at all -- it is FALSE when there is no log to hand over.
        const BOOL ok = ::CreateProcessW(
            exe.c_str(),
            cmdLine.data(),
            nullptr, nullptr,
            haveLog ? TRUE : FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workDir.c_str(),
            &si, &pi);

        // Ours to close either way: the child holds its own duplicate, and
        // leaving this open would keep the file locked for the editor's life.
        if (haveLog)
            ::CloseHandle(logHandle);

        if (!ok)
        {
            ARC_ERROR("RuntimeLaunch: CreateProcessW failed for '{}' (error {})",
                      exe.string(), ::GetLastError());
            return false;
        }

        // Fire-and-forget: no tracking, no waiting. Close both handles right
        // away so this process holds nothing open on the child.
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return true;
#else
        ARC_ERROR("RuntimeLaunch: SpawnDetached('{}') is Windows-only (CreateProcessW)", exe.string());
        return false;
#endif
    }
}
