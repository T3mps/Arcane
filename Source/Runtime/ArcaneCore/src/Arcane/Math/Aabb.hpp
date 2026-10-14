#pragma once

// Aabb -- the ONE axis-aligned box (F3, spec s2.1). Header-only, glm-only: no
// engine dependency, so Core's mesh code, the scene's WorldBounds, the
// editor's framing and the render layer's culling all read one type.
//
// An AGGREGATE on purpose: `MeshBounds` (MeshBuilder.hpp) is an alias of this
// struct and its brace-inits `{min, max}` keep compiling. The default-
// constructed ZERO box is the "framing an empty mesh" answer ComputeMeshBounds
// documents; Empty() is the fold sentinel (+inf/-inf) a Union chain starts
// from and is never stored on a component.
#include <glm/glm.hpp>

#include <limits>
#include <span>

namespace Arcane
{
    struct Aabb
    {
        glm::vec3 min{0.0f, 0.0f, 0.0f};
        glm::vec3 max{0.0f, 0.0f, 0.0f};

        [[nodiscard]] static Aabb Empty() noexcept
        {
            constexpr float inf = std::numeric_limits<float>::infinity();
            return Aabb{ glm::vec3(inf), glm::vec3(-inf) };
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return min.x > max.x || min.y > max.y || min.z > max.z;
        }

        [[nodiscard]] static Aabb FromPoints(std::span<const glm::vec3> points) noexcept
        {
            Aabb b = Empty();
            for (const glm::vec3& p : points)
            {
                b.min = glm::min(b.min, p);
                b.max = glm::max(b.max, p);
            }
            return b;
        }

        [[nodiscard]] glm::vec3 Center() const noexcept { return (min + max) * 0.5f; }
        [[nodiscard]] glm::vec3 Extent() const noexcept { return (max - min) * 0.5f; }   // half-size

        [[nodiscard]] Aabb Union(const Aabb& o) const noexcept
        {
            if (IsEmpty()) return o;
            if (o.IsEmpty()) return *this;
            return Aabb{ glm::min(min, o.min), glm::max(max, o.max) };
        }

        // The box of the eight transformed corners: conservative for any
        // affine (rotation grows it; the true rotated box is smaller).
        [[nodiscard]] Aabb Transformed(const glm::mat4& m) const noexcept
        {
            Aabb b = Empty();
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 c((i & 1) ? max.x : min.x,
                                  (i & 2) ? max.y : min.y,
                                  (i & 4) ? max.z : min.z);
                const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
                b.min = glm::min(b.min, w);
                b.max = glm::max(b.max, w);
            }
            return b;
        }

        [[nodiscard]] Aabb Widened(float epsilon) const noexcept
        {
            return Aabb{ min - glm::vec3(epsilon), max + glm::vec3(epsilon) };
        }

        [[nodiscard]] bool operator==(const Aabb&) const noexcept = default;
    };
}
