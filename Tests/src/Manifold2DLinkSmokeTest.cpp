// Proves the vendored Manifold2D library links + runs inside Aphelyon. The full
// physics/geometry test suite now lives in the standalone Manifold2D repo
// (github.com/T3mps/Manifold2D); Aphelyon keeps only this link-smoke so a broken
// vendor-in (bad include path, ABI/CRT mismatch, missing symbol) fails the
// Aphelyon build even though the suite left. Intentionally minimal: one dynamic
// body falls under gravity across a handful of steps.

#include <catch2/catch_test_macros.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

TEST_CASE("Manifold2D vendored lib links and a body falls under gravity", "[manifold2d]")
{
    using namespace Manifold2D::Physics;

    WorldDef wd; // gravity defaults to (0, 10) MKS -- y-down, Box2D v3 default
    PhysicsWorld world(wd);

    BodyDef def;
    def.type     = BodyType::Dynamic;
    def.position = Vec2(Real(0), Real(0));
    def.shape    = MakeCircle(Real(0.5)); // a fixture gives the body mass
    def.density  = Real(1);
    const BodyHandle body = world.AddBody(def);

    const Real y0 = world.Position(body).y;
    for (int i = 0; i < 30; ++i) world.Step(Real(1) / Real(60));

    // Engine is y-down: gravity increases y. If the lib linked and stepped, it fell.
    CHECK(world.Position(body).y > y0);
}
