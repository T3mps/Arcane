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
    // through ShaderSourceProvider) and the surface for a .armat "kind" string
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
    ARCANE_API std::string GenerateMaterialBindings(const MaterialTemplate& templ,
                                                    MaterialSurface surface = MaterialSurface::Fullscreen);

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
    ARCANE_API MaterialBuildResult BuildMaterialShaderSource(std::string_view templateText,
                                                             std::string_view snippet,
                                                             std::string materialName,
                                                             MaterialSurface surface = MaterialSurface::Fullscreen);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
