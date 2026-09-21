#pragma once

// Arcane::Toolchain -- where the build tools live on THIS machine, and which
// generated workspace file a project root carries. The ONE home for the
// vswhere probe and the premake / msbuild / devenv lookups: the editor's
// ModuleBuild carried them until the arcbuild driver arrived (spec
// docs/specs/2026-09-13-arcbuild-driver-design.md, s4.1 -- plan ruling R1),
// and both arcbuild.exe (premake + msbuild) and the editor's IdeLaunch
// (devenv) now consume this, so there is exactly one answer to "which Visual
// Studio". Presentation-free, std + Win32 only; ArcaneCore is also compiled
// into the Server workspace, where nothing here is called.
//
// DiscoverSolution, FindOnPath and the concrete Resolve* lookups (Premake,
// MsBuild, Make, Ninja, XcodeBuild) are pure enough to unit-test against a
// temp directory and an explicit PATH/PATHEXT ([build], ToolchainTest.cpp).
// VsWhere / ResolveDevenv spawn vswhere.exe and are desk-verify territory.

#include <filesystem>
#include <string>
#include <string_view>

#include <Arcane/Core/Api.hpp>

namespace Arcane::Toolchain
{
    // The generated workspace file a build drives: the first *.slnx in
    // `projectRoot` (lexicographic, for determinism), else the first *.sln,
    // else empty. Extension compare is ASCII case-insensitive (a hand-
    // generated "Game.SLNX" is still the workspace file). Non-recursive on
    // purpose -- the committed convention puts the premake workspace file in
    // the project root (Aphelyon.slnx beside Aphelyon.arcproj), and a
    // recursive scan would find ThirdParty/vendor solutions that are not
    // ours to build.
    ARCANE_CORE_API std::filesystem::path DiscoverSolution(const std::filesystem::path& projectRoot);

    // A shell-free PATH search: split `searchPath` on the native list
    // separator (';' on Windows, ':' on POSIX) and probe each directory in
    // order. On Windows, `command` is tried verbatim first (the "already has
    // its extension" case), then with each ';'-separated suffix in `pathExt`
    // appended in turn (the PATHEXT contract, e.g. ".COM;.EXE;.BAT;.CMD") --
    // a candidate only has to exist as a regular file, since NTFS carries no
    // executable-bit convention. On POSIX, `pathExt` is ignored and a
    // candidate must be a regular file with at least one executable bit set
    // (a readable-but-not-executable script is skipped, not run). The first
    // match, over directories in the order given, wins; the result is
    // `std::filesystem::absolute(...).lexically_normal()`, or empty when
    // nothing matched. Callers supply `searchPath`/`pathExt` explicitly (the
    // Resolve* functions below read PATH/PATHEXT themselves) so this stays a
    // pure function: temp-directory tests drive it directly without
    // mutating the process environment.
    ARCANE_CORE_API std::filesystem::path FindOnPath(
        std::string_view command,
        std::string_view searchPath,
        std::string_view pathExt = {});

    // The engine's bundled premake: <sdkRoot>/ThirdParty/premake5/premake5[.exe]
    // (the repo layout build/arcane.lua documents), lexically normalised,
    // falling back to a concrete FindOnPath() hit over the process PATH (and,
    // on Windows, PATHEXT) when the bundled copy is not there -- a packaged
    // SDK may ship it elsewhere. Empty when neither the bundled copy nor PATH
    // has one; callers refuse rather than shell out to an optimistic bare
    // name (ledger ruling, Task 2/3).
    ARCANE_CORE_API std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot);

    // The one VS-install-aware query Microsoft documents: run
    // %ProgramFiles(x86)%/Microsoft Visual Studio/Installer/vswhere.exe with
    // `arguments` and return the FIRST line it prints (a path), or empty when
    // vswhere is absent or found nothing. Shared by the two lookups below --
    // one probe, two questions. Windows-only; always empty elsewhere.
    ARCANE_CORE_API std::filesystem::path VsWhere(const std::string& arguments);

    // MSBuild via VsWhere:
    //   vswhere -latest -requires Microsoft.Component.MSBuild
    //           -find MSBuild\**\Bin\MSBuild.exe
    // falling back to a concrete "msbuild"/"MSBuild.exe" FindOnPath() hit over
    // PATH (a Developer Command Prompt launch). Empty when neither answers.
    ARCANE_CORE_API std::filesystem::path ResolveMsBuild();

    // Make: Windows prefers "mingw32-make" (MSYS2/MinGW's name) over "make"
    // on PATH; POSIX has only "make". Empty when PATH has neither.
    ARCANE_CORE_API std::filesystem::path ResolveMake();

    // Ninja: "ninja" on PATH (PATHEXT-aware on Windows). Empty when absent.
    ARCANE_CORE_API std::filesystem::path ResolveNinja();

    // xcodebuild: the fixed macOS location (/usr/bin/xcodebuild), else PATH.
    // Always empty on a non-macOS host -- there is no PATH worth searching.
    ARCANE_CORE_API std::filesystem::path ResolveXcodeBuild();

    // devenv.exe via VsWhere: vswhere -latest -find Common7\IDE\devenv.exe.
    // Empty when no Visual Studio install is found (the editor greys its
    // Open Visual Studio item on that).
    ARCANE_CORE_API std::filesystem::path ResolveDevenv();
}
