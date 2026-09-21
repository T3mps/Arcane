#include "Process.hpp"

#include "Compose.hpp"
#include "Exit.hpp"

#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#else
// POSIX process launch (Task 6). Deliberately the ONLY place these headers
// appear, and wholly outside the _WIN32 branch above: a stray unguarded
// <unistd.h> would break the Windows compilation of this very file.
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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

    namespace
    {
        // Portable on purpose (no <cerrno> value and no std::error_code
        // facility is POSIX-only), so DescribeChildSetupError below is
        // compiled and unit-tested on every platform -- including a Windows
        // desk, which is the only place the POSIX runner's own syscalls
        // cannot be exercised.
        //
        // std::generic_category().message() rather than std::strerror: the
        // same POSIX errno vocabulary, thread-safe, and not a deprecated-on-
        // MSVC call this shared translation unit would have to silence.
        std::string DescribeErrorNumber(
            int errorNumber)
        {
            return "errno " + std::to_string(errorNumber) + ": " +
                   std::generic_category().message(errorNumber);
        }

        // Drains every COMPLETE line out of `carry` to Output::Child and
        // leaves any unterminated tail behind for the next read. Shared by
        // both platform branches of ProcessRunner::Run below: a child's
        // stream is bytes, not lines, so a single line can straddle any two
        // reads.
        void EmitCompleteLines(
            std::string&     carry,
            const IOutput&   output,
            std::string_view prefix)
        {
            std::size_t newline = std::string::npos;

            while ((newline = carry.find('\n')) != std::string::npos)
            {
                std::string line = carry.substr(0, newline);

                // A CRLF-writing child must not leave a stray '\r' at the end
                // of every logged line.
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                output.Child(prefix, line);
                carry.erase(0, newline + 1);
            }
        }

        std::string DescribeChildSetupStage(
            ChildSetupStage stage)
        {
            switch (stage)
            {
                case ChildSetupStage::Chdir:
                    return "failed to enter the requested working directory";
                case ChildSetupStage::DupStdout:
                    return "failed to redirect the child's stdout onto the output pipe";
                case ChildSetupStage::DupStderr:
                    return "failed to redirect the child's stderr onto the output pipe";
                case ChildSetupStage::Exec:
                    return "failed to exec the requested executable";
            }

            // Unreachable for every enumerator above; a value from neither
            // (a corrupt record off the error pipe) still says something
            // true rather than falling off a value-returning function.
            return "failed during an unrecognized child setup stage";
        }
    }

    ProcessError DescribeChildSetupError(
        const ChildSetupError& error)
    {
        // "the child process ..." names WHO failed: this is never the
        // requested program's own diagnostic -- that program never ran.
        return ProcessError{
            "the child process " + DescribeChildSetupStage(error.stage) +
            " (" + DescribeErrorNumber(error.errorNumber) + ")" };
    }

