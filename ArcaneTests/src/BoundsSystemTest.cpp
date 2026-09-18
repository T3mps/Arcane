// BoundsSystem (F3 plan 1 T2): WorldBounds on every drawable, written after
// transform propagation. Mesh = the artifact AABB through the world matrix;
// sprite = the SpriteWorldQuad corners widened by kSpriteDepthEpsilon in Z; a
// bare node or an unresolved mesh carries none; a removed renderer drops the
// box; Changed<> exactness means an untouched row is not rewritten; Hidden is
// not consulted.
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <unordered_map>
#include <utility>

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>   meshes;
        std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> sprites;
        Astra::Entity root{};

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
            reg.SetResource<Arcane::SpriteTable>(Arcane::SpriteTable{ &sprites });
        }

        Astra::Entity Spawn(glm::vec3 pos)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            return e;
        }

        void Tick()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
        }
    };

    Arcane::Guid AddCube(World& w)
    {
        const Arcane::Guid id = Arcane::Guid::Generate();
        Arcane::MeshEntry entry;
        entry.data   = Arcane::BuildCube(2.0f);              // local box [-1, 1]^3
        entry.bounds = Arcane::ComputeMeshBounds(entry.data);
        w.meshes.emplace(id, entry);
        return id;
    }
}

TEST_CASE("BoundsSystem: a resolved mesh gets its local box through the world matrix", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity e = w.Spawn(glm::vec3(10, 0, 0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ cube, {} });
    w.Tick();
    const Arcane::WorldBounds* b = std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(e);
    REQUIRE(b);
    CHECK(b->box.min == glm::vec3(9, -1, -1));
    CHECK(b->box.max == glm::vec3(11, 1, 1));
}

TEST_CASE("BoundsSystem: an entity carrying BOTH renderers gets the UNION of the mesh box and the sprite box", "[bounds]")
{
    // Review round 1: the sprite and mesh passes each draw their own row without
    // excluding the other, so a both-renderer entity paints both, and a box that
    // covered the mesh alone would let the visible set cull the sprite half.
    // Geometry chosen so each drawable pokes out of the other: a 0.5 m cube
    // (local [-0.25, 0.25]^3) is thinner than the 1x1 m unresolved sprite in X/Y,
    // and the sprite's epsilon-thin Z is inside the cube's.
    World w;
    const Arcane::Guid small = Arcane::Guid::Generate();
    Arcane::MeshEntry entry;
    entry.data   = Arcane::BuildCube(0.5f);
    entry.bounds = Arcane::ComputeMeshBounds(entry.data);
    w.meshes.emplace(small, entry);

    Astra::Entity e = w.Spawn(glm::vec3(10, 0, 0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ small, {} });
    Arcane::SpriteRenderer s; s.shape = Arcane::SpriteShape::Rect;   // unresolved: 1x1 m, centre pivot
    w.reg.AddComponent<Arcane::SpriteRenderer>(e, s);
    w.Tick();

    const Arcane::WorldBounds* b = std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(e);
    REQUIRE(b);
    // mesh:   [9.75, 10.25] x [-0.25, 0.25] x [-0.25, 0.25]
    // sprite: [9.5 - eps, 10.5 + eps] x [-0.5 - eps, 0.5 + eps] x [-eps, +eps]
    const float eps = Arcane::kSpriteDepthEpsilon;
    CHECK(b->box.min.x == Catch::Approx(9.5f - eps));    // sprite wins X
    CHECK(b->box.max.x == Catch::Approx(10.5f + eps));
    CHECK(b->box.min.y == Catch::Approx(-0.5f - eps));   // sprite wins Y
    CHECK(b->box.max.y == Catch::Approx(0.5f + eps));
    CHECK(b->box.min.z == Catch::Approx(-0.25f));        // mesh wins Z
    CHECK(b->box.max.z == Catch::Approx(0.25f));
}

TEST_CASE("BoundsSystem: an unresolved mesh gets NO WorldBounds", "[bounds]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ Arcane::Guid::Generate(), {} });
    w.Tick();
    CHECK_FALSE(w.reg.HasComponent<Arcane::WorldBounds>(e));
}

TEST_CASE("BoundsSystem: a sprite gets the SpriteWorldQuad box widened by the depth epsilon", "[bounds]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(2, 3, 0));
    Arcane::SpriteRenderer s; s.shape = Arcane::SpriteShape::Rect;   // unresolved: 1x1 m, centre pivot
    w.reg.AddComponent<Arcane::SpriteRenderer>(e, s);
    w.Tick();
    const Arcane::WorldBounds* b = std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(e);
    REQUIRE(b);
    CHECK(b->box.min.x == Catch::Approx(1.5f - Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.max.x == Catch::Approx(2.5f + Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.min.y == Catch::Approx(2.5f - Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.max.y == Catch::Approx(3.5f + Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.min.z == Catch::Approx(-Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.max.z == Catch::Approx(+Arcane::kSpriteDepthEpsilon));
}

TEST_CASE("BoundsSystem: a bare transform node has no bounds; removing the renderer removes the box", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity bare = w.Spawn(glm::vec3(0));
    Astra::Entity mesh = w.Spawn(glm::vec3(0));
    w.reg.AddComponent<Arcane::MeshRenderer>(mesh, Arcane::MeshRenderer{ cube, {} });
    w.Tick();
    CHECK_FALSE(w.reg.HasComponent<Arcane::WorldBounds>(bare));
    REQUIRE(w.reg.HasComponent<Arcane::WorldBounds>(mesh));
    w.reg.RemoveComponent<Arcane::MeshRenderer>(mesh);
    w.Tick();
    CHECK_FALSE(w.reg.HasComponent<Arcane::WorldBounds>(mesh));
}

TEST_CASE("BoundsSystem: a moved entity's box follows; an untouched one is not rewritten", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity moving = w.Spawn(glm::vec3(0));
    Astra::Entity still  = w.Spawn(glm::vec3(5, 5, 5));
    w.reg.AddComponent<Arcane::MeshRenderer>(moving, Arcane::MeshRenderer{ cube, {} });
    w.reg.AddComponent<Arcane::MeshRenderer>(still,  Arcane::MeshRenderer{ cube, {} });
    w.Tick();
    const Astra::Tick after1 = w.reg.CurrentTick();
    w.reg.GetComponent<Arcane::Transform>(moving)->position = glm::vec3(3, 0, 0);
    w.Tick();
    CHECK(std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(moving)->box.min == glm::vec3(2, -1, -1));
    CHECK(w.reg.IsChanged<Arcane::WorldBounds>(moving, after1));
    CHECK_FALSE(w.reg.IsChanged<Arcane::WorldBounds>(still, after1));
}

TEST_CASE("BoundsSystem: a Hidden entity keeps its box (Hidden is a draw decision, not a bounds one)", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity e = w.Spawn(glm::vec3(0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ cube, {} });
    w.reg.AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
    w.Tick();
    CHECK(w.reg.HasComponent<Arcane::WorldBounds>(e));
}
