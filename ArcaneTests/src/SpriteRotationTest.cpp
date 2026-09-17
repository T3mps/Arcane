// Sprite rotation: the submission path turns sprites with their entity's
// WorldTransform rotation (so a rotating physics body's sprite rotates with it
// -- previously quads were axis-aligned and ignored rotation, which made
// rotating/compound bodies look "buggy" while the physics was correct).
//
// Since F4 plan 1 T5 a sprite is a WORLD quad: RenderSubmissionSystem hands the
// batcher four world-space corners in metres (SpriteWorldQuad) through
// QuadWorld / CircleWorld and projects nothing itself, so every expectation
// below is in world metres, +Y up, read off the recorded corners.
//
// CPU-only (tag [render], NOT [gpu]): the QuadCorners / SpriteWorldQuad
// geometry helpers are pure math, and RenderSubmissionSystem is driven against
// a RECORDING mock Batcher2D (no graphics device) to assert the pose flows from
// the transform to the draw call.

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
#include <Arcane/Render/RenderSystems.hpp>
#include <Arcane/Render/SpriteGeometry.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
// SpriteWorldQuad: the sprite corner rule (F4 plan 1 T5). WORLD-space corners
// TL,TR,BR,BL from the full basis; the image top is the +Y edge; the pivot
// ((0,0) = BOTTOM-left, (0.5,0.5) = centre) anchors the quad at the origin.
// ============================================================================
TEST_CASE("SpriteWorldQuad: the image top is +Y, the pivot anchors the quad, the full basis applies",
          "[render][sprite]")
{
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 0.5f));
    const auto q = Arcane::SpriteWorldQuad(world, {2.0f, 1.0f}, {0.5f, 0.5f});
    CHECK(q.corners[0] == glm::vec3(1.0f, 3.5f, 0.5f));   // TL: left, UP
    CHECK(q.corners[1] == glm::vec3(3.0f, 3.5f, 0.5f));   // TR
    CHECK(q.corners[2] == glm::vec3(3.0f, 2.5f, 0.5f));   // BR
    CHECK(q.corners[3] == glm::vec3(1.0f, 2.5f, 0.5f));   // BL

    // Pivot (0,0) = BOTTOM-left: the origin IS the BL corner and the quad
    // extends +x / +y (up) from it.
    const auto bl = Arcane::SpriteWorldQuad(glm::mat4(1.0f), {2.0f, 2.0f}, {0.0f, 0.0f});
    CHECK(bl.corners[3] == glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(bl.corners[0] == glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(bl.corners[1] == glm::vec3(2.0f, 2.0f, 0.0f));

    // The FULL basis, not the XY plane: a 90-degree X tilt lays the quad into
    // the XZ plane (local +y -> world +z).
    const auto tilted = Arcane::SpriteWorldQuad(
        glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1, 0, 0)), {1, 1}, {0.5f, 0.5f});
    CHECK(std::abs(tilted.corners[0].y) < 1e-5f);
    CHECK(tilted.corners[0].z == Approx(0.5f));
    CHECK(tilted.corners[0].x == Approx(-0.5f));

    // Scale rides the matrix: a (3,4) world scale on a (2,0.5) base is a
    // (6,2) quad about the centre pivot.
    const auto scaled = Arcane::SpriteWorldQuad(
        glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 4.0f, 1.0f)), {2.0f, 0.5f}, {0.5f, 0.5f});
    CHECK(scaled.corners[1] - scaled.corners[0] == glm::vec3(6.0f, 0.0f, 0.0f));
    CHECK(scaled.corners[0] - scaled.corners[3] == glm::vec3(0.0f, 2.0f, 0.0f));
}

// ============================================================================
// RenderSubmissionSystem passes the WorldTransform rotation to the batcher.
// ============================================================================
namespace
{
    // A sprite is named by asset Guid, so an ordinary generated Guid drives the
    // TEXTURED arm here -- the real key rather than a stand-in for one.

