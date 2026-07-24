// FullscreenMaterialPass first-pixels proof (Slice 4 gate): a known material
// renders through the REAL engine path -- runtime dual-target compile,
// createShader at the drain site, OffscreenCanvas::DrawPass into the linear
// canvas, ACES tonemap -- and the output pixels match the material's params,
// including a live instance override (the pack loop on the GPU).

#include <catch2/catch_test_macros.hpp>

// Implementation owned by VendorSmokeTest.cpp (header-only include here).
#include <stb_image_write.h>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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

namespace
{
    // A texture param through the WHOLE guid chain: png in a real project's
    // Content/ -> scan mints the .meta guid -> instance override carries the
    // guid -> FullscreenMaterialPass resolves it through the Assets facade at
    // bind time. The review flagged this path as never GPU-exercised.
    void CheckTextureParamPixels(Arcane::GraphicsBackend backend)
    {
        namespace fs = std::filesystem;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        REQUIRE(shaders != nullptr);
        constexpr uint32_t kSize = 64;
        auto oc = Arcane::OffscreenCanvas::Create(device->Nvrhi(), *shaders, kSize, kSize);
        REQUIRE(oc != nullptr);

        // Real project + a solid-green png; the scan mints the sidecar guid.
        const fs::path dir = fs::temp_directory_path() / "arcane_mat_texparam";
        std::error_code ec;
        fs::remove_all(dir, ec);
        REQUIRE(Arcane::Project::Create(dir / "Game", "TexParam").has_value());
        {
            const unsigned char px[2 * 2 * 4] = {
                0, 255, 0, 255,  0, 255, 0, 255,
                0, 255, 0, 255,  0, 255, 0, 255,
            };
            const auto png = (dir / "Game" / "Content" / "green.png").string();
            REQUIRE(stbi_write_png(png.c_str(), 2, 2, 4, px, 8) != 0);
        }
        auto project = Arcane::Project::Open(dir / "Game");
        REQUIRE(project.has_value());

        // The minted guid, from the sidecar (crude parse, zero extra deps).
        std::ifstream metaIn(dir / "Game" / "Content" / "green.png.meta");
        std::string meta((std::istreambuf_iterator<char>(metaIn)),
                         std::istreambuf_iterator<char>());
        const auto at = meta.find("\"guid\": \"");
        REQUIRE(at != std::string::npos);
        const auto guid = Arcane::Guid::FromString(meta.substr(at + 9, 36));
        REQUIRE(guid.has_value());

        auto assets = Arcane::Assets::Create(device->Nvrhi(), Arcane::AssetsDesc{});
        REQUIRE(assets != nullptr);
        assets->SetAssetResolver(
            [p = &*project](const Arcane::AssetId& id) { return p->ResolveAsset(id); });

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("shaders");
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const char* snippet =
            "//@param texture Tex\n"
            "float4 shade(Varyings v) { return Tex.Sample(MaterialSampler, v.uv); }\n";
        const Arcane::MaterialBuildResult build =
            Arcane::BuildMaterialShaderSource(*templateText, snippet, "texparam_gpu");
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler sc;
        REQUIRE(sc.Initialize(0.0));
        Arcane::ShaderCompileRequest req;
        req.debugName = "texparam_gpu.hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        const auto psResult = sc.CompileNow(req);
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        const auto vsResult = sc.CompileNow(req);
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

        Arcane::MaterialInstance instance(templ);
        REQUIRE(instance.Set("Tex", Arcane::MatParamValue::MakeTexture(*guid)));
        Arcane::GlobalParams globals;
        globals.viewportWidth = (float)kSize;
        globals.viewportHeight = (float)kSize;

        // THE CONTRACT (found the hard way -- the editor's logo material drew
        // nothing): texture params must be loaded BEFORE the canvas list is
        // recording. Assets uploads execute their own transient list, and an
        // executeCommandList issued while another list is open loses the
        // upload on BOTH backends -- the texture binds but samples (0,0,0,0).
        // Callers (ShaderEditorDocument::Tick, SpriteMaterialCache::Bind)
        // pre-resolve; FullscreenMaterialPass::Render documents the rule.
        REQUIRE(assets->GetTexture(Arcane::AssetId::FromGuid(*guid)) != nullptr);

        oc->DrawPass(
            [&](nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb)
            { pass->Render(cl, fb, instance, globals, assets.get()); },
            glm::vec4(0, 0, 0, 1));

        auto* output = reinterpret_cast<nvrhi::ITexture*>(
            static_cast<uintptr_t>(oc->TextureId()));
        nvrhi::IDevice* nv = device->Nvrhi();
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(kSize).setHeight(kSize)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("TexParamReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
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
        const int b = mid[0], g = mid[1], r = mid[2];
        nv->unmapStagingTexture(staging);

        INFO("center BGR = " << b << "," << g << "," << r
             << " (white => fallback bound; black => empty sample)");
        CHECK(g > 200);   // the green texel, tonemapped
        CHECK(r < 64);
        CHECK(b < 64);

        sc.Shutdown();
        nv->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
        fs::remove_all(dir, ec);
    }
}

TEST_CASE("d3d12: a texture param resolves by guid through the Assets facade",
          "[gpu][material][d3d12]")
{
    CheckTextureParamPixels(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: a texture param resolves by guid through the Assets facade",
          "[gpu][material][vulkan]")
{
    CheckTextureParamPixels(Arcane::GraphicsBackend::Vulkan);
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
