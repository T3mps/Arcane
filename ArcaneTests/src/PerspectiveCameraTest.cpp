// Task 5 (Phase 4): pins the perspective half of Camera/SceneCamera.hpp.
// Pure matrix math -- no device, no NRI -- exactly like SceneCameraTest.cpp
// pins the ortho half. The four properties this file MUST hold (plan
// docs/plans/2026-08-21-nri-phase4-3d-slice.md, Task 5):
//   1. a point at -nearZ maps to NDC z ~= 0 and -farZ to NDC z ~= 1 -- the
//      [0,1] depth convention (D3D/Vulkan), explicitly NOT reverse-Z.
//   2. aspect ratio scales x only.
//   3. a 90-degree fov puts a point at (z, 0, -z) exactly on the right clip
//      plane -- the property that pins this engine's handedness as
//      RIGHT-handed (see SceneCamera.hpp's PerspectiveCameraView comment for
//      the derivation: under a LEFT-handed camera that same point would be
//      BEHIND the camera and could not land on any clip plane at all).
//   4. Camera::projection defaults to Orthographic, so every scene authored
//      before this field existed is unchanged.

#include <catch2/catch_test_macros.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/SceneCamera.hpp>
#include <Arcane/Scene/SceneModule.hpp>   // RegisterSceneComponents

#include <glm/gtc/epsilon.hpp>

namespace
{
    // Same shape as SceneCameraTest.cpp's CameraFixture, duplicated rather
    // than shared: each test file owns its fixture in this codebase (see
    // that file's own note on why -- a bare test-local Runtime would steal
    // Arcane.dll's TypeContext slot).
    struct PerspectiveCameraFixture
    {
        Arcane::Runtime  runtime{&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false};
        Astra::Registry& reg = runtime.Registry();

        PerspectiveCameraFixture() = default;

        Astra::Entity AddPerspectiveCamera(glm::vec2 worldPos, float fovYDegrees = 60.0f,
                                            float nearZ = 0.1f, float farZ = 1000.0f,
                                            bool active = true)
        {
            const Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t;
            t.position = glm::vec3(worldPos, 0.0f);   // Task 3 (F1): 3D pose, planar scene
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{t.ToMatrix()});
            Arcane::Camera cam;
            cam.active       = active;
            cam.projection   = Arcane::CameraProjection::Perspective;
            cam.fovYDegrees  = fovYDegrees;
            cam.nearZ        = nearZ;
            cam.farZ         = farZ;
            reg.AddComponent<Arcane::Camera>(e, cam);
            return e;
        }

        Astra::Entity AddOrthoCamera(glm::vec2 worldPos, float halfHeight, bool active = true)
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

// ---------------------------------------------------------------------------
// Required property 4: the compatibility default.
// ---------------------------------------------------------------------------

TEST_CASE("Camera::projection defaults to Orthographic, so every existing 2D scene is unchanged",
          "[scene][camera][perspective]")
{
    const Arcane::Camera cam{};
    CHECK(cam.projection == Arcane::CameraProjection::Orthographic);
}

// ---------------------------------------------------------------------------
// Required property 1: the [0,1] depth convention, NOT reverse-Z.
// ---------------------------------------------------------------------------

TEST_CASE("a point at -nearZ maps to NDC z of 0 and -farZ maps to NDC z of 1 (D3D/Vulkan [0,1] "
          "depth, forward-Z -- reverse-Z is NOT used by this projection)",
          "[scene][camera][perspective]")
{
    const float nearZ = 0.1f;
    const float farZ  = 500.0f;
    const glm::mat4 proj = Arcane::PerspectiveProjection(60.0f, 16.0f / 9.0f, nearZ, farZ);

    auto ndcZ = [&](float viewZ)
    {
        const glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, viewZ, 1.0f);
        return clip.z / clip.w;
    };

    // Forward-Z, [0,1]: near -> 0, far -> 1. A reverse-Z projection would swap
    // these (near -> 1, far -> 0); this engine deliberately does not use one.
    CHECK(glm::epsilonEqual(ndcZ(-nearZ), 0.0f, 1e-5f));
    CHECK(glm::epsilonEqual(ndcZ(-farZ), 1.0f, 1e-5f));
}

// ---------------------------------------------------------------------------
// Required property 2: aspect ratio scales x only.
// ---------------------------------------------------------------------------

TEST_CASE("aspect ratio scales x only", "[scene][camera][perspective]")
{
    const float fovY = 60.0f, nearZ = 0.1f, farZ = 100.0f;
    const glm::mat4 square = Arcane::PerspectiveProjection(fovY, 1.0f, nearZ, farZ);
    const glm::mat4 wide   = Arcane::PerspectiveProjection(fovY, 2.0f, nearZ, farZ);

    const glm::vec4 p(1.0f, 1.0f, -5.0f, 1.0f);
    const glm::vec4 cSquare = square * p;
    const glm::vec4 cWide   = wide * p;

    // y, z and w are untouched by aspect -- only the x scale term (1 / (aspect
    // * tan(fovY/2))) depends on it.
    CHECK(glm::epsilonEqual(cWide.y, cSquare.y, 1e-5f));
    CHECK(glm::epsilonEqual(cWide.z, cSquare.z, 1e-5f));
    CHECK(glm::epsilonEqual(cWide.w, cSquare.w, 1e-5f));

    // Doubling the aspect ratio halves x -- the property that would fail if
    // aspect leaked into the y term too (a uniform scale, not an x-only one).
    CHECK(glm::epsilonEqual(cWide.x, cSquare.x * 0.5f, 1e-5f));
    CHECK_FALSE(glm::epsilonEqual(cWide.x, cSquare.x, 1e-5f));
}

