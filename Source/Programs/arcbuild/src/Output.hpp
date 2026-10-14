#pragma once

#include <string_view>

namespace arcbuild
{
    class IOutput
    {
    public:
        virtual ~IOutput() = default;

        virtual void SetQuiet(bool quiet) noexcept = 0;

        virtual void Info(
            std::string_view message) const = 0;

        virtual void Always(
            std::string_view message) const = 0;

        virtual void Error(
            std::string_view message) const = 0;

        virtual void Child(
            std::string_view prefix,
            std::string_view message) const = 0;
    };

    class Output final : public IOutput
    {
    public:
        void SetQuiet(bool quiet) noexcept override;

        void Info(
            std::string_view message) const override;

        void Always(
            std::string_view message) const override;

        void Error(
            std::string_view message) const override;

        void Child(
            std::string_view prefix,
            std::string_view message) const override;

    private:
        bool quiet_ = false;
    };
}
