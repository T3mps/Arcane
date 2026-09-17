// Epic 04.2 render interpolation: pure math (Lerp / shortest-arc AngleLerp for
// the physics-side 2D InterpPose, PhysicsInterpBuffer's slot poses for the
// sprite path), PhysicsSystem previous-pose capture, and the two render consumers
// (DrawPhysicsDebug overlay + RenderSubmissionSystem sprites) driven against a
// recording mock Batcher2D. CPU-only (tag [interp], never [gpu]).

#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/RenderSystems.hpp>
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

    // F4 plan 1 T3: RenderContext2D carries a ViewTransform. This one's
    // Affine2D is the PIXEL IDENTITY up to the mirror -- scale (1, -1), offset
    // (0, 0): Point(w) = (w.x, -w.y) -- so a world x lands on the same canvas x
    // the old identity camera produced (the assertions below read x only, or
    // measure y endpoints through the same submit).
    Arcane::ViewTransform PixelView()
    {
        return Arcane::ViewTransform::Orthographic({500.0f, -500.0f}, 500.0f, {1000u, 1000u});
    }
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
    Arcane::Transform lt; lt.position = glm::vec3(0.0f);
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
    const Arcane::InterpSlot* slot = buf->slotOf.TryGet(e);
    REQUIRE(slot != nullptr);
    CHECK(slot->index == h.index);
    CHECK(slot->generation == h.generation);
    CHECK(pp.position.y == Approx(static_cast<float>(afterStep1.y)));
    CHECK(world.Position(h).y > pp.position.y);   // it kept falling after the capture
}

namespace
{
    // Recording Batcher2D. The overlay (DrawPhysicsDebug) still draws in PIXELS
    // through Circle(); sprites submit WORLD quads through QuadWorld /
    // CircleWorld (F4 plan 1 T5), so both surfaces record. All other primitive
    // overrides are no-ops (the tests disable every non-outline overlay).
    struct RecBatcher final : Arcane::Batcher2D
    {
        // Screen-space surface (the overlay's).
        int       circleCalls = 0;
        glm::vec2 lastCircleCenter{0.0f, 0.0f};
        int       screenQuadCalls = 0;   // Rect/Quad: sprites must NOT land here any more

        // World-space surface (the sprites').
        int                       quadCalls = 0;
        std::array<glm::vec3, 4>  lastCorners{};   // TL, TR, BR, BL in world metres
        int                       worldCircleCalls = 0;
        glm::vec3                 lastWorldCircleCenter{0.0f};
        glm::mat4                 viewProjection{1.0f};

        void Begin(uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override
        { ++screenQuadCalls; }
        void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override
        { ++screenQuadCalls; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2 c, float, glm::vec4) override
        { ++circleCalls; lastCircleCenter = c; }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
        void QuadWorld(uint16_t, const Arcane::Guid&, const std::array<glm::vec3, 4>& corners,
                       glm::vec2, glm::vec2, glm::vec4) override
        { ++quadCalls; lastCorners = corners; }
        void CircleWorld(glm::vec3 c, glm::vec3, glm::vec3, float, glm::vec4) override
        { ++worldCircleCalls; lastWorldCircleCenter = c; }
        void SetViewProjection(const glm::mat4& vp) override { viewProjection = vp; }

        // Centre of the last world quad: the diagonal's midpoint, in metres.
        glm::vec3 lastQuadCentre() const { return (lastCorners[0] + lastCorners[2]) * 0.5f; }
        // The last world quad's turn about +Z: the angle of its top edge TL->TR,
        // which is the local +x axis (SpriteWorldQuad); 0 for an unrotated quad.
        float lastQuadAngle() const
        {
            const glm::vec3 top = lastCorners[1] - lastCorners[0];
            return std::atan2(top.y, top.x);
        }
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

namespace
{
    // A sprite entity addressed by a hand-built PhysicsInterpBuffer -- the exact
    // shape PASS 2.5 leaves behind (prev[slot] + slotOf[e]), with no PhysicsWorld
    // and no PhysicsBodyRef involved: the buffer is world-SLOT indexed and the
    // map IS the entity's address. Current world pose from `lt`; previous `prev`.
    Astra::Entity SpriteWithPrev(Astra::Registry& reg, const Arcane::Transform& lt,
                                 Arcane::InterpPose prev, std::uint32_t slot, std::uint32_t generation)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});

        Arcane::PhysicsInterpBuffer buf;
        buf.prev.resize(slot + 1);
        prev.generation = generation;
        buf.prev[slot] = prev;
        buf.slotOf[e] = Arcane::InterpSlot{ slot, generation };
        buf.captured = true;
        reg.SetResource<Arcane::PhysicsInterpBuffer>(std::move(buf));
        return e;
    }
}

