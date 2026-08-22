// ActiveSceneCamera is the ONE place three callers agree about the view --
// ArcaneRuntime, play-in-editor, and the editor's camera-rect overlay -- so its
// contract is pinned here rather than trusted to three call sites.

#include <catch2/catch_test_macros.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/SceneCamera.hpp>
#include <Arcane/Scene/SceneModule.hpp>              // RegisterSceneComponents
#include <Arcane/Serialization/SceneAsset.hpp>       // Scene::CreateEmpty (New Scene shape)

#include <glm/gtc/epsilon.hpp>

namespace
{
    // A real Runtime bound to the process-wide SharedTypeContext, never a bare one:
    // a test-local Runtime would steal Arcane.dll's TypeContext slot (see
    // CommandStackTest.cpp's note on the same hazard).
    struct CameraFixture
    {
        Arcane::Runtime  runtime{&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false};
        Astra::Registry& reg = runtime.Registry();

        // Deliberately does NOT call RegisterSceneComponents from THIS module:
        // Runtime's ctor already registers the engine roster inside Arcane.dll, and
        // that is the arrangement every real host has. Registering again here would
        // make the test module the registrar and hide any cross-module id problem --
        // which is exactly the shape of bug that aborts a host but not a test.
        CameraFixture() = default;

        Astra::Entity AddCamera(glm::vec2 worldPos, float halfHeight, bool active = true)
        {
            const Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t;
            t.position = glm::vec3(worldPos, 0.0f);   // Task 3 (F1): 3D pose, planar scene
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{t.ToMatrix()});
            reg.AddComponent<Arcane::Camera>(e, Arcane::Camera{halfHeight, active});
            return e;
        }
    };
}

TEST_CASE("no Camera entity means nullopt, not an identity view", "[scene][camera]")
{
    CameraFixture f;
    int count = -1;
    // The whole point: a host must be able to tell "no camera" from "a camera that
    // happens to be identity", because substituting identity renders every
    // pre-camera scene at 1 px/m in the corner.
    CHECK_FALSE(Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f, &count).has_value());
    CHECK(count == 0);
}

TEST_CASE("zoom is derived from the viewport height, so framing is resolution-independent",
          "[scene][camera]")
{
    CameraFixture f;
    f.AddCamera(glm::vec2(0.0f, 0.0f), /*halfHeight*/ 5.0f);

    const auto a = Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f);
    const auto b = Arcane::ActiveSceneCamera(f.reg, 2560.0f, 1440.0f);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    // 5 m half-height over a 720-px-tall viewport == 72 px per meter.
    CHECK(glm::epsilonEqual(a->zoom, 72.0f, 1e-4f));
    // Twice the height, twice the zoom -- the SAME 10 m of world stays framed.
    CHECK(glm::epsilonEqual(b->zoom, 144.0f, 1e-4f));
    // The authored facts survive for callers that draw the camera instead of
    // looking through it (the editor's rect overlay).
    CHECK(glm::epsilonEqual(a->halfHeight, 5.0f, 1e-4f));
}

TEST_CASE("the camera's world position lands at the viewport centre", "[scene][camera]")
{
    CameraFixture f;
    f.AddCamera(glm::vec2(3.0f, -2.0f), /*halfHeight*/ 5.0f);

    const auto v = Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f);
    REQUIRE(v.has_value());
    CHECK(glm::epsilonEqual(v->worldCenter.x, 3.0f, 1e-4f));

    // screen = world * zoom + offset, so the camera's own position must map to the
    // middle of the viewport. This is the assertion that would catch a sign flip.
    const glm::vec2 screen = v->worldCenter * v->zoom + v->offset;
    CHECK(glm::epsilonEqual(screen.x, 640.0f, 1e-3f));
    CHECK(glm::epsilonEqual(screen.y, 360.0f, 1e-3f));
}

TEST_CASE("inactive and non-positive cameras are ignored", "[scene][camera]")
{
    CameraFixture f;
    f.AddCamera(glm::vec2(0.0f, 0.0f), 5.0f, /*active*/ false);
    // A zero half-height has no view to derive and must not divide by zero.
    f.AddCamera(glm::vec2(0.0f, 0.0f), 0.0f, /*active*/ true);

    int count = -1;
    CHECK_FALSE(Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f, &count).has_value());
    CHECK(count == 0);
}

TEST_CASE("first active camera wins and the count reports the ambiguity", "[scene][camera]")
{
    CameraFixture f;
    f.AddCamera(glm::vec2(0.0f, 0.0f), 5.0f);
    f.AddCamera(glm::vec2(50.0f, 50.0f), 20.0f);

    int count = 0;
    const auto v = Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f, &count);
    REQUIRE(v.has_value());
    // Two candidates: one is used, and the caller is told there was more than one
    // so it can warn once (the PostProcess sweep's contract).
    CHECK(count == 2);
    // Whichever the view order yields, it must be ONE of them and never a blend.
    const bool isFirst  = glm::epsilonEqual(v->halfHeight, 5.0f, 1e-4f);
    const bool isSecond = glm::epsilonEqual(v->halfHeight, 20.0f, 1e-4f);
    CHECK((isFirst || isSecond));
}

TEST_CASE("a camera with no WorldTransform yet falls back to its local position",
          "[scene][camera]")
{
    CameraFixture f;
    // The frame an entity first gets a Camera it may have no WorldTransform: that
    // is DERIVED data, materialised at a different point in the frame by each host.
    // Skipping it for that frame would flicker the view back to the previous one.
    const Astra::Entity e = f.reg.CreateEntity();
    Arcane::Transform t;
    t.position = glm::vec3(7.0f, 1.0f, 0.0f);
    f.reg.AddComponent<Arcane::Transform>(e, t);
    f.reg.AddComponent<Arcane::Camera>(e, Arcane::Camera{5.0f, true});

    const auto v = Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f);
    REQUIRE(v.has_value());
    CHECK(glm::epsilonEqual(v->worldCenter.x, 7.0f, 1e-4f));
    CHECK(glm::epsilonEqual(v->worldCenter.y, 1.0f, 1e-4f));
}

TEST_CASE("CreateEmpty ships a scene with a working camera", "[scene][camera]")
{
    CameraFixture f;
    Arcane::Scene::CreateEmpty(f.reg);

    // A New Scene must be immediately playable -- authoring a level, pressing Play
    // and getting a black window is the failure this default exists to prevent.
    int count = 0;
    const auto v = Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f, &count);
    REQUIRE(v.has_value());
    CHECK(count == 1);
    CHECK(v->zoom > 0.0f);
}
