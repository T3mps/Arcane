#pragma once

// MaterialTemplate: the compiled parameter TABLE of a material -- layout, never
// values (UE's FUniformExpressionSet, scaled down). Build() takes the ordered
// declarations from the authoring side (the //@param parser in Slice 4, the node
// graph in Slice 9), assigns each numeric param a cbuffer byte offset under HLSL
// packing rules, assigns each texture param an ordinal in the parallel texture
// table, sizes the CB, and bakes the defaults blob the pack loop starts from.
// Immutable after Build -- instances reference it, they never mutate it.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API MaterialTemplate
    {
    public:
        MaterialTemplate() = default;

        // Build the immutable layout. Fills each decl's nameHash; assigns cbOffsets
        // under HLSL cbuffer packing (4-byte slots, a vector never straddles a
        // 16-byte register -- which forces float4/Color onto register starts);
        // assigns texture ordinals in declaration order; rounds cbSize up to 16;
        // packs the defaults blob. Duplicate names keep the FIRST decl (later ones
        // are dropped with a warning). `sourceHash` is the shader-identity stamp
        // (template kind + snippet content) the compile cache keys on later.
        static MaterialTemplate Build(std::string name, std::uint64_t sourceHash,
                                      std::vector<ParamDecl> decls);

        const std::string& Name() const { return m_name; }
        std::uint64_t SourceHash() const { return m_sourceHash; }

        const std::vector<ParamDecl>& Params() const { return m_params; }

        // Lookup by HashParamName(name); nullptr if unknown.
        const ParamDecl* Find(std::uint32_t nameHash) const;

        // Material cbuffer size in bytes, 16-aligned. 0 when there are no numeric params.
        std::uint32_t CbSize() const { return m_cbSize; }

        // Number of Texture params == size of the parallel texture table.
        std::uint32_t TextureCount() const { return m_textureCount; }

        // CbSize() bytes: every numeric param's default at its cbOffset, padding zeroed.
        // The pack loop copies this first so unwritten bytes are always deterministic.
        const std::vector<std::uint8_t>& Defaults() const { return m_defaults; }

    private:
        std::string   m_name;
        std::uint64_t m_sourceHash = 0;
        std::vector<ParamDecl> m_params;
        std::vector<std::uint8_t> m_defaults;
        std::uint32_t m_cbSize = 0;
        std::uint32_t m_textureCount = 0;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
