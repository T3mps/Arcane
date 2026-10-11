#pragma once

#include <filesystem>
#include <optional>

namespace arcbuild
{
    class IEnvironment
    {
    public:
        virtual ~IEnvironment() = default;

        [[nodiscard]]
        virtual std::optional<std::filesystem::path> ArcaneSdk() const = 0;

        [[nodiscard]]
        virtual bool SetArcaneSdk(
            const std::filesystem::path& root) const = 0;
    };

    class Environment final : public IEnvironment
    {
    public:
        [[nodiscard]]
        std::optional<std::filesystem::path> ArcaneSdk() const override;

        [[nodiscard]]
        bool SetArcaneSdk(
            const std::filesystem::path& root) const override;
    };
}
