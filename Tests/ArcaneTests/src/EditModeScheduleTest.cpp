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
#include <string>
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
        // Null-safe: TransformOrder is emplaced lazily by the propagation
        // system's first execution, so a scene that has not yet run one has
        // no resource at all -- 0 runs, not a null-dereference.
        std::uint32_t Runs()
        {
            const auto* c = reg.GetResource<Arcane::TransformOrder>();
            return c ? c->runs : 0u;
        }
    };
    const glm::vec2  kViewport(800.0f, 600.0f);
    const glm::uvec2 kViewportPx(800u, 600u);   // the same panel, as Resolve takes it
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

TEST_CASE("the physics edit pass runs before propagation, once per Edit frame, never in Play", "[editor][physics]")
{
    // Spec s6.1: EditModeSchedule owns Edit mode's ONLY physics -- the injected
    // pass (EditorApp binds Runtime::PhysicsEditPass) runs before the
    // propagation it feeds. Device-less: the seam is a callable, so this test
    // links no Manifold2D.
    Scene s;
    EditModeSchedule schedule;
    std::vector<std::string> order;
    // Rolling, not fixed: propagation's run count is monotonic, so a lambda
    // fired on a SECOND RunFrame call must compare against the count as of
    // just before THAT call, not the original baseline.
    std::uint32_t lastRuns = s.Runs();
    schedule.SetPhysicsEditPass([&] { order.push_back("physics"); CHECK(s.Runs() == lastRuns); });
    REQUIRE(schedule.RunFrame(s.reg, /*inPlayMode*/ false));
    REQUIRE(order.size() == 1);
    CHECK(s.Runs() == lastRuns + 1);
    lastRuns = s.Runs();
    CHECK_FALSE(schedule.RunFrame(s.reg, /*inPlayMode*/ true));
    CHECK(order.size() == 1);                       // Play: no edit pass
    REQUIRE(schedule.RunFrame(s.reg, false));
    CHECK(order.size() == 2);
    CHECK(s.Runs() == lastRuns + 1);
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

    const glm::vec3 centre = cam.Resolve(kViewportPx).WorldToScreen(glm::vec3(50.0f, 20.0f, 0.0f));
    CHECK(centre.x == Approx(kViewport.x * 0.5f));
    CHECK(centre.y == Approx(kViewport.y * 0.5f));
    CHECK(cam.ortho.center == glm::vec2(50.0f, 20.0f));
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
    const glm::vec3 centre = cam.Resolve(kViewportPx).WorldToScreen(glm::vec3(30.0f, 0.0f, 0.0f));   // b, not the scene's midpoint
    CHECK(centre.x == Approx(kViewport.x * 0.5f));
    CHECK(cam.ortho.center.x == Approx(30.0f));
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
    cam.ortho.center = glm::vec2(9.0f, -9.0f);   // somewhere else, so the reset is visible
    REQUIRE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.ortho.center == glm::vec2(0.0f, 0.0f));
    CHECK(cam.ortho.halfHeight == Approx(5.0f));   // CentreOrigin never rescales
    const glm::vec3 origin = cam.Resolve(kViewportPx).WorldToScreen(glm::vec3(0.0f));
    CHECK(origin.x == Approx(kViewport.x * 0.5f));
    CHECK(origin.y == Approx(kViewport.y * 0.5f));

    cam.ortho.center = glm::vec2(3.0f, 4.0f);
    const Arcane::Editor::Ortho2D before = cam.ortho;
    schedule.RequestFrame(FrameRequest::Scene);
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.ortho.center == before.center);
    CHECK(cam.ortho.halfHeight == before.halfHeight);
}

TEST_CASE("CancelFrame drops a pending SceneOpen so a camera restored from the ini survives boot; it leaves any other request alone",
          "[editor][change-detection]")
{
    // F4 plan 1 final review, F3: EditorApp::ViewportSettingsReadLine calls
    // CancelFrame(SceneOpen) when the persisted [EditorViewport][Camera]
    // block restores an Ortho=/Orbit= transform, AFTER OnProjectOpened
    // recorded the boot-time SceneOpen. The service that follows must then
    // leave the restored transform exactly where the ini put it -- on an
    // empty scene (where SceneOpen would CentreOrigin) as much as a full one.
    Scene s;   // root only: SceneOpen would centre the origin
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    schedule.RequestFrame(FrameRequest::SceneOpen);
    cam.ortho.center     = glm::vec2(12.0f, -7.0f);   // "restored from the ini"
    cam.ortho.halfHeight = 42.0f;
    schedule.CancelFrame(FrameRequest::SceneOpen);
    CHECK(schedule.Pending() == FrameRequest::None);
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.ortho.center == glm::vec2(12.0f, -7.0f));
    CHECK(cam.ortho.halfHeight == Approx(42.0f));

    // A different pending request is not the boot framing: a Home press
    // recorded before the ini read is kept.
    schedule.RequestFrame(FrameRequest::Scene);
    schedule.CancelFrame(FrameRequest::SceneOpen);
    CHECK(schedule.Pending() == FrameRequest::Scene);
}
