#pragma once

// Arcane::Paths -- the ONE place that names every well-known location
// (settings spec s11.0). Settings, logs, caches, crash capture and layouts
// resolve through it, and no subsystem joins "Saved"/"Intermediate"/
// LOCALAPPDATA by hand (PathsGuardTest). Packaging (mounts, paks) is a later
// spec built on this; the asset system's mounts are unchanged.
//
// Hosts call Configure at boot and on project open/close. Runtime does both:
// the engine dir at its first construction, the project on OpenProject and
// CloseProject. `dist` stays false until the Dist user directory lands (S7).

#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Arcane::Paths
{
    enum class Location : std::uint8_t
    {
        EngineDir, EngineData, EngineConfig,                 // read-only
        ProjectDir, ProjectConfig, ProjectContent,           // packaged; Config writable in the editor
        ProjectSaved, ProjectIntermediate, ProjectCache,     // dev only (empty in Dist)
        EditorUserDir,                                       // %LOCALAPPDATA%\Arcane\Editor (empty in Dist)
        GameUserDir,                                         // dev: <project>/Saved; Dist: %LOCALAPPDATA%\<Company>\<Game>
        DiagnosticsDir,                                      // <project>/Saved/Diagnostics; no project: <engine>/diagnostics
        TempDir,                                             // <OS temp>/Arcane/<pid>
    };

    struct Config
    {
        std::filesystem::path                engineDir;      // where data/ sits (the exe dir in the dev bin layout)
        std::optional<std::filesystem::path> projectDir;
        std::string                          companyName, gameName;
        bool                                 dist = false;
    };

    ARC_CORE_API void Configure(const Config& config);
    [[nodiscard]] ARC_CORE_API Config Current();
    // Current() with `projectDir` = projectRoot: for code that is handed a
    // project root (Project::Open, an outgoing project) rather than the open one.
    [[nodiscard]] ARC_CORE_API Config ForProject(const std::filesystem::path& projectRoot);
    // The pure resolver. Empty when the location does not exist for `config`.
    [[nodiscard]] ARC_CORE_API std::filesystem::path Resolve(Location location, const Config& config);
    // Resolve(location, config) / rel, or EMPTY when the location is empty --
    // never a relative path that would land in the working directory.
    [[nodiscard]] ARC_CORE_API std::filesystem::path Join(Location location, const Config& config,
                                                             const std::filesystem::path& rel);
    // Resolve(location, Current()). Creates nothing.
    [[nodiscard]] ARC_CORE_API std::filesystem::path Get(Location location);
    // Get, creating the directory when the location is writable (spec s11.0).
    ARC_CORE_API std::filesystem::path EnsureDir(Location location);
    // %LOCALAPPDATA%\Arcane (Windows), $XDG_DATA_HOME/Arcane or ~/.local/share/Arcane;
    // empty when the base is unset. The Hub's files live under it too.
    [[nodiscard]] ARC_CORE_API std::filesystem::path UserRoot();
}
