#include <Arcane/Render/MeshBuilder.hpp>

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace Arcane
{
    namespace
    {
        // One cube face's orthonormal (normal, u, v) frame, with u x v ==
        // normal for EVERY face -- that invariant is what makes the winding
        // below come out outward/CCW consistently across all six faces
        // rather than needing a per-face special case.
        //
        // WHY: with u x v == normal, the four corners
        //     p0 = normal*half - u*half - v*half
        //     p1 = normal*half + u*half - v*half
        //     p2 = normal*half + u*half + v*half
        //     p3 = normal*half - u*half + v*half
        // trace the same path as a standard CCW unit square in the (u, v)
        // plane -- (-1,-1) -> (1,-1) -> (1,1) -> (-1,1) -- viewed from a
        // point out along `normal` looking back down it (exactly the
        // MeshBuilder.hpp WINDING vantage). Fan-triangulating that quad as
        // (p0,p1,p2) and (p0,p2,p3) inherits the same orientation: e.g. for
        // the +X face, cross(p1-p0, p2-p0) works out to 4*half*half*normal,
        // a positive multiple of the face normal, not its negation.
        struct FaceBasis
        {
            glm::vec3 normal, u, v;
        };

        constexpr FaceBasis kCubeFaces[6] =
        {
            { { 1.0f,  0.0f,  0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },  // +X: Y x Z =  X
            { {-1.0f,  0.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f } },  // -X: Z x Y = -X
            { { 0.0f,  1.0f,  0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f } },  // +Y: Z x X =  Y
            { { 0.0f, -1.0f,  0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },  // -Y: X x Z = -Y
            { { 0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },  // +Z: X x Y =  Z
            { { 0.0f,  0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },  // -Z: Y x X = -Z
        };
    }

    MeshData BuildCube(float sizeMeters)
    {
        MeshData mesh;
        mesh.vertices.reserve(24);
        mesh.indices.reserve(36);

        const float half = sizeMeters * 0.5f;
        for (const FaceBasis& face : kCubeFaces)
        {
            const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
            const glm::vec3 n = face.normal * half;
            const glm::vec3 u = face.u * half;
            const glm::vec3 v = face.v * half;

            mesh.vertices.push_back(MeshVertex{ n - u - v, face.normal, { 0.0f, 0.0f } });
            mesh.vertices.push_back(MeshVertex{ n + u - v, face.normal, { 1.0f, 0.0f } });
            mesh.vertices.push_back(MeshVertex{ n + u + v, face.normal, { 1.0f, 1.0f } });
            mesh.vertices.push_back(MeshVertex{ n - u + v, face.normal, { 0.0f, 1.0f } });

            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 3);
        }
        return mesh;
    }

    MeshData BuildUvSphere(float radiusMeters, std::uint32_t rings, std::uint32_t segments)
    {
        MeshData mesh;

        const std::uint32_t rows = rings + 1;
        const std::uint32_t cols = segments + 1;
        mesh.vertices.reserve(static_cast<std::size_t>(rows) * cols);

        // theta: colatitude from the +Y pole, 0..pi. phi: azimuth about Y,
        // 0..2pi. dir = (sin(theta)*cos(phi), cos(theta), sin(theta)*sin(phi))
        // is already unit length by construction (sin/cos identity), so the
        // normal IS dir and the position is dir * radiusMeters -- no separate
        // normalize needed, and no drift between "on the sphere" and "unit
        // normal" to worry about.
        for (std::uint32_t r = 0; r < rows; ++r)
        {
            const float theta = static_cast<float>(r) * glm::pi<float>() / static_cast<float>(rings);
            const float sinT = std::sin(theta);
            const float cosT = std::cos(theta);
            for (std::uint32_t s = 0; s < cols; ++s)
            {
                const float phi = static_cast<float>(s) * glm::two_pi<float>() / static_cast<float>(segments);
                const glm::vec3 dir(sinT * std::cos(phi), cosT, sinT * std::sin(phi));
                const glm::vec2 uv(static_cast<float>(s) / static_cast<float>(segments),
                                    static_cast<float>(r) / static_cast<float>(rings));
                mesh.vertices.push_back(MeshVertex{ dir * radiusMeters, dir, uv });
            }
        }

        // Winding derivation (see MeshBuilder.hpp's WINDING contract): moving
        // along +s is the d/dphi direction, moving along +r is the d/dtheta
        // direction, and for THIS parametrization (y-pole,
        // x=sin(theta)cos(phi), y=cos(theta), z=sin(theta)sin(phi)) the cross
        // product cross(d/dphi, d/dtheta) points OUTWARD (away from the
        // origin) -- verified algebraically at theta=pi/2, phi=0 (the +X
        // equator point): d/dtheta = (0,-1,0), d/dphi = (0,0,1), and
        // cross(d/dphi, d/dtheta) = cross((0,0,1),(0,-1,0)) = (1,0,0), i.e.
        // +X, matching the outward normal there exactly.
        //
        // Both triangles below are built as (a +s edge) x (a +r edge) from
        // their first vertex, so both come out outward: e.g. triangle
        // (i00,i01,i10) has edges (i01-i00 ~ +s) and (i10-i00 ~ +r); triangle
        // (i01,i11,i10) has edges (i11-i01 ~ +r) and (i10-i01 ~ +r-s), whose
        // cross reduces to the same cross(+s,+r) term (the +r x +r part
        // vanishes). Both were also checked numerically against a concrete
        // ring/segment cell while this file was written (see the task
        // report) -- not just carried through the algebra.
        //
        // The poles (r==0, r==rings) are full rows sharing one position per
        // row -- the standard UV-sphere layout, needed so UV.u still runs
        // 0..1 there. That collapses one triangle per polar quad to zero
        // area (two of its three vertices coincide in position), which is
        // skipped outright below: a zero-area triangle's cross(edge1, edge2)
        // is the zero vector, and normalizing that is undefined -- exactly
        // what this module's winding-consistency contract cannot tolerate.
        for (std::uint32_t r = 0; r < rings; ++r)
        {
            const std::uint32_t row0 = r * cols;
            const std::uint32_t row1 = (r + 1) * cols;
            for (std::uint32_t s = 0; s < segments; ++s)
            {
                const std::uint32_t i00 = row0 + s;
                const std::uint32_t i01 = row0 + s + 1;
                const std::uint32_t i10 = row1 + s;
                const std::uint32_t i11 = row1 + s + 1;

                if (r != 0)   // top triangle degenerates at the north pole row
                {
                    mesh.indices.push_back(i00);
                    mesh.indices.push_back(i01);
                    mesh.indices.push_back(i10);
                }
                if (r != rings - 1)   // bottom triangle degenerates at the south pole row
                {
                    mesh.indices.push_back(i01);
                    mesh.indices.push_back(i11);
                    mesh.indices.push_back(i10);
                }
            }
        }
        return mesh;
    }
}
