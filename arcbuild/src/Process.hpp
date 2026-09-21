#pragma once

#include "Output.hpp"

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
    // only, and the eventual process-launch call sites (multibackend
    // hardening Task 5) consume the fields directly.
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

    class ProcessRunner
    {
    public:
        explicit ProcessRunner(
            IOutput& output)
            : output_(output)
        {
        }

        [[nodiscard]]
        std::optional<int> RunStreaming(
            const std::string& commandLine,
            std::string_view prefix) const;

    private:
        IOutput& output_;
    };
}
