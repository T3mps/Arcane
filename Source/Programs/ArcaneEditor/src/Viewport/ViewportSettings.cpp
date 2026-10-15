#include "Viewport/ViewportSettings.hpp"

#include "Settings/EditorViewportSettings.hpp"

#include <Arcane/Config/Settings.hpp>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Arcane::Editor
{
    namespace
    {
        bool Finite(float v) noexcept { return std::isfinite(v); }

        bool InRange(float v, float lo, float hi) noexcept
        {
            // `v >= lo && v <= hi` is already false for NaN, but the explicit
            // finiteness check keeps +-inf out of a range whose end is inf-free.
            return Finite(v) && v >= lo && v <= hi;
        }
    }

    void ViewportSettings::WriteIni(ImGuiTextBuffer& buf, const EditorCamera& cam)
    {
        buf.reserve(buf.size() + 192);
        buf.appendf("[%s][%s]\n", kIniType, kIniName);
        buf.appendf("Mode=%d\n", static_cast<int>(cam.mode));
        buf.appendf("Ortho=%f %f %f\n", cam.ortho.center.x, cam.ortho.center.y, cam.ortho.halfHeight);
        buf.appendf("Orbit=%f %f %f %f %f %f\n",
                    cam.orbit.pivot.x, cam.orbit.pivot.y, cam.orbit.pivot.z,
                    cam.orbit.yawDeg, cam.orbit.pitchDeg, cam.orbit.distance);
        buf.append("\n");
    }

    bool ViewportSettings::ReadIniLine(const char* line, EditorCamera& cam, LegacyViewportPrefs& legacy)
    {
        if (line == nullptr || *line == 0)
            return false;

        // Every branch parses into LOCALS, validates, and only then writes --
        // a line that fails halfway must not leave a half-applied transform.
        // The trailing "%c" catches junk after a complete match (sscanf would
        // otherwise accept "Mode=1garbage"): a clean line leaves it unmatched,
        // so the conversion count is exactly the field count.
        char trailing = 0;
        if (std::strncmp(line, "Mode=", 5) == 0)
        {
            int mode = -1;
            if (std::sscanf(line, "Mode=%d%c", &mode, &trailing) != 1) return false;
            if (mode < static_cast<int>(ViewMode::TwoD) || mode > static_cast<int>(ViewMode::Perspective)) return false;
            cam.mode = static_cast<ViewMode>(mode);
            return true;
        }
        if (std::strncmp(line, "Ortho=", 6) == 0)
        {
            float cx = 0, cy = 0, hh = 0;
            if (std::sscanf(line, "Ortho=%f %f %f%c", &cx, &cy, &hh, &trailing) != 3) return false;
            if (!Finite(cx) || !Finite(cy)) return false;
            const EditorCameraSettings& camPrefs = Settings<EditorCameraSettings>();   // the camera's own clamps
            if (!InRange(hh, camPrefs.orthoMinHalfHeight, camPrefs.orthoMaxHalfHeight)) return false;
            cam.ortho.center     = { cx, cy };
            cam.ortho.halfHeight = hh;
            return true;
        }
        if (std::strncmp(line, "Orbit=", 6) == 0)
        {
            // Six values (the pose), or the legacy seven whose last is the
            // fov -- a preference since S6-29, captured for the import.
            float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, dist = 0, fov = 0;
            // Each form is matched with its own trailing "%c": a six-value
            // scan of "... 6x" stops at 'x' with 6 conversions, so only the
            // six-value FORMAT's %c can tell it from a clean line.
            int n = 0;
            if (std::sscanf(line, "Orbit=%f %f %f %f %f %f %f%c",
                            &px, &py, &pz, &yaw, &pitch, &dist, &fov, &trailing) == 7)
                n = 7;
            else if (std::sscanf(line, "Orbit=%f %f %f %f %f %f%c",
                                 &px, &py, &pz, &yaw, &pitch, &dist, &trailing) == 6)
                n = 6;
            else
                return false;
            if (!Finite(px) || !Finite(py) || !Finite(pz) || !Finite(yaw)) return false;
            // Strictly inside the lock: +-90 exactly makes Right()/Up() NaN.
            if (!InRange(pitch, -kMaxPitchDeg, kMaxPitchDeg)) return false;
            const EditorCameraSettings& camPrefs = Settings<EditorCameraSettings>();   // the camera's own clamps
            if (!InRange(dist, camPrefs.minOrbitDistance, camPrefs.maxOrbitDistance)) return false;
            if (n == 7 && !InRange(fov, kMinFovYDeg, kMaxFovYDeg)) return false;
            cam.orbit.pivot    = { px, py, pz };
            cam.orbit.yawDeg   = yaw;
            cam.orbit.pitchDeg = pitch;
            cam.orbit.distance = dist;
            if (n == 7) legacy.fovYDeg = fov;
            return true;
        }
        if (std::strncmp(line, "Speed=", 6) == 0)
        {
            float speed = 0;
            if (std::sscanf(line, "Speed=%f%c", &speed, &trailing) != 1) return false;
            const auto [lo, hi] = RegisteredFloatRange("editor.camera.speedScalar");
            if (!InRange(speed, lo, hi)) return false;
            legacy.speedScalar = speed;
            return true;
        }
        if (std::strncmp(line, "Grid=", 5) == 0)
        {
            int show = -1, plane = -1;
            if (std::sscanf(line, "Grid=%d %d%c", &show, &plane, &trailing) != 2) return false;
            if (show < 0 || show > 1) return false;
            if (plane < static_cast<int>(GridPlane::XZ) || plane > static_cast<int>(GridPlane::XY)) return false;
            legacy.showGrid  = show == 1;
            legacy.gridPlane = static_cast<GridPlane>(plane);
            return true;
        }
        if (std::strncmp(line, "GizmoSize=", 10) == 0)
        {
            float size = 0;
            if (std::sscanf(line, "GizmoSize=%f%c", &size, &trailing) != 1) return false;
            const auto [lo, hi] = RegisteredFloatRange("editor.gizmo.size");
            if (!InRange(size, lo, hi)) return false;
            legacy.gizmoSize = size;
            return true;
        }
        return false;   // an unknown key: ignored, never trusted
    }

    void ApplyViewModeSeed(std::string_view flag, EditorCamera& cam) noexcept
    {
        if (flag == "perspective")  cam.mode = ViewMode::Perspective;
        else if (flag == "2d")      cam.mode = ViewMode::TwoD;
        // "" (absent), and nothing else reaches here: HostConfig refused it.
    }
}
