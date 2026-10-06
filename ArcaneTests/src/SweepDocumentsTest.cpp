// Settings arc S6-35: editor.shader.* / editor.mesh.* / editor.sprite.* /
// editor.preview.* / editor.preview.light.* -- the shader editor, mesh and
// sprite documents' caps and drag speeds, the material sphere and checker, and
// the ONE preview light (R1) the sphere, the thumbnails and the mesh document
// share.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/DocumentSettings.hpp"
#include "Documents/MaterialSpherePreview.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

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
    CHECK(p.checkerLight.r == 0.16f); CHECK(p.checkerLight.g == 0.16f); CHECK(p.checkerLight.b == 0.19f);
    CHECK(p.checkerLight.a == 1.0f);
    CHECK(p.checkerExtent == 0.8f);
    const Editor::ShaderEditorSettings s{};
    CHECK(s.renameListMax == 8); CHECK(s.bodyPreviewLines == 8); CHECK(s.bodyPreviewChars == 48);
    CHECK(s.nodePreviewMinPx == 96.0f); CHECK(s.dragSpeed == 0.01f); CHECK(s.rangeDragSpeed == 0.05f);
    const Editor::MeshDocSettings m{};
    CHECK(m.previewResolution == 512); CHECK(m.previewMargin == 1.5f);
    CHECK(m.subdivMax == 64); CHECK(m.ringsMax == 128); CHECK(m.capsuleRingsMax == 64); CHECK(m.segmentsMax == 128);
    CHECK(m.capsuleRatioMax == 20.0f); CHECK(m.capsuleRatioDragSpeed == 0.02f);
    const Editor::SpriteDocSettings sp{};
    CHECK(sp.ppuDragSpeed == 0.5f); CHECK(sp.ppuMin == 1.0f); CHECK(sp.ppuMax == 4096.0f);

    Test::RequireDefault("editor.preview.light.direction", CVarValue::Vec3({ 0.45f, 0.7f, 0.8f }));
    Test::RequireDefault("editor.preview.light.color", CVarValue::Color({ 1.0f, 1.0f, 1.0f, 1.0f }));
    Test::RequireDefault("editor.preview.sphereRings", CVarValue::Int32(24));
    Test::RequireDefault("editor.preview.sphereSegments", CVarValue::Int32(32));
    Test::RequireDefault("editor.preview.sphereFov", CVarValue::Float32(35.0f));
    Test::RequireDefault("editor.preview.checkerLight", CVarValue::Color({ 0.16f, 0.16f, 0.19f, 1.0f }));
    Test::RequireDefault("editor.preview.checkerExtent", CVarValue::Float32(0.8f));
    Test::RequireDefault("editor.shader.navHistoryMax", CVarValue::Int32(32));
    Test::RequireDefault("editor.shader.previewResolution", CVarValue::Int32(512));
    Test::RequireDefault("editor.shader.renameListMax", CVarValue::Int32(8));
    Test::RequireDefault("editor.shader.bodyPreviewLines", CVarValue::Int32(8));
    Test::RequireDefault("editor.shader.bodyPreviewChars", CVarValue::Int32(48));
    Test::RequireDefault("editor.shader.nodePreviewMinPx", CVarValue::Float32(96.0f));
    Test::RequireDefault("editor.shader.dragSpeed", CVarValue::Float32(0.01f));
    Test::RequireDefault("editor.shader.rangeDragSpeed", CVarValue::Float32(0.05f));
    Test::RequireDefault("editor.mesh.previewResolution", CVarValue::Int32(512));
    Test::RequireDefault("editor.mesh.previewFov", CVarValue::Float32(45.0f));
    Test::RequireDefault("editor.mesh.previewMargin", CVarValue::Float32(1.5f));
    Test::RequireDefault("editor.mesh.subdivMax", CVarValue::Int32(64));
    Test::RequireDefault("editor.mesh.ringsMax", CVarValue::Int32(128));
    Test::RequireDefault("editor.mesh.capsuleRingsMax", CVarValue::Int32(64));
    Test::RequireDefault("editor.mesh.segmentsMax", CVarValue::Int32(128));
    Test::RequireDefault("editor.mesh.capsuleRatioMax", CVarValue::Float32(20.0f));
    Test::RequireDefault("editor.mesh.capsuleRatioDragSpeed", CVarValue::Float32(0.02f));
    Test::RequireDefault("editor.sprite.ppuDragSpeed", CVarValue::Float32(0.5f));
    Test::RequireDefault("editor.sprite.ppuMin", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.sprite.ppuMax", CVarValue::Float32(4096.0f));
    Test::RequireDefault("editor.sprite.pivotDragSpeed", CVarValue::Float32(0.005f));
}
