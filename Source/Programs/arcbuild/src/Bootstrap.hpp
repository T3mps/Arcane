#pragma once

#include "DriverContext.hpp"
#include "Environment.hpp"

#include <expected>
#include <string>

namespace arcbuild
{
    using BootstrapResult =
        std::expected<DriverContext, std::string>;

    class Bootstrap
    {
    public:
        explicit Bootstrap(
            IEnvironment& environment)
            : environment_(environment)
        {
        }

        [[nodiscard]]
        BootstrapResult Prepare(
            Request request) const;

    private:
        IEnvironment& environment_;
    };
}
