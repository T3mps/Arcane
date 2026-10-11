#include "Output.hpp"

#include <cstdio>

namespace arcbuild
{
    namespace
    {
        void PrintView(
            std::string_view value)
        {
            std::printf(
                "%.*s",
                static_cast<int>(value.size()),
                value.data());
        }
    }

    void Output::SetQuiet(
        bool quiet) noexcept
    {
        quiet_ = quiet;
    }

    void Output::Info(
        std::string_view message) const
    {
        if (quiet_)
            return;

        std::printf("[arcbuild] ");
        PrintView(message);
        std::printf("\n");
        std::fflush(stdout);
    }

    void Output::Always(
        std::string_view message) const
    {
        std::printf("[arcbuild] ");
        PrintView(message);
        std::printf("\n");
        std::fflush(stdout);
    }

    void Output::Error(
        std::string_view message) const
    {
        std::printf("[arcbuild] error: ");
        PrintView(message);
        std::printf("\n");
        std::fflush(stdout);
    }

    void Output::Child(
        std::string_view prefix,
        std::string_view message) const
    {
        PrintView(prefix);
        std::printf(" ");
        PrintView(message);
        std::printf("\n");
        std::fflush(stdout);
    }
}