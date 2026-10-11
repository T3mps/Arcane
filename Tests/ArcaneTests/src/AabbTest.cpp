#include <Arcane/Math/Aabb.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <array>

TEST_CASE("Aabb: Empty is the fold sentinel and unions away", "[aabb]")
{
    const Arcane::Aabb e = Arcane::Aabb::Empty();
    CHECK(e.IsEmpty());
    const Arcane::Aabb b{ glm::vec3(-1.0f), glm::vec3(2.0f) };
    CHECK_FALSE(b.IsEmpty());
    const Arcane::Aabb u = e.Union(b);
    CHECK(u.min == b.min);
    CHECK(u.max == b.max);
}

TEST_CASE("Aabb: FromPoints, Center, Extent, Widened", "[aabb]")
{
    const std::array<glm::vec3, 3> pts{ glm::vec3(1, 2, 3), glm::vec3(-1, 0, 5), glm::vec3(0, 4, -2) };
    const Arcane::Aabb b = Arcane::Aabb::FromPoints(pts);
    CHECK(b.min == glm::vec3(-1, 0, -2));
    CHECK(b.max == glm::vec3(1, 4, 5));
    CHECK(b.Center() == glm::vec3(0, 2, 1.5f));
    CHECK(b.Extent() == glm::vec3(1, 2, 3.5f));
    const Arcane::Aabb w = b.Widened(0.5f);
    CHECK(w.min == glm::vec3(-1.5f, -0.5f, -2.5f));
    CHECK(w.max == glm::vec3(1.5f, 4.5f, 5.5f));
}

TEST_CASE("Aabb: Transformed is the box of the eight transformed corners", "[aabb]")
{
    const Arcane::Aabb unit{ glm::vec3(-0.5f), glm::vec3(0.5f) };
    // 45 degrees about Z: the unit cube's XY footprint grows to sqrt(2)/2 a side.
    const glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)),
                                    glm::radians(45.0f), glm::vec3(0, 0, 1));
    const Arcane::Aabb t = unit.Transformed(m);
    const float h = std::sqrt(2.0f) * 0.5f;
    CHECK(t.min.x == Catch::Approx(10.0f - h));
    CHECK(t.max.x == Catch::Approx(10.0f + h));
    CHECK(t.min.y == Catch::Approx(-h));
    CHECK(t.max.y == Catch::Approx(h));
    CHECK(t.min.z == Catch::Approx(-0.5f));
    CHECK(t.max.z == Catch::Approx(0.5f));
}

TEST_CASE("Aabb: Transformed contains every transformed corner (property)", "[aabb]")
{
    rc::prop("conservative under a random affine", [] {
        const glm::vec3 lo = glm::vec3(*rc::gen::inRange(-50, 50), *rc::gen::inRange(-50, 50), *rc::gen::inRange(-50, 50));
        const glm::vec3 ext = glm::vec3(*rc::gen::inRange(0, 20), *rc::gen::inRange(0, 20), *rc::gen::inRange(0, 20));
        const Arcane::Aabb box{ lo, lo + ext };
        const glm::quat q = glm::normalize(glm::quat(
            float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)),
            float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)) + 0.01f));
        glm::mat4 m = glm::mat4_cast(q);
        m[0] *= float(*rc::gen::inRange(1, 5)); m[1] *= float(*rc::gen::inRange(1, 5)); m[2] *= float(*rc::gen::inRange(1, 5));
        m[3] = glm::vec4(float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)), 1.0f);
        const Arcane::Aabb t = box.Transformed(m);
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec3 c((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z);
            const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
            const float eps = 1e-3f;
            RC_ASSERT(w.x >= t.min.x - eps && w.x <= t.max.x + eps);
            RC_ASSERT(w.y >= t.min.y - eps && w.y <= t.max.y + eps);
            RC_ASSERT(w.z >= t.min.z - eps && w.z <= t.max.z + eps);
        }
    });
}
