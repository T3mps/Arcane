// Rich physics-debug overlay (Sandbox unify-to-outlines pivot, Item A).
//
// DrawPhysicsDebug is now the SINGLE canonical Sandbox renderer (the scenes no
// longer add filled SpriteRenderer quads), so it grew richer per-body debug
// geometry so the user can "really see what's happening":
//   * a velocity vector  -- a line from the body COM along its linear velocity
//   * a center-of-mass marker -- a tiny cross/disc at the world COM
//   * an orientation tick -- a short line along the body's local +x so rotation
//     is visible even on a circle
// Each is gated behind a PhysicsDebugDrawOptions bool flag (sane defaults).
//
// CPU-only (tag [render], no graphics device): DrawPhysicsDebug takes the
// Batcher2D interface, so a recording mock captures the emitted Line/Circle
// submissions and we assert the new geometry appears (and is suppressed when
// the flag is off).

#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>

#include <glm/glm.hpp>

using Catch::Approx;
using namespace Arcane::Physics;

namespace
{
    // Recording mock: captures every Line + Circle submission so a test can
    // count / inspect the emitted debug geometry.
    struct RecMock final : Arcane::Batcher2D
    {
        std::vector<std::pair<glm::vec2, glm::vec2>> lines;
        std::vector<std::pair<glm::vec2, float>>     circles;

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Line(glm::vec2 a, glm::vec2 b, float, glm::vec4) override
        {
            lines.emplace_back(a, b);
        }
        void Circle(glm::vec2 c, float r, glm::vec4) override
        {
            circles.emplace_back(c, r);
        }
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };

    // A world with one moving dynamic box (no gravity, no floor) so the only
    // emitted geometry is the box outline + whatever rich overlays the options
    // enable. Returns by value via the out-params: the world is built in-place.
    void OneMovingBox(PhysicsWorld& w, BodyHandle& outHandle)
    {
        BodyDef bd;
        bd.type     = BodyType::Dynamic;
        bd.position = Vec2(Real(10), Real(10));
        // Polygon box so it can carry a nonzero angle (a dynamic Aabb is
        // forced fixedRotation); 3x2 m half-extents.
        const std::vector<Vec2> verts = {
            Vec2(Real(-3), Real(-2)), Vec2(Real(3), Real(-2)),
            Vec2(Real(3), Real(2)),   Vec2(Real(-3), Real(2)),
        };
        bd.shape   = MakePolygon(verts);
        bd.density = Real(1);
        outHandle  = w.AddBody(bd);
        w.SetVelocity(outHandle, Vec2(Real(20), Real(0)));   // moving +x fast
        w.SetAngle(outHandle, Real(0.5));                    // tilted so the tick reads
    }
}

TEST_CASE("PhysicsDebug rich: velocity vector emitted only when enabled", "[render]")
{
    WorldDef wd;  // gravity 0 -- isolates kinematics from the debug-draw overlay
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);
    BodyHandle h;
    OneMovingBox(w, h);

    // Baseline: velocity vector OFF -> outline lines only (4 for the box).
    {
        RecMock off;
        Arcane::PhysicsDebugDrawOptions opts;
        opts.drawVelocities  = false;
        opts.drawComMarkers  = false;
        opts.drawOrientations = false;
        opts.drawContacts    = false;
        Arcane::DrawPhysicsDebug(w, off, opts);
        CHECK(off.lines.size() == 4);   // just the 4 polygon edges
    }

    // Velocity vector ON -> at least one MORE line (the velocity ray).
    {
        RecMock on;
        Arcane::PhysicsDebugDrawOptions opts;
        opts.drawVelocities   = true;
        opts.drawComMarkers   = false;
        opts.drawOrientations = false;
        opts.drawContacts     = false;
        Arcane::DrawPhysicsDebug(w, on, opts);
        CHECK(on.lines.size() > 4);   // outline + velocity ray
    }
}

TEST_CASE("PhysicsDebug rich: orientation tick + COM marker gated by flags", "[render]")
{
    WorldDef wd;  // gravity 0 -- isolates kinematics from the debug-draw overlay
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);
    BodyHandle h;
    OneMovingBox(w, h);

    // Orientation tick ON (everything else off) -> at least one extra line.
    {
        RecMock on;
        Arcane::PhysicsDebugDrawOptions opts;
        opts.drawVelocities   = false;
        opts.drawComMarkers   = false;
        opts.drawOrientations = true;
        opts.drawContacts     = false;
        Arcane::DrawPhysicsDebug(w, on, opts);
        CHECK(on.lines.size() > 4);   // outline + orientation tick
    }

    // COM marker ON -> at least one extra primitive (line cross or disc).
    {
        RecMock on;
        Arcane::PhysicsDebugDrawOptions opts;
        opts.drawVelocities   = false;
        opts.drawComMarkers   = true;
        opts.drawOrientations = false;
        opts.drawContacts     = false;
        Arcane::DrawPhysicsDebug(w, on, opts);
        CHECK((on.lines.size() > 4 || on.circles.size() > 0));
    }
}

TEST_CASE("PhysicsDebug rich: a resting body draws no velocity ray", "[render]")
{
    WorldDef wd;  // gravity 0 -- isolates kinematics from the debug-draw overlay
    wd.gravityX = Real(0);
    wd.gravityY = Real(0);
    PhysicsWorld w(wd);

    // A static body never moves -> with drawVelocities on it must still emit
    // ONLY its outline (no zero-length velocity ray clutter).
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(5), Real(5));
    bd.shape    = MakeAabb(Real(2), Real(1));
    w.AddBody(bd);

    RecMock m;
    Arcane::PhysicsDebugDrawOptions opts;
    opts.drawVelocities   = true;
    opts.drawComMarkers   = false;
    opts.drawOrientations = false;
    opts.drawContacts     = false;
    Arcane::DrawPhysicsDebug(w, m, opts);

    // 4 outline lines for the AABB, and NO velocity ray (static -> v == 0).
    CHECK(m.lines.size() == 4);
}
