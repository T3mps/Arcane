#pragma once

// Apply a JSON object onto the registry as one SetBy rung, and write Archive
// cvars back out. input.json is document-shaped (action maps) and is skipped.
// Every other category is cvar-shaped: an unknown key is reported, not applied.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>

#include <Json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    struct CVarApplyReport
    {
        std::vector<std::string> unknownKeys;      // no cvar declares it (a Dev cvar a Dist build compiled out is not reported)
        std::vector<std::string> typeMismatches;   // declared, but the JSON value has the wrong type: refused
    };

    // One config problem, for the Problems panel (settings spec s4.8, s12).
    struct CVarConfigIssue
    {
        enum class Kind : std::uint8_t { UnknownKey, TypeMismatch };
        Kind                  kind = Kind::UnknownKey;
        std::filesystem::path file;
        std::string           key;      // the full cvar name, "<category>.<key>"
        int                   line = 0; // 1-based line of the key's first mention; 0 = not found
    };

    // Read every rung's files WITHOUT applying them, and report each key no cvar
    // declares and each value of the wrong JSON type. Rows are ordered by rung,
    // then file name.
    ARC_CORE_API std::vector<CVarConfigIssue> ValidateCVarLayers(CVarRegistry& registry, const LayerSources& layers);

    // Replace the Problems set "config.cvars" with one row per issue: the File
    // locator is the file at the key's line, and the message names the key.
    // Logs one warning per issue for headless runs.
    ARC_CORE_API void PublishCVarConfigDiagnostics(const std::vector<CVarConfigIssue>& issues);

    // `category` is the file stem. Keys in `doc` become `<category>.<key>`.
    // Nested objects join with further dots. Document-shaped categories record
    // nothing and set nothing.
    ARC_CORE_API CVarApplyReport ApplyCVarCategory(CVarRegistry& registry, std::string_view category,
                                                      const nlohmann::json& doc, SetBy by, bool documentShaped,
                                                      std::string_view sourceModule);

    // Every *.json in dir. "input" is document-shaped; the rest are cvars.
    // `onlyModule` non-empty: apply only that module's cvars, and report no
    // unknown keys (ApplyLayersFor; settings spec s4.4).
    ARC_CORE_API CVarApplyReport ApplyCVarDirectory(CVarRegistry& registry, const std::filesystem::path& dir,
                                                       SetBy by, std::string_view sourceModule,
                                                       std::string_view onlyModule = {});

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

    // The context a host's `--set` runs in (settings plan, integration ruling
    // I3): the Editor context in a Debug/Release build, so a developer's
    // `ArcaneRuntime --set render.meshCull=false` keeps working against a Game
    // setting; the local host's in Dist, where the command line is the player's.
    // The editor passes Editor in every build. A console still uses its session's
    // own context.
    constexpr CVarContext CommandLineCVarContext() noexcept
    {
#if defined(ARC_BUILD_DIST)
        return CVarContext::LocalHost;
#else
        return CVarContext::Editor;
#endif
    }

    // `--set name=value`, repeated. CommandLine rung, in `ctx` (the editor:
    // Editor; ArcaneRuntime: CommandLineCVarContext()). Does not publish.
    ARC_CORE_API void ApplyCVarCommandLine(CVarRegistry& registry, const std::vector<std::string>& sets,
                                              CVarContext ctx);
}
