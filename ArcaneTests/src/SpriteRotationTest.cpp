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
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

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
    // A sprite is named by asset Guid, so an ordinary generated Guid drives the
    // TEXTURED arm here -- the real key rather than a stand-in for one.

    // Recording mock: captures the Rect/Quad rotation + the Circle submissions.
    // `rects` keeps the FULL quad geometry per submission (both the untextured
    // Rect arm and the textured QuadTextured arm land here) so the pivot/size
    // derivation can be asserted, not just the rotation that flows through it.
    struct MockBatcher final : Arcane::Batcher2D
    {
        struct RectRec
        {
            glm::vec2    pos{0.0f, 0.0f};
            glm::vec2    size{0.0f, 0.0f};
            float        rotation = 0.0f;
            Arcane::Guid textureId{};        // nil == the untextured arm
            glm::vec2    uvMin{0.0f, 0.0f};
            glm::vec2    uvMax{0.0f, 0.0f};
        };

        int                  rectCalls = 0;
        int                  circleCalls = 0;
        float                lastRotation = 0.0f;
        float                lastCircleRadius = 0.0f;
        glm::vec2            lastCircleCenter{0.0f, 0.0f};
        std::vector<RectRec> rects;

        void Begin(uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2 pos, glm::vec2 size,
                  glm::vec2 uvMin, glm::vec2 uvMax,
                  glm::vec4, float rotation) override
        {
            ++rectCalls;
            lastRotation = rotation;
            rects.push_back(RectRec{pos, size, rotation, Arcane::Guid::Nil(),
                                    uvMin, uvMax});
        }
        // THE IDENTITY-CARRYING SUBMISSION -- what RenderSubmissionSystem's
        // textured arm actually calls. Overridden rather than left to the
        // interface default (which forwards to Quad and drops the Guid on the
        // floor), so a case can assert WHICH asset the draw named.
        void QuadTextured(uint16_t, const Arcane::Guid& textureId,
                          glm::vec2 pos, glm::vec2 size,
                          glm::vec2 uvMin, glm::vec2 uvMax,
                          glm::vec4, float rotation) override
        {
            ++rectCalls;
            lastRotation = rotation;
            rects.push_back(RectRec{pos, size, rotation, textureId, uvMin, uvMax});
        }
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4, float rotation) override
        {
            ++rectCalls;
            lastRotation = rotation;
            rects.push_back(RectRec{pos, size, rotation, Arcane::Guid::Nil(),
                                    glm::vec2(0.0f), glm::vec2(1.0f)});
        }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2 center, float radius, glm::vec4) override
        {
            ++circleCalls;
            lastCircleCenter = center;
            lastCircleRadius = radius;
        }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };

    // Spawn a single sprite of `shape` at an identity-rotation world transform
    // and run RenderSubmissionSystem against `mock`. `scale` is the entity's
    // Transform scale, which IS the sizing mechanism now that SpriteRenderer
    // carries no size: a primitive draws a 1x1 m base times the world scale.
    void SubmitSprite(MockBatcher& mock, Arcane::SpriteShape shape, glm::vec2 scale)
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Arcane::RegisterSceneComponents(reg);

        Astra::Entity e = reg.CreateEntity();
        Arcane::Transform lt; lt.position = glm::vec2(200.0f, 150.0f); lt.scale = scale;
        Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp; sp.shape = shape;
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
    CHECK(mock.lastCircleRadius == Approx(20.0f));             // radius = dstSize.x / 2
    CHECK(mock.lastCircleCenter.x == Approx(200.0f));          // centered on the entity
    CHECK(mock.lastCircleCenter.y == Approx(150.0f));
}

TEST_CASE("RenderSubmissionSystem draws a Capsule-shape sprite as a rect + 2 discs",
          "[render]")
{
    MockBatcher mock;
    // dstSize = (2*halfLen + 2r, 2r) = (60, 20) -> r=10, halfLen=20.
    SubmitSprite(mock, Arcane::SpriteShape::Capsule, glm::vec2(60.0f, 20.0f));

    CHECK(mock.rectCalls   == 1);                 // central band
    CHECK(mock.circleCalls == 2);                 // two end discs
    CHECK(mock.lastCircleRadius == Approx(10.0f)); // r = dstSize.y / 2
}

