// F2c Plan 2 Task 9 (spec s8, R5): mesh-thumbnail framing. MaterialPreviewHarvester
// widens from materials-only to also harvest thumbnails for mesh ASSETS (a
// .arcmesh), framed by the mesh's own AABB rather than a fixed sphere camera.
// FrameMeshBounds is the PURE half of that -- the same "pull the pure math out,
// testable without a device" split MeshResidencyBudget takes from NriMeshBufferCache
// (MeshImportWave.hpp's own header comment on this function has the full account).
// [editor] -- CPU-only, no GPU/ImGui/Project/device involved.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Project/MeshImportWave.hpp>

#include <glm/glm.hpp>
#include <glm/geometric.hpp>

#include <cmath>

using namespace Arcane::Editor;

TEST_CASE("mesh thumb framing: the camera looks at the box centre from outside it",
          "[editor]")
{
    Arcane::MeshBounds b; b.min = { -1, -2, -3 }; b.max = { 3, 4, 5 };
    const MeshThumbCamera c = FrameMeshBounds(b, 35.0f);
    CHECK(c.target.x == Catch::Approx(1.0f));   // the centre, per axis
    CHECK(c.target.y == Catch::Approx(1.0f));
    CHECK(c.target.z == Catch::Approx(1.0f));
    // Outside the box's own radius, so the whole thing is in front of the camera.
    const float radius = glm::length((b.max - b.min) * 0.5f);
    CHECK(glm::length(c.eye - c.target) > radius);
    CHECK(c.nearZ > 0.0f);
    CHECK(c.farZ > c.nearZ);
}

TEST_CASE("mesh thumb framing: a bigger box pushes the camera further out", "[editor]")
{
    // The property that makes one framing function work for a 1 m crate and a 40 m
    // building without a per-asset knob.
    Arcane::MeshBounds small; small.min = { -1, -1, -1 }; small.max = { 1, 1, 1 };
    Arcane::MeshBounds big;   big.min   = { -10, -10, -10 }; big.max = { 10, 10, 10 };
    const float dSmall = glm::length(FrameMeshBounds(small, 35.0f).eye
                                   - FrameMeshBounds(small, 35.0f).target);
    const float dBig   = glm::length(FrameMeshBounds(big, 35.0f).eye
                                   - FrameMeshBounds(big, 35.0f).target);
    CHECK(dBig > dSmall * 5.0f);
}

TEST_CASE("mesh thumb framing: a degenerate box yields a finite camera", "[editor]")
{
    // ComputeMeshBounds returns a ZERO box for an empty mesh, deliberately (its own
    // comment: "a caller framing an empty mesh needs a degenerate box it can still
    // build a camera from"). This is that caller, and a NaN here would be undefined
    // behaviour on the GPU rather than a blank thumbnail.
    const MeshThumbCamera c = FrameMeshBounds(Arcane::MeshBounds{}, 35.0f);
    CHECK(std::isfinite(c.eye.x)); CHECK(std::isfinite(c.eye.y)); CHECK(std::isfinite(c.eye.z));
    CHECK(c.nearZ > 0.0f);
    CHECK(c.farZ > c.nearZ);
    // Review fix-round finding: `isfinite` alone does not catch eye == target (a
    // zero-radius box computing a zero camera distance) -- and a coincident eye/
    // target is exactly the input glm::lookAtRH's own normalize(target - eye) turns
    // into the NaN this function exists to rule out. Pin it directly.
    CHECK(glm::length(c.eye - c.target) > 0.0f);
}

TEST_CASE("mesh thumb framing: a narrower FOV pushes the camera further out", "[editor]")
{
    // Same box, 20 vs 60 degrees; the 20-degree distance must be greater -- the
    // check that the fov parameter is USED, not decorative.
    Arcane::MeshBounds b; b.min = { -1, -1, -1 }; b.max = { 1, 1, 1 };
    const float dNarrow = glm::length(FrameMeshBounds(b, 20.0f).eye
                                    - FrameMeshBounds(b, 20.0f).target);
    const float dWide   = glm::length(FrameMeshBounds(b, 60.0f).eye
                                    - FrameMeshBounds(b, 60.0f).target);
    CHECK(dNarrow > dWide);
}
