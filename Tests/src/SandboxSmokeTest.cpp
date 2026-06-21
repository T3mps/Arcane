// [sandbox][gpu] live integration: drive the Sandbox plugin through a real Runtime +
// Batcher2D into an offscreen HDR canvas. Mirrors LoomSliceTest but loads Sandbox.dll
// (the physics sandbox plugin) instead of PlaygroundGame.dll: the plugin installs the
// REAL engine stack (PhysicsSystem + TransformPropagationSystem in fixedUpdate;
// RenderSubmissionSystem + the physics-debug overlay in render), builds a scene, then we
// step ~30 fixed frames and submit a render frame. The gate is: no crash and the NVRHI/VK
// validation layer stays silent (RenderErrorCount() == 0). Asserts via batcher->Stats()
// only -> no scene-component TypeID is touched in the test module (the plugin owns the
// component identities under the shared context).
//
// Task 5 (scene roster): the plugin now ships 8 demo scenes + a SetScene/Reset seam. The
// test drives the seam through a tiny shared resource, SceneControl, identified ONLY by
// its fully-qualified type name (Astra cross-module type identity = XXHash64 of the name;
// the test exe + plugin + Arcane.dll share one TypeContext, so an identically-named POD
// resolves to the same resource slot in both modules -- the same contract the scene
// components already rely on). The plugin writes sceneCount/currentScene; the test sets
// requestedScene / requestReset and the plugin consumes them in GamePlugin_FixedUpdate
// (which RunLoop runs BEFORE the engine fixedUpdate scheduler, so the rebuild happens on
// a clean registry, never mid-PhysicsSystem). This keeps the engine ABI untouched: the
// 7-entry PluginVTable is unchanged; SceneControl is a sandbox-private side channel.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>

// MIRROR of Arcane::Sandbox::SceneControl (SandboxApp.hpp). It is defined here under the
// EXACT same fully-qualified name (Arcane::Sandbox::SceneControl) and layout, so Astra's
// type identity (XXHash64 of the __FUNCSIG__-derived type name) resolves to the SAME
// resource ID in both the test module and the plugin -- the cross-module identity contract
// the scene components already rely on. (An alias would NOT work: TypeID hashes the
// underlying type's written name, so the struct must literally be named SceneControl in this
// namespace.) POD + <= 64 bytes => ResourceStorage keeps it inline (SBO) and the
// cross-module read/write aliases the same slot. Keep in lockstep with the plugin struct.
namespace Arcane::Sandbox
{
    struct SceneControl
    {
        std::int32_t sceneCount     = 0;   // plugin -> test: SceneRegistry().size()
        std::int32_t currentScene   = 0;   // plugin -> test: index after the last (re)build
        std::int32_t requestedScene = -1;  // test -> plugin: switch to this index (-1 = none)
        std::int32_t requestReset   = 0;   // test -> plugin: rebuild current scene (0/1)
    };
}

namespace
{
    using Arcane::Sandbox::SceneControl;

    // Step `n` fixed frames then submit ONE render frame; assert the GPU stayed clean.
    // Returns the batcher's quad count so the caller can spot-check scene 0's sprite floor.
    std::uint32_t StepAndRender(Arcane::Runtime& rt, Arcane::PluginHost& host,
                                Arcane::RenderDevice& device, Arcane::Canvas& canvas,
                                Arcane::Batcher2D& batcher, int n)
    {
        for (int i = 0; i < n; ++i)
            rt.Loop().Advance(1.0 / 60.0,
                [&](double dt) { host.Vtable()->FixedUpdate(dt); },
                [&](double dt, double a) { host.Vtable()->Update(dt, a); });

        nvrhi::CommandListHandle cl = device.Nvrhi()->createCommandList();
        cl->open();
        cl->clearTextureFloat(canvas.Texture(), nvrhi::AllSubresources,
                              nvrhi::Color(0, 0, 0, 1));
        batcher.Begin(cl, canvas.Framebuffer(), canvas.Width(), canvas.Height());
        rt.SetRenderContext(&batcher, glm::vec2(0.0f));
        rt.Loop().SubmitRender();   // RenderSubmissionSystem + physics-debug overlay -> batcher
        batcher.End();
        cl->close();
        device.Nvrhi()->executeCommandList(cl);
        device.Nvrhi()->waitForIdle();

        const std::uint32_t quads = batcher.Stats().quads;
        device.Nvrhi()->runGarbageCollection();
        return quads;
    }

