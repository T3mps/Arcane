#pragma once

// MaterialAsset: the .armat file -- a native JSON asset with an embedded
// top-level "id" (rides AssetRegistry::ScanContent's native path, exactly like
// .json assets). A BASE material stores: kind (which engine template it
// stitches into), the snippet text (the //@param decls live IN the snippet --
// stored once, never duplicated), and saved param VALUES. An INSTANCE stores
// a "parent" Guid + sparse override values only -- no snippet, no kind (both
// come from the base at the end of the parent chain). Param values are
// SELF-TYPED on disk ({"type","value"}) so an instance file loads standalone,
// without its parent's declarations; type-vs-decl mismatches drop at APPLY
// time (MaterialInstance::Set rejects them). Snippet stays inline (one file =
// one atomic asset; spec open-question 2 revisits at diff pain).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on dll-exported types: benign under /MD
#endif
    struct MaterialAssetData
    {
        Guid        id;                       // asset identity (embedded "id")
        Guid        parent;                   // nil = base material; valid = instance of it
        std::string name;                     // display name (defaults to file stem)
        std::string kind = "fullscreen";      // engine template kind (Slice 8: "sprite")
        std::string snippet;                  // //@param decls + shade() body (base only)
        // Saved param values by name -- applied over the //@param defaults (base)
        // or the parent chain (instance); unknown/mismatched names drop on apply.
        std::vector<std::pair<std::string, MatParamValue>> params;

        bool IsInstance() const { return parent.IsValid(); }
    };

    // Write `data` as .armat JSON (values typed per MatParamValue: float /
    // [x,y] / [x,y,z,w] / texture guid string). False on IO failure.
    ARCANE_API bool SaveMaterialAsset(const std::filesystem::path& path,
                                      const MaterialAssetData& data);

    // Parse a .armat. nullopt on IO/parse/shape failure. Param VALUE types are
    // resolved against the snippet's own //@param decls (the decl is the truth;
    // a value that no longer matches its decl is dropped with a warning).
    ARCANE_API std::optional<MaterialAssetData> LoadMaterialAsset(
        const std::filesystem::path& path);

    // Apply saved values onto an instance built over the asset's template
    // (unknown names / type mismatches warn + skip). Returns applied count.
    ARCANE_API std::size_t ApplyMaterialParams(const MaterialAssetData& data,
                                               MaterialInstance& instance);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
