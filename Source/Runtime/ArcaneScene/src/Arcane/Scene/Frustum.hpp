#pragma once

// Frustum -- six planes off a ViewTransform (F3, spec s3). Gribb & Hartmann
// (2001): with glm's column-major `clip = M * p`, the planes are sums and
// differences of M's ROWS; in [0,1] forward-Z the near plane is row 2 alone.
// The orthographic case needs no branch -- the same rows of orthoRH_ZO yield
// six planes. Every plane is normalised so `Distance` is metres.
//
// Contains() is CONSERVATIVE BY CONTRACT (spec s3): the centre/extent
// "push-out" test (UE's FConvexVolume::IntersectBox, ConvexVolume.cpp:283-289;
// algebraically the p-vertex test) has false positives at corners and never a
// false negative -- a box that touches the frustum is never rejected.
// FrustumTest.cpp pins the property with rapidcheck.
//
// THE JITTER RULE: extract from the UNJITTERED view. F5's TAA jitter must not
// flip a culling decision frame to frame.
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <array>
#include <cmath>

namespace Arcane
{
    struct Plane
    {
        glm::vec3 n{0.0f, 0.0f, 1.0f};
        float     d = 0.0f;   // n . p + d >= 0 is INSIDE

        [[nodiscard]] float Distance(const glm::vec3& p) const noexcept { return glm::dot(n, p) + d; }
    };

    struct Frustum
    {
        // L, R, B, T, N, F
        std::array<Plane, 6> planes{};

        [[nodiscard]] static Frustum FromViewProjection(const glm::mat4& vp) noexcept
        {
            const glm::vec4 r0 = glm::row(vp, 0);
            const glm::vec4 r1 = glm::row(vp, 1);
            const glm::vec4 r2 = glm::row(vp, 2);
            const glm::vec4 r3 = glm::row(vp, 3);
            const glm::vec4 rows[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
            Frustum f;
            for (int i = 0; i < 6; ++i)
            {
                const glm::vec3 n(rows[i]);
                const float len = glm::length(n);
                const float inv = len > 0.0f ? 1.0f / len : 0.0f;
                f.planes[static_cast<std::size_t>(i)] = Plane{ n * inv, rows[i].w * inv };
            }
            return f;
        }

        [[nodiscard]] static Frustum From(const ViewTransform& view) noexcept
        {
            return FromViewProjection(view.ViewProjection());
        }

        // Every plane pushed OUTWARD by `slack` metres: a bigger frustum.
        [[nodiscard]] Frustum Widened(float slack) const noexcept
        {
            Frustum f = *this;
            for (Plane& p : f.planes)
                p.d += slack;
            return f;
        }

        [[nodiscard]] bool Contains(const Aabb& box) const noexcept
        {
            const glm::vec3 c = box.Center();
            const glm::vec3 e = box.Extent();
            for (const Plane& p : planes)
            {
                const float r = e.x * std::fabs(p.n.x) + e.y * std::fabs(p.n.y) + e.z * std::fabs(p.n.z);
                if (p.Distance(c) + r < 0.0f)
                    return false;
            }
            return true;
        }
    };
}
