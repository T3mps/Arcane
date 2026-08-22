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

    // Local axis-aligned bounds of a mesh, in the mesh's own space.
    //
    // F3 owns culling; this exists in F2a because MeshDocument's preview has
    // to FRAME the mesh, and it is computed from vertices rather than derived
    // per-source analytically because F2c's imported meshes need that path
    // regardless. An EMPTY mesh returns a zero box, not the inverted-infinity
    // sentinel a min/max fold starts from -- a caller framing an empty mesh
    // needs a degenerate box it can still build a camera from, and the
    // sentinel would hand it infinities.
    struct MeshBounds
    {
        glm::vec3 min{0.0f, 0.0f, 0.0f};
        glm::vec3 max{0.0f, 0.0f, 0.0f};
    };

    [[nodiscard]] ARCANE_API MeshBounds ComputeMeshBounds(const MeshData& mesh);

    // ---- UNIT generators (F2a) ------------------------------------------
    // Every generator below emits a UNIT shape. Size is the Transform's job
    // (spec: "scale expresses size, rotation expresses orientation, the asset
    // expresses shape"), which is why none of them takes a size in meters the
    // way BuildCube/BuildUvSphere above do -- those two predate the rule and
    // keep their parameters because .arcmesh always passes 1.0 / 0.5.

    // The unit XZ plane: normal +Y, spanning [-0.5, +0.5] in X and Z.
    // `subdivisions` is quads PER AXIS, so 1 is a single quad (two triangles)
    // and 3 is nine quads. A camera-facing quad is this mesh under a -90 degree
    // X rotation -- orientation is the Transform's job for the same reason
    // size is, which is why there is no separate Quad source.
    [[nodiscard]] ARCANE_API MeshData BuildPlane(std::uint32_t subdivisions);

    // Unit cylinder: diameter 1, height 1, axis +Y, spanning [-0.5, +0.5] in Y,
    // with flat caps. `segments` is the radial count.
    //
    // It takes NO length ratio, and that asymmetry with BuildCapsule is the
    // point: a cylinder scaled non-uniformly in Y is still a correct cylinder
    // (its caps are flat discs; nothing distorts), so every cylinder in the
    // family is reachable from this one by scale alone.
    [[nodiscard]] ARCANE_API MeshData BuildCylinder(std::uint32_t segments);

    // Unit-diameter capsule: radius 0.5, axis +Y. `lengthRatio` is TOTAL HEIGHT
    // divided by diameter, so total height == lengthRatio and the cylindrical
    // section is (lengthRatio - 1) long. At exactly 1 the section vanishes and
    // this is a sphere -- legal, and pinned as such.
    //
    // The ratio exists where BuildCylinder needs none because a capsule scaled
    // in Y is NOT a capsule: its hemispherical caps become ellipsoids. That is
    // the test any future shape parameter must pass -- name a family scale
    // cannot reach, or do not exist.
    //
    // `rings` is the arc step count per hemispherical cap; `segments` is the
    // radial count.
    [[nodiscard]] ARCANE_API MeshData BuildCapsule(std::uint32_t rings,
                                                   std::uint32_t segments,
                                                   float lengthRatio);
}
