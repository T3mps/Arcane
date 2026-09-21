#pragma once

#include "Action.hpp"
#include "Build.hpp"
#include "ProjectLayout.hpp"

#include <filesystem>
#include <optional>

namespace arcbuild
{
    class BackendResolver
    {
    public:
        [[nodiscard]] std::optional<std::filesystem::path> ResolvePremake(const std::filesystem::path& sdkRoot) const;
        [[nodiscard]] std::optional<std::filesystem::path> ResolveBuilder(BuildBackend backend) const;
        [[nodiscard]] std::optional<BackendContext> ResolveBackendContext(BuildBackend backend, const ProjectLayout& project) const;
    };
}
