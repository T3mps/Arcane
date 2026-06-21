// [gpu] live integration: drive PlaygroundGame through a real Runtime + Batcher2D into
// an offscreen HDR canvas; assert the plugin's RenderSubmissionSystem submitted the two
// sprites and NVRHI/VK validation stayed silent. Device setup copied from SceneSliceTest.
// Asserts via batcher->Stats() only -> no scene-component TypeID is touched in the test
// module (the plugin owns the component identities under the shared context).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <glm/glm.hpp>

#include <filesystem>

namespace
{
    void RunLoomSlice(Arcane::GraphicsBackend backend)
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
        // and the plugin all resolve component TypeIDs from the same slot table.
        Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
        Arcane::PluginHost host(rt, std::filesystem::path("PlaygroundGame.dll"));
        REQUIRE(host.Load());   // plugin registers scene components + systems under the shared ctx

        // ABI v2: the plugin loaded under a v2 EngineContext (imgui fields null here --
        // headless, no ImGuiLayer). The host resolved the DrawUI hook; calling it is a
        // no-op for PlaygroundGame, but proves the entry point is live and callable.
        const Arcane::PluginVTable* vt = host.Vtable();
        REQUIRE(vt != nullptr);
        CHECK(vt->ABIVersion() == Arcane::kGamePluginABIVersion);
        REQUIRE(vt->DrawUI != nullptr);
        vt->DrawUI();   // no-op; must not crash with a null ImGui context

        for (int i = 0; i < 10; ++i)
            rt.Loop().Advance(1.0 / 60.0,
                [&](double dt) { host.Vtable()->FixedUpdate(dt); },
                [&](double, double) {});

        nvrhi::CommandListHandle cl = device->Nvrhi()->createCommandList();
        cl->open();
        cl->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                              nvrhi::Color(0, 0, 0, 1));
        batcher->Begin(cl, canvas->Framebuffer(), canvas->Width(), canvas->Height());
        rt.SetRenderContext(batcher.get(), glm::vec2(0.0f));
        rt.Loop().SubmitRender();   // plugin's RenderSubmissionSystem -> batcher
        batcher->End();
        cl->close();
        device->Nvrhi()->executeCommandList(cl);
        device->Nvrhi()->waitForIdle();

        CHECK(batcher->Stats().quads >= 2);         // orbiter + moon submitted via the plugin
        CHECK(Arcane::RenderErrorCount() == 0);
        host.Unload();
        device->Nvrhi()->runGarbageCollection();
    }
}

// Headless: the input-store path Loom wires in its frame loop (SetInputSnapshot ->
// Input()). No device needed -- the plugin reads input through Runtime::Input(), so
// the host's per-frame store must round-trip the snapshot verbatim. ([sandbox] so it
// runs alongside the rest of the v2 sandbox wiring under ~[gpu].)
TEST_CASE("Loom input store: Runtime::Input reflects the last SetInputSnapshot", "[sandbox]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());

    Arcane::InputSnapshot snap;
    snap.SetScancode(42);              // arbitrary physical key down
    snap.AddKeycode(0x61);             // 'a'
    snap.mouseButtons        = 0x05;   // LMB + MMB
    snap.mouseX              = 123.5f;
    snap.mouseY              = 456.25f;
    snap.gamepadConnected    = true;
    snap.gamepadAxes[0]      = -0.5f;
    snap.wantCaptureKeyboard = true;

    rt.SetInputSnapshot(snap);

    const Arcane::InputSnapshot& got = rt.Input();
    CHECK(got.ScancodeDown(42));
    CHECK(got.KeycodeDown(0x61));
    CHECK(got.mouseButtons == 0x05);
    CHECK(got.mouseX == 123.5f);
    CHECK(got.mouseY == 456.25f);
    CHECK(got.gamepadConnected);
    CHECK(got.gamepadAxes[0] == -0.5f);
    CHECK(got.wantCaptureKeyboard);
}

TEST_CASE("d3d12: Loom slice renders the plugin scene, no validation errors", "[gpu][d3d12]")
{
    RunLoomSlice(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: Loom slice renders the plugin scene, no validation errors", "[gpu][vulkan]")
{
    RunLoomSlice(Arcane::GraphicsBackend::Vulkan);
}
