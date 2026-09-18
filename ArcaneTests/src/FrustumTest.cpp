#include <Arcane/Scene/Frustum.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

namespace
{
    Arcane::ViewTransform PerspView()
    {
        return Arcane::ViewTransform::Perspective(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
                                                  60.0f, glm::uvec2{ 800, 600 }, 0.1f, 100.0f);
    }
    Arcane::ViewTransform OrthoView()
    {
        return Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 5.0f, glm::uvec2{ 800, 600 });
    }
}

TEST_CASE("Frustum: every plane is unit-length and the eye's look point is inside (perspective)", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(PerspView());
    for (const Arcane::Plane& p : f.planes)
        CHECK(glm::length(p.n) == Catch::Approx(1.0f).margin(1e-4f));
    const Arcane::Aabb origin{ glm::vec3(-0.1f), glm::vec3(0.1f) };
    CHECK(f.Contains(origin));
}

TEST_CASE("Frustum: boxes behind the eye, beyond far, and far off to the side are rejected", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(PerspView());
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, 20), glm::vec3(1, 1, 22) }));     // behind the eye (eye at z=10, looking -Z)
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, -200), glm::vec3(1, 1, -198) }));  // beyond far
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(500, -1, -1), glm::vec3(502, 1, 1) }));    // far right
    CHECK(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, -50), glm::vec3(1, 1, -48) }));          // ahead, inside far
}

TEST_CASE("Frustum: the orthographic case needs no branch", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(OrthoView());   // half-height 5 m, aspect 4:3 -> half-width 6.667
    CHECK(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1) }));
    CHECK(f.Contains(Arcane::Aabb{ glm::vec3(6.0f, 0, 0), glm::vec3(8.0f, 1, 1) }));       // straddles the right edge
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(7.0f, 0, 0), glm::vec3(8.0f, 1, 1) })); // wholly outside right
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(0, 6.0f, 0), glm::vec3(1, 7.0f, 1) }));  // wholly above
}

TEST_CASE("Frustum: Widened pushes every plane outward by the slack", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(OrthoView());
    const Arcane::Aabb justOutside{ glm::vec3(6.7f, 0, 0), glm::vec3(6.8f, 1, 1) };
    CHECK_FALSE(f.Contains(justOutside));
    CHECK(f.Widened(0.25f).Contains(justOutside));
}

TEST_CASE("Frustum: Contains is conservative -- a box holding an inside point is never rejected (property)", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(PerspView());
    rc::prop("no false negatives", [&] {
        // A point inside the frustum, then a random box around it.
        const float z = -float(*rc::gen::inRange(1, 80));               // ahead of the eye (z=10), inside far
        const float halfW = std::tan(glm::radians(30.0f)) * (10.0f - z) * (800.0f / 600.0f);
        const float halfH = std::tan(glm::radians(30.0f)) * (10.0f - z);
        const glm::vec3 p(halfW * float(*rc::gen::inRange(-90, 90)) / 100.0f,
                          halfH * float(*rc::gen::inRange(-90, 90)) / 100.0f, z);
        const glm::vec3 ext(float(*rc::gen::inRange(0, 30)), float(*rc::gen::inRange(0, 30)), float(*rc::gen::inRange(0, 30)));
        const glm::vec3 off(float(*rc::gen::inRange(0, 100)) / 100.0f, float(*rc::gen::inRange(0, 100)) / 100.0f, float(*rc::gen::inRange(0, 100)) / 100.0f);
        const Arcane::Aabb box{ p - ext * off, p + ext * (glm::vec3(1.0f) - off) };
        RC_ASSERT(f.Contains(box));
    });
}
