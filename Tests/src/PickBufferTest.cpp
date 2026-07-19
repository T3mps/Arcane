// Arcane pickable-collection emitter (Task 2 of the entity-id picking design,
// docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md SS3b/3c).
// CPU-only ([pick]), no GPU: exercises CollectPickables (sprite + collider
// emitters) and the id<->entity table (PickEntityForId).

#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include "Helpers/TestTypeContext.hpp"

using Catch::Approx;

namespace
{
    // Fresh registry with Scene + Physics components registered and a
    // zero-gravity PhysicsResource attached.
    //
    // Cross-DLL note (mirrors EntityPickTest.cpp's MakeSceneRegistry):
    // CollectPickables is compiled into Arcane.dll (Arcane/Render/PickEmit.cpp),
    // so its registry.CreateView<WorldTransform, SpriteRenderer>() resolves
    // component IDs through Arcane.dll's per-module TypeContext slot, not the
    // one main() installs in the test module. Pin that DLL slot to the shared
    // test context once (a throwaway Runtime installs it in Arcane.dll; the
    // slot persists after the Runtime is destroyed) so both modules agree on
    // component IDs -- otherwise [pick] run in isolation would see 0 sprites.
    std::unique_ptr<Astra::Registry> MakePickRegistry()
    {
        static const bool s_ctxPinned = []
        {
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            return true;
        }();
        (void)s_ctxPinned;

        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        Arcane::RegisterPhysicsComponents(*reg);

        Manifold2D::Physics::WorldDef wd;
        wd.gravityY = 0.0f;
        wd.gravityX = 0.0f;
        reg->SetResource(Arcane::PhysicsResource{
            std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd),
            {}
        });

        return reg;
    }

    Astra::Entity SpawnSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size)
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt;
        wt.matrix = glm::mat3(1.0f);
        wt.matrix[2] = glm::vec3(pos, 1.0f);          // column 2 = world position
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp;
        sp.size = size;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
        return e;
    }

    // Mints a kinematic body with a single Aabb fixture at `pos`, via the real
    // PhysicsSystem create pass (stepWorld=false: registers the body and
    // writes it back without stepping, so it stays exactly at `pos`).
    // `scale` is baked into the body's fixtures by the create pass (it sets
    // PhysicsBodyRef::appliedScale = lt.scale) -- used to prove CollectPickables
    // scales the silhouette to match the drawn collider.
    Astra::Entity SpawnAabbBody(Astra::Registry& reg, glm::vec2 pos, float halfW, float halfH,
                                glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        const Astra::Entity e = reg.CreateEntity();

        Arcane::LocalTransform lt;
        lt.position = pos;
        lt.scale    = scale;
        reg.AddComponent<Arcane::LocalTransform>(e, lt);

        Arcane::RigidBody2D rb;
        rb.type = Manifold2D::Physics::BodyType::Kinematic;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);

        Arcane::Collider2D col;
        Arcane::Fixture fx;
        fx.kind  = Manifold2D::Physics::ShapeKind::Aabb;
        fx.halfW = halfW;
        fx.halfH = halfH;
        col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(e, col);

        reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

        Arcane::PhysicsSystem sys(1.0f / 60.0f, /*stepWorld=*/false);
        sys(reg);

        return e;
    }
}

TEST_CASE("CollectPickables gathers sprites and physics colliders, ordered", "[pick]")
{
    auto reg = MakePickRegistry();

    // Known camera view: offset (100,50) canvas px, 10 px per world-meter.
    const Arcane::PickView view{ {100.0f, 50.0f}, 10.0f };

    // Sprite entity: world pos (2,3), size (4,6) -> world half-extents (2,3).
    const Astra::Entity spriteEntity = SpawnSprite(*reg, {2.0f, 3.0f}, {4.0f, 6.0f});

    // Physics entity: Aabb fixture halfW=0.5 halfH=0.25, body pos (5,-1).
    const Astra::Entity colliderEntity = SpawnAabbBody(*reg, {5.0f, -1.0f}, 0.5f, 0.25f);

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, view, out);

    REQUIRE(out.size() == 2);

    // Each entity appears exactly once, sprites-then-colliders order.
    CHECK(out[0].entity.GetValue() == spriteEntity.GetValue());
    CHECK(out[1].entity.GetValue() == colliderEntity.GetValue());

    // Sprite -> Quad. Projected: center = (2,3)*10 + (100,50) = (120,80);
    // half-extents = (2,3)*10 = (20,30) (world half-extents (2,3) from size*0.5).
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Quad);
    CHECK(out[0].center.x == Approx(120.0f));
    CHECK(out[0].center.y == Approx(80.0f));
    CHECK(out[0].halfExtents.x == Approx(20.0f));
    CHECK(out[0].halfExtents.y == Approx(30.0f));

    // Collider -> Box. Projected: center = (5,-1)*10 + (100,50) = (150,40);
    // half-extents = (0.5,0.25)*10 = (5,2.5).
    CHECK(out[1].kind == Arcane::PickDrawable::Kind::Box);
    CHECK(out[1].center.x == Approx(150.0f));
    CHECK(out[1].center.y == Approx(40.0f));
    CHECK(out[1].halfExtents.x == Approx(5.0f));
    CHECK(out[1].halfExtents.y == Approx(2.5f));
}