    // Recording mock for the WORLD-space surface (F4 plan 1 T5): every
    // QuadWorld lands in `rects` with its full corner geometry (both the
    // untextured arm and the textured/material arms), every CircleWorld in the
    // circle fields. The SCREEN-space primitives (Quad/QuadTextured/Rect/
    // Circle) count into `screenCalls` only: sprites must never reach them
    // again, and a case can assert exactly that.
    struct MockBatcher final : Arcane::Batcher2D
    {
        struct RectRec
        {
            std::array<glm::vec3, 4> corners{};   // TL, TR, BR, BL, world metres
            uint16_t                 materialId = 0;
            Arcane::Guid             textureId{};   // nil == the untextured arm
            glm::vec2                uvMin{0.0f, 0.0f};
            glm::vec2                uvMax{0.0f, 0.0f};

            glm::vec3 centre()  const { return (corners[0] + corners[2]) * 0.5f; }
            glm::vec3 topEdge() const { return corners[1] - corners[0]; }   // local +x, scaled
            glm::vec3 upEdge()  const { return corners[0] - corners[3]; }   // local +y, scaled
            // The quad's turn about +Z: the angle of its top edge.
            float angle() const { return std::atan2(topEdge().y, topEdge().x); }
        };

        int                  rectCalls = 0;
        int                  circleCalls = 0;
        int                  screenCalls = 0;
        float                lastCircleRadius = 0.0f;
        glm::vec3            lastCircleCenter{0.0f};
        std::vector<glm::vec3> circleCenters;
        std::vector<RectRec> rects;
        glm::mat4            viewProjection{1.0f};

        void Begin(uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override
        { ++screenCalls; }
        void QuadTextured(uint16_t, const Arcane::Guid&, glm::vec2, glm::vec2,
                          glm::vec2, glm::vec2, glm::vec4, float) override
        { ++screenCalls; }
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override { ++screenCalls; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2, float, glm::vec4) override { ++screenCalls; }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
        void QuadWorld(uint16_t materialId, const Arcane::Guid& textureId,
                       const std::array<glm::vec3, 4>& corners,
                       glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4) override
        {
            ++rectCalls;
            rects.push_back(RectRec{corners, materialId, textureId, uvMin, uvMax});
        }
        void CircleWorld(glm::vec3 center, glm::vec3, glm::vec3, float radius, glm::vec4) override
        {
            ++circleCalls;
            lastCircleCenter = center;
            lastCircleRadius = radius;
            circleCenters.push_back(center);
        }
        void SetViewProjection(const glm::mat4& vp) override { viewProjection = vp; }
    };

    // RenderContext2D carries a ViewTransform (F4 plan 1 T3), but since T5 the
    // submission system never reads it -- the corners are world metres and the
    // host's SetViewProjection does the projecting. Any view will do; this one
    // is deliberately NOT the pixel identity so a regression that projects
    // through it would move every expectation below.
    Arcane::ViewTransform PixelView()
    {
        return Arcane::ViewTransform::Orthographic({37.0f, -11.0f}, 3.0f, {800u, 600u});
    }

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
        Arcane::Transform lt; lt.position = glm::vec3(200.0f, 150.0f, 0.0f); lt.scale = glm::vec3(scale, 1.0f);
        Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp; sp.shape = shape;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

