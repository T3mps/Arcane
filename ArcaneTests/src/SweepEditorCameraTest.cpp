// Settings arc S6-30: editor.camera.* -- the camera's former static
// constexpr block and its literal sensitivities / boost / distance scale,
// now EditorCameraSettings fields read from the published snapshot.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorViewportSettings.hpp"
#include "Viewport/EditorCamera.hpp"

#include <Arcane/Config/Settings.hpp>

#include <cmath>

using namespace Arcane;
using namespace Arcane::Editor;

TEST_CASE("sweep: editor camera defaults are the pre-sweep literals", "[sweep][editor-camera]")
{
    const EditorCameraSettings c{};
    CHECK(c.wheelZoomStep == 1.12f); CHECK(c.frameFill == 0.9f); CHECK(c.nearClip == 0.05f); CHECK(c.farClip == 5000.0f);
    CHECK(c.baseFlySpeed == 5.0f); CHECK(c.lookSensitivity == 0.2f); CHECK(c.boostMultiplier == 2.0f);
    CHECK(c.orthoMinHalfHeight == 0.01f); CHECK(c.orthoMaxHalfHeight == 1.0e6f);
    CHECK(EditorCamera{}.ortho.halfHeight == 5.0f);
    CHECK(EditorCamera{}.orbit.yawDeg == -30.0f);
    Test::RequireDefault("editor.camera.wheelZoomStep", CVarValue::Float32(1.12f));
}

TEST_CASE("sweep: Dolly uses editor.camera.wheelZoomStep", "[sweep][editor-camera]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    EditorCamera cam; cam.orbit.distance = 10.0f;
    cam.Dolly(1.0f);
    CHECK(cam.orbit.distance == 10.0f / 1.12f);
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("editor.camera.wheelZoomStep"), CVarValue::Float32(2.0f), SetBy::Code); reg.PublishImmediate();
    cam.orbit.distance = 10.0f; cam.Dolly(1.0f);
    CHECK(cam.orbit.distance == 5.0f);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}

TEST_CASE("sweep: every editor.camera.* default is the declared literal", "[sweep][editor-camera]")
{
    const EditorCameraSettings c{};
    CHECK(c.default2DHalfHeight == 5.0f); CHECK(c.default3DYaw == -30.0f); CHECK(c.default3DPitch == 30.0f);
    CHECK(c.default3DDistance == 10.0f);
    CHECK(c.minOrbitDistance == 0.05f); CHECK(c.maxOrbitDistance == 1.0e5f);
    CHECK(c.distanceScaledSpeed); CHECK(c.refDistance == 10.0f); CHECK(c.speedFloor == 0.1f); CHECK(c.speedCap == 1000.0f);
    CHECK(c.orbitSensitivity == 0.2f); CHECK(c.speedWheelStep == 1.1f);

    const EditorCamera cam{};
    CHECK(cam.orbit.pitchDeg == 30.0f); CHECK(cam.orbit.distance == 10.0f);

    Test::RequireDefault("editor.camera.default2DHalfHeight", CVarValue::Float32(5.0f));
    Test::RequireDefault("editor.camera.default3DYaw", CVarValue::Float32(-30.0f));
    Test::RequireDefault("editor.camera.default3DPitch", CVarValue::Float32(30.0f));
    Test::RequireDefault("editor.camera.default3DDistance", CVarValue::Float32(10.0f));
    Test::RequireDefault("editor.camera.orthoMinHalfHeight", CVarValue::Float32(0.01f));
    Test::RequireDefault("editor.camera.orthoMaxHalfHeight", CVarValue::Float32(1.0e6f));
    Test::RequireDefault("editor.camera.frameFill", CVarValue::Float32(0.9f));
    Test::RequireDefault("editor.camera.minOrbitDistance", CVarValue::Float32(0.05f));
    Test::RequireDefault("editor.camera.maxOrbitDistance", CVarValue::Float32(1.0e5f));
    Test::RequireDefault("editor.camera.nearClip", CVarValue::Float32(0.05f));
    Test::RequireDefault("editor.camera.farClip", CVarValue::Float32(5000.0f));
    Test::RequireDefault("editor.camera.baseFlySpeed", CVarValue::Float32(5.0f));
    Test::RequireDefault("editor.camera.distanceScaledSpeed", CVarValue::Bool(true));
    Test::RequireDefault("editor.camera.refDistance", CVarValue::Float32(10.0f));
    Test::RequireDefault("editor.camera.floor", CVarValue::Float32(0.1f));
    Test::RequireDefault("editor.camera.speedCap", CVarValue::Float32(1000.0f));
    Test::RequireDefault("editor.camera.lookSensitivity", CVarValue::Float32(0.2f));
    Test::RequireDefault("editor.camera.orbitSensitivity", CVarValue::Float32(0.2f));
    Test::RequireDefault("editor.camera.boostMultiplier", CVarValue::Float32(2.0f));
    Test::RequireDefault("editor.camera.speedWheelStep", CVarValue::Float32(1.1f));

    // The Dev rows (inventory "Editor Dev") and the fresh-camera pose (NextWorld).
    const auto flagsOf = [](std::string_view n) { return CVarRegistry::Get().Describe(n)->flags; };
    CHECK(HasFlag(flagsOf("editor.camera.orthoMinHalfHeight"), CVarFlags::Dev));
    CHECK(HasFlag(flagsOf("editor.camera.default3DYaw"), CVarFlags::Dev));
    CHECK_FALSE(HasFlag(flagsOf("editor.camera.wheelZoomStep"), CVarFlags::Dev));
    CHECK(CVarRegistry::Get().Describe("editor.camera.default3DPitch")->apply == ApplyMode::NextWorld);
    CHECK(CVarRegistry::Get().Describe("editor.camera.nearClip")->apply == ApplyMode::Live);
}

