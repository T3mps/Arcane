// End-to-end slice: substrate (Registry+schedulers+RunLoop) advances a parented
// scene, then the render phase submits sprites to a Batcher2D into an offscreen
// HDR canvas. Asserts both sprites are submitted and NVRHI validation is silent.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

namespace
{
    void RunSlice(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "data/shaders");
        REQUIRE(shaders != nullptr);
        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 64, 64);
        REQUIRE(canvas != nullptr);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        REQUIRE(batcher != nullptr);

        Arcane::JobSystem jobs;
        auto sched = jobs.WorkScheduler();

        Astra::Registry::Config cfg;
        cfg.workScheduler = sched;
        Astra::Registry reg(cfg);
        Arcane::RegisterSceneComponents(reg);

        Arcane::SystemSchedulers schedulers(sched);
        // These two systems are what the sprite-count/validation assertions
        // below actually exercise; a silent registration failure would make
        // this slice test pass without submitting anything.
        REQUIRE(schedulers.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>().IsOk());
        REQUIRE(schedulers.render.AddSystem<Arcane::RenderSubmissionSystem>().IsOk());

        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});

        Astra::Entity parent = reg.CreateEntity();
        Arcane::Transform pT; pT.position = glm::vec2(10, 10);
        reg.AddComponent<Arcane::Transform>(parent, pT);
        reg.AddComponent<Arcane::WorldTransform>(parent, Arcane::WorldTransform{});
        reg.AddComponent<Arcane::SpriteRenderer>(parent, Arcane::SpriteRenderer{});
        reg.SetParent(parent, root);

        Astra::Entity child = reg.CreateEntity();
        Arcane::Transform cT; cT.position = glm::vec2(5, 5);
        reg.AddComponent<Arcane::Transform>(child, cT);
        reg.AddComponent<Arcane::WorldTransform>(child, Arcane::WorldTransform{});
        Arcane::SpriteRenderer csr; csr.tint = glm::vec4(0.2f, 0.8f, 0.3f, 1.0f);
        reg.AddComponent<Arcane::SpriteRenderer>(child, csr);
        reg.SetParent(child, parent);

        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        // SpriteTable is not set: GetResource<SpriteTable>() returns nullptr, so
        // RenderSubmissionSystem falls back to Rect() for all sprites (correct
        // for a nil .arcsprite Guid anyway). Both sprites therefore draw the
        // untextured 1x1 m base quad -- still one quad each, which is what the
        // >= 2 assertion below is about.

        Arcane::RunLoop loop(reg, schedulers);
        for (int i = 0; i < 4; ++i)
            loop.Advance(1.0 / 60.0);   // FixedUpdate: transform propagation

        nvrhi::CommandListHandle cl = device->Nvrhi()->createCommandList();
        cl->open();
        cl->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                              nvrhi::Color(0, 0, 0, 1));
        batcher->Begin(cl, canvas->Framebuffer(), canvas->Width(), canvas->Height());
        reg.SetResource<Arcane::RenderContext2D>(
            Arcane::RenderContext2D{batcher.get(), glm::vec2(0, 0)});
        loop.SubmitRender();   // RenderSubmissionSystem -> batcher
        batcher->End();
        cl->close();
        device->Nvrhi()->executeCommandList(cl);
        device->Nvrhi()->waitForIdle();

        CHECK(batcher->Stats().quads >= 2);
        CHECK(Arcane::RenderErrorCount() == 0);
        device->Nvrhi()->runGarbageCollection();
    }
}

TEST_CASE("d3d12: scene slice submits sprites with no validation errors", "[gpu][d3d12][scene]")
{
    RunSlice(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: scene slice submits sprites with no validation errors", "[gpu][vulkan][scene]")
{
    RunSlice(Arcane::GraphicsBackend::Vulkan);
}
