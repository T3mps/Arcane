// [sandbox] CPU-only: the Sandbox now renders EVERY body as an OUTLINE through
// the single canonical DrawPhysicsDebug overlay (the outline-unify pivot, Item
// A) -- the scene builders no longer attach filled SpriteRenderer quads to
// physics bodies. This test pins that new contract:
//
//   1. The "Compound bodies" scene authors NO body SpriteRenderers (the old
//      AttachFixtureVisuals per-fixture discs are gone). Previously this test
//      asserted >= 4 circle SPRITES; the deliberate user-approved pivot flips
//      that to >= 0 sprites and moves the visual coverage to the outline path.
//   2. After PhysicsSystem mints the bodies, DrawPhysicsDebug emits the bodies'
//      collider OUTLINES: each lopsided compound body's primary circle fixture
//      becomes one debug circle, and the floor becomes a rectangle (4 lines).
//
// CPU-only: path-A builders author Astra entities; one PhysicsSystem fixedUpdate
// mints the live PhysicsWorld bodies, then DrawPhysicsDebug runs against a
// recording mock Batcher2D (no graphics device needed).

#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../Sandbox/src/Scenes.hpp"

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace
{
    namespace Sbx = Arcane::Sandbox;

    // Find the scene index by name in the roster (robust to roster reordering).
    int SceneIndexByName(const char* name)
    {
        const std::span<const Sbx::SceneDef> scenes = Sbx::SceneRegistry();
        for (std::size_t i = 0; i < scenes.size(); ++i)
            if (std::string_view(scenes[i].name) == name)
                return static_cast<int>(i);
        return -1;
    }

    // Recording mock: counts the Line + Circle primitives DrawPhysicsDebug emits.
    struct CountMock final : Arcane::Batcher2D
    {
        int lines = 0;
        int circles = 0;

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float) override {}
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override {}
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override { ++lines; }
        void Circle(glm::vec2, float, glm::vec4) override { ++circles; }
        void End() override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };
}

TEST_CASE("Sandbox: compound scene authors no body sprites (outline-unify)", "[sandbox]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    // The builders create live bodies on a PhysicsResource world (path A mints
    // them on the first PhysicsSystem fixedUpdate; path-B builders create world
    // bodies directly), so install one before building.
    Arcane::PhysicsResource physRes;
    Arcane::Physics::WorldDef wd;
    wd.gravityY = Arcane::Physics::Real(900);
    physRes.world = std::make_unique<Arcane::Physics::PhysicsWorld>(wd);
    reg.SetResource<Arcane::PhysicsResource>(std::move(physRes));

    const int idx = SceneIndexByName("Compound bodies");
    REQUIRE(idx >= 0);
    Sbx::SceneRegistry()[idx].build(reg);

    // CONTRACT 1: the outline-unify pivot removed the per-fixture body sprites.
    // No SpriteRenderer should remain in the Compound scene (the bodies render
    // through DrawPhysicsDebug now). The old contract asserted >= 4 CIRCLE
    // sprites; the new one asserts ZERO sprites.
    int sprites = 0;
    auto view = reg.CreateView<Arcane::SpriteRenderer>();
    view.ForEach([&](Astra::Entity, Arcane::SpriteRenderer&) { ++sprites; });
    CHECK(sprites == 0);
}

TEST_CASE("Sandbox: compound bodies render as collider outlines (DrawPhysicsDebug)",
          "[sandbox]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    Arcane::PhysicsResource physRes;
    Arcane::Physics::WorldDef wd;
    wd.gravityY = Arcane::Physics::Real(900);
    physRes.world = std::make_unique<Arcane::Physics::PhysicsWorld>(wd);
    reg.SetResource<Arcane::PhysicsResource>(std::move(physRes));

    const int idx = SceneIndexByName("Compound bodies");
    REQUIRE(idx >= 0);
    Sbx::SceneRegistry()[idx].build(reg);

    // One PhysicsSystem fixedUpdate mints the path-A bodies on the world.
    Arcane::PhysicsSystem phys(1.0f / 60.0f);
    phys(reg);

    Arcane::PhysicsResource* res = reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    REQUIRE(res->world != nullptr);

    // The scene has a floor (Aabb -> rectangle, 4 lines) + 2 lopsided bodies,
    // each a primary CIRCLE fixture (-> one debug circle). With all rich overlays
    // and contacts off, the only circles are the two body primary-shape outlines.
    CountMock mock;
    Arcane::PhysicsDebugDrawOptions opts;
    opts.drawVelocities   = false;
    opts.drawComMarkers   = false;
    opts.drawOrientations = false;
    opts.drawContacts     = false;
    opts.drawAabbs        = false;
    Arcane::DrawPhysicsDebug(*res->world, mock, opts);

    CHECK(mock.circles >= 2);  // 2 compound bodies' primary circle outlines
    CHECK(mock.lines   >= 4);  // the floor rectangle (4 lines)
}
