// Settings arc S6-31: editor.gizmo.* / editor.gizmo.snap.* -- the snap steps
// nobody could change (GizmoSnap's old member defaults), the gizmo's pick /
// shape / shade literals (now GizmoTuning, filled by the editor) and the
// tool / mode / space a session starts in.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorViewportSettings.hpp"
#include "Settings/AxisColors.hpp"
#include <Arcane/Edit/Gizmo.hpp>

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Scene/RenderViewSettings.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <vector>

using namespace Arcane;

TEST_CASE("sweep: gizmo snap and tuning defaults are the pre-sweep literals", "[sweep][gizmo]")
{
    CHECK(Editor::EditorGizmoSnapSettings{}.translate == 0.5f);
    CHECK(Editor::EditorGizmoSnapSettings{}.rotateDegrees == 15.0f);
    CHECK(Editor::EditorGizmoSnapSettings{}.scale == 0.1f);
    const Editor::EditorGizmoSettings g{};
    CHECK(g.pickRadiusPx == 8.0f); CHECK(g.ringSegments == 48); CHECK(g.minScale == 0.01f);
    CHECK(g.brighten == 1.4f); CHECK(g.darken == 0.55f); CHECK(g.hotFillAlpha == 0.3f);
    const GizmoSnap s = Editor::MakeGizmoSnap(true);
    CHECK(s.enabled); CHECK(s.translate == 0.5f); CHECK(s.rotationDeg == 15.0f); CHECK(s.scale == 0.1f);
    Test::RequireDefault("editor.gizmo.snap.translate", CVarValue::Float32(0.5f));
}

TEST_CASE("sweep: every editor.gizmo.* default is the declared literal", "[sweep][gizmo]")
{
    const Editor::EditorGizmoSettings g{};
    CHECK(g.ringPickSlackPx == 4.0f); CHECK(g.minPlaneAreaPx2 == 4.0f); CHECK(g.planeEdgeOnCos == 0.2f);
    CHECK(g.minAxisLenPx == 2.0f);
    CHECK(g.defaultMode == GizmoMode::Translate); CHECK(g.defaultSpace == GizmoSpace::World); CHECK_FALSE(g.defaultTool);

    // The tuning the editor passes is the settings block field for field.
    const GizmoTuning t = Editor::ToGizmoTuning(g);
    CHECK(t.pickRadiusPx == 8.0f); CHECK(t.ringPickSlackPx == 4.0f); CHECK(t.minPlaneAreaPx2 == 4.0f);
    CHECK(t.planeEdgeOnCos == 0.2f); CHECK(t.minAxisLenPx == 2.0f); CHECK(t.ringSegments == 48);
    CHECK(t.minScale == 0.01f); CHECK(t.brighten == 1.4f); CHECK(t.darken == 0.55f); CHECK(t.hotFillAlpha == 0.3f);

    Test::RequireDefault("editor.gizmo.snap.rotateDegrees", CVarValue::Float32(15.0f));
    Test::RequireDefault("editor.gizmo.snap.scale", CVarValue::Float32(0.1f));
    Test::RequireDefault("editor.gizmo.pickRadiusPx", CVarValue::Float32(8.0f));
    Test::RequireDefault("editor.gizmo.ringPickSlackPx", CVarValue::Float32(4.0f));
    Test::RequireDefault("editor.gizmo.minPlaneAreaPx2", CVarValue::Float32(4.0f));
    Test::RequireDefault("editor.gizmo.planeEdgeOnCos", CVarValue::Float32(0.2f));
    Test::RequireDefault("editor.gizmo.minAxisLenPx", CVarValue::Float32(2.0f));
    Test::RequireDefault("editor.gizmo.ringSegments", CVarValue::Int32(48));
    Test::RequireDefault("editor.gizmo.minScale", CVarValue::Float32(0.01f));
    Test::RequireDefault("editor.gizmo.brighten", CVarValue::Float32(1.4f));
    Test::RequireDefault("editor.gizmo.darken", CVarValue::Float32(0.55f));
    Test::RequireDefault("editor.gizmo.hotFillAlpha", CVarValue::Float32(0.3f));
    Test::RequireDefault("editor.gizmo.defaultMode", CVarValue::Enum(0));    // Translate
    Test::RequireDefault("editor.gizmo.defaultSpace", CVarValue::Enum(0));   // World
    Test::RequireDefault("editor.gizmo.defaultTool", CVarValue::Bool(false));

    // The pick tolerances and the snap steps are user preferences; the shape
    // and shade knobs are the inventory's "Editor Dev" rows; the session
    // defaults apply to the next session.
    CVarRegistry& reg = CVarRegistry::Get();
    const auto flagsOf = [&](std::string_view n) { return reg.Describe(n)->flags; };
    CHECK_FALSE(HasFlag(flagsOf("editor.gizmo.snap.translate"), CVarFlags::Dev));
    CHECK_FALSE(HasFlag(flagsOf("editor.gizmo.pickRadiusPx"), CVarFlags::Dev));
    CHECK_FALSE(HasFlag(flagsOf("editor.gizmo.ringPickSlackPx"), CVarFlags::Dev));
    for (std::string_view dev : { "editor.gizmo.minPlaneAreaPx2", "editor.gizmo.planeEdgeOnCos", "editor.gizmo.minAxisLenPx",
                                  "editor.gizmo.ringSegments", "editor.gizmo.minScale", "editor.gizmo.brighten",
                                  "editor.gizmo.darken", "editor.gizmo.hotFillAlpha" })
    {
        if (!Test::InThisBuild(dev)) continue;   // Dist: compiled out
        INFO(std::string(dev));
        CHECK(HasFlag(flagsOf(dev), CVarFlags::Dev));
    }
    CHECK(reg.Describe("editor.gizmo.snap.translate")->scope == SettingScope::PreferencesProject);
    CHECK(reg.Describe("editor.gizmo.snap.translate")->apply == ApplyMode::Live);
    CHECK(reg.Describe("editor.gizmo.pickRadiusPx")->apply == ApplyMode::Live);
    CHECK(reg.Describe("editor.gizmo.defaultMode")->apply == ApplyMode::NextWorld);
    CHECK(reg.Describe("editor.gizmo.defaultSpace")->apply == ApplyMode::NextWorld);
    CHECK(reg.Describe("editor.gizmo.defaultTool")->apply == ApplyMode::NextWorld);
    CHECK(reg.Describe("editor.gizmo.defaultMode")->enumNames == std::vector<std::string>{ "Translate", "Rotate", "Scale" });
}

