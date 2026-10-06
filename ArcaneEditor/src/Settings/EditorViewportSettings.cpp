#include "Settings/EditorViewportSettings.hpp"

#include "Settings/SettingsEdit.hpp"   // RungSource
#include "Settings/SettingsHost.hpp"   // NoteSettingEdited

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>

#include <limits>
#include <string>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorViewportSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.viewport", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorViewportSettings, showGrid)
            ARC_REFLECT_ATTR(DisplayName, "Show grid")
            ARC_REFLECT_ATTR(Tooltip, "Draw the reference grid in the viewport (Edit mode only).")
        ARC_REFLECT_FIELD(EditorViewportSettings, gridPlane)
            ARC_REFLECT_ATTR(DisplayName, "Grid plane")
            ARC_REFLECT_ATTR(Tooltip, "The world plane the perspective view's grid lies on: XZ is the ground, XY the 2D authoring plane.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(EditorCameraSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.camera", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorCameraSettings, fovYDeg)
            ARC_REFLECT_ATTR(DisplayName, "Field of view") ARC_REFLECT_ATTR(Range, 1.0, 179.0)
            ARC_REFLECT_ATTR(Tooltip, "Vertical field of view (degrees) of the perspective viewport camera.")
        ARC_REFLECT_FIELD(EditorCameraSettings, speedScalar)
            ARC_REFLECT_ATTR(DisplayName, "Camera speed") ARC_REFLECT_ATTR(Range, 0.01, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Fly and pan speed multiplier. The mouse wheel while flying steps it by the speed wheel step.")
        ARC_REFLECT_FIELD(EditorCameraSettings, default2DHalfHeight)
            ARC_REFLECT_ATTR(DisplayName, "Default 2D half-height") ARC_REFLECT_ATTR(Range, 0.01, 1.0e6)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Half the 2D view's height (metres) on a fresh camera.")
        ARC_REFLECT_FIELD(EditorCameraSettings, default3DYaw)
            ARC_REFLECT_ATTR(DisplayName, "Default 3D yaw") ARC_REFLECT_ATTR(Range, -360.0, 360.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Yaw (degrees) of a fresh perspective camera about its pivot.")
        ARC_REFLECT_FIELD(EditorCameraSettings, default3DPitch)
            ARC_REFLECT_ATTR(DisplayName, "Default 3D pitch") ARC_REFLECT_ATTR(Range, -89.0, 89.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Pitch (degrees above the ground plane) of a fresh perspective camera.")
        ARC_REFLECT_FIELD(EditorCameraSettings, default3DDistance)
            ARC_REFLECT_ATTR(DisplayName, "Default 3D distance") ARC_REFLECT_ATTR(Range, 0.05, 1.0e5)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Eye-to-pivot distance (metres) of a fresh perspective camera.")
        ARC_REFLECT_FIELD(EditorCameraSettings, orthoMinHalfHeight)
            ARC_REFLECT_ATTR(DisplayName, "2D zoom-in limit") ARC_REFLECT_ATTR(Range, 0.0001, 1000.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Smallest 2D half-height (metres) the wheel and framing can reach.")
        ARC_REFLECT_FIELD(EditorCameraSettings, orthoMaxHalfHeight)
            ARC_REFLECT_ATTR(DisplayName, "2D zoom-out limit") ARC_REFLECT_ATTR(Range, 1.0, 1.0e9)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Largest 2D half-height (metres) the wheel and framing can reach.")
        ARC_REFLECT_FIELD(EditorCameraSettings, wheelZoomStep)
            ARC_REFLECT_ATTR(DisplayName, "Wheel zoom step") ARC_REFLECT_ATTR(Range, 1.01, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Zoom (2D) and dolly (3D) factor per mouse-wheel tick.")
        ARC_REFLECT_FIELD(EditorCameraSettings, frameFill)
            ARC_REFLECT_ATTR(DisplayName, "Frame fill") ARC_REFLECT_ATTR(Range, 0.5, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Fraction of the viewport a framed selection spans (F / Home); the rest is padding.")
        ARC_REFLECT_FIELD(EditorCameraSettings, minOrbitDistance)
            ARC_REFLECT_ATTR(DisplayName, "Min orbit distance") ARC_REFLECT_ATTR(Range, 0.001, 100.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Closest the perspective eye can come to its pivot (metres).")
        ARC_REFLECT_FIELD(EditorCameraSettings, maxOrbitDistance)
            ARC_REFLECT_ATTR(DisplayName, "Max orbit distance") ARC_REFLECT_ATTR(Range, 10.0, 1.0e7)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Farthest the perspective eye can be from its pivot (metres).")
        ARC_REFLECT_FIELD(EditorCameraSettings, nearClip)
            ARC_REFLECT_ATTR(DisplayName, "Near clip") ARC_REFLECT_ATTR(Range, 0.001, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Near clipping plane (metres) of the perspective viewport camera.")
        ARC_REFLECT_FIELD(EditorCameraSettings, farClip)
            ARC_REFLECT_ATTR(DisplayName, "Far clip") ARC_REFLECT_ATTR(Range, 20.0, 1.0e6)   // > nearClip's max: never a degenerate frustum
            ARC_REFLECT_ATTR(Tooltip, "Far clipping plane (metres) of the perspective viewport camera.")
        ARC_REFLECT_FIELD(EditorCameraSettings, baseFlySpeed)
            ARC_REFLECT_ATTR(DisplayName, "Base fly speed") ARC_REFLECT_ATTR(Range, 0.1, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "WASD/QE fly speed (m/s) at camera speed 1 and the reference distance.")
        ARC_REFLECT_FIELD(EditorCameraSettings, distanceScaledSpeed)
            ARC_REFLECT_ATTR(DisplayName, "Distance-scaled speed")
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Scale fly and pan speed with the distance to the pivot, so framing then flying feels the same at every zoom.")
        ARC_REFLECT_FIELD(EditorCameraSettings, refDistance)
            ARC_REFLECT_ATTR(DisplayName, "Reference distance") ARC_REFLECT_ATTR(Range, 0.01, 1.0e5)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Pivot distance (metres) at which the distance scale is 1.")
        ARC_REFLECT_FIELD(EditorCameraSettings, speedFloor)
            ARC_REFLECT_ATTR(DisplayName, "Speed scale floor") ARC_REFLECT_ATTR(Range, 0.001, 1.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Smallest distance scale, so a camera parked on its pivot can still move.")
        ARC_REFLECT_FIELD(EditorCameraSettings, speedCap)
            ARC_REFLECT_ATTR(DisplayName, "Speed scale cap") ARC_REFLECT_ATTR(Range, 1.0, 1.0e6)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Largest distance scale.")
        ARC_REFLECT_FIELD(EditorCameraSettings, lookSensitivity)
            ARC_REFLECT_ATTR(DisplayName, "Look sensitivity") ARC_REFLECT_ATTR(Range, 0.01, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Degrees per pixel of a right-drag mouselook.")
        ARC_REFLECT_FIELD(EditorCameraSettings, orbitSensitivity)
            ARC_REFLECT_ATTR(DisplayName, "Orbit sensitivity") ARC_REFLECT_ATTR(Range, 0.01, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Degrees per pixel of an Alt+left-drag orbit.")
        ARC_REFLECT_FIELD(EditorCameraSettings, boostMultiplier)
            ARC_REFLECT_ATTR(DisplayName, "Boost multiplier") ARC_REFLECT_ATTR(Range, 1.0, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Fly speed multiplier while Shift is held.")
        ARC_REFLECT_FIELD(EditorCameraSettings, speedWheelStep)
            ARC_REFLECT_ATTR(DisplayName, "Speed wheel step") ARC_REFLECT_ATTR(Range, 1.01, 2.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Camera speed factor per mouse-wheel tick while flying.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(EditorGizmoSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.gizmo", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorGizmoSettings, size)
            ARC_REFLECT_ATTR(DisplayName, "Gizmo size") ARC_REFLECT_ATTR(Range, 0.1, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Screen-size multiplier of the transform gizmo.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, pickRadiusPx)
            ARC_REFLECT_ATTR(DisplayName, "Pick radius") ARC_REFLECT_ATTR(Range, 1.0, 32.0)
            ARC_REFLECT_ATTR(Tooltip, "How close (pixels) the cursor must be to an arrow, box or the screen ring to grab it.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, ringPickSlackPx)
            ARC_REFLECT_ATTR(DisplayName, "Ring pick slack") ARC_REFLECT_ATTR(Range, 0.0, 32.0)
            ARC_REFLECT_ATTR(Tooltip, "Pixels added either side of a rotate band when grabbing it.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, minPlaneAreaPx2)
            ARC_REFLECT_ATTR(DisplayName, "Min plane handle area") ARC_REFLECT_ATTR(Range, 0.0, 100.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "A plane handle whose projected square is smaller than this (square pixels) cannot be grabbed.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, planeEdgeOnCos)
            ARC_REFLECT_ATTR(DisplayName, "Plane edge-on cutoff") ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "A plane handle is hidden when the cosine between its normal and the view direction falls below this.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, minAxisLenPx)
            ARC_REFLECT_ATTR(DisplayName, "Min arrow length") ARC_REFLECT_ATTR(Range, 0.0, 32.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "An arrow shorter than this on screen (pixels, pointing at the camera) draws without its cone head.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, ringSegments)
            ARC_REFLECT_ATTR(DisplayName, "Ring segments") ARC_REFLECT_ATTR(Range, 8, 256)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Segments in a full rotate ring and the screen ring; the rotate sweep follows it.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, minScale)
            ARC_REFLECT_ATTR(DisplayName, "Min scale") ARC_REFLECT_ATTR(Range, 1.0e-6, 1.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Smallest scale magnitude a scale drag can reach; the sign of a mirrored axis is kept.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, brighten)
            ARC_REFLECT_ATTR(DisplayName, "Highlight factor") ARC_REFLECT_ATTR(Range, 0.0, 4.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Colour multiplier of the lit side of the gizmo's rods, cones and cubes.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, darken)
            ARC_REFLECT_ATTR(DisplayName, "Shadow factor") ARC_REFLECT_ATTR(Range, 0.0, 4.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Colour multiplier of the shadow side of the gizmo's rods and cones.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, hotFillAlpha)
            ARC_REFLECT_ATTR(DisplayName, "Hot fill opacity") ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Opacity of a hovered plane handle's square and of the rotate sweep.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, defaultMode)
            ARC_REFLECT_ATTR(DisplayName, "Default gizmo mode") ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "The transform mode (translate, rotate, scale) the editor starts in.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, defaultSpace)
            ARC_REFLECT_ATTR(DisplayName, "Default gizmo space") ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Whether the gizmo starts aligned to the world axes or to the selection's local axes.")
        ARC_REFLECT_FIELD(EditorGizmoSettings, defaultTool)
            ARC_REFLECT_ATTR(DisplayName, "Start with the transform tool") ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Start with the transform gizmo active instead of the Select tool.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(EditorGizmoSnapSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.gizmo.snap", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorGizmoSnapSettings, translate)
            ARC_REFLECT_ATTR(DisplayName, "Translate snap") ARC_REFLECT_ATTR(Range, 0.001, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Step (metres) a Ctrl-held move snaps to.")
        ARC_REFLECT_FIELD(EditorGizmoSnapSettings, rotateDegrees)
            ARC_REFLECT_ATTR(DisplayName, "Rotate snap") ARC_REFLECT_ATTR(Range, 0.1, 90.0)
            ARC_REFLECT_ATTR(Tooltip, "Step (degrees) a Ctrl-held rotation snaps to.")
        ARC_REFLECT_FIELD(EditorGizmoSnapSettings, scale)
            ARC_REFLECT_ATTR(DisplayName, "Scale snap") ARC_REFLECT_ATTR(Range, 0.001, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Step a Ctrl-held scale snaps to.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorViewportSettings);
    ARC_SETTINGS(EditorCameraSettings);
    ARC_SETTINGS(EditorGizmoSettings);
    ARC_SETTINGS(EditorGizmoSnapSettings);

    GizmoTuning ToGizmoTuning(const EditorGizmoSettings& s) noexcept
    {
        GizmoTuning t;
        t.pickRadiusPx    = s.pickRadiusPx;
        t.ringPickSlackPx = s.ringPickSlackPx;
        t.minPlaneAreaPx2 = s.minPlaneAreaPx2;
        t.planeEdgeOnCos  = s.planeEdgeOnCos;
        t.minAxisLenPx    = s.minAxisLenPx;
        t.ringSegments    = s.ringSegments;
        t.minScale        = s.minScale;
        t.brighten        = s.brighten;
        t.darken          = s.darken;
        t.hotFillAlpha    = s.hotFillAlpha;
        return t;
    }

    GizmoTuning MakeGizmoTuning()
    {
        return ToGizmoTuning(Arcane::Settings<EditorGizmoSettings>());
    }

    GizmoSnap MakeGizmoSnap(bool enabled)
    {
        const EditorGizmoSnapSettings& s = Arcane::Settings<EditorGizmoSnapSettings>();
        GizmoSnap snap;
        snap.enabled     = enabled;
        snap.translate   = s.translate;
        snap.rotationDeg = s.rotateDegrees;
        snap.scale       = s.scale;
        return snap;
    }

    std::size_t ImportLegacyViewportPrefs(CVarRegistry& reg, const LegacyViewportPrefs& p)
    {
        std::size_t n = 0;
        const auto importOne = [&](std::string_view name, const CVarValue& v)
        {
            const auto e = reg.Explain(name);
            if (!e) return;
            for (const CVarHistoryRecord& r : e->history)
                if (r.by == SetBy::User) return;                  // the user already chose: keep it
            if (reg.Set(reg.Find(name), v, SetBy::User, "viewport-ini-import") == SetResult::Applied) ++n;
        };
        if (p.fovYDeg)     importOne("editor.camera.fovYDeg", CVarValue::Float32(*p.fovYDeg));
        if (p.speedScalar) importOne("editor.camera.speedScalar", CVarValue::Float32(*p.speedScalar));
        if (p.showGrid)    importOne("editor.viewport.showGrid", CVarValue::Bool(*p.showGrid));
        if (p.gridPlane)   importOne("editor.viewport.gridPlane", CVarValue::Enum(static_cast<std::int32_t>(*p.gridPlane)));
        if (p.gizmoSize)   importOne("editor.gizmo.size", CVarValue::Float32(*p.gizmoSize));
        return n;
    }

    std::pair<float, float> RegisteredFloatRange(std::string_view name)
    {
        // Unregistered (never in a host that links this TU): positive and
        // finite only, so the wheel can never zero the speed.
        std::pair<float, float> range{ std::numeric_limits<float>::min(), std::numeric_limits<float>::max() };
        if (const auto d = CVarRegistry::Get().Describe(name))
        {
            if (d->min && d->min->type == CVarType::Float32) range.first  = d->min->AsFloat32();
            if (d->max && d->max->type == CVarType::Float32) range.second = d->max->AsFloat32();
        }
        return range;
    }

    void SetViewportPref(std::string_view name, const CVarValue& value)
    {
        // Tagged as the User file's own loader, so the edit replaces the record
        // that file produced. RefusedWeaker still holds the User record (a
        // stronger rung, e.g. --set, wins for now): it is archived all the same.
        CVarRegistry& reg = CVarRegistry::Get();
        const SetResult r = reg.Set(reg.Find(name), value, SetBy::User, RungSource(SetBy::User));
        if (r == SetResult::Applied || r == SetResult::RefusedWeaker)
            NoteSettingEdited(SetBy::User, std::string(name));
    }
}
