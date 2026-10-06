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
            ARC_REFLECT_ATTR(Tooltip, "Fly and pan speed multiplier. The mouse wheel while flying steps it by x1.1.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(EditorGizmoSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.gizmo", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorGizmoSettings, size)
            ARC_REFLECT_ATTR(DisplayName, "Gizmo size") ARC_REFLECT_ATTR(Range, 0.1, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Screen-size multiplier of the transform gizmo.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorViewportSettings);
    ARC_SETTINGS(EditorCameraSettings);
    ARC_SETTINGS(EditorGizmoSettings);

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