TEST_CASE("sweep: MakeGizmoSnap and MakeGizmoTuning read the published editor.gizmo.* values", "[sweep][gizmo]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("editor.gizmo.snap.translate"), CVarValue::Float32(2.0f), SetBy::Code);
    reg.Set(reg.Find("editor.gizmo.snap.rotateDegrees"), CVarValue::Float32(45.0f), SetBy::Code);
    reg.Set(reg.Find("editor.gizmo.pickRadiusPx"), CVarValue::Float32(16.0f), SetBy::Code);
    // editor.gizmo.ringSegments is a Dev row: compiled out of Dist, where the tuning keeps the default.
    const bool ringRow = Test::InThisBuild("editor.gizmo.ringSegments");
    if (ringRow) reg.Set(reg.Find("editor.gizmo.ringSegments"), CVarValue::Int32(16), SetBy::Code);
    reg.PublishImmediate();

    const GizmoSnap s = Editor::MakeGizmoSnap(false);
    CHECK_FALSE(s.enabled); CHECK(s.translate == 2.0f); CHECK(s.rotationDeg == 45.0f); CHECK(s.scale == 0.1f);
    const GizmoTuning t = Editor::MakeGizmoTuning();
    CHECK(t.pickRadiusPx == 16.0f); CHECK(t.ringSegments == (ringRow ? 16 : 48)); CHECK(t.minScale == 0.01f);

    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
    CHECK(Editor::MakeGizmoSnap(true).translate == 0.5f);
    CHECK(Editor::MakeGizmoTuning().pickRadiusPx == 8.0f);
}

