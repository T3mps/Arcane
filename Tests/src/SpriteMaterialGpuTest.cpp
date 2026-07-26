// Sprite materials through the ONE Batcher2D path (Slice 8 gate): a compiled
// sprite-surface material registers with the batcher, draws via QuadMaterial
// into the OffscreenCanvas (canvas -> batcher -> tonemap), and the output
// pixels match the material's params -- including a live instance override
// (the pack loop runs inside Batcher2D::End). Built-in draws share the frame,
// proving the material table coexists with the pre-material pipelines.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <cstdint>
#include <memory>

namespace
{
    nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device, Arcane::GraphicsBackend backend,
                                   const Arcane::ShaderCompileResult& r,
                                   nvrhi::ShaderType type, const char* entry)
    {
        const auto& target = backend == Arcane::GraphicsBackend::Vulkan ? r.spirv : r.dxil;
        for (const Arcane::ShaderDiag& d : target.diags)
            UNSCOPED_INFO(entry << ": " << d.line << ": " << d.message);
        REQUIRE(target.succeeded);
        auto desc = nvrhi::ShaderDesc()
            .setShaderType(type)
            .setEntryName(entry)
            .setDebugName(r.debugName);
        return device->createShader(desc, target.bytecode.data(), target.bytecode.size());
    }

    void CheckSpriteMaterialPixels(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        REQUIRE(shaders != nullptr);

        constexpr uint32_t kSize = 64;
        auto oc = Arcane::OffscreenCanvas::Create(device->Nvrhi(), *shaders, kSize, kSize);
        REQUIRE(oc != nullptr);

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("shaders");
        const auto templateText = provider.Get("materials/sprite_material.hlsl");
        REQUIRE(templateText.has_value());

        // Tint * vertex color * the sprite texture (white fallback) -> solid.
        const char* snippet =
            "//@param color Tint = (0, 1, 0, 1)\n"
            "float4 shade(Varyings v)\n"
            "{\n"
            "    return Tint * v.color * SpriteTexture.Sample(MaterialSampler, v.uv);\n"
            "}\n";
        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText, snippet, "sprite_gpu_test", Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler sc;
        REQUIRE(sc.Initialize(0.0));
        Arcane::ShaderCompileRequest req;
        req.debugName = "sprite_gpu_test.hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        const auto psResult = sc.CompileNow(req);
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        const auto vsResult = sc.CompileNow(req);

        auto templ = std::make_shared<Arcane::MaterialTemplate>(build.templ);
        auto instance = std::make_shared<Arcane::MaterialInstance>(templ);

        Arcane::Material2DDesc mat;
        mat.vs = MakeShader(device->Nvrhi(), backend, vsResult,
                            nvrhi::ShaderType::Vertex, Arcane::kVsEntry);
        mat.ps = MakeShader(device->Nvrhi(), backend, psResult,
                            nvrhi::ShaderType::Pixel, Arcane::kPsEntry);
        REQUIRE(mat.vs != nullptr);
        REQUIRE(mat.ps != nullptr);
        mat.templ = templ;
        mat.instance = instance;

        Arcane::Batcher2D& batcher = oc->Batch();
        const uint16_t materialId = batcher.RegisterMaterial(mat);
        REQUIRE(materialId != Arcane::Batcher2D::kInvalidMaterialId);
        REQUIRE(materialId >= 3);   // built-ins own 0..2

        Arcane::GlobalParams globals;
        globals.viewportWidth = (float)kSize;
        globals.viewportHeight = (float)kSize;

        auto RenderAndReadCenter = [&](uint8_t out[4])
        {
            oc->Draw(
                [&](Arcane::Batcher2D& b)
                {
                    b.SetGlobals(globals);
                    // A built-in draw shares the frame (table coexistence),
                    // then the material quad covers the whole canvas.
                    b.Rect(glm::vec2(0, 0), glm::vec2(4, 4),
                           glm::vec4(1, 0, 1, 1));
                    b.QuadMaterial(materialId, glm::vec2(0, 0),
                                   glm::vec2((float)kSize, (float)kSize), nullptr,
                                   glm::vec2(0, 0), glm::vec2(1, 1),
                                   glm::vec4(1, 1, 1, 1));
                },
                glm::vec4(0, 0, 0, 1));

            auto* output = reinterpret_cast<nvrhi::ITexture*>(
                static_cast<uintptr_t>(oc->TextureId()));
            REQUIRE(output != nullptr);

            nvrhi::IDevice* nv = device->Nvrhi();
            auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(kSize).setHeight(kSize)
                .setFormat(nvrhi::Format::BGRA8_UNORM)
                .setDebugName("SpriteMaterialReadback");
            nvrhi::StagingTextureHandle staging =
                nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
            REQUIRE(staging != nullptr);

            nvrhi::CommandListHandle cl = nv->createCommandList();
            cl->open();
            cl->copyTexture(staging, nvrhi::TextureSlice(), output, nvrhi::TextureSlice());
            cl->close();
            nv->executeCommandList(cl);
            nv->waitForIdle();

            size_t rowPitch = 0;
            const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
                staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
            REQUIRE(pixels != nullptr);
            const uint8_t* mid = pixels + (kSize / 2) * rowPitch + (kSize / 2) * 4;
            out[0] = mid[0]; out[1] = mid[1]; out[2] = mid[2]; out[3] = mid[3];
            nv->unmapStagingTexture(staging);
        };

        // Default Tint (0,1,0,1): tonemapped green, other channels dark.
        uint8_t bgra[4] = {};
        RenderAndReadCenter(bgra);
        CHECK((int)bgra[1] > 200);   // G
        CHECK((int)bgra[2] < 64);    // R
        CHECK((int)bgra[0] < 64);    // B

        // Live override through the batcher's pack-at-End loop: red, no recompile.
        REQUIRE(instance->SetColor("Tint", 1.0f, 0.0f, 0.0f, 1.0f));
        RenderAndReadCenter(bgra);
        CHECK((int)bgra[2] > 200);   // R
        CHECK((int)bgra[1] < 64);    // G

        sc.Shutdown();
        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: a sprite material renders through Batcher2D with live overrides",
          "[gpu][material][d3d12]")
{
    CheckSpriteMaterialPixels(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: a sprite material renders through Batcher2D with live overrides",
          "[gpu][material][vulkan]")
{
    CheckSpriteMaterialPixels(Arcane::GraphicsBackend::Vulkan);
}
