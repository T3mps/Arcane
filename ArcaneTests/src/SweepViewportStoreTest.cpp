// Settings sweep S6-29 (inventory Part 3 "Viewport/Camera/Gizmo", "Persistence
// stores" [EditorViewport][Camera]): the viewport PREFERENCES (grid, plane,
// fov, speed, gizmo size) are Pref-P cvars, imported once from an old ini
// block and never written there again; the camera POSE stays ini state.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorViewportSettings.hpp"
#include "Viewport/ViewportSettings.hpp"
#include <Arcane/Config/Settings.hpp>
#include <imgui.h>
#include <string>
using namespace Arcane;
using namespace Arcane::Editor;

namespace
{
    // Reverts the User rung the import writes even when a check fails midway.
    struct UserLayerReset
    {
        ~UserLayerReset()
        {
            CVarRegistry::Get().RevertLayer(SetBy::User);
            CVarRegistry::Get().PublishImmediate();
        }
    };
}

TEST_CASE("sweep: the viewport preferences keep their defaults", "[sweep][viewport-store]")
{
    CHECK(EditorViewportSettings{}.showGrid);
    CHECK(EditorViewportSettings{}.gridPlane == GridPlane::XZ);
    CHECK(EditorCameraSettings{}.fovYDeg == 60.0f);
    CHECK(EditorCameraSettings{}.speedScalar == 1.0f);
    CHECK(EditorGizmoSettings{}.size == 1.0f);
    Test::RequireDefault("editor.gizmo.size", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.viewport.gridPlane", CVarValue::Enum(0));
    // The camera's own runtime defaults equal the cvars' (the pose before the
    // first sync is the same view).
    CHECK(EditorCamera{}.orbit.fovYDeg == EditorCameraSettings{}.fovYDeg);
    CHECK(EditorCamera{}.speedScalar == EditorCameraSettings{}.speedScalar);
}

TEST_CASE("sweep: an old [EditorViewport][Camera] block is imported once and no longer written", "[sweep][viewport-store]")
{
    UserLayerReset reset;
    EditorCamera cam;
    LegacyViewportPrefs legacy;
    CHECK(ViewportSettings::ReadIniLine("Orbit=1 2 3 -30 30 10 75", cam, legacy));   // legacy 7-value line
    CHECK(ViewportSettings::ReadIniLine("Speed=4", cam, legacy));
    CHECK(ViewportSettings::ReadIniLine("Grid=0 1", cam, legacy));
    CHECK(ViewportSettings::ReadIniLine("GizmoSize=2", cam, legacy));
    CHECK(cam.orbit.pivot == glm::vec3(1, 2, 3));                                     // the pose is still ini state
    REQUIRE(legacy.fovYDeg); CHECK(*legacy.fovYDeg == 75.0f);
    CVarRegistry& reg = CVarRegistry::Get();
    CHECK(ImportLegacyViewportPrefs(reg, legacy) == 5u);
    reg.PublishImmediate();
    CHECK(Settings<EditorCameraSettings>().fovYDeg == 75.0f);
    CHECK(Settings<EditorCameraSettings>().speedScalar == 4.0f);
    CHECK_FALSE(Settings<EditorViewportSettings>().showGrid);
    CHECK(Settings<EditorViewportSettings>().gridPlane == GridPlane::XY);
    CHECK(Settings<EditorGizmoSettings>().size == 2.0f);
    CHECK(ImportLegacyViewportPrefs(reg, legacy) == 0u);                              // once: the User rung now holds them
    ImGuiTextBuffer buf;
    ViewportSettings::WriteIni(buf, cam);
    const std::string text = buf.c_str();
    CHECK(text.find("Speed=") == std::string::npos);
    CHECK(text.find("Grid=") == std::string::npos);
    CHECK(text.find("GizmoSize=") == std::string::npos);
    CHECK(text.find("Orbit=1.000000 2.000000 3.000000 -30.000000 30.000000 10.000000\n") != std::string::npos);
}

TEST_CASE("sweep: a User value the user already chose beats the old ini block", "[sweep][viewport-store]")
{
    UserLayerReset reset;
    CVarRegistry& reg = CVarRegistry::Get();
    REQUIRE(reg.Set(reg.Find("editor.camera.speedScalar"), CVarValue::Float32(3.0f), SetBy::User, "user") == SetResult::Applied);
    LegacyViewportPrefs legacy;
    legacy.speedScalar = 7.0f;
    legacy.gizmoSize   = 1.5f;
    CHECK(ImportLegacyViewportPrefs(reg, legacy) == 1u);   // the gizmo only
    reg.PublishImmediate();
    CHECK(Settings<EditorCameraSettings>().speedScalar == 3.0f);
    CHECK(Settings<EditorGizmoSettings>().size == 1.5f);
}

TEST_CASE("sweep: the camera's speed clamp is the cvar's range", "[sweep][viewport-store]")
{
    const auto [lo, hi] = CameraSpeedScalarRange();
    CHECK(lo == 0.01f);
    CHECK(hi == 100.0f);
    EditorCamera cam;
    cam.AdjustSpeed(-1000.0f); CHECK(cam.speedScalar == lo);
    cam.AdjustSpeed(+1000.0f); CHECK(cam.speedScalar == hi);
}