TEST_CASE("RenderSubmissionSystem interpolates a sprite by PhysicsInterpBuffer + alpha", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // Current world pose at x=10; previous world-slot pose at x=0. Untextured Rect
    // sprite: nil .arcsprite -> a 1x1 m quad, so the scale IS the 4x4 size.
    Arcane::Transform lt; lt.position = glm::vec3(10.0f, 0.0f, 0.0f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f, 0.0f), 0.0f, 0 }, /*slot*/ 3, /*gen*/ 7);

    RecBatcher rec;
    Arcane::RenderContext2D ctx{ &rec, PixelView(), 0.5f };  // alpha 0.5
    reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.quadCalls == 1);
    CHECK(rec.screenQuadCalls == 0);                    // a WORLD quad, not a pixel rect
    CHECK(rec.lastQuadCentre().x == Approx(5.0f));      // lerp(0, 10, 0.5), in metres
    CHECK(rec.lastQuadCentre().y == Approx(0.0f));
    CHECK(rec.lastQuadAngle() == Approx(0.0f).margin(1e-5));
}

TEST_CASE("RenderSubmissionSystem submits sprites as WORLD quads with the interpolated XY pose",
          "[render][sprite][interp]")
{
    // F4 plan 1 T5: the system projects NOTHING. It hands the batcher world
    // METRES -- corners from the full world basis (SpriteWorldQuad) with the
    // interpolated pose re-baked into the matrix's XY translation and Z-angle --
    // and the host's view-projection (SetViewProjection) does the rest on the
    // GPU. The view in RenderContext2D is therefore irrelevant to the corners.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // Current pose (10, 4, z=1.5) scaled 4x4; previous world-slot pose (0, 0).
    Arcane::Transform lt; lt.position = glm::vec3(10.0f, 4.0f, 1.5f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f, 0.0f), 0.0f, 0 }, /*slot*/ 1, /*gen*/ 2);

    RecBatcher rec;
    // A deliberately NON-identity view: the corners must not move with it.
    reg.SetResource<Arcane::RenderContext2D>(Arcane::RenderContext2D{
        &rec, Arcane::ViewTransform::Orthographic({123.0f, -45.0f}, 7.0f, {640u, 480u}), 0.25f });
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.quadCalls == 1);
    CHECK(rec.screenQuadCalls == 0);
    // centre == lerp(prev, current, alpha) in XY; z == the entity's z (untouched by
    // the 2D interp).
    CHECK(rec.lastQuadCentre().x == Approx(2.5f));
    CHECK(rec.lastQuadCentre().y == Approx(1.0f));
    CHECK(rec.lastQuadCentre().z == Approx(1.5f));
    // The re-bake preserved the basis lengths (the 4x4 scale): a 4 m top edge
    // along +x, a 4 m left edge with TL ABOVE BL (+Y up = image top).
    CHECK(rec.lastCorners[1] - rec.lastCorners[0] == glm::vec3(4.0f, 0.0f, 0.0f));
    CHECK(rec.lastCorners[0] - rec.lastCorners[3] == glm::vec3(0.0f, 4.0f, 0.0f));
    CHECK(rec.lastCorners[0].z == Approx(1.5f));
}

TEST_CASE("RenderSubmissionSystem interpolates sprite rotation on the shortest arc", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    Arcane::Transform lt; lt.position = glm::vec3(0.0f);
    lt.rotation = Arcane::RotationAboutZ(10.0f * kPi / 180.0f);   // current 10deg about +Z
    lt.scale    = glm::vec3(4.0f, 4.0f, 1.0f);
    SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f), 350.0f * kPi / 180.0f, 0 }, 0, 1);

    RecBatcher rec;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &rec, PixelView(), 0.5f });
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.quadCalls == 1);
    // Shortest arc 350 -> 10 midpoint is 0deg, NOT 180deg. The angle is read
    // off the WORLD quad's top edge (world sense, +Y up; nothing mirrors it).
    CHECK(std::sin(rec.lastQuadAngle()) == Approx(0.0f).margin(1e-5));
    CHECK(std::cos(rec.lastQuadAngle()) == Approx(1.0f).margin(1e-5));
    // The re-bake keeps the basis lengths: still a 4 m top edge.
    CHECK(glm::length(rec.lastCorners[1] - rec.lastCorners[0]) == Approx(4.0f));
}

TEST_CASE("RenderSubmissionSystem snaps to the current pose on any buffer miss", "[interp]")
{
    // Every miss path takes the snap: a generation mismatch (recycled slot), a
    // slot past the buffer, an uncaptured buffer, no entry for the entity.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::Transform lt; lt.position = glm::vec3(10.0f, 0.0f, 0.0f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    const Astra::Entity e = SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f), 0.0f, 0 }, 2, 5);
    (void)e;
    reg.SetResource<Arcane::RenderContext2D>(Arcane::RenderContext2D{ nullptr, PixelView(), 0.5f });

    auto submit = [&]
    {
        RecBatcher rec;
        reg.GetResource<Arcane::RenderContext2D>()->batcher = &rec;
        Arcane::RenderSubmissionSystem{}(reg);
        REQUIRE(rec.quadCalls == 1);
        return rec.lastQuadCentre().x;
    };
    CHECK(submit() == Approx(5.0f));                                              // the hit, for contrast

    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev[2].generation = 6;        // recycled slot
    CHECK(submit() == Approx(10.0f));
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev[2].generation = 5;
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev.resize(2);               // slot past the end
    CHECK(submit() == Approx(10.0f));
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev.resize(3);
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev[2] = Arcane::InterpPose{ glm::vec2(0.0f), 0.0f, 5 };
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->captured = false;             // never captured
    CHECK(submit() == Approx(10.0f));
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->captured = true;
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->slotOf.Clear();               // no entry for the entity
    CHECK(submit() == Approx(10.0f));
}

