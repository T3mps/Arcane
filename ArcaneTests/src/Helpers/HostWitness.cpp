#include "HostWitness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
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

namespace Arcane::Test
{
#ifdef _WIN32
    namespace
    {
        // UTF-8 -> UTF-16 for the CreateProcessW boundary, same idiom as
        // ArcaneEditor's RuntimeLaunch::QuoteArg / ModuleBuild::Widen.
        std::wstring Utf8ToWide(const std::string& utf8)
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

        // Windows command-line quoting for ONE token (CreateProcessW takes a
        // single mutable string, not an argv array). Lifted verbatim from
        // RuntimeLaunch::QuoteArg -- same escaping rules, kept local here
        // because this is the only other spawn site in the tree.
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

        // stdout/stderr land beside reportPath (same directory, named off its
        // stem) when a report path was given, so a scenario's three artifacts
        // sit together; otherwise a fresh, uniquely-named temp directory --
        // multiple report-less runs in the same process must not collide.
        std::pair<std::filesystem::path, std::filesystem::path> ResolveStdioPaths(
            const WitnessInvocation& inv)
        {
            std::error_code ec;
            if (!inv.reportPath.empty())
            {
                const std::filesystem::path dir = inv.reportPath.parent_path();
                if (!dir.empty())
                    std::filesystem::create_directories(dir, ec);
                const std::wstring stem = inv.reportPath.stem().wstring();
                return { dir / (stem + L".stdout.txt"), dir / (stem + L".stderr.txt") };
            }

            static std::atomic<int> s_counter{ 0 };
            const std::filesystem::path uniqueDir =
                std::filesystem::temp_directory_path() / "arcane-witness-stdio" /
                (std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(s_counter.fetch_add(1, std::memory_order_relaxed)));
            std::filesystem::create_directories(uniqueDir, ec);
            return { uniqueDir / "stdout.txt", uniqueDir / "stderr.txt" };
        }

        HANDLE CreateInheritableLogFile(const std::filesystem::path& path)
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength        = sizeof(sa);
            sa.bInheritHandle = TRUE;
            // FILE_SHARE_READ so the watchdog can poll this file's size with
            // an independent handle (std::filesystem::file_size) while the
            // child's inherited duplicate keeps writing to it.
            return ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        std::uint64_t FileSizeOrZero(const std::filesystem::path& p)
        {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(p, ec);
            return ec ? 0 : static_cast<std::uint64_t>(sz);
        }

        // Kernel + user CPU time, in 100ns units -- a monotonically
        // non-decreasing scalar cheap enough to sample every watchdog slice.
        std::uint64_t ProcessCpu100ns(HANDLE process)
        {
            FILETIME creation{}, exitT{}, kernel{}, user{};
            if (!::GetProcessTimes(process, &creation, &exitT, &kernel, &user))
                return 0;
            ULARGE_INTEGER k{}, u{};
            k.LowPart  = kernel.dwLowDateTime;
            k.HighPart = kernel.dwHighDateTime;
            u.LowPart  = user.dwLowDateTime;
            u.HighPart = user.dwHighDateTime;
            return k.QuadPart + u.QuadPart;
        }
    }
#endif

