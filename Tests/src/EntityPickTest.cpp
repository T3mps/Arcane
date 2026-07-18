// Arcane sprite-OBB entity pick. CPU-only ([pick]).

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/EntityPick.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    // Fresh registry with the shared Scene components registered, mirroring the
    // fixture in RenderInterpolationTest.cpp / GrimoireEntityListTest.cpp.
    // Returned as a unique_ptr: Astra::Registry declares a user-provided copy
    // constructor (which suppresses the implicit move constructor), so it is
    // not a cheap-to-return-by-value type here.
    //
    // Cross-DLL note: PickEntitiesAt (and its CreateView<WorldTransform,
    // SpriteRenderer>) is compiled into Arcane.dll, so it resolves component IDs
    // through Arcane.dll's per-module TypeContext slot -- NOT the one main()
    // installs in the test module. Pin that DLL slot to the shared test context
    // once (a throwaway Runtime installs it in Arcane.dll; the slot persists
    // after the Runtime is destroyed -- see Runtime::~Runtime). Without this the
    // engine's view resolves different IDs than the ones the test module assigns
    // when adding the components, so [pick] run in isolation would see 0 hits.
    // (The Grimoire helper tests dodge this only because their iterators --
    // CollectEntities et al. -- are source-compiled into the test exe.)
    std::unique_ptr<Astra::Registry> MakeSceneRegistry()
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
        return reg;
    }

    Astra::Entity SpawnSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size,
                              int32_t layer, int32_t order)
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt;
        wt.matrix = glm::mat3(1.0f);
        wt.matrix[2] = glm::vec3(pos, 1.0f);          // column 2 = world position
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp;
        sp.size = size; sp.sortingLayer = layer; sp.orderInLayer = order;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
        return e;
    }
}

TEST_CASE("PickEntitiesAt hits inside the sprite OBB and misses outside", "[pick]")
{
    auto reg = MakeSceneRegistry();
    SpawnSprite(*reg, {0, 0}, {2, 2}, 0, 0);          // box [-1,1]^2 at origin

    CHECK(Arcane::PickEntitiesAt(*reg, {0.0f, 0.0f}).size() == 1);
    CHECK(Arcane::PickEntitiesAt(*reg, {0.9f, 0.9f}).size() == 1);
    CHECK(Arcane::PickEntitiesAt(*reg, {1.5f, 0.0f}).empty());   // outside half-extent 1
}

TEST_CASE("PickEntitiesAt sorts overlapping hits front-most first", "[pick]")
{
    auto reg = MakeSceneRegistry();
    const Astra::Entity back  = SpawnSprite(*reg, {0, 0}, {4, 4}, 0, 0);
    const Astra::Entity front = SpawnSprite(*reg, {0, 0}, {4, 4}, 5, 0);   // higher layer

    std::vector<Astra::Entity> hits = Arcane::PickEntitiesAt(*reg, {0.0f, 0.0f});
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].GetValue() == front.GetValue());   // front-most first
    CHECK(hits[1].GetValue() == back.GetValue());
}

TEST_CASE("PickEntitiesAt respects a rotated + translated world matrix", "[pick]")
{
    auto reg = MakeSceneRegistry();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::WorldTransform wt;
    // translate (10,0), rotate 90deg (glm column-major, m[i] = column i). This
    // maps local (lx,ly) -> world (10 - ly, lx): local +x -> world +y, and
    // local +y -> world -x. So the sprite's LONG local-y axis (half-extent 3)
    // lies along world X, and its SHORT local-x axis (half-extent 1) along world Y.
    glm::mat3 m(1.0f);
    const float c = 0.0f, s = 1.0f;                   // cos/sin 90deg
    m[0] = glm::vec3( c, s, 0.0f);
    m[1] = glm::vec3(-s, c, 0.0f);
    m[2] = glm::vec3(10.0f, 0.0f, 1.0f);
    wt.matrix = m;
    reg->AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp; sp.size = {2, 6};       // long along local y (half 3)
    reg->AddComponent<Arcane::SpriteRenderer>(e, sp);

    // Both probes DISCRIMINATE rotation: each flips outcome vs. an unrotated box.
    // (12,0): 2 units along world X -- inside the rotated LONG axis (half 3), yet
    // an unrotated 2x6 box (world-x half 1) would MISS it.
    CHECK(Arcane::PickEntitiesAt(*reg, {12.0f, 0.0f}).size() == 1);
    // (10,2): 2 units along world Y -- beyond the rotated SHORT axis (half 1), yet
    // an unrotated box (world-y half 3) would HIT it.
    CHECK(Arcane::PickEntitiesAt(*reg, {10.0f, 2.0f}).empty());
}
