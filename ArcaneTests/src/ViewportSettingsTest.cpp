// Arcane Editor viewport pose persistence (F4 plan 1 T7; the preferences left
// for cvars in settings S6-29): the PURE
// [EditorViewport][Camera] ini line writer/reader that the EditorApp's
// ImGuiSettingsHandler callbacks wrap. Driven here with a bare ImGuiTextBuffer
// and no ImGui context -- appendf needs none -- so the round-trip and the
// refusal table run headlessly ([editor][settings]).

#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <Viewport/EditorCamera.hpp>
#include <Viewport/ViewportSettings.hpp>

using Catch::Approx;

namespace
{
    // Every non-empty line that is not a "[section][name]" header, so the
    // reader is fed exactly what ImGui's ReadLineFn would receive.
    std::vector<std::string> SplitLines(const char* text)
    {
        std::vector<std::string> out;
        std::string cur;
        for (const char* p = text; *p; ++p)
        {
            if (*p == '\n')
            {
                if (!cur.empty() && cur.front() != '[')
                    out.push_back(cur);
                cur.clear();
            }
            else if (*p != '\r')
            {
                cur += *p;
            }
        }
        if (!cur.empty() && cur.front() != '[')
            out.push_back(cur);
        return out;
    }
}

TEST_CASE("Viewport settings ini lines round-trip the camera pose and mode", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam; cam.mode = Arcane::Editor::ViewMode::Perspective;
    cam.ortho = { {1.5f, -2.0f}, 3.25f }; cam.orbit = { {1,2,3}, 12.0f, -8.0f, 42.0f, 55.0f }; cam.speedScalar = 2.5f;
    ImGuiTextBuffer buf;
    Arcane::Editor::ViewportSettings::WriteIni(buf, cam);
    Arcane::Editor::EditorCamera back; Arcane::Editor::LegacyViewportPrefs legacy;
    for (const std::string& line : SplitLines(buf.c_str()))   // a tiny local helper; skips the [section] header
        CHECK(Arcane::Editor::ViewportSettings::ReadIniLine(line.c_str(), back, legacy));
    CHECK(back.mode == cam.mode); CHECK(back.ortho.halfHeight == Approx(3.25f)); CHECK(back.orbit.distance == Approx(42.0f));
    CHECK(back.orbit.yawDeg == Approx(12.0f));
    CHECK(back.ortho.center.x == Approx(1.5f)); CHECK(back.ortho.center.y == Approx(-2.0f));
    CHECK(back.orbit.pivot.x == Approx(1.0f)); CHECK(back.orbit.pivot.y == Approx(2.0f)); CHECK(back.orbit.pivot.z == Approx(3.0f));
    CHECK(back.orbit.pitchDeg == Approx(-8.0f));
    // The preferences are cvars (settings S6-29): the block carries none, so
    // the read leaves the camera's runtime copies and the legacy capture alone.
    CHECK(back.orbit.fovYDeg == Approx(60.0f)); CHECK(back.speedScalar == Approx(1.0f));
    CHECK_FALSE(legacy.Any());
}

TEST_CASE("A malformed or out-of-range ini line leaves the defaults", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam; Arcane::Editor::LegacyViewportPrefs legacy;
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Mode=7", cam, legacy));
    CHECK(cam.mode == Arcane::Editor::ViewMode::TwoD);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=nan", cam, legacy));
    CHECK(cam.orbit.distance == Approx(10.0f));

    // Task 6 note: a persisted pitch AT +-90 would make Right()/Up() NaN, so the
    // read refuses it (and everything past the clamps) rather than loading it.
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 90 10 60", cam, legacy));
    CHECK(cam.orbit.pitchDeg == Approx(30.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 0 0 60", cam, legacy));   // distance below kMinDistance
    CHECK(cam.orbit.distance == Approx(10.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 10 10 0", cam, legacy));  // legacy fov below kMinFovYDeg
    CHECK(cam.orbit.pitchDeg == Approx(30.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 10 10x", cam, legacy));   // junk after six values
    CHECK(cam.orbit.pitchDeg == Approx(30.0f));
    CHECK_FALSE(legacy.fovYDeg);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Ortho=0 0 0", cam, legacy));            // halfHeight below kMinHalfHeight
    CHECK(cam.ortho.halfHeight == Approx(5.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Ortho=inf 0 1", cam, legacy));
    CHECK(cam.ortho.center.x == Approx(0.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Speed=0", cam, legacy));
    CHECK(cam.speedScalar == Approx(1.0f));
    CHECK_FALSE(legacy.speedScalar);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Grid=1 2", cam, legacy));
    CHECK_FALSE(legacy.gridPlane);
    CHECK_FALSE(legacy.showGrid);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("GizmoSize=-1", cam, legacy));
    CHECK_FALSE(legacy.gizmoSize);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Bogus=1", cam, legacy));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("", cam, legacy));
    CHECK_FALSE(legacy.Any());

    // A pitch just inside the lock is accepted as written; the six-value pose
    // form (what WriteIni now writes) captures no fov.
    CHECK(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 89.5 10", cam, legacy));
    CHECK(cam.orbit.pitchDeg == Approx(89.5f));
    CHECK_FALSE(legacy.fovYDeg);
    CHECK(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 89.5 10 60", cam, legacy));
    REQUIRE(legacy.fovYDeg); CHECK(*legacy.fovYDeg == Approx(60.0f));
}

TEST_CASE("--view-mode seeds the camera mode; an empty flag leaves it alone", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam;
    Arcane::Editor::ApplyViewModeSeed("perspective", cam);
    CHECK(cam.mode == Arcane::Editor::ViewMode::Perspective);
    Arcane::Editor::ApplyViewModeSeed("", cam);
    CHECK(cam.mode == Arcane::Editor::ViewMode::Perspective);
    Arcane::Editor::ApplyViewModeSeed("2d", cam);
    CHECK(cam.mode == Arcane::Editor::ViewMode::TwoD);
}
