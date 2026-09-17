#pragma once

// The editor viewport's PERSISTED preferences (F4 plan 1 T7) and the pure
// [EditorViewport][Camera] ini line writer/reader behind them.
//
// What persists, and why HERE rather than in EditorCamera: the camera struct is
// pure math the [editor][camera] units drive (EditorCamera.hpp); the ini line
// format is a SERIALISATION concern with its own refusal table (finiteness,
// the camera's clamps, enum ranges), so it lives beside the settings it also
// serialises. EditorApp's ImGuiSettingsHandler callbacks wrap WriteIni /
// ReadIniLine and nothing else -- same split as PanelVisibility's
// ParsePanelVisibilityLine -- so ArcaneTests drives the round-trip with a
// bare ImGuiTextBuffer and no ImGui context.
//
// The section is ONE block, "[EditorViewport][Camera]", carrying the camera
// (mode, both persisted transforms, the speed scalar) AND the view settings
// (grid, gizmo size): they are one preference set the viewport's settings
// popup (Task 8) edits together, and the grids (Tasks 9/10) read here.
//
// Every line is validated on read and REFUSED WHOLE on any fault -- a hand
// edit, a NaN, a value past the camera's clamps -- leaving the defaults
// (EditorApp.cpp's PlayMode handler: "never trust an ini line"). The pitch
// refusal is the important one: EditorCamera::Right()/Up() are NaN at exactly
// +-90 deg, so a persisted 90 would boot a camera whose every 3D op poisons
// the view. The read therefore accepts only what the ops themselves would
// have produced, strictly inside the lock.
//
// The headless verify layout (ReferenceProject/Saved/verify-layout.ini)
// carries NO [EditorViewport] block on purpose: goldens run at these defaults
// (2D) unless --view-mode says otherwise.

#include "Viewport/EditorCamera.hpp"

#include <cstdint>
#include <string_view>

struct ImGuiTextBuffer;

namespace Arcane::Editor
{
    // Which world plane the reference grid lies on. Persisted as int; append
    // only. XZ is the 3D "ground" (+Y up, spec s2); XY is the 2D authoring
    // plane the Ortho2D view looks down -Z at.
    enum class GridPlane : std::uint8_t
    {
        XZ = 0,
        XY = 1,
    };

    struct ViewportSettings
    {
        bool      showGrid  = true;
        GridPlane gridPlane = GridPlane::XZ;
        float     gizmoSize = 1.0f;

        // Accepted range of the persisted gizmo scale (Task 8's slider spans
        // 0.5..3; the clamp is wider so a future slider range needs no ini
        // migration, but a zero or negative scale is still refused).
        static constexpr float kMinGizmoSize = 0.1f;
        static constexpr float kMaxGizmoSize = 10.0f;
        // Accepted vertical field of view, degrees: the open interval (0, 180)
        // held away from both ends so the projection stays finite.
        static constexpr float kMinFovYDeg = 1.0f;
        static constexpr float kMaxFovYDeg = 179.0f;
        // The speed scalar's clamp, as EditorCamera::AdjustSpeed holds it.
        static constexpr float kMinSpeedScalar = 0.01f;
        static constexpr float kMaxSpeedScalar = 100.0f;
        // Strictly inside the +-90 pitch lock EditorCamera::Look/Orbit apply.
        static constexpr float kMaxPitchDeg = 90.0f - 1e-3f;

        // The ini section: "[EditorViewport][Camera]".
        static constexpr const char* kIniType = "EditorViewport";
        static constexpr const char* kIniName = "Camera";

        // Appends the section header and every line:
        //   Mode=%d                      ViewMode
        //   Ortho=%f %f %f               center.x center.y halfHeight
        //   Orbit=%f %f %f %f %f %f %f   pivot.xyz yaw pitch distance fovY
        //   Speed=%f                     speedScalar
        //   Grid=%d %d                   showGrid gridPlane
        //   GizmoSize=%f
        // followed by the blank line ImGui's own handlers end a section with.
        static void WriteIni(ImGuiTextBuffer& buf, const EditorCamera& cam, const ViewportSettings& s);

        // Applies ONE line. Returns true only when the line parsed completely
        // and every value is finite and within range, in which case the
        // corresponding state is written; otherwise nothing is touched and
        // false comes back (the unknown-line and malformed-line cases alike).
        static bool ReadIniLine(const char* line, EditorCamera& cam, ViewportSettings& s);
    };

    // --view-mode: "2d" | "perspective" set the camera's mode; anything else
    // (the empty default = "no seed") leaves it alone. HostConfig::Parse has
    // already refused every other spelling. Applied AFTER the persisted block
    // is read so the flag beats the ini -- see EditorApp::StageFinalize and
    // the ViewportSettingsReadLine callback for the two sites and why both.
    void ApplyViewModeSeed(std::string_view flag, EditorCamera& cam) noexcept;
}
