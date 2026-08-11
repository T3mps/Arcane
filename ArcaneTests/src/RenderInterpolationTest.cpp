// Epic 04.2 render interpolation: pure math (Lerp / shortest-arc AngleLerp),
// PhysicsSystem previous-pose capture, and the two render consumers
// (DrawPhysicsDebug overlay + RenderSubmissionSystem sprites) driven against a
// recording mock Batcher2D. CPU-only (tag [interp], never [gpu]).

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <memory>
#include <utility>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>

using Catch::Approx;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
}

TEST_CASE("Lerp is the standard affine blend", "[interp]")
{
    CHECK(Arcane::Lerp(0.0f, 10.0f, 0.0f) == Approx(0.0f));
    CHECK(Arcane::Lerp(0.0f, 10.0f, 1.0f) == Approx(10.0f));
    CHECK(Arcane::Lerp(2.0f, 6.0f, 0.5f) == Approx(4.0f));
}

TEST_CASE("AngleLerp takes the shortest arc across the pi wrap", "[interp]")
{
    // 350deg -> 10deg: shortest arc is +20deg through 0, NOT -340deg.
    const float a = 350.0f * kPi / 180.0f;
    const float b =  10.0f * kPi / 180.0f;
    const float mid = Arcane::AngleLerp(a, b, 0.5f);
    // Midpoint is 360deg == 0deg (mod 2pi). Compare via sin/cos to dodge the wrap.
    CHECK(std::sin(mid) == Approx(0.0f).margin(1e-5));
    CHECK(std::cos(mid) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("AngleLerp endpoints and non-wrapping case", "[interp]")
{
    CHECK(Arcane::AngleLerp(0.3f, 1.1f, 0.0f) == Approx(0.3f));
    CHECK(Arcane::AngleLerp(0.3f, 1.1f, 1.0f) == Approx(1.1f));
    CHECK(Arcane::AngleLerp(0.2f, 0.8f, 0.5f) == Approx(0.5f)); // no wrap: plain midpoint
}

TEST_CASE("PhysicsInterpBuffer captures the pre-step pose each fixed step", "[interp]")
{
    namespace P = Manifold2D::Physics;
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    P::WorldDef wd; wd.gravityY = 10.0f;
    reg.SetResource(Arcane::PhysicsResource{
        std::make_unique<P::PhysicsWorld>(wd), {} });
    reg.SetResource(Arcane::PhysicsInterpBuffer{});   // opt in to capture

    // One dynamic circle free-falling from the origin.
    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec2(0.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = P::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col;
    { Arcane::Fixture fx; fx.kind = P::ShapeKind::Circle; fx.radius = 0.5f;
      col.fixtures.push_back(fx); }
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

    constexpr float kDt = 1.0f / 60.0f;
    Arcane::PhysicsSystem physics(kDt);

    // Step once: creates the body, captures prev (== the initial pose (0,0)), steps.
    physics(reg);
    const P::BodyHandle h = reg.GetComponent<Arcane::PhysicsBodyRef>(e)->handle;
    const P::PhysicsWorld& world = *reg.GetResource<Arcane::PhysicsResource>()->world;
    const P::Vec2 afterStep1 = world.Position(h);   // pose after step 1

    // Step again: prev must now hold the post-step-1 pose (the pre-step-2 state).
    physics(reg);

    const auto* buf = reg.GetResource<Arcane::PhysicsInterpBuffer>();
    REQUIRE(buf->captured);
    REQUIRE(h.index < buf->prev.size());
    const Arcane::InterpPose& pp = buf->prev[h.index];
    CHECK(pp.generation == h.generation);
    CHECK(pp.position.y == Approx(static_cast<float>(afterStep1.y)));
    CHECK(world.Position(h).y > pp.position.y);   // it kept falling after the capture
}

namespace
{
    // Recording Batcher2D: captures the last Circle center + count. All other
    // primitive overrides are no-ops (the tests disable every non-outline overlay).
    struct RecBatcher final : Arcane::Batcher2D
    {
        int       circleCalls = 0;
        glm::vec2 lastCircleCenter{0.0f, 0.0f};
        int       rectCalls = 0;
        float     lastRotation = 0.0f;
        glm::vec2 lastRectPos{0.0f, 0.0f};    // top-left origin of the last Rect/Quad
        glm::vec2 lastRectSize{0.0f, 0.0f};

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2 p, glm::vec2 sz, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float rot) override
        { ++rectCalls; lastRotation = rot; lastRectPos = p; lastRectSize = sz; }
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2 p, glm::vec2 sz, glm::vec4, float rot) override
        { ++rectCalls; lastRotation = rot; lastRectPos = p; lastRectSize = sz; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2 c, float, glm::vec4) override
        { ++circleCalls; lastCircleCenter = c; }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        void RemoveTexture(nvrhi::ITexture*) override {}
        Arcane::Batch2DStats Stats() const override { return {}; }

        // Center of the last Rect/Quad (Batcher2D quads are top-left origin).
        glm::vec2 lastRectCenter() const { return lastRectPos + lastRectSize * 0.5f; }
    };
}

TEST_CASE("DrawPhysicsDebug interpolates the body outline by alpha", "[interp]")
{
    namespace P = Manifold2D::Physics;
    P::WorldDef wd; P::PhysicsWorld world(wd);

    // One dynamic circle at (10, 0). No stepping -> velocity 0 (no velocity ray).
    P::BodyDef bd; bd.type = P::BodyType::Dynamic;
    bd.position = P::Vec2(P::Real(10), P::Real(0));
    bd.shape = P::MakeCircle(P::Real(0.5)); bd.density = P::Real(1);
    const P::BodyHandle h = world.AddBody(bd);

    // Synthesized previous pose at (0, 0), same generation as the live slot.
    Arcane::PhysicsInterpBuffer buf;
    buf.prev.resize(world.Count());
    buf.prev[h.index] = Arcane::InterpPose{ glm::vec2(0.0f, 0.0f), 0.0f, h.generation };
    buf.captured = true;

    Arcane::PhysicsDebugDrawOptions opts;
    opts.drawContacts = opts.drawAabbs = opts.drawVelocities = false;
    opts.drawComMarkers = opts.drawOrientations = false;   // isolate the outline
    opts.interp = &buf;
    opts.alpha  = 0.5f;                                     // halfway 0 -> 10

    RecBatcher rec;
    Arcane::DrawPhysicsDebug(world, rec, opts);

    REQUIRE(rec.circleCalls == 1);
    CHECK(rec.lastCircleCenter.x == Approx(5.0f));   // lerp(0, 10, 0.5) at identity zoom
    CHECK(rec.lastCircleCenter.y == Approx(0.0f));

    // Generation mismatch (stale slot) -> no interp, drawn at the current pose.
    buf.prev[h.index].generation = h.generation + 1u;
    RecBatcher rec2;
    Arcane::DrawPhysicsDebug(world, rec2, opts);
    CHECK(rec2.lastCircleCenter.x == Approx(10.0f));
}

TEST_CASE("RenderSubmissionSystem interpolates a sprite by PreviousTransform + alpha", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // Current world pose at x=10; previous local pose at x=0. Untextured Rect
    // sprite: nil .arcsprite -> a 1x1 m quad, so the scale IS the 4x4 size.
    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec2(10.0f, 0.0f); lt.scale = glm::vec2(4.0f, 4.0f);
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp;
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
    Arcane::PreviousTransform prev; prev.position = glm::vec2(0.0f, 0.0f); prev.rotation = 0.0f;
    reg.AddComponent<Arcane::PreviousTransform>(e, prev);

    RecBatcher rec;
    Arcane::RenderContext2D ctx{ &rec, glm::vec2(0.0f, 0.0f), 1.0f, 0.5f };  // alpha 0.5
    // MSVC: SetResource<T> takes T&&; the named lvalue `ctx` needs std::move
    // (the brief's literal `SetResource<...>(ctx)` does not bind).
    reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));
    Arcane::RenderSubmissionSystem{}(reg);

    // Untextured -> Rect path (top-left origin). The quad is centered on the
    // interpolated screen position: center x = lerp(0, 10, 0.5) = 5 at identity zoom.
    REQUIRE(rec.rectCalls == 1);
    CHECK(rec.lastRectCenter().x == Approx(5.0f));
    CHECK(rec.lastRectCenter().y == Approx(0.0f));
    CHECK(rec.lastRotation == Approx(0.0f).margin(1e-5));
}

TEST_CASE("RenderSubmissionSystem interpolates sprite rotation on the shortest arc", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec2(0.0f, 0.0f);
    lt.rotation = 10.0f * kPi / 180.0f;            // current 10deg
    lt.scale    = glm::vec2(4.0f, 4.0f);           // the 4x4 quad, sized by scale
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp;
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
    Arcane::PreviousTransform prev; prev.rotation = 350.0f * kPi / 180.0f;  // previous 350deg
    reg.AddComponent<Arcane::PreviousTransform>(e, prev);

    RecBatcher rec;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &rec, glm::vec2(0.0f, 0.0f), 1.0f, 0.5f });
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.rectCalls == 1);
    // Shortest arc 350 -> 10 midpoint is 0deg, NOT 180deg.
    CHECK(std::sin(rec.lastRotation) == Approx(0.0f).margin(1e-5));
    CHECK(std::cos(rec.lastRotation) == Approx(1.0f).margin(1e-5));
}
