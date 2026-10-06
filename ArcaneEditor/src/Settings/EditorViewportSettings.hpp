#pragma once

// editor.viewport.* / editor.camera.* / editor.gizmo.* (settings arc S6-29;
// inventory Part 3 "Viewport/Camera/Gizmo"): the viewport PREFERENCES the
// [EditorViewport][Camera] imgui.ini block used to carry. Per-project editor
// preferences (Pref-P), Live. The camera POSE (mode, ortho, orbit) stays
// layout state in that block (ViewportSettings.hpp); an old block's
// preference lines are imported once (ImportLegacyViewportPrefs) and never
// written again.
//
// EditorCamera::orbit.fovYDeg / speedScalar stay runtime members, synced from
// the snapshot every frame; the wheel and the view-settings popup write the
// cvars (SetBy::User) and the camera picks them up next frame.

#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Reflection.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace Arcane { class CVarRegistry; }

namespace Arcane::Editor
{
    // Which world plane the reference grid lies on. Stored as the declared
    // ordinal (append only). XZ is the 3D "ground" (+Y up, spec s2); XY is the
    // 2D authoring plane the Ortho2D view looks down -Z at.
    enum class GridPlane : std::uint8_t
    {
        XZ = 0,
        XY = 1,
    };

    ARC_REFLECT_ENUM(GridPlane)
        ARC_REFLECT_ENUM_VALUE(GridPlane, XZ)
        ARC_REFLECT_ENUM_VALUE(GridPlane, XY)
    ARC_END_REFLECT_ENUM()

    struct EditorViewportSettings
    {
        bool      showGrid  = true;
        GridPlane gridPlane = GridPlane::XZ;
    };

    struct EditorCameraSettings
    {
        float fovYDeg     = 60.0f;   // Range 1..179: the projection stays finite
        float speedScalar = 1.0f;    // Range 0.01..100: the wheel's clamp
    };

    struct EditorGizmoSettings
    {
        float size = 1.0f;           // Range 0.1..10: a zero or negative scale is refused
    };

    // The preference lines of an old [EditorViewport][Camera] block, captured
    // by ViewportSettings::ReadIniLine (validated exactly as before) and not
    // applied to any state.
    struct LegacyViewportPrefs
    {
        std::optional<float>     fovYDeg, speedScalar, gizmoSize;
        std::optional<bool>      showGrid;
        std::optional<GridPlane> gridPlane;

        [[nodiscard]] bool Any() const noexcept
        {
            return fovYDeg || speedScalar || gizmoSize || showGrid || gridPlane;
        }
    };

    // Sets each present value at SetBy::User, but only on a cvar whose history
    // holds no User record (the user already chose: keep it). Returns how many
    // were applied. The caller publishes and queues the archive write.
    std::size_t ImportLegacyViewportPrefs(CVarRegistry& reg, const LegacyViewportPrefs& p);

    // Calls `fn(name)` for each cvar `p` carries a value for (the names the
    // import may have written -- what the archive's dirty set needs).
    template <typename Fn>
    void ForEachLegacyViewportPref(const LegacyViewportPrefs& p, Fn&& fn)
    {
        if (p.fovYDeg)     fn(std::string_view("editor.camera.fovYDeg"));
        if (p.speedScalar) fn(std::string_view("editor.camera.speedScalar"));
        if (p.showGrid)    fn(std::string_view("editor.viewport.showGrid"));
        if (p.gridPlane)   fn(std::string_view("editor.viewport.gridPlane"));
        if (p.gizmoSize)   fn(std::string_view("editor.gizmo.size"));
    }

    // A Float32 cvar's registered range (the ranges live on the cvars only):
    // the legacy ini lines' validation, EditorCamera::AdjustSpeed's clamp and
    // the popup slider's ends. An unregistered name or an open end answers
    // (FLT_MIN, FLT_MAX): positive, finite.
    [[nodiscard]] std::pair<float, float> RegisteredFloatRange(std::string_view name);
    [[nodiscard]] inline std::pair<float, float> CameraSpeedScalarRange()
    {
        return RegisteredFloatRange("editor.camera.speedScalar");
    }

    // A viewport preference edit from outside the settings windows (the speed
    // wheel, the view-settings popup): Set at SetBy::User and queue the
    // debounced archive write. Visible at the next frame's publish.
    void SetViewportPref(std::string_view name, const CVarValue& value);
}
