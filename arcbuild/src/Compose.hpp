#pragma once

// Child command lines and the path conventions they sit on. ComposeGenerate /
// ComposeMsBuild are the SHARED spawn-line layer -- `--engine` reuses them
// (premake then msbuild, parenthesised, UTF-8 quoted). Layout / SlotPath /
// SolutionPath / CleanTargets are the GAME-PROJECT conventions (manifest
// `name` + `gameModule`, single-slot Binaries/). Engine layout (Arcane.slnx,
// ReferenceProject-first, staging) is a sibling unit, not a field on Layout.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace arcbuild
{
    struct Layout
    {
        std::filesystem::path root;        // ABSOLUTE project directory (main.cpp absolutises; ruling R3)
        std::filesystem::path manifest;    // the .arcproj
        std::string           name;        // manifest `name` -> the <name>.slnx convention
        std::string           gameModule;  // manifest `gameModule`; empty = content-only project
    };

    // <root>/Binaries/<gameModule> -- the single slot HostBoot loads from
    // (Arcane/Host/ProjectBoot.hpp). Empty when the project has no module.
    [[nodiscard]] std::filesystem::path SlotPath(const Layout& l);

    // The workspace file msbuild drives: `discovered` (Toolchain::
    // DiscoverSolution's answer) when non-empty, else <root>/<name>.slnx --
    // the committed convention (a project's premake workspace is named after
    // the project), exactly as EditorApp::StartModuleRebuild assumed.
    // DiscoverSolution returns absolute iff `projectRoot` is (directory_iterator
    // paths); ComposeMsBuild does not cd, so a relative `discovered` is joined
    // onto `l.root` here rather than assumed absolute.
    [[nodiscard]] std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered);

    // Every line runs through cmd.exe /c (_wpopen). Parenthesised so the
    // trailing 2>&1 folds the member's stderr into the captured stdout. Paths
    // are wrapped in plain quotes as UTF-8 (path::u8string) so Widen() at the
    // _wpopen boundary is honest on non-ASCII install paths. An embedded
    // quote is not defended against. `--action` is NOT quoted -- it is an
    // identifier (IsValidAction), not a path.

    struct Tools
    {
        std::filesystem::path premake;
        std::filesystem::path msbuild;
    };

    // ( cd /d "<root>" && "<premake>" <action> ) 2>&1 -- premake reads
    // ./premake5.lua from the cwd, hence the cd.
    [[nodiscard]] std::string ComposeGenerate(const Layout& l, const Tools& t, std::string_view action);

    enum class MsBuildTarget : std::uint8_t { Build, Rebuild, Clean };

    // ( "<msbuild>" "<solution>" /p:Configuration=<cfg> [/t:Rebuild|/t:Clean] /m /nologo ) 2>&1
    // No cd (the solution path is absolute); /t: only when the target is not
    // the default Build.
    [[nodiscard]] std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution,
                                             std::string_view config, MsBuildTarget target);

    // What `clean` removes after msbuild /t:Clean: <root>/Binaries (the whole
    // slot -- it is one slot) and <root>/Intermediate/<config>. Never Source/,
    // Content/, Saved/, the .slnx (generate rewrites it), nor
    // Intermediate/Artifacts (arccook's).
    [[nodiscard]] std::vector<std::filesystem::path> CleanTargets(const Layout& l, std::string_view config);
}
