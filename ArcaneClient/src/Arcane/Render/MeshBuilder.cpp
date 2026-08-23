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

    MeshBounds ComputeMeshBounds(const MeshData& mesh)
    {
        // Empty is explicitly legal (MeshBuilder.hpp's contract on this
        // function): the fold below only starts once there IS a vertex 0 to
        // seed min/max from, so this returns the struct's own zero default
        // rather than reaching into an out-of-range element.
        if (mesh.vertices.empty())
            return MeshBounds{};

        MeshBounds bounds;
        bounds.min = mesh.vertices[0].position;
        bounds.max = mesh.vertices[0].position;
        for (const MeshVertex& v : mesh.vertices)
        {
            bounds.min = glm::min(bounds.min, v.position);
            bounds.max = glm::max(bounds.max, v.position);
        }
        return bounds;
    }

    MeshData BuildPlane(std::uint32_t subdivisions)
    {
        // Validation is BuildMeshData's job (a later F2a task) -- this raw
        // builder only guards the one input that would otherwise divide by
        // zero below.
        if (subdivisions == 0)
            subdivisions = 1;

        MeshData mesh;
        const std::uint32_t cols = subdivisions + 1;
        mesh.vertices.reserve(static_cast<std::size_t>(cols) * cols);
        mesh.indices.reserve(static_cast<std::size_t>(subdivisions) * subdivisions * 6);

        // r walks +Z, c walks +X; UV is the grid coordinate itself, 0..1 on
        // both axes -- a plane has edges, not a wrap, so unlike the sphere
        // and the cylinder below there is no seam column to duplicate.
        for (std::uint32_t r = 0; r < cols; ++r)
        {
            const float z = -0.5f + static_cast<float>(r) / static_cast<float>(subdivisions);
            for (std::uint32_t c = 0; c < cols; ++c)
            {
                const float x = -0.5f + static_cast<float>(c) / static_cast<float>(subdivisions);
                const glm::vec2 uv(static_cast<float>(c) / static_cast<float>(subdivisions),
                                    static_cast<float>(r) / static_cast<float>(subdivisions));
                mesh.vertices.push_back(MeshVertex{ { x, 0.0f, z }, { 0.0f, 1.0f, 0.0f }, uv });
            }
        }

        // Winding derivation (same discipline as BuildUvSphere's above, for
        // a flat grid instead of a sphere): this engine's right-handed
        // X,Y,Z basis gives cross(+Z, +X) == +Y (cross(+X, +Z) is -Y, the
        // wrong way), so a triangle whose first edge walks +Z (increasing
        // r) and whose second walks +X (increasing c) lands on +Y. Both
        // triangles below are built that way from their first vertex:
        // (i00,i10,i01) has edges (i10-i00 ~ +Z) and (i01-i00 ~ +X);
        // (i10,i11,i01) has edges (i11-i10 ~ +X) and (i01-i10 ~ +X-Z), whose
        // cross reduces to the same cross(+Z,+X) term (the +X x +X part
        // vanishes) -- the same "second triangle reduces to the first
        // triangle's cross term" shape BuildUvSphere's derivation above
        // uses. Checked numerically against a concrete 2x2 grid cell while
        // this file was written (see the task report).
        for (std::uint32_t r = 0; r < subdivisions; ++r)
        {
            const std::uint32_t row0 = r * cols;
            const std::uint32_t row1 = (r + 1) * cols;
            for (std::uint32_t c = 0; c < subdivisions; ++c)
            {
                const std::uint32_t i00 = row0 + c;
                const std::uint32_t i01 = row0 + c + 1;
                const std::uint32_t i10 = row1 + c;
                const std::uint32_t i11 = row1 + c + 1;

                mesh.indices.push_back(i00);
                mesh.indices.push_back(i10);
                mesh.indices.push_back(i01);

                mesh.indices.push_back(i10);
                mesh.indices.push_back(i11);
                mesh.indices.push_back(i01);
            }
        }
        return mesh;
    }

    namespace
    {
        // Shared by BuildCylinder's two caps: a disc built as a FAN through
        // a centre vertex (never as quads -- a quad touching the fan's own
        // centre is the degenerate trap this arc's ambiguity note calls
        // out), with its own vertices kept separate from the side ring for
        // the same reason BuildCube uses 24 vertices rather than 8 -- a
        // shared vertex cannot carry both the side's radial normal and the
        // cap's vertical one.
        //
        // Increasing-phi order (centre, ring[s], ring[s+1]) lands on -Y: the
        // cap is flat, so its cross product has no y-dependence, only the
        // XZ terms, and that order is correct AS-IS for the bottom cap.
        // The top cap reverses it, the same opposite-face pattern
        // kCubeFaces above uses for its +/-Y entries (swapped u/v).
        void AppendCylinderCap(MeshData& mesh, std::uint32_t segments, float y, float normalY)
        {
            const std::uint32_t centreIdx = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(MeshVertex{ { 0.0f, y, 0.0f }, { 0.0f, normalY, 0.0f }, { 0.5f, 0.5f } });

            const std::uint32_t ringBase = centreIdx + 1;
            for (std::uint32_t s = 0; s <= segments; ++s)   // duplicate seam vertex, same as the side ring
            {
                const float phi = static_cast<float>(s) * glm::two_pi<float>() / static_cast<float>(segments);
                const float cx = std::cos(phi);
                const float cz = std::sin(phi);
                mesh.vertices.push_back(MeshVertex{ { cx * 0.5f, y, cz * 0.5f }, { 0.0f, normalY, 0.0f },
                                                     { 0.5f + cx * 0.5f, 0.5f + cz * 0.5f } });
            }

            for (std::uint32_t s = 0; s < segments; ++s)
            {
                const std::uint32_t o0 = ringBase + s;
                const std::uint32_t o1 = ringBase + s + 1;
                if (normalY > 0.0f)
                {
                    mesh.indices.push_back(centreIdx);
                    mesh.indices.push_back(o1);
                    mesh.indices.push_back(o0);
                }
                else
                {
                    mesh.indices.push_back(centreIdx);
                    mesh.indices.push_back(o0);
                    mesh.indices.push_back(o1);
                }
            }
        }
    }

    MeshData BuildCylinder(std::uint32_t segments)
    {
        MeshData mesh;

        constexpr float kRadius = 0.5f;
        constexpr float kHalfHeight = 0.5f;
        const std::uint32_t cols = segments + 1;   // duplicate seam column, u 0..1

        // ---- Side ring: 2 rows (bottom, top) x cols columns, purely radial
        // normals -- a flat side has no vertical component to its normal,
        // unlike the sphere's dir formula above.
        const std::uint32_t sideBase = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.reserve(static_cast<std::size_t>(cols) * 4 + 2);   // side (2*cols) + 2 caps (cols+1 each)
        mesh.indices.reserve(static_cast<std::size_t>(segments) * 12);   // side (6*segments) + 2 caps (3*segments each)
        for (std::uint32_t row = 0; row < 2; ++row)
        {
            const float y = (row == 0) ? -kHalfHeight : kHalfHeight;
            for (std::uint32_t s = 0; s < cols; ++s)
            {
                const float phi = static_cast<float>(s) * glm::two_pi<float>() / static_cast<float>(segments);
                const glm::vec3 radial(std::cos(phi), 0.0f, std::sin(phi));
                const glm::vec2 uv(static_cast<float>(s) / static_cast<float>(segments), static_cast<float>(row));
                mesh.vertices.push_back(MeshVertex{ radial * kRadius + glm::vec3(0.0f, y, 0.0f), radial, uv });
            }
        }

        // Winding: a side quad's two edges are (a +Y edge, bottom->top) and
        // (a +phi edge, along the ring); cross(+Y, +phi-tangent) works out
        // to the outward radial direction -- checked at phi=0, dphi=pi/2
        // while writing this file: cross((0,1,0), (-0.5,0,0.5)) = (0.5,0,
        // 0.5), the same direction as the outward normal there. Both
        // triangles below are built (a +Y edge) x (a +phi edge) from their
        // first vertex for exactly that reason, the same edge-order
        // discipline BuildUvSphere and BuildPlane use above with a
        // different pair of directions.
        for (std::uint32_t s = 0; s < segments; ++s)
        {
            const std::uint32_t b0 = sideBase + s;
            const std::uint32_t b1 = sideBase + s + 1;
            const std::uint32_t t0 = sideBase + cols + s;
            const std::uint32_t t1 = sideBase + cols + s + 1;

            mesh.indices.push_back(b0);
            mesh.indices.push_back(t0);
            mesh.indices.push_back(b1);

            mesh.indices.push_back(t0);
            mesh.indices.push_back(t1);
            mesh.indices.push_back(b1);
        }

        AppendCylinderCap(mesh, segments, -kHalfHeight, -1.0f);
        AppendCylinderCap(mesh, segments, kHalfHeight, 1.0f);

        return mesh;
    }

    namespace
    {
        // Shared by BuildCapsule's two hemispheres: BuildUvSphere's exact
        // (theta, phi) -> dir parametrization above, generalized to an
        // arbitrary theta range and a y-offset so ONE routine builds both
        // caps (theta 0..pi/2 for the top, pi/2..pi for the bottom). dir IS
        // the outward normal here too, radial from whichever centre this
        // call is offsetting around. At theta == pi/2 the formula gives
        // cosT == 0, i.e. a purely horizontal dir -- exactly the
        // "horizontal radial" band normal this arc's brief asks for, with
        // no separate case needed: a band vertex IS an equator vertex, just
        // relabelled.
        void AppendHemisphereVertices(MeshData& mesh, std::uint32_t rings, std::uint32_t segments,
                                       float thetaStart, float thetaEnd, float yOffset)
        {
            const std::uint32_t cols = segments + 1;
            for (std::uint32_t r = 0; r <= rings; ++r)
            {
                const float theta = thetaStart + (thetaEnd - thetaStart) * static_cast<float>(r) / static_cast<float>(rings);
                const float sinT = std::sin(theta);
                const float cosT = std::cos(theta);
                for (std::uint32_t s = 0; s < cols; ++s)
                {
                    const float phi = static_cast<float>(s) * glm::two_pi<float>() / static_cast<float>(segments);
                    const glm::vec3 dir(sinT * std::cos(phi), cosT, sinT * std::sin(phi));
                    const glm::vec2 uv(static_cast<float>(s) / static_cast<float>(segments),
                                        static_cast<float>(r) / static_cast<float>(rings));
                    mesh.vertices.push_back(MeshVertex{ dir * 0.5f + glm::vec3(0.0f, yOffset, 0.0f), dir, uv });
                }
            }
        }

        // A hemisphere block's own internal triangles: the SAME
        // (i00,i01,i10) / (i01,i11,i10) winding BuildUvSphere derives above
        // (identical dir formula, so the identical proof applies), with the
        // pole-skip on whichever end of THIS call is actually degenerate.
        // AppendHemisphereVertices' two callers walk opposite halves of
        // theta 0..pi, so their poles land on opposite ends: the top
        // hemisphere's pole is row 0 (skip the top triangle there); the
        // bottom's is row `rings` (skip the bottom triangle at the band
        // feeding it). Neither hemisphere's OWN equator row is a pole, so
        // it never trips either skip -- the equator stays a full,
        // non-degenerate ring in both blocks.
        void AppendHemisphereTriangles(MeshData& mesh, std::uint32_t base, std::uint32_t rings,
                                        std::uint32_t segments, bool poleAtStart)
        {
            const std::uint32_t cols = segments + 1;
            for (std::uint32_t r = 0; r < rings; ++r)
            {
                const std::uint32_t row0 = base + r * cols;
                const std::uint32_t row1 = base + (r + 1) * cols;
                for (std::uint32_t s = 0; s < segments; ++s)
                {
                    const std::uint32_t i00 = row0 + s;
                    const std::uint32_t i01 = row0 + s + 1;
                    const std::uint32_t i10 = row1 + s;
                    const std::uint32_t i11 = row1 + s + 1;

                    if (!(poleAtStart && r == 0))
                    {
                        mesh.indices.push_back(i00);
                        mesh.indices.push_back(i01);
                        mesh.indices.push_back(i10);
                    }
                    if (!(!poleAtStart && r == rings - 1))
                    {
                        mesh.indices.push_back(i01);
                        mesh.indices.push_back(i11);
                        mesh.indices.push_back(i10);
                    }
                }
            }
        }
    }

    MeshData BuildCapsule(std::uint32_t rings, std::uint32_t segments, float lengthRatio)
    {
        MeshData mesh;

        // Total height is lengthRatio (diameter 1), so half of the
        // CYLINDRICAL section's length is (lengthRatio - 1) / 2 -- clamped
        // at zero so a ratio <= 1 collapses to a sphere (the pinned legal
        // case at exactly 1) rather than folding the two hemispheres
        // through each other.
        const float halfSection = 0.5f * glm::max(lengthRatio - 1.0f, 0.0f);
        const std::uint32_t cols = segments + 1;
        const std::uint32_t rows = rings + 1;   // rings+1 vertex rows PER hemisphere

        const std::uint32_t topBase = 0;
        const std::uint32_t botBase = rows * cols;
        mesh.vertices.reserve(static_cast<std::size_t>(rows) * cols * 2);
        mesh.indices.reserve(static_cast<std::size_t>(rings) * segments * 12 + static_cast<std::size_t>(segments) * 6);

        AppendHemisphereVertices(mesh, rings, segments, 0.0f, glm::half_pi<float>(), halfSection);               // top: pole -> its own equator
        AppendHemisphereVertices(mesh, rings, segments, glm::half_pi<float>(), glm::pi<float>(), -halfSection);  // bottom: its own equator -> pole

        AppendHemisphereTriangles(mesh, topBase, rings, segments, /*poleAtStart=*/true);
        AppendHemisphereTriangles(mesh, botBase, rings, segments, /*poleAtStart=*/false);

        // The band: the top hemisphere's LAST row (its own equator)
        // connected to the bottom hemisphere's FIRST row (its own equator).
        // Both rings share the same radius (0.5) and the same per-column
        // horizontal normal by construction (theta == pi/2 at both ends),
        // so this is an ordinary, non-polar ring-to-ring step -- reusing
        // the identical (i00,i01,i10) / (i01,i11,i10) winding needs no new
        // derivation, only new row indices.
        //
        // Skipped entirely at halfSection == 0 (lengthRatio == 1): the two
        // rings are then COINCIDENT (same position, same normal), and
        // connecting them would emit 2 * segments zero-area triangles (the
        // loop below runs once per segment and pushes two) -- exactly the
        // trap this arc's ambiguity note calls out.
        if (halfSection > 0.0f)
        {
            const std::uint32_t row0 = topBase + rings * cols;
            const std::uint32_t row1 = botBase;
            for (std::uint32_t s = 0; s < segments; ++s)
            {
                const std::uint32_t i00 = row0 + s;
                const std::uint32_t i01 = row0 + s + 1;
                const std::uint32_t i10 = row1 + s;
                const std::uint32_t i11 = row1 + s + 1;

                mesh.indices.push_back(i00);
                mesh.indices.push_back(i01);
                mesh.indices.push_back(i10);

                mesh.indices.push_back(i01);
                mesh.indices.push_back(i11);
                mesh.indices.push_back(i10);
            }
        }

        return mesh;
    }
}
