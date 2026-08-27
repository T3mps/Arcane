// Task 6 (Phase 4): pins Arcane/Render/MeshBuilder.hpp's two procedural
// generators. Pure geometry -- no device, no NRI, no Registry -- exactly the
// "device-less, no device" contract the header itself states.
//
// The properties this file MUST hold (task-6-brief.md, Step 1):
//   1. BuildCube: 24 vertices / 36 indices (per-face normals need split
//      vertices; a cube cannot be 8 vertices here).
//   2. Every index (cube and sphere) is in range.
//   3. Every normal (cube and sphere) is unit length.
//   4. Every cube vertex lies on the box of the given size.
//   5. Every sphere vertex is `radius` from the origin within 1e-5.
//   6. SPHERE WINDING IS CONSISTENT: every triangle's geometric normal
//      (cross of its own edges) agrees with its vertex normals -- the
//      assertion that actually catches a flipped triangle. A dedicated test
//      below proves this has teeth by flipping one triangle and showing the
//      same check reports disagreement.
//
// F2a Task 1 extends this file: BuildPlane / BuildCylinder / BuildCapsule /
// ComputeMeshBounds (task-1-brief.md, Step 1), appended below rather than
// interleaved so the Task 6 coverage above stays untouched -- BuildCube and
// BuildUvSphere are shipped and pixel-verified on the GPU.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Arcane/Render/MeshBuilder.hpp>

#include <glm/glm.hpp>
#include <glm/geometric.hpp>

#include <cstdint>
#include <utility>

using Catch::Approx;
using Arcane::BuildCube;
using Arcane::BuildUvSphere;
using Arcane::MeshData;
using Arcane::MeshVertex;

namespace
{
    // The geometric (flat, per-triangle) normal, from vertex POSITIONS alone
    // -- deliberately independent of whatever normals the builder stored, so
    // comparing it against those stored normals is a genuine cross-check and
    // not a tautology.
    glm::vec3 GeometricNormal(const MeshVertex& a, const MeshVertex& b, const MeshVertex& c)
    {
        return glm::normalize(glm::cross(b.position - a.position, c.position - a.position));
    }
}

// ---------------------------------------------------------------------------
// Required property 1: 24 vertices / 36 indices, exactly.
// ---------------------------------------------------------------------------

TEST_CASE("BuildCube: 24 vertices and 36 indices -- split-per-face, not 8 shared corners",
          "[render][mesh]")
{
    const MeshData cube = BuildCube(2.0f);
    CHECK(cube.vertices.size() == 24);
    CHECK(cube.indices.size() == 36);
}

// ---------------------------------------------------------------------------
// Required property 2 (cube half): every index is in range.
// ---------------------------------------------------------------------------

TEST_CASE("BuildCube: every index is in range", "[render][mesh]")
{
    const MeshData cube = BuildCube(3.0f);
    for (std::uint32_t idx : cube.indices)
        CHECK(idx < cube.vertices.size());
}

// ---------------------------------------------------------------------------
// Required property 3 (cube half): every normal is unit length.
// ---------------------------------------------------------------------------

