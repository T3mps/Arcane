// Settings arc S6-19: the render look -- the canvas clear colour, the sprite
// filter, texture anisotropy, the canvas and depth formats, the default mesh
// light, the CPU cull slack and render.meshCull. The clear colour, the
// canvas format and the default light are golden-bound: every default is the
// literal it replaces, bit for bit.

// NRI headers first (NriCommon.hpp's include-order rule).
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>
#include <Arcane/Render/RenderLookSettings.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>

using namespace Arcane;

TEST_CASE("sweep: render look defaults are the pre-sweep literals (golden-bound)", "[sweep][render-look]")
{
    const CVarColor c = RenderSettings{}.clearColor;
    CHECK(Test::SameBits(c.r, 0.02f)); CHECK(Test::SameBits(c.g, 0.02f)); CHECK(Test::SameBits(c.b, 0.04f)); CHECK(Test::SameBits(c.a, 1.0f));
    CHECK(RenderSettings{}.textureAnisotropy == 16u);
    CHECK(RenderSettings{}.canvasFormat == CanvasFormat::Rgba16f);
    CHECK(RenderSettings{}.depthFormat == DepthFormat::D32);
    CHECK(RenderSettings{}.meshCull);
    CHECK(RenderSpriteSettings{}.filter == SamplerFilter::Linear);
    const MeshSceneDesc scene{};
    CHECK(scene.lightDirection == glm::vec3(0, 0, 1));
    CHECK(scene.lightColor == glm::vec3(1, 1, 1));
    CHECK(scene.ambient == glm::vec3(0.05f));
    CHECK(Test::SameBits(RenderCullSettings{}.frustumSlackMeters, 0.25f));
    Test::RequireDefault("render.meshCull", CVarValue::Bool(true));
    CHECK(MeshCullFrustumEnabled());
}

TEST_CASE("sweep: render look cvars are registered with their declared defaults", "[sweep][render-look]")
{
    Test::RequireDefault("render.clearColor", CVarValue::Color(CVarColor{ 0.02f, 0.02f, 0.04f, 1.0f }));
    Test::RequireDefault("render.textureAnisotropy", CVarValue::UInt32(16u));
    Test::RequireDefault("render.cull.frustumSlackMeters", CVarValue::Float32(0.25f));
    Test::RequireDefault("render.mesh.defaultLight.direction", CVarValue::Vec3(CVarVec3{ 0.0f, 0.0f, 1.0f }));
    Test::RequireDefault("render.mesh.defaultLight.color", CVarValue::Color(CVarColor{ 1.0f, 1.0f, 1.0f, 1.0f }));
    Test::RequireDefault("render.mesh.defaultLight.ambient", CVarValue::Color(CVarColor{ 0.05f, 0.05f, 0.05f, 1.0f }));
    // Enums store the declared ordinal (Integration ruling I7).
    Test::RequireDefault("render.sprite.filter", CVarValue::Enum(0));
    Test::RequireDefault("render.canvasFormat", CVarValue::Enum(0));
    Test::RequireDefault("render.depthFormat", CVarValue::Enum(0));
}

TEST_CASE("sweep: the canvas and depth format settings map to the NRI formats the graph used", "[sweep][render-look]")
{
    CHECK(ToNriFormat(CanvasFormat::Rgba16f) == nri::Format::RGBA16_SFLOAT);
    CHECK(ToNriFormat(CanvasFormat::R11g11b10f) == nri::Format::R11_G11_B10_UFLOAT);
    CHECK(ToNriFormat(DepthFormat::D32) == nri::Format::D32_SFLOAT);
    CHECK(ToNriFormat(DepthFormat::D24s8) == nri::Format::D24_UNORM_S8_UINT);
    // The process latch, at the defaults: exactly the old kGraph* constants.
    CHECK(GraphCanvasFormat() == nri::Format::RGBA16_SFLOAT);
    CHECK(GraphDepthFormat() == nri::Format::D32_SFLOAT);
}

TEST_CASE("sweep: the default mesh light is applied from the published setting", "[sweep][render-look]")
{
    RenderMeshDefaultLightSettings light;
    light.direction = CVarVec3{ 0.5f, -1.0f, 2.0f };
    light.color     = CVarColor{ 0.25f, 0.5f, 0.75f, 1.0f };
    light.ambient   = CVarColor{ 0.125f, 0.0f, 1.0f, 1.0f };
    MeshSceneDesc scene;
    ApplyDefaultLight(scene, light);
    CHECK(scene.lightDirection == glm::vec3(0.5f, -1.0f, 2.0f));
    CHECK(scene.lightColor == glm::vec3(0.25f, 0.5f, 0.75f));
    CHECK(scene.ambient == glm::vec3(0.125f, 0.0f, 1.0f));

    // At the defaults the applied light is MeshSceneDesc's own, bit for bit.
    MeshSceneDesc byDefault;
    ApplyDefaultLight(byDefault, RenderMeshDefaultLightSettings{});
    CHECK(byDefault.lightDirection == MeshSceneDesc{}.lightDirection);
    CHECK(byDefault.lightColor == MeshSceneDesc{}.lightColor);
    CHECK(byDefault.ambient == MeshSceneDesc{}.ambient);
}

TEST_CASE("sweep: the CPU cull slack is render.cull.frustumSlackMeters", "[sweep][render-look]")
{
    CHECK(Test::SameBits(VisibilitySlackMeters(), 0.25f));
}
