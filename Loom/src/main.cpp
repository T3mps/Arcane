// Loom -- the thin host. Engine boot + render plumbing (the M4 Playground loop, scene
// content removed) + RunLoop interleaving the plugin's FixedUpdate/Update with the engine
// schedulers + a PluginHost watching the game DLL. The scene now lives in PlaygroundGame.dll.
//   --backend dx12|vulkan   --frames N   --no-vsync   --plugin <path>   (default ./PlaygroundGame.dll)

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/Swapchain.hpp>
#include <Arcane/Render/TonemapPass.hpp>

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    void PrintUsage()
    {
        std::printf(
            "Arcane Loom (M5: plugin host)\n"
            "  --backend dx12|vulkan   graphics backend (default dx12)\n"
            "  --frames N              render N frames then exit\n"
            "  --no-vsync              present without vsync\n"
            "  --plugin <path>         game DLL to host (default ./PlaygroundGame.dll)\n");
    }
}

int main(int argc, char** argv)
{
    Arcane::GraphicsBackend backend = Arcane::GraphicsBackend::D3D12;
    uint64_t maxFrames = 0;
    bool vsync = true;
    std::string pluginPath = "PlaygroundGame.dll";

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            ++i;
            if      (std::strcmp(argv[i], "vulkan") == 0) backend = Arcane::GraphicsBackend::Vulkan;
            else if (std::strcmp(argv[i], "dx12")   == 0) backend = Arcane::GraphicsBackend::D3D12;
            else { PrintUsage(); return 2; }
        }
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) { maxFrames = std::strtoull(argv[++i], nullptr, 10); }
        else if (std::strcmp(argv[i], "--no-vsync") == 0)               { vsync = false; }
        else if (std::strcmp(argv[i], "--plugin") == 0 && i + 1 < argc) { pluginPath = argv[++i]; }
        else { PrintUsage(); return 2; }
    }

    Arcane::Log::Init();
    ARC_INFO("{} -- Loom host, backend {}", Arcane::BuildInfo(), Arcane::ToString(backend));

    // window is declared first so the destructor fires last (after all modules
    // that hold references to the SDL window -- imgui, inputDevices -- are gone).
    Arcane::Window window;
    Arcane::WindowDesc wd;
    wd.title  = "Arcane Loom";
    wd.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
    if (!window.Create(wd))
        return 1;

    Arcane::RenderDeviceDesc dd;
    dd.backend = backend;
    auto device = Arcane::RenderDevice::Create(dd);
    if (!device) return 1;

    auto swapchain = device->CreateSwapchain(window, vsync);
    if (!swapchain) return 1;

    auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
    if (!shaders) return 1;

    auto canvas = Arcane::CreateCanvas(device->Nvrhi(), swapchain->Width(), swapchain->Height());
    if (!canvas) return 1;

    auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
    if (!batcher) return 1;

    auto tonemap = Arcane::TonemapPass::Create(device->Nvrhi(), *shaders);
    if (!tonemap) return 1;

    // ImGuiLayer must be destroyed before window (the layer taps window events),
    // so it is declared AFTER window -- stack unwinds in reverse declaration order.
    auto imgui = Arcane::ImGuiLayer::Create(window, *device, *shaders);
    if (!imgui) return 1;

    auto inputDevices = Arcane::InputDevices::Create();
    auto input = Arcane::InputActions::Create();
    if (!inputDevices || !input || !input->LoadFile("data/input_actions.json"))
        return 1;
    input->SetBaseContext("demo");

    uint64_t frameCount = 0;

    // Runtime and PluginHost live in this inner scope so they destruct BEFORE
    // the render resources (batcher/canvas/shaders/swapchain/device/window) that
    // outlive them. PluginHost::~PluginHost calls Unload -> TeardownLive ->
    // ClearSystems + ResetRegistry, which must run while the plugin DLL is still
    // mapped and before the render pipeline is torn down.
    {
        // The TypeContext is the process-wide type-identity singleton shared across
        // Loom.exe, Arcane.dll, and every loaded plugin. It is intentionally
        // heap-allocated and never freed: TypeMeta entries registered by the plugin
        // (via ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled
        // into the plugin DLL. After PluginHost::Unload -> DLClose, those thunks
        // point to unmapped memory. If the TypeContext (and its MetaRegistry) were
        // ever destructed, ~std::function() would invoke those thunks -> crash.
        // Heap-leaking is the correct production pattern for a long-running host;
        // the OS reclaims all process memory on exit anyway.
        // (The test exe is safe for a different reason: MetaRegistry::Register is first-writer-wins,
        // and the test TUs that reflect these types are never unloaded, so their thunks -- not the
        // plugin's -- own the MetaRegistry entries.)
        static Astra::TypeContext* const s_typeContext = new Astra::TypeContext();
        Arcane::Runtime runtime(s_typeContext);

        // ABI v2: install the host's ImGui context + allocators on the Runtime BEFORE
        // the plugin loads. PluginHost::RefreshContext copies these into the EngineContext
        // at Init time, so the plugin's Init adopts the host's GImGui across the DLL boundary.
        // The ImGuiLayer (outer scope) has already created + set the current context by here.
        {
            ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
            ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
            runtime.SetImGui(ImGui::GetCurrentContext(),
                             reinterpret_cast<void*>(allocFn),
                             reinterpret_cast<void*>(freeFn),
                             ud);
        }

        Arcane::PluginHost plugin(runtime, std::filesystem::path(pluginPath));
        if (!plugin.Load())
        {
            ARC_ERROR("Loom: failed to load plugin '{}'", pluginPath);
            return 1;
        }

        nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> backbufferFramebuffers;

        auto simPrev       = std::chrono::steady_clock::now();
        auto lastFrameTime = simPrev;
        auto lastShaderPoll = simPrev;
        bool running = true;

        while (running)
        {
            auto events = window.PumpEvents();
            if (events.quitRequested) break;
            if (events.resized)
            {
                backbufferFramebuffers.clear();
                swapchain->Resize(events.width, events.height);
                canvas->Resize(swapchain->Width(), swapchain->Height());
            }
            if (window.IsMinimized())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Input: sample SDL state, evaluate actions. Must precede ImGui BeginFrame
            // so capture flags are set before the evaluator reads them.
            {
                const auto now = std::chrono::steady_clock::now();
                const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
                lastFrameTime = now;
                const Arcane::InputSnapshot snap =
                    inputDevices->Sample(imgui->WantCaptureKeyboard(), imgui->WantCaptureMouse());
                runtime.SetInputSnapshot(snap);   // plugins read it via Runtime::Input()
                input->Update(frameDt, snap);
                if (input->Pressed("quit"))                break;
                if (input->Pressed("reload_plugin"))       plugin.ForceReload();
                if (input->Pressed("reload_plugin_fresh")) plugin.ReloadFresh();
            }

            // Sim advance: clamp dt, drive RunLoop with plugin callbacks interleaved.
            {
                const auto now = std::chrono::steady_clock::now();
                double simDt = std::chrono::duration<double>(now - simPrev).count();
                simPrev = now;
                if (simDt > 0.25) simDt = 0.25;
                const Arcane::PluginVTable* vt = plugin.Vtable();
                runtime.Loop().Advance(simDt,
                    [&](double dt)          { if (vt) vt->FixedUpdate(dt); },
                    [&](double dt, double a){ if (vt) vt->Update(dt, a); });
            }

            imgui->BeginFrame();
            {
                ImGui::Begin("Loom");
                ImGui::Text("Backend: %s", Arcane::ToString(device->Backend()));
                ImGui::Text("Plugin gen: %u", plugin.Generation());
                const Arcane::Batch2DStats s = batcher->Stats();
                ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
                ImGui::End();
            }

            // ABI v2: the plugin draws its own ImGui between BeginFrame and Render.
            // Null-checked (a v2 plugin may omit DrawUI); PlaygroundGame's is a no-op.
            const Arcane::PluginVTable* vtUI = plugin.Vtable();
            if (vtUI && vtUI->DrawUI) vtUI->DrawUI();

            nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
            if (!backbuffer)
            {
                // No backbuffer this frame: still balance BeginFrame with EndFrame
                // so ImGui's assert (double-Begin) doesn't fire next iteration.
                ImGui::EndFrame();
                continue;
            }

            // Hot reload: poll shaders once a second; pipeline caches rebuild lazily.
            {
                const auto now0 = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now0 - lastShaderPoll).count() >= 1.0)
                {
                    shaders->Poll();
                    lastShaderPoll = now0;
                }
            }

            commandList->open();
            commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                           nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

            batcher->Begin(commandList, canvas->Framebuffer(), canvas->Width(), canvas->Height());

            // Set the render context IN Arcane.dll so TypeID<RenderContext2D> resolves
            // in the correct module; then drive the plugin's RenderSubmissionSystem.
            // Loom stays camera-agnostic: SetRenderContext writes the STORED camera the
            // plugin drives via Runtime::SetCamera (default identity if it never does).
            runtime.SetRenderContext(batcher.get());
            runtime.Loop().SubmitRender();

            batcher->End();

            nvrhi::FramebufferHandle& fb = backbufferFramebuffers[backbuffer];
            if (!fb)
                fb = device->Nvrhi()->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(backbuffer));
            tonemap->Run(commandList, canvas->Texture(), fb);
            imgui->Render(commandList, fb);

            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            swapchain->Present();

            // Debounced watcher: rebuild PlaygroundGame -> auto hot reload.
            plugin.Poll();

            ++frameCount;
            if (maxFrames != 0 && frameCount >= maxFrames)
                running = false;
        }

        device->Nvrhi()->waitForIdle();
        // commandList and backbufferFramebuffers release their NVRHI handles here
        // (end of scope), while device is still alive in the outer scope above.
        // ~PluginHost fires next (Unload: TeardownLive while DLL is still loaded).
        // ~Runtime fires last in this scope (destroys JobSystem + empty Registry).
    }

    ARC_INFO("Loom exiting after {} frames", frameCount);
    return 0;
}