TEST_CASE("BuildCube: every normal is unit length", "[render][mesh]")
{
    const MeshData cube = BuildCube(3.0f);
    for (const MeshVertex& v : cube.vertices)
        CHECK(glm::length(v.normal) == Approx(1.0f).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// Required property 4: every cube vertex lies on the box of the given size.
// ---------------------------------------------------------------------------

TEST_CASE("BuildCube: every vertex lies on the box of the given size", "[render][mesh]")
{
    const float size = 3.0f;
    const float half = size * 0.5f;
    const MeshData cube = BuildCube(size);
    REQUIRE(cube.vertices.size() == 24);
    for (const MeshVertex& v : cube.vertices)
    {
        // Every emitted cube vertex is a true corner: all three coordinates
        // sit at +-half, not merely "inside" the box.
        CHECK(std::fabs(v.position.x) == Approx(half).margin(1e-5f));
        CHECK(std::fabs(v.position.y) == Approx(half).margin(1e-5f));
        CHECK(std::fabs(v.position.z) == Approx(half).margin(1e-5f));
    }
}

// ---------------------------------------------------------------------------
// Beyond the four pinned cube properties: the WINDING contract MeshBuilder.hpp
// states applies to every emitted triangle, cube included -- cheap to check
// and it directly exercises the (normal, u, v) basis table's u x v == normal
// invariant for all six faces, not just the sphere's.
// ---------------------------------------------------------------------------

TEST_CASE("BuildCube: every triangle's geometric normal agrees with its vertex normals",
          "[render][mesh]")
{
    const MeshData cube = BuildCube(2.0f);
    REQUIRE(cube.indices.size() % 3 == 0);
    for (std::size_t i = 0; i < cube.indices.size(); i += 3)
    {
        const MeshVertex& a = cube.vertices[cube.indices[i + 0]];
        const MeshVertex& b = cube.vertices[cube.indices[i + 1]];
        const MeshVertex& c = cube.vertices[cube.indices[i + 2]];
        const glm::vec3 faceNormal = GeometricNormal(a, b, c);
        INFO("triangle starting at index " << i);
        // A cube face is flat, so this should be an exact match (a.normal
        // == b.normal == c.normal == faceNormal for every face), not merely
        // "agrees" -- checked at full precision, not just same-hemisphere.
        CHECK(glm::dot(faceNormal, a.normal) == Approx(1.0f).margin(1e-4f));
        CHECK(glm::dot(faceNormal, b.normal) == Approx(1.0f).margin(1e-4f));
        CHECK(glm::dot(faceNormal, c.normal) == Approx(1.0f).margin(1e-4f));
    }
}

// ---------------------------------------------------------------------------
// Required property 2 (sphere half): every index is in range.
// ---------------------------------------------------------------------------

TEST_CASE("BuildUvSphere: every index is in range", "[render][mesh]")
{
    const MeshData sphere = BuildUvSphere(2.5f, 8, 12);
    for (std::uint32_t idx : sphere.indices)
        CHECK(idx < sphere.vertices.size());
}

// ---------------------------------------------------------------------------
// Required property 3 (sphere half): every normal is unit length.
// ---------------------------------------------------------------------------

TEST_CASE("BuildUvSphere: every normal is unit length", "[render][mesh]")
{
    const MeshData sphere = BuildUvSphere(2.5f, 8, 12);
    for (const MeshVertex& v : sphere.vertices)
        CHECK(glm::length(v.normal) == Approx(1.0f).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// Required property 5: every sphere vertex is `radius` from the origin,
// within 1e-5 -- poles included (theta = 0 / pi collapses phi's contribution
// to zero, not to something merely close to zero).
// ---------------------------------------------------------------------------

TEST_CASE("BuildUvSphere: every vertex is radius from the origin, within 1e-5",
          "[render][mesh]")
{
    const float radius = 2.5f;
    const MeshData sphere = BuildUvSphere(radius, 8, 12);
    REQUIRE(sphere.vertices.size() == (8 + 1) * (12 + 1));
    for (const MeshVertex& v : sphere.vertices)
        CHECK(glm::length(v.position) == Approx(radius).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// Required property 6, THE ONE THAT MATTERS: sphere winding is consistent.
// Every triangle's geometric normal (from its vertex POSITIONS, via cross
// product -- see GeometricNormal above) agrees with its vertex normals. A
// dot product > 0 means "same hemisphere, i.e. not flipped"; a genuinely
// flipped triangle would report a strongly NEGATIVE dot here (proven by the
// next test case, which flips one on purpose).
//
// Threshold 0.5 (not 0.9+): chosen to comfortably tolerate the geometric
// normal's divergence from the smooth vertex normal on a real, non-trivial
// tessellation (8 rings x 12 segments here) without being so tight it starts
// testing tessellation fineness instead of winding. A flip fails this by a
// wide margin (empirically ~ -0.87 to -0.98 across the triangles checked
// while writing this file), so 0.5 has real teeth in both directions.
// ---------------------------------------------------------------------------

TEST_CASE("BuildUvSphere: winding is consistent -- every triangle's geometric normal "
          "agrees with its vertex normals",
          "[render][mesh]")
{
    const MeshData sphere = BuildUvSphere(1.0f, 8, 12);
    REQUIRE(sphere.indices.size() % 3 == 0);
    REQUIRE(sphere.indices.size() > 0);
    for (std::size_t i = 0; i < sphere.indices.size(); i += 3)
    {
        const MeshVertex& a = sphere.vertices[sphere.indices[i + 0]];
        const MeshVertex& b = sphere.vertices[sphere.indices[i + 1]];
        const MeshVertex& c = sphere.vertices[sphere.indices[i + 2]];
        const glm::vec3 faceNormal = GeometricNormal(a, b, c);
        INFO("triangle starting at index " << i);
        CHECK(glm::dot(faceNormal, a.normal) > 0.5f);
        CHECK(glm::dot(faceNormal, b.normal) > 0.5f);
        CHECK(glm::dot(faceNormal, c.normal) > 0.5f);
    }
}

// ---------------------------------------------------------------------------
// Evidence the check above has teeth: deliberately flip one triangle's
// winding (swap two of its three indices, the exact defect class a wrong
// triangulation order in BuildUvSphere would produce) and show the SAME
// geometric-normal-vs-vertex-normal check now reports clear disagreement,
// not a pass.
// ---------------------------------------------------------------------------

TEST_CASE("BuildUvSphere: flipping a triangle's winding is caught by the geometric-normal "
          "agreement check",
          "[render][mesh]")
{
    MeshData sphere = BuildUvSphere(1.0f, 8, 12);
    REQUIRE(sphere.indices.size() >= 3);

    // As built, the first triangle agrees with its vertex normals (same
    // check as the test above, isolated to one triangle).
    {
        const MeshVertex& a = sphere.vertices[sphere.indices[0]];
        const MeshVertex& b = sphere.vertices[sphere.indices[1]];
        const MeshVertex& c = sphere.vertices[sphere.indices[2]];
        REQUIRE(glm::dot(GeometricNormal(a, b, c), a.normal) > 0.5f);
    }

    // Flip it: swap the second and third index of the triangle, which
    // negates the cross product exactly (cross(v2-v0, v1-v0) == -cross(v1-v0,
    // v2-v0)).
    std::swap(sphere.indices[1], sphere.indices[2]);

    const MeshVertex& a = sphere.vertices[sphere.indices[0]];
    const MeshVertex& b = sphere.vertices[sphere.indices[1]];
    const MeshVertex& c = sphere.vertices[sphere.indices[2]];
    const glm::vec3 flippedNormal = GeometricNormal(a, b, c);

    // Now clearly disagrees -- the property this whole test file exists to
    // pin actually distinguishes correct winding from flipped winding.
    CHECK(glm::dot(flippedNormal, a.normal) < -0.5f);
    CHECK(glm::dot(flippedNormal, b.normal) < -0.5f);
    CHECK(glm::dot(flippedNormal, c.normal) < -0.5f);
}

// ---------------------------------------------------------------------------
// F2a Task 1: BuildPlane / BuildCylinder / BuildCapsule / ComputeMeshBounds
// (task-1-brief.md, Step 1). Verbatim from the brief.
// ---------------------------------------------------------------------------

using namespace Arcane;
using Catch::Matchers::WithinAbs;

namespace
{
    // The winding contract from MeshBuilder.hpp, as a predicate: for every
    // triangle, the geometric normal must agree with the averaged vertex
    // normal. A generator that emits a face backwards is INVISIBLE under
    // CullMode::BACK, and nothing else in the suite would notice.
    bool WindingIsOutward(const MeshData& m)
    {
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
        {
            const MeshVertex& a = m.vertices[m.indices[i + 0]];
            const MeshVertex& b = m.vertices[m.indices[i + 1]];
            const MeshVertex& c = m.vertices[m.indices[i + 2]];
            const glm::vec3 geo = glm::cross(b.position - a.position, c.position - a.position);
            if (glm::length(geo) < 1e-12f)
                return false;                       // a degenerate triangle is a bug too
            const glm::vec3 avg = a.normal + b.normal + c.normal;
            if (glm::dot(glm::normalize(geo), glm::normalize(avg)) <= 0.0f)
                return false;
        }
        return true;
    }

    MeshBounds BoundsOf(const MeshData& m) { return ComputeMeshBounds(m); }
}

TEST_CASE("BuildPlane is a unit XZ plane with a +Y normal", "[mesh]")
{
    const MeshData p = BuildPlane(1);
    REQUIRE(p.indices.size() == 6);          // one quad
    REQUIRE(p.vertices.size() == 4);

    const MeshBounds b = BoundsOf(p);
    CHECK_THAT(b.min.x, WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(b.max.x, WithinAbs( 0.5f, 1e-6f));
    CHECK_THAT(b.min.z, WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(b.max.z, WithinAbs( 0.5f, 1e-6f));
    CHECK_THAT(b.min.y, WithinAbs(0.0f, 1e-6f));   // flat: zero thickness
    CHECK_THAT(b.max.y, WithinAbs(0.0f, 1e-6f));

    for (const MeshVertex& v : p.vertices)
    {
        CHECK_THAT(v.normal.y, WithinAbs(1.0f, 1e-6f));
    }
    CHECK(WindingIsOutward(p));

    // Subdivision multiplies quads per AXIS, so 3 -> 9 quads -> 54 indices.
    const MeshData p3 = BuildPlane(3);
    CHECK(p3.indices.size() == 54);
    CHECK(WindingIsOutward(p3));
}

TEST_CASE("BuildCylinder is a unit-diameter, unit-height Y-axis cylinder", "[mesh]")
{
    const MeshData c = BuildCylinder(16);
    const MeshBounds b = BoundsOf(c);
    CHECK_THAT(b.min.y, WithinAbs(-0.5f, 1e-6f));
    CHECK_THAT(b.max.y, WithinAbs( 0.5f, 1e-6f));
    CHECK_THAT(b.max.x, WithinAbs( 0.5f, 1e-4f));   // radius 0.5, sampled at 16 segments
    CHECK_THAT(b.min.x, WithinAbs(-0.5f, 1e-4f));
    CHECK(WindingIsOutward(c));
}

TEST_CASE("BuildCapsule's total height IS its length ratio", "[mesh]")
{
    // lengthRatio 2 -> total height 2 (Unity's capsule proportions), diameter 1.
    const MeshData c = BuildCapsule(8, 16, 2.0f);
    const MeshBounds b = BoundsOf(c);
    CHECK_THAT(b.max.y - b.min.y, WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(b.max.x, WithinAbs(0.5f, 1e-3f));
    CHECK(WindingIsOutward(c));

    // ratio 1 degenerates to a sphere -- LEGAL, and the height must collapse
    // to the diameter rather than going negative.
    const MeshData s = BuildCapsule(8, 16, 1.0f);
    const MeshBounds sb = BoundsOf(s);
    CHECK_THAT(sb.max.y - sb.min.y, WithinAbs(1.0f, 1e-4f));
    CHECK(WindingIsOutward(s));
}

TEST_CASE("ComputeMeshBounds covers every vertex, and survives an empty mesh", "[mesh]")
{
    const MeshData cube = BuildCube(1.0f);
    const MeshBounds b = ComputeMeshBounds(cube);
    for (const MeshVertex& v : cube.vertices)
    {
        CHECK(v.position.x >= b.min.x - 1e-6f);
        CHECK(v.position.x <= b.max.x + 1e-6f);
        CHECK(v.position.y >= b.min.y - 1e-6f);
        CHECK(v.position.y <= b.max.y + 1e-6f);
        CHECK(v.position.z >= b.min.z - 1e-6f);
        CHECK(v.position.z <= b.max.z + 1e-6f);
    }

    // Zero, not an inverted-infinity sentinel: a caller framing an empty mesh
    // must get a degenerate box it can still build a camera from.
    const MeshBounds empty = ComputeMeshBounds(MeshData{});
    CHECK_THAT(empty.min.x, WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(empty.max.x, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("the existing generators still satisfy the winding contract", "[mesh]")
{
    // A regression net for Task 1's edits -- these two shipped in Phase 4 and
    // their winding is what the [pixel] case already proved on the GPU.
    CHECK(WindingIsOutward(BuildCube(1.0f)));
    CHECK(WindingIsOutward(BuildUvSphere(0.5f, 8, 16)));
}
