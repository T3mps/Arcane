// Shader artifacts load on both backends. (The hot-reload mechanism test
// joins this file in the hot-reload task.)

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    void CheckShaderLoads(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "shaders");
        REQUIRE(shaders != nullptr);
        REQUIRE(shaders->Generation() == 1);

        REQUIRE(shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel) != nullptr);
        REQUIRE(shaders->Get("circle_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("circle_ps", nvrhi::ShaderType::Pixel) != nullptr);
        REQUIRE(shaders->Get("tonemap_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("tonemap_ps", nvrhi::ShaderType::Pixel) != nullptr);

        // Cached: same handle back.
        REQUIRE(shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex) ==
                shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex));

        // Missing artifact: null, not a crash. (Logs one ARC_ERROR -- that is
        // an engine log, not an NVRHI validation error; RenderErrorCount is
        // unaffected.)
        REQUIRE(shaders->Get("does_not_exist", nvrhi::ShaderType::Pixel) == nullptr);

        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: shader artifacts load", "[gpu][d3d12]")
{
    CheckShaderLoads(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: shader artifacts load", "[gpu][vulkan]")
{
    CheckShaderLoads(Arcane::GraphicsBackend::Vulkan);
}
