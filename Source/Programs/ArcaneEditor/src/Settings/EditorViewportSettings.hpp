#pragma once

// editor.viewport.* / editor.camera.* / editor.gizmo.* (settings arc S6-29..31;
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
#include <Arcane/Edit/Gizmo.hpp>   // GizmoMode / GizmoSpace (reflected there), GizmoTuning, GizmoSnap
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

        // S6-32 (Dev). The View -> Physics Overlay toggle stays session state
        // (spec s6.3): this is the state a session starts in, and a change to
        // the cvar moves the toggle to it (EditorApp's per-frame sync).
        bool physicsOverlay = false;
        // Frames a viewport pick may stay in flight before DeferredPick gives
        // up (Pref-M): far above the swapchain depth, so a briefly collapsed
        // panel does not lose the click.
        std::uint32_t pickMaxFramesInFlight = 64;
        // The extent the viewport context is first built at when there is no
        // panel measurement yet -- one frame's picture (Pref-M, Restart).
        std::uint32_t fallbackExtentW = 1280;
        std::uint32_t fallbackExtentH = 720;
    };

    // S6-30: the rest of editor.camera.* -- what EditorCamera's static
    // constexpr block and literals were. EditorCamera's methods read the
    // published snapshot once per call; Ortho2D / Orbit3D member defaults read
    // EditorCameraSettings{} (the fresh-camera pose cvars reach a camera
    // through ApplyFreshPose, NextWorld).
    struct EditorCameraSettings
    {
        float fovYDeg     = 60.0f;   // Range 1..179: the projection stays finite
        float speedScalar = 1.0f;    // Range 0.01..100: the wheel's clamp

        // The fresh-camera pose (Dev, NextWorld): 5 m shows a 10 m slice in
        // 2D; the 3/4 perspective view.
        float default2DHalfHeight = 5.0f;
        float default3DYaw        = -30.0f;
        float default3DPitch      = 30.0f;
        float default3DDistance   = 10.0f;

        // 2D zoom clamp in world half-height (UE's MIN_/MAX_ORTHOZOOM). Both
        // ends are positive, so the pixels-per-metre the 2D ops divide by is
        // never zero.
        float orthoMinHalfHeight = 0.01f;
        float orthoMaxHalfHeight = 1.0e6f;
        float wheelZoomStep      = 1.12f;   // multiplicative zoom / dolly per wheel tick
        float frameFill          = 0.9f;    // fraction of the viewport a framed AABB spans
        float minOrbitDistance   = 0.05f;   // the eye can never reach the pivot
        float maxOrbitDistance   = 1.0e5f;
        float nearClip           = 0.05f;
        float farClip            = 5000.0f;
        float baseFlySpeed       = 5.0f;    // m/s at speedScalar 1 and distance scale 1

        // UE's bUseDistanceScaledCameraSpeed shape: fly / pan speed x
        // clamp(distance / refDistance, speedFloor, speedCap). ON here, with
        // a floor (ours, not UE's) so a camera parked on its pivot still moves.
        bool  distanceScaledSpeed = true;
        float refDistance         = 10.0f;
        float speedFloor          = 0.1f;
        float speedCap            = 1000.0f;

        float lookSensitivity  = 0.2f;      // deg per px (UE's MouseSensitivity)
        float orbitSensitivity = 0.2f;
        float boostMultiplier  = 2.0f;      // Shift while flying
        float speedWheelStep   = 1.1f;      // speedScalar x / per wheel tick while flying
    };

    // S6-31: the rest of editor.gizmo.* -- the pick tolerances, shape and
    // shade literals Gizmo.cpp held (passed to Arcane::Gizmo as a GizmoTuning,
    // MakeGizmoTuning), and the tool / mode / space a session starts in
    // (NextWorld: applied at editor boot and again on every windowed project
    // switch -- ApplyGizmoSessionDefaults -- exactly like the camera's
    // editor.camera.default* through ApplyFreshPose).
    struct EditorGizmoSettings
    {
        float size = 1.0f;           // Range 0.1..10: a zero or negative scale is refused

        float        pickRadiusPx    = 8.0f;    // axis segment / screen ring pick radius
        float        ringPickSlackPx = 4.0f;    // the band half-width plus this is the ring pick radius
        float        minPlaneAreaPx2 = 4.0f;    // an edge-on plane corner is not a target
        float        planeEdgeOnCos  = 0.2f;    // a corner within ~78 deg of edge-on is hidden
        float        minAxisLenPx    = 2.0f;    // an arrow pointing at the camera: a dot, no head
        std::int32_t ringSegments    = 48;      // full ring
        float        minScale        = 0.01f;   // a scale drag never reaches zero
        float        brighten        = 1.4f;    // the lit side of a rod / cone / cube
        float        darken          = 0.55f;   // the shadow side
        float        hotFillAlpha    = 0.3f;    // the hot plane square and the rotate sweep

        Arcane::GizmoMode  defaultMode  = Arcane::GizmoMode::Translate;
        Arcane::GizmoSpace defaultSpace = Arcane::GizmoSpace::World;
        bool               defaultTool  = false;   // false = the Select tool (click-to-pick, no gizmo)
    };

    // editor.gizmo.snap.* (S6-31): the steps a Ctrl-held gizmo drag rounds
    // to. Before the sweep they were GizmoSnap's member defaults and the
    // editor only ever set `enabled`, so nobody could change them.
    struct EditorGizmoSnapSettings
    {
        float translate     = 0.5f;    // metres
        float rotateDegrees = 15.0f;
        float scale         = 0.1f;    // ratio
    };

    // editor.gizmo.color.* (inventory "Gizmo.cpp:61-64", reconciled R1; S6-45):
    // the hot / screen / screen-arc / centre colours the engine gizmo paints,
    // carried to it on GizmoAxisColors by DeriveAxisRoles. The X/Y/Z axis
    // colours are NOT here: S5-2 option A holds them pending the unification
    // re-bless. Defaults = GizmoAxisColors{}'s legacy values.
    struct EditorGizmoColorSettings
    {
        CVarColor hot{ 1.00f, 0.86f, 0.18f, 1.0f };
        CVarColor screen{ 0.90f, 0.91f, 0.93f, 1.0f };
        CVarColor screenArc{ 0.96f, 0.90f, 0.42f, 1.0f };
        CVarColor centre{ 0.97f, 0.97f, 0.98f, 1.0f };
    };

    // The engine gizmo's inputs from the settings: pure (ToGizmoTuning) and
    // from the published snapshot (MakeGizmoTuning / MakeGizmoSnap, read once
    // per call -- the frame's HitTest, drag and Draw each call once).
    [[nodiscard]] Arcane::GizmoTuning ToGizmoTuning(const EditorGizmoSettings& s) noexcept;
    [[nodiscard]] Arcane::GizmoTuning MakeGizmoTuning();
    [[nodiscard]] Arcane::GizmoSnap   MakeGizmoSnap(bool enabled);

    // The editor's gizmo session state: the transform mode, its space, and
    // whether the transform tool (rather than Select) is active.
    struct GizmoSessionState
    {
        Arcane::GizmoMode  mode    = Arcane::GizmoMode::Translate;
        Arcane::GizmoSpace space   = Arcane::GizmoSpace::World;
        bool               enabled = false;   // false = the Select tool
    };

    // editor.gizmo.default* (NextWorld) -> the state a session starts in:
    // pure (ToGizmoSessionState) and from the published snapshot
    // (ApplyGizmoSessionDefaults). The editor calls the latter at boot
    // (StageEditorShell, beside ApplyFreshPose) and on a windowed project
    // switch (ViewportSettingsClearAll, beside ApplyFreshPose), so the
    // incoming project's Pref-P values take effect without a restart.
    [[nodiscard]] GizmoSessionState ToGizmoSessionState(const EditorGizmoSettings& s) noexcept;
    void ApplyGizmoSessionDefaults(GizmoSessionState& state);

    // --tool: "select" turns the transform tool off; "rotate" / "scale" pick
    // that mode; any other non-empty spelling HostConfig accepted means
    // translate; "" (absent) = no seed. Applied after the defaults (boot:
    // StageFinalize; switch: ViewportSettingsClearAll), so the flag beats
    // editor.gizmo.default* exactly as --view-mode beats the persisted camera.
    void ApplyGizmoToolSeed(std::string_view flag, GizmoSessionState& state) noexcept;

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
