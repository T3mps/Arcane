#include "Process.hpp"

#include "Compose.hpp"
#include "Exit.hpp"

#include <cstddef>
#include <string>
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

namespace arcbuild
{
#ifdef _WIN32
    namespace
    {
        // ---- UTF-8 <-> UTF-16, reporting real conversion failures ---------
        //
        // MB_ERR_INVALID_CHARS turns an ill-formed UTF-8 byte sequence into a
        // hard error here rather than a silent best-effort substitution -- a
        // process launch is exactly the place a swallowed encoding bug turns
        // into "ran the wrong thing" or "argv[3] came out empty".
        std::expected<std::wstring, ProcessError> Widen(
            std::string_view utf8)
        {
            if (utf8.empty())
                return std::wstring();

            const int count =
                ::MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    utf8.data(),
                    static_cast<int>(utf8.size()),
                    nullptr,
                    0);

            if (count <= 0)
            {
                return std::unexpected(ProcessError{
                    "failed to convert UTF-8 to UTF-16 (error " +
                    std::to_string(::GetLastError()) + ")" });
            }

            std::wstring wide(static_cast<std::size_t>(count), L'\0');

            if (::MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    utf8.data(),
                    static_cast<int>(utf8.size()),
                    wide.data(),
                    count) <= 0)
            {
                return std::unexpected(ProcessError{
                    "failed to convert UTF-8 to UTF-16 (error " +
                    std::to_string(::GetLastError()) + ")" });
            }

            return wide;
        }

        // Best-effort widen -> UTF-8 narrow, used only to render a path for
        // a HUMAN-READABLE command-line token (BuildWindowsCommandLine's
        // argv[0]) and inside error messages -- never for lpApplicationName,
        // which always takes the path's own native UTF-16 directly. A
        // failure here degrades to an empty token, never a launch failure.
        std::string Narrow(
            const std::wstring& wide)
        {
            if (wide.empty())
                return {};

            const int count =
                ::WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    wide.c_str(),
                    static_cast<int>(wide.size()),
                    nullptr,
                    0,
                    nullptr,
                    nullptr);

            if (count <= 0)
                return {};

            std::string narrow(static_cast<std::size_t>(count), '\0');

            ::WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.c_str(),
                static_cast<int>(wide.size()),
                narrow.data(),
                count,
                nullptr,
                nullptr);

            return narrow;
        }

        // RAII for a Win32 HANDLE. Every handle this file creates (both pipe
        // ends, the process, the thread) is wrapped in one of these, so a
        // launch-error early return -- at ANY point -- closes exactly the
        // handles that exist at that point and nothing else.
        class UniqueHandle
        {
        public:
            UniqueHandle() = default;
            explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
            ~UniqueHandle() { reset(); }

            UniqueHandle(const UniqueHandle&)            = delete;
            UniqueHandle& operator=(const UniqueHandle&) = delete;

            UniqueHandle(UniqueHandle&& other) noexcept
                : handle_(other.release())
            {
            }

            UniqueHandle& operator=(UniqueHandle&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    handle_ = other.release();
                }
                return *this;
            }

            [[nodiscard]] HANDLE get() const noexcept { return handle_; }

            [[nodiscard]] HANDLE release() noexcept
            {
                HANDLE released = handle_;
                handle_ = nullptr;
                return released;
            }

            void reset(HANDLE handle = nullptr) noexcept
            {
                if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
                    ::CloseHandle(handle_);

                handle_ = handle;
            }

        private:
            HANDLE handle_ = nullptr;
        };

        // RAII for LPPROC_THREAD_ATTRIBUTE_LIST -- a separate type from
        // UniqueHandle because it is torn down with
        // DeleteProcThreadAttributeList (not CloseHandle) and never owns the
        // byte buffer backing it (that lives in a std::vector in Run()).
        class AttributeListGuard
        {
        public:
            AttributeListGuard() = default;

            explicit AttributeListGuard(LPPROC_THREAD_ATTRIBUTE_LIST list)
                : list_(list)
            {
            }

            ~AttributeListGuard()
            {
                if (list_)
                    ::DeleteProcThreadAttributeList(list_);
            }

            AttributeListGuard(const AttributeListGuard&)            = delete;
            AttributeListGuard& operator=(const AttributeListGuard&) = delete;

        private:
            LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
        };
    }
