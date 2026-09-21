#pragma once

#include "Action.hpp"
#include "Build.hpp"
#include "Process.hpp"
#include "ProjectLayout.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace arcbuild
{
    // Compose* build a STRUCTURED description of the child process(es) a
    // generate/build/rebuild/clean action needs -- executable + argv +
    // (optionally) a working directory, as ProcessSpec/ProcessPlan
    // (Process.hpp) -- never a shell string. Nothing here quotes, escapes,
    // or concatenates for a shell.
    [[nodiscard]] ProcessSpec ComposeGenerate(const ProjectLayout& project, const std::filesystem::path& premake, std::string_view action);
    [[nodiscard]] ProcessPlan ComposeBuild(BuildBackend backend, const std::filesystem::path& builder, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] ProcessPlan ComposeMsBuild(const std::filesystem::path& msbuild, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] ProcessPlan ComposeMake(const std::filesystem::path& make, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] ProcessPlan ComposeNinja(const std::filesystem::path& ninja, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] ProcessPlan ComposeXcodeBuild(const std::filesystem::path& xcodebuild, const BackendContext& context, std::string_view config, BuildOperation operation);

    // RenderProcess: a HUMAN-READABLE rendering of a ProcessSpec for logging
    // ONLY -- naive whitespace-triggered quoting, no shell escaping of any
    // kind. The result is NEVER re-parsed as an executable command line and
    // is never handed to a shell or a process-launch API; it exists purely
    // so arcbuild can print, for a human, what it is about to run.
    [[nodiscard]] std::string RenderProcess(const ProcessSpec& process);
}
