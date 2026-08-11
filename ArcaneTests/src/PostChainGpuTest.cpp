// Post arc slice 2 proof: the scene post hook and its cache render through
// the REAL engine path. Part one drives OffscreenCanvas::SetPostChain -- the
// batcher-drawn scene feeds a post-mode chain as its external Scene input,
// the tonemap samples the chain's output, and clearing the hook is
// BYTE-IDENTICAL to the never-hooked path (the arc's free tripwire). Part two
// drives PostChainCache end-to-end off SAVED .arcmat assets: async compile
// through the shared service, instance-over-base layering, atomic bind, and
// last-good across a broken re-save.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialChain.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t kSize = 64;

    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d =
            std::filesystem::temp_directory_path() / "arcane_post_chain_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    nvrhi::ShaderHandle MakeShader(nvrhi::IDevice* device,
                                   Arcane::GraphicsBackend backend,
                                   const Arcane::ShaderCompileResult& r,
                                   nvrhi::ShaderType type, const char* entry)
    {
        const auto& target = backend == Arcane::GraphicsBackend::Vulkan ? r.spirv : r.dxil;
        REQUIRE(target.succeeded);
        auto desc = nvrhi::ShaderDesc()
            .setShaderType(type)
            .setEntryName(entry)
            .setDebugName(r.debugName);
        return device->createShader(desc, target.bytecode.data(), target.bytecode.size());
    }

    // The whole output image, tightly packed BGRA rows -- full-buffer compares
    // are the byte-identity proof.
    std::vector<uint8_t> ReadOutput(nvrhi::IDevice* nv, Arcane::OffscreenCanvas& oc)
    {
        auto* output = reinterpret_cast<nvrhi::ITexture*>(
            static_cast<uintptr_t>(oc.TextureId()));
        REQUIRE(output != nullptr);

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(kSize).setHeight(kSize)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("PostChainReadback");
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
        std::vector<uint8_t> out(kSize * kSize * 4);
        for (uint32_t y = 0; y < kSize; ++y)
            std::memcpy(out.data() + y * kSize * 4, pixels + y * rowPitch, kSize * 4);
        nv->unmapStagingTexture(staging);
        return out;
    }

    const uint8_t* Center(const std::vector<uint8_t>& image)
    {
        return image.data() + ((kSize / 2) * kSize + kSize / 2) * 4;
    }

    void CheckPostChainPixels(Arcane::GraphicsBackend backend, const char* leaf)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "data/shaders");
        REQUIRE(shaders != nullptr);
        auto oc = Arcane::OffscreenCanvas::Create(nv, *shaders, kSize, kSize);
        REQUIRE(oc != nullptr);

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText =
            provider.Get(Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Fullscreen));
        REQUIRE(templateText.has_value());

        Arcane::ShaderCompiler sc;
        REQUIRE(sc.Initialize(0.0));

        Arcane::GlobalParams globals;
        globals.viewportWidth = (float)kSize;
        globals.viewportHeight = (float)kSize;

        const auto drawScene = [&]
        {
            // The scene is the clear color alone: solid linear red through the
            // canonical Draw() path (batcher pass opens + closes, no records).
            oc->Draw([](Arcane::Batcher2D&) {}, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        };

        // --- the hook: scene -> chain -> tonemap --------------------------------
        // One post-mode pass whose ONLY input is the Scene sentinel: invert.
        const std::string_view invert =
            "float4 shade(Varyings v)\n"
            "{\n"
            "    float4 s = InputTexture.Sample(MaterialSampler, v.uv);\n"
            "    return float4(1.0 - s.rgb, 1.0);\n"
            "}\n";
        const std::uint32_t sceneIn[] = { Arcane::kSceneInput };
        const Arcane::MaterialChainPassDesc descs[] = { { invert, sceneIn } };
        Arcane::MaterialChainBuildResult build = Arcane::BuildMaterialChainSource(
            *templateText, descs, "post_hook", {}, /*externalInput=*/true);
        REQUIRE(build.Ok());
        REQUIRE(build.hlsl.size() == 1);

        Arcane::ShaderCompileRequest req;
        req.debugName = "post_hook.hlsl";
        req.sourceUtf8 = build.hlsl[0];
        req.entry = Arcane::kVsEntry;
        req.profile = Arcane::kVsProfile;
        const auto vsResult = sc.CompileNow(req);
        req.entry = Arcane::kPsEntry;
        req.profile = Arcane::kPsProfile;
        const auto psResult = sc.CompileNow(req);

        Arcane::FullscreenMaterialChain::PassShaders pass;
        pass.vs = MakeShader(nv, backend, vsResult, nvrhi::ShaderType::Vertex,
                             Arcane::kVsEntry);
        pass.ps = MakeShader(nv, backend, psResult, nvrhi::ShaderType::Pixel,
                             Arcane::kPsEntry);
        pass.inputs = build.passInputs[0];
        REQUIRE(pass.vs != nullptr);
        REQUIRE(pass.ps != nullptr);

        auto chain = Arcane::FullscreenMaterialChain::Create(nv);
        REQUIRE(chain != nullptr);
        auto templ = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
        REQUIRE(chain->SetChain(templ, { &pass, 1 }, build.chainInputSlots));
        Arcane::MaterialInstance instance(templ);

        // A: never hooked. Tonemapped red (sanity), the byte-identity baseline.
        drawScene();
        const std::vector<uint8_t> imageA = ReadOutput(nv, *oc);
        CHECK((int)Center(imageA)[2] > 200);   // R
        CHECK((int)Center(imageA)[1] < 64);    // G
        CHECK((int)Center(imageA)[0] < 64);    // B

        // B: hooked. The chain READ the scene (red -> inverted cyan).
        oc->SetPostGlobals(globals);
        oc->SetPostChain(chain.get(), &instance, nullptr);
        drawScene();
        const std::vector<uint8_t> imageB = ReadOutput(nv, *oc);
        CHECK((int)Center(imageB)[0] > 200);   // B
        CHECK((int)Center(imageB)[1] > 200);   // G
        CHECK((int)Center(imageB)[2] < 64);    // R

        // C: unhooked again -- BYTE-IDENTICAL to the never-hooked baseline.
        oc->SetPostChain(nullptr, nullptr, nullptr);
        drawScene();
        const std::vector<uint8_t> imageC = ReadOutput(nv, *oc);
        REQUIRE(imageC == imageA);

        // --- the cache: SAVED assets -> bound chain -----------------------------
        // Base post material (reads the scene, tints the inversion) + an
        // instance overriding the tint to green-only. The cache must layer
        // instance over base and bind the merged values.
        const auto dir = TempDir(leaf);
        Arcane::MaterialAssetData base;
        base.id = Arcane::Guid::Generate();
        base.name = "PostBase";
        base.snippet =
            "//@param color Tint = (1, 1, 1, 1)\n"
            "float4 shade(Varyings v)\n"
            "{\n"
            "    float4 s = InputTexture.Sample(MaterialSampler, v.uv);\n"
            "    return float4((1.0 - s.rgb) * Tint.rgb, 1.0);\n"
            "}\n";
        base.baseInputs = { Arcane::kSceneInput };
        REQUIRE(Arcane::SaveMaterialAsset(dir / "base.arcmat", base));

        Arcane::MaterialAssetData inst;
        inst.id = Arcane::Guid::Generate();
        inst.parent = base.id;
        inst.name = "PostInstance";
        inst.params.emplace_back("Tint",
            Arcane::MatParamValue::MakeColor(0.0f, 1.0f, 0.0f, 1.0f));
        REQUIRE(Arcane::SaveMaterialAsset(dir / "instance.arcmat", inst));

        std::unordered_map<Arcane::Guid, std::filesystem::path> files{
            { base.id, dir / "base.arcmat" },
            { inst.id, dir / "instance.arcmat" },
        };
        Arcane::PostChainCache::Services services;
        services.compiler = &sc;
        services.sources = &provider;
        services.device = nv;
        services.backend = backend;
        services.resolveAsset = [&files](const Arcane::Guid& g)
            -> std::optional<std::filesystem::path>
        {
            const auto it = files.find(g);
            return it != files.end() ? std::optional(it->second) : std::nullopt;
        };
        Arcane::PostChainCache cache(std::move(services));

        cache.Request(inst.id, 0.0);
        for (int i = 0; i < 2000 && !cache.Chain(inst.id); ++i)
        {
            sc.Poll(0.0);
            for (const Arcane::ShaderCompileResult& r : sc.Drain())
                cache.ConsumeResult(r);
            if (!cache.Chain(inst.id))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Arcane::FullscreenMaterialChain* cached = cache.Chain(inst.id);
        REQUIRE(cached != nullptr);
        REQUIRE(cached->Ready());
        CHECK(cached->PassCount() == 1);
        REQUIRE(cache.Instance(inst.id) != nullptr);

        // Red scene -> inverted (0,1,1) -> instance tint (0,1,0) = green.
        // Proves the Scene input AND the instance-over-base layering landed.
        oc->SetPostChain(cached, cache.Instance(inst.id), nullptr);
        drawScene();
        const std::vector<uint8_t> imageD = ReadOutput(nv, *oc);
        CHECK((int)Center(imageD)[1] > 200);   // G
        CHECK((int)Center(imageD)[2] < 64);    // R
        CHECK((int)Center(imageD)[0] < 64);    // B

        // Break the BASE on disk and invalidate: the failed re-compile must
        // keep the previous chain bound (last-good), still rendering green.
        base.snippet = "float4 shade(Varyings v) { return oops; }\n";
        REQUIRE(Arcane::SaveMaterialAsset(dir / "base.arcmat", base));
        cache.Invalidate(inst.id);
        cache.Request(inst.id, 0.0);
        int consumed = 0;
        for (int i = 0; i < 2000 && consumed < 1; ++i)
        {
            sc.Poll(0.0);
            for (const Arcane::ShaderCompileResult& r : sc.Drain())
                consumed += cache.ConsumeResult(r) ? 1 : 0;
            if (consumed < 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(consumed >= 1);   // the failed compile came back
        REQUIRE(cache.Chain(inst.id) == cached);
        REQUIRE(cache.Instance(inst.id) != nullptr);
        oc->SetPostChain(cache.Chain(inst.id), cache.Instance(inst.id), nullptr);
        drawScene();
        const std::vector<uint8_t> imageE = ReadOutput(nv, *oc);
        CHECK((int)Center(imageE)[1] > 200);   // G -- last-good kept rendering
        CHECK((int)Center(imageE)[2] < 64);

        oc->SetPostChain(nullptr, nullptr, nullptr);
        sc.Shutdown();
        nv->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: the scene post hook chains canvas -> material -> tonemap",
          "[gpu][material][d3d12]")
{
    CheckPostChainPixels(Arcane::GraphicsBackend::D3D12, "d3d12");
}

TEST_CASE("vulkan: the scene post hook chains canvas -> material -> tonemap",
          "[gpu][material][vulkan]")
{
    CheckPostChainPixels(Arcane::GraphicsBackend::Vulkan, "vulkan");
}
