#pragma once

// ProjectManifest: the parsed form of a .arcproj file -- the project's identity and
// engine/module/plugin declarations. Required fields: formatVersion (>0), name,
// engine.abi. Everything else is optional (a content-only project omits gameModule).

#include <Arcane/Base/Api.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <Json.hpp>   // nlohmann::json (the vendored single header)

namespace Arcane
{
    struct ProjectManifest
    {
        struct PluginRef
        {
            std::string name;
            bool        enabled = true;
        };

        int                    formatVersion = 0;
        std::string            name;
        std::string            description;
        int                    engineAbi = 0;      // "engine": { "abi": N }
        std::string            gameModule;         // may be empty (content-only)
        std::vector<PluginRef> plugins;
        std::string            bootScene;           // asset Guid text (see Project::SetBootScene); empty = none

        // The project's durable identity, as canonical Guid text; empty when the
        // manifest predates the field (Project::Open self-heals by stamping one).
        // The Arcane Hub keys its per-project metadata on this so a MOVED project
        // heals in place instead of becoming a stranger -- but only when the old
        // entry's path is gone, so a hand-copied folder (which copies the guid)
        // stays a separate project. The Hub's Duplicate regenerates it outright.
        std::string            guid;

        // Parse + validate a JSON document. nullopt on schema violation.
        static ARCANE_API std::optional<ProjectManifest> FromJson(const nlohmann::json& doc);

        // Read + parse + validate a .arcproj file. nullopt on IO/parse/schema failure.
        static ARCANE_API std::optional<ProjectManifest> LoadFile(const std::filesystem::path& file);
    };
}
