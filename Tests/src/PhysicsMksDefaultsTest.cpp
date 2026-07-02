// Box2D v3 default parity for WorldDef (b2DefaultWorldDef) -- every value cites
// the vendored source at ThirdParty/box2d-3.1.1. Spec:
// docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md #3.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

TEST_CASE("WorldDef defaults are Box2D v3 MKS", "[physics][mks]")
{
    WorldDef def;
    CHECK(def.gravityX == Real(0));                        // types.c:12
    CHECK(def.gravityY == Real(10));                       // types.c:13 ((0,-10) y-up -> +10 y-down)
    CHECK(def.sleepThreshold == Approx(Real(0.05)));       // types.c:34 (BodyDef-level in b2)
    CHECK(def.restitutionThreshold == Approx(Real(1)));    // types.c:15
    CHECK(def.contactPushMaxVelocity == Approx(Real(3)));  // types.c:16 maxContactPushSpeed
    CHECK(def.maxLinearVelocity == Approx(Real(400)));     // types.c:21 (unchanged)
    CHECK(def.contactHertz == Approx(Real(30)));           // types.c:17 (unchanged)
    CHECK(def.contactDampingRatio == Approx(Real(10)));    // types.c:18 (unchanged)
    CHECK(def.hashCellSize == Approx(Real(1)));            // Arcane grid tuning (MKS)
}

TEST_CASE("BodyDef.sleepThreshold defaults to inherit", "[physics][mks]")
{
    BodyDef bd;
    CHECK(bd.sleepThreshold == Real(-1)); // inherit WorldDef (Arcane architecture; value carries)
}

TEST_CASE("Engine length constants are Box2D v3 MKS", "[physics][mks]")
{
    CHECK(kLinearSlop == Approx(Real(0.005)));          // constants.h:23 (unchanged)
    CHECK(kSkin == Approx(Real(0.02)));                 // constants.h:38 B2_SPECULATIVE_DISTANCE = 4*slop
    CHECK(kSkin == Approx(Real(4) * kLinearSlop));      // the RELATION, not just the value
    CHECK(DynamicTree::kMargin == Approx(Real(0.05))); // constants.h:44 B2_AABB_MARGIN (v3.1.1)
    CHECK(kMaxRotation == Approx(Real(0.25) * kPi));    // constants.h:33 (unchanged)
}