#if !defined(_WIN32)
    namespace
    {
        // RAII for a POSIX file descriptor -- every descriptor this file
        // creates (both ends of both pipes) is wrapped in one, so a launch-
        // error early return at ANY point closes exactly the descriptors
        // that exist at that point and nothing else. The Win32 branch's
        // UniqueHandle above is the same contract for HANDLEs.
        class FileDescriptor
        {
        public:
            FileDescriptor() = default;
            explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
            ~FileDescriptor() { reset(); }

            FileDescriptor(const FileDescriptor&)            = delete;
            FileDescriptor& operator=(const FileDescriptor&) = delete;

            FileDescriptor(FileDescriptor&& other) noexcept
                : descriptor_(other.release())
            {
            }

            FileDescriptor& operator=(FileDescriptor&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    descriptor_ = other.release();
                }
                return *this;
            }

            [[nodiscard]] int get() const noexcept { return descriptor_; }

            [[nodiscard]] int release() noexcept
            {
                const int released = descriptor_;
                descriptor_ = -1;
                return released;
            }

            void reset(int descriptor = -1) noexcept
            {
                // close() is never retried on EINTR: on Linux the descriptor
                // is already closed when close() reports EINTR, so a retry
                // would close whatever unrelated descriptor has since been
                // handed out that number -- the classic close()-retry bug.
                if (descriptor_ >= 0)
                    ::close(descriptor_);

                descriptor_ = descriptor;
            }

        private:
            int descriptor_ = -1;
        };

        // CHILD SIDE ONLY, between fork() and execvp(). Everything here is
        // async-signal-safe: no allocation, no locale, no iostreams -- only
        // write()/_exit() over a record whose bytes are already laid out on
        // the stack. Never returns.
        //
        // The 127 mirrors the shell's "command not found" convention, but it
        // is NOT how the parent learns what happened: the {stage, errno}
        // record below is, and the parent discards this status entirely when
        // it receives one (see ProcessRunner::Run). The write is EINTR-safe
        // and tolerates a short write; a write that fails outright is
        // deliberately NOT retried forever -- the parent then sees a
        // truncated/absent record and still refuses the launch.
        [[noreturn]] void ReportChildSetupFailure(
            int             errorPipeWrite,
            ChildSetupStage stage,
            int             errorNumber) noexcept
        {
            // Zero-initialized FIRST, then filled: `{}` on an aggregate
            // zero-initializes the whole object including the padding between
            // `stage` and `errorNumber`, so the bytes this writes onto the
            // pipe are fully determinate (a raw aggregate init would leave
            // that padding indeterminate -- harmless in practice, but exactly
            // what a memory sanitizer flags).
            ChildSetupError record{};
            record.stage       = stage;
            record.errorNumber = errorNumber;

            const char* bytes     = reinterpret_cast<const char*>(&record);
            std::size_t remaining = sizeof(record);

            while (remaining > 0)
            {
                const ssize_t written = ::write(errorPipeWrite, bytes, remaining);

                if (written < 0)
                {
                    if (errno == EINTR)
                        continue;

                    break;
                }

                bytes     += written;
                remaining -= static_cast<std::size_t>(written);
            }

            _exit(127);
        }

        // EINTR-safe waitpid for a child whose exit status is not wanted --
        // used only to reap a child that already reported a setup failure,
        // so it never becomes a zombie even though its 127 is discarded.
        void ReapChild(
            pid_t child) noexcept
        {
            int status = 0;

            while (::waitpid(child, &status, 0) < 0)
            {
                if (errno != EINTR)
                    break;
            }
        }
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
            EmitCompleteLines(carry, output_, prefix);
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
        // ---- POSIX: pipe + fork + dup2 + (chdir) + execvp + waitpid -------
        //
        // No shell anywhere: `arguments` becomes an argv array verbatim, so
        // spaces, quotes, `$`, `;` and `|` inside an argument are ordinary
        // bytes to the child -- there is no second parse for them to survive.
        // BuildWindowsCommandLine/QuoteWindowsArgument are not involved on
        // this side at all (nothing re-joins argv into a string).

        // Everything the child will need is built HERE, before fork(): after
        // fork the child may only call async-signal-safe functions, so it can
        // neither allocate nor convert a path.
        std::vector<std::string> argumentStorage;
        argumentStorage.reserve(spec.arguments.size() + 1);
        // argv[0] is the executable itself, the universal convention every
        // tool this driver launches expects to see.
        argumentStorage.push_back(spec.executable.string());

        for (const std::string& argument : spec.arguments)
            argumentStorage.push_back(argument);

        std::vector<char*> argv;
        argv.reserve(argumentStorage.size() + 1);

        for (std::string& argument : argumentStorage)
            argv.push_back(argument.data());

        argv.push_back(nullptr);

        const std::string workingDirectory =
            spec.workingDirectory ? spec.workingDirectory->string() : std::string();

        // ---- the merged stdout+stderr pipe ------------------------------
        int outputPipe[2] = { -1, -1 };

        if (::pipe(outputPipe) != 0)
        {
            return std::unexpected(ProcessError{
                "pipe failed for the child's output (" + DescribeErrorNumber(errno) + ")" });
        }

        FileDescriptor outputRead(outputPipe[0]);
        FileDescriptor outputWrite(outputPipe[1]);

        // ---- the close-on-exec error pipe -------------------------------
        int errorPipe[2] = { -1, -1 };

        if (::pipe(errorPipe) != 0)
        {
            return std::unexpected(ProcessError{
                "pipe failed for the child's setup-error channel (" +
                DescribeErrorNumber(errno) + ")" });
        }

        FileDescriptor errorRead(errorPipe[0]);
        FileDescriptor errorWrite(errorPipe[1]);

        // FD_CLOEXEC on the error pipe's WRITE end, set in the PARENT before
        // fork() -- three reasons, all of them load-bearing:
        //
        //  1. It is what makes EOF on the read end MEAN "execvp succeeded":
        //     a successful execvp closes this descriptor for us, atomically,
        //     as part of replacing the process image. The parent needs no
        //     handshake and no timeout.
        //  2. The flag is a property of the descriptor, so the forked child
        //     inherits it already set -- the post-fork window stays free of
        //     any work that could itself fail (and of any fcntl call that
        //     would have to be async-signal-safe).
        //  3. Before fork, nothing else in this process can leak the
        //     descriptor: were it set only in the child, a concurrent thread
        //     spawning some other process in between would hand that process
        //     a copy of this pipe's write end, and this parent's read would
        //     then never see EOF.
        const int errorWriteFlags = ::fcntl(errorWrite.get(), F_GETFD);

        if (errorWriteFlags < 0 ||
            ::fcntl(errorWrite.get(), F_SETFD, errorWriteFlags | FD_CLOEXEC) < 0)
        {
            return std::unexpected(ProcessError{
                "fcntl(FD_CLOEXEC) failed on the child's setup-error channel (" +
                DescribeErrorNumber(errno) + ")" });
        }

        const pid_t child = ::fork();

        if (child < 0)
        {
            return std::unexpected(ProcessError{
                "fork failed for '" + spec.executable.string() + "' (" +
                DescribeErrorNumber(errno) + ")" });
        }

        if (child == 0)
        {
            // ================= CHILD ==================================
            // Async-signal-safe only from here to execvp: close(), chdir(),
            // dup2(), write(), _exit(). Nothing allocates, and the
            // FileDescriptor destructors never run (every path ends in
            // execvp's image replacement or _exit), so these closes are
            // explicit and cannot double-close.
            //
            // stdin is deliberately left as inherited: a POSIX build tool
            // sharing the invoking terminal is the platform's own convention
            // (make does it), unlike the Windows branch, where a null stdin
            // handle is how the same "we hand the child nothing to read" is
            // spelled.
            ::close(outputRead.get());   // the parent's end of the output pipe
            ::close(errorRead.get());    // the parent's end of the error pipe

            if (!workingDirectory.empty() && ::chdir(workingDirectory.c_str()) != 0)
                ReportChildSetupFailure(errorWrite.get(), ChildSetupStage::Chdir, errno);

            if (::dup2(outputWrite.get(), STDOUT_FILENO) < 0)
                ReportChildSetupFailure(errorWrite.get(), ChildSetupStage::DupStdout, errno);

            // The SAME write end onto stderr: one stream, merged, exactly as
            // the Windows branch hands one pipe handle to both hStdOutput and
            // hStdError -- so interleaving is the child's own ordering.
            if (::dup2(outputWrite.get(), STDERR_FILENO) < 0)
                ReportChildSetupFailure(errorWrite.get(), ChildSetupStage::DupStderr, errno);

            // The original write-end descriptor is redundant now that 1 and 2
            // both name the pipe, and it must go: every copy the child keeps
            // is another writer the parent's read loop would wait on for EOF.
            // The guard covers the pathological case where the pipe landed ON
            // fd 1 or 2 (possible only if this process was started with those
            // already closed) -- closing it then would close the redirection
            // itself.
            if (outputWrite.get() != STDOUT_FILENO && outputWrite.get() != STDERR_FILENO)
                ::close(outputWrite.get());

            // execvp, with argv[0] an absolute path (ProcessSpec::executable
            // always is -- Compose*/BackendResolver resolve the tool first),
            // so its name contains a '/' and NO $PATH search happens: the
            // same "this exact binary, never a PATH lookup" contract
            // lpApplicationName gives the Windows branch.
            ::execvp(argv[0], argv.data());

            // Reached ONLY if execvp failed -- on success it never returns.
            ReportChildSetupFailure(errorWrite.get(), ChildSetupStage::Exec, errno);
        }

        // ================= PARENT =====================================
        //
        // Both child-side ends go now. The output write end MUST: this
        // process holding a copy would keep the pipe writable forever and the
        // read loop below would never see EOF. The error write end likewise:
        // its closure in the parent plus FD_CLOEXEC in the child is exactly
        // what turns "EOF" into "the new image is running".
        outputWrite.reset();
        errorWrite.reset();

        // Read the error pipe FIRST, to completion. This cannot deadlock
        // behind a chatty child: the child writes nothing to the output pipe
        // before exec, and the error pipe reaches EOF at the instant exec
        // succeeds -- no matter how much the child then writes, or whether it
        // blocks on a full output pipe afterwards.
        ChildSetupError record{};
        std::size_t     recordBytes = 0;

        for (;;)
        {
            const ssize_t got =
                ::read(
                    errorRead.get(),
                    reinterpret_cast<char*>(&record) + recordBytes,
                    sizeof(record) - recordBytes);

            if (got < 0)
            {
                if (errno == EINTR)
                    continue;

                // A read failure on this channel leaves us unable to tell
                // "launched" from "failed to launch" by the record -- treat
                // it as no record and fall through to waitpid, which still
                // reports whatever really happened to the child.
                break;
            }

            if (got == 0)
                break;   // EOF: execvp succeeded (nothing was ever written)

            recordBytes += static_cast<std::size_t>(got);

            if (recordBytes == sizeof(record))
                break;
        }

        errorRead.reset();

        if (recordBytes == sizeof(record))
        {
            // The child reported a setup failure and has already _exit(127)'d.
            // Reap it so it is no zombie, then DISCARD that 127: it is not
            // the requested program's exit status -- that program never ran.
            ReapChild(child);

            return std::unexpected(DescribeChildSetupError(record));
        }

        if (recordBytes > 0)
        {
            // A partial record: the child began reporting a setup failure and
            // its write was cut short. Still a launch failure -- refusing
            // with a truthful "truncated" diagnostic beats guessing a stage.
            ReapChild(child);

            return std::unexpected(ProcessError{
                "the child process reported a truncated setup failure (" +
                std::to_string(recordBytes) + " of " + std::to_string(sizeof(record)) +
                " bytes) while starting '" + spec.executable.string() + "'" });
        }

        std::string carry;
        char        buffer[4096];

        for (;;)
        {
            const ssize_t got = ::read(outputRead.get(), buffer, sizeof(buffer));

            if (got < 0)
            {
                if (errno == EINTR)
                    continue;

                // A genuine mid-stream read error. Same policy as the Windows
                // branch: say the log may be incomplete (never a
                // ProcessError -- the child launched and is running/finished,
                // so this must not turn into kExitRefused). Abandoning the
                // read here can leave a still-writing child to take SIGPIPE
                // when the descriptor closes below; waitpid then reports that
                // honestly as 128 + SIGPIPE, after this diagnostic has
                // already said the capture broke.
                output_.Error(
                    "reading " + std::string(prefix) +
                    "'s output failed mid-stream (" + DescribeErrorNumber(errno) +
                    ") -- the captured log above may be incomplete");

                break;
            }

            if (got == 0)
                break;   // EOF: every write end of the output pipe is closed

            carry.append(buffer, static_cast<std::size_t>(got));
            EmitCompleteLines(carry, output_, prefix);
        }

        // A final unterminated line (the child exited without a trailing
        // newline) still reaches the caller, rather than being dropped.
        if (!carry.empty())
            output_.Child(prefix, carry);

        outputRead.reset();

        int status = 0;

        for (;;)
        {
            if (::waitpid(child, &status, 0) >= 0)
                break;

            if (errno == EINTR)
                continue;

            return std::unexpected(ProcessError{
                "waitpid failed for '" + spec.executable.string() + "' (" +
                DescribeErrorNumber(errno) + ")" });
        }

        if (WIFEXITED(status))
            return WEXITSTATUS(status);

        if (WIFSIGNALED(status))
        {
            // The shell's own convention, and the one every CI system reads:
            // a child killed by signal N reports 128 + N, keeping it distinct
            // from the exit codes a child chooses for itself.
            return 128 + WTERMSIG(status);
        }

        // Neither exited nor signalled. With no WUNTRACED/WCONTINUED in the
        // waitpid flags above, a stop/continue status cannot be reported
        // here, so this is unreachable in practice -- but a value-returning
        // function must not fall off its end, and inventing an exit code for
        // a status we cannot interpret would be a lie.
        return std::unexpected(ProcessError{
            "the child process '" + spec.executable.string() +
            "' ended in an unrecognized state (raw wait status " +
            std::to_string(status) + ")" });
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
