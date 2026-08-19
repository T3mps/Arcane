#pragma once

// Render module: loads compiled shader artifacts (DXIL or SPIR-V chosen by
// the device's backend) by name from a directory of loose .bin files.
// Poll() re-stats the files and reloads changed ones (hot reload);
// Generation() bumps on every reload so pipeline caches can lazily rebuild.
// The ARCANE_SHADER_DIR environment variable overrides the directory --
// point it at Arcane/data/shaders/generated for the recompile-while-running loop.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>

#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace Arcane
{
    class ARCANE_API ShaderLibrary
    {
    public:
        // Relative shaderDir resolves against the executable's directory
        // (artifacts are postbuild-copied next to consumer exes); ARCANE_SHADER_DIR
        // overrides; absolute paths pass through.
        // Returns null (with ARC_ERROR) when the directory is missing.
        static std::unique_ptr<ShaderLibrary> Create(
            nvrhi::IDevice* device, GraphicsBackend backend,
            const std::filesystem::path& shaderDir);

        // The flavor-directory resolution Create() is implemented on top of
        // MOVED OUT to Arcane::ShaderPaths::ResolveFlavorDir
        // (Render/ShaderPaths.hpp) at NRI Phase 5a, Task 7. It was the ONLY
        // member of this class the NRI graph path ever called -- it loads the
        // same .bin artifacts as raw bytecode, because NRI's ShaderDesc takes
        // a blob rather than an nvrhi::ShaderHandle -- so hosting a pure path
        // helper here forced every graph translation unit that wanted it to
        // include an NVRHI header. Nothing about the resolution changed; the
        // rationale for a single shared resolver lives with it there.

        virtual ~ShaderLibrary() = default;

        // Loads (and caches) "<dir>/<dxil|spirv>/<name>.bin". Returns null
        // with ARC_ERROR when the artifact is missing or unreadable.
        virtual nvrhi::ShaderHandle Get(std::string_view name,
                                        nvrhi::ShaderType type) = 0;

        // Re-stats every loaded artifact; reloads the changed ones.
        // Returns true when anything reloaded (Generation() bumped).
        virtual bool Poll() = 0;

        // Monotonic; starts at 1. Pipeline caches compare and rebuild.
        virtual uint64_t Generation() const = 0;
    };
}