// ---------------------------------------------------------------------------
// Required property 3: 90-degree fov puts (z, 0, -z) exactly on the right
// clip plane -- the property that pins RH handedness (see the derivation in
// SceneCamera.hpp above PerspectiveCameraView).
// ---------------------------------------------------------------------------

TEST_CASE("a 90 degree fov puts a point at (z, 0, -z) exactly on the right clip plane",
          "[scene][camera][perspective]")
{
    // fovY = 90, aspect = 1 -> fovX = fovY = 90 too, so tan(fovX/2) = 1 and the
    // right clip plane is the set of view-space points where x == -Z (x == the
    // forward distance). Because a symmetric frustum's side planes pass
    // through the eye, this holds at ANY depth -- not just at the near plane
    // -- which is exactly what "exactly on the clip plane" is pinning here.
    const glm::mat4 proj = Arcane::PerspectiveProjection(90.0f, 1.0f, 0.1f, 1000.0f);

    for (const float z : {0.5f, 1.0f, 10.0f, 250.0f})
    {
        const glm::vec4 clip = proj * glm::vec4(z, 0.0f, -z, 1.0f);
        REQUIRE(clip.w > 0.0f);   // in front of the camera, not behind it
        // On the right clip plane means clip.x == clip.w (NDC x == +1 after
        // the perspective divide).
        CHECK(glm::epsilonEqual(clip.x, clip.w, 1e-4f));
        CHECK(glm::epsilonEqual(clip.x / clip.w, 1.0f, 1e-5f));

        // The corresponding y is untouched (y == 0 in, y == 0 out) -- the
        // point is on the right plane, not a corner.
        CHECK(glm::epsilonEqual(clip.y, 0.0f, 1e-5f));
    }
}

// ---------------------------------------------------------------------------
// Beyond the four required properties: the ECS-level wiring the branch adds
// to SceneCamera.hpp -- projection-mode discrimination between the two
// sweeps, and ActivePerspectiveSceneCamera's own contract, mirroring the
// coverage SceneCameraTest.cpp already gives ActiveSceneCamera.
// ---------------------------------------------------------------------------

TEST_CASE("a Perspective camera is invisible to the ortho sweep, and an Orthographic camera is "
          "invisible to the perspective sweep",
          "[scene][camera][perspective]")
{
    PerspectiveCameraFixture f;
    f.AddPerspectiveCamera(glm::vec2(0.0f, 0.0f));

    // The scene's only camera is Perspective: ActiveSceneCamera (ortho) must
    // report none found, not silently derive a bogus 2D view from a camera
    // whose orthographicSize was never authored.
    int orthoCount = -1;
    CHECK_FALSE(Arcane::ActiveSceneCamera(f.reg, 1280.0f, 720.0f, &orthoCount).has_value());
    CHECK(orthoCount == 0);

    // And the perspective sweep DOES find it.
    int perspCount = -1;
    CHECK(Arcane::ActivePerspectiveSceneCamera(f.reg, 16.0f / 9.0f, &perspCount).has_value());
    CHECK(perspCount == 1);

    // Add an Orthographic camera too: the perspective sweep must keep
    // ignoring it.
    f.AddOrthoCamera(glm::vec2(5.0f, 5.0f), 5.0f);
    int perspCount2 = -1;
    const auto v = Arcane::ActivePerspectiveSceneCamera(f.reg, 16.0f / 9.0f, &perspCount2);
    REQUIRE(v.has_value());
    CHECK(perspCount2 == 1);   // still just the one Perspective camera
}

TEST_CASE("no active Perspective camera means nullopt, not identity", "[scene][camera][perspective]")
{
    PerspectiveCameraFixture f;
    int count = -1;
    CHECK_FALSE(Arcane::ActivePerspectiveSceneCamera(f.reg, 16.0f / 9.0f, &count).has_value());
    CHECK(count == 0);

    f.AddPerspectiveCamera(glm::vec2(0.0f, 0.0f), 60.0f, 0.1f, 1000.0f, /*active*/ false);
    CHECK_FALSE(Arcane::ActivePerspectiveSceneCamera(f.reg, 16.0f / 9.0f, &count).has_value());
    CHECK(count == 0);
}

TEST_CASE("ActivePerspectiveSceneCamera's view matrix places the camera's world position at the "
          "view-space origin",
          "[scene][camera][perspective]")
{
    PerspectiveCameraFixture f;
    f.AddPerspectiveCamera(glm::vec2(3.0f, -2.0f));

    const auto v = Arcane::ActivePerspectiveSceneCamera(f.reg, 16.0f / 9.0f);
    REQUIRE(v.has_value());

    // The eye is the entity's world position at Z=0 (Transform is 2D-only
    // today); transforming that exact point by its own view matrix must land
    // it at the view-space origin, same shape of assertion SceneCameraTest.cpp
    // uses for the ortho path's "world position lands at the viewport centre".
    const glm::vec4 viewSpace = v->view * glm::vec4(3.0f, -2.0f, 0.0f, 1.0f);
    CHECK(glm::epsilonEqual(viewSpace.x, 0.0f, 1e-4f));
    CHECK(glm::epsilonEqual(viewSpace.y, 0.0f, 1e-4f));
    CHECK(glm::epsilonEqual(viewSpace.z, 0.0f, 1e-4f));
}
