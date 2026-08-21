#pragma once

// MeshBuilder: procedural mesh generators for the 3D slice's own geometry.
//
// PURE AND DEVICE-FREE -- no NRI, no NriDevice, no GPU type anywhere in this
// file or its .cpp. Same discipline GraphGridPhase
// (ArcaneEditor/src/Widgets/GraphGridPhase.hpp) follows, and for the same
// reason: it is what lets a headless test drive the whole builder without a
// device.
//
// Task 6 (Phase 4): this phase renders procedural geometry ON PURPOSE -- mesh
// import (cgltf, .arcmesh) is a separate, later arc, and staying out of it is
// a decision already made, not something this file revisits. The interface
// below is exactly BuildCube and BuildUvSphere -- no plane, no cylinder, no
// capsule, no tangents (tangent generation needs a vendored library, which
// belongs to that later arc), no index-buffer optimisation.
//
// WINDING (Task 7 depends on this being stated exactly): front-facing is
// COUNTER-CLOCKWISE as seen from OUTSIDE the surface -- i.e. from a point
// out along the vertex normal, looking back down it toward the surface. That
// is the same vantage this engine's camera takes on everything it renders,
// per Task 5's right-handed convention (+X right, +Y up, camera forward
// -Z): looking down -Z from outside a surface whose normal points toward you
// is exactly "looking down -normal". Concretely, for every emitted triangle
// (v0, v1, v2):
//     normalize(cross(v1.position - v0.position, v2.position - v0.position))
// points the same way as the triangle's (averaged) vertex normal -- never the
// opposite way. Task 7's rasterizer state must cull CLOCKWISE-as-seen-from-
// outside faces (i.e. treat CCW as front-facing) to match what this module
// emits.
//
// Units are METERS throughout (sizeMeters, radiusMeters) -- MKS, matching the
// rest of the engine.

#include <Arcane/Base/Api.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Arcane
{
    struct MeshVertex
    {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
    };

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    // A cube centered at the origin, side length `sizeMeters` (so every
    // vertex sits at (+-sizeMeters/2, +-sizeMeters/2, +-sizeMeters/2)).
    //
    // 24 vertices, not 8: each of the 6 faces needs its OWN normal, and a
    // shared-corner vertex cannot carry three different face normals at
    // once -- so every face gets its own 4 vertices (4 * 6 = 24). 36
    // indices: 2 triangles * 3 indices * 6 faces.
    [[nodiscard]] ARCANE_API MeshData BuildCube(float sizeMeters);

    // A UV sphere of the given radius, centered at the origin, poled on Y.
    // `rings` is the number of latitude bands between the poles (so there
    // are rings+1 vertex rows, theta running 0..pi); `segments` is the
    // number of longitude slices (segments+1 vertex columns per row -- the
    // extra column duplicates the seam vertex so UV.u can run 0..1 without a
    // wraparound discontinuity, the standard UV-sphere layout).
    //
    // Every vertex lands at exactly `radiusMeters` from the origin (its
    // normal IS its position, normalized) and every vertex normal is that
    // same unit radial direction -- so the winding-consistency contract
    // above and the "on the sphere" contract are, for this builder, the same
    // geometry expressed twice.
    [[nodiscard]] ARCANE_API MeshData BuildUvSphere(float radiusMeters, std::uint32_t rings, std::uint32_t segments);
}
