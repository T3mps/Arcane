#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Scene/ViewTransform.hpp>
#include <cmath>
using Catch::Approx; using Arcane::ViewTransform;

TEST_CASE("Orthographic: +Y up -- a point above the centre lands ABOVE the viewport centre", "[viewtransform]")
{
    const auto v = ViewTransform::Orthographic({0,0}, 5.0f, {800,600});
    const glm::vec3 p = v.WorldToScreen({0, 1, 0});
    CHECK(p.x == Approx(400.0f));
    CHECK(p.y < 300.0f);                      // screen y is DOWN, world +Y is UP
    CHECK(p.y == Approx(300.0f - 60.0f));     // 600 px / (2*5 m) = 60 px per metre
    CHECK(p.z == Approx(0.5f));               // z = 0 sits mid-range of [-1000, 1000]
}

TEST_CASE("Orthographic matches the old affine mapping in X for the same centre and half-height", "[viewtransform]")
{
    // The retired mapping: screen = world * zoom + offset with zoom = H/(2*halfH), offset = viewport/2 - center*zoom.
    const glm::vec2 center{2.5f, -1.0f}; const float halfH = 4.0f; const glm::uvec2 vp{1024, 512};
    const float zoom = 512.0f / (2.0f * halfH);
    const auto v = ViewTransform::Orthographic(center, halfH, vp);
    const glm::vec3 w{7.0f, 3.0f, 0.0f};
    const glm::vec3 s = v.WorldToScreen(w);
    CHECK(s.x == Approx(w.x * zoom + (512.0f - center.x * zoom)));           // X byte-stable
    CHECK(s.y == Approx(256.0f - (w.y - center.y) * zoom));                  // Y flipped about the centre
}

TEST_CASE("Perspective: +Y up too, and the eye looks down -Z by default", "[viewtransform]")
{
    const auto v = ViewTransform::Perspective({0,0,10}, {0,0,0}, {0,1,0}, 60.0f, {800,600}, 0.1f, 1000.0f);
    CHECK_FALSE(v.IsOrthographic());
    const glm::vec3 up = v.WorldToScreen({0, 1, 0});
    const glm::vec3 c  = v.WorldToScreen({0, 0, 0});
    CHECK(c.x == Approx(400.0f)); CHECK(c.y == Approx(300.0f));
    CHECK(up.y < c.y);
    CHECK(v.WorldToScreen({0,0,9.9f}).z < v.WorldToScreen({0,0,0}).z);   // nearer = smaller depth (forward-Z)
}

TEST_CASE("ScreenToRay inverts WorldToScreen in both projections", "[viewtransform]")
{
    const glm::vec3 target{1.5f, -0.75f, -3.0f};
    for (int mode = 0; mode < 2; ++mode)
    {
        const ViewTransform v = mode == 0
            ? ViewTransform::Orthographic({0.5f, 0.25f}, 3.0f, {640, 480})
            : ViewTransform::Perspective({2,3,8}, {0,0,0}, {0,1,0}, 50.0f, {640,480}, 0.1f, 100.0f);
        const glm::vec3 s = v.WorldToScreen(target);
        const Arcane::Ray r = v.ScreenToRay({s.x, s.y});
        // distance from target to the ray line
        const glm::vec3 d = target - r.origin;
        const float along = glm::dot(d, r.direction);
        const float miss = glm::length(d - along * r.direction);
        CHECK(miss < 1e-3f);
        CHECK(along > 0.0f);
        if (mode == 0) CHECK(r.direction == glm::vec3(0,0,-1));   // ortho rays are parallel to the view axis
    }
}

TEST_CASE("AsAffine2D exists only for the orthographic view and carries the Y mirror", "[viewtransform]")
{
    const auto o = ViewTransform::Orthographic({1,2}, 5.0f, {800,600});
    const auto a = o.AsAffine2D();
    REQUIRE(a.has_value());
    CHECK(a->scale.x == Approx(60.0f)); CHECK(a->scale.y == Approx(-60.0f));
    CHECK(a->AngleSign() == -1.0f);
    const glm::vec2 w{3.0f, 4.0f};
    const glm::vec3 s = o.WorldToScreen(glm::vec3(w, 0));
    CHECK(a->Point(w).x == Approx(s.x)); CHECK(a->Point(w).y == Approx(s.y));
    CHECK(a->Unpoint(a->Point(w)).x == Approx(w.x)); CHECK(a->Unpoint(a->Point(w)).y == Approx(w.y));
    CHECK(a->Length(2.0f) == Approx(120.0f));
    const auto p = ViewTransform::Perspective({0,0,10},{0,0,0},{0,1,0},60.0f,{800,600},0.1f,100.0f);
    CHECK_FALSE(p.AsAffine2D().has_value());
    CHECK_FALSE(Arcane::ViewTransform{}.AsAffine2D().has_value());   // zero viewport: no pixel map (never a zero scale)
}