    void RunSandboxSmoke(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        auto canvas  = Arcane::CreateCanvas(device->Nvrhi(), 1280, 720);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        REQUIRE(shaders != nullptr);
        REQUIRE(canvas  != nullptr);
        REQUIRE(batcher != nullptr);

        // Inject the process-wide shared TypeContext so the test exe, Arcane.dll,
        // and the plugin all resolve component/resource TypeIDs from the same slot table.
        Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
        Arcane::PluginHost host(rt, std::filesystem::path("Sandbox.dll"));
        REQUIRE(host.Load());   // plugin registers physics/scene components + systems + builds scene 0

        const Arcane::PluginVTable* vt = host.Vtable();
        REQUIRE(vt != nullptr);
        CHECK(vt->ABIVersion() == Arcane::kGamePluginABIVersion);
        REQUIRE(vt->DrawUI != nullptr);
        vt->DrawUI();   // no-op for now; must not crash with a null ImGui context (headless)

        // The plugin published the scene roster size + current index on Init.
        Astra::Registry& reg = rt.Registry();
        SceneControl* ctrl = reg.GetResource<SceneControl>();
        REQUIRE(ctrl != nullptr);
        CHECK(ctrl->sceneCount   == 8);   // 8-scene roster (Task 5)
        CHECK(ctrl->currentScene == 0);   // fresh boot builds scene 0

        // Scene 0 ("Playground"): floor + 2 walls + 5 dynamic sprites = 8 quads minimum
        // from RenderSubmissionSystem; the physics-debug overlay adds more. Step + render.
        const std::uint32_t scene0Quads = StepAndRender(rt, host, *device, *canvas, *batcher, 30);
        CHECK(scene0Quads >= 8);
        CHECK(Arcane::RenderErrorCount() == 0);

        // ---- per-scene smoke: drive SetScene for EVERY scene in the roster -------------
        // For each scene: request the switch (plugin consumes it next FixedUpdate, rebuilds
        // on a clean registry + fresh PhysicsResource), step ~30 fixed frames, render, and
        // assert the GPU stayed clean. Every scene must build + step + render without crash
        // and with the validation layer silent.
        for (std::int32_t i = 0; i < ctrl->sceneCount; ++i)
        {
            // Re-fetch each iteration: the resource pointer is stable across Clear() (Clear
            // does not touch resources), but a fresh read keeps the intent explicit.
            ctrl = reg.GetResource<SceneControl>();
            REQUIRE(ctrl != nullptr);
            ctrl->requestedScene = i;

            const std::uint32_t quads =
                StepAndRender(rt, host, *device, *canvas, *batcher, 30);
            (void)quads;   // scene-specific quad counts vary; the gate is no-crash + clean GPU

            ctrl = reg.GetResource<SceneControl>();
            REQUIRE(ctrl != nullptr);
            CHECK(ctrl->currentScene   == i);   // the plugin switched to the requested scene
            CHECK(ctrl->requestedScene == -1);  // ... and cleared the request
            CHECK(Arcane::RenderErrorCount() == 0);
        }

        // ---- reset: rebuild the CURRENT scene from scratch ----------------------------
        ctrl = reg.GetResource<SceneControl>();
        REQUIRE(ctrl != nullptr);
        const std::int32_t before = ctrl->currentScene;
        ctrl->requestReset = 1;
        StepAndRender(rt, host, *device, *canvas, *batcher, 30);
        ctrl = reg.GetResource<SceneControl>();
        REQUIRE(ctrl != nullptr);
        CHECK(ctrl->currentScene == before);  // reset stays on the same index
        CHECK(ctrl->requestReset == 0);        // ... and cleared the flag
        CHECK(Arcane::RenderErrorCount() == 0);

        host.Unload();
        device->Nvrhi()->runGarbageCollection();
    }
}

TEST_CASE("SandboxSmoke d3d12: sandbox plugin scene renders, no validation errors", "[sandbox][gpu][d3d12]")
{
    RunSandboxSmoke(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("SandboxSmoke vulkan: sandbox plugin scene renders, no validation errors", "[sandbox][gpu][vulkan]")
{
    RunSandboxSmoke(Arcane::GraphicsBackend::Vulkan);
}