// Fix round 1: editor.gizmo.default* are NextWorld Pref-P settings, so the
// editor re-applies them on a windowed project switch (ViewportSettingsClearAll,
// beside ApplyFreshPose) as well as at boot. Both sites call this one helper;
// the switch itself (an ImGui ClearIniSettings inside EditorApp) has no
// ArcaneTests seam, so the helper's contract is what is pinned here: it reads
// the CURRENT snapshot (an incoming project's rung), not a boot-time copy.
TEST_CASE("sweep: editor.gizmo.default* -> the session start, re-read from the snapshot, --tool on top", "[sweep][gizmo]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    using Editor::GizmoSessionState;
    const GizmoSessionState def = Editor::ToGizmoSessionState(Editor::EditorGizmoSettings{});
    CHECK(def.mode == GizmoMode::Translate); CHECK(def.space == GizmoSpace::World); CHECK_FALSE(def.enabled);

    Editor::EditorGizmoSettings g;
    g.defaultMode = GizmoMode::Rotate; g.defaultSpace = GizmoSpace::Local; g.defaultTool = true;
    const GizmoSessionState custom = Editor::ToGizmoSessionState(g);
    CHECK(custom.mode == GizmoMode::Rotate); CHECK(custom.space == GizmoSpace::Local); CHECK(custom.enabled);

    // A session the user drove away from the defaults (W/E/R, the toolbar) ...
    GizmoSessionState live{ GizmoMode::Rotate, GizmoSpace::World, false };
    // ... then a project switch whose incoming project holds other defaults.
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("editor.gizmo.defaultMode"), CVarValue::Enum(2), SetBy::Code);    // Scale
    reg.Set(reg.Find("editor.gizmo.defaultSpace"), CVarValue::Enum(1), SetBy::Code);   // Local
    reg.Set(reg.Find("editor.gizmo.defaultTool"), CVarValue::Bool(true), SetBy::Code);
    reg.PublishImmediate();
    Editor::ApplyGizmoSessionDefaults(live);
    CHECK(live.mode == GizmoMode::Scale); CHECK(live.space == GizmoSpace::Local); CHECK(live.enabled);

    // Switching back to a project at the defaults re-applies those too.
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
    Editor::ApplyGizmoSessionDefaults(live);
    CHECK(live.mode == GizmoMode::Translate); CHECK(live.space == GizmoSpace::World); CHECK_FALSE(live.enabled);

    // --tool beats the defaults; "" is no seed; the space is never the flag's.
    GizmoSessionState seeded{ GizmoMode::Scale, GizmoSpace::Local, true };
    Editor::ApplyGizmoToolSeed("", seeded);
    CHECK(seeded.mode == GizmoMode::Scale); CHECK(seeded.space == GizmoSpace::Local); CHECK(seeded.enabled);
    Editor::ApplyGizmoToolSeed("select", seeded);
    CHECK_FALSE(seeded.enabled); CHECK(seeded.mode == GizmoMode::Scale);
    Editor::ApplyGizmoToolSeed("rotate", seeded);
    CHECK(seeded.enabled); CHECK(seeded.mode == GizmoMode::Rotate); CHECK(seeded.space == GizmoSpace::Local);
    Editor::ApplyGizmoToolSeed("move", seeded);
    CHECK(seeded.enabled); CHECK(seeded.mode == GizmoMode::Translate);
    Editor::ApplyGizmoToolSeed("scale", seeded);
    CHECK(seeded.enabled); CHECK(seeded.mode == GizmoMode::Scale);
}

namespace
{
    // The 2D view: 800x600 at 100 px/m, the pivot at pixel (400,300); the X
    // arrow runs (400..494, 300).
    ViewTransform OrthoView() { return Ortho2DView({0.0f, 0.0f}, 3.0f, {800u, 600u}); }
    ViewTransform ObliqueView() { return ViewTransform::Perspective({4.0f, 3.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f, {800u, 600u}, 0.1f, 100.0f); }

    struct ColourSink final : GizmoDrawSink
    {
        int lines = 0;
        std::vector<glm::vec4> triangleColours;
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override { ++lines; }
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4 c) override { triangleColours.push_back(c); }
        void Rect(glm::vec2, glm::vec2, glm::vec4) override {}
        void Circle(glm::vec2, float, glm::vec4) override {}
    };
}