        reg.SetResource<Arcane::RenderContext2D>(
            Arcane::RenderContext2D{ &mock, PixelView() });
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
    CHECK(mock.screenCalls == 0);                              // ...and in WORLD space
    CHECK(mock.lastCircleRadius == Approx(20.0f));             // radius = worldSize.x / 2, metres
    CHECK(mock.lastCircleCenter.x == Approx(200.0f));          // centred on the entity, world
    CHECK(mock.lastCircleCenter.y == Approx(150.0f));          // +Y up, nothing mirrored
    CHECK(mock.lastCircleCenter.z == Approx(0.0f));
}

TEST_CASE("RenderSubmissionSystem draws a Capsule-shape sprite as a rect + 2 discs",
          "[render]")
{
    MockBatcher mock;
    // worldSize = (2*halfLen + 2r, 2r) = (60, 20) m -> r=10, halfLen=20.
    SubmitSprite(mock, Arcane::SpriteShape::Capsule, glm::vec2(60.0f, 20.0f));

    CHECK(mock.rectCalls   == 1);                 // central band
    CHECK(mock.circleCalls == 2);                 // two end discs
    CHECK(mock.screenCalls == 0);
    CHECK(mock.lastCircleRadius == Approx(10.0f)); // r = worldSize.y / 2
    // The band is (size.x - size.y) x size.y about the centre; the discs sit
    // at centre +/- right * halfLen, in WORLD metres.
    REQUIRE(mock.rects.size() == 1);
    CHECK(mock.rects[0].centre().x == Approx(200.0f));
    CHECK(mock.rects[0].centre().y == Approx(150.0f));
    CHECK(mock.rects[0].topEdge().x == Approx(40.0f));
    CHECK(mock.rects[0].topEdge().y == Approx(0.0f).margin(1e-5));
    CHECK(mock.rects[0].upEdge().y  == Approx(20.0f));
    REQUIRE(mock.circleCenters.size() == 2);
    CHECK(mock.circleCenters[0].x == Approx(220.0f));
    CHECK(mock.circleCenters[0].y == Approx(150.0f));
    CHECK(mock.circleCenters[1].x == Approx(180.0f));
    CHECK(mock.circleCenters[1].y == Approx(150.0f));
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
    lt.position = glm::vec3(100.0f, 100.0f, 0.0f);
    lt.rotation = Arcane::RotationAboutZ(theta);
    lt.scale    = glm::vec3(40.0f, 12.0f, 1.0f);

    Astra::Entity e = reg.CreateEntity();
    Arcane::WorldTransform wt;
    wt.matrix = lt.ToMatrix();                  // no TransformPropagation in this unit test
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp;
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

    MockBatcher mock;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &mock, PixelView() });

    Arcane::RenderSubmissionSystem sys;
    sys(reg);

    // The sprite was submitted as a WORLD quad turned by the body's angle: its
    // top edge (local +x through the full basis) points along theta in the
    // world sense (+Y up; nothing mirrors it), is 40 m long, and the quad is
    // 12 m tall about the entity position.
    REQUIRE(mock.rectCalls == 1);
    CHECK(mock.screenCalls == 0);
    const MockBatcher::RectRec& q = mock.rects[0];
    CHECK(static_cast<double>(q.angle()) == Approx(static_cast<double>(theta)).margin(1e-4));
    CHECK(glm::length(q.topEdge()) == Approx(40.0f));
    CHECK(glm::length(q.upEdge())  == Approx(12.0f));
    CHECK(q.centre().x == Approx(100.0f));
    CHECK(q.centre().y == Approx(100.0f));
    // The up edge is the top edge turned +90 degrees (a right-handed, +Y-up
    // quad -- not a mirrored one).
    const glm::vec3 t = glm::normalize(q.topEdge()), u = glm::normalize(q.upEdge());
    CHECK(u.x == Approx(-t.y).margin(1e-5));
    CHECK(u.y == Approx( t.x).margin(1e-5));
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

    // Transform scale (3,4) -> (2,0.5) * (3,4) = (6, 2) m.
    Astra::Entity ent = reg.CreateEntity();
    Arcane::Transform lt; lt.scale = glm::vec3(3.0f, 4.0f, 1.0f);
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(ent, wt);
    Arcane::SpriteRenderer sp;
    sp.sprite = gid;
    reg.AddComponent<Arcane::SpriteRenderer>(ent, sp);

    MockBatcher batcher;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &batcher, PixelView() });
    Arcane::RenderSubmissionSystem sys;
    sys(reg);

    REQUIRE(batcher.rects.size() == 1);
    CHECK(batcher.rects[0].topEdge() == glm::vec3(6.0f, 0.0f, 0.0f));
    CHECK(batcher.rects[0].upEdge()  == glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(batcher.rects[0].materialId == Arcane::Batcher2D::kMaterialSprite);
    CHECK(batcher.rects[0].textureId == texId);
    // uvMin rides the TL corner (QuadWorld's order): the image top is +Y.
    CHECK(batcher.rects[0].uvMin == glm::vec2(0.25f, 0.5f));
    CHECK(batcher.rects[0].uvMax == glm::vec2(0.75f, 1.0f));
}

