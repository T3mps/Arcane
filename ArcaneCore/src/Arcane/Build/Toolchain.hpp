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
// DiscoverSolution and ResolvePremake are pure enough to unit-test against a
// temp directory ([build], ToolchainTest.cpp). VsWhere / ResolveMsBuild /
// ResolveDevenv spawn vswhere.exe and are desk-verify territory.

#include <filesystem>
#include <string>

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

    // The engine's bundled premake: <sdkRoot>/ThirdParty/premake5/premake5.exe
    // (the repo layout build/arcane.lua documents), lexically normalised,
    // falling back to bare "premake5" (PATH) when the bundled copy is not
    // there -- a packaged SDK may ship it elsewhere, and cmd's own resolution
    // is the honest fallback.
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
    // falling back to bare "msbuild" (PATH -- a Developer Command Prompt).
    ARCANE_CORE_API std::filesystem::path ResolveMsBuild();

    // devenv.exe via VsWhere: vswhere -latest -find Common7\IDE\devenv.exe.
    // Empty when no Visual Studio install is found (the editor greys its
    // Open Visual Studio item on that).
    ARCANE_CORE_API std::filesystem::path ResolveDevenv();
}
