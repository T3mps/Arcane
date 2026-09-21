#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace arcbuild
{
    enum class BuildOperation : std::uint8_t
    {
        Build,
        Rebuild,
        Clean
    };

    // What BackendResolver::ResolveBackendContext hands Compose for one
    // backend: the generated artifact the build tool is pointed at (the
    // .slnx for MSBuild, the project root for Make/Ninja, the .xcodeproj
    // for xcodebuild) and, for the two backends whose tool needs to be told
    // WHICH target to build, the module stem that names it -- Ninja's
    // per-configuration aggregate is <target>_<Config>, xcodebuild's is
    // `-target <target>` (beta8 emits no shared scheme). MSBuild and Make
    // build the whole generated workspace and leave it unset; Compose
    // refuses (an empty ProcessPlan) a Ninja/Xcode context that lacks it
    // rather than composing a malformed "_Debug" / empty -target.
    struct BackendContext
    {
        std::filesystem::path       path;
        std::optional<std::string> target;
    };
}