TEST_CASE("Non-center pivot offsets the quad and survives rotation", "[render][sprite]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // entry: sizeMeters (2,2), pivot (0,0) (BOTTOM-left, +Y up). Transform
    // position P, rotation 0, scale 1.
    std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> table;
    const auto gid = Arcane::Guid::Generate();
    Arcane::SpriteEntry e;
    e.sizeMeters = glm::vec2(2.0f, 2.0f);
    e.pivot      = glm::vec2(0.0f, 0.0f);
    table.emplace(gid, e);
    reg.SetResource<Arcane::SpriteTable>(Arcane::SpriteTable{ &table });

    const glm::vec2 P(10.0f, 20.0f);
    Astra::Entity ent = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(P, 0.0f);
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(ent, wt);
    Arcane::SpriteRenderer sp;
    sp.sprite = gid;
    reg.AddComponent<Arcane::SpriteRenderer>(ent, sp);

    MockBatcher batcher;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &batcher, PixelView() });
    Arcane::RenderSubmissionSystem sys;
    sys(reg);

    // pivot (0,0) means the PIVOT sits at P as the quad's BOTTOM-LEFT corner
    // and the quad extends +x / +y (up) from it IN WORLD:
    //   BL = P = (10,20), TL = (10,22), TR = (12,22), BR = (12,20).
    // Exact equality: the unrotated matrix is exact, so no transcendental
    // error enters this path.
    REQUIRE(batcher.rects.size() == 1);
    CHECK(batcher.rects[0].corners[3] == glm::vec3(10.0f, 20.0f, 0.0f));   // BL == pivot
    CHECK(batcher.rects[0].corners[0] == glm::vec3(10.0f, 22.0f, 0.0f));   // TL
    CHECK(batcher.rects[0].corners[1] == glm::vec3(12.0f, 22.0f, 0.0f));   // TR
    CHECK(batcher.rects[0].corners[2] == glm::vec3(12.0f, 20.0f, 0.0f));   // BR

    // Same entity rotated 90 deg (pi/2) about +Z: the quad must ORBIT the
    // pivot, which stays put at P. R(pi/2)(x,y) = (-y, x):
    //   TL = P + R(0,2) = (8,20), TR = P + R(2,2) = (8,22), BR = P + R(2,0) = (10,22).
    // Approx here, not ==: cos(half_pi<float>()) is -4.37e-8, not 0.
    Arcane::Transform rot; rot.position = glm::vec3(P, 0.0f);
    rot.rotation = Arcane::RotationAboutZ(glm::half_pi<float>());
    reg.GetComponent<Arcane::WorldTransform>(ent)->matrix = rot.ToMatrix();
    sys(reg);

    REQUIRE(batcher.rects.size() == 2);
    const MockBatcher::RectRec& q = batcher.rects[1];
    CHECK(q.corners[3].x == Approx(10.0f).margin(1e-4));   // BL still the pivot
    CHECK(q.corners[3].y == Approx(20.0f).margin(1e-4));
    CHECK(q.corners[0].x == Approx(8.0f).margin(1e-4));    // TL
    CHECK(q.corners[0].y == Approx(20.0f).margin(1e-4));
    CHECK(q.corners[1].x == Approx(8.0f).margin(1e-4));    // TR
    CHECK(q.corners[1].y == Approx(22.0f).margin(1e-4));
    CHECK(q.corners[2].x == Approx(10.0f).margin(1e-4));   // BR
    CHECK(q.corners[2].y == Approx(22.0f).margin(1e-4));
    CHECK(q.angle() == Approx(glm::half_pi<float>()).margin(1e-4));
}
