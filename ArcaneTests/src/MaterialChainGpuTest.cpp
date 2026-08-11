// FullscreenMaterialChain proof (queue item 4b): a three-pass chain renders
// through the REAL engine path -- BuildMaterialChainSource, runtime dual-target
// compile, SetChain, ping-pong intermediates, OffscreenCanvas::DrawPass, ACES
// tonemap -- and the output pixels prove each pass READ the previous pass's
// output through InputTexture. viewIndex truncation shows every intermediate,
// and a live override through the ONE shared CB reaches a late pass.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialChain.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    nvrhi::ShaderHandle MakeChainShader(nvrhi::IDevice* device,
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

    void CheckChainPixels(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "data/shaders");
        REQUIRE(shaders != nullptr);

        constexpr uint32_t kSize = 64;
        auto oc = Arcane::OffscreenCanvas::Create(device->Nvrhi(), *shaders, kSize, kSize);
        REQUIRE(oc != nullptr);

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());

        // Color-rotating DAG (the bloom composite shape): each pass's output is
        // a distinct primary, and the last pass reads TWO upstream passes.
        //   p0: green -> p1 (reads p0): .grba = red
        //   p2 (reads p1 AND p0): p1.bgra=blue + p0=green -> cyan, * Gain.
        const std::string_view pass0 =
            "float4 shade(Varyings v) { return float4(0.0, 1.0, 0.0, 1.0); }\n";
        const std::string_view pass1 =
            "float4 shade(Varyings v)\n"
            "{ return InputTexture.Sample(MaterialSampler, v.uv).grba; }\n";
        const std::string_view pass2 =
            "//@param float Gain = 1.0\n"
            "float4 shade(Varyings v)\n"
            "{\n"
            "    float4 a = InputTexture.Sample(MaterialSampler, v.uv);\n"
            "    float4 b = InputTexture1.Sample(MaterialSampler, v.uv);\n"
            "    return float4(a.bgr + b.rgb, 1.0) * Gain;\n"
            "}\n";
        const std::uint32_t p1in[] = { 0 };
        const std::uint32_t p2in[] = { 1, 0 };
        const Arcane::MaterialChainPassDesc descs[] = {
            { pass0, {} }, { pass1, p1in }, { pass2, p2in },
        };

        const Arcane::MaterialChainBuildResult build =
            Arcane::BuildMaterialChainSource(*templateText, descs, "chain_gpu");
        REQUIRE(build.Ok());
        REQUIRE(build.hlsl.size() == 3);
        REQUIRE(build.chainInputSlots == 2);

        Arcane::ShaderCompiler sc;
        REQUIRE(sc.Initialize(0.0));
        std::vector<Arcane::FullscreenMaterialChain::PassShaders> passShaders;
        for (std::size_t p = 0; p < build.hlsl.size(); ++p)
        {
            Arcane::ShaderCompileRequest req;
            req.debugName = "chain_gpu_p" + std::to_string(p) + ".hlsl";
            req.sourceUtf8 = build.hlsl[p];
            req.entry = Arcane::kVsEntry;
            req.profile = Arcane::kVsProfile;
            const auto vsResult = sc.CompileNow(req);
            req.entry = Arcane::kPsEntry;
            req.profile = Arcane::kPsProfile;
            const auto psResult = sc.CompileNow(req);
            Arcane::FullscreenMaterialChain::PassShaders ps;
            ps.vs = MakeChainShader(device->Nvrhi(), backend, vsResult,
                                    nvrhi::ShaderType::Vertex, Arcane::kVsEntry);
            ps.ps = MakeChainShader(device->Nvrhi(), backend, psResult,
                                    nvrhi::ShaderType::Pixel, Arcane::kPsEntry);
            ps.inputs = build.passInputs[p];
            REQUIRE(ps.vs != nullptr);
            REQUIRE(ps.ps != nullptr);
            passShaders.push_back(ps);
        }

        auto chain = Arcane::FullscreenMaterialChain::Create(device->Nvrhi());
        REQUIRE(chain != nullptr);
        auto templ = std::make_shared<Arcane::MaterialTemplate>(build.templ);
        REQUIRE(chain->SetChain(templ, passShaders, build.chainInputSlots));
        REQUIRE(chain->Ready());
        CHECK(chain->PassCount() == 3);

        Arcane::MaterialInstance instance(templ);
        Arcane::GlobalParams globals;
        globals.viewportWidth = (float)kSize;
        globals.viewportHeight = (float)kSize;

        auto RenderAndReadCenter = [&](std::size_t viewIndex, uint8_t out[4])
        {
            oc->DrawPass(
                [&](nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb)
                { chain->Render(cl, fb, instance, globals, nullptr, viewIndex); },
                glm::vec4(0, 0, 0, 1));

            auto* output = reinterpret_cast<nvrhi::ITexture*>(
                static_cast<uintptr_t>(oc->TextureId()));
            REQUIRE(output != nullptr);

            nvrhi::IDevice* nv = device->Nvrhi();
            auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(kSize).setHeight(kSize)
                .setFormat(nvrhi::Format::BGRA8_UNORM)
                .setDebugName("ChainReadback");
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

        uint8_t bgra[4] = {};

        // Full chain (default view = last pass): cyan -- proof p2 read BOTH
        // upstream passes (blue from p1's swizzle + green straight from p0).
        RenderAndReadCenter(static_cast<std::size_t>(-1), bgra);
        CHECK((int)bgra[0] > 200);   // B
        CHECK((int)bgra[1] > 200);   // G
        CHECK((int)bgra[2] < 64);    // R

        // View any pass: pass 0 alone is green, pass 1 is red.
        RenderAndReadCenter(0, bgra);
        CHECK((int)bgra[1] > 200);   // G
        CHECK((int)bgra[0] < 64);
        CHECK((int)bgra[2] < 64);
        RenderAndReadCenter(1, bgra);
        CHECK((int)bgra[2] > 200);   // R
        CHECK((int)bgra[0] < 64);
        CHECK((int)bgra[1] < 64);

        // Per-pass intermediates stay live for thumbnails.
        CHECK(chain->PassOutput(0) != nullptr);
        CHECK(chain->PassOutput(1) != nullptr);
        CHECK(chain->PassOutput(2) != nullptr);
        CHECK(chain->PassOutput(3) == nullptr);

        // One shared CB serves every pass: a live override on the merged
        // instance reaches pass 2 without a recompile.
        REQUIRE(instance.SetFloat("Gain", 0.0f));
        RenderAndReadCenter(static_cast<std::size_t>(-1), bgra);
        CHECK((int)bgra[0] < 32);
        CHECK((int)bgra[1] < 32);
        CHECK((int)bgra[2] < 32);

        sc.Shutdown();
        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: a pass chain ping-pongs InputTexture with any-view truncation",
          "[gpu][material][d3d12]")
{
    CheckChainPixels(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: a pass chain ping-pongs InputTexture with any-view truncation",
          "[gpu][material][vulkan]")
{
    CheckChainPixels(Arcane::GraphicsBackend::Vulkan);
}
