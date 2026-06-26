// Loom -- the thin host. Engine boot + render plumbing (the M4 Playground loop, scene
// content removed) + RunLoop interleaving the plugin's FixedUpdate/Update with the engine
// schedulers + a PluginHost watching the game DLL. The scene now lives in PlaygroundGame.dll.
//   --backend dx12|vulkan   --frames N   --no-vsync   --plugin <path>   (default ./Sandbox.dll)

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Batcher2D.hpp>   // Arcane::Batch2DStats (loop HUD + perf tick)
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (boot log + HUD)

#include <LoomConfig.hpp>

#include "FramePerf.hpp"
#include "GpuContext.hpp"

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2
    const LoomConfig& cfg = *parsed.config;

    // Transitional: locals shadow cfg.* so the rest of main compiles untouched;
    // Tasks 3-5 thread cfg through directly + fold these away. (vsync now flows
    // straight into GpuContext::Create(cfg), so its shadow is gone.)
    Arcane::GraphicsBackend backend = cfg.backend;
    uint64_t maxFrames = cfg.maxFrames;
    bool perf  = cfg.perf;
    std::string pluginPath = cfg.pluginPath;

    ARC_INFO("{} -- Loom host, backend {}", Arcane::BuildInfo(), Arcane::ToString(backend));

    // The whole platform/render/input stack, booted in order. Declared in main's
    // OUTER scope -- BEFORE the inner runtime/plugin scope below -- so it destructs
    // AFTER them: the render resources it owns (window/device/swapchain/shaders/
    // canvas/batcher/tonemap/imgui/input + commandList/framebuffers) must outlive
    // the runtime + plugin. GpuContext's own member order is the teardown contract.
    auto gpu = GpuContext::Create(cfg);
    if (!gpu) return 1;

    uint64_t frameCount = 0;

    // Runtime and PluginHost live in this inner scope so they destruct BEFORE
    // `gpu` (the render stack) in the outer scope -- which therefore outlives
    // them. PluginHost::~PluginHost calls Unload -> TeardownLive -> ClearSystems
    // + ResetRegistry, which must run while the plugin DLL is still mapped and
    // before the render pipeline (owned by gpu) is torn down.
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

        // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
        // Runtime so a plugin can build its own engine render objects (e.g. the
        // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
        // plugin (gpu is declared in the outer scope above). Null in a headless host
        // -> the plugin skips its GPU-resource creation.
        runtime.SetRenderResources(gpu->Device().Nvrhi(), &gpu->Shaders());

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

        // The reused command list + the lazy backbuffer-framebuffer cache now live
        // in `gpu` (gpu->Cmd() / gpu->FramebufferFor(bb)) so they release their
        // NVRHI handles before the device, in the outer scope's teardown.

        auto simPrev       = std::chrono::steady_clock::now();
        auto lastFrameTime = simPrev;
        auto lastShaderPoll = simPrev;
        bool running = true;

        FramePerf fp(perf);

        while (running)
        {
            auto events = gpu->Win().PumpEvents();
            if (events.quitRequested) break;
            if (events.resized)
                gpu->OnResize(events.width, events.height);
            if (gpu->Win().IsMinimized())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            fp.FrameStart();

            // Input: sample SDL state, evaluate actions. Must precede ImGui BeginFrame
            // so capture flags are set before the evaluator reads them.
            {
                const auto now = std::chrono::steady_clock::now();
                const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
                lastFrameTime = now;
                const Arcane::InputSnapshot snap =
                    gpu->InDevices().Sample(gpu->Imgui().WantCaptureKeyboard(),
                                            gpu->Imgui().WantCaptureMouse());
                runtime.SetInputSnapshot(snap);   // plugins read it via Runtime::Input()
                gpu->Input().Update(frameDt, snap);
                if (gpu->Input().Pressed("quit"))                break;
                if (gpu->Input().Pressed("reload_plugin"))       plugin.ForceReload();
                if (gpu->Input().Pressed("reload_plugin_fresh")) plugin.ReloadFresh();
            }

            // Sim advance: clamp dt, drive RunLoop with plugin callbacks interleaved.
            {
                const auto now = std::chrono::steady_clock::now();
                double simDt = std::chrono::duration<double>(now - simPrev).count();
                simPrev = now;
                if (simDt > 0.25) simDt = 0.25;
                const Arcane::PluginVTable* vt = plugin.Vtable();
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                runtime.Loop().Advance(simDt,
                    [&](double dt)          { if (vt) vt->FixedUpdate(dt); },
                    [&](double dt, double a){ if (vt) vt->Update(dt, a); });
                fp.Add(fp.accSim, t0, fp.Now());
            }

            gpu->Imgui().BeginFrame();
            {
                ImGui::Begin("Loom");
                ImGui::Text("Backend: %s", Arcane::ToString(gpu->Device().Backend()));
                ImGui::Text("Plugin gen: %u", plugin.Generation());
                const Arcane::Batch2DStats s = gpu->Batch().Stats();
                ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
                ImGui::End();
            }

            // ABI v2: the plugin draws its own ImGui between BeginFrame and Render.
            // Null-checked (a v2 plugin may omit DrawUI); PlaygroundGame's is a no-op.
            const Arcane::PluginVTable* vtUI = plugin.Vtable();
            if (vtUI && vtUI->DrawUI) vtUI->DrawUI();

            nvrhi::ITexture* backbuffer = gpu->Swap().BeginFrame();
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
                    gpu->Shaders().Poll();
                    lastShaderPoll = now0;
                }
            }

            gpu->Cmd()->open();
            gpu->Cmd()->clearTextureFloat(gpu->Cnv().Texture(), nvrhi::AllSubresources,
                                          nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

            gpu->Batch().Begin(gpu->Cmd(), gpu->Cnv().Framebuffer(),
                               gpu->Cnv().Width(), gpu->Cnv().Height());

            // Set the render context IN Arcane.dll so TypeID<RenderContext2D> resolves
            // in the correct module; then drive the plugin's RenderSubmissionSystem.
            // Loom stays camera-agnostic: SetRenderContext writes the STORED camera the
            // plugin drives via Runtime::SetCamera (default identity if it never does).
            {
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                runtime.SetRenderContext(&gpu->Batch());
                runtime.Loop().SubmitRender();
                fp.Add(fp.accRec, t0, fp.Now());
            }

            {
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                gpu->Batch().End();
                fp.Add(fp.accEnd, t0, fp.Now());
            }

            nvrhi::FramebufferHandle& fb = gpu->FramebufferFor(backbuffer);
            {
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                gpu->Tone().Run(gpu->Cmd(), gpu->Cnv().Texture(), fb);
                fp.Add(fp.accTone, t0, fp.Now());
            }
            {
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                gpu->Imgui().Render(gpu->Cmd(), fb);
                fp.Add(fp.accImgui, t0, fp.Now());
            }

            gpu->Cmd()->close();
            {
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                gpu->Device().Nvrhi()->executeCommandList(gpu->Cmd());
                gpu->Swap().Present();
                fp.Add(fp.accPresent, t0, fp.Now());
            }

            // Debounced watcher: rebuild PlaygroundGame -> auto hot reload.
            {
                const auto t0 = fp.On() ? fp.Now() : FramePerf::Clock::time_point{};
                plugin.Poll();
                fp.Add(fp.accPoll, t0, fp.Now());
            }

            if (fp.On())
            {
                const Arcane::Batch2DStats bs = gpu->Batch().Stats();
                fp.Tick(bs.quads, bs.drawCalls);
            }

            ++frameCount;
            if (maxFrames != 0 && frameCount >= maxFrames)
                running = false;
        }

        gpu->Device().Nvrhi()->waitForIdle();
        // ~PluginHost fires at the end of this inner scope (Unload: TeardownLive
        // while the DLL is still loaded); ~Runtime fires last in this scope
        // (destroys JobSystem + empty Registry). `gpu` (the render stack, incl. the
        // command list + framebuffer cache that hold NVRHI handles) is in the OUTER
        // scope, so it tears down AFTER -- releasing those handles before the device
        // it owns, exactly as the old in-scope locals did. See GpuContext's header.
    }

    ARC_INFO("Loom exiting after {} frames", frameCount);
    return 0;
}
