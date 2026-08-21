#pragma once

// Render module: WHICH DIRECTORY the compiled shader artifacts come from.
//
// One function, no device, no backend objects -- just the flavor-directory
// resolution every shader loader shares, in a header that pulls in nothing
// else.
//
// THE ONE THING THE LOADERS MUST NOT DISAGREE ABOUT is the directory. A desk
// user pointing ARCANE_SHADER_DIR at a live recompile would otherwise feed
// two paths different shaders while comparing their pixels. Every loader
// resolves through here, so they cannot drift.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // GraphicsBackend

#include <filesystem>

namespace Arcane::ShaderPaths
{
    // The directory "<name>.bin" artifacts load from: ARCANE_SHADER_DIR if
    // set, else `shaderDir` (resolved against the EXECUTABLE when relative --
    // never the CWD, because the build copies artifacts next to consumer exes
    // and tests must pass regardless of where they are launched from), then
    // the backend's flavor subdirectory (dxil/spirv).
    //
    // Returns an EMPTY path when that directory does not exist, having
    // already logged why -- callers treat empty as "logged, give up".
    [[nodiscard]] ARCANE_API std::filesystem::path ResolveFlavorDir(
        GraphicsBackend backend, const std::filesystem::path& shaderDir);
}
