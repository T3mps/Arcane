#pragma once

// Material source authoring pipeline (Slice 4 of the shader-editor arc): the
// designer writes a SNIPPET -- `//@param` declarations + a `float4 shade(Varyings)`
// body -- and the engine owns everything else. This module turns that snippet
// into the exact HLSL the compile service sees:
//
//   ParseMaterialSource   //@param lines -> ParamDecl/ParamMeta (+ line errors)
//   MaterialTemplate::Build   decls -> cbuffer layout (Slice 1)
//   GenerateMaterialBindings  layout -> the `cbuffer Material : register(b0)`
//                             block + Texture2D/SamplerState declarations
//   StitchShaderTemplate      %{SLOT} substitution into the engine template
//   BuildMaterialShaderSource the one-stop: snippet + template text -> full HLSL
//
// The node graph (Slice 9) emits into the SAME %{MATERIAL_BODY} slot -- this
// seam is why both authoring front-ends cost one implementation.
//
// `//@param` grammar (settled here per spec section 8.1; parser kept isolated
// so changes stay cheap):
//   //@param <type> <name> [= <default>] [[min..max]]
//   type    = float | float2 | float4 | color | texture
//   default = <n> for float; (x, y) for float2; (x, y, z[, w]) for float4
//             (w required) and color (w optional, defaults 1). texture takes
//             no default (Guid refs bind through the instance).
//   range   = [min..max] slider hint (numeric types only) -> ParamMeta.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on dll-exported types: benign under /MD
#endif
    struct MaterialSourceParse
    {
        std::vector<ParamDecl> decls;    // valid declarations, in source order
        std::vector<ParamMeta> metas;    // parallel to decls
        std::vector<std::string> errors; // "line N: message" -- bad lines are skipped
    };

    // Extract //@param declarations from a snippet. The snippet itself is NOT
    // transformed -- //@param lines are legal HLSL comments and ride into the
    // stitched source verbatim, so diagnostic line numbers match what the
    // designer sees. Duplicate names and names reserved by the template
    // (Time, DeltaTime, ViewportSize, MaterialSampler) are errors.
    ARCANE_API MaterialSourceParse ParseMaterialSource(std::string_view snippet);

    // Which engine template a material source stitches into. Each surface owns
    // a register map (GlobalParams.hpp): Fullscreen = material CB b0, textures
    // t0.., sampler emitted here; Sprite = the batcher's push constants own b0
    // and the sprite texture owns t0, so the material CB sits at b1 and
    // declared textures at t1.. (the template declares MaterialSampler itself).
    enum class MaterialSurface : std::uint8_t { Fullscreen, Sprite };

    // The engine template file for a surface ("materials/....hlsl", resolved
    // through ShaderSourceProvider) and the surface for a .arcmat "kind" string
    // (unknown kinds fall back to Fullscreen).
    ARCANE_API const char* MaterialTemplateFile(MaterialSurface surface);
    ARCANE_API MaterialSurface MaterialSurfaceForKind(std::string_view kind);

    // The %{MATERIAL_CBUFFER} payload for a built template: the Material cbuffer
    // (members in declaration order -- HLSL's packing mirrors
    // MaterialTemplate::Build's, keeping shader offsets in lockstep with the
    // CPU layout), one Texture2D per texture param and, on the Fullscreen
    // surface, one shared MaterialSampler (s0) when any exist. Register
    // assignments follow the surface's map. Empty when the template has no
    // params.
    // A pass may read up to this many upstream pass outputs (InputTexture,
    // InputTexture1..3 -- the reserved-name list is sized to match).
    inline constexpr std::uint32_t kMaxPassInputs = 4;

    // `chainInputs` (fullscreen pass chains only): additionally declare that
    // many reserved upstream textures -- `InputTexture`, `InputTexture1`, ... --
    // at the slots after the material's own textures, and always emit
    // MaterialSampler (they need it even when the material declares no
    // textures). 0 = not a chain (the single-material path).
    ARCANE_API std::string GenerateMaterialBindings(const MaterialTemplate& templ,
                                                    MaterialSurface surface = MaterialSurface::Fullscreen,
                                                    std::uint32_t chainInputs = 0);

    // Replace every %{NAME} in `templateText` with its slot value. Slot names
    // not in `slots` are left in place and reported through `unresolved` (when
    // non-null) -- a stitched source with unresolved slots will not compile,
    // by design.
    ARCANE_API std::string StitchShaderTemplate(
        std::string_view templateText,
        std::span<const std::pair<std::string_view, std::string_view>> slots,
        std::vector<std::string>* unresolved = nullptr);

    struct MaterialBuildResult
    {
        MaterialTemplate templ;          // layout from the parsed declarations
        std::vector<ParamMeta> metas;    // parallel to templ.Params()
        std::string hlsl;                // the stitched full source
        std::vector<std::string> errors; // parse errors + unresolved slots
    };

    // The one-stop authoring call: parse the snippet, build the layout (source
    // hash covers template + snippet text -- the compile cache key input), emit
    // bindings for `surface`, stitch. `hlsl` is always produced (the editor
    // compiles what it can and shows `errors` beside it).
    //
    // `vertexSnippet` (the vertex stage): an optional designer
    // `Varyings displace(Varyings v)` body stitched into %{VERTEX_BODY};
    // empty = identity passthrough. It runs at the END of vs_main (pos is
    // clip space) and sees params + Globals; //@param declarations belong in
    // the PIXEL snippet only.
    ARCANE_API MaterialBuildResult BuildMaterialShaderSource(std::string_view templateText,
                                                             std::string_view snippet,
                                                             std::string materialName,
                                                             MaterialSurface surface = MaterialSurface::Fullscreen,
                                                             std::string_view vertexSnippet = {});

    // One pass's authored surface, as the chain builder consumes it. `inputs`
    // are CHAIN indices (0 = the base snippet) feeding InputTexture(N); they
    // must reference EARLIER passes only (execution order = span order), which
    // is what keeps the pass DAG acyclic by construction.
    struct MaterialChainPassDesc
    {
        std::string_view snippet;
        std::span<const std::uint32_t> inputs;
    };

    struct MaterialChainBuildResult
    {
        MaterialTemplate templ;               // ONE merged layout across all passes
        std::vector<ParamMeta> metas;         // parallel to templ.Params()
        std::vector<std::string> hlsl;        // one stitched full source per pass
        std::vector<std::vector<std::string>> passErrors;   // per-pass parse errors
        std::vector<std::string> errors;      // chain-level: merge/DAG violations
        // Validated per-pass input lists (verbatim from the descs) and the
        // UNIFORM InputTexture decl count every pass's source carries -- what
        // the runner sizes its binding layout with.
        std::vector<std::vector<std::uint32_t>> passInputs;
        std::uint32_t chainInputSlots = 1;
        [[nodiscard]] bool Ok() const noexcept
        {
            bool ok = errors.empty();
            for (const auto& pe : passErrors)
                ok = ok && pe.empty();
            return ok;
        }
    };

    // Fullscreen pass chains: stitch EACH pass snippet into its own full source,
    // all sharing ONE merged param surface (the union of every pass's //@param
    // decls -- same name + same type is one shared param, first declaration
    // wins default/range; conflicting types are chain errors) and one UNIFORM
    // set of reserved upstream textures (see GenerateMaterialBindings; the decl
    // count is the max input count over the chain, min 1). One template, one
    // instance, one packed CB bound to every pass. DAG rules enforced here:
    // pass 0 takes no inputs, every input references an earlier pass, at most
    // kMaxPassInputs per pass.
    // `vertexSnippet`: the material's ONE vertex stage, stitched into every
    // pass's source (chains share the fullscreen vertex path).
    ARCANE_API MaterialChainBuildResult BuildMaterialChainSource(
        std::string_view templateText,
        std::span<const MaterialChainPassDesc> passes,
        std::string materialName,
        std::string_view vertexSnippet = {});
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
