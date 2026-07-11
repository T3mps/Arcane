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
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../Sandbox/src/Scenes.hpp"

#include <Manifold2D/Physics/PhysicsWorld.hpp>
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
        void RemoveTexture(nvrhi::ITexture*) override {}
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
    Manifold2D::Physics::WorldDef wd;   // MKS defaults (gravity +10, sleepThreshold 0.05, ...)
    physRes.world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
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
    Manifold2D::Physics::WorldDef wd;   // MKS defaults (gravity +10, sleepThreshold 0.05, ...)
    physRes.world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
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

// =============================================================================
// Stress scene (BuildStressTest / scene 8): CPU-only stability + volume gate.
// =============================================================================
// Two DECOUPLED cases. The registered scene 8 / Loom perf showcase builds the
// full kStressBodyCount (10000) churn -- but that procedural grid is 20 columns
// wide, so 10000 bodies stack ~500 rows / ~410 m tall (top row y ~ -404 m at MKS,
// was ~ -40,000 px). That is fine for the perf scene, but it overshoots the
// spatial bound the stability check uses, so the two concerns are split:
//
//   * VOLUME/VARIETY/AGITATORS run on the registered 10000-body scene (no
//     stepping): the knob built the configured count, world-direct bodies exist,
//     the kinematic whisk is present. (spec section 8, assertions a/c/d)
//   * STABILITY runs on a calibrated subset (kStressStabilityBodyCount = 1200 =>
//     60-row column, top y ~ -40.4 m, comfortably inside kYMin = -50 m) so that a
//     position leaving the bound is a REAL instability (energy gain / escape),
//     not just the spawn column being taller than the bound. (assertion b)
//
// (Investigated 2026-06-24: at 10000 bodies the original single-case "allBounded"
// failed purely because most bodies START above the bound at spawn -- the physics
// is stable: nothing escapes the bowl in X, no NaN, speeds bounded by the whisk
// drive, and the column falls DOWN into bounds. Calibration, not a defect.)
// =============================================================================
TEST_CASE("Sandbox: stress scene builds kStressBodyCount bodies (volume/variety/agitators)",
          "[sandbox]")
{
    namespace Phys = Manifold2D::Physics;

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    Arcane::PhysicsResource physRes;
    Phys::WorldDef wd;   // MKS defaults (gravity +10, sleepThreshold 0.05, ...)
    physRes.world = std::make_unique<Phys::PhysicsWorld>(wd);
    reg.SetResource<Arcane::PhysicsResource>(std::move(physRes));

    const int idx = SceneIndexByName("Stress test");
    REQUIRE(idx >= 0);
    Sbx::SceneRegistry()[idx].build(reg);

    // One PhysicsSystem fixedUpdate mints the path-A bodies on the world.
    Arcane::PhysicsSystem phys(1.0f / 60.0f);
    phys(reg);

    Arcane::PhysicsResource* res = reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    REQUIRE(res->world != nullptr);
    Phys::PhysicsWorld& world = *res->world;

    // (d) Variety: count path-A entities (those with PhysicsBodyRef -- minted via
    //     PhysicsSystem). The world should have MORE bodies than that because the
    //     scene also spawns world-direct polygons, compounds, spinners, and walls.
    int pathACount = 0;
    {
        auto view = reg.CreateView<Arcane::PhysicsBodyRef>();
        view.ForEach([&](Astra::Entity, Arcane::PhysicsBodyRef&) { ++pathACount; });
    }

    // (a) Volume: the world must have at least kStressBodyCount bodies (the
    //     procedural dynamics alone); walls and spinners push Count() even higher.
    const std::uint32_t totalCount = world.Count();
    CHECK(static_cast<int>(totalCount) >= Sbx::kStressBodyCount);

    // (d) Variety: some bodies must be world-direct (not path-A entities).
    CHECK(static_cast<int>(totalCount) > pathACount);

    // (c) Agitators: at least kStressWhiskCount kinematic bodies (the whisk).
    int kineticCount = 0;
    for (std::uint32_t i = 0; i < world.Count(); ++i)
    {
        if (!world.Alive(i)) continue;
        if (world.TypeSlot(i) == Phys::BodyType::Kinematic)
            ++kineticCount;
    }
    CHECK(kineticCount >= Sbx::kStressWhiskCount);
}

TEST_CASE("Sandbox: stress scene stays bounded after 30 steps (calibrated subset)",
          "[sandbox]")
{
    namespace Phys = Manifold2D::Physics;

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    Arcane::PhysicsResource physRes;
    Phys::WorldDef wd;   // MKS defaults (gravity +10, sleepThreshold 0.05, ...)
    physRes.world = std::make_unique<Phys::PhysicsWorld>(wd);
    reg.SetResource<Arcane::PhysicsResource>(std::move(physRes));

    // Build the SAME stress arena/whisk/seed, but at the calibrated body count so
    // the spawn column fits the stability bound (decoupled from the perf knob).
    Sbx::BuildStressTestN(reg, Sbx::kStressStabilityBodyCount);

    // One PhysicsSystem fixedUpdate mints the path-A bodies on the world.
    Arcane::PhysicsSystem phys(1.0f / 60.0f);
    phys(reg);

    Arcane::PhysicsResource* res = reg.GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    REQUIRE(res->world != nullptr);
    Phys::PhysicsWorld& world = *res->world;

    // Sanity: the calibrated subset actually built (procedural dynamics + arena).
    CHECK(static_cast<int>(world.Count()) >= Sbx::kStressStabilityBodyCount);

    // Step 30 fixed frames (the sim must survive without explosion).
    constexpr int   kSteps = 30;
    constexpr float kDt    = 1.0f / 60.0f;
    for (int s = 0; s < kSteps; ++s)
        world.Step(Phys::Real(kDt));

    // (b) Stability: iterate every live slot; all positions must be finite and
    //     within a generous arena bound (MKS, /100 of the old px bounds). Arena
    //     inner X = [-2.2, 15.0] m; walls at -2.2-0.5 = -2.7 to 15.0+0.5 = 15.5 m.
    //     kYMin re-derived for MKS: 1200 bodies / 20 cols = 60 rows x 0.82 m pitch,
    //     spawn bottom = kFloorY - kPitch = 8.8 - 0.82 = 7.98 m, so the top row sits
    //     at 7.98 - 59*0.82 ~ -40.4 m; the column only falls DOWN, so a -50 m floor
    //     (~1.24x below the spawn top) is crossed ONLY by a genuine runaway/escape.
    constexpr float kXMin = -8.0f;
    constexpr float kXMax = 21.0f;
    constexpr float kYMin = -50.0f;   // top spawn row ~ -40.4 m; headroom to -50 m
    constexpr float kYMax = 15.0f;

    bool allBounded = true;
    for (std::uint32_t i = 0; i < world.Count(); ++i)
    {
        if (!world.Alive(i)) continue;

        const Phys::Vec2 pos = world.PosSlot(i);

        // Check finite.
        const bool finiteX = std::isfinite(static_cast<float>(pos.x));
        const bool finiteY = std::isfinite(static_cast<float>(pos.y));
        if (!finiteX || !finiteY) { allBounded = false; continue; }

        // Check in generous arena bound.
        if (static_cast<float>(pos.x) < kXMin || static_cast<float>(pos.x) > kXMax ||
            static_cast<float>(pos.y) < kYMin || static_cast<float>(pos.y) > kYMax)
        {
            allBounded = false;
        }
    }

    CHECK(allBounded);  // (b) no NaN / runaway position after 30 steps
}
