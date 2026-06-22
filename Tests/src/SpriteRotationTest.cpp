// Sprite rotation: the 2D submission path turns sprites with their entity's
// WorldTransform rotation (so a rotating physics body's sprite rotates with it
// -- previously quads were axis-aligned and ignored rotation, which made
// rotating/compound bodies look "buggy" while the physics was correct).
//
// CPU-only (tag [render], NOT [gpu]): the QuadCorners geometry helper is pure
// math, and RenderSubmissionSystem is driven against a RECORDING mock Batcher2D
// (no graphics device) to assert the rotation flows from the transform to the
// draw call.

#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

using Catch::Approx;

// ============================================================================
// QuadCorners: pure geometry of a (possibly rotated) quad.
// ============================================================================
TEST_CASE("QuadCorners: rotation 0 is the axis-aligned corners", "[render]")
{
    const auto c = Arcane::QuadCorners(glm::vec2(0, 0), glm::vec2(10, 4), 0.0f);
    CHECK(c[0].x == Approx(0.0f));  CHECK(c[0].y == Approx(0.0f));  // TL
    CHECK(c[1].x == Approx(10.0f)); CHECK(c[1].y == Approx(0.0f));  // TR
    CHECK(c[2].x == Approx(10.0f)); CHECK(c[2].y == Approx(4.0f));  // BR
    CHECK(c[3].x == Approx(0.0f));  CHECK(c[3].y == Approx(4.0f));  // BL
}

TEST_CASE("QuadCorners: 90 degrees rotates the edges about the center", "[render]")
{
    const glm::vec2 pos(0, 0), size(10, 4);
    const float ninety = 1.5707963267948966f; // pi/2
    const auto c = Arcane::QuadCorners(pos, size, ninety);

    // The top edge TL->TR was (size.x, 0); after R(90) (= (-vy, vx)) it is
    // (0, size.x). The left edge TL->BL was (0, size.y) -> (-size.y, 0).
    const glm::vec2 topEdge  = c[1] - c[0];
    const glm::vec2 leftEdge = c[3] - c[0];
    CHECK(topEdge.x  == Approx(0.0f).margin(1e-4));
    CHECK(topEdge.y  == Approx(10.0f).margin(1e-4));
    CHECK(leftEdge.x == Approx(-4.0f).margin(1e-4));
    CHECK(leftEdge.y == Approx(0.0f).margin(1e-4));

    // The center (diagonal midpoint) is invariant under rotation.
    const glm::vec2 center = (c[0] + c[2]) * 0.5f;
    CHECK(center.x == Approx(5.0f)); CHECK(center.y == Approx(2.0f));
}

// ============================================================================
// RenderSubmissionSystem passes the WorldTransform rotation to the batcher.
// ============================================================================
namespace
{
    // Recording mock: captures the Rect/Quad rotation + the Circle submissions.
    struct MockBatcher final : Arcane::Batcher2D
    {
        int       rectCalls = 0;
        int       circleCalls = 0;
        float     lastRotation = 0.0f;
        float     lastCircleRadius = 0.0f;
        glm::vec2 lastCircleCenter{0.0f, 0.0f};

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float rotation) override { ++rectCalls; lastRotation = rotation; }
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float rotation) override
        {
            ++rectCalls;
            lastRotation = rotation;
        }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2 center, float radius, glm::vec4) override
        {
            ++circleCalls;
            lastCircleCenter = center;
            lastCircleRadius = radius;
        }
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };

    // Spawn a single sprite of `shape`/`size` at an identity world transform and
    // run RenderSubmissionSystem against `mock`.
    void SubmitSprite(MockBatcher& mock, Arcane::SpriteShape shape, glm::vec2 size)
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Arcane::RegisterSceneComponents(reg);

        Astra::Entity e = reg.CreateEntity();
        Arcane::LocalTransform lt; lt.position = glm::vec2(200.0f, 150.0f);
        Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp; sp.size = size; sp.shape = shape;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

        reg.SetResource<Arcane::RenderContext2D>(
            Arcane::RenderContext2D{ &mock, glm::vec2(0.0f, 0.0f), 1.0f });
        Arcane::RenderSubmissionSystem sys;
        sys(reg);
    }
}

TEST_CASE("RenderSubmissionSystem draws a Circle-shape sprite as a disc", "[render]")
{
    MockBatcher mock;
    SubmitSprite(mock, Arcane::SpriteShape::Circle, glm::vec2(40.0f, 40.0f));

    CHECK(mock.circleCalls == 1);                              // one disc...
    CHECK(mock.rectCalls   == 0);                              // ...not a rect
    CHECK(mock.lastCircleRadius == Approx(20.0f));             // radius = size.x / 2
    CHECK(mock.lastCircleCenter.x == Approx(200.0f));          // centered on the entity
    CHECK(mock.lastCircleCenter.y == Approx(150.0f));
}

TEST_CASE("RenderSubmissionSystem draws a Capsule-shape sprite as a rect + 2 discs",
          "[render]")
{
    MockBatcher mock;
    // size = (2*halfLen + 2r, 2r) = (60, 20) -> r=10, halfLen=20.
    SubmitSprite(mock, Arcane::SpriteShape::Capsule, glm::vec2(60.0f, 20.0f));

    CHECK(mock.rectCalls   == 1);                 // central band
    CHECK(mock.circleCalls == 2);                 // two end discs
    CHECK(mock.lastCircleRadius == Approx(10.0f)); // r = size.y / 2
}

TEST_CASE("RenderSubmissionSystem rotates the sprite quad by the WorldTransform",
          "[render]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // One untextured sprite (textureId 0 -> Rect path) at a body rotated by theta.
    const float theta = 0.6f;
    Arcane::LocalTransform lt;
    lt.position = glm::vec2(100.0f, 100.0f);
    lt.rotation = theta;

    Astra::Entity e = reg.CreateEntity();
    Arcane::WorldTransform wt;
    wt.matrix = lt.ToMatrix();                  // no TransformPropagation in this unit test
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp;
    sp.size = glm::vec2(40.0f, 12.0f);
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

    MockBatcher mock;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &mock, glm::vec2(0.0f, 0.0f), 1.0f });

    Arcane::RenderSubmissionSystem sys;
    sys(reg);

    // The sprite was submitted, rotated by the body's angle (extracted from the
    // WorldTransform matrix). The old axis-aligned path passed rotation 0.
    CHECK(mock.rectCalls == 1);
    CHECK(static_cast<double>(mock.lastRotation) == Approx(static_cast<double>(theta)).margin(1e-4));
}
