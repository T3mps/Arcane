#pragma once

#include "Action.hpp"
#include "Build.hpp"
#include "ProjectLayout.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace arcbuild
{
    [[nodiscard]] std::string ComposeGenerate(const ProjectLayout& project, const std::filesystem::path& premake, std::string_view action);
    [[nodiscard]] std::string ComposeBuild(BuildBackend backend, const std::filesystem::path& builder, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] std::string ComposeMsBuild(const std::filesystem::path& msbuild, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] std::string ComposeMake(const std::filesystem::path& make, const BackendContext& context, std::string_view config, BuildOperation operation);
    [[nodiscard]] std::string ComposeNinja(const std::filesystem::path& ninja, const BackendContext& context, BuildOperation operation);
    [[nodiscard]] std::string ComposeXcodeBuild(const std::filesystem::path& xcodebuild, const BackendContext& context, std::string_view config, BuildOperation operation);
}
