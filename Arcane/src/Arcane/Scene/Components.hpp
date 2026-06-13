#pragma once

// Scene components: plain reflected data. Every engine component is
// ASTRA_REFLECT-annotated from day one (design rule) so the editor/JSON/server
// path stays open at ~zero cost via the Astra 3.2 visitFields seam. Header-only;
// reflect blocks at namespace scope register once per module (MetaRegistry is
// idempotent), and the simulation Registry is owned by the host module.

#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Arcane
{
    struct LocalTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;          // radians
        glm::vec2 scale{1.0f, 1.0f};

        // 2D TRS in column-major homogeneous mat3 (translation in column 2).
        glm::mat3 ToMatrix() const
        {
            const float c = std::cos(rotation);
            const float s = std::sin(rotation);
            glm::mat3 m(1.0f);
            m[0] = glm::vec3(c * scale.x,  s * scale.x, 0.0f);
            m[1] = glm::vec3(-s * scale.y, c * scale.y, 0.0f);
            m[2] = glm::vec3(position.x,   position.y,  1.0f);
            return m;
        }
    };

    struct WorldTransform
    {
        glm::mat3 matrix{1.0f};             // computed by TransformPropagationSystem; never authored
    };

    struct SpriteRenderer
    {
        uint32_t  textureId = 0;            // 0 => untextured tinted quad; resolved via TextureTable
        glm::vec2 size{32.0f, 32.0f};       // base pixel size before world scale
        glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
        int32_t   sortingLayer = 0;
        int32_t   orderInLayer = 0;
    };
}

// Reflection blocks at namespace scope (NOT anonymous). WorldTransform::matrix is
// derived state -> Serializable(false) so name-keyed JSON skips it (recomputed on
// load; binary trivially-copyable path still round-trips it harmlessly).
namespace Arcane
{
    ASTRA_REFLECT_TYPE(LocalTransform)
        ASTRA_REFLECT_FIELD(LocalTransform, position)
        ASTRA_REFLECT_FIELD(LocalTransform, rotation)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
        ASTRA_REFLECT_FIELD(LocalTransform, scale)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(WorldTransform)
        ASTRA_REFLECT_FIELD(WorldTransform, matrix)
            ASTRA_REFLECT_ATTR(Serializable, false)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(SpriteRenderer)
        ASTRA_REFLECT_FIELD(SpriteRenderer, textureId)
        ASTRA_REFLECT_FIELD(SpriteRenderer, size)
        ASTRA_REFLECT_FIELD(SpriteRenderer, tint)
        ASTRA_REFLECT_FIELD(SpriteRenderer, sortingLayer)
        ASTRA_REFLECT_FIELD(SpriteRenderer, orderInLayer)
    ASTRA_END_REFLECT_TYPE()
}
