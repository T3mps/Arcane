// Astra adoption Task 8 (spec s7): the editor-owned Edit-mode scheduler and its
// single pending camera-frame request, driven headlessly.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Scene/EditModeSchedule.hpp>
#include <Viewport/EditorCamera.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <span>
#include <vector>

using Catch::Approx;
using Arcane::Editor::EditModeSchedule;
using Arcane::Editor::FrameRequest;

namespace
{
    struct Scene
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Astra::Entity root{}, sprite{};
        Scene()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        }
        // A 1x1 m untextured sprite (no SpriteTable -> unit base), centre pivot.
        Astra::Entity AddSprite(glm::vec2 pos)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = glm::vec3(pos, 0.0f);
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
            reg.SetParent(e, root);
            return e;
        }
        std::uint32_t Runs() { return reg.GetResource<Arcane::TransformOrder>()->runs; }
    };
    const glm::vec2 kViewport(800.0f, 600.0f);
}

TEST_CASE("EditModeSchedule runs propagation exactly once per Edit-mode frame and never in Play",
          "[editor][change-detection]")
{
    Scene s;
    s.AddSprite({1.0f, 0.0f});
    EditModeSchedule schedule;

    for (int frame = 0; frame < 5; ++frame)
        CHECK(schedule.RunFrame(s.reg, /*inPlayMode*/ false));
    CHECK(s.Runs() == 5u);

    CHECK_FALSE(schedule.RunFrame(s.reg, /*inPlayMode*/ true));
    CHECK(s.Runs() == 5u);
}

TEST_CASE("a moved entity is framed at its NEW bounds on the next frame service",
          "[editor][change-detection]")
{
    Scene s;
    const Astra::Entity e = s.AddSprite({1.0f, 0.0f});
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    // Frame N: the entity moves (gizmo-style, a stamping write) and the user
    // presses Home in the same frame. The request must see the moved pose.
    s.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(50.0f, 20.0f, 0.0f);
    schedule.RequestFrame(FrameRequest::Scene);
    REQUIRE(schedule.RunFrame(s.reg, false));
    REQUIRE(schedule.ServicePendingFrame(s.reg, std::span<const Astra::Entity>{}, cam, kViewport));

    const glm::vec2 centre = cam.WorldToScreen(glm::vec2(50.0f, 20.0f));
    CHECK(centre.x == Approx(kViewport.x * 0.5f));
    CHECK(centre.y == Approx(kViewport.y * 0.5f));
    CHECK(schedule.Pending() == FrameRequest::None);          // consumed
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));   // nothing pending now
}

TEST_CASE("the frame request is a single slot: last wins, and Selection frames the selection",
          "[editor][change-detection]")
{
    Scene s;
    const Astra::Entity a = s.AddSprite({-10.0f, 0.0f});
    const Astra::Entity b = s.AddSprite({30.0f, 0.0f});
    (void)a;
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    schedule.RequestFrame(FrameRequest::Scene);
    schedule.RequestFrame(FrameRequest::Selection);
    CHECK(schedule.Pending() == FrameRequest::Selection);
    const std::vector<Astra::Entity> sel{ b };
    REQUIRE(schedule.ServicePendingFrame(s.reg, sel, cam, kViewport));
    const glm::vec2 centre = cam.WorldToScreen(glm::vec2(30.0f, 0.0f));   // b, not the scene's midpoint
    CHECK(centre.x == Approx(kViewport.x * 0.5f));
}

TEST_CASE("SceneOpen on an empty scene centres the origin; Scene leaves the view alone; zero viewport defers",
          "[editor][change-detection]")
{
    Scene s;   // root only: nothing framable
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    schedule.RequestFrame(FrameRequest::SceneOpen);
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, glm::vec2(0.0f)));   // not laid out yet
    CHECK(schedule.Pending() == FrameRequest::SceneOpen);                          // kept
    REQUIRE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.offset.x == Approx(kViewport.x * 0.5f));
    CHECK(cam.offset.y == Approx(kViewport.y * 0.5f));

    const glm::vec2 before = cam.offset;
    schedule.RequestFrame(FrameRequest::Scene);
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.offset == before);
}
