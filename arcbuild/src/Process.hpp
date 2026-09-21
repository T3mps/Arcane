#pragma once

#include "Output.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arcbuild
{
    // A single child-process invocation, as DATA -- no shell, no quoting, no
    // string concatenation. `arguments` are exactly the argv entries a real
    // process-launch API (CreateProcess/posix_spawn) would receive one at a
    // time; `workingDirectory`, when set, is the process's cwd. Compose*
    // (Compose.hpp) builds these. Nothing here is ever handed to a shell --
    // RenderProcess (Compose.hpp) produces a readable rendering for logging
    // only, and ProcessRunner::Run/ExecutePlan below are the eventual
    // process-launch call sites (multibackend hardening Task 5).
    struct ProcessSpec
    {
        std::filesystem::path                executable;
        std::vector<std::string>             arguments;
        std::optional<std::filesystem::path> workingDirectory;
    };

    // An ORDERED sequence of process launches that together perform one
    // logical build operation -- e.g. Make/Ninja's rebuild is "clean" then
    // "build" as two separate invocations, never one shell `&&` chain. A
    // caller runs steps in order and stops at the first failure.
    struct ProcessPlan
    {
        std::vector<ProcessSpec> steps;
    };

    // A launch that never produced a child exit code at all -- on Windows:
    // UTF-16 conversion failed, CreateProcessW itself failed (missing
    // executable, access denied, a bad working directory, ...), or a Win32
    // handle/attribute-list step failed; on POSIX: pipe/fork/waitpid failed,
    // or the child reported a chdir/dup2/execvp failure over its error pipe
    // before ever becoming the requested program (see ChildSetupError below).
    // Distinct from a child that ran to completion and exited non-zero, which
    // is an ordinary ProcessResult VALUE, never an error -- see ExecutePlan's
    // mapping of the two.
    struct ProcessError
    {
        std::string message;
    };

    // A completed launch's exit code, or the launch failure that prevented
    // one from ever existing.
    using ProcessResult = std::expected<int, ProcessError>;

    // ---- POSIX child-side setup failures -----------------------------------
    //
    // On POSIX a launch is fork() + execvp(), so the steps that can fail
    // (chdir into the working directory, dup2 the pipe onto stdout/stderr,
    // execvp itself) all run in the CHILD, after fork already succeeded --
    // where there is no way to return a value. The child reports them over a
    // second, close-on-exec pipe as this fixed-size {stage, errno} record and
    // then _exit(127); the parent turns a received record into a
    // ProcessError. The 127 is deliberately DISCARDED: a tool that really ran
    // and exited 127 is an ordinary ProcessResult value, and a tool that
    // never started at all must never be confused with it (ExecutePlan maps
    // the latter to kExitRefused). Compiled on every platform -- these are
    // plain enums/PODs over standard <cerrno> values with no POSIX-only type
    // in sight -- so DescribeChildSetupError is directly unit-testable
    // anywhere, including a Windows desk.
    enum class ChildSetupStage : std::uint8_t
    {
        Chdir,
        DupStdout,
        DupStderr,
        Exec,
    };

    struct ChildSetupError
    {
        ChildSetupStage stage;
        int             errorNumber;
    };

    [[nodiscard]] ProcessError DescribeChildSetupError(
        const ChildSetupError& error);

    // The seam BuildExecutor and ExecutePlan depend on -- never the concrete
    // ProcessRunner directly -- so a fake can record the specs it was asked
    // to run and hand back scripted results without spawning anything real
    // ([build] tests; the process-fixture integration tests below exercise
    // the real ProcessRunner instead).
    class IProcessRunner
    {
    public:
        virtual ~IProcessRunner() = default;

        [[nodiscard]]
        virtual ProcessResult Run(
            const ProcessSpec& spec,
            std::string_view   prefix) const = 0;
    };

    // The real implementation, per platform: CreateProcessW on Windows,
    // pipe/fork/dup2/execvp/waitpid on POSIX. Never a shell, never
    // system()/popen(), and no libc process-spawn fallback of any kind on
    // either side -- see Process.cpp for the quoting + handle-inheritance
    // contract (Windows) and the error-pipe + descriptor contract (POSIX)
    // this type owns.
    class ProcessRunner final : public IProcessRunner
    {
    public:
        explicit ProcessRunner(
            IOutput& output)
            : output_(output)
        {
        }

        [[nodiscard]]
        ProcessResult Run(
            const ProcessSpec& spec,
            std::string_view   prefix) const override;

    private:
        IOutput& output_;
    };

    // Windows command-line quoting -- the real MSVC CRT argv parsing rules
    // (see Process.cpp for the exact algorithm and its citation). Pure
    // string algorithms with no Win32 calls of their own: always compiled
    // and directly unit-tested, even though only ProcessRunner::Run's
    // CreateProcessW call ever actually consumes their output.
    [[nodiscard]] std::string QuoteWindowsArgument(std::string_view argument);
    [[nodiscard]] std::string BuildWindowsCommandLine(const ProcessSpec& spec);

    // Runs an ORDERED ProcessPlan against `runner`, logging RenderProcess
    // (Compose.hpp) to `output` before each launch. Stops at the first step
    // whose result is a launch error (logged, then mapped to kExitRefused --
    // Exit.hpp) or a non-zero child exit code (returned unchanged, never
    // re-mapped). An empty plan, or a plan whose every step exits 0, is
    // kExitOk.
    [[nodiscard]] int ExecutePlan(
        const ProcessPlan& plan,
        IProcessRunner&    runner,
        IOutput&           output,
        std::string_view   prefix);
}
