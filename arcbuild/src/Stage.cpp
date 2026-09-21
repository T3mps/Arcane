#include "Stage.hpp"

#include <system_error>

namespace arcbuild
{
    namespace
    {
        std::expected<void, std::string> CopyOver(
            const std::filesystem::path& from,
            const std::filesystem::path& to)
        {
            std::error_code ec;

            std::filesystem::copy_file(
                from,
                to,
                std::filesystem::copy_options::overwrite_existing,
                ec);

            if (ec)
            {
                return std::unexpected(
                    "failed to stage " + from.generic_string() +
                    " -> " + to.generic_string() + ": " + ec.message());
            }

            return {};
        }
    }

    std::expected<StagedModule, std::string> StageBuiltModule(
        const std::filesystem::path& built,
        const std::filesystem::path& slot)
    {
        std::error_code ec;

        if (built.empty() || slot.empty())
        {
            return std::unexpected(
                "cannot stage a module without both a built path and a slot path");
        }

        if (!std::filesystem::is_regular_file(built, ec) || ec)
        {
            return std::unexpected(
                "the backend reported success but no module exists at " +
                built.generic_string());
        }

        // create_directories is a no-op success when the directory exists;
        // an error here is a real one (a FILE named Binaries, permissions).
        std::filesystem::create_directories(slot.parent_path(), ec);

        if (ec)
        {
            return std::unexpected(
                "failed to create " + slot.parent_path().generic_string() +
                ": " + ec.message());
        }

        StagedModule staged;

        if (const auto copied = CopyOver(built, slot); !copied)
            return std::unexpected(copied.error());

        staged.copied.push_back(slot);

        // Symbols: <stem>.pdb beside the built module, when the toolset
        // produced one (MSVC does; a GCC/Clang ninja toolset would not).
        // Best effort only in the sense that ABSENCE is fine; a PDB that
        // exists but cannot be copied is a failure like any other.
        std::filesystem::path builtPdb = built;
        builtPdb.replace_extension(".pdb");

        if (std::filesystem::is_regular_file(builtPdb, ec) && !ec)
        {
            std::filesystem::path slotPdb = slot;
            slotPdb.replace_extension(".pdb");

            if (const auto copied = CopyOver(builtPdb, slotPdb); !copied)
                return std::unexpected(copied.error());

            staged.copied.push_back(slotPdb);
        }

        return staged;
    }
}
