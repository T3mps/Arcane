// FullscreenMaterialPass first-pixels proof (Slice 4 gate): a known material
// renders through the REAL engine path -- runtime dual-target compile,
// createShader at the drain site, OffscreenCanvas::DrawPass into the linear
// canvas, ACES tonemap -- and the output pixels match the material's params,
// including a live instance override (the pack loop on the GPU).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>
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
        REQUIRE(target.succeeded);
        auto desc = nvrhi::ShaderDesc()
            .setShaderType(type)
            .setEntryName(entry)          // SPIR-V OpEntryPoint lookup; DXIL ignores
            .setDebugName(r.debugName);
        return device->createShader(desc, target.bytecode.data(), target.bytecode.size());
    }

    void CheckMaterialFirstPixels(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        // ShaderLibrary only feeds the OffscreenCanvas's batcher/tonemap.
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        REQUIRE(shaders != nullptr);

        constexpr uint32_t kSize = 64;
        auto oc = Arcane::OffscreenCanvas::Create(device->Nvrhi(), *shaders, kSize, kSize);
        REQUIRE(oc != nullptr);

        // Author a solid-color material through the real seams.
        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("shaders");
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());

        const char* snippet =
            "//@param color Tint = (0, 1, 0, 1)\n"
            "float4 shade(Varyings v) { return Tint; }\n";
        const Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, snippet, "gpu_test");
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler sc;
        REQUIRE(sc.Initialize(0.0));
        Arcane::ShaderCompileRequest req;
        req.debugName = "gpu_test.hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        const auto psResult = sc.CompileNow(req);
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        const auto vsResult = sc.CompileNow(req);

        // The drain-site contract: NVRHI shader creation happens here, on the
        // main thread, from finished bytecode.
        nvrhi::ShaderHandle vs = MakeShader(device->Nvrhi(), backend, vsResult,
                                            nvrhi::ShaderType::Vertex, Arcane::kVsEntry);
        nvrhi::ShaderHandle ps = MakeShader(device->Nvrhi(), backend, psResult,
                                            nvrhi::ShaderType::Pixel, Arcane::kPsEntry);
        REQUIRE(vs != nullptr);
        REQUIRE(ps != nullptr);

        auto pass = Arcane::FullscreenMaterialPass::Create(device->Nvrhi());
        REQUIRE(pass != nullptr);
        auto templ = std::make_shared<Arcane::MaterialTemplate>(build.templ);
        REQUIRE(pass->SetMaterial(templ, vs, ps));
        REQUIRE(pass->Ready());

        Arcane::MaterialInstance instance(templ);
        Arcane::GlobalParams globals;
        globals.viewportWidth = (float)kSize;
        globals.viewportHeight = (float)kSize;

        auto RenderAndReadCenter = [&](uint8_t out[4])
        {
            oc->DrawPass(
                [&](nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb)
                { pass->Render(cl, fb, instance, globals, nullptr); },
                glm::vec4(0, 0, 0, 1));

            auto* output = reinterpret_cast<nvrhi::ITexture*>(
                static_cast<uintptr_t>(oc->TextureId()));
            REQUIRE(output != nullptr);

            nvrhi::IDevice* nv = device->Nvrhi();
            auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(kSize).setHeight(kSize)
                .setFormat(nvrhi::Format::BGRA8_UNORM)
                .setDebugName("MaterialReadback");
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

        // Default Tint (0,1,0,1): tonemapped green ~231, other channels dark.
        uint8_t bgra[4] = {};
        RenderAndReadCenter(bgra);
        CHECK((int)bgra[1] > 200);   // G
        CHECK((int)bgra[2] < 64);    // R
        CHECK((int)bgra[0] < 64);    // B

        // Live override through the pack loop: no recompile, red output.
        REQUIRE(instance.SetColor("Tint", 1.0f, 0.0f, 0.0f, 1.0f));
        RenderAndReadCenter(bgra);
        CHECK((int)bgra[2] > 200);   // R
        CHECK((int)bgra[1] < 64);    // G

        sc.Shutdown();
        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: a compiled material renders first pixels with live overrides",
          "[gpu][material][d3d12]")
{
    CheckMaterialFirstPixels(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: a compiled material renders first pixels with live overrides",
          "[gpu][material][vulkan]")
{
    CheckMaterialFirstPixels(Arcane::GraphicsBackend::Vulkan);
}