TEST_CASE("RenderSubmissionSystem rotates the sprite quad by the WorldTransform",
          "[render]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // One untextured sprite (nil sprite Guid -> unresolved -> Rect path) at a
    // body rotated by theta. The 40x12 quad comes from the Transform scale.
    const float theta = 0.6f;
    Arcane::Transform lt;
    lt.position = glm::vec2(100.0f, 100.0f);
    lt.rotation = theta;
    lt.scale    = glm::vec2(40.0f, 12.0f);

    Astra::Entity e = reg.CreateEntity();
    Arcane::WorldTransform wt;
    wt.matrix = lt.ToMatrix();                  // no TransformPropagation in this unit test
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp;
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

// ============================================================================
// The sprite ASSET supplies the base size / UVs / pivot; the Transform scales it.
// ============================================================================
TEST_CASE("Sprite with a resolved SpriteTable entry uses derived size and UVs",
          "[render][sprite]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> table;
    const auto gid = Arcane::Guid::Generate();
    Arcane::SpriteEntry e;
    const auto texId = Arcane::Guid::Generate();
    e.textureId  = texId;                 // textured arm: UVs must reach the draw
    e.sizeMeters = glm::vec2(2.0f, 0.5f); // e.g. a 200x50 px sub-rect at ppu 100
    e.uvMin      = glm::vec2(0.25f, 0.5f);
    e.uvMax      = glm::vec2(0.75f, 1.0f);
    table.emplace(gid, e);
    reg.SetResource<Arcane::SpriteTable>(Arcane::SpriteTable{ &table });

    // Transform scale (3,4) at zoom 1 -> (2,0.5) * (3,4) = (6, 2).
    Astra::Entity ent = reg.CreateEntity();
    Arcane::Transform lt; lt.scale = glm::vec2(3.0f, 4.0f);
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(ent, wt);
    Arcane::SpriteRenderer sp;
    sp.sprite = gid;
    reg.AddComponent<Arcane::SpriteRenderer>(ent, sp);

    MockBatcher batcher;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &batcher, glm::vec2(0.0f, 0.0f), 1.0f });
    Arcane::RenderSubmissionSystem sys;
    sys(reg);

    REQUIRE(batcher.rects.size() == 1);
    CHECK(batcher.rects[0].size == glm::vec2(6.0f, 2.0f));
    CHECK(batcher.rects[0].textureId == texId);
    CHECK(batcher.rects[0].uvMin == glm::vec2(0.25f, 0.5f));
    CHECK(batcher.rects[0].uvMax == glm::vec2(0.75f, 1.0f));
}

TEST_CASE("Non-center pivot offsets the quad and survives rotation", "[render][sprite]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // entry: sizeMeters (2,2), pivot (0,0) (top-left). Transform position P,
    // rotation 0, scale 1, zoom 1.
    std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> table;
    const auto gid = Arcane::Guid::Generate();
    Arcane::SpriteEntry e;
    e.sizeMeters = glm::vec2(2.0f, 2.0f);
    e.pivot      = glm::vec2(0.0f, 0.0f);
    table.emplace(gid, e);
    reg.SetResource<Arcane::SpriteTable>(Arcane::SpriteTable{ &table });

    const glm::vec2 P(10.0f, 20.0f);
    Astra::Entity ent = reg.CreateEntity();
    Arcane::Transform lt; lt.position = P;
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(ent, wt);
    Arcane::SpriteRenderer sp;
    sp.sprite = gid;
    reg.AddComponent<Arcane::SpriteRenderer>(ent, sp);

    MockBatcher batcher;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &batcher, glm::vec2(0.0f, 0.0f), 1.0f });
    Arcane::RenderSubmissionSystem sys;
    sys(reg);

    // pivot (0,0) means the PIVOT sits at P and the quad extends +x/+y:
    //   centerOff = (0.5-0.0, 0.5-0.0) * (2,2) = (1,1); dstPos = P + (1,1) - (1,1) = P.
    // Exact equality: cos(0)==1 and sin(0)==0 exactly, so no transcendental
    // error enters the unrotated path.
    REQUIRE(batcher.rects.size() == 1);
    CHECK(batcher.rects[0].pos == P);

    // Same entity rotated 90 deg (pi/2): the center must ORBIT the pivot.
    //   rotated centerOff = R(pi/2)*(1,1) = (c-s, s+c) = (-1, 1)
    //   -> dstPos = P + (-1,1) - (1,1) = P + (-2, 0).
    // Approx here, not ==: cos(half_pi<float>()) is -4.37e-8, not 0.
    Arcane::Transform rot; rot.position = P; rot.rotation = glm::half_pi<float>();
    reg.GetComponent<Arcane::WorldTransform>(ent)->matrix = rot.ToMatrix();
    sys(reg);

    REQUIRE(batcher.rects.size() == 2);
    CHECK(batcher.rects[1].pos.x == Approx(P.x - 2.0f).margin(1e-4));
    CHECK(batcher.rects[1].pos.y == Approx(P.y).margin(1e-4));
    CHECK(batcher.rects[1].rotation == Approx(glm::half_pi<float>()).margin(1e-4));
}
