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
    ARC_CORE_API CVarApplyReport ApplyCVarCategory(CVarRegistry& registry, std::string_view category,
                                                      const nlohmann::json& doc, SetBy by, bool documentShaped,
                                                      std::string_view sourceModule);

    // Every *.json in dir. "input" is document-shaped; the rest are cvars.
    ARC_CORE_API CVarApplyReport ApplyCVarDirectory(CVarRegistry& registry, const std::filesystem::path& dir,
                                                       SetBy by, std::string_view sourceModule);

    // The user layer's write half (T3-D2): one <category>.json per category
    // with something to archive, in the shape ApplyCVarDirectory(..., SetBy::User)
    // reads back. A cvar is written only when it is Archive, neither Dev nor
    // Cheat, and the User rung holds a value for it -- that value (its newest
    // User record), even when a stronger rung (command line, console, code)
    // currently wins. Defaults and every other rung are never written; a
    // document-shaped category (input) is never touched.
    // An existing file is MERGED: keys this write does not own (another
    // module's cvars, hand-written settings) stay, and a key already present
    // -- flat ("graph.x") or nested ({"graph":{"x":..}}) -- is updated in
    // place. A renamed cvar's old key (RegisterAlias, settings spec s4.7) is
    // dropped wherever its new name is written, so the next save migrates the
    // file. An unreadable file is kept beside it as <category>.json.bad and
    // replaced. Each file goes to <category>.json.tmp first and is renamed
    // over the old one, so a crash mid-write never leaves a torn file; an
    // unchanged file is not rewritten.
    ARC_CORE_API void WriteCVarArchive(const CVarRegistry& registry, const std::filesystem::path& userDir);

    // `--set name=value`, repeated. CommandLine rung. Does not publish.
    ARC_CORE_API void ApplyCVarCommandLine(CVarRegistry& registry, const std::vector<std::string>& sets,
                                              Permission permission);
}
