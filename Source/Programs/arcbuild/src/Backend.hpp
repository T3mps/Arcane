#pragma once

#include "Action.hpp"
#include "Build.hpp"
#include "ProjectLayout.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace arcbuild
{
    // Wraps Arcane::Toolchain's concrete tool discovery (real FindOnPath /
    // PATH+PATHEXT lookups, no optimistic bare names -- see Toolchain.hpp)
    // with the project-side "is there a generated build context to drive"
    // check, and turns an empty/missing result into a descriptive
    // std::unexpected the caller can surface verbatim.
    class BackendResolver
    {
    public:
        [[nodiscard]] std::expected<std::filesystem::path, std::string>
            ResolvePremake(const std::filesystem::path& sdkRoot) const;

        [[nodiscard]] std::expected<std::filesystem::path, std::string>
            ResolveBuilder(BuildBackend backend) const;

        [[nodiscard]] std::expected<BackendContext, std::string>
            ResolveBackendContext(BuildBackend backend, const ProjectLayout& project) const;
    };
}
