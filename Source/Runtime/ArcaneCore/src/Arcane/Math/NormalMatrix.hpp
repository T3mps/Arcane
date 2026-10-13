#pragma once

// NormalMatrixFor -- moved VERBATIM from Render/Nri/nodes/MeshNode.hpp (F3
// plan 1 T5) so the GPU scene's CPU staging (Render/GpuSceneSync.hpp) can
// compute the per-row normal matrix without pulling <NRI.h>. MeshNode.hpp
// includes this and keeps using the name; MeshNodeTest.cpp pins it.
#include <glm/glm.hpp>

#include <cmath>

namespace Arcane
{
    // NormalMatrixFor -- the inverse transpose of `model`'s upper 3x3, which is
    // the transform that keeps a surface normal PERPENDICULAR TO ITS SURFACE
    // under a NON-UNIFORM scale. (Why the upper 3x3 alone is wrong: a surface
    // tangent scales WITH the object, so for dot(normal, tangent) to stay zero
    // after a non-uniform scale, the normal has to scale by the INVERSE along
    // each axis, not the same factor. A uniform scale s*I is the one case
    // where this doesn't matter -- its inverse transpose is (1/s)*I, a
    // positive multiple of I that this file's vs_main normalize() divides
    // straight back out -- which is exactly why the upper-3x3 shortcut looked
    // correct until F1 gave Transform a per-axis glm::vec3 scale the Inspector
    // authors freely: the first non-uniform scale-handle drag on a mesh hits
    // this.)
    //
    // SINGULAR GUARD -- computed, not predicted. `glm::inverse` runs
    // UNCONDITIONALLY below; the guard checks its OUTPUT (all nine elements
    // finite) rather than pre-screening `upper`'s determinant against a fixed
    // threshold. A pre-screen is a CONDITIONING HEURISTIC, not a test for the
    // true singular set, and the difference is not academic: a 3x3
    // determinant scales as s^3 under a uniform scale s, so no fixed
    // threshold is scale-invariant. A too-tight one -- the bug this shape
    // actually had -- lets a small NON-UNIFORM scale (whose determinant can
    // sit anywhere, independent of how ill-conditioned any one axis is) sail
    // past the check and fall through to a normal transform that is silently
    // wrong, reinstating exactly the defect this function exists to fix, with
    // no diagnostic. Checking the RESULT instead is exact by construction:
    // `glm::inverse` divides by the determinant unconditionally, so a genuine
    // singularity (or a non-finite `model`, e.g. a degenerate ancestor in a
    // WorldTransform product) is precisely the input for which the computed
    // inverse transpose comes back non-finite, and nothing else does.
    //
    // WHY GUARD AT ALL: an Inf/NaN normal is undefined behaviour on the GPU
    // rather than a wrong picture -- the same class of guard SceneCamera.hpp's
    // degenerate-basis fallback (ActivePerspectiveSceneCamera) takes, and the
    // same class MeshNode.cpp's own IsFinite/SafeNormalize take for the
    // camera and the light. Identity is the least-wrong answer here: an
    // instance whose model has collapsed a dimension has already lost its
    // geometry to the same degeneracy (its vertices are degenerate too), so
    // there is no "correct" normal direction left to recover -- identity just
    // keeps the pixel shader's arithmetic finite.
    //
    // PURE and header-only, matching SceneCamera.hpp's PerspectiveProjection:
    // no NRI device, no Registry, no MeshNode instance, so ArcaneTests can pin
    // the analytic property directly (MeshNodeTest.cpp).
    //
    // Computed PER INSTANCE inside MeshNode::Record from `model` alone --
    // MeshInstance gains no field for this; see its own comment below.
    [[nodiscard]] inline glm::mat3 NormalMatrixFor(const glm::mat4& model) noexcept
    {
        const glm::mat3 upper = glm::mat3(model);
        const glm::mat3 inverseTranspose = glm::transpose(glm::inverse(upper));
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (!std::isfinite(inverseTranspose[c][r]))
                    return glm::mat3(1.0f);
        return inverseTranspose;
    }
}
