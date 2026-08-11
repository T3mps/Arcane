#pragma once

// ShaderSourceProvider: logical shader/template name -> HLSL source TEXT. The
// seam between "who stores shader source" and "who consumes it" (the material
// template stitcher + the runtime ShaderCompiler, Slice 4). Today it reads
// loose files under registered root directories (first hit wins; relative
// roots anchor exe-relative, the ShaderLibrary convention). Later the same
// signature resolves Guid-addressed sources through the AssetRegistry -- the
// consumers never learn where source text lives. Deliberately uncached: source
// files are tiny, a fresh read keeps the hot-reload loop honest, and the
// compile service already dedupes by content hash.

#include <Arcane/Base/Api.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API ShaderSourceProvider
    {
    public:
        // Register a directory sources resolve under; search order = add order.
        // Relative roots anchor to the executable directory at lookup time.
        void AddRoot(const std::filesystem::path& root);

        std::size_t RootCount() const { return m_roots.size(); }

        // Resolve a logical name ("materials/fullscreen_material.hlsl") to its
        // source text via the first root that has the file. Rejects absolute
        // names and any ".." component -- logical names address INTO the roots,
        // never out of them. nullopt when no root has it (caller logs context).
        std::optional<std::string> Get(std::string_view logicalName) const;

    private:
        std::vector<std::filesystem::path> m_roots;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
