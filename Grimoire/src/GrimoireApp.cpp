// GrimoireApp: Init -> MainLoop -> Shutdown. Reuses Loom's host-boot helpers
// (GpuContext/FramePerf/LoomConfig) by source-compile and hosts Sandbox.dll via
// the lifted Arcane::PluginHost. The frame loop advances the sim through the
// RunLoop and draws a placeholder ImGui window straight to the backbuffer (no
// scene->tonemap->backbuffer pass yet -- that arrives with the viewport panel
// in a later task). The render plumbing + teardown order live in GpuContext
// (m_gpu). The teardown CONTRACT is encoded in the GrimoireApp member
// declaration order -- see GrimoireApp.hpp.

#include "GrimoireApp.hpp"

#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

namespace Grimoire
{
    GrimoireApp::GrimoireApp(LoomConfig cfg)
        : m_config(std::move(cfg)), m_perf(m_config.perf) {}

    bool GrimoireApp::Init()
    {
        // The whole platform/render/input stack, booted in order. Owned by m_gpu and
        // declared BEFORE m_runtime/m_plugin in GrimoireApp -- so it destructs AFTER
        // them: the render resources it owns (window/device/swapchain/shaders/canvas/
        // batcher/tonemap/imgui/input + commandList/framebuffers) must outlive runtime
        // + plugin.
        m_gpu = GpuContext::Create(m_config);
        if (!m_gpu)
        {
            ARC_ERROR("Grimoire: GPU context create failed");
            return false;
        }

        ARC_INFO("{} -- Grimoire host, backend {}", Arcane::BuildInfo(), Arcane::ToString(m_config.backend));

        // The TypeContext is the process-wide type-identity singleton shared across
        // Grimoire.exe, Arcane.dll, and every loaded plugin. It is intentionally
        // heap-allocated and never freed: TypeMeta entries registered by the plugin
        // (via ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled
        // into the plugin DLL. After PluginHost::Unload -> DLClose, those thunks
        // point to unmapped memory. If the TypeContext (and its MetaRegistry) were
        // ever destructed, ~std::function() would invoke those thunks -> crash.
        // Heap-leaking is the correct production pattern for a long-running host;
        // the OS reclaims all process memory on exit anyway.
        m_typeContext = new Astra::TypeContext();
        // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
        // until quit). The scripted "Grimoire --frames N" GPU-verify is headless -> false
        // -> miniaudio's null backend (no real device grabbed on a CI box).
        m_runtime.emplace(m_typeContext, m_config.maxFrames == 0);

        // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
        // Runtime so a plugin can build its own engine render objects (e.g. the
        // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
        // plugin (m_gpu is declared before the runtime/plugin in GrimoireApp). Null in
        // a headless host -> the plugin skips its GPU-resource creation.
        m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

        // ABI v2: install the host's ImGui context + allocators on the Runtime BEFORE
        // the plugin loads. PluginHost::RefreshContext copies these into the EngineContext
        // at Init time, so the plugin's Init adopts the host's GImGui across the DLL boundary.
        // The ImGuiLayer (in m_gpu) has already created + set the current context by here.
        {
            ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
            ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
            m_runtime->SetImGui(ImGui::GetCurrentContext(),
                                reinterpret_cast<void*>(allocFn),
                                reinterpret_cast<void*>(freeFn),
                                ud);
        }

        m_plugin.emplace(*m_runtime, std::filesystem::path(m_config.pluginPath));
        if (!m_plugin->Load())
        {
            ARC_ERROR("Grimoire: failed to load plugin '{}'", m_config.pluginPath);
            return false;
        }

        return true;
    }

    void GrimoireApp::MainLoop()
    {
        auto simPrev = std::chrono::steady_clock::now();
        auto lastFrameTime = simPrev;
        bool running = true;

        while (running)
        {
            auto events = m_gpu->Win().PumpEvents();
            if (events.quitRequested) break;
            if (events.resized) m_gpu->OnResize(events.width, events.height);
            if (m_gpu->Win().IsMinimized())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Input sample (before ImGui BeginFrame so capture flags are set).
            {
                const auto now = std::chrono::steady_clock::now();
                const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
                lastFrameTime = now;
                const Arcane::InputSnapshot snap =
                    m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(),
                                              m_gpu->Imgui().WantCaptureMouse());
                m_runtime->SetInputSnapshot(snap);
                m_gpu->Input().Update(frameDt, snap);
            }

            // Sim advance through the RunLoop with the plugin callbacks interleaved.
            {
                const auto now = std::chrono::steady_clock::now();
                double simDt = std::chrono::duration<double>(now - simPrev).count();
                simPrev = now;
                if (simDt > 0.25) simDt = 0.25;
                const Arcane::PluginVTable* vt = m_plugin->Vtable();
                m_runtime->Loop().Advance(simDt,
                    [&](double dt)          { if (vt) vt->FixedUpdate(dt); },
                    [&](double dt, double a){ if (vt) vt->Update(dt, a); });
                m_runtime->AudioSystem().Update(simDt);
            }

            // ImGui: placeholder editor window (dockspace + panels arrive in later tasks).
            m_gpu->Imgui().BeginFrame();
            {
                ImGui::Begin("Grimoire");
                ImGui::Text("Backend: %s", Arcane::ToString(m_gpu->Device().Backend()));
                ImGui::Text("Plugin gen: %u", m_plugin->Generation());
                ImGui::Text("Paused: %s", m_runtime->Loop().IsPaused() ? "yes" : "no");
                ImGui::End();
            }
            const Arcane::PluginVTable* vtUI = m_plugin->Vtable();
            if (vtUI && vtUI->DrawUI) vtUI->DrawUI();

            nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
            if (!backbuffer) { ImGui::EndFrame(); continue; }

            m_gpu->Cmd()->open();
            // Clear the backbuffer directly (Grimoire's scene will live in a panel,
            // so there is no scene->tonemap->backbuffer pass as in Loom).
            m_gpu->Cmd()->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                            nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
            nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
            m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
            m_gpu->Cmd()->close();
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            m_gpu->Swap().Present();

            m_plugin->Poll();

            ++m_frameCount;
            if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames) running = false;
        }
    }

    void GrimoireApp::Shutdown()
    {
        // defensive: today Shutdown only runs after a successful Init, so m_gpu is non-null;
        // the guard covers a future partial-init/destructor path.
        if (m_gpu) m_gpu->Device().Nvrhi()->waitForIdle();
        ARC_INFO("Grimoire exiting after {} frames", m_frameCount);

        // The member destructors then run (after Run returns + ~GrimoireApp), in
        // reverse declaration order -- the load-bearing TEARDOWN CONTRACT:
        //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
        //                ResetRegistry) while the plugin DLL is STILL mapped.
        //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
        //   m_gpu     -> ~GpuContext: the render stack (command list + framebuffer
        //                cache release their NVRHI handles before the device), window
        //                LAST. So gpu outlives runtime + plugin exactly as Loom's did.
        //                See GpuContext's header.
        // m_typeContext is intentionally NOT freed (heap-leaked, see Init).
    }

    int GrimoireApp::Run()
    {
        if (!Init()) return 1;
        MainLoop();
        Shutdown();
        return 0;
    }
}