    WitnessRun RunWitness(const WitnessInvocation& inv)
    {
        WitnessRun run;

#ifdef _WIN32
        const auto [stdoutPath, stderrPath] = ResolveStdioPaths(inv);
        run.stdoutPath = stdoutPath;
        run.stderrPath = stderrPath;

        HANDLE hOut = CreateInheritableLogFile(stdoutPath);
        HANDLE hErr = CreateInheritableLogFile(stderrPath);
        const bool haveOut = (hOut != INVALID_HANDLE_VALUE);
        const bool haveErr = (hErr != INVALID_HANDLE_VALUE);

        // cmd.exe (and only cmd.exe, empirically -- other targets don't care)
        // misparses its OWN argv[0] when the command-line TEXT spells it with
        // forward slashes: "C:/Windows/System32/cmd.exe /c \"exit 3\"" comes
        // back "The syntax of the command is incorrect." and exit code 1,
        // even though the exact same command with backslashes runs "exit 3"
        // correctly. This is a property of the bytes CreateProcessW hands the
        // child (GetCommandLineW), not of file lookup -- lpApplicationName
        // resolves fine either way. make_preferred() normalises to backslash
        // before either use, so callers (this file's own tests included) can
        // still spell exePath with forward slashes.
        const std::filesystem::path exe =
            std::filesystem::path(inv.exePath).make_preferred();

        std::wstring cmdLine = QuoteArg(exe.wstring());
        for (const std::string& a : inv.args)
        {
            cmdLine.push_back(L' ');
            cmdLine += QuoteArg(Utf8ToWide(a));
        }

        STARTUPINFOW si{};
        si.cb         = sizeof(si);
        si.dwFlags   |= STARTF_USESTDHANDLES;
        si.hStdOutput = haveOut ? hOut : nullptr;
        si.hStdError  = haveErr ? hErr : nullptr;
        si.hStdInput  = nullptr;   // the child never reads stdin

        PROCESS_INFORMATION pi{};

        const auto start = std::chrono::steady_clock::now();

        // lpCommandLine must be a MUTABLE buffer (CreateProcessW may rewrite
        // it in place); cmdLine is a local std::wstring, so .data() is safe.
        const BOOL ok = ::CreateProcessW(
            exe.c_str(),
            cmdLine.data(),
            nullptr, nullptr,
            TRUE,   // inherit handles -- required for the stdout/stderr redirect
            CREATE_NO_WINDOW,
            nullptr,
            inv.workingDir.empty() ? nullptr : inv.workingDir.c_str(),
            &si, &pi);

        // Ours to close either way: the child holds its own duplicate.
        if (haveOut) ::CloseHandle(hOut);
        if (haveErr) ::CloseHandle(hErr);

        if (ok)
        {
            // Watchdog: 250ms slices up to hardCapMs. Each slice that does
            // NOT observe exit, sample stdout size + CPU time so that if the
            // NEXT slice is the one that hits the cap, progressingAtKill
            // reflects the final window rather than the whole run.
            constexpr DWORD kSliceMs = 250;
            std::uint64_t elapsedMs      = 0;
            std::uint64_t lastStdoutSize = 0;
            std::uint64_t lastCpu        = 0;
            bool          haveSample     = false;
            DWORD         exitCode       = 0;
            bool          exited         = false;

            for (;;)
            {
                DWORD waitMs = kSliceMs;
                if (elapsedMs + kSliceMs >= inv.hardCapMs)
                    waitMs = (inv.hardCapMs > elapsedMs)
                                 ? static_cast<DWORD>(inv.hardCapMs - elapsedMs)
                                 : 0;

                const DWORD wr = ::WaitForSingleObject(pi.hProcess, waitMs);
                elapsedMs += waitMs;

                if (wr == WAIT_OBJECT_0)
                {
                    ::GetExitCodeProcess(pi.hProcess, &exitCode);
                    exited = true;
                    break;
                }

                const std::uint64_t curSize = FileSizeOrZero(run.stdoutPath);
                const std::uint64_t curCpu  = ProcessCpu100ns(pi.hProcess);
                const bool stdoutGrew  = haveSample && (curSize > lastStdoutSize);
                const bool cpuAdvanced = haveSample && (curCpu > lastCpu);
                lastStdoutSize = curSize;
                lastCpu        = curCpu;
                haveSample     = true;

                if (elapsedMs >= inv.hardCapMs)
                {
                    run.timedOut          = true;
                    run.progressingAtKill = stdoutGrew || cpuAdvanced;

                    ::TerminateProcess(pi.hProcess, 1);
                    // Arc A's unkillable-process guard: bounded wait, never
                    // hang the suite even if termination doesn't land.
                    const DWORD killWr = ::WaitForSingleObject(pi.hProcess, 3000);
                    if (killWr == WAIT_OBJECT_0)
                    {
                        ::GetExitCodeProcess(pi.hProcess, &exitCode);
                        exited = true;
                    }
                    break;
                }
            }

            run.exitCode = exited ? static_cast<int>(exitCode) : -1;

            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }

        run.wallMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        // Report load: only asked for when a path was given; found/parsed
        // are independent facts so a caller can tell "never wrote it" apart
        // from "wrote something we couldn't parse".
        if (!inv.reportPath.empty())
        {
            std::error_code ec;
            run.reportFound = std::filesystem::exists(inv.reportPath, ec);
            if (run.reportFound)
            {
                std::ifstream rf(inv.reportPath, std::ios::binary);
                nlohmann::json parsed = nlohmann::json::parse(rf, nullptr, false);
                run.reportParsed = !parsed.is_discarded();
                if (run.reportParsed)
                    run.report = std::move(parsed);
            }
        }
#else
        (void)inv;
#endif

        return run;
    }

    std::optional<Arcane::Verdict> GradeProcessFacts(const WitnessRun& run)
    {
        if (run.timedOut)
            return Arcane::Verdict::Errored;
        if (!run.reportFound || !run.reportParsed)
            return Arcane::Verdict::Indeterminate;
        return std::nullopt;
    }

    WitnessScratch::WitnessScratch(const std::filesystem::path& srcStagedDir,
                                    std::string scenarioName)
    {
#ifdef _WIN32
        const std::uint32_t pid = ::GetCurrentProcessId();
#else
        const std::uint32_t pid = 0;
#endif
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "arcane-witness";
        m_dir = root / (scenarioName + "-" + std::to_string(pid));

        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);   // stale leftover from a prior KEPT run
        std::filesystem::create_directories(root, ec);
        std::filesystem::copy(srcStagedDir, m_dir,
                               std::filesystem::copy_options::recursive, ec);
    }

    WitnessScratch::~WitnessScratch()
    {
        if (std::uncaught_exceptions() > 0)
        {
            // Catch2 REQUIRE failures throw during unwind -- this is how a
            // failing scenario keeps its artifact instead of deleting the
            // one thing that would explain the failure.
            std::cerr << "KEPT WITNESS ARTIFACT: " << m_dir.string() << "\n";
            return;
        }

        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    const std::filesystem::path& WitnessScratch::Dir() const
    {
        return m_dir;
    }
}