TEST_CASE("id->entity table maps 1-based, 0 is background", "[pick]")
{
    auto reg = MakePickRegistry();
    const Astra::Entity e0 = reg->CreateEntity();
    const Astra::Entity e1 = reg->CreateEntity();

    std::vector<Arcane::PickDrawable> table;
    Arcane::PickDrawable d0; d0.entity = e0; table.push_back(d0);
    Arcane::PickDrawable d1; d1.entity = e1; table.push_back(d1);

    CHECK_FALSE(Arcane::PickEntityForId(table, 0).IsValid());   // 0 == background
    CHECK(Arcane::PickEntityForId(table, 1).GetValue() == e0.GetValue());
    CHECK(Arcane::PickEntityForId(table, 2).GetValue() == e1.GetValue());
    CHECK_FALSE(Arcane::PickEntityForId(table, 3).IsValid());   // out of range
}

// The silhouette must match the DRAWN collider, which the physics create pass
// bakes at lt.scale (MakeScaledShape) -- so a scaled body has to pick at its
// scaled size, not its authored size. Aabb scales per-axis (halfW*|sx|,
// halfH*|sy|), mirroring PhysicsSystem::MakeScaledShape.
TEST_CASE("CollectPickables scales a collider silhouette by the body's baked scale", "[pick]")
{
    auto reg = MakePickRegistry();

    // 10 px per world-meter, no offset.
    const Arcane::PickView view{ {0.0f, 0.0f}, 10.0f };

    // Aabb halfW=0.5 halfH=0.25 at body pos (1,1), authored scale (2,4). The
    // create pass bakes appliedScale=(2,4); the silhouette half-extents scale to
    //   world half = (0.5*2, 0.25*4) = (1.0, 1.0); canvas = *10 = (10, 10).
    const Astra::Entity e = SpawnAabbBody(*reg, {1.0f, 1.0f}, 0.5f, 0.25f, {2.0f, 4.0f});

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, view, out);

    REQUIRE(out.size() == 1);
    CHECK(out[0].entity.GetValue() == e.GetValue());
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Box);
    CHECK(out[0].center.x == Approx(10.0f));       // (1,1)*10
    CHECK(out[0].center.y == Approx(10.0f));
    CHECK(out[0].halfExtents.x == Approx(10.0f));  // 0.5 * 2 * 10
    CHECK(out[0].halfExtents.y == Approx(10.0f));  // 0.25 * 4 * 10
}

// The drawable index IS the hit-proxy id (id = index+1), so the collection order
// must be DETERMINISTIC and must not depend on unordered_map hash layout. Pins
// the documented contract: sprites first (back), then colliders (front), stable
// across repeated collections.
TEST_CASE("CollectPickables orders sprites before colliders, deterministically", "[pick]")
{
    auto reg = MakePickRegistry();
    const Arcane::PickView view{ {0.0f, 0.0f}, 1.0f };

    const Astra::Entity sprite    = SpawnSprite(*reg, {0.0f, 0.0f}, {1.0f, 1.0f});
    const Astra::Entity colliderA = SpawnAabbBody(*reg, {1.0f, 0.0f}, 0.5f, 0.5f);
    const Astra::Entity colliderB = SpawnAabbBody(*reg, {2.0f, 0.0f}, 0.5f, 0.5f);

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, view, out);

    REQUIRE(out.size() == 3);
    CHECK(out[0].entity.GetValue() == sprite.GetValue());   // sprite first (back)

    // Both colliders appear exactly once, after the sprite (order between the two
    // is archetype-stable; assert set membership to stay robust to that detail).
    const bool colsPresent =
        (out[1].entity.GetValue() == colliderA.GetValue() && out[2].entity.GetValue() == colliderB.GetValue()) ||
        (out[1].entity.GetValue() == colliderB.GetValue() && out[2].entity.GetValue() == colliderA.GetValue());
    CHECK(colsPresent);

    // Determinism: a second collection yields the identical ordering.
    std::vector<Arcane::PickDrawable> out2;
    Arcane::CollectPickables(*reg, view, out2);
    REQUIRE(out2.size() == 3);
    CHECK(out2[0].entity.GetValue() == out[0].entity.GetValue());
    CHECK(out2[1].entity.GetValue() == out[1].entity.GetValue());
    CHECK(out2[2].entity.GetValue() == out[2].entity.GetValue());
}
