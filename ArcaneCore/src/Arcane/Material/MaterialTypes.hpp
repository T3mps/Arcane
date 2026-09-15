#pragma once

// Material parameter primitives (the CPU core of the material system, Slice 1 of the
// shader-editor arc). A material declares an ordered set of typed parameters; one
// tagged-union value type (MatParamValue) carries any of them, so declarations,
// sparse instance overrides, and the pack loop all speak a single currency (UE's
// FMaterialParameterValue, scaled down). Numeric params live in the material cbuffer
// at offsets assigned by MaterialTemplate::Build; Texture params carry an asset Guid
// and are bound through a parallel texture table -- never packed into the CB.
// Editor-only metadata (ParamMeta) rides BESIDE the runtime decl, not inside it.

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace Arcane
{
    enum class MatParamType : std::uint8_t
    {
        Float,
        Float2,
        Float4,
        Color,     // float4 in the CB; distinct so the editor shows a color picker
        Texture,   // Guid ref resolved via the texture table, no CB space
    };

    // Numeric lane count (0 for Texture).
    [[nodiscard]] constexpr std::uint32_t ComponentCount(MatParamType t) noexcept
    {
        switch (t)
        {
            case MatParamType::Float:   return 1;
            case MatParamType::Float2:  return 2;
            case MatParamType::Float4:  return 4;
            case MatParamType::Color:   return 4;
            case MatParamType::Texture: return 0;
        }
        return 0;
    }

    // Bytes the param occupies in the material cbuffer (0 for Texture).
    [[nodiscard]] constexpr std::uint32_t CbByteSize(MatParamType t) noexcept
    {
        return ComponentCount(t) * sizeof(float);
    }

    // 32-bit FNV-1a over the parameter name -- the runtime key for lookups and
    // overrides (decls keep the string for the editor, codegen, and serialization).
    [[nodiscard]] constexpr std::uint32_t HashParamName(std::string_view name) noexcept
    {
        std::uint32_t h = 2166136261u;
        for (char c : name)
        {
            h ^= static_cast<std::uint8_t>(c);
            h *= 16777619u;
        }
        return h;
    }

    // One value slot that can carry any parameter type. Factories zero the unused
    // lanes so equality and the pack loop are deterministic byte-wise.
    struct MatParamValue
    {
        MatParamType type = MatParamType::Float;
        union
        {
            float f[4];
            Guid  tex;
        };

        MatParamValue() : f{0.0f, 0.0f, 0.0f, 0.0f} {}

        [[nodiscard]] static MatParamValue MakeFloat(float x)
        {
            MatParamValue v;
            v.type = MatParamType::Float;
            v.f[0] = x;
            return v;
        }

        [[nodiscard]] static MatParamValue MakeFloat2(float x, float y)
        {
            MatParamValue v;
            v.type = MatParamType::Float2;
            v.f[0] = x;
            v.f[1] = y;
            return v;
        }

        [[nodiscard]] static MatParamValue MakeFloat4(float x, float y, float z, float w)
        {
            MatParamValue v;
            v.type = MatParamType::Float4;
            v.f[0] = x;
            v.f[1] = y;
            v.f[2] = z;
            v.f[3] = w;
            return v;
        }

        [[nodiscard]] static MatParamValue MakeColor(float r, float g, float b, float a = 1.0f)
        {
            MatParamValue v = MakeFloat4(r, g, b, a);
            v.type = MatParamType::Color;
            return v;
        }

        [[nodiscard]] static MatParamValue MakeTexture(Guid id)
        {
            MatParamValue v;
            v.type = MatParamType::Texture;
            v.tex = id;
            return v;
        }

        [[nodiscard]] bool operator==(const MatParamValue& o) const noexcept
        {
            if (type != o.type)
                return false;
            if (type == MatParamType::Texture)
                return tex == o.tex;
            return std::memcmp(f, o.f, sizeof(f)) == 0;
        }
        [[nodiscard]] bool operator!=(const MatParamValue& o) const noexcept { return !(*this == o); }
    };

    // Editor-facing metadata for one parameter -- grouping, help text, slider range.
    // Kept beside the runtime decl so the hot pack loop never touches strings it
    // doesn't need (UE's FMaterialParameterMetadata split).
    struct ParamMeta
    {
        std::string group;
        std::string tooltip;
        float sliderMin = 0.0f;
        float sliderMax = 1.0f;
    };

    // One declared parameter. Authoring fills name/type/def; MaterialTemplate::Build
    // fills nameHash and assigns exactly one slot: cbOffset for numeric params (HLSL
    // cbuffer packing) or textureIndex for Texture params (ordinal in the texture
    // table). The unassigned slot stays kNoSlot.
    struct ParamDecl
    {
        static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

        std::string   name;
        std::uint32_t nameHash = 0;
        MatParamType  type = MatParamType::Float;
        MatParamValue def;

        std::uint32_t cbOffset = kNoSlot;
        std::uint32_t textureIndex = kNoSlot;
    };
}
