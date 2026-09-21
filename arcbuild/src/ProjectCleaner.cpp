#include "ProjectCleaner.hpp"

#include "Exit.hpp"

#include <filesystem>
#include <string>

namespace arcbuild
{
    int ProjectCleaner::Clean(
        const ProjectLayout& project,
        std::string_view config) const
    {
        int exit =
            kExitOk;

        for (const auto& directory :
             CleanTargets(
                 project,
                 config))
        {
            std::error_code ec;

            const auto removed =
                std::filesystem::remove_all(
                    directory,
                    ec);

            if (ec)
            {
                output_.Error(
                    "failed to remove " +
                    directory.generic_string() +
                    ": " +
                    ec.message());

                exit =
                    kExitRefused;

                continue;
            }

            output_.Info(
                "removed " +
                directory.generic_string() +
                " (" +
                std::to_string(removed) +
                " entries)");
        }

        return exit;
    }
}
