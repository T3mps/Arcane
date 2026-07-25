#pragma once

// MaterialAsset: the .arcmat file -- a native JSON asset with an embedded
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
#include <Arcane/Material/MaterialGraph.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <Json.hpp>

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
    // One EXTRA pass of a fullscreen pass chain (the main `snippet` is always
    // pass 0). Text-authored only for now; a per-pass graph is a follow-up.
    //
    // `inputs` makes the chain a DAG: entry k names the pass (by CHAIN index --
    // 0 = the base snippet, 1 = passes[0], ...) whose output this pass reads
    // through the reserved InputTexture (k = 0), InputTexture1, ... Execution
    // order is ARRAY order, so inputs may only reference EARLIER passes --
    // acyclic by construction; the editor topo-sorts on wire edits and
    // BuildMaterialChainSource refuses violations. Loading a pre-DAG file
    // (no "inputs" key) defaults to { previous pass } -- the linear chain.
    // An explicit empty array is legal (a generator pass reads nothing).
    struct MaterialPass
    {
        std::string name;      // display label ("blur", "composite", ...)
        std::string snippet;   // same //@param + shade() shape as the main snippet
        std::vector<std::uint32_t> inputs;   // chain indices feeding InputTextureN
        float posX = 0.0f, posY = 0.0f;      // pass-canvas layout (persisted)
        // Graph-owned pass (same contract as the base's graph): present = the
        // graph is this pass's editing truth and `snippet` is its GENERATED
        // text, saved so snippet-only consumers keep working. PassInput nodes
        // sample the slots `inputs` wires.
        std::optional<MaterialGraph> graph;
    };

    struct MaterialAssetData
    {
        Guid        id;                       // asset identity (embedded "id")
        Guid        parent;                   // nil = base material; valid = instance of it
        std::string name;                     // display name (defaults to file stem)
        std::string kind = "fullscreen";      // engine template kind (Slice 8: "sprite")
        std::string snippet;                  // //@param decls + shade() body (base only)
        // The vertex stage (base only, both surfaces, ONE per material --
        // chains share it): an optional `Varyings displace(Varyings v)` body
        // for %{VERTEX_BODY}. Empty = identity. Text-authored.
        std::string vertexSnippet;
        // Pass chain (fullscreen BASE materials only; additive schema: absent =
        // single-pass, exactly the pre-chain format). Passes run in order after
        // the main snippet; each reads the previous pass's output through the
        // reserved InputTexture. The sprite kind REFUSES passes (multi-pass
        // sprites are renderer-owned); instances never carry them.
        std::vector<MaterialPass> passes;
        // Saved param values by name -- applied over the //@param defaults (base)
        // or the parent chain (instance); unknown/mismatched names drop on apply.
        std::vector<std::pair<std::string, MatParamValue>> params;
        // Graph-owned authoring (Slice 9, base materials only): present = the
        // graph is the editing truth and `snippet` is its GENERATED text --
        // saved anyway so every snippet-only consumer (sprite cache, parent
        // chains, older loads) works untouched. Convert-to-text = reset() this.
        std::optional<MaterialGraph> graph;
        // Pass-canvas layout for the two fixed nodes (per-pass positions live
        // on each MaterialPass). All-zero = never laid out -> auto-layout.
        float chainBaseX = 0.0f, chainBaseY = 0.0f;
        float chainOutX = 0.0f, chainOutY = 0.0f;

        bool IsInstance() const { return parent.IsValid(); }
        bool IsGraphOwned() const { return graph.has_value(); }
    };

    // Write `data` as .arcmat JSON (values typed per MatParamValue: float /
    // [x,y] / [x,y,z,w] / texture guid string). False on IO failure.
    ARCANE_API bool SaveMaterialAsset(const std::filesystem::path& path,
                                      const MaterialAssetData& data);

    // Parse a .arcmat. nullopt on IO/parse/shape failure. Param VALUE types are
    // resolved against the snippet's own //@param decls (the decl is the truth;
    // a value that no longer matches its decl is dropped with a warning).
    ARCANE_API std::optional<MaterialAssetData> LoadMaterialAsset(
        const std::filesystem::path& path);

    // Apply saved values onto an instance built over the asset's template
    // (unknown names / type mismatches warn + skip). Returns applied count.
    ARCANE_API std::size_t ApplyMaterialParams(const MaterialAssetData& data,
                                               MaterialInstance& instance);

    // The self-typed {"type","value"} param-value shape shared by .arcmat params
    // and graph Param-node declarations (exported for MaterialGraph's use).
    ARCANE_API nlohmann::json MatParamValueToJson(const MatParamValue& v);
    ARCANE_API std::optional<MatParamValue> MatParamValueFromJson(const nlohmann::json& entry);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
