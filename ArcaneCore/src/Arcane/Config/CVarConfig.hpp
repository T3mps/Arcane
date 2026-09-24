#pragma once

// Apply a JSON object onto the registry as one SetBy rung, and write Archive
// cvars back out. input.json is document-shaped (action maps) and is skipped.
// Every other category is cvar-shaped: an unknown key is reported, not applied.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>

#include <Json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    struct CVarApplyReport
    {
        std::vector<std::string> unknownKeys;
    };

    // `category` is the file stem. Keys in `doc` become `<category>.<key>`.
    // Nested objects join with further dots. Document-shaped categories record
    // nothing and set nothing.
    ARCANE_CORE_API CVarApplyReport ApplyCVarCategory(CVarRegistry& registry, std::string_view category,
                                                      const nlohmann::json& doc, SetBy by, bool documentShaped,
                                                      std::string_view sourceModule);

    // Every *.json in dir. "input" is document-shaped; the rest are cvars.
    ARCANE_CORE_API CVarApplyReport ApplyCVarDirectory(CVarRegistry& registry, const std::filesystem::path& dir,
                                                       SetBy by, std::string_view sourceModule);

    // One <category>.json per category that has something to archive. A cvar
    // is written only when it is Archive and its winning SetBy is User or stronger.
    ARCANE_CORE_API void WriteCVarArchive(const CVarRegistry& registry, const std::filesystem::path& userDir);

    // `--set name=value`, repeated. CommandLine rung. Does not publish.
    ARCANE_CORE_API void ApplyCVarCommandLine(CVarRegistry& registry, const std::vector<std::string>& sets,
                                              Permission permission);
}
