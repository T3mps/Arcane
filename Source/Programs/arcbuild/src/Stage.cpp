#include "Stage.hpp"

#include <Json.hpp>

#include <fstream>
#include <system_error>
#include <vector>

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

    std::expected<StagedPackage, std::string> StagePackage(
        const std::filesystem::path& manifest,
        std::string_view config,
        const std::filesystem::path& outDir)
    {
        std::ifstream in(manifest, std::ios::binary);
        if (!in)
            return std::unexpected("no package stage list at " + manifest.generic_string() +
                                   " -- the monolithic generate writes it (build/arcane.lua)");

        const nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
        if (doc.is_discarded() || !doc.is_object() || !doc.contains("configs") || !doc["configs"].is_object())
            return std::unexpected("malformed package stage list " + manifest.generic_string());

        const auto configs = doc["configs"];
        const auto it = configs.find(std::string(config));
        if (it == configs.end() || !it->is_array())
            return std::unexpected("the package stage list has no '" + std::string(config) +
                                   "' configuration (" + manifest.generic_string() + ")");

        // Validate EVERY entry before touching outDir: a refusal leaves the
        // previous package intact rather than half-replaced.
        struct Planned
        {
            std::filesystem::path from, to;
            bool dir = false;
        };
        std::error_code ec;
        const std::filesystem::path root = outDir.lexically_normal();
        std::vector<Planned> plan;
        StagedPackage staged;
        for (const nlohmann::json& entry : *it)
        {
            if (!entry.is_object() || !entry.contains("from") || !entry.contains("to") ||
                !entry["from"].is_string() || !entry["to"].is_string() ||
                (entry.contains("optional") && !entry["optional"].is_boolean()) ||
                (entry.contains("kind") && !entry["kind"].is_string()))
                return std::unexpected("malformed package stage entry in " + manifest.generic_string());

            const std::filesystem::path from = entry["from"].get<std::string>();
            const std::string           rel  = entry["to"].get<std::string>();
            const bool optional = entry.contains("optional") && entry["optional"].get<bool>();
            const bool dir      = entry.contains("kind") && entry["kind"].get<std::string>() == "dir";

            const std::filesystem::path to = (root / rel).lexically_normal();
            const std::filesystem::path back = to.lexically_relative(root);
            if (rel.empty() || std::filesystem::path(rel).is_absolute() || back.empty() ||
                *back.begin() == "..")
                return std::unexpected("package stage entry '" + rel + "' leaves the package directory");

            const bool present = dir ? std::filesystem::is_directory(from, ec)
                                     : std::filesystem::is_regular_file(from, ec);
            if (!present)
            {
                if (optional) { ++staged.skipped; continue; }
                return std::unexpected("package stage: missing " + std::string(dir ? "directory " : "file ") +
                                       from.generic_string());
            }
            plan.push_back({ from, to, dir });
        }

        std::filesystem::remove_all(outDir, ec);
        if (ec)
            return std::unexpected("failed to clear " + outDir.generic_string() + ": " + ec.message());
        std::filesystem::create_directories(outDir, ec);
        if (ec)
            return std::unexpected("failed to create " + outDir.generic_string() + ": " + ec.message());

        for (const Planned& p : plan)
        {
            const std::filesystem::path& from = p.from;
            const std::filesystem::path& to = p.to;
            const bool dir = p.dir;
            std::filesystem::create_directories(dir ? to : to.parent_path(), ec);
            if (ec)
                return std::unexpected("failed to create " + to.generic_string() + ": " + ec.message());

            if (dir)
            {
                std::filesystem::copy(from, to,
                    std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                    return std::unexpected("failed to stage " + from.generic_string() + " -> " +
                                           to.generic_string() + ": " + ec.message());
            }
            else if (const auto copied = CopyOver(from, to); !copied)
            {
                return std::unexpected(copied.error());
            }
            ++staged.copied;
        }
        return staged;
    }
}
