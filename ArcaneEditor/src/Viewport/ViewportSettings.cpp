#include "Viewport/ViewportSettings.hpp"

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

    void ViewportSettings::WriteIni(ImGuiTextBuffer& buf, const EditorCamera& cam, const ViewportSettings& s)
    {
        buf.reserve(buf.size() + 256);
        buf.appendf("[%s][%s]\n", kIniType, kIniName);
        buf.appendf("Mode=%d\n", static_cast<int>(cam.mode));
        buf.appendf("Ortho=%f %f %f\n", cam.ortho.center.x, cam.ortho.center.y, cam.ortho.halfHeight);
        buf.appendf("Orbit=%f %f %f %f %f %f %f\n",
                    cam.orbit.pivot.x, cam.orbit.pivot.y, cam.orbit.pivot.z,
                    cam.orbit.yawDeg, cam.orbit.pitchDeg, cam.orbit.distance, cam.orbit.fovYDeg);
        buf.appendf("Speed=%f\n", cam.speedScalar);
        buf.appendf("Grid=%d %d\n", s.showGrid ? 1 : 0, static_cast<int>(s.gridPlane));
        buf.appendf("GizmoSize=%f\n", s.gizmoSize);
        buf.append("\n");
    }

    bool ViewportSettings::ReadIniLine(const char* line, EditorCamera& cam, ViewportSettings& s)
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
            if (!InRange(hh, EditorCamera::kMinHalfHeight, EditorCamera::kMaxHalfHeight)) return false;
            cam.ortho.center     = { cx, cy };
            cam.ortho.halfHeight = hh;
            return true;
        }
        if (std::strncmp(line, "Orbit=", 6) == 0)
        {
            float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, dist = 0, fov = 0;
            if (std::sscanf(line, "Orbit=%f %f %f %f %f %f %f%c",
                            &px, &py, &pz, &yaw, &pitch, &dist, &fov, &trailing) != 7) return false;
            if (!Finite(px) || !Finite(py) || !Finite(pz) || !Finite(yaw)) return false;
            // Strictly inside the lock: +-90 exactly makes Right()/Up() NaN.
            if (!InRange(pitch, -kMaxPitchDeg, kMaxPitchDeg)) return false;
            if (!InRange(dist, EditorCamera::kMinDistance, EditorCamera::kMaxDistance)) return false;
            if (!InRange(fov, kMinFovYDeg, kMaxFovYDeg)) return false;
            cam.orbit.pivot    = { px, py, pz };
            cam.orbit.yawDeg   = yaw;
            cam.orbit.pitchDeg = pitch;
            cam.orbit.distance = dist;
            cam.orbit.fovYDeg  = fov;
            return true;
        }
        if (std::strncmp(line, "Speed=", 6) == 0)
        {
            float speed = 0;
            if (std::sscanf(line, "Speed=%f%c", &speed, &trailing) != 1) return false;
            if (!InRange(speed, kMinSpeedScalar, kMaxSpeedScalar)) return false;
            cam.speedScalar = speed;
            return true;
        }
        if (std::strncmp(line, "Grid=", 5) == 0)
        {
            int show = -1, plane = -1;
            if (std::sscanf(line, "Grid=%d %d%c", &show, &plane, &trailing) != 2) return false;
            if (show < 0 || show > 1) return false;
            if (plane < static_cast<int>(GridPlane::XZ) || plane > static_cast<int>(GridPlane::XY)) return false;
            s.showGrid  = show == 1;
            s.gridPlane = static_cast<GridPlane>(plane);
            return true;
        }
        if (std::strncmp(line, "GizmoSize=", 10) == 0)
        {
            float size = 0;
            if (std::sscanf(line, "GizmoSize=%f%c", &size, &trailing) != 1) return false;
            if (!InRange(size, kMinGizmoSize, kMaxGizmoSize)) return false;
            s.gizmoSize = size;
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