TEST_CASE("sweep: the fresh-camera pose follows editor.camera.default*", "[sweep][editor-camera]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    EditorCamera cam;
    ApplyFreshPose(cam);   // at the defaults: unchanged
    CHECK(cam.ortho.halfHeight == 5.0f); CHECK(cam.orbit.yawDeg == -30.0f);
    CHECK(cam.orbit.pitchDeg == 30.0f);  CHECK(cam.orbit.distance == 10.0f);

    reg.Set(reg.Find("editor.camera.default2DHalfHeight"), CVarValue::Float32(8.0f), SetBy::Code);
    reg.Set(reg.Find("editor.camera.default3DYaw"), CVarValue::Float32(45.0f), SetBy::Code);
    reg.Set(reg.Find("editor.camera.default3DPitch"), CVarValue::Float32(15.0f), SetBy::Code);
    reg.Set(reg.Find("editor.camera.default3DDistance"), CVarValue::Float32(20.0f), SetBy::Code);
    reg.PublishImmediate();
    cam.mode = ViewMode::Perspective; cam.ortho.center = { 3.0f, 4.0f };
    ApplyFreshPose(cam);
    CHECK(cam.ortho.halfHeight == 8.0f); CHECK(cam.orbit.yawDeg == 45.0f);
    CHECK(cam.orbit.pitchDeg == 15.0f);  CHECK(cam.orbit.distance == 20.0f);
    CHECK(cam.mode == ViewMode::Perspective);          // the pose only: mode and centre untouched
    CHECK(cam.ortho.center.x == 3.0f);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}

TEST_CASE("sweep: clip planes, sensitivities, boost and the distance scale follow the snapshot", "[sweep][editor-camera]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    const auto set = [&](std::string_view n, const CVarValue& v) { reg.Set(reg.Find(n), v, SetBy::Code); };

    EditorCamera cam; cam.mode = ViewMode::Perspective;
    const glm::uvec2 vp{ 100u, 100u };
    const auto projWith = [&](float nearZ, float farZ)
    { return ViewTransform::Perspective(cam.Eye(), cam.orbit.pivot, glm::vec3(0, 1, 0), cam.orbit.fovYDeg, vp, nearZ, farZ).projection; };
    CHECK(cam.Resolve(vp).projection == projWith(0.05f, 5000.0f));

    set("editor.camera.nearClip", CVarValue::Float32(0.5f));
    set("editor.camera.farClip", CVarValue::Float32(200.0f));
    set("editor.camera.lookSensitivity", CVarValue::Float32(1.0f));
    set("editor.camera.orbitSensitivity", CVarValue::Float32(0.5f));
    set("editor.camera.boostMultiplier", CVarValue::Float32(4.0f));
    set("editor.camera.distanceScaledSpeed", CVarValue::Bool(false));
    set("editor.camera.speedWheelStep", CVarValue::Float32(2.0f));
    reg.PublishImmediate();

    CHECK(cam.Resolve(vp).projection == projWith(0.5f, 200.0f));

    cam.orbit.yawDeg = 0.0f; cam.Orbit({ 10.0f, 0.0f });
    CHECK(cam.orbit.yawDeg == -5.0f);
    cam.orbit.yawDeg = 0.0f; cam.Look({ 10.0f, 0.0f });
    CHECK(cam.orbit.yawDeg == -10.0f);

    // Distance scaling off: 5 m/s x 4 boost regardless of a 100 m pivot.
    cam.orbit.distance = 100.0f; cam.speedScalar = 1.0f;
    const glm::vec3 p0 = cam.orbit.pivot;
    cam.Fly({ 1.0f, 0.0f, 0.0f }, 1.0f, /*boost*/ true);
    const glm::vec3 moved = cam.orbit.pivot - p0;
    CHECK(std::abs(std::sqrt(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z) - 20.0f) < 1e-3f);

    cam.speedScalar = 1.0f; cam.AdjustSpeed(1.0f);
    CHECK(cam.speedScalar == 2.0f);

    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}