TEST_CASE("RenderSubmissionSystem blends FROM the captured pose TOWARD the current one: alpha 0.25 lands a quarter of the way",
          "[interp]")
{
    // The owed case (spec 2026-09-11-physics-2d-wiring s8, "Plan 2's owed
    // case"): the two hand-built cases above use alpha 0.5, which is
    // SYMMETRIC -- a reversed Lerp endpoint order would still pass them. This
    // one runs the REAL chain (PhysicsSystem PASS 2.5 capture -> step -> PASS
    // 4 write-back -> propagation) and asks at 0.25 and 0.75, which only the
    // right direction satisfies. The endpoints are MEASURED through the same
    // submit at alpha 0 and 1 rather than computed from world units, so the
    // assertion is about the blend and not about the batcher's screen mapping.
    namespace P = Manifold2D::Physics;
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    P::WorldDef wd; wd.gravityY = 10.0f;
    reg.SetResource(Arcane::PhysicsResource{ std::make_unique<P::PhysicsWorld>(wd), {} });
    reg.SetResource(Arcane::PhysicsInterpBuffer{});   // opt in to capture

    // A scene root: TransformPropagationSystem is a no-op with no SceneRoot
    // resource (it returns immediately -- TransformSystems.hpp), and composes
    // only entities reachable from it (PhysicsSystemTest.cpp's BuildScene is
    // the precedent every other real-chain PhysicsSystem test follows).
    Astra::Entity root = reg.CreateEntity();
    Arcane::Transform rootLt; rootLt.position = glm::vec3(0.0f);
    reg.AddComponent<Arcane::Transform>(root, rootLt);
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    // One dynamic circle free-falling from the origin, with a sprite on it.
    // Untextured Rect sprite: nil .arcsprite -> a 1x1 m quad scaled 4x4.
    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = P::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col;
    { Arcane::Fixture fx; fx.kind = P::ShapeKind::Circle; fx.radius = 0.5f; col.fixtures.push_back(fx); }
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
    reg.SetParent(e, root);
    // No PhysicsBodyRef on purpose: PASS 1.5 adds it (Plan 1 Task 5).

    Arcane::PhysicsSystem physics(1.0f / 60.0f);
    Arcane::TransformPropagationSystem propagate;
    physics(reg);      // mint, capture prev = the authored pose, step, write back
    propagate(reg);    // WorldTransform = the post-step pose
    REQUIRE(reg.GetComponent<Arcane::PhysicsBodyRef>(e) != nullptr);
    REQUIRE(reg.GetResource<Arcane::PhysicsInterpBuffer>()->captured);

    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ nullptr, PixelView(), 0.0f });
    auto submitAt = [&](float alpha)
    {
        RecBatcher rec;
        Arcane::RenderContext2D* ctx = reg.GetResource<Arcane::RenderContext2D>();
        ctx->batcher = &rec;
        ctx->alpha   = alpha;
        Arcane::RenderSubmissionSystem{}(reg);
        REQUIRE(rec.quadCalls == 1);
        return rec.lastQuadCentre().y;
    };

    const float atPrev = submitAt(0.0f);
    const float atCur  = submitAt(1.0f);
    REQUIRE(atPrev != Approx(atCur));   // the body moved this step: the endpoints differ
    const float span = atCur - atPrev;
    // margin, not Approx's relative epsilon alone: the measured centre is the
    // world quad's diagonal midpoint, and that round trip through a 2 m
    // half-size costs ~1 ulp at magnitude 2 (1.2e-7) on values of ~4e-4 m -- a
    // 3e-4 relative error, above the 1.2e-5 Approx allows. The blend under test
    // is ~1.7e-3 m end to end, so a 1e-6 margin still separates 0.25 from 0.75.
    CHECK(submitAt(0.25f) == Approx(atPrev + 0.25f * span).margin(1e-6f));
    CHECK(submitAt(0.75f) == Approx(atPrev + 0.75f * span).margin(1e-6f));
    // And the reversed direction is what these two would read under a
    // swapped Lerp -- stated so the failure mode is named, not just implied.
    CHECK(submitAt(0.25f) != Approx(atPrev + 0.75f * span).margin(1e-6f));
}
