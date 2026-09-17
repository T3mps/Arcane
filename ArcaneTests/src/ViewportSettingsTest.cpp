// Arcane Editor viewport settings persistence (F4 plan 1 T7): the PURE
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

TEST_CASE("Viewport settings ini lines round-trip the camera, mode, grid and speed", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam; cam.mode = Arcane::Editor::ViewMode::Perspective;
    cam.ortho = { {1.5f, -2.0f}, 3.25f }; cam.orbit = { {1,2,3}, 12.0f, -8.0f, 42.0f, 55.0f }; cam.speedScalar = 2.5f;
    Arcane::Editor::ViewportSettings s; s.showGrid = false; s.gridPlane = Arcane::Editor::GridPlane::XY; s.gizmoSize = 1.5f;
    ImGuiTextBuffer buf;
    Arcane::Editor::ViewportSettings::WriteIni(buf, cam, s);
    Arcane::Editor::EditorCamera back; Arcane::Editor::ViewportSettings sb;
    for (const std::string& line : SplitLines(buf.c_str()))   // a tiny local helper; skips the [section] header
        CHECK(Arcane::Editor::ViewportSettings::ReadIniLine(line.c_str(), back, sb));
    CHECK(back.mode == cam.mode); CHECK(back.ortho.halfHeight == Approx(3.25f)); CHECK(back.orbit.distance == Approx(42.0f));
    CHECK(back.orbit.yawDeg == Approx(12.0f)); CHECK(back.speedScalar == Approx(2.5f));
    CHECK(back.ortho.center.x == Approx(1.5f)); CHECK(back.ortho.center.y == Approx(-2.0f));
    CHECK(back.orbit.pivot.x == Approx(1.0f)); CHECK(back.orbit.pivot.y == Approx(2.0f)); CHECK(back.orbit.pivot.z == Approx(3.0f));
    CHECK(back.orbit.pitchDeg == Approx(-8.0f)); CHECK(back.orbit.fovYDeg == Approx(55.0f));
    CHECK_FALSE(sb.showGrid); CHECK(sb.gridPlane == Arcane::Editor::GridPlane::XY); CHECK(sb.gizmoSize == Approx(1.5f));
}

TEST_CASE("A malformed or out-of-range ini line leaves the defaults", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam; Arcane::Editor::ViewportSettings s;
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Mode=7", cam, s));
    CHECK(cam.mode == Arcane::Editor::ViewMode::TwoD);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=nan", cam, s));
    CHECK(cam.orbit.distance == Approx(10.0f));

    // Task 6 note: a persisted pitch AT +-90 would make Right()/Up() NaN, so the
    // read refuses it (and everything past the clamps) rather than loading it.
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 90 10 60", cam, s));
    CHECK(cam.orbit.pitchDeg == Approx(30.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 0 0 60", cam, s));   // distance below kMinDistance
    CHECK(cam.orbit.distance == Approx(10.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Ortho=0 0 0", cam, s));            // halfHeight below kMinHalfHeight
    CHECK(cam.ortho.halfHeight == Approx(5.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Ortho=inf 0 1", cam, s));
    CHECK(cam.ortho.center.x == Approx(0.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Speed=0", cam, s));
    CHECK(cam.speedScalar == Approx(1.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Grid=1 2", cam, s));
    CHECK(s.gridPlane == Arcane::Editor::GridPlane::XZ);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("GizmoSize=-1", cam, s));
    CHECK(s.gizmoSize == Approx(1.0f));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Bogus=1", cam, s));
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("", cam, s));

    // A pitch just inside the lock is accepted as written.
    CHECK(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=0 0 0 0 89.5 10 60", cam, s));
    CHECK(cam.orbit.pitchDeg == Approx(89.5f));
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