TEST_CASE("sweep: the gizmo reads its pick radius, ring tessellation, fill alpha and scale floor from the tuning", "[sweep][gizmo]")
{
    const GizmoTuning def = Editor::ToGizmoTuning(Editor::EditorGizmoSettings{});
    const GizmoTransform pivot;

    // 12 px off the X arrow: outside the default 8 px pick radius, inside 16.
    GizmoTuning wide = def; wide.pickRadiusPx = 16.0f;
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, pivot, OrthoView(), GizmoHandleMask::All(), 1.0f, {460.0f, 312.0f}, def) == GizmoAxis::None);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, pivot, OrthoView(), GizmoHandleMask::All(), 1.0f, {460.0f, 312.0f}, wide) == GizmoAxis::X);

    // The screen ring is ringSegments lines.
    GizmoTuning coarse = def; coarse.ringSegments = 16;
    ColourSink ring48, ring16;
    Draw(ring48, GizmoMode::Rotate, GizmoSpace::World, pivot, ObliqueView(), GizmoHandleMask::All(), 1.0f, GizmoAxis::None, GizmoAxis::None, def);
    Draw(ring16, GizmoMode::Rotate, GizmoSpace::World, pivot, ObliqueView(), GizmoHandleMask::All(), 1.0f, GizmoAxis::None, GizmoAxis::None, coarse);
    CHECK(ring48.lines == 48);
    CHECK(ring16.lines == 16);

    // A hot plane square is painted at hotFillAlpha.
    GizmoTuning opaque = def; opaque.hotFillAlpha = 0.75f;
    ColourSink hot3, hot75;
    Draw(hot3, GizmoMode::Translate, GizmoSpace::World, pivot, ObliqueView(), GizmoHandleMask::All(), 1.0f, GizmoAxis::XY, GizmoAxis::None, def);
    Draw(hot75, GizmoMode::Translate, GizmoSpace::World, pivot, ObliqueView(), GizmoHandleMask::All(), 1.0f, GizmoAxis::XY, GizmoAxis::None, opaque);
    REQUIRE(!hot3.triangleColours.empty());
    REQUIRE(!hot75.triangleColours.empty());
    CHECK(hot3.triangleColours.front().w == 0.3f);    // the plane fill is the first thing painted
    CHECK(hot75.triangleColours.front().w == 0.75f);

    // A scale drag onto the pivot clamps to minScale.
    GizmoTuning floor = def; floor.minScale = 0.5f;
    const GizmoTransform r0 = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, pivot, OrthoView(), {500, 300}, {400, 300}, GizmoSnap{}, def);
    const GizmoTransform r1 = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, pivot, OrthoView(), {500, 300}, {400, 300}, GizmoSnap{}, floor);
    CHECK(r0.scale.x == 0.01f);
    CHECK(r1.scale.x == 0.5f);
}

// S6-45: editor.gizmo.color.{hot,screen,screenArc,centre} -- the gizmo's
// non-axis colours ride GizmoAxisColors; the axis X/Y/Z stay held (S5-2 A).
TEST_CASE("sweep: the gizmo's hot, screen and centre colours are editor.gizmo.color.*", "[sweep][gizmo]")
{
    const GizmoAxisColors legacy{};
    CHECK(legacy.hot == glm::vec4(1.00f, 0.86f, 0.18f, 1.0f));
    CHECK(legacy.screen == glm::vec4(0.90f, 0.91f, 0.93f, 1.0f));
    CHECK(legacy.screenArc == glm::vec4(0.96f, 0.90f, 0.42f, 1.0f));
    CHECK(legacy.centre == glm::vec4(0.97f, 0.97f, 0.98f, 1.0f));
    Test::RequireDefault("editor.gizmo.color.hot", CVarValue::Color({ 1.00f, 0.86f, 0.18f, 1.0f }));
    Test::RequireDefault("editor.gizmo.color.screen", CVarValue::Color({ 0.90f, 0.91f, 0.93f, 1.0f }));
    Test::RequireDefault("editor.gizmo.color.screenArc", CVarValue::Color({ 0.96f, 0.90f, 0.42f, 1.0f }));
    Test::RequireDefault("editor.gizmo.color.centre", CVarValue::Color({ 0.97f, 0.97f, 0.98f, 1.0f }));

    // The colours the Draw is given are the ones painted: a hot plane square
    // fills in colors.hot at the tuning's alpha.
    const GizmoTuning def = Editor::ToGizmoTuning(Editor::EditorGizmoSettings{});
    GizmoAxisColors magenta{};
    magenta.hot = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
    ColourSink painted;
    Draw(painted, GizmoMode::Translate, GizmoSpace::World, GizmoTransform{}, ObliqueView(), GizmoHandleMask::All(), 1.0f,
         GizmoAxis::XY, GizmoAxis::None, def, nullptr, magenta);
    REQUIRE(!painted.triangleColours.empty());
    CHECK(painted.triangleColours.front() == glm::vec4(1.0f, 0.0f, 1.0f, 0.3f));

    // The editor fills them from the published settings.
    CHECK(Editor::DeriveAxisRoles(Editor::Theme::Live()).gizmo.hot == legacy.hot);
    const Test::ScopedCodeRung hot("editor.gizmo.color.hot", CVarValue::Color({ 0.25f, 0.5f, 0.75f, 1.0f }));
    const Editor::AxisRoleColors roles = Editor::DeriveAxisRoles(Editor::Theme::Live());
    CHECK(roles.gizmo.hot == glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));
    CHECK(roles.gizmo.x == legacy.x);   // S5-2 option A: the axis colours stay held
}
