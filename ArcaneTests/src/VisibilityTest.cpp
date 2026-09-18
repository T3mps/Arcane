// VisibleSet -- the CPU coarse visibility stage (F3 plan 1 T3, spec s4).
// Linear over every WorldBounds; the culling frustum comes from the
// UNJITTERED ViewTransform, widened by kVisibilitySlack metres.
#include <Arcane/Render/VisibilitySystem.hpp>
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

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
        Astra::Entity root{};
        Arcane::Guid cube;

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
            cube = Arcane::Guid::Generate();
            Arcane::MeshEntry entry;
            entry.data   = Arcane::BuildCube(2.0f);
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            meshes.emplace(cube, entry);
        }
        Astra::Entity Cube(glm::vec3 pos)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ cube, {} });
            return e;
        }
        void Tick() { Arcane::TransformPropagationSystem{}(reg); Arcane::BoundsSystem{}(reg); }
    };

    Arcane::ViewTransform Ortho10()   // half-height 10 m, 4:3 -> half-width 13.33
    {
        return Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 10.0f, glm::uvec2{ 800, 600 });
    }
}

TEST_CASE("BuildVisibleSet: inside in, outside out, straddling in, nearDepth is the box's nearest view depth", "[visibility]")
{
    World w;
    Astra::Entity in       = w.Cube(glm::vec3(0, 0, 0));
    Astra::Entity out      = w.Cube(glm::vec3(40, 0, 0));
    Astra::Entity straddle = w.Cube(glm::vec3(13.5f, 0, 0));   // [12.5, 14.5] crosses the 13.33 edge
    Astra::Entity deep     = w.Cube(glm::vec3(0, 0, -5));
    w.Tick();
    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, Ortho10(), vis);
    CHECK(vis.Contains(in));
    CHECK_FALSE(vis.Contains(out));
    CHECK(vis.Contains(straddle));
    CHECK(vis.Contains(deep));
    REQUIRE(vis.entries.size() == 3);
    // The ortho view sits at z=0 looking -Z; the [-1,1] cube at the origin has its nearest face at z=+1 -> depth -1 -> clamped 0; the deep one at z=-4 -> depth 4.
    float depthIn = -1.0f, depthDeep = -1.0f;
    for (const Arcane::VisibleEntry& e : vis.entries)
    {
        if (e.entity == in)   depthIn   = e.nearDepth;
        if (e.entity == deep) depthDeep = e.nearDepth;
    }
    CHECK(depthIn == Catch::Approx(0.0f));
    CHECK(depthDeep == Catch::Approx(4.0f));
}

TEST_CASE("BuildVisibleSet: the slack admits a box just outside the frustum", "[visibility]")
{
    World w;
    Astra::Entity e = w.Cube(glm::vec3(14.5f, 0, 0));   // [13.5, 15.5]: 0.17 m outside the 13.33 edge, inside the 0.25 slack
    w.Tick();
    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, Ortho10(), vis);
    CHECK(vis.Contains(e));
}

TEST_CASE("BuildVisibleSet: Hidden entities and entities without WorldBounds are not members; Clear on entry", "[visibility]")
{
    World w;
    Astra::Entity hidden = w.Cube(glm::vec3(0));
    w.reg.AddComponent<Arcane::Hidden>(hidden, Arcane::Hidden{});
    Astra::Entity bare = w.reg.CreateEntity();
    w.reg.AddComponent<Arcane::Transform>(bare, Arcane::Transform{});
    w.reg.SetParent(bare, w.root);
    w.Tick();
    Arcane::VisibleSet vis;
    vis.entries.push_back(Arcane::VisibleEntry{ bare, {}, 0.0f });   // stale content must not survive
    Arcane::BuildVisibleSet(w.reg, Ortho10(), vis);
    CHECK_FALSE(vis.Contains(hidden));
    CHECK_FALSE(vis.Contains(bare));
    CHECK(vis.entries.empty());
}

TEST_CASE("MainVisibleSet: absent resource is nullptr; views[0] once set", "[visibility]")
{
    World w;
    CHECK(Arcane::MainVisibleSet(w.reg) == nullptr);
    Arcane::SceneVisibility* sv = w.reg.EmplaceResource<Arcane::SceneVisibility>();
    REQUIRE(sv);
    CHECK(Arcane::MainVisibleSet(w.reg) == nullptr);   // no views yet
    sv->views.emplace_back();
    CHECK(Arcane::MainVisibleSet(w.reg) == &sv->views[0]);
}