#endif

    // ---- Windows command-line quoting --------------------------------------
    //
    // The algorithm Microsoft documents for building a command line that a
    // real CRT-parsed program re-splits back into the SAME argv -- see
    // "Parsing C++ Command-Line Arguments" (learn.microsoft.com/cpp/c-
    // language/parsing-c-command-line-arguments) and CommandLineToArgvW's
    // matching parse rules. Byte-wise iteration over UTF-8 is safe here:
    // every UTF-8 continuation/lead byte is >= 0x80, so it can never be
    // confused with the ASCII space/tab/quote/backslash bytes this
    // algorithm looks for -- non-ASCII text passes through unexamined.
    std::string QuoteWindowsArgument(
        std::string_view argument)
    {
        // Bare and unquoted is fine UNLESS the argument is empty (an empty,
        // unquoted argv entry does not exist -- it would vanish from the
        // child's argv entirely and shift every later index) or contains a
        // character the CRT parser treats as a token/quote boundary.
        if (!argument.empty() &&
            argument.find_first_of(" \t\n\v\"") == std::string_view::npos)
        {
            return std::string(argument);
        }

        std::string result;
        result.push_back('"');

        std::size_t i = 0;

        for (;;)
        {
            std::size_t backslashes = 0;

            while (i < argument.size() && argument[i] == '\\')
            {
                ++i;
                ++backslashes;
            }

            if (i == argument.size())
            {
                // Trailing backslashes right before the closing quote WE are
                // about to append: double them, so the CRT reads 2n
                // backslashes then a quote as n literal backslashes with the
                // quote ending the string, never as an escaped quote.
                result.append(backslashes * 2, '\\');
                break;
            }
            else if (argument[i] == '"')
            {
                // Backslashes immediately before an EMBEDDED quote: double
                // them plus one more, so the extra backslash escapes the
                // quote itself (2n+1 backslashes then a quote == n literal
                // backslashes then a literal quote character).
                result.append(backslashes * 2 + 1, '\\');
                result.push_back('"');
                ++i;
            }
            else
            {
                // Backslashes not immediately before a quote are never
                // special to the CRT parser -- copied through as-is.
                result.append(backslashes, '\\');
                result.push_back(argument[i]);
                ++i;
            }
        }

        result.push_back('"');
        return result;
    }

    std::string BuildWindowsCommandLine(
        const ProcessSpec& spec)
    {
#ifdef _WIN32
        // The executable's own UTF-8 form, for THIS argv[0] token only --
        // never for lpApplicationName (ProcessRunner::Run takes that
        // directly from the path's native UTF-16, see the comment there).
        std::string commandLine = QuoteWindowsArgument(Narrow(spec.executable.wstring()));
#else
        std::string commandLine = QuoteWindowsArgument(spec.executable.string());
#endif

        for (const std::string& argument : spec.arguments)
        {
            commandLine.push_back(' ');
            commandLine += QuoteWindowsArgument(argument);
        }

        return commandLine;
    }

    ProcessResult ProcessRunner::Run(
        const ProcessSpec& spec,
        std::string_view   prefix) const
    {
#ifdef _WIN32
        const std::expected<std::wstring, ProcessError> commandLine =
            Widen(BuildWindowsCommandLine(spec));

        if (!commandLine)
            return std::unexpected(commandLine.error());

        // lpApplicationName: the EXACT binary this launch runs, taken
        // directly from the path's own native UTF-16 representation -- never
        // re-derived from lpCommandLine's argv[0], and (because it is
        // non-null) never subject to a %PATH% search at all. ProcessSpec::
        // executable is always an absolute, already-resolved path by the
        // time it reaches here (Compose*/BackendResolver never hand this a
        // bare tool name).
        const std::wstring applicationName = spec.executable.wstring();

        std::optional<std::wstring> workingDirectory;
        if (spec.workingDirectory)
            workingDirectory = spec.workingDirectory->wstring();

        // ---- one pipe, merged stdout+stderr -----------------------------
        SECURITY_ATTRIBUTES pipeAttributes{};
        pipeAttributes.nLength              = sizeof(pipeAttributes);
        pipeAttributes.bInheritHandle       = TRUE;
        pipeAttributes.lpSecurityDescriptor = nullptr;

        HANDLE readRaw  = nullptr;
        HANDLE writeRaw = nullptr;

        if (!::CreatePipe(&readRaw, &writeRaw, &pipeAttributes, 0))
        {
            return std::unexpected(ProcessError{
                "CreatePipe failed (error " + std::to_string(::GetLastError()) + ")" });
        }

        UniqueHandle readHandle(readRaw);
        UniqueHandle writeHandle(writeRaw);

        // CreatePipe makes BOTH ends inheritable (there is no way to ask for
        // only one). Only the WRITE end is meant to cross into the child --
        // and even that only via the explicit handle list below, never
        // broad bInheritHandles=TRUE inheritance -- so the PARENT's read end
        // has its inheritability stripped immediately: belt-and-braces
        // against a future child (spawned some other way) ever picking it
        // up too.
        if (!::SetHandleInformation(readHandle.get(), HANDLE_FLAG_INHERIT, 0))
        {
            return std::unexpected(ProcessError{
                "SetHandleInformation failed (error " + std::to_string(::GetLastError()) + ")" });
        }

        // ---- STARTUPINFOEXW + PROC_THREAD_ATTRIBUTE_HANDLE_LIST ---------
        //
        // bInheritHandles=TRUE is still required below (it is the master
        // switch for inheritance at all), but with this attribute present,
        // Windows 8+ restricts what ACTUALLY inherits to exactly the
        // handles named here -- the write end, and nothing else, regardless
        // of what other inheritable-flagged handles happen to exist in this
        // process at launch time (a handle-list contract test below pins
        // this: a stray inheritable sentinel handle NOT in this list must
        // come up invalid in the child).
        SIZE_T attributeListSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);

        std::vector<std::byte> attributeListStorage(attributeListSize);
        LPPROC_THREAD_ATTRIBUTE_LIST attributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());

        if (!::InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize))
        {
            return std::unexpected(ProcessError{
                "InitializeProcThreadAttributeList failed (error " +
                std::to_string(::GetLastError()) + ")" });
        }

        AttributeListGuard attributeListGuard(attributeList);

        HANDLE inheritedHandles[1] = { writeHandle.get() };

        if (!::UpdateProcThreadAttribute(
                attributeList,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandles,
                sizeof(inheritedHandles),
                nullptr,
                nullptr))
        {
            return std::unexpected(ProcessError{
                "UpdateProcThreadAttribute failed (error " +
                std::to_string(::GetLastError()) + ")" });
        }

        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb      = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        // No stdin: a null standard-input handle needs no entry in the
        // handle list above (it is not being inherited, just left absent),
        // and none of arcbuild's children ever read from it.
        startupInfo.StartupInfo.hStdInput  = nullptr;
        startupInfo.StartupInfo.hStdOutput = writeHandle.get();
        startupInfo.StartupInfo.hStdError  = writeHandle.get();
        startupInfo.lpAttributeList        = attributeList;

        // CreateProcessW may write into this buffer while parsing it -- a
        // string literal or a const-derived buffer is not safe here.
        std::vector<wchar_t> commandLineBuffer(commandLine->begin(), commandLine->end());
        commandLineBuffer.push_back(L'\0');

        PROCESS_INFORMATION processInfo{};

        const BOOL launched =
            ::CreateProcessW(
                applicationName.c_str(),
                commandLineBuffer.data(),
                nullptr,
                nullptr,
                /* bInheritHandles */ TRUE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                workingDirectory ? workingDirectory->c_str() : nullptr,
                &startupInfo.StartupInfo,
                &processInfo);

        if (!launched)
        {
            return std::unexpected(ProcessError{
                "CreateProcessW failed for '" + Narrow(applicationName) +
                "' (error " + std::to_string(::GetLastError()) + ")" });
        }

        UniqueHandle processHandle(processInfo.hProcess);
        UniqueHandle threadHandle(processInfo.hThread);

        // Close the PARENT's copy of the write end now: the child's
        // inherited copy is the only one left, so the read loop below sees
        // EOF exactly when the child (and anything it may have handed the
        // handle to) closes it -- never before, and never hangs on a copy
        // the parent forgot to release.
        writeHandle.reset();

        std::string carry;
        char        buffer[4096];

        for (;;)
        {
            DWORD      bytesRead = 0;
            const BOOL readOk =
                ::ReadFile(readHandle.get(), buffer, sizeof(buffer), &bytesRead, nullptr);

            if (!readOk)
            {
                // ERROR_BROKEN_PIPE is the NORMAL end of stream -- it fires
                // exactly when the child (and everything it may have handed
                // the write handle to) has closed it, i.e. every ordinary
                // clean exit. Any other failure is a genuine mid-stream read
                // error: rare, but silently truncating the child's log and
                // still reporting its exit code as if the capture were
                // complete would be worse than saying so -- surface it as a
                // diagnostic (never a ProcessError: the child already
                // launched and is running/finished, so this is not a
                // launch/setup failure, and must not turn into kExitRefused).
                const DWORD readError = ::GetLastError();

                if (readError != ERROR_BROKEN_PIPE)
                {
                    output_.Error(
                        "reading " + std::string(prefix) +
                        "'s output failed mid-stream (error " +
                        std::to_string(readError) +
                        ") -- the captured log above may be incomplete");
                }

                break;
            }

            if (bytesRead == 0)
                break;

            carry.append(buffer, bytesRead);

            std::size_t newline;
            while ((newline = carry.find('\n')) != std::string::npos)
            {
                std::string line = carry.substr(0, newline);

                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                output_.Child(prefix, line);
                carry.erase(0, newline + 1);
            }
        }

        // A final unterminated line (the child exited without a trailing
        // newline) still reaches the caller, rather than being dropped.
        if (!carry.empty())
            output_.Child(prefix, carry);

        ::WaitForSingleObject(processHandle.get(), INFINITE);

        DWORD exitCode = 0;

        if (!::GetExitCodeProcess(processHandle.get(), &exitCode))
        {
            return std::unexpected(ProcessError{
                "GetExitCodeProcess failed (error " + std::to_string(::GetLastError()) + ")" });
        }

        return static_cast<int>(exitCode);
#else
        (void)spec;
        (void)prefix;

        return std::unexpected(ProcessError{
            "process execution is not implemented on this platform yet" });
#endif
    }

    int ExecutePlan(
        const ProcessPlan& plan,
        IProcessRunner&    runner,
        IOutput&           output,
        std::string_view   prefix)
    {
        for (const ProcessSpec& step : plan.steps)
        {
            output.Info(RenderProcess(step));

            const ProcessResult result = runner.Run(step, prefix);

            if (!result)
            {
                output.Error(result.error().message);
                return kExitRefused;
            }

            if (*result != kExitOk)
                return *result;
        }

        return kExitOk;
    }
}
