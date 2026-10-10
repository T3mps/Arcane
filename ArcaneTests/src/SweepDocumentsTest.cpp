// Settings arc S6-35 (+ S6-45's frozen-name moves): editor.shader.* /
// editor.shader.chainLayout.* / editor.mesh.* / editor.mesh.primitiveRanges.* /
// editor.sprite.* / editor.preview.* / editor.preview.light.* -- the shader editor, mesh and
// sprite documents' caps and drag speeds, the material sphere and checker, and
// the ONE preview light (R1) the sphere, the thumbnails and the mesh document
// share.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/DocumentSettings.hpp"
#include "Documents/CustomBodyPreview.hpp"
#include "Documents/MaterialSpherePreview.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <optional>

using namespace Arcane;

TEST_CASE("sweep: one preview light; document defaults are the pre-sweep literals", "[sweep][documents]")
{
    const Editor::EditorPreviewLightSettings l{};
    CHECK(l.direction.x == 0.45f); CHECK(l.direction.y == 0.7f); CHECK(l.direction.z == 0.8f);
    CHECK(l.ambient == 0.12f);
    const MeshSceneDesc sphere = Editor::MaterialPreviewSphereScene({});
    CHECK(sphere.lightDirection == glm::vec3(0.45f, 0.7f, 0.8f));     // thumbnails/material sphere: unchanged
    CHECK(sphere.ambient == glm::vec3(0.12f));
    CHECK(Editor::ShaderEditorSettings{}.navHistoryMax == 32);
    CHECK(Editor::ShaderEditorSettings{}.previewResolution == 512);
    CHECK(Editor::MeshDocSettings{}.previewFov == 45.0f);
    CHECK(Editor::SpriteDocSettings{}.pivotDragSpeed == 0.005f);
    Test::RequireDefault("editor.preview.light.ambient", CVarValue::Float32(0.12f));
}

TEST_CASE("sweep: the material sphere scene is the pre-sweep picture", "[sweep][documents]")
{
    const MeshSceneDesc sphere = Editor::MaterialPreviewSphereScene({});
    CHECK(sphere.lightColor == glm::vec3(1.0f));
    CHECK(sphere.projection == PerspectiveProjection(35.0f, 1.0f, 0.05f, 10.0f));
    CHECK(sphere.view == glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));

    const MeshData built = Editor::BuildMaterialPreviewSphere();
    const MeshData legacy = BuildUvSphere(0.5f, 24, 32);
    CHECK(built.vertices.size() == legacy.vertices.size());
    CHECK(built.indices == legacy.indices);
}

