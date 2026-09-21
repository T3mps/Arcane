#pragma once

#include "Action.hpp"
#include "ProjectLayout.hpp"
#include "Request.hpp"

#include <filesystem>
#include <optional>

namespace arcbuild
{
    struct DriverContext
    {
        Request               request;
        BuildBackend          backend = BuildBackend::None;
        std::optional<std::filesystem::path> sdkRoot;
        ProjectLayout         project;
    };
}
