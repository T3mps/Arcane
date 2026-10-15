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
// POSE only (mode and both persisted transforms) -- layout state. The view
// PREFERENCES (grid, plane, fov, speed, gizmo size) are Pref-P cvars since
// settings S6-29 (Settings/EditorViewportSettings.hpp): an old block's
// Speed= / Grid= / GizmoSize= lines and the seventh Orbit= value (fov) are
// still read and validated, but CAPTURED into LegacyViewportPrefs for a
// one-time import, never applied and never written again.
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

#include "Settings/EditorViewportSettings.hpp"   // GridPlane, LegacyViewportPrefs
#include "Viewport/EditorCamera.hpp"

#include <cstdint>
#include <string_view>
#include <Arcane/Core/Constant.hpp>

struct ImGuiTextBuffer;

namespace Arcane::Editor
{
    struct ViewportSettings
    {
        // Accepted vertical field of view, degrees: the open interval (0, 180)
        // held away from both ends so the projection stays finite.
        ARC_CONSTANT("math identity: the projection is finite only inside (0, 180) degrees")
        static constexpr float kMinFovYDeg = 1.0f;
        ARC_CONSTANT("math identity: the projection is finite only inside (0, 180) degrees")
        static constexpr float kMaxFovYDeg = 179.0f;
        // Strictly inside the +-90 pitch lock EditorCamera::Look/Orbit apply.
        ARC_CONSTANT("math identity: Right()/Up() are NaN at +-90 degrees of pitch")
        static constexpr float kMaxPitchDeg = 90.0f - 1e-3f;

        // The ini section: "[EditorViewport][Camera]".
        static constexpr const char* kIniType = "EditorViewport";
        static constexpr const char* kIniName = "Camera";

        // Appends the section header and every line:
        //   Mode=%d                      ViewMode
        //   Ortho=%f %f %f               center.x center.y halfHeight
        //   Orbit=%f %f %f %f %f %f      pivot.xyz yaw pitch distance
        // followed by the blank line ImGui's own handlers end a section with.
        // No preference lines (they are cvars).
        static void WriteIni(ImGuiTextBuffer& buf, const EditorCamera& cam);

        // Applies ONE line. Returns true only when the line parsed completely
        // and every value is finite and within range, in which case the
        // corresponding state is written; otherwise nothing is touched and
        // false comes back (the unknown-line and malformed-line cases alike).
        // Orbit= takes six values, or the legacy seven whose last (fov) goes
        // to `legacy.fovYDeg`; Speed= / Grid= / GizmoSize= fill `legacy`.
        static bool ReadIniLine(const char* line, EditorCamera& cam, LegacyViewportPrefs& legacy);
    };

    // --view-mode: "2d" | "perspective" set the camera's mode; anything else
    // (the empty default = "no seed") leaves it alone. HostConfig::Parse has
    // already refused every other spelling. Applied AFTER the persisted block
    // is read so the flag beats the ini -- see EditorApp::StageFinalize and
    // the ViewportSettingsReadLine callback for the two sites and why both.
    void ApplyViewModeSeed(std::string_view flag, EditorCamera& cam) noexcept;
}