TEST_CASE("sweep: every editor document / preview default is the declared literal", "[sweep][documents]")
{
    const Editor::EditorPreviewSettings p{};
    CHECK(p.sphereRings == 24); CHECK(p.sphereSegments == 32); CHECK(p.sphereFov == 35.0f);
    const Editor::ShaderEditorSettings s{};
    CHECK(s.previewCheckerLight.r == 0.16f); CHECK(s.previewCheckerLight.g == 0.16f);
    CHECK(s.previewCheckerLight.b == 0.19f); CHECK(s.previewCheckerLight.a == 1.0f);
    CHECK(s.previewCheckerSpriteScale == 0.8f);
    CHECK(s.passThumbPx == 72.0f);
    CHECK(s.renameListMax == 8); CHECK(s.bodyPreviewLines == 8); CHECK(s.bodyPreviewChars == 48);
    const Editor::MeshDocSettings m{};
    CHECK(m.previewResolution == 512); CHECK(m.previewMargin == 1.5f);
    CHECK(m.capsuleRatioDragSpeed == 0.02f);
    const Editor::EditorMeshPrimitiveRangesSettings r{};   // the pre-sweep row bounds (MeshDocument.cpp)
    CHECK(r.planeSubdivisionsMin == 1); CHECK(r.planeSubdivisionsMax == 64);
    CHECK(r.sphereRingsMin == 3);       CHECK(r.sphereRingsMax == 128);
    CHECK(r.segmentsMin == 3);          CHECK(r.segmentsMax == 128);
    CHECK(r.capsuleRingsMin == 2);      CHECK(r.capsuleRingsMax == 64);
    CHECK(r.capsuleLengthRatioMin == 1.0f); CHECK(r.capsuleLengthRatioMax == 20.0f);
    const Editor::ShaderChainLayoutSettings c{};
    CHECK(c.originX == 40.0f); CHECK(c.originY == 40.0f); CHECK(c.pitchX == 190.0f);
    CHECK(40.0f + c.sceneOffsetX == 40.0f - 170.0f); CHECK(c.sceneOffsetY == 90.0f);
    const Editor::SpriteDocSettings sp{};
    CHECK(sp.ppuDragSpeed == 0.5f); CHECK(sp.ppuMin == 1.0f); CHECK(sp.ppuMax == 4096.0f);

    Test::RequireDefault("editor.preview.light.direction", CVarValue::Vec3({ 0.45f, 0.7f, 0.8f }));
    Test::RequireDefault("editor.preview.light.colour", CVarValue::Color({ 1.0f, 1.0f, 1.0f, 1.0f }));
    Test::RequireDefault("editor.preview.sphereRings", CVarValue::Int32(24));
    Test::RequireDefault("editor.preview.sphereSegments", CVarValue::Int32(32));
    Test::RequireDefault("editor.preview.sphereFov", CVarValue::Float32(35.0f));
    Test::RequireDefault("editor.shader.navHistoryMax", CVarValue::Int32(32));
    Test::RequireDefault("editor.shader.previewResolution", CVarValue::Int32(512));
    Test::RequireDefault("editor.shader.renameListMax", CVarValue::Int32(8));
    Test::RequireDefault("editor.shader.bodyPreviewLines", CVarValue::Int32(8));
    Test::RequireDefault("editor.shader.bodyPreviewChars", CVarValue::Int32(48));
    Test::RequireDefault("editor.shader.previewCheckerLight", CVarValue::Color({ 0.16f, 0.16f, 0.19f, 1.0f }));
    Test::RequireDefault("editor.shader.previewCheckerSpriteScale", CVarValue::Float32(0.8f));
    Test::RequireDefault("editor.shader.passThumbPx", CVarValue::Float32(72.0f));
    Test::RequireDefault("editor.shader.chainLayout.originX", CVarValue::Float32(40.0f));
    Test::RequireDefault("editor.shader.chainLayout.originY", CVarValue::Float32(40.0f));
    Test::RequireDefault("editor.shader.chainLayout.pitchX", CVarValue::Float32(190.0f));
    Test::RequireDefault("editor.shader.chainLayout.sceneOffsetX", CVarValue::Float32(-170.0f));
    Test::RequireDefault("editor.shader.chainLayout.sceneOffsetY", CVarValue::Float32(90.0f));
    Test::RequireDefault("editor.mesh.previewResolution", CVarValue::Int32(512));
    Test::RequireDefault("editor.mesh.previewFov", CVarValue::Float32(45.0f));
    Test::RequireDefault("editor.mesh.previewMargin", CVarValue::Float32(1.5f));
    Test::RequireDefault("editor.mesh.primitiveRanges.planeSubdivisionsMin", CVarValue::Int32(1));
    Test::RequireDefault("editor.mesh.primitiveRanges.planeSubdivisionsMax", CVarValue::Int32(64));
    Test::RequireDefault("editor.mesh.primitiveRanges.sphereRingsMin", CVarValue::Int32(3));
    Test::RequireDefault("editor.mesh.primitiveRanges.sphereRingsMax", CVarValue::Int32(128));
    Test::RequireDefault("editor.mesh.primitiveRanges.segmentsMin", CVarValue::Int32(3));
    Test::RequireDefault("editor.mesh.primitiveRanges.segmentsMax", CVarValue::Int32(128));
    Test::RequireDefault("editor.mesh.primitiveRanges.capsuleRingsMin", CVarValue::Int32(2));
    Test::RequireDefault("editor.mesh.primitiveRanges.capsuleRingsMax", CVarValue::Int32(64));
    Test::RequireDefault("editor.mesh.primitiveRanges.capsuleLengthRatioMin", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.mesh.primitiveRanges.capsuleLengthRatioMax", CVarValue::Float32(20.0f));
    Test::RequireDefault("editor.mesh.capsuleRatioDragSpeed", CVarValue::Float32(0.02f));
    Test::RequireDefault("editor.sprite.ppuDragSpeed", CVarValue::Float32(0.5f));
    Test::RequireDefault("editor.sprite.ppuMin", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.sprite.ppuMax", CVarValue::Float32(4096.0f));
    Test::RequireDefault("editor.sprite.pivotDragSpeed", CVarValue::Float32(0.005f));
}

TEST_CASE("sweep: the Custom body preview honours a non-default line cap and marks the cut", "[sweep][documents]")
{
    using Arcane::Editor::BuildCustomBodyPreview;
    const std::string tenLines = "l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9";

    // editor.shader.bodyPreviewLines = 4 on a 10-line body: four lines + the "..." marker.
    const auto four = BuildCustomBodyPreview(tenLines, 4, 48);
    REQUIRE(four.lines.size() == 4);
    CHECK(four.lines.front() == "l0");
    CHECK(four.lines.back() == "l3");
    CHECK(four.truncated);

    // The default cap (8) still cuts and marks.
    const auto eight = BuildCustomBodyPreview(tenLines, 8, 48);
    CHECK(eight.lines.size() == 8);
    CHECK(eight.truncated);

    // A body that fits is not marked -- including one of exactly the cap with
    // no trailing newline, and one ending in a newline.
    CHECK_FALSE(BuildCustomBodyPreview("a\nb\nc\nd", 4, 48).truncated);
    CHECK_FALSE(BuildCustomBodyPreview("a\nb\nc\nd\n", 4, 48).truncated);
    CHECK(BuildCustomBodyPreview("a\nb\nc\nd\ne", 4, 48).truncated);

    // Lines clip to bodyPreviewChars with "..."; CR is stripped.
    const auto clipped = BuildCustomBodyPreview("abcdefgh\r\nxy", 8, 4);
    REQUIRE(clipped.lines.size() == 2);
    CHECK(clipped.lines[0] == "abcd...");
    CHECK(clipped.lines[1] == "xy");
    CHECK_FALSE(clipped.truncated);
}

// S6-44: the preview checkerboard's cell is a render-target texel count, not
// UI chrome (the S5-2 review restored it from DERIVED).
TEST_CASE("sweep: editor.shader.previewCheckerCell is the pre-sweep 32 texels", "[sweep][documents]")
{
    CHECK(Editor::ShaderEditorSettings{}.previewCheckerCell == 32.0f);
    if (!Test::InThisBuild("editor.shader.previewCheckerCell")) return;   // Dev: compiled out of Dist
    Test::RequireDefault("editor.shader.previewCheckerCell", CVarValue::Float32(32.0f));
    const std::optional<CVarDescInfo> d = CVarRegistry::Get().Describe("editor.shader.previewCheckerCell");
    REQUIRE(d.has_value());
    CHECK(d->scope == SettingScope::PreferencesMachine);
    CHECK(d->apply == ApplyMode::Live);
    CHECK(HasFlag(d->flags, CVarFlags::Dev));
    REQUIRE(d->min.has_value());
    REQUIRE(d->max.has_value());
    CHECK(*d->min == CVarValue::Float32(4.0f));
    CHECK(*d->max == CVarValue::Float32(128.0f));
}
